#include <Arduino.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <esp_wifi.h>
#include <math.h>
#include <time.h>

namespace
{
constexpr const char *DEFAULT_TELEMETRY_INGEST_URL = "https://telemetry-core.pl/api/v1/telemetry";
constexpr const char *DEFAULT_DEVICE_KEY = "";
constexpr const char *DEFAULT_PAIRING_CODE = "";

constexpr const char *CONFIG_AP_SSID = "Telemetry-Setup";
constexpr const char *CONFIG_AP_PASSWORD = "telemetry123";
constexpr int CONFIG_BUTTON_PIN = 9;
constexpr unsigned long CONFIG_BUTTON_HOLD_MS = 5000;
constexpr unsigned long CONFIG_PORTAL_TIMEOUT_SECONDS = 180;

constexpr int I2C_SDA_PIN = 5;
constexpr int I2C_SCL_PIN = 6;

constexpr float MIN_VALID_TEMPERATURE_C = -40.0F;
constexpr float MAX_VALID_TEMPERATURE_C = 85.0F;
constexpr float MIN_VALID_HUMIDITY_PCT = 0.0F;
constexpr float MAX_VALID_HUMIDITY_PCT = 100.0F;
constexpr float MIN_VALID_PRESSURE_HPA = 300.0F;
constexpr float MAX_VALID_PRESSURE_HPA = 1100.0F;
constexpr uint8_t MAX_INVALID_BME_READINGS = 3;
constexpr unsigned long BME_REINIT_DELAY_MS = 250;

constexpr unsigned long POST_INTERVAL_MS = 60000;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;
constexpr unsigned long HTTP_TIMEOUT_MS = 10000;
constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 15000;
constexpr unsigned long FIRST_RETRY_DELAY_MS = 5000;
constexpr unsigned long SECOND_RETRY_DELAY_MS = 15000;
constexpr uint8_t HTTP_MAX_ATTEMPTS = 3;

constexpr size_t URL_BUFFER_SIZE = 160;
constexpr size_t DEVICE_KEY_BUFFER_SIZE = 64;
constexpr size_t PAIRING_CODE_BUFFER_SIZE = 32;
constexpr size_t TOKEN_BUFFER_SIZE = 96;
constexpr const char *PREFERENCES_NAMESPACE = "telemetry";

Adafruit_BME280 bme;
Preferences preferences;

bool sensorReady = false;
bool telemetryBlockedByConfiguration = false;
bool wifiWasConnected = false;
uint8_t bmeAddress = 0;
uint8_t consecutiveInvalidBmeReadings = 0;
unsigned long lastPostAt = 0;
unsigned long configButtonPressedAt = 0;
unsigned long lastWiFiReconnectAttemptAt = 0;

struct DeviceConfig
{
    String telemetryUrl;
    String deviceKey;
    String pairingCode;
    String ingestToken;
};

struct Measurement
{
    float temperatureC;
    float humidityPct;
    float pressureHpa;
};

struct HttpResponse
{
    int status;
    String body;
};

DeviceConfig config;

void syncTime();

float roundTo2(float value)
{
    return roundf(value * 100.0F) / 100.0F;
}

String trimString(const String &value)
{
    String trimmed = value;
    trimmed.trim();
    return trimmed;
}

String normalizePairingCode(const String &value)
{
    String normalized;

    for (size_t index = 0; index < value.length(); index++)
    {
        const char current = value.charAt(index);

        if (isalnum(static_cast<unsigned char>(current)))
        {
            normalized += static_cast<char>(toupper(static_cast<unsigned char>(current)));
        }
    }

    return normalized;
}

bool isHttpsUrl(const String &url)
{
    return url.startsWith("https://");
}

String buildPairingUrl(const String &telemetryUrl)
{
    const String suffix = "/telemetry";

    if (telemetryUrl.endsWith(suffix))
    {
        return telemetryUrl.substring(0, telemetryUrl.length() - suffix.length()) + "/device-pairing";
    }

    if (telemetryUrl.endsWith("/"))
    {
        return telemetryUrl + "device-pairing";
    }

    return telemetryUrl + "/device-pairing";
}

void loadConfig()
{
    preferences.begin(PREFERENCES_NAMESPACE, false);

    config.telemetryUrl = trimString(preferences.getString("telemetry_url", DEFAULT_TELEMETRY_INGEST_URL));
    config.deviceKey = trimString(preferences.getString("device_key", DEFAULT_DEVICE_KEY));
    config.pairingCode = normalizePairingCode(preferences.getString("pairing_code", DEFAULT_PAIRING_CODE));
    config.ingestToken = trimString(preferences.getString("ingest_token", ""));

    preferences.end();
}

void saveConfig()
{
    preferences.begin(PREFERENCES_NAMESPACE, false);
    preferences.putString("telemetry_url", config.telemetryUrl);
    preferences.putString("device_key", config.deviceKey);
    preferences.putString("pairing_code", config.pairingCode);
    preferences.putString("ingest_token", config.ingestToken);
    preferences.end();
}

void clearDeviceCredentials()
{
    config.pairingCode = "";
    config.ingestToken = "";
    saveConfig();
}

bool hasRequiredRuntimeConfig()
{
    return config.telemetryUrl.length() > 0 && config.deviceKey.length() > 0;
}

void printCurrentConfig()
{
    Serial.println("Current device configuration:");
    Serial.print("  telemetry_url: ");
    Serial.println(config.telemetryUrl);
    Serial.print("  device_key: ");
    Serial.println(config.deviceKey);
    Serial.print("  pairing_code_set: ");
    Serial.println(config.pairingCode.length() > 0 ? "yes" : "no");
    Serial.print("  ingest_token_set: ");
    Serial.println(config.ingestToken.length() > 0 ? "yes" : "no");
}

void startConfigPortal(bool resetWifi)
{
    char telemetryUrlBuffer[URL_BUFFER_SIZE];
    char deviceKeyBuffer[DEVICE_KEY_BUFFER_SIZE];
    char pairingCodeBuffer[PAIRING_CODE_BUFFER_SIZE];

    strlcpy(telemetryUrlBuffer, config.telemetryUrl.c_str(), sizeof(telemetryUrlBuffer));
    strlcpy(deviceKeyBuffer, config.deviceKey.c_str(), sizeof(deviceKeyBuffer));
    strlcpy(pairingCodeBuffer, config.pairingCode.c_str(), sizeof(pairingCodeBuffer));

    WiFiManager wifiManager;
    wifiManager.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SECONDS);

