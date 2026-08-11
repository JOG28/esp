#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

// ---------------------------------------------------------
// PINES DE HARDWARE
// ---------------------------------------------------------
#define PZEM_RX_PIN 17
#define PZEM_TX_PIN 7
#define RELAY_PIN 5
#define BOOT_BTN_PIN 0 // Botón BOOT del ESP32

// ---------------------------------------------------------
// BACKEND — URL para sincronizar datos (Modelo PUSH)
// ---------------------------------------------------------
#define BACKEND_SYNC_URL "http://78.12.10.160/dispositivos/sync"

// ---------------------------------------------------------
// PZEM - MODBUS RTU
// ---------------------------------------------------------
#define PZEM_SERIAL Serial2
#define PZEM_ADDR 0xF8 // Dirección de broadcast del PZEM
#define RESPONSE_SIZE 25
#define READ_TIMEOUT 400

// ---------------------------------------------------------
// CONSTANTES BLE
// ---------------------------------------------------------
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// ---------------------------------------------------------
// VARIABLES GLOBALES
// ---------------------------------------------------------
WebServer server(80);
Preferences prefs;

bool wifiConnected = false;
bool relayState = false;
bool pzemReady = false;

float g_voltage = 0.0;
float g_current = 0.0;
float g_power = 0.0;
float g_energy = 0.0;
float g_frequency = 0.0;
float g_pf = 0.0;

String wifi_ssid = "";
String wifi_password = "";
bool newCredentialsReceived = false;

String current_device_type = "Ninguno";
float standby_power_max = 0.0;
float active_power_min = 0.0;
float active_power_max = 0.0;
bool anomaly_detected = false;

BLECharacteristic *g_pChar = nullptr;

// ---------------------------------------------------------
// Prototipos
// ---------------------------------------------------------
void handleGetStatus();
void handlePostRelay();
void handleUpdateProfile();
void handleUpdateWiFi();
void connectWiFiAndSetup(const String &ssid, const String &password);
void syncWithBackend();

// ---------------------------------------------------------
// MODBUS RTU - CRC16
// ---------------------------------------------------------
uint16_t modbusCRC16(const uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 0x0001)
        crc = (crc >> 1) ^ 0xA001;
      else
        crc >>= 1;
    }
  }
  return crc;
}

// ---------------------------------------------------------
// MODBUS RTU - Leer PZEM
// ---------------------------------------------------------
bool readPZEM() {
  uint8_t cmd[8];
  cmd[0] = PZEM_ADDR;
  cmd[1] = 0x04;
  cmd[2] = 0x00;
  cmd[3] = 0x00;
  cmd[4] = 0x00;
  cmd[5] = 0x0A;

  uint16_t crc = modbusCRC16(cmd, 6);
  cmd[6] = crc & 0xFF;
  cmd[7] = (crc >> 8) & 0xFF;

  while (PZEM_SERIAL.available())
    PZEM_SERIAL.read();
  PZEM_SERIAL.write(cmd, 8);

  uint8_t response[RESPONSE_SIZE];
  unsigned long startTime = millis();
  int index = 0;
  while ((millis() - startTime) < READ_TIMEOUT && index < RESPONSE_SIZE) {
    if (PZEM_SERIAL.available())
      response[index++] = PZEM_SERIAL.read();
  }

  if (index != RESPONSE_SIZE)
    return false;

  uint16_t respCRC = modbusCRC16(response, RESPONSE_SIZE - 2);
  uint16_t receivedCRC =
      (response[RESPONSE_SIZE - 1] << 8) | response[RESPONSE_SIZE - 2];
  if (respCRC != receivedCRC)
    return false;
  if (response[1] != 0x04)
    return false; // Solo comprobamos que el comando sea 0x04

  g_voltage = ((response[3] << 8) | response[4]) / 10.0;
  g_current = (((uint32_t)(response[7] << 8) | response[8]) << 16 |
               ((uint32_t)(response[5] << 8) | response[6])) /
              1000.0;
  g_power = (((uint32_t)(response[11] << 8) | response[12]) << 16 |
             ((uint32_t)(response[9] << 8) | response[10])) /
            10.0;
  g_energy = (((uint32_t)(response[15] << 8) | response[16]) << 16 |
              ((uint32_t)(response[13] << 8) | response[14])) /
             1000.0;
  g_frequency = ((response[17] << 8) | response[18]) / 10.0;
  g_pf = ((response[19] << 8) | response[20]) / 100.0;

  return true;
}

