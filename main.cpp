/*
  Czujnik jakości powietrza - ESP32 (XIAO ESP32S3)
  Sensory: SPS30 (PM1/2.5/4/10), BME280 (temp/wilgotność/ciśnienie), SHT40 (temp/wilgotność precyzyjna)

  Funkcje:
   - Pierwsze uruchomienie: tryb AP + captive portal (WiFiManager) do wpisania
     danych WiFi, IP przekaźnika Shelly oraz progu PM2.5
   - Lokalny panel WWW (działa ZAWSZE, niezależnie od Home Assistant) pod adresem z DHCP:
     odczyty czujników, edycja/zmiana sieci WiFi, sterowanie przekaźnikiem Shelly
     (auto wg progu PM2.5 albo ręcznie on/off), konfiguracja progu i histerezy
   - Sterowanie przekaźnikiem Shelly (Gen2+, RPC API: /rpc/Switch.Set) -
     WYLACZA rekuperacje po przekroczeniu progu PM (z histereza), WLACZA
     z powrotem gdy powietrze sie poprawi - albo recznie z panelu
   - Opcjonalna integracja z Home Assistant przez MQTT Discovery - jeśli broker
     nie jest skonfigurowany albo niedostępny, cała reszta działa normalnie dalej
   - Fizyczny reset: przytrzymanie wbudowanego przycisku BOOT (GPIO0) przez 5s
     kasuje WiFi i wszystkie ustawienia, na wypadek utraty dostępu do sieci i AP

  Biblioteka SPS30 (paulvha/sps30): API zweryfikowane na podstawie oficjalnego
  przykładu Example1_sps30_BasicReadings z repozytorium biblioteki -
  sps30.begin(I2C_COMMS) / .probe() / .start() / .GetValues(&val), pola
  val.MassPM1/MassPM2/MassPM4/MassPM10 (MassPM2 to PM2.5). Brak metody
  dataAvailable() w tej wersji - GetValues() zwraca SPS30_ERR_DATALENGTH,
  gdy dane jeszcze nie są gotowe, więc kod po prostu próbuje ponownie przy
  kolejnym cyklu odczytu.
*/

#include <Arduino.h>
#include <esp_random.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SHT4x.h>
#include <sps30.h>
#include <PubSubClient.h>

// ---------- Piny I2C ----------
// Wartości przychodzą z platformio.ini (osobne dla env:devkit_v1 i env:xiao_esp32s3),
// więc przełączenie płytki = zmiana jednej linijki w platformio.ini, nic tutaj.
// Domyślne wartości poniżej to fallback na wypadek budowania spoza PlatformIO.
#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 21
#endif
#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 22
#endif

// ---------- Fizyczny reset (przycisk BOOT wbudowany w plytke) ----------
// Zarowno DevKitV1 jak i XIAO ESP32S3 maja wbudowany przycisk BOOT/FLASH
// podpiety pod GPIO0 - nie trzeba nic dolutowywac. Dwa progi przytrzymania:
//  - 3-8s (puszczony w tym oknie): kasuje TYLKO haslo do panelu (WiFi,
//    Shelly, MQTT zostaja bez zmian) - na wypadek zapomnienia hasla
//  - 8s+ (trzymany do konca): pelny reset fabryczny - WiFi i wszystkie
//    ustawienia, dokladnie jak przy pierwszym uruchomieniu
#define RESET_BUTTON_PIN 0
#define AUTH_RESET_HOLD_MS 3000
#define FACTORY_RESET_HOLD_MS 8000

// Nazwa pod ktora urzadzenie bedzie dostepne w sieci lokalnej jako
// http://czujnik-powietrza.local/ - dziala automatycznie (mDNS), bez
// potrzeby znajomości adresu IP. Jesli kiedys zbudujesz wiecej niz jedno
// takie urzadzenie w tej samej sieci, kazde musi miec inna nazwe tutaj.
#define MDNS_HOSTNAME "czujnik-powietrza"

// Haslo do aktualizacji firmware przez WiFi (OTA) - BEZ TEGO KAZDY W TWOJEJ
// SIECI MOZE WGRAC DOWOLNY KOD NA URZADZENIE. Zmien na wlasne, silne haslo
// przed uzyciem OTA. Ta sama wartosc musi byc wpisana w platformio.ini
// (upload_flags = --auth=...) dla srodowisk z "_ota" w nazwie - obie musza
// sie zgadzac.
#define OTA_PASSWORD "zmien-to-haslo-123"

// ---------- Obiekty czujników ----------
Adafruit_BME280 bme;
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
SPS30 sps30;
bool bmeOk = false;
bool shtOk = false;
bool spsOk = false;

// ---------- Serwer WWW i konfiguracja ----------
AsyncWebServer server(80);
Preferences prefs;

// ---------- MQTT (opcjonalnie, dla Home Assistant) ----------
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
String deviceId;

// ---------- Konfiguracja zapisywana w pamięci NVS ----------
struct Config {
  char relayIp[32] = "";
  float pm25Threshold = 35.0;   // ug/m3 - wartość domyślna, edytowalna z panelu
  float pm25Hysteresis = 5.0;   // margines, żeby uniknąć częstego przełączania
  bool relayAutoMode = true;    // true = steruje automatyka wg progu PM, false = ręcznie z panelu
  char mqttBroker[64] = "";
  int  mqttPort = 1883;
  char mqttUser[32] = "";
  char mqttPass[32] = "";
  bool panelAuthEnabled = false;  // domyslnie WYLACZONE - decyzja nalezy do uzytkownika
  char panelUser[32] = "admin";
  char panelPass[32] = "";
} config;

void loadConfig() {
  prefs.begin("airq", true);
  prefs.getString("relayIp", config.relayIp, sizeof(config.relayIp));
  config.pm25Threshold = prefs.getFloat("pm25Thr", config.pm25Threshold);
  config.pm25Hysteresis = prefs.getFloat("pm25Hys", config.pm25Hysteresis);
  config.relayAutoMode = prefs.getBool("relayAuto", config.relayAutoMode);
  prefs.getString("mqttBroker", config.mqttBroker, sizeof(config.mqttBroker));
  config.mqttPort = prefs.getInt("mqttPort", config.mqttPort);
  prefs.getString("mqttUser", config.mqttUser, sizeof(config.mqttUser));
  prefs.getString("mqttPass", config.mqttPass, sizeof(config.mqttPass));
  config.panelAuthEnabled = prefs.getBool("authEn", config.panelAuthEnabled);
  prefs.getString("authUser", config.panelUser, sizeof(config.panelUser));
  prefs.getString("authPass", config.panelPass, sizeof(config.panelPass));
  prefs.end();
}

void saveConfig() {
  prefs.begin("airq", false);
  prefs.putString("relayIp", config.relayIp);
  prefs.putFloat("pm25Thr", config.pm25Threshold);
  prefs.putFloat("pm25Hys", config.pm25Hysteresis);
  prefs.putBool("relayAuto", config.relayAutoMode);
  prefs.putString("mqttBroker", config.mqttBroker);
  prefs.putInt("mqttPort", config.mqttPort);
  prefs.putString("mqttUser", config.mqttUser);
  prefs.putString("mqttPass", config.mqttPass);
  prefs.putBool("authEn", config.panelAuthEnabled);
  prefs.putString("authUser", config.panelUser);
  prefs.putString("authPass", config.panelPass);
  prefs.end();
}

