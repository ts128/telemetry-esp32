#pragma once

// Wi-Fi credentials.
// Replace these values before flashing.
static constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
static constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// telemetry-core endpoint that accepts HTTP POST with JSON payload.
// Example: "http://192.168.1.50:8080/api/telemetry"
static constexpr const char *TELEMETRY_CORE_URL = "http://192.168.1.50:8080/api/telemetry";

// Optional bearer token. Leave empty when telemetry-core does not require auth.
static constexpr const char *TELEMETRY_BEARER_TOKEN = "";

// Device metadata sent in every payload.
static constexpr const char *DEVICE_ID = "esp32-c3-bme280-01";
static constexpr const char *DEVICE_LOCATION = "lab";

// ESP32-C3 I2C pins used by the BME280 module.
static constexpr int I2C_SDA_PIN = 5;
static constexpr int I2C_SCL_PIN = 6;

// Sensor and network timings.
static constexpr unsigned long POST_INTERVAL_MS = 30000;
static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr unsigned long HTTP_TIMEOUT_MS = 8000;