    WiFiManagerParameter telemetryUrlParam("telemetry_url", "Telemetry ingest URL", telemetryUrlBuffer, sizeof(telemetryUrlBuffer));
    WiFiManagerParameter deviceKeyParam("device_key", "Device key", deviceKeyBuffer, sizeof(deviceKeyBuffer));
    WiFiManagerParameter pairingCodeParam("pairing_code", "Pairing code", pairingCodeBuffer, sizeof(pairingCodeBuffer));

    wifiManager.addParameter(&telemetryUrlParam);
    wifiManager.addParameter(&deviceKeyParam);
    wifiManager.addParameter(&pairingCodeParam);

    if (resetWifi)
    {
        wifiManager.resetSettings();
    }

    Serial.print("Starting setup portal: ");
    Serial.println(CONFIG_AP_SSID);

    const bool connected = wifiManager.startConfigPortal(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);

    if (!connected)
    {
        Serial.println("Setup portal timeout. Restarting...");
        delay(1000);
        ESP.restart();
    }

    config.telemetryUrl = trimString(String(telemetryUrlParam.getValue()));
    config.deviceKey = trimString(String(deviceKeyParam.getValue()));
    config.pairingCode = normalizePairingCode(String(pairingCodeParam.getValue()));

    if (config.telemetryUrl.length() == 0)
    {
        config.telemetryUrl = DEFAULT_TELEMETRY_INGEST_URL;
    }

    saveConfig();
    printCurrentConfig();
    Serial.println("Wi-Fi and device configuration saved");
}

String getSavedWiFiSsid()
{
    wifi_config_t wifiConfig;
    memset(&wifiConfig, 0, sizeof(wifiConfig));

    if (esp_wifi_get_config(WIFI_IF_STA, &wifiConfig) != ESP_OK)
    {
        return "";
    }

    return String(reinterpret_cast<const char *>(wifiConfig.sta.ssid));
}

