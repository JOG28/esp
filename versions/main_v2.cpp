#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <Preferences.h>

// ---------------------------------------------------------
// IDENTIFICADOR UNICO DEL DISPOSITIVO
// Cambia este ID por uno único por cada dispositivo físico
// Este mismo ID va codificado en el QR impreso en la carcasa
// ---------------------------------------------------------
#define DEVICE_ID "ESP32-MONITOR-001"

// ---------------------------------------------------------
// PINES DE HARDWARE
// PZEM: PZEM-TX -> Pin5 (RX del ESP32), PZEM-RX -> Pin6 (TX del ESP32)
// ---------------------------------------------------------
#define PZEM_RX_PIN   5
#define PZEM_TX_PIN   6
#define RELAY_PIN     39

// ---------------------------------------------------------
// PZEM - MODBUS RTU
// ---------------------------------------------------------
#define PZEM_SERIAL   Serial2
#define PZEM_ADDR     0x01
#define RESPONSE_SIZE 25
#define READ_TIMEOUT  400

// ---------------------------------------------------------
// MQTT - BROKER EN LA NUBE (HiveMQ Cloud - Free Tier)
// Regístrate en: https://console.hivemq.cloud
// Sustituye estas 3 líneas con tus credenciales del broker
// ---------------------------------------------------------
#define MQTT_BROKER   "TU_BROKER.s2.eu.hivemq.cloud"
#define MQTT_PORT     8883
#define MQTT_USER     "TU_USUARIO"
#define MQTT_PASS     "TU_PASSWORD"

// Topics MQTT (el Device ID se añade automáticamente)
// Publicación de datos:  "monitor/ESP32-MONITOR-001/data"
// Control del relé:      "monitor/ESP32-MONITOR-001/relay"
// Respuesta del relé:    "monitor/ESP32-MONITOR-001/relay/status"

// ---------------------------------------------------------
// CONSTANTES BLE (deben coincidir con la app)
// ---------------------------------------------------------
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ---------------------------------------------------------
// VARIABLES GLOBALES
// ---------------------------------------------------------
Preferences prefs;              // Para guardar WiFi en memoria flash
WiFiClient  wifiClient;
PubSubClient mqtt(wifiClient);

bool wifiConnected            = false;
bool mqttConnected            = false;
bool relayState               = false;
bool newCredentialsReceived   = false;
String wifi_ssid              = "";
String wifi_password          = "";

// Datos del PZEM
float g_voltage   = 0.0;
float g_current   = 0.0;
float g_power     = 0.0;
float g_energy    = 0.0;
float g_frequency = 0.0;
float g_pf        = 0.0;

// Detección de anomalías
String current_device_type = "Ninguno";
float  standby_power_max   = 0.0;
float  active_power_min    = 0.0;
float  active_power_max    = 0.0;
bool   anomaly_detected    = false;

// Topics MQTT dinámicos
String topicData;
String topicRelay;
String topicRelayStatus;

// ---------------------------------------------------------
// MODBUS RTU - CRC16
// ---------------------------------------------------------
uint16_t modbusCRC16(const uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
      else              crc >>= 1;
    }
  }
  return crc;
}

