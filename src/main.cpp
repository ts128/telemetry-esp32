#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include "config.h"

Adafruit_BME280 bme;

static bool sensorReady = false;
static uint8_t bmeAddress = 0;
static unsigned long lastPostAt = 0;

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

static void connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return;
    }

    Serial.print("Connecting to Wi-Fi SSID: ");
    Serial.println(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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

static String buildPayload(const Measurement &measurement)
{
    JsonDocument doc;

    doc["device_id"] = DEVICE_ID;
    doc["location"] = DEVICE_LOCATION;
    doc["sensor"] = "bme280";
    doc["bme280_address"] = bmeAddress;
    doc["temperature_celsius"] = roundTo2(measurement.temperatureC);
    doc["humidity_pct"] = roundTo2(measurement.humidityPct);
    doc["pressure_hpa"] = roundTo2(measurement.pressureHpa);
    doc["wifi_rssi_dbm"] = WiFi.RSSI();
    doc["uptime_ms"] = millis();

    String payload;
    serializeJson(doc, payload);
    return payload;
}

static bool postTelemetry(const String &payload)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        connectWiFi();
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Skipping POST: Wi-Fi is offline");
        return false;
    }

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);

    if (!http.begin(TELEMETRY_CORE_URL))
    {
        Serial.println("HTTP begin failed");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Id", DEVICE_ID);

    if (strlen(TELEMETRY_BEARER_TOKEN) > 0)
    {
        http.addHeader("Authorization", String("Bearer ") + TELEMETRY_BEARER_TOKEN);
    }

    Serial.print("POST ");
    Serial.println(TELEMETRY_CORE_URL);
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

    return status >= 200 && status < 300;
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP32-C3 BME280 telemetry firmware starting");

    sensorReady = initBme280();
    connectWiFi();
    lastPostAt = millis() - POST_INTERVAL_MS;
}

void loop()
{
    if (millis() - lastPostAt < POST_INTERVAL_MS)
    {
        delay(100);
        return;
    }

    lastPostAt = millis();

    Measurement measurement{};
    if (!readMeasurement(measurement))
    {
        return;
    }

    const String payload = buildPayload(measurement);
    const bool ok = postTelemetry(payload);

    Serial.println(ok ? "Telemetry sent" : "Telemetry send failed");
}