// ---------- Aktualne odczyty ----------
struct Readings {
  float pm1 = 0, pm25 = 0, pm4 = 0, pm10 = 0;
  float tempBme = 0, humBme = 0, pressure = 0;
  float tempSht = 0, humSht = 0;
  bool relayOn = false;
  unsigned long lastUpdate = 0;
} readings;

unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL_MS = 5000;
unsigned long lastMqttPublish = 0;
const unsigned long MQTT_PUBLISH_INTERVAL_MS = 10000;
unsigned long lastRelayStatusCheck = 0;
const unsigned long RELAY_STATUS_INTERVAL_MS = 15000;
unsigned long pendingRestartAt = 0;  // 0 = brak zaplanowanego restartu

// ---------- Monitorowanie polaczenia WiFi w trakcie pracy ----------
unsigned long wifiDisconnectedSince = 0;    // 0 = polaczony (albo jeszcze nie sprawdzano)
unsigned long lastWifiReconnectAttempt = 0;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000;
const unsigned long WIFI_RESTART_AFTER_MS = 300000;

// ================== CZUJNIKI ==================

// Szybkie, pojedyncze sprawdzenie czy cokolwiek odpowiada pod danym adresem
// I2C - zanim w ogole wywolamy biblioteke czujnika. Dzieki temu, jesli
// czujnik fizycznie nie jest podlaczony, w ogole nie wchodzimy w jego
// begin()/probe(), ktore u niektorych bibliotek potrafia sie zapetlac
// we wlasnych, wewnetrznych probach ponawiania.
bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void setupSensors() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  if (i2cDevicePresent(0x76) || i2cDevicePresent(0x77)) {
    bmeOk = bme.begin(0x76) || bme.begin(0x77);
  }
  if (!bmeOk) Serial.println("BME280: nie wykryto na I2C - pomijam");

  if (i2cDevicePresent(0x44)) {
    shtOk = sht4.begin();
    if (shtOk) {
      sht4.setPrecision(SHT4X_HIGH_PRECISION);
      sht4.setHeater(SHT4X_NO_HEATER);
    }
  }
  if (!shtOk) Serial.println("SHT40: nie wykryto na I2C - pomijam");

  if (!i2cDevicePresent(0x69)) {
    Serial.println("SPS30: nie wykryto na I2C - pomijam");
    spsOk = false;
  } else if (!sps30.begin(I2C_COMMS)) {
    Serial.println("SPS30: nie udalo sie zainicjowac komunikacji");
    spsOk = false;
  } else {
    // sps30.begin(I2C_COMMS) moze wewnetrznie zresetowac magistrale do
    // domyslnych pinow ESP32 - odtwarzamy nasze na wszelki wypadek.
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!sps30.probe()) {
      Serial.println("SPS30: brak polaczenia");
      spsOk = false;
    } else if (!sps30.start()) {
      Serial.println("SPS30: nie udalo sie uruchomic pomiaru");
      spsOk = false;
    } else {
      spsOk = true;
    }
  }
}

// Wygladzanie odczytow PM (SPS30 potrafi mocno skakac probka do probki) -
// wykladnicza srednia ruchoma (EMA). Nizsza wartosc = wiecej wygladzania,
// ale wolniejsza reakcja na realna zmiane; wyzsza = szybciej reaguje,
// ale mniej wygladza. Pierwszy odczyt po starcie/wykryciu czujnika ustawia
// wartosc bezposrednio (bez wygladzania), zeby nie czekac az EMA "dojdzie".
const float PM_EMA_ALPHA = 0.3;
bool pmEmaInitialized = false;

void readSensors() {
  if (bmeOk) {
    readings.tempBme = bme.readTemperature();
    readings.humBme = bme.readHumidity();
    readings.pressure = bme.readPressure() / 100.0F;
  }

  if (shtOk) {
    sensors_event_t humEvent, tempEvent;
    sht4.getEvent(&humEvent, &tempEvent);
    readings.tempSht = tempEvent.temperature;
    readings.humSht = humEvent.relative_humidity;
  }

  if (spsOk) {
    struct sps_values val;
    uint8_t ret = sps30.GetValues(&val);
    if (ret == SPS30_ERR_OK) {
      if (!pmEmaInitialized) {
        readings.pm1 = val.MassPM1;
        readings.pm25 = val.MassPM2;
        readings.pm4 = val.MassPM4;
        readings.pm10 = val.MassPM10;
        pmEmaInitialized = true;
      } else {
        readings.pm1  = PM_EMA_ALPHA * val.MassPM1  + (1 - PM_EMA_ALPHA) * readings.pm1;
        readings.pm25 = PM_EMA_ALPHA * val.MassPM2  + (1 - PM_EMA_ALPHA) * readings.pm25;
        readings.pm4  = PM_EMA_ALPHA * val.MassPM4  + (1 - PM_EMA_ALPHA) * readings.pm4;
        readings.pm10 = PM_EMA_ALPHA * val.MassPM10 + (1 - PM_EMA_ALPHA) * readings.pm10;
      }
    }
    // SPS30_ERR_DATALENGTH = dane jeszcze nie gotowe, sproboj przy nastepnym cyklu
  }

  readings.lastUpdate = millis();
}

// ================== STEROWANIE PRZEKAŹNIKIEM ==================

// Pozwala wpisac adres Shelly zarowno jako samo IP ("192.168.30.34"),
// jak i pelny URL ("http://192.168.30.34/") - i tak, i tak dziala.
String normalizeHostOrIp(String s) {
  s.trim();
  String lower = s;
  lower.toLowerCase();
  if (lower.startsWith("http://")) s = s.substring(7);
  else if (lower.startsWith("https://")) s = s.substring(8);
  int slashPos = s.indexOf('/');
  if (slashPos >= 0) s = s.substring(0, slashPos);
  return s;
}

void triggerRelay(bool turnOn) {
  if (strlen(config.relayIp) == 0) return;

  HTTPClient http;
  String url = "http://" + String(config.relayIp) + "/rpc/Switch.Set?id=0&on=" + (turnOn ? "true" : "false");
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  Serial.printf("Shelly GET %s -> %d\n", url.c_str(), code);
  http.end();

  readings.relayOn = turnOn;
}

// Odpytuje Shelly o rzeczywisty stan (przyda się, jesli ktos przelaczyl
// wtyczke recznie na obudowie albo w aplikacji Shelly - panel ma wtedy
// prawdziwy stan, a nie tylko to, co sami ostatnio wyslalismy).
void fetchRelayStatus() {
  if (strlen(config.relayIp) == 0) return;

  HTTPClient http;
  String url = "http://" + String(config.relayIp) + "/rpc/Switch.GetStatus?id=0";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      readings.relayOn = doc["output"] | readings.relayOn;
    }
  }
  http.end();
}

void checkThreshold() {
  if (strlen(config.relayIp) == 0 || !spsOk || !config.relayAutoMode) return;

  // Powietrze na zewnatrz zle (np. smog/dym) -> wylacz rekuperacje, zeby nie
  // wciagac go do domu. Gdy sie poprawi (z zapasem histerezy) -> wlacz z powrotem.
  if (readings.relayOn && readings.pm25 > config.pm25Threshold) {
    triggerRelay(false);
  } else if (!readings.relayOn && readings.pm25 < (config.pm25Threshold - config.pm25Hysteresis)) {
    triggerRelay(true);
  }
}

