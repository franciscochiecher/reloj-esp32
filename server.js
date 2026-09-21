const express = require("express");

const app = express();

const PORT = process.env.PORT || 10000;

// ============================================================
// CONFIGURACIÓN
// ============================================================

app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// ============================================================
// ESTADO DEL ESP32
// ============================================================

let espState = {
  online: false,
  ip: null,
  lastSeen: null,
  data: {}
};

// ============================================================
// COLA DE COMANDOS PARA EL ESP32
// ============================================================

let pendingCommands = [];

// ============================================================
// PÁGINA PRINCIPAL
// ============================================================

app.get("/", (req, res) => {
  res.status(200).send(`
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Reloj ESP32</title>

  <style>
    body {
      font-family: Arial, sans-serif;
      background: #111;
      color: white;
      text-align: center;
      padding: 40px;
    }

    .card {
      max-width: 600px;
      margin: auto;
      background: #222;
      padding: 30px;
      border-radius: 15px;
    }

    h1 {
      color: #00ff99;
    }

    .online {
      color: #00ff99;
      font-weight: bold;
    }

    .offline {
      color: #ff5555;
      font-weight: bold;
    }

    pre {
      text-align: left;
      background: #000;
      padding: 15px;
      border-radius: 10px;
      overflow-x: auto;
    }
  </style>
</head>

<body>

<div class="card">

  <h1>Reloj ESP32</h1>

  <p>
    Estado del ESP32:
    <span id="estado">Comprobando...</span>
  </p>

  <p>
    IP:
    <span id="ip">-</span>
  </p>

  <p>
    Última conexión:
    <span id="lastSeen">-</span>
  </p>

  <h3>Datos recibidos</h3>

  <pre id="datos">Cargando...</pre>

</div>

<script>

async function actualizar() {

  try {

    const respuesta = await fetch("/state");

    const datos = await respuesta.json();

    const estado = document.getElementById("estado");
    const ip = document.getElementById("ip");
    const lastSeen = document.getElementById("lastSeen");
    const datosElemento = document.getElementById("datos");

    if (datos.online) {

      estado.textContent = "ONLINE";
      estado.className = "online";

    } else {

      estado.textContent = "OFFLINE";
      estado.className = "offline";

    }

    ip.textContent = datos.ip || "-";

    lastSeen.textContent = datos.lastSeen || "-";

    datosElemento.textContent =
      JSON.stringify(datos.data || {}, null, 2);

  } catch (error) {

    document.getElementById("estado").textContent = "ERROR";

  }

}

actualizar();

setInterval(actualizar, 2000);

</script>

</body>
</html>
  `);
});

// ============================================================
// HEALTH CHECK
// ============================================================

app.get("/health", (req, res) => {

  res.status(200).json({
    ok: true,
    server: "reloj-esp32-remoto",
    time: new Date().toISOString()
  });

});

// ============================================================
// ESTADO DEL ESP32
// ============================================================
//
// GET /state
//
// Este endpoint lo puede consultar la página web.
//

app.get("/state", (req, res) => {

  res.status(200).json({
    ok: true,
    online: espState.online,
    ip: espState.ip,
    lastSeen: espState.lastSeen,
    data: espState.data
  });

});

// ============================================================
// ESTADO ENVIADO POR EL ESP32
// ============================================================
//
// POST /state
//
// El ESP32 manda aquí su estado.
//

app.post("/state", (req, res) => {

  espState.online = true;

  espState.ip =
    req.body.ip ||
    req.headers["x-forwarded-for"] ||
    null;

  espState.lastSeen =
    new Date().toISOString();

  espState.data =
    req.body;

  res.status(200).json({
    ok: true
  });

});

// ============================================================
// COMPATIBILIDAD CON /update
// ============================================================

app.post("/update", (req, res) => {

  espState.online = true;

  espState.ip =
    req.body.ip ||
    req.headers["x-forwarded-for"] ||
    null;

  espState.lastSeen =
    new Date().toISOString();

  espState.data =
    req.body;

  res.status(200).json({
    ok: true
  });

});

// ============================================================
// POLL DEL ESP32
// ============================================================
//
// MUY IMPORTANTE:
//
// El ESP32 actual espera una respuesta de texto:
//
// timer_start|0|5|0
//
// timer_pause
//
// timer_reset
//
// display_mode|timer
//
// display_mode|clock
//
// set_time|XXXXXXXX
//
// add_alarm|12:30
//
// del_alarm|0
//
// Si no hay comandos:
//
// OK
//
// No devolvemos JSON aquí porque el código actual del ESP32
// trabaja con comandos de texto.
//

