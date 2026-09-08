#include <WiFi.h>
#include <WiFiServer.h>
#include <ESP32Time.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <vector>
#include <SPI.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

Preferences preferences;
ESP32Time rtc;
RTC_DS3231 ds3231;
WiFiServer server(80);

// MAX7219 - modulo de 8 digitos de 7 segmentos
// DIN = GPIO 23, CLK = GPIO 18, CS/LOAD = GPIO 5
const int MAX_DIN = 23;
const int MAX_CLK = 18;
const int MAX_CS  = 5;
// MAX7219 controlado directamente por SPI (compatible con ESP32)
// No se usa LedControl porque esa librería depende de avr/pgmspace.h.
void max7219Enviar(byte registro, byte dato) {
  digitalWrite(MAX_CS, LOW);
  SPI.transfer(registro);
  SPI.transfer(dato);
  digitalWrite(MAX_CS, HIGH);
}

void max7219Init() {
  pinMode(MAX_CS, OUTPUT);
  digitalWrite(MAX_CS, HIGH);
  SPI.begin(MAX_CLK, -1, MAX_DIN, MAX_CS);

  max7219Enviar(0x0F, 0x00); // display test OFF
  max7219Enviar(0x0C, 0x01); // shutdown OFF / normal operation
  max7219Enviar(0x0B, 0x07); // usar los 8 digitos del MAX7219
  max7219Enviar(0x09, 0x0F); // decodificacion BCD para los 8 digitos
  max7219Enviar(0x0A, 0x08); // intensidad media

  for (byte i = 1; i <= 8; i++) {
    max7219Enviar(i, 0x0F); // blank en todos los digitos
  }
}

void max7219ApagarDigito(byte posicion) {
  max7219Enviar(posicion + 1, 0x0F);
}

long gmtOffset_sec = -10800; // UTC-3 (Argentina)

struct Alarma {
  int id;
  int hora;
  int minuto;
  bool activa;
};

std::vector<Alarma> listaAlarmas;
int proximoId = 1;

// ============================================================
// GUARDADO PERMANENTE DE ALARMAS EN LA MEMORIA NVS DEL ESP32
// ============================================================
// Las alarmas se guardan en cada alta/baja y se recuperan al
// iniciar el ESP32. No se borran al resetear o apagar.

void guardarAlarmas() {
  Preferences alarmPrefs;

  if (!alarmPrefs.begin("alarmas", false)) {
    Serial.println("ERROR: No se pudo abrir NVS para guardar alarmas.");
    return;
  }

  const uint32_t cantidad = (uint32_t)listaAlarmas.size();
  alarmPrefs.putUInt("count", cantidad);
  alarmPrefs.putUInt("nextId", (uint32_t)proximoId);

  // Guardamos el vector completo como bytes. Esto evita depender de
  // muchas claves de texto y hace el guardado más fiable tras un reset.
  if (cantidad > 0) {
    size_t bytes = cantidad * sizeof(Alarma);
    size_t escritos = alarmPrefs.putBytes("data", listaAlarmas.data(), bytes);
    if (escritos != bytes) {
      Serial.printf("ERROR: solo se guardaron %u de %u bytes.\n",
                    (unsigned)escritos, (unsigned)bytes);
    }
  } else {
    alarmPrefs.remove("data");
  }

  alarmPrefs.end();
  Serial.printf("Alarmas guardadas en NVS: %u\n", (unsigned)cantidad);
}

void cargarAlarmas() {
  Preferences alarmPrefs;

  // Abrimos en modo lectura/escritura.
  // Si la particion/namespace "alarmas" todavia no existe (primer arranque),
  // Preferences puede fallar al abrirlo en modo solo lectura.
  // En modo false el namespace se crea automaticamente.
  if (!alarmPrefs.begin("alarmas", false)) {
    Serial.println("ERROR: No se pudo abrir/crear NVS para cargar alarmas.");
    return;
  }

  listaAlarmas.clear();
  uint32_t cantidad = alarmPrefs.getUInt("count", 0);
  proximoId = (int)alarmPrefs.getUInt("nextId", 1);

  if (cantidad > 50) cantidad = 50;

  size_t esperado = cantidad * sizeof(Alarma);
  size_t disponible = alarmPrefs.getBytesLength("data");

  if (cantidad > 0 && disponible >= esperado) {
    listaAlarmas.resize(cantidad);
    size_t leidos = alarmPrefs.getBytes("data", listaAlarmas.data(), esperado);
    if (leidos != esperado) {
      listaAlarmas.clear();
      Serial.println("ERROR: datos de alarmas incompletos. Se inicia sin alarmas.");
    }
  }

  alarmPrefs.end();

  int mayorId = 0;
  for (const auto &alarma : listaAlarmas) {
    if (alarma.id > mayorId) mayorId = alarma.id;
  }
  if (proximoId <= mayorId) proximoId = mayorId + 1;
  if (proximoId < 1) proximoId = 1;

  Serial.printf("Alarmas cargadas desde NVS: %u\n",
                (unsigned)listaAlarmas.size());
}

// ============================================================
// CRONOMETRO / TEMPORIZADOR
// ============================================================
// El tiempo se configura desde la pagina. En el display de 4 digitos
// se muestra MM:SS cuando queda menos de una hora y HH:MM cuando queda
// una hora o mas. En la pagina siempre se muestra HH:MM:SS.
uint32_t timerTotalSegundos = 0;
uint32_t timerRestantesSegundos = 0;
bool timerCorriendo = false;
unsigned long timerUltimoTick = 0;
bool modoCronometro = false;


// ============================================================
// ACCESO REMOTO POR INTERNET
// El ESP32 inicia las conexiones hacia el servidor, por lo que
// no hace falta abrir puertos del router.
// ============================================================
const char* REMOTE_SERVER_URL = "https://TU-APP.onrender.com/api/device";
const char* DEVICE_TOKEN = "CAMBIAR_DEVICE_TOKEN";
unsigned long ultimoPollRemoto = 0;
unsigned long ultimoStateRemoto = 0;

String campoComando(const String &linea, int numero) {
  int inicio = 0;
  for (int i = 0; i < numero; i++) {
    inicio = linea.indexOf('|', inicio);
    if (inicio < 0) return "";
    inicio++;
  }
  int fin = linea.indexOf('|', inicio);
  if (fin < 0) fin = linea.length();
  return linea.substring(inicio, fin);
}

void ejecutarComandoRemoto(const String &linea) {
  if (linea.length() == 0 || linea == "NO_COMMANDS") return;

  String tipo = campoComando(linea, 0);
  Serial.println("Comando remoto: " + linea);

  if (tipo == "timer_start") {
    int h = constrain(campoComando(linea, 1).toInt(), 0, 99);
    int m = constrain(campoComando(linea, 2).toInt(), 0, 59);
    int sec = constrain(campoComando(linea, 3).toInt(), 0, 59);
    uint32_t total = (uint32_t)h * 3600UL + (uint32_t)m * 60UL + (uint32_t)sec;
    if (total > 0) {
      timerTotalSegundos = total;
      timerRestantesSegundos = total;
      timerUltimoTick = millis();
      timerCorriendo = true;
      modoCronometro = true;
      Serial.printf("Cronometro remoto iniciado: %02d:%02d:%02d\n", h, m, sec);
    }
  }
  else if (tipo == "timer_pause") {
    actualizarTimer();
    timerRestantesSegundos = obtenerTimerRestante();
    timerCorriendo = false;
  }
  else if (tipo == "timer_reset") {
    timerRestantesSegundos = timerTotalSegundos;
    timerCorriendo = false;
    modoCronometro = true;
  }
  else if (tipo == "display_mode") {
    String modo = campoComando(linea, 1);
    if (modo == "timer") {
      modoCronometro = true;
    } else {
      modoCronometro = false;
      timerCorriendo = false;
    }
  }
  else if (tipo == "set_time") {
    unsigned long epoch = strtoul(campoComando(linea, 1).c_str(), nullptr, 10);
    unsigned long horaLocal = epoch + gmtOffset_sec;
    rtc.setTime(horaLocal);
    ds3231.adjust(DateTime(horaLocal));
    Serial.println("Hora ajustada remotamente.");
  }
  else if (tipo == "add_alarm") {
    String timeStr = campoComando(linea, 1);
    if (timeStr.length() >= 5) {
      Alarma nueva = {
        proximoId++,
        timeStr.substring(0, 2).toInt(),
        timeStr.substring(3, 5).toInt(),
        true
      };
      listaAlarmas.push_back(nueva);
      guardarAlarmas();
    }
  }
  else if (tipo == "del_alarm") {
    int idDel = campoComando(linea, 1).toInt();
    for (auto it = listaAlarmas.begin(); it != listaAlarmas.end(); ++it) {
      if (it->id == idDel) {
        listaAlarmas.erase(it);
        break;
      }
    }
    guardarAlarmas();
  }
}