bool hasSavedWiFiCredentials()
{
    return getSavedWiFiSsid().length() > 0;
}

void printWiFiConnected()
{
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
}

bool connectWiFi(bool openPortalIfNoCredentials)
{
    if (WiFi.status() == WL_CONNECTED)
    {
        wifiWasConnected = true;
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    const String savedSsid = getSavedWiFiSsid();

    if (savedSsid.length() == 0)
    {
        Serial.println("No saved Wi-Fi credentials");

        if (openPortalIfNoCredentials)
        {
            startConfigPortal(false);
            wifiWasConnected = WiFi.status() == WL_CONNECTED;
            return wifiWasConnected;
        }

        return false;
    }

    Serial.println("Connecting to saved Wi-Fi");
    Serial.print("SSID: ");
    Serial.println(savedSsid);

    WiFi.begin();
    lastWiFiReconnectAttemptAt = millis();

    const unsigned long startedAt = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiWasConnected = true;
        printWiFiConnected();
        return true;
    }

    Serial.println("Wi-Fi connection timeout. Will retry without opening setup portal.");
    wifiWasConnected = false;
    return false;
}

void maintainWiFiConnection()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!wifiWasConnected)
        {
            wifiWasConnected = true;
            printWiFiConnected();
            syncTime();
            lastPostAt = millis() - POST_INTERVAL_MS;
        }

        return;
    }

    if (wifiWasConnected)
    {
        Serial.println("Wi-Fi lost. Telemetry will resume after reconnect.");
        wifiWasConnected = false;
    }

    if (!hasSavedWiFiCredentials())
    {
        return;
    }

    if (millis() - lastWiFiReconnectAttemptAt < WIFI_RECONNECT_INTERVAL_MS)
    {
        return;
    }

    lastWiFiReconnectAttemptAt = millis();
    Serial.println("Wi-Fi offline. Trying reconnect with saved credentials.");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin();
}

bool initBme280()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);

    const uint8_t addresses[] = {0x76, 0x77};

    for (const uint8_t address : addresses)
    {
        if (bme.begin(address, &Wire))
        {
            bmeAddress = address;
            Serial.print("BME280 detected at 0x");
            Serial.println(bmeAddress, HEX);
            Serial.print("BME280 sensor ID: 0x");
            Serial.println(bme.sensorID(), HEX);

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

void markBmeReadingFailure()
{
    if (consecutiveInvalidBmeReadings < UINT8_MAX)
    {
        consecutiveInvalidBmeReadings++;
    }

    if (consecutiveInvalidBmeReadings < MAX_INVALID_BME_READINGS)
    {
        return;
    }

    Serial.println("BME280 returned invalid readings repeatedly. Reinitializing sensor.");
    delay(BME_REINIT_DELAY_MS);
    sensorReady = initBme280();
    consecutiveInvalidBmeReadings = 0;
}

bool isMeasurementPlausible(const Measurement &measurement)
{
    const bool validTemperature = measurement.temperatureC >= MIN_VALID_TEMPERATURE_C &&
                                  measurement.temperatureC <= MAX_VALID_TEMPERATURE_C;
    const bool validHumidity = measurement.humidityPct >= MIN_VALID_HUMIDITY_PCT &&
                               measurement.humidityPct <= MAX_VALID_HUMIDITY_PCT;
    const bool validPressure = measurement.pressureHpa >= MIN_VALID_PRESSURE_HPA &&
                               measurement.pressureHpa <= MAX_VALID_PRESSURE_HPA;

    if (validTemperature && validHumidity && validPressure)
    {
        return true;
    }

    Serial.println("BME280 reading outside expected range. Skipping telemetry.");
    Serial.print("  temperature_c: ");
    Serial.println(measurement.temperatureC);
    Serial.print("  pressure_hpa: ");
    Serial.println(measurement.pressureHpa);
    Serial.print("  humidity_pct: ");
    Serial.println(measurement.humidityPct);

    return false;
}

bool readMeasurement(Measurement &measurement)
{
    if (!sensorReady)
    {
        Serial.println("BME280 is not ready. Trying to initialize sensor.");
        sensorReady = initBme280();

        if (!sensorReady)
        {
            markBmeReadingFailure();
            return false;
        }
    }

    if (!bme.takeForcedMeasurement())
    {
        Serial.println("BME280 forced measurement failed");
        markBmeReadingFailure();
        return false;
    }

    measurement.temperatureC = bme.readTemperature();
    measurement.humidityPct = bme.readHumidity();
    measurement.pressureHpa = bme.readPressure() / 100.0F;

    if (isnan(measurement.temperatureC) || isnan(measurement.humidityPct) || isnan(measurement.pressureHpa))
    {
        Serial.println("Invalid BME280 reading");
        markBmeReadingFailure();
        return false;
    }

    if (!isMeasurementPlausible(measurement))
    {
        markBmeReadingFailure();
        return false;
    }

    consecutiveInvalidBmeReadings = 0;
    return true;
}

time_t getUnixTimestamp()
{
    const time_t now = time(nullptr);

    if (now > 1700000000)
    {
        return now;
    }

    return 0;
}

void syncTime()
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
        return;
    }

    Serial.println("NTP sync timeout");
}

