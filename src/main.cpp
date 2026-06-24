#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include "config.h"

Adafruit_BME280 bme;
Preferences preferences;

static bool sensorReady = false;
static uint8_t bmeAddress = 0;
static unsigned long lastPostAt = 0;
static unsigned long configButtonPressedAt = 0;
static bool shouldSavePortalConfig = false;
static bool telemetryBlockedByConfiguration = false;

struct AppConfig
{
    String ingestUrl;
    String deviceKey;
    String deviceToken;
};

static AppConfig appConfig;

struct Measurement
{
    float temperatureC;
    float humidityPct;
    float pressureHpa;
};

static float roundTo2(float value)
{
    return roundf(value * 100.0F) / 100.0F;
}

static void loadAppConfig()
{
    preferences.begin("telemetry", true);
    appConfig.ingestUrl = preferences.getString("url", DEFAULT_TELEMETRY_INGEST_URL);
    appConfig.deviceKey = preferences.getString("dev_key", DEFAULT_DEVICE_KEY);
    appConfig.deviceToken = preferences.getString("dev_token", DEFAULT_DEVICE_TOKEN);
    preferences.end();
}

static void saveAppConfig()
{
    preferences.begin("telemetry", false);
    preferences.putString("url", appConfig.ingestUrl);
    preferences.putString("dev_key", appConfig.deviceKey);
    preferences.putString("dev_token", appConfig.deviceToken);
    preferences.end();
}

static void markPortalConfigChanged()
{
    shouldSavePortalConfig = true;
}

static void startConfigPortal(bool resetWifi)
{
    char url[160];
    char deviceKey[64];
    char deviceToken[128];

    strlcpy(url, appConfig.ingestUrl.c_str(), sizeof(url));
    strlcpy(deviceKey, appConfig.deviceKey.c_str(), sizeof(deviceKey));
    strlcpy(deviceToken, appConfig.deviceToken.c_str(), sizeof(deviceToken));

    WiFiManagerParameter ingestUrlParam("url", "telemetry-core ingest URL", url, sizeof(url));
    WiFiManagerParameter deviceKeyParam("device_key", "Device key", deviceKey, sizeof(deviceKey));
    WiFiManagerParameter deviceTokenParam("device_token", "X-Device-Token", deviceToken, sizeof(deviceToken));

    WiFiManager wifiManager;
    wifiManager.setSaveConfigCallback(markPortalConfigChanged);
    wifiManager.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SECONDS);
    wifiManager.addParameter(&ingestUrlParam);
    wifiManager.addParameter(&deviceKeyParam);
    wifiManager.addParameter(&deviceTokenParam);

    if (resetWifi)
    {
        wifiManager.resetSettings();
    }

    Serial.print("Starting setup portal: ");
    Serial.println(CONFIG_AP_SSID);
    Serial.print("Portal password: ");
    Serial.println(CONFIG_AP_PASSWORD);

    const bool connected = wifiManager.autoConnect(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
    if (!connected)
    {
        Serial.println("Setup portal timeout. Restarting...");
        delay(1000);
        ESP.restart();
    }

    if (shouldSavePortalConfig)
    {
        appConfig.ingestUrl = ingestUrlParam.getValue();
        appConfig.deviceKey = deviceKeyParam.getValue();
        appConfig.deviceToken = deviceTokenParam.getValue();
        saveAppConfig();
        shouldSavePortalConfig = false;
        telemetryBlockedByConfiguration = false;
        Serial.println("Customer configuration saved");
    }
}

static void connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin();

    Serial.println("Connecting to saved Wi-Fi");
    const unsigned long startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("Wi-Fi connected, IP: ");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("Wi-Fi connection timeout");
        startConfigPortal(false);
    }
}

static bool initBme280()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    const uint8_t addresses[] = {0x76, 0x77};
    for (const uint8_t address : addresses)
    {
        if (bme.begin(address, &Wire))
        {
            bmeAddress = address;
            Serial.print("BME280 detected at 0x");
            Serial.println(bmeAddress, HEX);

            bme.setSampling(
                Adafruit_BME280::MODE_FORCED,
                Adafruit_BME280::SAMPLING_X2,
                Adafruit_BME280::SAMPLING_X16,
                Adafruit_BME280::SAMPLING_X1,
                Adafruit_BME280::FILTER_X16,
                Adafruit_BME280::STANDBY_MS_1000);

            return true;
        }
    }

    Serial.println("BME280 not found. Check wiring and address 0x76/0x77.");
    return false;
}

static bool readMeasurement(Measurement &measurement)
{
    if (!sensorReady)
    {
        return false;
    }

    bme.takeForcedMeasurement();

    measurement.temperatureC = bme.readTemperature();
    measurement.humidityPct = bme.readHumidity();
    measurement.pressureHpa = bme.readPressure() / 100.0F;

    if (isnan(measurement.temperatureC) || isnan(measurement.humidityPct) || isnan(measurement.pressureHpa))
    {
        Serial.println("Invalid BME280 reading");
        return false;
    }

    return true;
}