// ================== MQTT / HOME ASSISTANT DISCOVERY (opcjonalne) ==================

void publishDiscovery() {
  String base = "homeassistant/sensor/" + deviceId;
  String devBlock = "\"device\":{\"identifiers\":[\"" + deviceId +
                     "\"],\"name\":\"Czujnik jakosci powietrza\",\"manufacturer\":\"DIY\",\"model\":\"ESP32S3\"}";
  String stateTopic = "airq/" + deviceId + "/state";

  struct { const char* key; const char* name; const char* unit; const char* devClass; } sensorsMeta[] = {
    {"pm25", "PM2.5", "\u00b5g/m\u00b3", "pm25"},
    {"pm10", "PM10", "\u00b5g/m\u00b3", "pm10"},
    {"temp", "Temperatura", "\u00b0C", "temperature"},
    {"hum", "Wilgotnosc", "%", "humidity"},
    {"press", "Cisnienie", "hPa", "pressure"},
  };

  for (auto &s : sensorsMeta) {
    String topic = base + "_" + s.key + "/config";
    String uniqueId = deviceId + "_" + s.key;
    String payload = "{\"name\":\"" + String(s.name) + "\",\"unique_id\":\"" + uniqueId +
                      "\",\"state_topic\":\"" + stateTopic + "\",\"unit_of_measurement\":\"" + s.unit +
                      "\",\"device_class\":\"" + s.devClass + "\",\"value_template\":\"{{ value_json." + s.key + " }}\"," +
                      devBlock + "}";
    bool ok = mqttClient.publish(topic.c_str(), payload.c_str(), true);
    if (!ok) {
      Serial.printf("MQTT discovery FAILED dla %s (dlugosc payloadu: %d bajtow)\n", s.key, payload.length());
    }
  }
}

unsigned long lastMqttAttempt = 0;
const unsigned long MQTT_RETRY_INTERVAL_MS = 5000;

// Tlumaczy kod stanu PubSubClient na czytelny opis - przydatne w panelu,
// zeby bylo widac DLACZEGO MQTT sie nie laczy, zamiast zgadywac.
String mqttStateText(int state) {
  switch (state) {
    case -4: return "Timeout polaczenia z brokerem (zly adres/port albo broker nieosiagalny)";
    case -3: return "Polaczenie zerwane";
    case -2: return "Nie udalo sie polaczyc z brokerem (sprawdz adres IP i port)";
    case -1: return "Rozlaczony";
    case 0:  return "Polaczony";
    case 1:  return "Zla wersja protokolu MQTT";
    case 2:  return "Broker odrzucil identyfikator klienta";
    case 3:  return "Broker niedostepny";
    case 4:  return "Bledny login lub haslo";
    case 5:  return "Brak autoryzacji (broker wymaga logowania albo konto nie ma uprawnien)";
    default: return "Nieznany stan";
  }
}