app.get("/poll", (req, res) => {

  espState.online = true;

  espState.lastSeen =
    new Date().toISOString();

  if (pendingCommands.length > 0) {

    const comando =
      pendingCommands.shift();

    console.log(
      "Enviando comando al ESP32:",
      comando
    );

    res
      .status(200)
      .type("text/plain")
      .send(comando);

    return;
  }

  res
    .status(200)
    .type("text/plain")
    .send("OK");

});

// ============================================================
// AGREGAR COMANDO
// ============================================================
//
// POST /command
//
// Ejemplos:
//
// {
//   "command": "timer_start|0|5|0"
// }
//
// o:
//
// {
//   "command": "timer_pause"
// }
//

app.post("/command", (req, res) => {

  let comando = null;

  // ----------------------------------------------------------
  // Si viene directamente como string
  // ----------------------------------------------------------

  if (typeof req.body === "string") {

    comando = req.body.trim();

  }

  // ----------------------------------------------------------
  // Si viene como { command: "..." }
// ----------------------------------------------------------

  if (
    !comando &&
    typeof req.body.command === "string"
  ) {

    comando =
      req.body.command.trim();

  }

  // ----------------------------------------------------------
  // También aceptamos { type: "...", ... }
// ----------------------------------------------------------

  if (!comando && req.body.type) {

    const tipo = req.body.type;

    if (tipo === "timer_start") {

      const h =
        Number(req.body.hours || 0);

      const m =
        Number(req.body.minutes || 0);

      const s =
        Number(req.body.seconds || 0);

      comando =
        `timer_start|${h}|${m}|${s}`;

    }

    else if (tipo === "timer_pause") {

      comando = "timer_pause";

    }

    else if (tipo === "timer_reset") {

      comando = "timer_reset";

    }

    else if (tipo === "display_mode") {

      const modo =
        req.body.mode || "clock";

      comando =
        `display_mode|${modo}`;

    }

    else if (tipo === "set_time") {

      const epoch =
        Number(req.body.epoch || 0);

      comando =
        `set_time|${epoch}`;

    }

    else if (tipo === "add_alarm") {

      const hora =
        req.body.time || "";

      comando =
        `add_alarm|${hora}`;

    }

    else if (tipo === "del_alarm") {

      const id =
        Number(req.body.id || 0);

      comando =
        `del_alarm|${id}`;

    }

  }

  // ----------------------------------------------------------
  // Validar comando
  // ----------------------------------------------------------

  if (!comando) {

    res.status(400).json({
      ok: false,
      error: "No se recibió ningún comando válido"
    });

    return;
  }

  // ----------------------------------------------------------
  // Agregar comando a la cola
  // ----------------------------------------------------------

  pendingCommands.push(comando);

  console.log(
    "Comando agregado:",
    comando
  );

  res.status(200).json({
    ok: true,
    command: comando,
    queue: pendingCommands.length
  });

});

// ============================================================
// VER COLA DE COMANDOS
// ============================================================

app.get("/commands", (req, res) => {

  res.status(200).json({
    ok: true,
    queue: pendingCommands
  });

});

// ============================================================
// BORRAR COMANDOS PENDIENTES
// ============================================================

app.delete("/commands", (req, res) => {

  pendingCommands = [];

  res.status(200).json({
    ok: true
  });

});

// ============================================================
// RUTA PARA EVITAR 404 CONFUSOS
// ============================================================

app.use((req, res) => {

  res.status(404).json({
    ok: false,
    error: "Ruta no encontrada",
    path: req.originalUrl,
    method: req.method
  });

});

// ============================================================
// INICIAR SERVIDOR
// ============================================================

app.listen(PORT, "0.0.0.0", () => {

  console.log("");
  console.log("======================================");
  console.log("   SERVIDOR RELOJ ESP32 INICIADO");
  console.log("======================================");
  console.log("");
  console.log("Puerto:", PORT);
  console.log("");
  console.log("Rutas disponibles:");
  console.log("GET  /");
  console.log("GET  /health");
  console.log("GET  /state");
  console.log("POST /state");
  console.log("POST /update");
  console.log("GET  /poll");
  console.log("POST /command");
  console.log("GET  /commands");
  console.log("DELETE /commands");
  console.log("");
  console.log("======================================");
  console.log("");

});