static time_t getUnixTimestamp()
{
    time_t now = time(nullptr);
    if (now > 1700000000)
    {
        return now;
    }

    return 0;
}

static void syncTime()
{
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    const unsigned long startedAt = millis();
    while (getUnixTimestamp() == 0 && millis() - startedAt < NTP_SYNC_TIMEOUT_MS)
    {
        delay(250);
    }

    const time_t now = getUnixTimestamp();
    if (now > 0)
    {
        Serial.print("NTP time synced: ");
        Serial.println(static_cast<unsigned long>(now));
    }
    else
    {
        Serial.println("NTP sync timeout");
    }
}

static String buildPayload(const Measurement &measurement)
{
    JsonDocument doc;

    doc["device_key"] = appConfig.deviceKey;
    doc["timestamp"] = static_cast<unsigned long>(getUnixTimestamp());

    JsonObject data = doc["data"].to<JsonObject>();
    data["temperature"] = roundTo2(measurement.temperatureC);
    data["pressure"] = roundTo2(measurement.pressureHpa);
    data["humidity"] = roundTo2(measurement.humidityPct);

    String payload;
    serializeJson(doc, payload);
    return payload;
}

struct HttpResponse
{
    int status;
    String body;
};

static HttpResponse sendTelemetryRequest(const String &payload)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        connectWiFi();
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Skipping POST: Wi-Fi is offline");
        return {HTTPC_ERROR_CONNECTION_REFUSED, ""};
    }

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);

    if (!http.begin(appConfig.ingestUrl))
    {
        Serial.println("HTTP begin failed");
        return {HTTPC_ERROR_CONNECTION_REFUSED, ""};
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("X-Device-Token", appConfig.deviceToken);

    Serial.print("POST ");
    Serial.println(appConfig.ingestUrl);
    Serial.print("Payload: ");
    Serial.println(payload);

    const int status = http.POST(payload);
    const String response = http.getString();
    http.end();

    Serial.print("HTTP status: ");
    Serial.println(status);

    if (response.length() > 0)
    {
        Serial.print("Response: ");
        Serial.println(response);
    }

    return {status, response};
}

static void logIngestMeta(const String &response)
{
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok)
    {
        return;
    }

    Serial.print("Ingested records: ");
    Serial.println(doc["meta"]["ingested_records"] | 0);
    Serial.print("Skipped records: ");
    Serial.println(doc["meta"]["skipped_records"] | 0);
}

static bool postTelemetry(const String &payload)
{
    const unsigned long retryDelays[] = {0, FIRST_RETRY_DELAY_MS, SECOND_RETRY_DELAY_MS};

    for (uint8_t attempt = 0; attempt < HTTP_MAX_ATTEMPTS; ++attempt)
    {
        if (retryDelays[attempt] > 0)
        {
            Serial.print("Retrying in ms: ");
            Serial.println(retryDelays[attempt]);
            delay(retryDelays[attempt]);
        }

        const HttpResponse response = sendTelemetryRequest(payload);
        if (response.status >= 200 && response.status < 300)
        {
            logIngestMeta(response.body);
            return true;
        }

        if (response.status == 401 || response.status == 404)
        {
            telemetryBlockedByConfiguration = true;
            Serial.println("Telemetry disabled: check device key and token in the setup portal");
            return false;
        }

        const bool retryable = response.status < 0 || response.status == 429 || response.status >= 500;
        if (!retryable)
        {
            Serial.println("Request rejected. It will not be retried in this cycle.");
            return false;
        }
    }

    Serial.println("Telemetry retries exhausted");
    return false;
}

static void handleConfigButton()
{
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW)
    {
        if (configButtonPressedAt == 0)
        {
            configButtonPressedAt = millis();
        }

        if (millis() - configButtonPressedAt >= CONFIG_BUTTON_HOLD_MS)
        {
            Serial.println("Config button held. Opening setup portal.");
            startConfigPortal(true);
            configButtonPressedAt = 0;
            lastPostAt = millis() - POST_INTERVAL_MS;
        }
        return;
    }

    configButtonPressedAt = 0;
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP32-C3 BME280 telemetry firmware starting");

    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    WiFi.mode(WIFI_STA);
    loadAppConfig();
    sensorReady = initBme280();
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED)
    {
        syncTime();
    }
    lastPostAt = millis() - POST_INTERVAL_MS;
}

void loop()
{
    handleConfigButton();

    if (millis() - lastPostAt < POST_INTERVAL_MS)
    {
        delay(100);
        return;
    }

    lastPostAt = millis();

    if (telemetryBlockedByConfiguration)
    {
        Serial.println("Telemetry is blocked until device configuration is updated");
        return;
    }

    Measurement measurement{};
    if (!readMeasurement(measurement))
    {
        return;
    }

    const String payload = buildPayload(measurement);
    if (getUnixTimestamp() == 0)
    {
        Serial.println("Skipping POST: timestamp is not synced");
        if (WiFi.status() == WL_CONNECTED)
        {
            syncTime();
        }
        return;
    }

    const bool ok = postTelemetry(payload);

    Serial.println(ok ? "Telemetry sent" : "Telemetry send failed");
}