// ---------------------------------------------------------
// SINCRONIZAR CON AWS (Modelo PUSH)
// ---------------------------------------------------------
void syncWithBackend() {
  String mac = WiFi.macAddress();
  String ip = WiFi.localIP().toString();

  StaticJsonDocument<512> doc;
  doc["mac_address"] = mac;
  doc["ip_local"] = ip; // Para compatibilidad
  doc["voltage"] = g_voltage;
  doc["current"] = g_current;
  doc["power"] = g_power;
  doc["energy"] = g_energy;
  doc["frequency"] = g_frequency;
  doc["power_factor"] = g_pf;
  doc["relay_state"] = relayState ? "ON" : "OFF";
  doc["anomaly_detected"] = anomaly_detected;

  String body;
  serializeJson(doc, body);

  Serial.println("[Sync] Enviando JSON al servidor:");
  Serial.println(body);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, BACKEND_SYNC_URL);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(body);
  if (httpCode > 0) {
    String payload = http.getString();
    Serial.printf("[Sync] Respuesta del servidor (%d): %s\n", httpCode,
                  payload.c_str());
    // Parsear respuesta para ver si AWS nos mandó apagar o encender el Relay
    StaticJsonDocument<256> respDoc;
    DeserializationError error = deserializeJson(respDoc, payload);
    if (!error) {
      if (respDoc.containsKey("relay_command") &&
          !respDoc["relay_command"].isNull()) {
        String cmd = respDoc["relay_command"].as<String>();
        if (cmd == "ON" && !relayState) {
          relayState = true;
          digitalWrite(RELAY_PIN, LOW); // Lógica LOW: encendido
          Serial.println("[Sync] Comando recibido de AWS: Rele ON");
        } else if (cmd == "OFF" && relayState) {
          relayState = false;
          digitalWrite(RELAY_PIN, HIGH); // Lógica LOW: apagado
          Serial.println("[Sync] Comando recibido de AWS: Rele OFF");
        }
      }
    }
  } else {
    Serial.printf("[Sync] Error HTTP de conexion con AWS: %d\n", httpCode);
  }
  http.end();
}

// ---------------------------------------------------------
// CALLBACKS BLE
// ---------------------------------------------------------
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    if (rxValue.length() > 0) {
      Serial.println("BLE: Datos recibidos: " + rxValue);
      StaticJsonDocument<256> doc;
      DeserializationError error = deserializeJson(doc, rxValue);
      if (!error) {
        wifi_ssid = doc["ssid"].as<String>();
        wifi_password = doc["pass"].as<String>();
        newCredentialsReceived = true;
        Serial.println("BLE: Credenciales extraídas correctamente.");
      } else {
        Serial.println("BLE: Error al parsear JSON.");
      }
    }
  }
};

// Callback del SERVIDOR BLE para reiniciar advertising cuando el celular se
// desconecta
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    Serial.println("BLE: Cliente conectado.");
  }

  void onDisconnect(BLEServer *pServer) {
    Serial.println("BLE: Cliente desconectado. Reiniciando advertising...");
    // CRUCIAL: Si el celular se desconecta sin enviar credenciales,
    // volver a anunciar para que lo pueda encontrar de nuevo
    delay(500);
    BLEDevice::startAdvertising();
    Serial.println("BLE: Anunciando de nuevo como 'ESP32_EnergyMonitor'...");
  }
};

