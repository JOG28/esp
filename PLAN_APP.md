# Plan de Implementación de la App Móvil
## Monitor de Energía ESP32-S3 - Versión 2.0
### (Provisión por QR + MQTT en la Nube)

---

## Resumen del Flujo de Usuario

```
PRIMERA VEZ:
[Usuario abre la app]
      │
      ▼
[Registro: ingresa su WiFi de casa (SSID + contraseña)]
      │  La app guarda estos datos de forma segura (Keychain/Keystore)
      ▼
[Pantalla "Agregar Dispositivo"]
      │
      ▼
[Usuario escanea el QR impreso en la carcasa del ESP32]
      │  El QR contiene: {"device_id":"ESP32-MONITOR-001", "ble_uuid":"4fafc201-..."}
      ▼
[La app conecta automáticamente al ESP32 por BLE]
      │  Sin diálogo de emparejamiento (BLE no requiere pairing manual)
      ▼
[La app manda el WiFi al ESP32 automáticamente]
      │  Formato JSON: {"ssid":"MiRed","pass":"MiContraseña"}
      ▼
[ESP32 se conecta al WiFi de la casa y al broker MQTT]
      │
      ▼
[La app muestra: "¡Dispositivo configurado!" y redirige al Dashboard]

USO DIARIO (desde cualquier lugar del mundo):
[App] ←──── MQTT Broker (HiveMQ Cloud) ────► [ESP32 en casa]
```

---

## Stack Tecnológico Recomendado para la App

| Componente | Tecnología |
|---|---|
| Framework | **Ionic + Angular** (genera APK y .ipa desde un solo código) |
| BLE | `@capacitor-community/bluetooth-le` |
| QR Scanner | `@capacitor-community/barcode-scanner` |
| MQTT | `mqtt` (npm package, funciona en WebSocket) |
| Almacenamiento seguro | `@capacitor/preferences` + `capacitor-secure-storage-plugin` |
| Notificaciones push | Firebase Cloud Messaging (FCM) |

---

## Módulo 1: Registro y Guardado de Credenciales WiFi

### Pantalla: `ConfigurarWifiPage`

**Qué debe hacer:**
1. Mostrar un formulario con dos campos: Nombre de red (SSID) y Contraseña.
2. Guardar los datos de forma segura usando `SecureStorage` (NO en localStorage).
3. Mostrar botón "Siguiente: Agregar Dispositivo".

**Código de referencia (Ionic/Angular):**
```typescript
import { SecureStoragePlugin } from 'capacitor-secure-storage-plugin';

async guardarWifi(ssid: string, password: string) {
  await SecureStoragePlugin.set({ key: 'wifi_ssid', value: ssid });
  await SecureStoragePlugin.set({ key: 'wifi_pass', value: password });
}
```

---

## Módulo 2: Scanner de QR

### Pantalla: `EscanearDispositivoPage`

**Qué debe hacer:**
1. Abrir la cámara y escanear el QR del ESP32.
2. El QR contiene este JSON: `{"device_id":"ESP32-MONITOR-001","ble_uuid":"4fafc201-1fb5-459e-8fcc-c5c9c331914b","char_uuid":"beb5483e-36e1-4688-b7f5-ea07361b26a8"}`
3. Parsear el JSON y extraer el `device_id`, `ble_uuid` y `char_uuid`.
4. Pasar automáticamente al Módulo 3 (provisión BLE).

**Código de referencia:**
```typescript
import { BarcodeScanner } from '@capacitor-community/barcode-scanner';

async escanearQR() {
  await BarcodeScanner.checkPermission({ force: true });
  const result = await BarcodeScanner.startScan();
  if (result.hasContent) {
    const datos = JSON.parse(result.content);
    // datos.device_id  → "ESP32-MONITOR-001"
    // datos.ble_uuid   → UUID del servicio BLE
    // datos.char_uuid  → UUID de la característica BLE
    await this.provisionarPorBLE(datos);
  }
}
```

---

## Módulo 3: Provisión WiFi por BLE (Automática)

### Servicio: `BleProvisionService`

**Qué debe hacer:**
1. Solicitar permisos de Bluetooth (una sola vez).
2. Escanear dispositivos BLE buscando el `ble_uuid` del ESP32.
3. Conectarse automáticamente (sin diálogo de pairing).
4. Leer las credenciales WiFi guardadas en el Módulo 1.
5. Escribir el JSON `{"ssid":"...","pass":"..."}` en la característica BLE del ESP32.
6. Desconectarse.
7. Mostrar animación de "Configurando dispositivo..." y esperar confirmación.

**Código de referencia:**
```typescript
import { BleClient } from '@capacitor-community/bluetooth-le';

async provisionarPorBLE(datos: {device_id: string, ble_uuid: string, char_uuid: string}) {
  await BleClient.initialize();

  // Escanear y conectar al ESP32
  const device = await BleClient.requestDevice({
    services: [datos.ble_uuid]
  });
  await BleClient.connect(device.deviceId);

  // Leer WiFi guardado
  const ssid = await SecureStoragePlugin.get({ key: 'wifi_ssid' });
  const pass = await SecureStoragePlugin.get({ key: 'wifi_pass' });

  // Enviar credenciales al ESP32
  const payload = JSON.stringify({ ssid: ssid.value, pass: pass.value });
  const encoder = new TextEncoder();
  await BleClient.write(
    device.deviceId,
    datos.ble_uuid,
    datos.char_uuid,
    encoder.encode(payload)
  );

  // Desconectar BLE
  await BleClient.disconnect(device.deviceId);

  // Guardar device_id para usarlo con MQTT
  await SecureStoragePlugin.set({ key: 'device_id', value: datos.device_id });

  // Mostrar éxito y navegar al Dashboard
  this.router.navigate(['/dashboard']);
}
```