void mqttReconnect() {
  if (strlen(config.mqttBroker) == 0) return;
  if (mqttClient.connected()) return;
  if (millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttAttempt = millis();

  mqttClient.setServer(config.mqttBroker, config.mqttPort);
  bool ok = mqttClient.connect(deviceId.c_str(), config.mqttUser, config.mqttPass);
  if (ok) {
    Serial.println("MQTT polaczony");
    publishDiscovery();
  } else {
    Serial.print("MQTT nie polaczony, kod: ");
    Serial.println(mqttClient.state());
  }
}

void publishState() {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["pm1"] = readings.pm1;
  doc["pm25"] = readings.pm25;
  doc["pm4"] = readings.pm4;
  doc["pm10"] = readings.pm10;
  doc["temp"] = bmeOk ? readings.tempBme : readings.tempSht;
  doc["hum"] = bmeOk ? readings.humBme : readings.humSht;
  doc["press"] = readings.pressure;
  doc["relay"] = readings.relayOn;

  String payload;
  serializeJson(doc, payload);
  String stateTopic = "airq/" + deviceId + "/state";
  mqttClient.publish(stateTopic.c_str(), payload.c_str());
}

// ================== PANEL WWW ==================

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Czujnik jakości powietrza</title>
<style>
:root{
  --bg:#f2f3f5; --card-bg:#fff; --text:#1a1a1a; --text-sec:#666; --text-muted:#888;
  --border:#f0f0f0; --input-border:#d8d8d8; --input-bg:#fff; --pill-bg:#fff; --pill-border:#ddd;
  --accent:#2b6cb0; --sec-btn-bg:#eef0f3; --sec-btn-text:#333; --shadow:rgba(0,0,0,.06);
}
@media (prefers-color-scheme:dark){:root{
  --bg:#14161a; --card-bg:#1e2126; --text:#e7e7e7; --text-sec:#a3a3a3; --text-muted:#7d7d7d;
  --border:#2a2d33; --input-border:#3a3d44; --input-bg:#25282e; --pill-bg:#1e2126; --pill-border:#33363c;
  --accent:#5b9bd5; --sec-btn-bg:#2a2d33; --sec-btn-text:#ddd; --shadow:rgba(0,0,0,.4);
}}
html[data-theme="dark"]{
  --bg:#14161a; --card-bg:#1e2126; --text:#e7e7e7; --text-sec:#a3a3a3; --text-muted:#7d7d7d;
  --border:#2a2d33; --input-border:#3a3d44; --input-bg:#25282e; --pill-bg:#1e2126; --pill-border:#33363c;
  --accent:#5b9bd5; --sec-btn-bg:#2a2d33; --sec-btn-text:#ddd; --shadow:rgba(0,0,0,.4);
}
html[data-theme="light"]{
  --bg:#f2f3f5; --card-bg:#fff; --text:#1a1a1a; --text-sec:#666; --text-muted:#888;
  --border:#f0f0f0; --input-border:#d8d8d8; --input-bg:#fff; --pill-bg:#fff; --pill-border:#ddd;
  --accent:#2b6cb0; --sec-btn-bg:#eef0f3; --sec-btn-text:#333; --shadow:rgba(0,0,0,.06);
}
*{box-sizing:border-box}
body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:520px;margin:0 auto;padding:20px 16px 40px;background:var(--bg);color:var(--text);transition:background .2s,color .2s}
h1{font-size:19px;margin:0 0 4px}
h2{font-size:14px;margin:0 0 12px;color:var(--text-sec);text-transform:uppercase;letter-spacing:.03em}
.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:18px;gap:8px}
.top-right{display:flex;align-items:center;gap:8px}
.pill{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:20px;font-size:12px;background:var(--pill-bg);border:1px solid var(--pill-border)}
.dot{width:8px;height:8px;border-radius:50%;background:#9ae6b4}
.iconbtn{width:40px;height:40px;border-radius:50%;border:1px solid var(--pill-border);background:var(--pill-bg);color:var(--text);cursor:pointer;font-size:18px;line-height:1;display:flex;align-items:center;justify-content:center;margin-top:0;padding:0}
.card{background:var(--card-bg);border-radius:14px;padding:18px;margin-bottom:14px;box-shadow:0 1px 2px var(--shadow)}
.row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid var(--border)}
.row:last-child{border-bottom:none}
.row span:first-child{color:var(--text-sec);font-size:14px}
.val{font-weight:600}
label{display:block;margin-top:12px;margin-bottom:4px;font-size:13px;color:var(--text-sec)}
input{width:100%;padding:9px 10px;border:1px solid var(--input-border);border-radius:8px;font-size:14px;background:var(--input-bg);color:var(--text)}
input:focus{outline:2px solid var(--accent);border-color:transparent}
button{margin-top:14px;padding:10px 16px;border:none;border-radius:8px;background:var(--accent);color:#fff;cursor:pointer;font-size:14px;font-weight:500}
button.secondary{background:var(--sec-btn-bg);color:var(--sec-btn-text)}
button:disabled{opacity:.4;cursor:not-allowed}
.badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:600}
.aqi-good{background:#c6f6d5;color:#22543d}
.aqi-mid{background:#fefcbf;color:#744210}
.aqi-poor{background:#feebc8;color:#7b341e}
.aqi-bad{background:#fed7d7;color:#822727}
.aqi-verybad{background:#e9d8fd;color:#44337a}
.relay-row{display:flex;justify-content:space-between;align-items:center;margin-top:10px}
.switch{position:relative;display:inline-block;width:44px;height:24px;flex-shrink:0}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;inset:0;background:#ccc;border-radius:24px;transition:.2s}
.slider:before{content:"";position:absolute;height:18px;width:18px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.2s}
input:checked + .slider{background:var(--accent)}
input:checked + .slider:before{transform:translateX(20px)}
.manual-btns{display:flex;gap:8px;margin-top:12px}
.manual-btns button{flex:1;margin-top:0}
.hint{font-size:12px;color:var(--text-muted);margin-top:8px;line-height:1.5}
.msg{font-size:13px;margin-top:10px;padding:8px 10px;border-radius:8px;display:none}
.msg.ok{background:#c6f6d5;color:#22543d;display:block}
.msg.err{background:#fed7d7;color:#822727;display:block}
.sensor-warn{display:none;background:#fefcbf;color:#744210;font-size:13px;padding:8px 10px;border-radius:8px;margin-bottom:10px}
</style></head><body>

<div class="top">
  <div><h1>Jakość powietrza</h1></div>
  <div class="top-right">
    <div class="pill"><span class="dot"></span><span id="wifiPill">-</span></div>
    <button class="iconbtn" id="themeToggle" aria-label="Przełącz motyw">-</button>
  </div>
</div>

<div class="card">
  <h2>Odczyty</h2>
  <div class="sensor-warn" id="sensorWarn"></div>
  <div class="row"><span>PM2.5</span><span id="pm25" class="badge">-</span></div>
  <div class="row"><span>PM1.0</span><span class="val" id="pm1">-</span></div>
  <div class="row"><span>PM4.0</span><span class="val" id="pm4">-</span></div>
  <div class="row"><span>PM10</span><span class="val" id="pm10">-</span></div>
  <div class="row"><span>Temperatura</span><span class="val" id="temp">-</span></div>
  <div class="row"><span>Wilgotność</span><span class="val" id="hum">-</span></div>
  <div class="row"><span>Ciśnienie</span><span class="val" id="press">-</span></div>
</div>

<div class="card">
  <h2>Przekaźnik (Shelly)</h2>
  <label>Adres IP Shelly</label>
  <input id="relayIp" placeholder="np. 192.168.1.50 lub http://192.168.1.50/">
  <div class="relay-row">
    <span>Tryb automatyczny (wg progu PM2.5)</span>
    <label class="switch">
      <input type="checkbox" id="relayAuto">
      <span class="slider"></span>
    </label>
  </div>
  <div class="manual-btns">
    <button class="secondary" id="btnOff">Wyłącz</button>
    <button id="btnOn">Włącz</button>
  </div>
  <div class="hint">Ręczne przełączenie automatycznie wyłącza tryb automatyczny powyżej.</div>
  <label>Próg PM2.5, powyżej którego przekaźnik się wyłącza (µg/m³)</label>
  <input id="pm25Threshold" type="number" step="0.1">
  <label>Histereza (µg/m³)</label>
  <input id="pm25Hysteresis" type="number" step="0.1">
  <button id="btnSaveRelay">Zapisz</button>
  <div class="msg" id="relayMsg"></div>
</div>

<div class="card">
  <h2>Sieć WiFi</h2>
  <div class="row"><span>Adres panelu</span><span class="val">czujnik-powietrza.local</span></div>
  <div class="row"><span>Połączono z</span><span class="val" id="wifiSsid">-</span></div>
  <div class="row"><span>Sygnał</span><span class="val" id="wifiRssi">-</span></div>
  <label>Połącz z inną siecią</label>
  <input id="newSsid" placeholder="nazwa sieci (SSID)">
  <label>Hasło</label>
  <input id="newPass" type="password">
  <button id="btnWifiConnect">Połącz</button>
  <button class="secondary" id="btnWifiForget">Zapomnij sieć i uruchom portal konfiguracyjny</button>
  <div class="hint">Portal konfiguracyjny (AP) uruchamia się po restarcie, jeśli urządzenie nie połączy się z żadną zapamiętaną siecią. Po połączeniu z nową siecią urządzenie się zrestartuje i dostanie nowy adres IP.</div>
  <div class="msg" id="wifiMsg"></div>
</div>

<div class="card">
  <h2>Home Assistant (opcjonalnie, przez MQTT)</h2>
  <div class="row"><span>Status</span><span class="val" id="mqttStatus">-</span></div>
  <label>Broker MQTT (adres IP)</label>
  <input id="mqttBroker" placeholder="np. 192.168.1.10">
  <label>Użytkownik (jeśli broker wymaga logowania)</label>
  <input id="mqttUser" placeholder="opcjonalnie">
  <label>Hasło</label>
  <input id="mqttPass" type="password" placeholder="zostaw puste, jeśli bez zmian">
  <button id="btnSaveMqtt">Zapisz</button>
  <div class="msg" id="mqttMsg"></div>
  <div class="hint">To musi być adres działającego brokera MQTT (np. dodatku "Mosquitto broker" w Home Assistant), a NIE adres samego Home Assistant. Domyślny dodatek Mosquitto zwykle wymaga logowania - konto stworzysz w HA: Ustawienia -> Osoby -> Użytkownicy.</div>
</div>

<div class="card">
  <h2>Bezpieczeństwo panelu</h2>
  <div class="relay-row">
    <span>Wymagaj hasła do panelu</span>
    <label class="switch">
      <input type="checkbox" id="authEnabled">
      <span class="slider"></span>
    </label>
  </div>
  <label>Login</label>
  <input id="authUser" placeholder="admin">
  <label>Hasło</label>
  <input id="authPass" type="password" placeholder="zostaw puste, jeśli bez zmian">
  <button id="btnSaveAuth">Zapisz</button>
  <div class="msg" id="authMsg"></div>
  <div class="hint">Domyślnie każdy w Twojej sieci domowej ma dostęp do panelu bez hasła - to zwykle wystarczające zabezpieczenie w prywatnej sieci. Włącz to pole, jeśli chcesz dodatkową ochronę, np. w sieci współdzielonej z innymi osobami. Wymagane jest ustawienie hasła - bez niego przełącznik się nie włączy.</div>
  <button class="secondary" id="btnLogout" style="display:none">Wyloguj się teraz</button>
</div>

<script>
function applyTheme(t){
  document.documentElement.setAttribute('data-theme', t);
  themeToggle.textContent = t === 'dark' ? String.fromCodePoint(0x2600) : String.fromCodePoint(0x1F319);
  themeToggle.setAttribute('aria-label', t === 'dark' ? 'Przełącz na jasny motyw' : 'Przełącz na ciemny motyw');
}
let savedTheme = localStorage.getItem('theme');
if(!savedTheme) savedTheme = matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
applyTheme(savedTheme);
themeToggle.addEventListener('click', ()=>{
  const next = document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
  localStorage.setItem('theme', next);
  applyTheme(next);
});

function aqiClass(v){
  if(v<=12) return 'aqi-good';
  if(v<=35) return 'aqi-mid';
  if(v<=55) return 'aqi-poor';
  if(v<=150) return 'aqi-bad';
  return 'aqi-verybad';
}
function showMsg(el, text, ok){
  el.textContent = text; el.className = 'msg ' + (ok?'ok':'err');
  clearTimeout(el._hideTimer);
  el._hideTimer = setTimeout(()=>{ el.className = 'msg'; }, 3000);
}
// Jak sesja wygasnie (albo ktos wylogowal sie w innej karcie), API zwroci
// 401 - zamiast psuc sie na parsowaniu JSON, po prostu wracamy do logowania.
async function afetch(url, opts){
  const r = await fetch(url, opts);
  if(r.status === 401){ location.reload(); throw new Error('unauthorized'); }
  return r;
}
async function refresh(){
  const r = await afetch('/api/data'); const d = await r.json();

  if(d.spsOk){
    pm25.textContent = d.pm25.toFixed(1)+' µg/m³';
    pm25.className = 'badge ' + aqiClass(d.pm25);
    pm1.textContent=d.pm1.toFixed(1); pm4.textContent=d.pm4.toFixed(1); pm10.textContent=d.pm10.toFixed(1);
  } else {
    pm25.textContent = '-'; pm25.className = 'badge';
    pm1.textContent='-'; pm4.textContent='-'; pm10.textContent='-';
  }

  if(d.bmeOk || d.shtOk){
    temp.textContent=d.temp.toFixed(1)+' °C'; hum.textContent=d.hum.toFixed(1)+' %';
  } else {
    temp.textContent='-'; hum.textContent='-';
  }
  press.textContent = d.bmeOk ? d.press.toFixed(0)+' hPa' : '-';

  const missing = [];
  if(!d.spsOk) missing.push('SPS30 (PM)');
  if(!d.bmeOk) missing.push('BME280');
  if(!d.shtOk) missing.push('SHT40');
  if(missing.length){
    sensorWarn.style.display = 'block';
    sensorWarn.textContent = '⚠ Nie wykryto: ' + missing.join(', ') + ' - sprawdź podłączenie na I2C.';
  } else {
    sensorWarn.style.display = 'none';
  }
}
async function refreshWifi(){
  const r = await afetch('/api/wifi'); const w = await r.json();
  wifiSsid.textContent = w.ssid || '-';
  wifiRssi.textContent = w.rssi + ' dBm';
  wifiPill.textContent = w.ssid || 'brak połączenia';
}
async function refreshRelay(){
  const r = await afetch('/api/relay'); const rl = await r.json();
  relayAuto.checked = rl.auto;
}
async function refreshMqtt(){
  const r = await afetch('/api/mqtt'); const m = await r.json();
  if(!m.configured){ mqttStatus.textContent = 'nie skonfigurowano'; mqttStatus.style.color=''; }
  else if(m.connected){ mqttStatus.textContent = 'Połączono'; mqttStatus.style.color='#38a169'; }
  else { mqttStatus.textContent = m.stateText; mqttStatus.style.color='#e53e3e'; }
}
async function loadConfig(){
  const r = await afetch('/api/config'); const c = await r.json();
  relayIp.value=c.relayIp; pm25Threshold.value=c.pm25Threshold;
  pm25Hysteresis.value=c.pm25Hysteresis; mqttBroker.value=c.mqttBroker; mqttUser.value=c.mqttUser;
  authEnabled.checked=c.authEnabled; authUser.value=c.authUser;
  btnLogout.style.display = c.authEnabled ? 'block' : 'none';
}
btnSaveRelay.addEventListener('click', async ()=>{
  await afetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({relayIp: relayIp.value, pm25Threshold: parseFloat(pm25Threshold.value), pm25Hysteresis: parseFloat(pm25Hysteresis.value)})});
  showMsg(relayMsg, 'Zapisano', true);
});
btnSaveMqtt.addEventListener('click', async ()=>{
  await afetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({mqttBroker: mqttBroker.value, mqttUser: mqttUser.value, mqttPassword: mqttPass.value})});
  mqttPass.value = '';
  showMsg(mqttMsg, 'Zapisano, próbuję połączyć...', true);
  setTimeout(refreshMqtt, 2500);
});
btnSaveAuth.addEventListener('click', async ()=>{
  const r = await afetch('/api/auth', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({enabled: authEnabled.checked, user: authUser.value, password: authPass.value})});
  authPass.value = '';
  if(r.status === 400){
    authEnabled.checked = false;
    showMsg(authMsg, 'Ustaw najpierw hasło, zanim włączysz ochronę', false);
    return;
  }
  btnLogout.style.display = authEnabled.checked ? 'block' : 'none';
  showMsg(authMsg, 'Zapisano', true);
});
btnLogout.addEventListener('click', async ()=>{
  await fetch('/logout', {method:'POST'});
  location.reload();
});
relayAuto.addEventListener('change', async ()=>{
  await afetch('/api/relay', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({auto: relayAuto.checked})});
});
btnOn.addEventListener('click', async ()=>{
  const r = await afetch('/api/relay', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({auto:false, on:true})});
  const rl = await r.json();
  relayAuto.checked = rl.auto;
});
btnOff.addEventListener('click', async ()=>{
  const r = await afetch('/api/relay', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({auto:false, on:false})});
  const rl = await r.json();
  relayAuto.checked = rl.auto;
});
btnWifiConnect.addEventListener('click', async ()=>{
  if(!newSsid.value){ showMsg(wifiMsg, 'Podaj nazwę sieci', false); return; }
  await afetch('/api/wifi/connect', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({ssid:newSsid.value, password:newPass.value})});
  showMsg(wifiMsg, 'Łączenie... urządzenie zaraz się zrestartuje. Sprawdź nowy adres IP na routerze.', true);
});
btnWifiForget.addEventListener('click', async ()=>{
  if(!confirm('Na pewno zapomnieć sieć WiFi? Urządzenie uruchomi ponownie portal konfiguracyjny (AP).')) return;
  await afetch('/api/wifi/forget', {method:'POST'});
  showMsg(wifiMsg, 'Restart... połącz się z siecią AirQuality-Setup, żeby skonfigurować WiFi od nowa.', true);
});
refresh(); refreshWifi(); refreshRelay(); refreshMqtt(); loadConfig();
setInterval(refresh, 4000);
setInterval(refreshRelay, 6000);
setInterval(refreshMqtt, 5000);
</script>
</body></html>
)rawliteral";