String buildTelemetryPayload(const Measurement &measurement)
{
    JsonDocument doc;

    doc["device_key"] = config.deviceKey;
    doc["timestamp"] = static_cast<unsigned long>(getUnixTimestamp());

    JsonObject data = doc["data"].to<JsonObject>();
    data["temperature"] = roundTo2(measurement.temperatureC);
    data["pressure"] = roundTo2(measurement.pressureHpa);
    data["humidity"] = roundTo2(measurement.humidityPct);

    String payload;
    serializeJson(doc, payload);

    return payload;
}

HttpResponse sendJsonRequest(const String &url, const String &payload, const char *deviceTokenHeader = nullptr)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        connectWiFi(false);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Skipping HTTP request: Wi-Fi is offline");
        return {HTTPC_ERROR_CONNECTION_REFUSED, ""};
    }

    HTTPClient http;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    http.setTimeout(HTTP_TIMEOUT_MS);

    bool started = false;

    if (isHttpsUrl(url))
    {
        secureClient.setInsecure();
        started = http.begin(secureClient, url);
    }
    else
    {
        started = http.begin(plainClient, url);
    }

    if (!started)
    {
        Serial.println("HTTP begin failed");
        return {HTTPC_ERROR_CONNECTION_REFUSED, ""};
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");

    if (deviceTokenHeader != nullptr && strlen(deviceTokenHeader) > 0)
    {
        http.addHeader("X-Device-Token", deviceTokenHeader);
    }

    Serial.print("POST ");
    Serial.println(url);
    Serial.print("Payload: ");
    Serial.println(payload);

    const int status = http.POST(payload);
    const String response = http.getString();

    http.end();

    Serial.print("HTTP status: ");
    Serial.println(status);
    Serial.print("Response length: ");
    Serial.println(response.length());

    if (status >= 400 && response.length() > 0)
    {
        Serial.print("Response: ");
        Serial.println(response);
    }

    return {status, response};
}

bool completePairing()
{
    if (config.pairingCode.length() == 0)
    {
        Serial.println("Pairing skipped: no pairing code configured");
        return false;
    }

    JsonDocument doc;
    doc["device_key"] = config.deviceKey;
    doc["pairing_code"] = config.pairingCode;

    String payload;
    serializeJson(doc, payload);

    const HttpResponse response = sendJsonRequest(buildPairingUrl(config.telemetryUrl), payload);

    if (response.status >= 200 && response.status < 300)
    {
        const int jsonStart = response.body.indexOf('{');
        const int jsonEnd = response.body.lastIndexOf('}');

        if (jsonStart < 0 || jsonEnd <= jsonStart)
        {
            Serial.println("Pairing response does not contain a JSON object");
            return false;
        }

        String jsonBody = response.body.substring(jsonStart, jsonEnd + 1);
        JsonDocument body;
        const DeserializationError error = deserializeJson(body, jsonBody);

        if (error != DeserializationError::Ok)
        {
            Serial.print("Pairing response JSON error: ");
            Serial.println(error.c_str());
            Serial.print("Pairing response length: ");
            Serial.println(response.body.length());
            Serial.print("Extracted JSON length: ");
            Serial.println(jsonBody.length());
            Serial.print("Free heap: ");
            Serial.println(ESP.getFreeHeap());
            return false;
        }

        const char *token = body["data"]["ingest_token"] | "";
        const char *telemetryUrl = body["data"]["telemetry_ingest_url"] | "";

        config.ingestToken = trimString(String(token));

        if (config.ingestToken.length() == 0)
        {
            Serial.println("Pairing response did not contain an ingest token");
            return false;
        }

        if (strlen(telemetryUrl) > 0)
        {
            config.telemetryUrl = trimString(String(telemetryUrl));
        }

        config.pairingCode = "";
        saveConfig();

        Serial.println("Device pairing completed and ingest token saved");
        return true;
    }

    if (response.status == 404 || response.status == 422)
    {
        telemetryBlockedByConfiguration = true;
        Serial.println("Pairing rejected: check device key and pairing code in the setup portal");
    }

    return false;
}