**Notas importantes:**
- En **Android**, necesitas el permiso `BLUETOOTH_SCAN` y `BLUETOOTH_CONNECT` en `AndroidManifest.xml`.
- En **iOS**, necesitas `NSBluetoothAlwaysUsageDescription` en `Info.plist`.
- El ESP32 **no requiere pairing tradicional**, la conexión es directa.

---

## Módulo 4: Dashboard con MQTT (Datos en Tiempo Real)

### Pantalla: `DashboardPage` + Servicio: `MqttService`

**Qué debe hacer:**
1. Conectarse al broker MQTT (HiveMQ Cloud) usando WebSocket.
2. Suscribirse al topic: `monitor/{device_id}/data`
3. Actualizar la UI en tiempo real con cada mensaje que llega.
4. Publicar al topic `monitor/{device_id}/relay` para controlar el relé.

**Configurar el broker MQTT (HiveMQ Cloud):**
1. Regístrate en https://console.hivemq.cloud (gratis).
2. Crea un cluster.
3. Crea un usuario y contraseña.
4. Anota el hostname del broker (ej: `abc123.s2.eu.hivemq.cloud`).
5. El mismo hostname, usuario y contraseña van en el `main_v2.cpp` del ESP32.

**Código de referencia:**
```typescript
import * as mqtt from 'mqtt';

@Injectable({ providedIn: 'root' })
export class MqttService {
  private client: mqtt.MqttClient;

  conectar(deviceId: string) {
    this.client = mqtt.connect('wss://TU_BROKER.s2.eu.hivemq.cloud:8884/mqtt', {
      username: 'TU_USUARIO',
      password: 'TU_PASSWORD',
      clientId: 'app-' + Math.random().toString(16).substr(2, 8)
    });

    this.client.on('connect', () => {
      console.log('MQTT conectado');
      this.client.subscribe(`monitor/${deviceId}/data`);
      this.client.subscribe(`monitor/${deviceId}/relay/status`);
    });

    this.client.on('message', (topic, payload) => {
      const datos = JSON.parse(payload.toString());
      // Actualizar variables del dashboard:
      // datos.voltage, datos.current, datos.power,
      // datos.energy, datos.frequency, datos.relay_state,
      // datos.anomaly_detected
    });
  }

  controlarRele(deviceId: string, estado: 'ON' | 'OFF') {
    const msg = JSON.stringify({ state: estado });
    this.client.publish(`monitor/${deviceId}/relay`, msg);
  }
}
```

---

## Módulo 5: Notificaciones de Anomalías (Opcional - Fase 2)

**Flujo:**
- El ESP32 detecta una anomalía y publica en MQTT con `"anomaly_detected": true`.
- Un servidor backend (puede ser Firebase Functions o un servidor Node.js) escucha ese topic MQTT.
- El servidor envía una notificación push via **Firebase Cloud Messaging (FCM)** al teléfono del usuario.
- El usuario recibe: *"⚠️ Anomalía detectada en tu dispositivo ESP32-MONITOR-001"*.

---

## QR que se imprime en la carcasa del dispositivo

El QR debe codificar este texto exacto (reemplaza el device_id si tienes múltiples dispositivos):

```json
{"device_id":"ESP32-MONITOR-001","ble_uuid":"4fafc201-1fb5-459e-8fcc-c5c9c331914b","char_uuid":"beb5483e-36e1-4688-b7f5-ea07361b26a8"}
```

Puedes generar el QR en: https://www.qr-code-generator.com

---

## Permisos necesarios en la App

### Android (`AndroidManifest.xml`)
```xml
<uses-permission android:name="android.permission.BLUETOOTH" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
<uses-permission android:name="android.permission.CAMERA" />
<uses-permission android:name="android.permission.INTERNET" />
```

### iOS (`Info.plist`)
```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Usamos Bluetooth para configurar el dispositivo de monitoreo.</string>
<key>NSCameraUsageDescription</key>
<string>Usamos la cámara para escanear el código QR del dispositivo.</string>
<key>NSLocalNetworkUsageDescription</key>
<string>Necesitamos acceso a la red para comunicarnos con el servidor.</string>
```

---

## Dependencias npm a instalar

```bash
npm install mqtt
npm install @capacitor-community/bluetooth-le
npm install @capacitor-community/barcode-scanner
npm install capacitor-secure-storage-plugin
npx cap sync
```

---

## Resumen de archivos del ESP32

| Archivo | Descripción |
|---|---|
| `src/main.cpp` | Versión 1: WiFi local + BLE + WebServer (funcional, sin MQTT) |
| `versions/main_v2.cpp` | Versión 2: MQTT en la nube + provisión por QR (requiere configurar broker) |

Para usar la Versión 2, copia el contenido de `versions/main_v2.cpp` a `src/main.cpp`
y rellena las 4 constantes del broker MQTT con tus credenciales de HiveMQ Cloud.
