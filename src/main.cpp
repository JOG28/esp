#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <HardwareSerial.h>
#include <WebServer.h>
#include <WiFi.h>

// ---------------------------------------------------------
// PINES DE HARDWARE
// ---------------------------------------------------------
// PZEM: PZEM-TX va a Pin5 (RX del ESP32), PZEM-RX va a Pin6 (TX del ESP32)
#define PZEM_RX_PIN 5 // ESP32 recibe datos del PZEM TX
#define PZEM_TX_PIN 6 // ESP32 envía datos al PZEM RX
#define RELAY_PIN 4

// ---------------------------------------------------------
// PZEM - MODBUS RTU (Raw, sin librería)
// ---------------------------------------------------------
#define PZEM_SERIAL Serial2
#define PZEM_ADDR 0x01 // Dirección confirmada del módulo
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
bool wifiConnected = false;
bool relayState = false;
bool pzemReady = false;

// Variables de los datos leídos del PZEM
float g_voltage = 0.0;
float g_current = 0.0;
float g_power = 0.0;
float g_energy = 0.0;
float g_frequency = 0.0;
float g_pf = 0.0;

// Variables para provisión de Wi-Fi vía BLE
String wifi_ssid = "";
String wifi_password = "";
bool newCredentialsReceived = false;

// Variables del modelo de detección de anomalías
String current_device_type = "Ninguno";
float standby_power_max = 0.0;
float active_power_min = 0.0;
float active_power_max = 0.0;
bool anomaly_detected = false;

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
// MODBUS RTU - Leer PZEM (10 registros = todos los datos)
// ---------------------------------------------------------
bool readPZEM() {
  uint8_t cmd[8];
  cmd[0] = PZEM_ADDR;
  cmd[1] = 0x04; // Read Input Registers
  cmd[2] = 0x00; // Start Hi
  cmd[3] = 0x00; // Start Lo
  cmd[4] = 0x00; // Num regs Hi
  cmd[5] = 0x0A; // Num regs Lo (10 registros)

  uint16_t crc = modbusCRC16(cmd, 6);
  cmd[6] = crc & 0xFF;
  cmd[7] = (crc >> 8) & 0xFF;

  // Limpiar buffer de entrada
  while (PZEM_SERIAL.available())
    PZEM_SERIAL.read();

  // Enviar comando
  PZEM_SERIAL.write(cmd, 8);

  // Esperar respuesta
  uint8_t response[RESPONSE_SIZE];
  unsigned long startTime = millis();
  int index = 0;
  while ((millis() - startTime) < READ_TIMEOUT && index < RESPONSE_SIZE) {
    if (PZEM_SERIAL.available()) {
      response[index++] = PZEM_SERIAL.read();
    }
  }

  if (index != RESPONSE_SIZE)
    return false;

  // Verificar CRC
  uint16_t respCRC = modbusCRC16(response, RESPONSE_SIZE - 2);
  uint16_t receivedCRC =
      (response[RESPONSE_SIZE - 1] << 8) | response[RESPONSE_SIZE - 2];
  if (respCRC != receivedCRC)
    return false;
  if (response[0] != PZEM_ADDR || response[1] != 0x04)
    return false;

  // Parsear y guardar en variables globales
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
// CALLBACKS BLE (Recibir credenciales Wi-Fi desde la App)
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
        Serial.println("BLE: Credenciales Wi-Fi extraídas correctamente.");
      } else {
        Serial.println("BLE: Error al parsear JSON.");
      }
    }
  }
};

void initBLE() {
  BLEDevice::init("ESP32_EnergyMonitor");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pChar = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pChar->setCallbacks(new MyCallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE: Anunciando como 'ESP32_EnergyMonitor'. Esperando "
                 "credenciales Wi-Fi...");
}