// ---------- Sesja logowania (zamiast Basic Auth, ktorego przegladarka sama
// cache'uje bez mozliwosci kontroli) ----------
// Jeden aktywny token na raz - to urzadzenie osobiste, nie wieloosobowy
// serwer. Token zyje tylko w RAM: restart urzadzenia = trzeba zalogowac sie
// od nowa. Ciasteczko sesyjne (bez Max-Age) znika, gdy przegladarka sie
// zamknie. Przycisk "Wyloguj" w panelu czysci token recznie w kazdej chwili.
String sessionToken = "";

String generateSessionToken() {
  char buf[33];
  for (int i = 0; i < 4; i++) {
    snprintf(buf + i * 8, 9, "%08x", (unsigned int)esp_random());
  }
  return String(buf);
}

String getCookie(AsyncWebServerRequest *request, const char *name) {
  if (!request->hasHeader("Cookie")) return "";
  String cookies = request->header("Cookie");
  String key = String(name) + "=";
  int start = cookies.indexOf(key);
  if (start == -1) return "";
  start += key.length();
  int end = cookies.indexOf(';', start);
  if (end == -1) end = cookies.length();
  return cookies.substring(start, end);
}

bool hasValidSession(AsyncWebServerRequest *request) {
  if (!config.panelAuthEnabled) return true;
  if (sessionToken.length() == 0) return false;
  String cookie = getCookie(request, "session");
  return cookie.length() > 0 && cookie == sessionToken;
}

