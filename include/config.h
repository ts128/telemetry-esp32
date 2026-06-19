#pragma once

// Default values shown in the customer setup portal.
// Wi-Fi SSID and password are entered in the portal and stored by WiFiManager.
static constexpr const char *DEFAULT_TELEMETRY_CORE_URL = "http://192.168.1.50:8080/api/telemetry";
static constexpr const char *DEFAULT_TELEMETRY_BEARER_TOKEN = "";
static constexpr const char *DEFAULT_DEVICE_LOCATION = "site";
static constexpr const char *DEVICE_ID_PREFIX = "esp32-c3-bme280";

// Captive portal shown on first boot, after failed Wi-Fi connect, or after holding
// the config button for CONFIG_BUTTON_HOLD_MS.
static constexpr const char *CONFIG_AP_SSID = "Telemetry-Setup";
static constexpr const char *CONFIG_AP_PASSWORD = "telemetry123";
static constexpr int CONFIG_BUTTON_PIN = 9;
static constexpr unsigned long CONFIG_BUTTON_HOLD_MS = 5000;
static constexpr unsigned long CONFIG_PORTAL_TIMEOUT_SECONDS = 180;

// ESP32-C3 I2C pins used by the BME280 module.
static constexpr int I2C_SDA_PIN = 5;
static constexpr int I2C_SCL_PIN = 6;

// Sensor and network timings.
static constexpr unsigned long POST_INTERVAL_MS = 30000;
static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr unsigned long HTTP_TIMEOUT_MS = 8000;
