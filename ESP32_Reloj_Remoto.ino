#include <WiFi.h>
#include <WiFiServer.h>
#include <ESP32Time.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <vector>

Preferences preferences;
ESP32Time rtc;
RTC_DS3231 ds3231;
WiFiServer server(80);

// DISPLAY 4 DIGITOS - SIN MAX7219
// Segmentos: A=13, B=14, C=16, D=17, E=18, F=19, G=23
// Digitos:   DIG0=25, DIG1=26, DIG2=27, DIG3=32
// El display fisico muestra HH:MM.
const int segA = 13;
const int segB = 14;
const int segC = 16;
const int segD = 17;
const int segE = 18;
const int segF = 19;
const int segG = 23;

const int dig0 = 25;
const int dig1 = 26;
const int dig2 = 27;
const int dig3 = 32;

const byte numeros[10][7] = {
  {1,1,1,1,1,1,0}, // 0
  {0,1,1,0,0,0,0}, // 1
  {1,1,0,1,1,0,1}, // 2
  {1,1,1,1,0,0,1}, // 3
  {0,1,1,0,0,1,1}, // 4
  {1,0,1,1,0,1,1}, // 5
  {1,0,1,1,1,1,1}, // 6
  {1,1,1,0,0,0,0}, // 7
  {1,1,1,1,1,1,1}, // 8
  {1,1,1,1,0,1,1}  // 9
};

void apagarDigitos() {
  digitalWrite(dig0, LOW);
  digitalWrite(dig1, LOW);
  digitalWrite(dig2, LOW);
  digitalWrite(dig3, LOW);
}

void apagarSegmentos() {
  digitalWrite(segA, LOW);
  digitalWrite(segB, LOW);
  digitalWrite(segC, LOW);
  digitalWrite(segD, LOW);
  digitalWrite(segE, LOW);
  digitalWrite(segF, LOW);
  digitalWrite(segG, LOW);
}

void ponerNumero(byte numero) {
  digitalWrite(segA, numeros[numero][0]);
  digitalWrite(segB, numeros[numero][1]);
  digitalWrite(segC, numeros[numero][2]);
  digitalWrite(segD, numeros[numero][3]);
  digitalWrite(segE, numeros[numero][4]);
  digitalWrite(segF, numeros[numero][5]);
  digitalWrite(segG, numeros[numero][6]);
}

// Multiplexado no bloqueante: cambia de digito cada 2 ms con tiempo muerto anti-ghosting.
// Asi el display sigue actualizandose incluso mientras se atiende la pagina web.
byte displayDigitos[4] = {0, 0, 0, 0};
byte displayActual = 0;
unsigned long displayAnterior = 0;

void prepararHoraDisplay() {
  DateTime ahora = ds3231.now();

  displayDigitos[0] = ahora.hour() / 10;
  displayDigitos[1] = ahora.hour() % 10;
  displayDigitos[2] = ahora.minute() / 10;
  displayDigitos[3] = ahora.minute() % 10;
}

void refrescarDisplay() {
  unsigned long ahoraMicros = micros();

  if (ahoraMicros - displayAnterior < 2000) return;
  displayAnterior = ahoraMicros;

  // CORTE TOTAL ANTES DE CAMBIAR DE DIGITO
  // Evita el "ghosting" (LEDs que quedan encendidos tenuemente).
  apagarDigitos();
  apagarSegmentos();

  // Tiempo para que los IRF9540 y 2N3904 terminen de apagarse.
  delayMicroseconds(250);

  ponerNumero(displayDigitos[displayActual]);

  // Pequeña espera con los segmentos preparados pero TODOS los
  // digitos todavia apagados.
  delayMicroseconds(50);

  if (displayActual == 0) digitalWrite(dig0, HIGH);
  else if (displayActual == 1) digitalWrite(dig1, HIGH);
  else if (displayActual == 2) digitalWrite(dig2, HIGH);
  else digitalWrite(dig3, HIGH);

  displayActual++;
  if (displayActual >= 4) displayActual = 0;
}

void actualizarMAX7219() {
  // Se conserva este nombre para que el resto del programa original
  // siga funcionando sin tocar la pagina web.
  static unsigned long anteriorHora = 0;
  unsigned long ahoraMillis = millis();

  if (ahoraMillis - anteriorHora >= 250 || anteriorHora == 0) {
    anteriorHora = ahoraMillis;
    prepararHoraDisplay();
  }

  refrescarDisplay();
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

        <h2>⏱️ Hora del Reloj</h2>

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

            loadAlarms();
            loadNetworks();

            // NUEVO: cargar la red Wi-Fi actual
            loadCurrentWifi();
        });
    }

    function getESPTime() {

        fetch(
            '/get-time',
            {
                headers: {
                    'Authorization': authHeader
                }
            }
        )
        .then(res => res.text())
        .then(t => {

            if(t) {

                document.getElementById('clock')
                    .innerText = t;

                checkAlarmTrigger(t);
            }
        });
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

  // DISPLAY DIRECTO - GPIO
  pinMode(segA, OUTPUT);
  pinMode(segB, OUTPUT);
  pinMode(segC, OUTPUT);
  pinMode(segD, OUTPUT);
  pinMode(segE, OUTPUT);
  pinMode(segF, OUTPUT);
  pinMode(segG, OUTPUT);

  pinMode(dig0, OUTPUT);
  pinMode(dig1, OUTPUT);
  pinMode(dig2, OUTPUT);
  pinMode(dig3, OUTPUT);

  apagarDigitos();
  apagarSegmentos();

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

  WiFiClient client =
      server.available();

  if (client) {

    String currentLine = "";
    String requestHeader = "";

    while (client.connected()) {

      // Mantener el multiplexado del display mientras se atiende la pagina web.
      actualizarMAX7219();

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
                "\r\n"
                "OK"
              );
            }

            else if (
              requestHeader.indexOf(
                "GET /get-time"
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
                "\r\n"
                "OK"
              );
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

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
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

              client.println(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
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
    client.stop();
  }
}