void logIngestMeta(const String &response)
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

bool postTelemetry(const String &payload)
{
    const unsigned long retryDelays[] = {0, FIRST_RETRY_DELAY_MS, SECOND_RETRY_DELAY_MS};

    for (uint8_t attempt = 0; attempt < HTTP_MAX_ATTEMPTS; attempt++)
    {
        if (retryDelays[attempt] > 0)
        {
            Serial.print("Retrying in ms: ");
            Serial.println(retryDelays[attempt]);
            delay(retryDelays[attempt]);
        }

        const HttpResponse response = sendJsonRequest(config.telemetryUrl, payload, config.ingestToken.c_str());

        if (response.status >= 200 && response.status < 300)
        {
            logIngestMeta(response.body);
            return true;
        }

        if (response.status == 401)
        {
            Serial.println("Ingest token rejected. Clearing saved token and returning to pairing mode.");
            config.ingestToken = "";
            saveConfig();
            return false;
        }

        if (response.status == 404)
        {
            telemetryBlockedByConfiguration = true;
            Serial.println("Telemetry rejected: device_key was not found in telemetry-core");
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

void handleConfigButton()
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
            clearDeviceCredentials();
            telemetryBlockedByConfiguration = false;
            startConfigPortal(true);
            configButtonPressedAt = 0;
            lastPostAt = millis() - POST_INTERVAL_MS;
        }

        return;
    }

    configButtonPressedAt = 0;
}

void ensureDeviceIsPaired()
{
    if (config.ingestToken.length() > 0)
    {
        return;
    }

    if (config.pairingCode.length() == 0)
    {
        Serial.println("No ingest token and no pairing code. Opening setup portal.");
        startConfigPortal(false);
    }

    if (config.pairingCode.length() > 0)
    {
        completePairing();
    }
}
} // namespace

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP32-C3 BME280 telemetry firmware starting");

    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

    loadConfig();
    printCurrentConfig();

    sensorReady = initBme280();
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);

    if (!hasRequiredRuntimeConfig())
    {
        Serial.println("Missing runtime config. Opening setup portal.");
        startConfigPortal(false);
    }

    connectWiFi(true);

    if (WiFi.status() == WL_CONNECTED)
    {
        syncTime();
        ensureDeviceIsPaired();
    }

    lastPostAt = millis() - POST_INTERVAL_MS;
}

void loop()
{
    handleConfigButton();
    maintainWiFiConnection();

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

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Telemetry waiting for Wi-Fi reconnect");
        return;
    }

    if (WiFi.status() == WL_CONNECTED && getUnixTimestamp() == 0)
    {
        syncTime();
    }

    ensureDeviceIsPaired();

    if (config.ingestToken.length() == 0)
    {
        Serial.println("Telemetry skipped: device is not paired yet");
        return;
    }

    if (getUnixTimestamp() == 0)
    {
        Serial.println("Skipping POST: timestamp is not synced");
        return;
    }

    Measurement measurement{};

    if (!readMeasurement(measurement))
    {
        return;
    }

    const String payload = buildTelemetryPayload(measurement);
    const bool ok = postTelemetry(payload);

    Serial.println(ok ? "Telemetry sent" : "Telemetry send failed");
}