// ---------------------------------------------------------
// MODBUS RTU - Leer todos los registros del PZEM
// ---------------------------------------------------------
bool readPZEM() {
  uint8_t cmd[8];
  cmd[0] = PZEM_ADDR; cmd[1] = 0x04;
  cmd[2] = 0x00;      cmd[3] = 0x00;
  cmd[4] = 0x00;      cmd[5] = 0x0A;
  uint16_t crc = modbusCRC16(cmd, 6);
  cmd[6] = crc & 0xFF; cmd[7] = (crc >> 8) & 0xFF;

  while (PZEM_SERIAL.available()) PZEM_SERIAL.read();
  PZEM_SERIAL.write(cmd, 8);

  uint8_t response[RESPONSE_SIZE];
  unsigned long startTime = millis();
  int index = 0;
  while ((millis() - startTime) < READ_TIMEOUT && index < RESPONSE_SIZE) {
    if (PZEM_SERIAL.available()) response[index++] = PZEM_SERIAL.read();
  }

  if (index != RESPONSE_SIZE) return false;

  uint16_t respCRC     = modbusCRC16(response, RESPONSE_SIZE - 2);
  uint16_t receivedCRC = (response[RESPONSE_SIZE - 1] << 8) | response[RESPONSE_SIZE - 2];
  if (respCRC != receivedCRC)                           return false;
  if (response[0] != PZEM_ADDR || response[1] != 0x04) return false;

  g_voltage   = ((response[3]  << 8) | response[4])  / 10.0;
  g_current   = (((uint32_t)(response[7]  << 8) | response[8])  << 16 |
                  ((uint32_t)(response[5]  << 8) | response[6]))  / 1000.0;
  g_power     = (((uint32_t)(response[11] << 8) | response[12]) << 16 |
                  ((uint32_t)(response[9]  << 8) | response[10])) / 10.0;
  g_energy    = (((uint32_t)(response[15] << 8) | response[16]) << 16 |
                  ((uint32_t)(response[13] << 8) | response[14])) / 1000.0;
  g_frequency = ((response[17] << 8) | response[18]) / 10.0;
  g_pf        = ((response[19] << 8) | response[20]) / 100.0;
  return true;
}

// ---------------------------------------------------------
// MQTT - Callback cuando llega un mensaje de la app
// ---------------------------------------------------------
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String topicStr(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] Mensaje recibido en '%s': %s\n", topic, msg.c_str());

  // Control del relé: topic "monitor/DEVICE_ID/relay"
  if (topicStr == topicRelay) {
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, msg)) {
      String state = doc["state"].as<String>();
      if (state == "ON" || state == "on") {
        relayState = true;
        digitalWrite(RELAY_PIN, HIGH);
      } else if (state == "OFF" || state == "off") {
        relayState = false;
        digitalWrite(RELAY_PIN, LOW);
      }
      // Confirmar el nuevo estado a la app
      String statusMsg = "{\"relay_state\":\"" + String(relayState ? "ON" : "OFF") + "\"}";
      mqtt.publish(topicRelayStatus.c_str(), statusMsg.c_str(), true);
      Serial.printf("[RELE] Estado cambiado a: %s\n", relayState ? "ON" : "OFF");
    }
  }
}

// ---------------------------------------------------------
// MQTT - Conectar/Reconectar al broker
// ---------------------------------------------------------
void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("[MQTT] Conectando al broker...");
    String clientId = String(DEVICE_ID);
    if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println(" ¡Conectado!");
      // Suscribirse al topic de control del relé
      mqtt.subscribe(topicRelay.c_str());
      Serial.printf("[MQTT] Suscrito a '%s'\n", topicRelay.c_str());
      mqttConnected = true;
    } else {
      Serial.printf(" Error rc=%d. Reintentando en 5s...\n", mqtt.state());
      delay(5000);
    }
  }
}

// ---------------------------------------------------------
// BLE - Callback al recibir credenciales WiFi de la app
// ---------------------------------------------------------
class BLEProvisionCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    if (rxValue.length() > 0) {
      Serial.println("[BLE] Datos recibidos: " + rxValue);
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, rxValue)) {
        wifi_ssid             = doc["ssid"].as<String>();
        wifi_password         = doc["pass"].as<String>();
        newCredentialsReceived = true;

        // Guardar en memoria flash para reconexión automática al reiniciar
        prefs.begin("wifi", false);
        prefs.putString("ssid", wifi_ssid);
        prefs.putString("pass", wifi_password);
        prefs.end();
        Serial.println("[BLE] Credenciales guardadas en flash.");
      }
    }
  }
};

void initBLE() {
  BLEDevice::init(DEVICE_ID);
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(BLE_SERVICE_UUID);
  BLECharacteristic *pChar = pService->createCharacteristic(
    BLE_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  pChar->setCallbacks(new BLEProvisionCallback());
  pService->start();
  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] Anunciando como '%s'. Escanea el QR con la app.\n", DEVICE_ID);
}