void enviarEstadoRemoto() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure clientSecure;
  clientSecure.setInsecure();
  HTTPClient http;
  String url = String(REMOTE_SERVER_URL) + "/state";
  if (!http.begin(clientSecure, url)) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_TOKEN);

  actualizarTimer();
  uint32_t restante = obtenerTimerRestante();
  uint32_t hh = restante / 3600UL;
  uint32_t mm = (restante % 3600UL) / 60UL;
  uint32_t ss = restante % 60UL;
  char timerText[16];
  snprintf(timerText, sizeof(timerText), "%02lu:%02lu:%02lu",
           (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);

  DateTime ahora = ds3231.now();
  char horaText[16];
  snprintf(horaText, sizeof(horaText), "%02d:%02d:%02d",
           ahora.hour(), ahora.minute(), ahora.second());

  String json = "{\"hora\":\"" + String(horaText) +
                "\",\"timer\":{" +
                "\"horas\":" + String(hh) +
                ",\"minutos\":" + String(mm) +
                ",\"segundos\":" + String(ss) +
                ",\"corriendo\":" + String(timerCorriendo ? "true" : "false") +
                "},\"modo\":\"" + String(modoCronometro ? "timer" : "clock") +
                "\",\"display\":\"" + String(timerText) +
                "\",\"wifi\":\"" + WiFi.SSID() + "\",\"alarms\":[";

  for (size_t i = 0; i < listaAlarmas.size(); i++) {
    json += "{\"id\":" + String(listaAlarmas[i].id) +
            ",\"hora\":" + String(listaAlarmas[i].hora) +
            ",\"minuto\":" + String(listaAlarmas[i].minuto) + "}";
    if (i + 1 < listaAlarmas.size()) json += ",";
  }
  json += "]}";

  int code = http.POST(json);
  if (code < 0) Serial.printf("Error enviando estado remoto: %d\n", code);
  http.end();
}

void consultarServidorRemoto() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure clientSecure;
  clientSecure.setInsecure();
  HTTPClient http;
  String url = String(REMOTE_SERVER_URL) + "/poll";
  if (!http.begin(clientSecure, url)) return;
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String body = http.getString();
    int inicio = 0;
    while (inicio < body.length()) {
      int fin = body.indexOf('\n', inicio);
      if (fin < 0) fin = body.length();
      String linea = body.substring(inicio, fin);
      linea.trim();
      if (linea.length()) ejecutarComandoRemoto(linea);
      inicio = fin + 1;
    }
  }
  http.end();
}

void servicioRemoto() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long ahora = millis();
  if (ahora - ultimoPollRemoto >= 500) {
    ultimoPollRemoto = ahora;
    consultarServidorRemoto();
  }
  if (ahora - ultimoStateRemoto >= 1000) {
    ultimoStateRemoto = ahora;
    enviarEstadoRemoto();
  }
}

uint32_t obtenerTimerRestante() {
  if (!timerCorriendo) return timerRestantesSegundos;
  unsigned long transcurrido = (millis() - timerUltimoTick) / 1000UL;
  if (transcurrido >= timerRestantesSegundos) return 0;
  return timerRestantesSegundos - transcurrido;
}

void actualizarTimer() {
  if (!timerCorriendo) return;

  unsigned long ahora = millis();
  if (ahora - timerUltimoTick >= 1000UL) {
    unsigned long transcurrido = (ahora - timerUltimoTick) / 1000UL;
    timerUltimoTick += transcurrido * 1000UL;

    if (transcurrido >= timerRestantesSegundos) {
      timerRestantesSegundos = 0;
      timerCorriendo = false;
      Serial.println("Cronometro finalizado.");
    } else {
      timerRestantesSegundos -= transcurrido;
    }
  }
}

void mostrarNumero4(byte d3, byte d2, byte d1, byte d0, bool puntoD2) {
  max7219Enviar(0x01, d0);
  max7219Enviar(0x02, d1);
  max7219Enviar(0x03, d2 | (puntoD2 ? 0x80 : 0x00));
  max7219Enviar(0x04, d3);
  max7219ApagarDigito(4);
  max7219ApagarDigito(5);
  max7219ApagarDigito(6);
  max7219ApagarDigito(7);
}

void actualizarMAX7219() {
  actualizarTimer();

  if (modoCronometro) {
    uint32_t restante = obtenerTimerRestante();
    uint32_t horas = restante / 3600UL;
    uint32_t minutos = (restante % 3600UL) / 60UL;
    uint32_t segundos = restante % 60UL;

    if (horas > 0) {
      // HH.MM mientras queda una hora o mas.
      if (horas > 99) horas = 99;
      mostrarNumero4((byte)(horas / 10), (byte)(horas % 10),
                     (byte)(minutos / 10), (byte)(minutos % 10), true);
    } else {
      // MM.SS mientras queda menos de una hora.
      mostrarNumero4((byte)(minutos / 10), (byte)(minutos % 10),
                     (byte)(segundos / 10), (byte)(segundos % 10), true);
    }
    return;
  }

  DateTime ahora = ds3231.now();
  int h = ahora.hour();
  int m = ahora.minute();
  mostrarNumero4((byte)(h / 10), (byte)(h % 10),
                 (byte)(m / 10), (byte)(m % 10), true);
}