// Do uzycia w endpointach API (JSON) - przy braku sesji wysyla 401 i
// zwraca false, wiec wywolujacy handler ma po prostu zrobic "return".
bool requireAuth(AsyncWebServerRequest *request) {
  if (hasValidSession(request)) return true;
  request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Logowanie - czujnik jakości powietrza</title>
<style>
body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0b0f14;color:#eef1f5;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.box{background:#141a23;border:1px solid #212832;border-radius:14px;padding:32px;width:280px}
h1{font-size:17px;margin:0 0 20px;font-weight:600}
input{width:100%;padding:10px;margin-bottom:12px;border:1px solid #2a2d33;border-radius:8px;background:#1c222b;color:#eef1f5;box-sizing:border-box;font-size:14px}
button{width:100%;padding:11px;border:none;border-radius:8px;background:#4fd1c5;color:#04211d;font-weight:700;cursor:pointer;font-size:14px}
.err{background:#fed7d7;color:#822727;padding:8px 10px;border-radius:8px;font-size:13px;margin-bottom:12px;display:none}
</style></head><body>
<form class="box" method="POST" action="/login">
  <h1>Czujnik jakości powietrza</h1>
  <div class="err" id="err">Błędny login lub hasło</div>
  <input name="user" placeholder="Login" autocomplete="username">
  <input name="pass" type="password" placeholder="Hasło" autocomplete="current-password">
  <button type="submit">Zaloguj</button>
</form>
<script>
if(location.search.indexOf('err=1') !== -1) document.getElementById('err').style.display='block';
</script>
</body></html>
)rawliteral";

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response;
    if (hasValidSession(request)) {
      response = request->beginResponse_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
    } else {
      response = request->beginResponse_P(200, "text/html; charset=utf-8", LOGIN_HTML);
    }
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  server.on("/login", HTTP_POST, [](AsyncWebServerRequest *request) {
    String user = request->hasArg("user") ? request->arg("user") : "";
    String pass = request->hasArg("pass") ? request->arg("pass") : "";
    AsyncWebServerResponse *response = request->beginResponse(302);
    if (config.panelAuthEnabled && user == String(config.panelUser) && pass == String(config.panelPass)) {
      sessionToken = generateSessionToken();
      response->addHeader("Location", "/");
      // Brak Max-Age/Expires = ciasteczko sesyjne, znika gdy przegladarka sie zamknie.
      response->addHeader("Set-Cookie", "session=" + sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
      Serial.println("Logowanie: OK");
    } else {
      response->addHeader("Location", "/?err=1");
      Serial.println("Logowanie: bledny login lub haslo");
    }
    request->send(response);
  });

  server.on("/logout", HTTP_POST, [](AsyncWebServerRequest *request) {
    sessionToken = "";
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", "/");
    response->addHeader("Set-Cookie", "session=; Path=/; Max-Age=0");
    request->send(response);
  });

  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireAuth(request)) return;
    JsonDocument doc;
    doc["pm1"] = readings.pm1;
    doc["pm25"] = readings.pm25;
    doc["pm4"] = readings.pm4;
    doc["pm10"] = readings.pm10;
    doc["temp"] = bmeOk ? readings.tempBme : readings.tempSht;
    doc["hum"] = bmeOk ? readings.humBme : readings.humSht;
    doc["press"] = readings.pressure;
    doc["relay"] = readings.relayOn;
    doc["bmeOk"] = bmeOk;
    doc["shtOk"] = shtOk;
    doc["spsOk"] = spsOk;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireAuth(request)) return;
    JsonDocument doc;
    doc["relayIp"] = config.relayIp;
    doc["pm25Threshold"] = config.pm25Threshold;
    doc["pm25Hysteresis"] = config.pm25Hysteresis;
    doc["mqttBroker"] = config.mqttBroker;
    doc["mqttUser"] = config.mqttUser;
    // hasla celowo nie sa zwracane w GET - pola w panelu zostaja puste,
    // a zapis nadpisuje haslo tylko jesli cos w nie wpisano
    doc["authEnabled"] = config.panelAuthEnabled;
    doc["authUser"] = config.panelUser;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireAuth(request)) return;
    JsonDocument doc;
    doc["configured"] = strlen(config.mqttBroker) > 0;
    doc["connected"] = mqttClient.connected();
    doc["state"] = mqttClient.state();
    doc["stateText"] = mqttStateText(mqttClient.state());
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  AsyncCallbackJsonWebHandler *cfgHandler = new AsyncCallbackJsonWebHandler(
    "/api/config",
    [](AsyncWebServerRequest *request, JsonVariant &json) {
      if (!requireAuth(request)) return;
      JsonObject obj = json.as<JsonObject>();
      if (!obj["relayIp"].isNull())
        strlcpy(config.relayIp, normalizeHostOrIp(obj["relayIp"].as<const char*>()).c_str(), sizeof(config.relayIp));
      if (!obj["pm25Threshold"].isNull())
        config.pm25Threshold = obj["pm25Threshold"].as<float>();
      if (!obj["pm25Hysteresis"].isNull())
        config.pm25Hysteresis = obj["pm25Hysteresis"].as<float>();
      // haslo nadpisujemy tylko jesli w polu cos faktycznie wpisano (niepuste) -
      // dzieki temu nie trzeba go wpisywac ponownie przy kazdej zmianie innych ustawien
      bool mqttChanged = false;
      if (!obj["mqttBroker"].isNull()) {
        strlcpy(config.mqttBroker, obj["mqttBroker"].as<const char*>(), sizeof(config.mqttBroker));
        mqttChanged = true;
      }
      if (!obj["mqttUser"].isNull()) {
        strlcpy(config.mqttUser, obj["mqttUser"].as<const char*>(), sizeof(config.mqttUser));
        mqttChanged = true;
      }
      if (!obj["mqttPassword"].isNull() && strlen(obj["mqttPassword"].as<const char*>()) > 0) {
        strlcpy(config.mqttPass, obj["mqttPassword"].as<const char*>(), sizeof(config.mqttPass));
        mqttChanged = true;
      }
      saveConfig();
      // Rozlaczamy MQTT tylko gdy faktycznie zmienily sie dane logowania -
      // zapis progu PM czy adresu Shelly nie powinien tego ruszac (zbedne
      // rozlaczenie+ponowne laczenie spowalnialo kazdy zapis w panelu).
      if (mqttChanged) mqttClient.disconnect();
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
  server.addHandler(cfgHandler);

  // ---------- WiFi: status + zmiana sieci ----------
  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireAuth(request)) return;
    JsonDocument doc;
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // Zapomina zapisana siec i uruchamia ponownie tryb AP + captive portal.
  server.on("/api/wifi/forget", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!requireAuth(request)) return;
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    WiFi.disconnect(true, true);   // eraseap=true kasuje zapisana siec z pamieci ESP32
    pendingRestartAt = millis() + 1000;
  });

  // Laczy od razu z inna siecia (bez wchodzenia w tryb AP) - jesli sie nie uda,
  // WiFiManager przy nastepnym rozruchu i tak sam wpadnie w tryb AP.
  AsyncCallbackJsonWebHandler *wifiHandler = new AsyncCallbackJsonWebHandler(
    "/api/wifi/connect",
    [](AsyncWebServerRequest *request, JsonVariant &json) {
      if (!requireAuth(request)) return;
      JsonObject obj = json.as<JsonObject>();
      if (obj["ssid"].isNull() || strlen(obj["ssid"].as<const char*>()) == 0) {
        request->send(400, "application/json", "{\"error\":\"brak ssid\"}");
        return;
      }
      String ssid = obj["ssid"].as<const char*>();
      String pass = obj["password"].isNull() ? "" : obj["password"].as<const char*>();
      request->send(200, "application/json", "{\"status\":\"ok\"}");
      WiFi.begin(ssid.c_str(), pass.c_str());
      pendingRestartAt = millis() + 1500;
    });
  server.addHandler(wifiHandler);

  // ---------- Przekaznik: status, tryb auto/reczny, przelaczanie ----------
  server.on("/api/relay", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireAuth(request)) return;
    JsonDocument doc;
    doc["auto"] = config.relayAutoMode;
    doc["on"] = readings.relayOn;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  AsyncCallbackJsonWebHandler *relayHandler = new AsyncCallbackJsonWebHandler(
    "/api/relay",
    [](AsyncWebServerRequest *request, JsonVariant &json) {
      if (!requireAuth(request)) return;
      JsonObject obj = json.as<JsonObject>();
      if (!obj["auto"].isNull())
        config.relayAutoMode = obj["auto"].as<bool>();
      saveConfig();
      if (!config.relayAutoMode && !obj["on"].isNull())
        triggerRelay(obj["on"].as<bool>());
      JsonDocument doc;
      doc["auto"] = config.relayAutoMode;
      doc["on"] = readings.relayOn;
      String out;
      serializeJson(doc, out);
      request->send(200, "application/json", out);
    });
  server.addHandler(relayHandler);

  // ---------- Bezpieczenstwo panelu (opcjonalne haslo) ----------
  server.on("/api/auth", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!requireAuth(request)) return;
    JsonDocument doc;
    doc["enabled"] = config.panelAuthEnabled;
    doc["user"] = config.panelUser;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  AsyncCallbackJsonWebHandler *authHandler = new AsyncCallbackJsonWebHandler(
    "/api/auth",
    [](AsyncWebServerRequest *request, JsonVariant &json) {
      if (!requireAuth(request)) return;
      JsonObject obj = json.as<JsonObject>();
      bool wantEnabled = config.panelAuthEnabled;
      if (!obj["enabled"].isNull())
        wantEnabled = obj["enabled"].as<bool>();
      if (!obj["user"].isNull() && strlen(obj["user"].as<const char*>()) > 0)
        strlcpy(config.panelUser, obj["user"].as<const char*>(), sizeof(config.panelUser));
      if (!obj["password"].isNull() && strlen(obj["password"].as<const char*>()) > 0)
        strlcpy(config.panelPass, obj["password"].as<const char*>(), sizeof(config.panelPass));

      // Nie pozwalamy wlaczyc ochrony bez ustawionego hasla - to by dawalo
      // falszywe poczucie bezpieczenstwa.
      if (wantEnabled && strlen(config.panelPass) == 0) {
        request->send(400, "application/json", "{\"error\":\"brak_hasla\"}");
        return;
      }
      config.panelAuthEnabled = wantEnabled;
      if (!config.panelAuthEnabled) sessionToken = "";  // wylaczono ochrone - sesja juz niepotrzebna
      saveConfig();
      Serial.printf("Autoryzacja panelu zapisana jako: %s (uzytkownik: %s)\n",
        config.panelAuthEnabled ? "WLACZONA" : "wylaczona", config.panelUser);
      JsonDocument doc;
      doc["enabled"] = config.panelAuthEnabled;
      doc["user"] = config.panelUser;
      String out;
      serializeJson(doc, out);
      request->send(200, "application/json", out);
    });
  server.addHandler(authHandler);

  server.begin();
}

