```js
const express = require('express');
const crypto = require('crypto');

const app = express();

const PORT = process.env.PORT || 10000;

const DEVICE_TOKEN =
  process.env.DEVICE_TOKEN || 'CAMBIAR_DEVICE_TOKEN';

const REGISTRATION_KEY =
  process.env.REGISTRATION_KEY || 'taller';

const SUPABASE_URL =
  (process.env.SUPABASE_URL || '').replace(/\/+$/, '');

const SUPABASE_SERVICE_ROLE_KEY =
  process.env.SUPABASE_SERVICE_ROLE_KEY || '';

const SUPABASE_TABLE = 'users';


// =====================================================
// EXPRESS
// =====================================================

app.use(express.json({ limit: '32kb' }));
app.use(express.static('public'));


// =====================================================
// ESTADO DEL ESP32
// =====================================================

let state = {
  online: false,
  lastSeen: 0,

  hora: '--:--:--',

  timer: {
    horas: 0,
    minutos: 0,
    segundos: 0,
    corriendo: false
  },

  modo: 'clock',

  display: '00:00:00',

  wifi: '',

  alarms: []
};


// =====================================================
// COLA DE COMANDOS
// =====================================================

const commands = [];


// =====================================================
// SESIONES
// =====================================================

const sessions = new Map();


// =====================================================
// SUPABASE
// =====================================================

function sbHeaders(extra = {}) {

  return {
    apikey: SUPABASE_SERVICE_ROLE_KEY,

    Authorization:
      `Bearer ${SUPABASE_SERVICE_ROLE_KEY}`,

    'Content-Type':
      'application/json',

    ...extra
  };
}


async function sb(path, options = {}) {

  if (
    !SUPABASE_URL ||
    !SUPABASE_SERVICE_ROLE_KEY
  ) {
    throw new Error(
      'Supabase no configurado en Render'
    );
  }

  const r = await fetch(
    `${SUPABASE_URL}/rest/v1/${path}`_
```