void initBLE() {
  BLEDevice::init("ESP32_EnergyMonitor");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(
      new MyServerCallbacks()); // <-- Reiniciar advertising al desconectar
  BLEService *pService = pServer->createService(SERVICE_UUID);

  g_pChar = pService->createCharacteristic(
      CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_READ |
                               BLECharacteristic::PROPERTY_WRITE |
                               BLECharacteristic::PROPERTY_NOTIFY);
  g_pChar->addDescriptor(new BLE2902());
  g_pChar->setCallbacks(new MyCallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE: Anunciando como 'ESP32_EnergyMonitor'...");
}

// ---------------------------------------------------------
// CONECTAR WIFI Y COMPLETAR SETUP
// ---------------------------------------------------------
void connectWiFiAndSetup(const String &ssid, const String &password) {
  Serial.println("Wi-Fi: Conectando a '" + ssid + "'...");
  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
        "Wi-Fi: Error al conectar. Borrando credenciales y reiniciando...");
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    delay(1000);
    ESP.restart();
    return;
  }

  wifiConnected = true;
  Serial.println("Wi-Fi: Conectado!");
  Serial.print("Wi-Fi: IP local: ");
  Serial.println(WiFi.localIP());
  Serial.println("Wi-Fi: MAC: " + WiFi.macAddress());

  // Guardar credenciales en NVS para arranques futuros
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.end();
  Serial.println("NVS: Credenciales guardadas.");

  // Forzamos la primera sincronizacion con AWS para "registrar" el dispositivo
  syncWithBackend();

  // Notificar MAC al app por BLE y cerrar BLE
  if (g_pChar != nullptr) {
    String mac = WiFi.macAddress();
    Serial.println("BLE: Notificando MAC: " + mac);
    g_pChar->setValue(mac.c_str());
    g_pChar->notify();
    delay(800);
    BLEDevice::deinit(true);
    g_pChar = nullptr;
    Serial.println("BLE: Apagado.");
  }

  // Mantener los endpoints locales por si acaso, aunque ya no se usarán desde
  // AWS
  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/relay", HTTP_POST, handlePostRelay);
  server.on("/api/update_profile", HTTP_POST, handleUpdateProfile);
  server.on("/api/wifi", HTTP_POST, handleUpdateWiFi);
  server.begin();
  Serial.println("HTTP: Servidor local de respaldo iniciado");
}

// ---------------------------------------------------------
// HANDLERS DEL SERVIDOR WEB (Respaldo Local)
// ---------------------------------------------------------
void handleUpdateWiFi() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Body not received");
    return;
  }
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  if (!doc.containsKey("ssid") || !doc.containsKey("pass")) {
    server.send(400, "text/plain", "Missing 'ssid' or 'pass'");
    return;
  }
  String newSsid = doc["ssid"].as<String>();
  String newPass = doc["pass"].as<String>();
  prefs.begin("wifi", false);
  prefs.putString("ssid", newSsid);
  prefs.putString("pass", newPass);
  prefs.end();
  server.send(200, "application/json",
              "{\"status\":\"success\",\"message\":\"Wi-Fi actualizado. "
              "Reiniciando...\"}");
  delay(1000);
  ESP.restart();
}

void handleGetStatus() {
  bool ok = readPZEM();
  if (!ok) {
    g_voltage = 0.0;
    g_current = 0.0;
    g_power = 0.0;
    g_energy = 0.0;
  }
  StaticJsonDocument<256> doc;
  doc["mac_address"] = WiFi.macAddress();
  doc["voltage"] = g_voltage;
  doc["current"] = g_current;
  doc["power"] = g_power;
  doc["energy"] = g_energy;
  doc["frequency"] = g_frequency;
  doc["power_factor"] = g_pf;
  doc["relay_state"] = relayState ? "ON" : "OFF";
  doc["device_type"] = current_device_type;
  doc["anomaly_detected"] = anomaly_detected;
  String response;
  serializeJson(doc, response);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", response);
}

void handlePostRelay() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Body not received");
    return;
  }
  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  if (doc.containsKey("state")) {
    String state = doc["state"].as<String>();
    if (state == "ON" || state == "on") {
      relayState = true;
      digitalWrite(RELAY_PIN, LOW);
    } else if (state == "OFF" || state == "off") {
      relayState = false;
      digitalWrite(RELAY_PIN, HIGH);
    }
    server.send(200, "application/json",
                "{\"status\":\"success\",\"relay_state\":\"" +
                    String(relayState ? "ON" : "OFF") + "\"}");
  } else {
    server.send(400, "text/plain", "Missing 'state' in JSON");
  }
}