// ================== FIZYCZNY RESET (przycisk BOOT) ==================

void factoryReset() {
  Serial.println("RESET FABRYCZNY - kasuje siec WiFi i wszystkie ustawienia...");
  WiFi.disconnect(true, true);   // kasuje zapisana siec WiFi z pamieci ESP32
  prefs.begin("airq", false);
  prefs.clear();                 // kasuje adres Shelly, progi PM, dane MQTT itd.
  prefs.end();
  delay(500);
  ESP.restart();
}

// Lzejsza wersja - kasuje TYLKO ochrone hasłem panelu. WiFi, adres Shelly,
// progi PM i dane MQTT zostaja nietkniete. Zapisuje tylko klucze zwiazane
// z haslem bezposrednio (NIE przez pelny saveConfig()) - ta funkcja moze
// byc wywolana zanim loadConfig() wczyta reszte ustawien (np. przy starcie
// urzadzenia), wiec saveConfig() nadpisalby je pustymi wartosciami domyslnymi.
void resetPanelAuthOnly() {
  Serial.println("RESET HASLA PANELU - kasuje ochrone hasłem (reszta ustawien bez zmian)");
  config.panelAuthEnabled = false;
  strlcpy(config.panelUser, "admin", sizeof(config.panelUser));
  config.panelPass[0] = '\0';
  sessionToken = "";
  prefs.begin("airq", false);
  prefs.putBool("authEn", config.panelAuthEnabled);
  prefs.putString("authUser", config.panelUser);
  prefs.putString("authPass", config.panelPass);
  prefs.end();
}