// ---------------------------------------------------------
// HANDLERS DEL SERVIDOR WEB
// ---------------------------------------------------------
void handleGetStatus() {
  // Leer datos frescos del PZEM
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
      digitalWrite(RELAY_PIN, HIGH);
    } else if (state == "OFF" || state == "off") {
      relayState = false;
      digitalWrite(RELAY_PIN, LOW);
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
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Body not received");
    return;
  }

  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  if (doc.containsKey("device_type"))
    current_device_type = doc["device_type"].as<String>();
  if (doc.containsKey("standby_power_max"))
    standby_power_max = doc["standby_power_max"].as<float>();
  if (doc.containsKey("active_power_min"))
    active_power_min = doc["active_power_min"].as<float>();
  if (doc.containsKey("active_power_max"))
    active_power_max = doc["active_power_max"].as<float>();

  server.send(200, "application/json",
              "{\"status\":\"success\",\"message\":\"Perfil actualizado a " +
                  current_device_type + "\"}");
}

// ---------------------------------------------------------
// SETUP
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Iniciar puerto serie del PZEM
  PZEM_SERIAL.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);

  // Configurar Relé
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  relayState = false;

  Serial.println("\n========================================");
  Serial.println("  Monitor de Energía ESP32-S3 v2.0");
  Serial.println("========================================");
  Serial.printf("  PZEM: Serial2 | RX=Pin%d | TX=Pin%d | Addr=0x%02X\n",
                PZEM_RX_PIN, PZEM_TX_PIN, PZEM_ADDR);

  // Verificar comunicación con el PZEM
  delay(1500);
  Serial.println("  Verificando PZEM-004T v4.0...");
  if (readPZEM()) {
    pzemReady = true;
    Serial.printf("  ¡PZEM OK! Voltaje de red: %.1f V\n", g_voltage);
  } else {
    pzemReady = false;
    Serial.println("  ¡ADVERTENCIA! No se pudo comunicar con el PZEM.");
    Serial.println("  Verifica: 1) Cables RX/TX bien conectados");
    Serial.println("            2) VCC del PZEM en el pin de 5V");
    Serial.println("            3) 110V conectados al PZEM");
  }
  Serial.println("========================================\n");

  // Iniciar BLE para recibir credenciales Wi-Fi
  initBLE();
}

// ---------------------------------------------------------
// LOOP
// ---------------------------------------------------------
void loop() {
  // 1. Si llegaron credenciales Wi-Fi por BLE, conectar
  if (newCredentialsReceived && !wifiConnected) {
    Serial.println("Wi-Fi: Conectando a '" + wifi_ssid + "'...");
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      newCredentialsReceived = false;
      Serial.println("Wi-Fi: ¡Conectado!");
      Serial.print("Wi-Fi: IP local: ");
      Serial.println(WiFi.localIP());

      // Apagar BLE para liberar memoria
      BLEDevice::deinit(true);

      // Registrar rutas del servidor HTTP
      server.on("/api/status", HTTP_GET, handleGetStatus);
      server.on("/api/relay", HTTP_POST, handlePostRelay);
      server.on("/api/update_profile", HTTP_POST, handleUpdateProfile);
      server.begin();
      Serial.println("Wi-Fi: Servidor HTTP iniciado.");
    } else {
      Serial.println("Wi-Fi: Error al conectar. Reintenta enviando las "
                     "credenciales por BLE.");
      newCredentialsReceived = false;
    }
  }

  // 2. Atender clientes HTTP
  if (wifiConnected) {
    server.handleClient();
  }

  // 3. Leer PZEM y detectar anomalías cada 5 segundos
  static unsigned long lastRead = 0;
  if (millis() - lastRead > 5000) {
    lastRead = millis();

    if (readPZEM()) {
      pzemReady = true;

      // Detección de anomalías según el perfil del dispositivo
      if (current_device_type != "Ninguno") {
        if (!relayState) {
          // Modo Reposo (Relé apagado)
          anomaly_detected = (g_power > standby_power_max);
        } else {
          // Modo Activo (Relé encendido)
          anomaly_detected =
              (g_power < active_power_min || g_power > active_power_max);
        }
      }

      Serial.printf("[PZEM] %.1fV | %.3fA | %.1fW | %.3fkWh | %.1fHz | PF:%.2f "
                    "| Rele:%s | Anomalia:%s\n",
                    g_voltage, g_current, g_power, g_energy, g_frequency, g_pf,
                    relayState ? "ON" : "OFF", anomaly_detected ? "SI" : "NO");
    } else {
      pzemReady = false;
      Serial.println("[PZEM] Error de lectura. Verificar conexiones.");
    }
  }
}
