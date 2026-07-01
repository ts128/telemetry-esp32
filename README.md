# Telemetry ESP32

Firmware dla ESP32-C3 Super Mini z BME280, provisioningiem przez WiFiManager i wysylka danych do telemetry-core.

## Wi-Fi i provisioning

- Przy pierwszym uruchomieniu bez zapisanych danych Wi-Fi urzadzenie otwiera portal `Telemetry-Setup`.
- Zapisane dane Wi-Fi sa uzywane po restarcie przez standardowy tryb STA.
- Chwilowa utrata sieci nie otwiera portalu konfiguracji.
- Po utracie Wi-Fi firmware ponawia reconnect co 10 s i po odzyskaniu polaczenia wraca do wysylki telemetry.
- Portal setup mozna wymusic przyciskiem konfiguracji na GPIO 9 przytrzymanym przez 5 s.

## BME280

Domyslne piny I2C:

- SDA: GPIO 5
- SCL: GPIO 6

Firmware sprawdza adresy BME280 `0x76` i `0x77`.