// Sprawdzane raz na starcie, ZANIM proba polaczyc sie z WiFi - obsluguje
// przypadek, gdy urzadzenie w ogole nie moze wystartowac (np. zawieszony
// portal AP), bo to blokujace sprawdzenie idzie przed setupWifi().
void checkResetAtBoot() {
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_BUTTON_PIN) != LOW) return;

  Serial.println("Przycisk BOOT przytrzymany przy starcie - 3s: reset hasla panelu, 8s: pelny reset fabryczny...");
  unsigned long start = millis();
  while (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (millis() - start >= FACTORY_RESET_HOLD_MS) {
      factoryReset();  // konczy sie restartem, nie wracamy z tej petli
    }
    delay(50);
  }
  unsigned long held = millis() - start;
  if (held >= AUTH_RESET_HOLD_MS) {
    resetPanelAuthOnly();
    Serial.println("Haslo panelu zresetowane - kontynuuje normalny rozruch.");
  } else {
    Serial.println("Przycisk puszczony za wczesnie (przed 3s) - normalny rozruch, nic nie zmieniono.");
  }
}

// Sprawdzane w petli glownej - pozwala zresetowac urzadzenie w dowolnym
// momencie pracy, nawet jesli panel WWW jest niedostepny.
unsigned long resetButtonPressedAt = 0;
bool resetStage1MessagePrinted = false;
bool resetStage2MessagePrinted = false;

void checkResetButton() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (resetButtonPressedAt == 0) {
      resetButtonPressedAt = millis();
      resetStage1MessagePrinted = false;
      resetStage2MessagePrinted = false;
    } else {
      unsigned long held = millis() - resetButtonPressedAt;
      if (!resetStage1MessagePrinted && held >= 500) {
        Serial.println("Przycisk BOOT trzymany - puszczenie za 3-8s zresetuje TYLKO haslo panelu...");
        resetStage1MessagePrinted = true;
      }
      if (!resetStage2MessagePrinted && held >= AUTH_RESET_HOLD_MS) {
        Serial.println("Mozna juz puscic (reset hasla) albo trzymac dalej do 8s (pelny reset fabryczny)...");
        resetStage2MessagePrinted = true;
      }
      if (held >= FACTORY_RESET_HOLD_MS) {
        factoryReset();
      }
    }
  } else {
    if (resetButtonPressedAt != 0) {
      unsigned long held = millis() - resetButtonPressedAt;
      if (held >= AUTH_RESET_HOLD_MS && held < FACTORY_RESET_HOLD_MS) {
        resetPanelAuthOnly();
      }
    }
    resetButtonPressedAt = 0;
    resetStage1MessagePrinted = false;
    resetStage2MessagePrinted = false;
  }
}

// ================== MONITOROWANIE WIFI W TRAKCIE PRACY ==================
// setupWifi() obsluguje tylko polaczenie przy starcie. To tutaj pilnuje,
// zeby urzadzenie samo proboawlo wrocic do sieci, jesli polaczenie padnie
// w trakcie normalnej pracy (np. restart routera).

void checkWifiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDisconnectedSince != 0) {
      Serial.println("WiFi: polaczenie przywrocone");
      wifiDisconnectedSince = 0;
    }
    return;
  }

  unsigned long now = millis();

  if (wifiDisconnectedSince == 0) {
    wifiDisconnectedSince = now;
    Serial.println("WiFi: polaczenie utracone, probuje wrocic...");
  }

  if (now - lastWifiReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWifiReconnectAttempt = now;
    Serial.println("WiFi: probuje ponownie polaczyc (WiFi.reconnect())");
    WiFi.reconnect();
  }

  if (now - wifiDisconnectedSince >= WIFI_RESTART_AFTER_MS) {
    Serial.println("WiFi: brak polaczenia od 5 minut - restart urzadzenia");
    delay(200);
    ESP.restart();
  }
}

// ================== WIFIMANAGER (AP + CAPTIVE PORTAL) ==================

void setupOTA();  // definicja ponizej - deklaracja zapowiadajaca, bo wywolywana z setupWifi()

void setupWifi() {
  // Ustawione PRZED polaczeniem - dzieki temu router w swojej liscie
  // podlaczonych urzadzen tez pokaze te nazwe zamiast anonimowego "ESP32".
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(MDNS_HOSTNAME);

  WiFiManager wm;

  WiFiManagerParameter p_relayIp("relayIp", "IP przekaznika Shelly", config.relayIp, 32);
  WiFiManagerParameter p_pm25("pm25", "Prog PM2.5, powyzej ktorego przekaznik sie wylacza (ug/m3)", String(config.pm25Threshold).c_str(), 8);
  WiFiManagerParameter p_mqtt("mqtt", "Broker MQTT (opcjonalnie)", config.mqttBroker, 64);

  wm.addParameter(&p_relayIp);
  wm.addParameter(&p_pm25);
  wm.addParameter(&p_mqtt);

  wm.setConfigPortalTimeout(300);

  String apName = "AirQuality-Setup-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool connected = wm.autoConnect(apName.c_str());

  if (!connected) {
    Serial.println("Nie polaczono z WiFi - restart");
    delay(3000);
    ESP.restart();
  }

  strlcpy(config.relayIp, normalizeHostOrIp(p_relayIp.getValue()).c_str(), sizeof(config.relayIp));
  config.pm25Threshold = atof(p_pm25.getValue());
  strlcpy(config.mqttBroker, p_mqtt.getValue(), sizeof(config.mqttBroker));
  saveConfig();

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Panel dostepny takze pod: http://" MDNS_HOSTNAME ".local/");
  } else {
    Serial.println("Nie udalo sie uruchomic mDNS - dostep tylko po adresie IP");
  }

  setupOTA();
}

// Aktualizacja firmware przez WiFi (bez kabla USB). Wymaga hasla (OTA_PASSWORD)
// i dziala z: pio run -e xiao_esp32s3_ota -t upload (albo devkit_v1_ota).
void setupOTA() {
  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA: rozpoczynam aktualizacje firmware...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA: gotowe, restart...");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r\n", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA: blad [%u]\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("OTA gotowe - aktualizacja przez WiFi mozliwa");
}

// ================== SETUP / LOOP ==================

void setup() {
  Serial.begin(115200);
  delay(3000);  // DEBUG: wydluzone tymczasowo, zeby zdazyc podlaczyc Serial Monitor na natywnym USB (S3)

  checkResetAtBoot();

  loadConfig();
  Serial.printf("Autoryzacja panelu: %s\n", config.panelAuthEnabled ? "WLACZONA" : "wylaczona");
  deviceId = "airq-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  // Domyslny limit PubSubClient to 256 bajtow na wiadomosc - za malo na pelne
  // payloady discovery dla Home Assistant (~350-400 bajtow z blokiem "device").
  // Bez tego publish() dla discovery po cichu zwraca false i nic nie dociera,
  // mimo ze samo polaczenie MQTT dziala poprawnie.
  mqttClient.setBufferSize(512);

  setupSensors();
  setupWifi();
  setupWebServer();

  Serial.print("Panel dostepny pod: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  unsigned long now = millis();

  checkResetButton();
  checkWifiConnection();
  ArduinoOTA.handle();

  if (pendingRestartAt && now >= pendingRestartAt) {
    ESP.restart();
  }

  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;
    readSensors();
    checkThreshold();
  }

  if (now - lastRelayStatusCheck >= RELAY_STATUS_INTERVAL_MS) {
    lastRelayStatusCheck = now;
    fetchRelayStatus();
  }

  if (strlen(config.mqttBroker) > 0) {
    if (!mqttClient.connected()) {
      mqttReconnect();
    } else {
      mqttClient.loop();
      if (now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL_MS) {
        lastMqttPublish = now;
        publishState();
      }
    }
  }
}