void handleUpdateProfile() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("plain"))
    return;
  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, server.arg("plain")))
    return;
  if (doc.containsKey("device_type"))
    current_device_type = doc["device_type"].as<String>();
  if (doc.containsKey("standby_power_max"))
    standby_power_max = doc["standby_power_max"].as<float>();
  if (doc.containsKey("active_power_min"))
    active_power_min = doc["active_power_min"].as<float>();
  if (doc.containsKey("active_power_max"))
    active_power_max = doc["active_power_max"].as<float>();
  server.send(200, "application/json", "{\"status\":\"success\"}");
}

// ---------------------------------------------------------
// SETUP
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  PZEM_SERIAL.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);

  // Boton BOOT para borrar memoria y forzar Bluetooth
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  // RELAY - Inicia APAGADO (lógica Invertida: HIGH es OFF)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  relayState = false;

  Serial.println("\n========================================");
  Serial.println("  Monitor de Energía ESP32-S3 v4.0 (AWS PUSH)");
  Serial.println("========================================");

  // Checkear si el boton de BOOT esta presionado al arrancar para borrar WiFi
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    Serial.println("\n!!! BOTON BOOT PRESIONADO !!!");
    Serial.println("Borrando memoria WiFi guardada...");
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    Serial.println("Memoria borrada. Reiniciando...\n");
    delay(2000);
    ESP.restart();
  }

  WiFi.mode(WIFI_STA);
  Serial.println("  Tu MAC Address es: " + WiFi.macAddress());
  Serial.println("  Manten presionado el boton BOOT al encender si quieres "
                 "borrar la WiFi.");
  Serial.println("========================================");

  delay(1500);
  if (readPZEM()) {
    pzemReady = true;
    Serial.printf("  PZEM OK! Voltaje: %.1f V\n", g_voltage);
  } else {
    Serial.println("  ADVERTENCIA: No se pudo comunicar con el PZEM.");
  }
  Serial.println("========================================\n");

  // Intentar cargar credenciales WiFi guardadas en NVS
  prefs.begin("wifi", true);
  String saved_ssid = prefs.getString("ssid", "");
  String saved_pass = prefs.getString("pass", "");
  prefs.end();

  if (saved_ssid.length() > 0) {
    // Credenciales guardadas — conectar sin BLE
    Serial.println("NVS: Credenciales encontradas. Conectando sin BLE...");
    connectWiFiAndSetup(saved_ssid, saved_pass);
  } else {
    // Primera vez — iniciar BLE
    Serial.println("NVS: Sin credenciales. Iniciando BLE para vinculacion...");
    initBLE();
  }
}

// ---------------------------------------------------------
// LOOP
// ---------------------------------------------------------
void loop() {
  // 1. Si llegaron credenciales por BLE, conectar
  if (newCredentialsReceived && !wifiConnected) {
    newCredentialsReceived = false;
    connectWiFiAndSetup(wifi_ssid, wifi_password);
  }

  // 2. Atender clientes HTTP (local fallback)
  if (wifiConnected) {
    server.handleClient();
  }

  // 3. Leer PZEM y Sincronizar con AWS cada 5 segundos
  static unsigned long lastRead = 0;
  if (millis() - lastRead > 5000) {
    lastRead = millis();

    if (readPZEM()) {
      pzemReady = true;

      if (current_device_type != "Ninguno") {
        if (!relayState) {
          anomaly_detected = (g_power > standby_power_max);
        } else {
          anomaly_detected =
              (g_power < active_power_min || g_power > active_power_max);
        }
      }

      Serial.printf("[PZEM] %.1fV | %.3fA | %.1fW | %.3fkWh | PF:%.2f | "
                    "Rele:%s | Anomalia:%s\n",
                    g_voltage, g_current, g_power, g_energy, g_pf,
                    relayState ? "ON" : "OFF", anomaly_detected ? "SI" : "NO");
    } else {
      pzemReady = false;
      // No imprimir cada 5 seg para no saturar el serial
    }

    // SIEMPRE sincronizar con AWS, tenga PZEM o no
    // Así el dashboard muestra el dispositivo en línea y el relay funciona
    if (wifiConnected) {
      syncWithBackend();
    }
  }
}