// Decodificación de caracteres especiales en URL
String urlDecode(String input) {
  String decoded = "";
  char c;
  for (unsigned int i = 0; i < input.length(); i++) {
    c = input.charAt(i);
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < input.length()) {
      int code;
      sscanf(input.substring(i + 1, i + 3).c_str(), "%x", &code);
      decoded += (char)code;
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

// Funciones para manejo de múltiples redes en memoria EEPROM/Preferences
int getNetworkCount() {
  preferences.begin("wifi-list", true);
  int count = preferences.getInt("count", 0);
  preferences.end();
  return count;
}

void saveNetwork(String s, String p) {
  int count = getNetworkCount();
  preferences.begin("wifi-list", false);
  preferences.putString(("ssid_" + String(count)).c_str(), s);
  preferences.putString(("pass_" + String(count)).c_str(), p);
  preferences.putInt("count", count + 1);
  preferences.end();
}

void removeNetwork(int index) {
  int count = getNetworkCount();
  if (index < 0 || index >= count) return;

  std::vector<std::pair<String, String>> temp;
  preferences.begin("wifi-list", true);

  for (int i = 0; i < count; i++) {
    if (i != index) {
      String s = preferences.getString(("ssid_" + String(i)).c_str(), "");
      String p = preferences.getString(("pass_" + String(i)).c_str(), "");
      if (s != "") temp.push_back({s, p});
    }
  }

  preferences.end();

  preferences.begin("wifi-list", false);
  preferences.clear();
  preferences.putInt("count", temp.size());

  for (size_t i = 0; i < temp.size(); i++) {
    preferences.putString(("ssid_" + String(i)).c_str(), temp[i].first);
    preferences.putString(("pass_" + String(i)).c_str(), temp[i].second);
  }

  preferences.end();
}

String getNetworkJSON() {
  int count = getNetworkCount();

  preferences.begin("wifi-list", true);

  String json = "[";

  for (int i = 0; i < count; i++) {
    String s = preferences.getString(("ssid_" + String(i)).c_str(), "");

    json += "{\"id\":" + String(i) + ",\"ssid\":\"" + s + "\"}";

    if (i < count - 1) json += ",";
  }

  json += "]";

  preferences.end();
  return json;
}

// Interfaz HTML principal
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 - Control de Reloj</title>

    <style>
        :root {
            --bg-color: #000000;
            --card-bg: #111111;
            --yellow-main: #facc15;
            --yellow-hover: #eab308;
            --text-color: #ffffff;
            --border-color: #222222;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }

        body {
            font-family: 'Segoe UI', system-ui, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            padding: 20px;
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        #loginScreen {
            width: 100%;
            max-width: 400px;
            background: var(--card-bg);
            padding: 30px;
            border-radius: 16px;
            border: 1px solid var(--border-color);
            margin-top: 50px;
        }

        .tab-buttons {
            display: flex;
            gap: 10px;
            margin-bottom: 20px;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 10px;
        }

        .tab-btn {
            flex: 1;
            padding: 10px;
            background: transparent;
            border: 1px solid var(--border-color);
            color: #aaa;
            border-radius: 8px;
            font-weight: bold;
            cursor: pointer;
            text-transform: none;
        }

        .tab-btn.active {
            background-color: var(--yellow-main);
            color: #000;
            border-color: var(--yellow-main);
        }

        .dashboard-container {
            width: 100%;
            max-width: 1200px;
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
            gap: 20px;
            margin-top: 20px;
        }

        .card {
            background-color: var(--card-bg);
            padding: 24px;
            border-radius: 16px;
            border: 1px solid var(--border-color);
            display: flex;
            flex-direction: column;
            justify-content: space-between;
        }

        h2 {
            color: var(--yellow-main);
            font-size: 1.1rem;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 10px;
            margin-bottom: 15px;
            text-transform: uppercase;
        }

        .clock-display {
            font-size: 3rem;
            font-weight: 800;
            text-align: center;
            color: var(--yellow-main);
            font-family: monospace;
            margin: 15px 0;
        }

        .form-group {
            display: flex;
            flex-direction: column;
            gap: 10px;
        }

        label {
            font-size: 0.85rem;
            color: #aaa;
        }

        input, select {
            padding: 12px;
            border-radius: 8px;
            border: 1px solid #333;
            background-color: #050505;
            color: #fff;
            width: 100%;
            color-scheme: dark;
        }

        button {
            padding: 12px;
            border: none;
            border-radius: 8px;
            background-color: var(--yellow-main);
            color: #000;
            font-weight: bold;
            cursor: pointer;
            text-transform: uppercase;
            margin-top: 5px;
        }

        button:hover {
            background-color: var(--yellow-hover);
        }

        .btn-outline {
            background: transparent;
            border: 1px solid var(--yellow-main);
            color: var(--yellow-main);
        }

        .btn-danger {
            background-color: #ef4444 !important;
            color: #ffffff !important;
        }

        .btn-danger:hover {
            background-color: #dc2626 !important;
        }

        .item-list {
            background: #080808;
            border: 1px solid #222;
            padding: 10px;
            border-radius: 8px;
            display: flex;
            justify-content: space-between;
            margin-top: 8px;
            align-items: center;
        }

        .btn-small {
            padding: 5px 10px;
            width: auto;
            font-size: 0.8rem;
        }

        .modal {
            display: flex;
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0, 0, 0, 0.85);
            justify-content: center;
            align-items: center;
            z-index: 1000;
            backdrop-filter: blur(4px);
        }

        .modal-card {
            background: var(--card-bg);
            padding: 25px;
            border-radius: 16px;
            border: 1px solid var(--yellow-main);
            max-width: 400px;
            width: 90%;
            text-align: center;
        }

        .modal-card h3 {
            color: var(--yellow-main);
            margin-bottom: 12px;
            font-size: 1.2rem;
        }

        .modal-card p {
            font-size: 0.95rem;
            color: #ccc;
            margin-bottom: 20px;
            line-height: 1.4;
        }

        .alarm-trigger-card {
            border: 2px solid var(--yellow-main);
            animation: pulse-border 1s infinite alternate;
        }

        @keyframes pulse-border {
            from {
                border-color: var(--yellow-main);
                box-shadow: 0 0 10px var(--yellow-main);
            }

            to {
                border-color: #ffffff;
                box-shadow: 0 0 25px var(--yellow-main);
            }
        }

        .hidden {
            display: none !important;
        }
    </style>
</head>

<body>

<!-- Login / Registro Screen -->
<div id="loginScreen">

    <div class="tab-buttons">

        <button
            id="tabLogin"
            class="tab-btn active"
            onclick="switchTab('login')"
        >
            Iniciar Sesión
        </button>

        <button
            id="tabRegister"
            class="tab-btn"
            onclick="switchTab('register')"
        >
            Crear Usuario
        </button>

    </div>

    <!-- Formulario Iniciar Sesión -->
    <div id="formLogin" class="form-group">

        <h2>🔑 Acceso Directo</h2>

        <label>Nombre de Usuario:</label>
        <input
            type="text"
            id="loginUser"
            placeholder="Tu usuario"
        >

        <label>Contraseña Personal:</label>
        <input
            type="password"
            id="loginPass"
            placeholder="Tu contraseña"
        >

        <button onclick="loginOnly()">Ingresar</button>

    </div>

    <!-- Formulario Crear Usuario -->
    <div id="formRegister" class="form-group hidden">

        <h2>📝 Registro Nuevo</h2>

        <label>Palabra Clave (Requerida):</label>

        <input
            type="password"
            id="tallerKeyInput"
            placeholder="Ej: taller"
        >

        <label>Nombre de Usuario Nuevo:</label>

        <input
            type="text"
            id="regUser"
            placeholder="Escribe un usuario"
        >

        <label>Contraseña Personal Nueva:</label>

        <input
            type="password"
            id="regPass"
            placeholder="Escribe tu contraseña"
        >

        <button onclick="registerOnly()">Crear Cuenta</button>

    </div>
</div>

<!-- Modal Zona Horaria -->
<div id="countryModal" class="modal hidden">

    <div class="modal-card">

        <h2>🌎 Zona Horaria</h2>

        <div class="form-group" style="margin-top:15px;">

            <select id="countrySelect">

                <option value="-10800">
                    🇦🇷 Argentina / Chile (UTC-3)
                </option>

                <option value="-18000">
                    🇨🇴 Colombia / Perú (UTC-5)
                </option>

                <option value="-21600">
                    🇲🇽 México CDMX (UTC-6)
                </option>

                <option value="3600">
                    🇪🇸 España (UTC+1)
                </option>

            </select>

            <button onclick="confirmCountry()">
                Acceder al Reloj
            </button>

        </div>
    </div>
</div>

<!-- Modal Interactivo / Notificaciones -->
<div id="customModal" class="modal hidden">

    <div class="modal-card">

        <h3 id="modalTitle">Notificación</h3>

        <p id="modalMessage">Mensaje de prueba</p>

        <div
            id="modalInputs"
            class="form-group hidden"
            style="margin-bottom: 15px;"
        >

            <label id="modalInputLabel">
                Contraseña personal requerida:
            </label>

            <input
                type="password"
                id="modalSecretInput"
                placeholder="Tu contraseña"
            >

        </div>

        <div style="display:flex; gap:10px; justify-content:center;">

            <button id="modalBtnConfirm">
                Aceptar
            </button>

            <button
                id="modalBtnCancel"
                class="btn-outline hidden"
                onclick="closeNotification()"
            >
                Cancelar
            </button>

        </div>
    </div>
</div>

<!-- Modal Alarma Sonando -->
<div id="alarmTriggerModal" class="modal hidden">

    <div class="modal-card alarm-trigger-card">

        <h3 style="font-size: 1.8rem;">
            ⏰ ¡ALARMA!
        </h3>

        <div
            class="clock-display"
            id="alarmTriggerTime"
        >
            --:--
        </div>

        <p>
            Es hora de la alarma programada.
        </p>

        <button
            onclick="dismissAlarmTrigger()"
            style="font-size: 1.1rem; padding: 15px;"
        >
            🔔 DESACTIVAR ALARMA
        </button>

    </div>
</div>

<!-- Dashboard -->
<div id="dashboard" class="dashboard-container hidden">

    <!-- Reloj -->
    <div class="card">

        <h2 id="mainModeTitle">⏱️ Hora del Reloj</h2>

        <div id="mainModeBadge" style="text-align:center; color:#aaa; font-size:.85rem; margin-top:-5px;">🕐 MODO RELOJ</div>

        <div
            class="clock-display"
            id="clock"
        >
            --:--:--
        </div>

        <button
            class="btn-outline"
            onclick="syncWithDevice()"
        >
            Sincronizar Celular
        </button>

    </div>

    <!-- Ajuste Manual -->
    <div class="card">

        <h2>⚙️ Ajustar Hora del Reloj</h2>

        <div class="form-group">

            <label>
                Selecciona Fecha y Hora exacta:
            </label>

            <input
                type="datetime-local"
                id="manualDateTime"
            >

            <button onclick="setManualTime()">
                Establecer Hora
            </button>

        </div>
    </div>

    <!-- Gestor de Redes Wi-Fi -->
    <div class="card">

        <h2>📶 Gestor de Redes Wi-Fi</h2>

        <!-- RED WI-FI ACTUAL - CARTEL PEQUEÑO -->
        <div
            style="
                background:#080808;
                border:1px solid #222;
                padding:10px;
                border-radius:8px;
                display:flex;
                justify-content:space-between;
                align-items:center;
                gap:10px;
                margin-bottom:15px;
            "
        >

            <span
                style="
                    color:#aaa;
                    font-size:0.85rem;
                "
            >
                📶 Conectado a:
            </span>

            <span
                id="currentWifi"
                style="
                    color:var(--yellow-main);
                    font-size:0.9rem;
                    font-weight:bold;
                    text-align:right;
                    word-break:break-word;
                "
            >
                Cargando...
            </span>

        </div>

        <div class="form-group">

            <label>
                Guardar Nueva Red Wi-Fi:
            </label>

            <input
                type="text"
                id="wifiSSID"
                placeholder="SSID (Nombre)"
            >

            <input
                type="password"
                id="wifiPASS"
                placeholder="Contraseña de la Red"
            >

            <button onclick="requestAddWifi()">
                Agregar Red
            </button>

        </div>

        <hr
            style="
                border-color: var(--border-color);
                margin: 15px 0;
            "
        >

        <label>
            Redes Guardadas:
        </label>

        <div
            id="wifiContainer"
            style="
                max-height: 150px;
                overflow-y: auto;
                margin-top: 5px;
            "
        ></div>

    </div>

    <!-- Cronometro -->
    <div class="card">

        <h2>⏱️ Cronómetro / Temporizador</h2>

        <div class="clock-display" id="timerDisplay">00:00:00</div>

        <div class="form-group">
            <label>Tiempo a configurar:</label>
            <div style="display:flex; gap:8px; flex-wrap:wrap;">
                <input type="number" id="timerHours" min="0" max="99" value="0" placeholder="Horas" style="flex:1; min-width:90px;">
                <input type="number" id="timerMinutes" min="0" max="59" value="5" placeholder="Minutos" style="flex:1; min-width:90px;">
                <input type="number" id="timerSeconds" min="0" max="59" value="0" placeholder="Segundos" style="flex:1; min-width:90px;">
            </div>
        </div>

        <div style="display:flex; gap:8px; flex-wrap:wrap; margin-top:10px;">
            <button onclick="timerStart()">▶️ Iniciar</button>
            <button onclick="timerPause()">⏸️ Pausar</button>
            <button onclick="timerReset()">🔄 Reiniciar</button>
        </div>

        <div style="display:flex; gap:8px; flex-wrap:wrap; margin-top:12px;">
            <button class="btn-outline" onclick="setDisplayMode('clock')">🕐 Modo Reloj</button>
            <button class="btn-outline" onclick="setDisplayMode('timer')">⏱️ Modo Cronómetro</button>
        </div>

        <p style="color:#aaa; margin-top:10px; font-size:.85rem;">
            El reloj físico mostrará el modo elegido. En cronómetro: MM:SS; si queda una hora o más, HH:MM.
        </p>
    </div>

    <!-- Alarmas -->
    <div class="card">

        <h2>⏰ Programar Alarmas</h2>

        <div class="form-group">

            <input
                type="time"
                id="alarmInput"
            >

            <button onclick="addAlarm()">
                Agregar Alarma
            </button>

        </div>

        <div
            id="alarmContainer"
            style="
                max-height: 150px;
                overflow-y: auto;
                margin-top: 10px;
            "
        ></div>

    </div>

</div>

<script>

    let authHeader = "";
    let userPassword = "";
    let confirmCallback = null;
    let activeAlarms = [];
    let lastTriggeredMinute = "";
    let timerPoll = null;

    function switchTab(tab) {

        if(tab === 'login') {

            document.getElementById('tabLogin')
                .classList.add('active');

            document.getElementById('tabRegister')
                .classList.remove('active');

            document.getElementById('formLogin')
                .classList.remove('hidden');

            document.getElementById('formRegister')
                .classList.add('hidden');

        } else {

            document.getElementById('tabRegister')
                .classList.add('active');

            document.getElementById('tabLogin')
                .classList.remove('active');

            document.getElementById('formRegister')
                .classList.remove('hidden');

            document.getElementById('formLogin')
                .classList.add('hidden');
        }
    }

    function showAuthDialog(title, message, callback) {

        document.getElementById('modalTitle').innerText = title;
        document.getElementById('modalMessage').innerText = message;

        document.getElementById('modalInputs')
            .classList.remove('hidden');

        document.getElementById('modalBtnCancel')
            .classList.remove('hidden');

        document.getElementById('modalSecretInput').value = '';

        confirmCallback = callback;

        document.getElementById('customModal')
            .classList.remove('hidden');
    }

    function showNotification(title, message) {

        document.getElementById('modalTitle').innerText = title;
        document.getElementById('modalMessage').innerText = message;

        document.getElementById('modalInputs')
            .classList.add('hidden');

        document.getElementById('modalBtnCancel')
            .classList.add('hidden');

        confirmCallback = null;

        document.getElementById('customModal')
            .classList.remove('hidden');
    }

    function closeNotification() {
        document.getElementById('customModal')
            .classList.add('hidden');
    }

    document.getElementById('modalBtnConfirm').onclick = function() {

        const pass =
            document.getElementById('modalSecretInput').value;

        if (confirmCallback) {
            confirmCallback(pass);
        } else {
            closeNotification();
        }
    };

    function loginOnly() {

        const u =
            document.getElementById('loginUser').value;

        const p =
            document.getElementById('loginPass').value;

        if(!u || !p) {

            return showNotification(
                "Campos Incompletos",
                "Por favor ingresa tu usuario y contraseña."
            );
        }

        userPassword = p;
        authHeader =
            'Basic ' + btoa(u + ':' + p);

        fetch(
            `/login-user?user=${encodeURIComponent(u)}&pass=${encodeURIComponent(p)}`
        )
        .then(res => res.text())
        .then(resText => {

            if(resText.trim() === "OK") {

                document.getElementById('loginScreen')
                    .classList.add('hidden');

                document.getElementById('countryModal')
                    .classList.remove('hidden');

            } else {

                showNotification(
                    "Acceso Denegado",
                    "Usuario o contraseña incorrectos."
                );
            }

        })
        .catch(() =>
            showNotification(
                "Error",
                "No se pudo conectar con el ESP32."
            )
        );
    }

    function registerOnly() {

        const key =
            document.getElementById('tallerKeyInput').value;

        const u =
            document.getElementById('regUser').value;

        const p =
            document.getElementById('regPass').value;

        if(!key || !u || !p) {

            return showNotification(
                "Campos Incompletos",
                "Debes completar la palabra clave, el nuevo usuario y la contraseña."
            );
        }

        userPassword = p;
        authHeader =
            'Basic ' + btoa(u + ':' + p);

        fetch(
            `/register-user?key=${encodeURIComponent(key)}&user=${encodeURIComponent(u)}&pass=${encodeURIComponent(p)}`
        )
        .then(res => res.text())
        .then(resText => {

            if(resText.trim() === "OK") {

                document.getElementById('loginScreen')
                    .classList.add('hidden');

                document.getElementById('countryModal')
                    .classList.remove('hidden');

            } else if(resText.trim() === "WRONG_KEY") {

                showNotification(
                    "Palabra Clave Incorrecta",
                    "La palabra clave introducida es errónea."
                );

            } else {

                showNotification(
                    "Usuario Existente",
                    "Ese usuario ya existe con otra contraseña."
                );
            }

        })
        .catch(() =>
            showNotification(
                "Error",
                "No se pudo conectar con el ESP32."
            )
        );
    }

    function confirmCountry() {

        const offset =
            document.getElementById('countrySelect').value;

        fetch(
            '/set-timezone?offset=' + offset,
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(() => {

            document.getElementById('countryModal')
                .classList.add('hidden');

            document.getElementById('dashboard')
                .classList.remove('hidden');

            syncWithDevice();

            setInterval(getESPTime, 1000);
            timerPoll = setInterval(loadTimer, 1000);

            loadAlarms();
            loadTimer();
            loadNetworks();

            // NUEVO: cargar la red Wi-Fi actual
            loadCurrentWifi();
        });
    }

    function getESPTime() {
        // Cada modo tiene su propia fuente de datos.
        // Si estamos en cronometro, NO consultamos ni mostramos la hora del reloj.
        fetch('/get-timer', {headers:{'Authorization':authHeader}})
            .then(res => res.json())
            .then(timerState => {
                if (timerState.modo === 'timer') {
                    document.getElementById('clock').innerText = timerState.display;
                    actualizarModoPagina('timer', timerState.display);
                    return;
                }

                // Solo en MODO RELOJ se consulta el DS3231.
                return fetch('/get-time', {headers:{'Authorization':authHeader}})
                    .then(res => res.text())
                    .then(t => {
                        if (t) {
                            document.getElementById('clock').innerText = t;
                            actualizarModoPagina('clock');
                            checkAlarmTrigger(t);
                        }
                    });
            })
            .catch(() => {});
    }

    // NUEVO: MOSTRAR LA RED WI-FI ACTUAL
    function loadCurrentWifi() {

        fetch(
            '/get-current-network',
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(res => res.text())
        .then(ssid => {

            const wifiElement =
                document.getElementById('currentWifi');

            if (ssid && ssid.trim() !== '') {

                wifiElement.innerText =
                    ssid.trim();

            } else {

                wifiElement.innerText =
                    'Sin conexión Wi-Fi';
            }

        })
        .catch(() => {

            document.getElementById('currentWifi')
                .innerText = 'No disponible';
        });
    }

    function checkAlarmTrigger(currentTimeStr) {

        const currentHM =
            currentTimeStr.substring(0, 5);

        const seconds =
            currentTimeStr.substring(6, 8);

        if (
            seconds === "00" &&
            currentHM !== lastTriggeredMinute
        ) {

            activeAlarms.forEach(al => {

                const h =
                    String(al.hora).padStart(2, '0');

                const m =
                    String(al.minuto).padStart(2, '0');

                if(`${h}:${m}` === currentHM) {

                    lastTriggeredMinute =
                        currentHM;

                    document.getElementById(
                        'alarmTriggerTime'
                    ).innerText = `${h}:${m}`;

                    document.getElementById(
                        'alarmTriggerModal'
                    ).classList.remove('hidden');
                }
            });
        }
    }

    function dismissAlarmTrigger() {

        document.getElementById(
            'alarmTriggerModal'
        ).classList.add('hidden');
    }

    function setManualTime() {

        const val =
            document.getElementById('manualDateTime').value;

        if (!val) {

            return showNotification(
                "Fecha Inválida",
                "Selecciona una fecha y hora válidas."
            );
        }

        const epoch =
            Math.floor(
                new Date(val).getTime() / 1000
            );

        fetch(
            '/set-time?epoch=' + epoch,
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(() => {

            showNotification(
                "✅ Hora Ajustada",
                "La hora del ESP32 se actualizó con éxito."
            );

            getESPTime();
        });
    }

    function loadNetworks() {

        fetch(
            '/get-networks',
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(res => res.json())
        .then(networks => {

            const c =
                document.getElementById('wifiContainer');

            c.innerHTML = '';

            if(networks.length === 0) {

                c.innerHTML =
                    '<p style="color:#666; font-size:0.85rem;">No hay redes guardadas.</p>';

                return;
            }

            networks.forEach(net => {

                c.innerHTML += `
                    <div class="item-list">

                        <span>
                            📶 ${net.ssid}
                        </span>

                        <div>

                            <button
                                class="btn-small btn-outline"
                                onclick="requestConnectWifi(${net.id})"
                            >
                                Conectar
                            </button>

                            <button
                                class="btn-small btn-danger"
                                onclick="requestDelWifi(${net.id})"
                            >
                                Borrar
                            </button>

                        </div>

                    </div>`;
            });
        });
    }

    function requestAddWifi() {

        const s =
            document.getElementById('wifiSSID').value;

        const p =
            document.getElementById('wifiPASS').value;

        if(!s || !p) {

            return showNotification(
                "Campos Incompletos",
                "Por favor ingresa SSID y Contraseña."
            );
        }

        showAuthDialog(
            "🔒 Verificación Requerida",
            "Ingresa tu contraseña personal para guardar la nueva red:",
            (pass) => {

                if(pass === userPassword) {

                    fetch(
                        `/add-wifi?ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent(p)}`,
                        {
                            headers: {
                                'Authorization': authHeader
                            }
                        }
                    )
                    .then(() => {

                        closeNotification();

                        document.getElementById(
                            'wifiSSID'
                        ).value = '';

                        document.getElementById(
                            'wifiPASS'
                        ).value = '';

                        loadNetworks();

                        showNotification(
                            "✅ Red Guardada",
                            "La red Wi-Fi fue agregada correctamente."
                        );
                    });

                } else {

                    showNotification(
                        "Acceso Denegado",
                        "Contraseña personal incorrecta."
                    );
                }
            }
        );
    }

    function requestConnectWifi(id) {

        showAuthDialog(
            "🔒 Autorizar Cambio de Red",
            "Ingresa tu contraseña personal para conectar a esta red:",
            (pass) => {

                if(pass === userPassword) {

                    fetch(
                        `/connect-wifi?id=${id}`,
                        {
                            headers: {
                                'Authorization': authHeader
                            }
                        }
                    )
                    .then(() => {

                        closeNotification();

                        showNotification(
                            "🔄 Conectando...",
                            "El ESP32 se está reiniciando para conectarse a la red seleccionada."
                        );
                    });

                } else {

                    showNotification(
                        "Acceso Denegado",
                        "Contraseña personal incorrecta."
                    );
                }
            }
        );
    }

    function requestDelWifi(id) {

        showAuthDialog(
            "🔒 Autorizar Eliminación",
            "Ingresa tu contraseña personal para borrar la red:",
            (pass) => {

                if(pass === userPassword) {

                    fetch(
                        `/del-wifi?id=${id}`,
                        {
                            headers: {
                                'Authorization': authHeader
                            }
                        }
                    )
                    .then(() => {

                        closeNotification();

                        loadNetworks();

                        showNotification(
                            "🗑️ Eliminada",
                            "Red eliminada de la lista."
                        );
                    });

                } else {

                    showNotification(
                        "Acceso Denegado",
                        "Contraseña personal incorrecta."
                    );
                }
            }
        );
    }

    function syncWithDevice() {

        const epoch =
            Math.floor(Date.now() / 1000);

        fetch(
            '/set-time?epoch=' + epoch,
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(() => getESPTime());
    }

    function timerSet() {
        const h = Math.max(0, Math.min(99, parseInt(document.getElementById('timerHours').value || 0)));
        const m = Math.max(0, Math.min(59, parseInt(document.getElementById('timerMinutes').value || 0)));
        const sec = Math.max(0, Math.min(59, parseInt(document.getElementById('timerSeconds').value || 0)));
        fetch(`/timer-set?h=${h}&m=${m}&s=${sec}`, {headers:{'Authorization':authHeader}})
            .then(r => r.text())
            .then(() => setDisplayMode('timer'))
            .catch(() => showNotification('Error', 'No se pudo configurar el cronómetro.'));
    }

    function timerStart() {
        // Al iniciar, toma directamente el tiempo escrito en los campos.
        // Ya no hace falta un boton separado de "Configurar".
        const h = Math.max(0, Math.min(99, parseInt(document.getElementById('timerHours').value || 0)));
        const m = Math.max(0, Math.min(59, parseInt(document.getElementById('timerMinutes').value || 0)));
        const sec = Math.max(0, Math.min(59, parseInt(document.getElementById('timerSeconds').value || 0)));

        fetch(`/timer-start?h=${h}&m=${m}&s=${sec}`, {headers:{'Authorization':authHeader}})
            .then(r => r.text())
            .then(() => {
                setDisplayMode('timer');
                loadTimer();
            })
            .catch(() => showNotification('Error', 'No se pudo iniciar el cronómetro.'));
    }

    function timerPause() {
        fetch('/timer-pause', {headers:{'Authorization':authHeader}})
            .then(r => r.text())
            .then(() => { setDisplayMode('timer'); })
            .catch(() => showNotification('Error', 'No se pudo pausar el cronómetro.'));
    }

    function timerReset() {
        fetch('/timer-reset', {headers:{'Authorization':authHeader}})
            .then(r => r.text())
            .then(() => { setDisplayMode('timer'); })
            .catch(() => showNotification('Error', 'No se pudo reiniciar el cronómetro.'));
    }

    function actualizarModoPagina(mode, timerText) {
        const title = document.getElementById('mainModeTitle');
        const badge = document.getElementById('mainModeBadge');
        const mainDisplay = document.getElementById('clock');

        if (mode === 'timer') {
            title.innerText = '⏱️ Cronómetro / Temporizador';
            badge.innerText = '⏱️ MODO CRONÓMETRO';
            if (timerText) mainDisplay.innerText = timerText;
        } else {
            title.innerText = '⏱️ Hora del Reloj';
            badge.innerText = '🕐 MODO RELOJ';
        }
    }

    function setDisplayMode(mode) {
        // Cambiamos inmediatamente la interfaz para que no dependa del polling.
        if (mode === 'timer') {
            actualizarModoPagina('timer');
        } else {
            actualizarModoPagina('clock');
        }

        fetch('/display-mode?mode=' + encodeURIComponent(mode), {headers:{'Authorization':authHeader}})
            .then(r => r.text())
            .then(() => loadTimer())
            .catch(() => showNotification('Error', 'No se pudo cambiar el modo del display.'));
    }

    function loadTimer() {
        fetch('/get-timer', {headers:{'Authorization':authHeader}})
            .then(res => res.json())
            .then(t => {
                const timerText = t.display || '00:00:00';
                const mode = t.modo || 'clock';

                document.getElementById('timerDisplay').innerText = timerText;
                actualizarModoPagina(mode, timerText);

                const active = document.activeElement;
                if (active !== document.getElementById('timerHours') &&
                    active !== document.getElementById('timerMinutes') &&
                    active !== document.getElementById('timerSeconds')) {
                    document.getElementById('timerHours').value = t.horas;
                    document.getElementById('timerMinutes').value = t.minutos;
                    document.getElementById('timerSeconds').value = t.segundos;
                }
            })
            .catch(() => {});
    }

    function loadAlarms() {

        fetch(
            '/get-alarms',
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(res => res.json())
        .then(alarms => {

            activeAlarms = alarms;

            const c =
                document.getElementById('alarmContainer');

            c.innerHTML = '';

            alarms.forEach(al => {

                const h =
                    String(al.hora).padStart(2, '0');

                const m =
                    String(al.minuto).padStart(2, '0');

                c.innerHTML += `
                    <div class="item-list">

                        <span>
                            ⏰ ${h}:${m} hs
                        </span>

                        <button
                            class="btn-small btn-danger"
                            onclick="deleteAlarm(${al.id})"
                        >
                            Borrar
                        </button>

                    </div>`;
            });
        });
    }

    function addAlarm() {

        const val =
            document.getElementById('alarmInput').value;

        if(!val) {

            return showNotification(
                "Hora requerida",
                "Selecciona un horario para la alarma."
            );
        }

        fetch(
            '/add-alarm?time=' + val,
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(() => loadAlarms());
    }

    function deleteAlarm(id) {

        fetch(
            '/del-alarm?id=' + id,
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(() => loadAlarms());
    }

</script>

</body>
</html>
)rawliteral";


void iniciarModoDual() {

  Serial.println(
    "\n--- Activando Modo Dual (AP + STA) ---"
  );

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(
    "ESP32-Reloj-Config",
    "12345678"
  );

  Serial.print(
    "Punto de acceso permanente activo: http://"
  );

  Serial.println(WiFi.softAPIP());
}


bool conectarWifiPorIndice(int index) {

  preferences.begin(
    "wifi-list",
    true
  );

  String target_ssid =
      preferences.getString(
        ("ssid_" + String(index)).c_str(),
        ""
      );

  String target_pass =
      preferences.getString(
        ("pass_" + String(index)).c_str(),
        ""
      );

  preferences.end();

  if (target_ssid == "")
    return false;

  WiFi.persistent(false);

  iniciarModoDual();

  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  Serial.printf(
    "Conectando a red [%s]...\n",
    target_ssid.c_str()
  );

  WiFi.begin(
    target_ssid.c_str(),
    target_pass.c_str()
  );

  int intentos = 0;

  while (
    WiFi.status() != WL_CONNECTED &&
    intentos < 20
  ) {

    delay(500);

    Serial.print(".");

    intentos++;
  }

  return (
    WiFi.status() == WL_CONNECTED
  );
}


void setup() {

  Serial.begin(115200);

  delay(1000);

  // MAX7219
  max7219Init();

  // DS3231 - I2C del ESP32
  Wire.begin(21, 22);

  if (!ds3231.begin()) {
    Serial.println("ERROR: No se encontro el DS3231. Verifica SDA, SCL, VCC y GND.");
  } else {
    Serial.println("DS3231 detectado correctamente.");

    if (ds3231.lostPower()) {
      Serial.println("El DS3231 perdio la alimentacion o no tiene la hora configurada.");
      Serial.println("Usa 'Sincronizar Celular' para ajustar la hora.");
    }

    DateTime ahora = ds3231.now();

    // Cargar la hora del DS3231 tambien en el reloj interno del ESP32.
    rtc.setTime(ahora.unixtime());

    Serial.printf("Hora DS3231: %02d:%02d:%02d\n",
                  ahora.hour(), ahora.minute(), ahora.second());
  }

  // Recuperar las alarmas guardadas antes de iniciar el servidor.
  cargarAlarmas();

  int totalRedes =
      getNetworkCount();

  if (totalRedes > 0) {

    Serial.printf(
      "\nSe encontraron %d redes Wi-Fi guardadas.\n",
      totalRedes
    );

    if (conectarWifiPorIndice(0)) {

      Serial.println(
        "\n🎉 ¡CONECTADO EXITOSAMENTE!"
      );

      Serial.print(
        "IP asignada: http://"
      );

      Serial.println(
        WiFi.localIP()
      );
      // antes era mireloj.local
      if (MDNS.begin("reloj")) {
  MDNS.addService("http", "tcp", 80);
  Serial.println("Dominio local activo: http://reloj.local");
}

    } else {

      Serial.println(
        "\n❌ No se pudo conectar a la red predeterminada."
      );
    }

  } else {

    Serial.println(
      "No hay redes Wi-Fi guardadas."
    );

    iniciarModoDual();
  }

  server.begin();
  actualizarMAX7219();
}


void loop() {

  actualizarMAX7219();
  servicioRemoto();

  WiFiClient client =
      server.available();

  if (client) {

    String currentLine = "";
    String requestHeader = "";

    while (client.connected()) {

      if (client.available()) {

        char c = client.read();

        requestHeader += c;

        if (c == '\n') {

          if (currentLine.length() == 0) {

            // INICIAR SESIÓN CON USUARIO REGISTRADO
            if (
              requestHeader.indexOf(
                "GET /login-user"
              ) >= 0
            ) {

              int idxUser =
                  requestHeader.indexOf("user=") + 5;

              int endUser =
                  requestHeader.indexOf("&", idxUser);

              int idxPass =
                  requestHeader.indexOf("pass=") + 5;

              int endPass =
                  requestHeader.indexOf(" ", idxPass);

              String user =
                  (idxUser > 4 && endUser > 0)
                  ? urlDecode(
                      requestHeader.substring(
                        idxUser,
                        endUser
                      )
                    )
                  : "";

              String pass =
                  (idxPass > 4 && endPass > 0)
                  ? urlDecode(
                      requestHeader.substring(
                        idxPass,
                        endPass
                      )
                    )
                  : "";

              user.trim();
              pass.trim();

              preferences.begin(
                "users",
                true
              );

              String storedPass =
                  preferences.getString(
                    user.c_str(),
                    ""
                  );

              preferences.end();

              if (
                storedPass != "" &&
                storedPass == pass
              ) {

                client.println(
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/plain\r\n"
                  "\r\n"
                  "OK"
                );

              } else {

                client.println(
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/plain\r\n"
                  "\r\n"
                  "FAIL"
                );
              }
            }

            // REGISTRO DE NUEVO USUARIO
            // (REQUIERE 'taller')
            else if (
              requestHeader.indexOf(
                "GET /register-user"
              ) >= 0
            ) {

              int idxKey =
                  requestHeader.indexOf("key=") + 4;

              int endKey =
                  requestHeader.indexOf("&", idxKey);

              int idxUser =
                  requestHeader.indexOf("user=") + 5;

              int endUser =
                  requestHeader.indexOf("&", idxUser);

              int idxPass =
                  requestHeader.indexOf("pass=") + 5;

              int endPass =
                  requestHeader.indexOf(" ", idxPass);

              String key =
                  (idxKey > 3 && endKey > 0)
                  ? urlDecode(
                      requestHeader.substring(
                        idxKey,
                        endKey
                      )
                    )
                  : "";

              String user =
                  (idxUser > 4 && endUser > 0)
                  ? urlDecode(
                      requestHeader.substring(
                        idxUser,
                        endUser
                      )
                    )
                  : "";

              String pass =
                  (idxPass > 4 && endPass > 0)
                  ? urlDecode(
                      requestHeader.substring(
                        idxPass,
                        endPass
                      )
                    )
                  : "";

              key.trim();
              user.trim();
              pass.trim();

              if (
                key.equalsIgnoreCase("taller")
              ) {

                preferences.begin(
                  "users",
                  false
                );

                String existingPass =
                    preferences.getString(
                      user.c_str(),
                      ""
                    );

                if (
                  existingPass == "" ||
                  existingPass == pass
                ) {

                  preferences.putString(
                    user.c_str(),
                    pass
                  );

                  preferences.end();

                  client.println(
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "\r\n"
                    "OK"
                  );

                } else {

                  preferences.end();

                  client.println(
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "\r\n"
                    "USER_EXISTS"
                  );
                }

              } else {

                client.println(
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/plain\r\n"
                  "\r\n"
                  "WRONG_KEY"
                );
              }
            }

            // RUTA DE ESTADO DE LA RED
            else if (
              requestHeader.indexOf(
                "GET /get-status"
              ) >= 0
            ) {

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "{\"status\":\"ok\"}"
              );
            }

            // NUEVO:
            // OBTENER RED WI-FI ACTUAL
            else if (
              requestHeader.indexOf(
                "GET /get-current-network"
              ) >= 0
            ) {

              String currentSSID =
                  WiFi.SSID();

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; charset=UTF-8\r\n"
                "\r\n" +
                currentSSID
              );
            }

            // OBTENER LISTA DE REDES
            else if (
              requestHeader.indexOf(
                "GET /get-networks"
              ) >= 0
            ) {

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n" +
                getNetworkJSON()
              );
            }

            // AGREGAR NUEVA RED WI-FI
            else if (
              requestHeader.indexOf(
                "GET /add-wifi?"
              ) >= 0
            ) {

              int idxSSID =
                  requestHeader.indexOf("ssid=") + 5;

              int endSSID =
                  requestHeader.indexOf("&", idxSSID);

              int idxPASS =
                  requestHeader.indexOf("pass=") + 5;

              int endPASS =
                  requestHeader.indexOf(" ", idxPASS);

              String newSSID =
                  urlDecode(
                    requestHeader.substring(
                      idxSSID,
                      endSSID
                    )
                  );

              String newPASS =
                  urlDecode(
                    requestHeader.substring(
                      idxPASS,
                      endPASS
                    )
                  );

              saveNetwork(
                newSSID,
                newPASS
              );

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "OK"
              );
            }

            // ELIMINAR RED WI-FI
            else if (
              requestHeader.indexOf(
                "GET /del-wifi?"
              ) >= 0
            ) {

              int idx =
                  requestHeader.indexOf("id=") + 3;

              int targetId =
                  requestHeader.substring(
                    idx,
                    requestHeader.indexOf(" ", idx)
                  ).toInt();

              removeNetwork(targetId);

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "OK"
              );
            }

            // CONECTAR A RED ESPECÍFICA
            else if (
              requestHeader.indexOf(
                "GET /connect-wifi?"
              ) >= 0
            ) {

              int idx =
                  requestHeader.indexOf("id=") + 3;

              int targetId =
                  requestHeader.substring(
                    idx,
                    requestHeader.indexOf(" ", idx)
                  ).toInt();

              preferences.begin(
                "wifi-list",
                true
              );

              String s =
                  preferences.getString(
                    ("ssid_" + String(targetId)).c_str(),
                    ""
                  );

              String p =
                  preferences.getString(
                    ("pass_" + String(targetId)).c_str(),
                    ""
                  );

              preferences.end();

              if (s != "") {

                removeNetwork(targetId);

                std::vector<
                  std::pair<String, String>
                > temp;

                temp.push_back({
                  s,
                  p
                });

                int count =
                    getNetworkCount();

                preferences.begin(
                  "wifi-list",
                  true
                );

                for (int i = 0; i < count; i++) {

                  temp.push_back({

                    preferences.getString(
                      ("ssid_" + String(i)).c_str(),
                      ""
                    ),

                    preferences.getString(
                      ("pass_" + String(i)).c_str(),
                      ""
                    )
                  });
                }

                preferences.end();

                preferences.begin(
                  "wifi-list",
                  false
                );

                preferences.clear();

                preferences.putInt(
                  "count",
                  temp.size()
                );

                for (
                  size_t i = 0;
                  i < temp.size();
                  i++
                ) {

                  preferences.putString(
                    ("ssid_" + String(i)).c_str(),
                    temp[i].first
                  );

                  preferences.putString(
                    ("pass_" + String(i)).c_str(),
                    temp[i].second
                  );
                }

                preferences.end();
              }

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "OK"
              );

              delay(1000);

              ESP.restart();
            }

            else if (
              requestHeader.indexOf(
                "GET /set-timezone?offset="
              ) >= 0
            ) {

              int idx =
                  requestHeader.indexOf("offset=") + 7;

              gmtOffset_sec =
                  requestHeader.substring(
                    idx,
                    requestHeader.indexOf(" ", idx)
                  ).toInt();

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "OK"
              );
            }

            else if (
              requestHeader.indexOf(
                "GET /get-time "
              ) >= 0
            ) {

              DateTime ahora = ds3231.now();

              char hora[9];
              snprintf(
                hora,
                sizeof(hora),
                "%02d:%02d:%02d",
                ahora.hour(),
                ahora.minute(),
                ahora.second()
              );

              client.println("HTTP/1.1 200 OK");
              client.println("Content-Type: text/plain");
              client.println("Cache-Control: no-cache, no-store, must-revalidate");
              client.println("Pragma: no-cache");
              client.println("Expires: 0");
              client.println("Connection: close");
              client.println();
              client.println(hora);
            }

            else if (
              requestHeader.indexOf(
                "GET /set-time?epoch="
              ) >= 0
            ) {

              int idx =
                  requestHeader.indexOf("epoch=") + 6;

              unsigned long epoch =
                  requestHeader.substring(
                    idx,
                    requestHeader.indexOf(" ", idx)
                  ).toInt();

              unsigned long horaLocal =
                epoch + gmtOffset_sec;

              rtc.setTime(horaLocal);

              // Guardar tambien la hora en el DS3231
              ds3231.adjust(
                DateTime(horaLocal)
              );

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "OK"
              );
            }

            else if (requestHeader.indexOf("GET /get-timer ") >= 0) {
              actualizarTimer();
              uint32_t restante = obtenerTimerRestante();
              uint32_t hh = restante / 3600UL;
              uint32_t mm = (restante % 3600UL) / 60UL;
              uint32_t ss = restante % 60UL;

              char display[12];
              snprintf(display, sizeof(display), "%02lu:%02lu:%02lu",
                       (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);

              String json = "{\"horas\":" + String(hh) +
                            ",\"minutos\":" + String(mm) +
                            ",\"segundos\":" + String(ss) +
                            ",\"corriendo\":" + String(timerCorriendo ? "true" : "false") +
                            ",\"modo\":\"" + String(modoCronometro ? "timer" : "clock") +
                            "\",\"display\":\"" + String(display) + "\"}";
              client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" + json);
            }

            else if (requestHeader.indexOf("GET /timer-set?") >= 0) {
              int hidx = requestHeader.indexOf("h=") + 2;
              int midx = requestHeader.indexOf("m=") + 2;
              int sidx = requestHeader.indexOf("s=") + 2;
              int h = requestHeader.substring(hidx, requestHeader.indexOf("&", hidx)).toInt();
              int m = requestHeader.substring(midx, requestHeader.indexOf("&", midx)).toInt();
              int sec = requestHeader.substring(sidx, requestHeader.indexOf(" ", sidx)).toInt();
              h = constrain(h, 0, 99); m = constrain(m, 0, 59); sec = constrain(sec, 0, 59);
              timerTotalSegundos = (uint32_t)h * 3600UL + (uint32_t)m * 60UL + (uint32_t)sec;
              timerRestantesSegundos = timerTotalSegundos;
              timerCorriendo = false;
              modoCronometro = true;
              client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK");
            }

            else if (requestHeader.indexOf("GET /timer-start") >= 0) {
              // Iniciar tambien recibe el tiempo de los campos de la pagina.
              // Asi el usuario puede escribir minutos/segundos y pulsar INICIAR
              // sin tener que configurar previamente.
              int h = 0;
              int m = 0;
              int sec = 0;

              int hidx = requestHeader.indexOf("h=");
              int midx = requestHeader.indexOf("m=");
              int sidx = requestHeader.indexOf("s=");

              if (hidx >= 0) {
                hidx += 2;
                int end = requestHeader.indexOf("&", hidx);
                if (end < 0) end = requestHeader.indexOf(" ", hidx);
                h = requestHeader.substring(hidx, end).toInt();
              }
              if (midx >= 0) {
                midx += 2;
                int end = requestHeader.indexOf("&", midx);
                if (end < 0) end = requestHeader.indexOf(" ", midx);
                m = requestHeader.substring(midx, end).toInt();
              }
              if (sidx >= 0) {
                sidx += 2;
                int end = requestHeader.indexOf(" ", sidx);
                sec = requestHeader.substring(sidx, end).toInt();
              }

              h = constrain(h, 0, 99);
              m = constrain(m, 0, 59);
              sec = constrain(sec, 0, 59);

              uint32_t nuevoTotal = (uint32_t)h * 3600UL + (uint32_t)m * 60UL + (uint32_t)sec;
              if (nuevoTotal > 0) {
                timerTotalSegundos = nuevoTotal;
                timerRestantesSegundos = nuevoTotal;
                timerUltimoTick = millis();
                timerCorriendo = true;
                // Iniciar el cronometro activa exclusivamente este modo.
                modoCronometro = true;
                Serial.printf("Cronometro iniciado: %02d:%02d:%02d\n", h, m, sec);
              } else {
                timerCorriendo = false;
                modoCronometro = true;
                Serial.println("Cronometro no iniciado: tiempo en 00:00:00.");
              }

              client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK");
            }

            else if (requestHeader.indexOf("GET /timer-pause") >= 0) {
              actualizarTimer();
              timerRestantesSegundos = obtenerTimerRestante();
              timerCorriendo = false;
              client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK");
            }

            else if (requestHeader.indexOf("GET /timer-reset") >= 0) {
              timerRestantesSegundos = timerTotalSegundos;
              timerCorriendo = false;
              modoCronometro = true;
              client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK");
            }

            else if (requestHeader.indexOf("GET /display-mode?mode=") >= 0) {
              int midx = requestHeader.indexOf("mode=") + 5;
              String mode = requestHeader.substring(midx, requestHeader.indexOf(" ", midx));

              if (mode.indexOf("timer") >= 0) {
                // MODO CRONOMETRO: el reloj deja de ser el modo activo.
                modoCronometro = true;
              } else {
                // MODO RELOJ: detener completamente el cronometro para que
                // ambos modos nunca funcionen al mismo tiempo.
                modoCronometro = false;
                timerCorriendo = false;
              }

              client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nOK");
            }

            else if (
              requestHeader.indexOf(
                "GET /get-alarms"
              ) >= 0
            ) {

              String json = "[";

              for (
                size_t i = 0;
                i < listaAlarmas.size();
                i++
              ) {

                json +=
                  "{\"id\":" +
                  String(listaAlarmas[i].id) +
                  ",\"hora\":" +
                  String(listaAlarmas[i].hora) +
                  ",\"minuto\":" +
                  String(listaAlarmas[i].minuto) +
                  "}";

                if (
                  i < listaAlarmas.size() - 1
                ) {
                  json += ",";
                }
              }

              json += "]";

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n" +
                json
              );
            }

            else if (
              requestHeader.indexOf(
                "GET /add-alarm?time="
              ) >= 0
            ) {

              int idx =
                  requestHeader.indexOf("time=") + 5;

              String timeStr =
                  requestHeader.substring(
                    idx,
                    requestHeader.indexOf(" ", idx)
                  );

              Alarma nueva = {
                proximoId++,
                timeStr.substring(0, 2).toInt(),
                timeStr.substring(3, 5).toInt(),
                true
              };

              listaAlarmas.push_back(
                nueva
              );

              // Guardar inmediatamente la nueva alarma en NVS.
              guardarAlarmas();

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "OK"
              );
            }

            else if (
              requestHeader.indexOf(
                "GET /del-alarm?id="
              ) >= 0
            ) {

              int idDel =
                  requestHeader.substring(
                    requestHeader.indexOf("id=") + 3
                  ).toInt();

              for (
                auto it = listaAlarmas.begin();
                it != listaAlarmas.end();
                ++it
              ) {

                if (it->id == idDel) {

                  listaAlarmas.erase(it);

                  break;
                }
              }

              // Guardar tambien la eliminacion en NVS.
              guardarAlarmas();

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "OK"
              );
            }

            else {

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=UTF-8\r\n"
                "\r\n"
              );

              client.println(
                index_html
              );
            }

            break;

          } else {

            currentLine = "";
          }

        } else if (c != '\r') {

          currentLine += c;
        }
      }
    }

    client.stop();
  }
}