// ---------------------------------------------------------
// CONECTAR WIFI
// ---------------------------------------------------------
bool connectWiFi(String ssid, String pass) {
  Serial.printf("[WiFi] Conectando a '%s'...", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 24) {
    delay(500); Serial.print("."); attempts++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] ¡Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[WiFi] Error al conectar.");
  return false;
}

// ---------------------------------------------------------
// SETUP
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  PZEM_SERIAL.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // Construir topics MQTT con el Device ID
  topicData        = "monitor/" + String(DEVICE_ID) + "/data";
  topicRelay       = "monitor/" + String(DEVICE_ID) + "/relay";
  topicRelayStatus = "monitor/" + String(DEVICE_ID) + "/relay/status";

  Serial.println("\n========================================");
  Serial.println("  Monitor de Energía v2.0 (MQTT + QR)");
  Serial.println("  Device ID: " + String(DEVICE_ID));
  Serial.println("========================================");

  // Intentar cargar WiFi guardado en flash (de configuraciones anteriores)
  prefs.begin("wifi", true);
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();

  if (savedSSID.length() > 0) {
    Serial.println("[Flash] Credenciales WiFi guardadas encontradas. Conectando...");
    wifiConnected = connectWiFi(savedSSID, savedPass);
  }

  if (!wifiConnected) {
    // No hay WiFi guardado: iniciar BLE para que la app provea las credenciales via QR
    Serial.println("[Flash] No hay credenciales guardadas. Iniciando BLE...");
    initBLE();
  } else {
    // Ya tiene WiFi: conectar al broker MQTT directamente
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    connectMQTT();
  }

  // Verificar PZEM
  delay(1500);
  if (readPZEM()) {
    Serial.printf("[PZEM] OK. Voltaje de red: %.1f V\n", g_voltage);
  } else {
    Serial.println("[PZEM] Advertencia: No se pudo leer. Verificar conexiones.");
  }
}

// ---------------------------------------------------------
// LOOP
// ---------------------------------------------------------
void loop() {
  // 1. Si llegaron credenciales nuevas por BLE, conectar WiFi
  if (newCredentialsReceived && !wifiConnected) {
    newCredentialsReceived = false;
    wifiConnected = connectWiFi(wifi_ssid, wifi_password);
    if (wifiConnected) {
      BLEDevice::deinit(true);  // Apagar BLE, ya no se necesita
      mqtt.setServer(MQTT_BROKER, MQTT_PORT);
      mqtt.setCallback(mqttCallback);
      connectMQTT();
    }
  }

  // 2. Mantener conexión MQTT activa
  if (wifiConnected) {
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();
  }

  // 3. Leer PZEM y publicar datos cada 5 segundos
  static unsigned long lastRead = 0;
  if (millis() - lastRead > 5000) {
    lastRead = millis();

    if (readPZEM()) {
      // Detección de anomalías
      if (current_device_type != "Ninguno") {
        anomaly_detected = relayState
          ? (g_power < active_power_min || g_power > active_power_max)
          : (g_power > standby_power_max);
      }

      Serial.printf("[PZEM] %.1fV | %.3fA | %.1fW | %.3fkWh | %.1fHz | Rele:%s | Anomalia:%s\n",
                    g_voltage, g_current, g_power, g_energy, g_frequency,
                    relayState ? "ON" : "OFF", anomaly_detected ? "SI" : "NO");

      // Publicar datos JSON al broker MQTT
      if (wifiConnected && mqtt.connected()) {
        StaticJsonDocument<256> doc;
        doc["device_id"]       = DEVICE_ID;
        doc["voltage"]         = g_voltage;
        doc["current"]         = g_current;
        doc["power"]           = g_power;
        doc["energy"]          = g_energy;
        doc["frequency"]       = g_frequency;
        doc["power_factor"]    = g_pf;
        doc["relay_state"]     = relayState ? "ON" : "OFF";
        doc["device_type"]     = current_device_type;
        doc["anomaly_detected"] = anomaly_detected;

        char buf[256];
        serializeJson(doc, buf);
        mqtt.publish(topicData.c_str(), buf, true);
        Serial.println("[MQTT] Datos publicados.");
      }
    } else {
      Serial.println("[PZEM] Error de lectura.");
    }
  }
}
