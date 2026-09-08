const express = require('express');
const crypto = require('crypto');

const app = express();
const PORT = process.env.PORT || 10000;
const WEB_PASSWORD = process.env.WEB_PASSWORD || 'cambiar-esta-clave';
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || 'CAMBIAR_DEVICE_TOKEN';

app.use(express.json({limit: '32kb'}));
app.use(express.static('public'));

let state = {
  online: false,
  lastSeen: 0,
  hora: '--:--:--',
  timer: { horas: 0, minutos: 0, segundos: 0, corriendo: false },
  modo: 'clock',
  display: '00:00:00',
  wifi: '',
  alarms: []
};

const commands = [];
const sessions = new Map();

function cleanSessions() {
  const now = Date.now();
  for (const [token, expires] of sessions) {
    if (expires < now) sessions.delete(token);
  }
}

function requireWebAuth(req, res, next) {
  cleanSessions();
  const token = req.get('Authorization')?.replace(/^Bearer\s+/i, '');
  if (!token || !sessions.has(token) || sessions.get(token) < Date.now()) {
    return res.status(401).json({error: 'No autorizado'});
  }
  next();
}

function requireDevice(req, res, next) {
  const token = req.get('X-Device-Token') || req.query.token;
  if (!token || token !== DEVICE_TOKEN) {
    return res.status(401).json({error: 'Dispositivo no autorizado'});
  }
  next();
}

app.post('/api/login', (req, res) => {
  if (!req.body || req.body.password !== WEB_PASSWORD) {
    return res.status(401).json({ok:false, error:'Clave incorrecta'});
  }
  const token = crypto.randomBytes(24).toString('hex');
  sessions.set(token, Date.now() + 24 * 60 * 60 * 1000);
  res.json({ok:true, token});
});

app.get('/api/state', requireWebAuth, (req, res) => {
  const copy = JSON.parse(JSON.stringify(state));
  copy.online = Date.now() - state.lastSeen < 5000;
  res.json(copy);
});

app.post('/api/command', requireWebAuth, (req, res) => {
  const allowed = new Set(['timer_start','timer_pause','timer_reset','display_mode','set_time','add_alarm','del_alarm']);
  const {type, args = {}} = req.body || {};
  if (!allowed.has(type)) return res.status(400).json({error:'Comando no permitido'});
  const id = crypto.randomBytes(8).toString('hex');
  commands.push({id, type, args, created: Date.now()});
  while (commands.length > 30) commands.shift();
  res.json({ok:true, id});
});

app.get('/api/device/poll', requireDevice, (req, res) => {
  state.lastSeen = Date.now();
  state.online = true;
  const out = commands.splice(0, commands.length);
  let text = '';
  for (const c of out) {
    const a = c.args || {};
    if (c.type === 'timer_start') text += `timer_start|${Number(a.h)||0}|${Number(a.m)||0}|${Number(a.s)||0}\n`;
    else if (c.type === 'timer_pause') text += 'timer_pause\n';
    else if (c.type === 'timer_reset') text += 'timer_reset\n';
    else if (c.type === 'display_mode') text += `display_mode|${a.mode === 'timer' ? 'timer' : 'clock'}\n`;
    else if (c.type === 'set_time') text += `set_time|${Number(a.epoch)||0}\n`;
    else if (c.type === 'add_alarm') text += `add_alarm|${String(a.time||'')}\n`;
    else if (c.type === 'del_alarm') text += `del_alarm|${Number(a.id)||0}\n`;
  }
  res.type('text/plain').send(text || 'NO_COMMANDS\n');
});

app.post('/api/device/state', requireDevice, (req, res) => {
  const body = req.body || {};
  state = {
    online: true,
    lastSeen: Date.now(),
    hora: String(body.hora || '--:--:--'),
    timer: body.timer || {horas:0,minutos:0,segundos:0,corriendo:false},
    modo: body.modo === 'timer' ? 'timer' : 'clock',
    display: String(body.display || '00:00:00'),
    wifi: String(body.wifi || ''),
    alarms: Array.isArray(body.alarms) ? body.alarms : []
  };
  res.json({ok:true});
});

app.get('/health', (req,res) => res.json({ok:true, deviceOnline: Date.now()-state.lastSeen < 5000}));

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Servidor escuchando en ${PORT}`);
});
