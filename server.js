
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


// ============================================================
// MIDDLEWARE
// ============================================================

app.use(express.json({ limit: '32kb' }));

app.use(express.static('public'));


// ============================================================
// ESTADO DEL ESP32
// ============================================================

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


// ============================================================
// COMANDOS PENDIENTES PARA EL ESP32
// ============================================================

const commands = [];


// ============================================================
// SESIONES WEB
// ============================================================

const sessions = new Map();


// ============================================================
// SUPABASE
// ============================================================

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

  const response = await fetch(
    `${SUPABASE_URL}/rest/v1/${path}`,
    {
      ...options,

      headers: sbHeaders(
        options.headers || {}
      )
    }
  );

  const text = await response.text();

  let data = null;

  try {
    data = text
      ? JSON.parse(text)
      : null;
  } catch {
    data = text;
  }

  if (!response.ok) {

    const detail =
      typeof data === 'object' && data
        ? (
            data.message ||
            data.hint ||
            data.details ||
            data.error ||
            JSON.stringify(data)
          )
        : String(
            data ||
            `HTTP ${response.status}`
          );

    const error =
      new Error(
        `Supabase HTTP ${response.status}: ${detail}`
      );

    error.status = response.status;

    throw error;
  }

  return data;
}


async function findUser(username) {

  const key =
    encodeURIComponent(
      String(username).toLowerCase()
    );

  const data =
    await sb(
      `${SUPABASE_TABLE}?select=username,password&username=eq.${key}&limit=1`
    );

  return (
    Array.isArray(data) &&
    data.length
  )
    ? data[0]
    : null;
}


async function createUser(
  username,
  passwordHash
) {

  return await sb(
    SUPABASE_TABLE,
    {
      method: 'POST',

      headers: {
        Prefer:
          'return=representation'
      },

      body: JSON.stringify({
        username:
          String(username).toLowerCase(),

        password:
          passwordHash
      })
    }
  );
}


// ============================================================
// SESIONES
// ============================================================

function cleanSessions() {

  const now = Date.now();

  for (
    const [token, info]
    of sessions
  ) {

    if (
      info.expires < now
    ) {
      sessions.delete(token);
    }
  }
}


function makeSession(username) {

  const token =
    crypto
      .randomBytes(32)
      .toString('hex');

  sessions.set(
    token,
    {
      username,

      expires:
        Date.now() +
        24 * 60 * 60 * 1000
    }
  );

  return token;
}


// ============================================================
// CONTRASEÑAS
// ============================================================

function hashPassword(password) {

  const salt =
    crypto.randomBytes(16);

  const hash =
    crypto.scryptSync(
      password,
      salt,
      64
    );

  return (
    salt.toString('hex') +
    ':' +
    hash.toString('hex')
  );
}


function verifyPassword(
  password,
  stored
) {

  try {

    const [
      saltHex,
      hashHex
    ] =
      stored.split(':');

    const hash =
      crypto.scryptSync(
        password,
        Buffer.from(
          saltHex,
          'hex'
        ),
        64
      );

    const expected =
      Buffer.from(
        hashHex,
        'hex'
      );

    return (
      hash.length ===
        expected.length &&
      crypto.timingSafeEqual(
        hash,
        expected
      )
    );

  } catch {

    return false;
  }
}


// ============================================================
// AUTENTICACIÓN WEB
// ============================================================

function requireWebAuth(
  req,
  res,
  next
) {

  cleanSessions();

  const token =
    (
      req.get('Authorization') ||
      ''
    ).replace(
      /^Bearer\s+/i,
      ''
    );

  const session =
    sessions.get(token);

  if (
    !session ||
    session.expires < Date.now()
  ) {

    return res
      .status(401)
      .json({
        error:
          'No autorizado'
      });
  }

  next();
}


// ============================================================
// AUTENTICACIÓN DEL ESP32
// ============================================================

function requireDevice(
  req,
  res,
  next
) {

  const token =
    req.get('X-Device-Token') ||
    req.query.token;

  if (
    !token ||
    token !== DEVICE_TOKEN
  ) {

    return res
      .status(401)
      .json({
        error:
          'Dispositivo no autorizado'
      });
  }

  next();
}


// ============================================================
// REGISTRO
// ============================================================

app.post(
  '/api/register',
  async (req, res) => {

    try {

      const username =
        String(
          req.body?.username || ''
        ).trim();

      const password =
        String(
          req.body?.password || ''
        );

      const registrationKey =
        String(
          req.body?.registrationKey || ''
        );


      if (
        registrationKey !==
        REGISTRATION_KEY
      ) {

        return res
          .status(403)
          .json({
            ok: false,
            error:
              'Palabra de autorización incorrecta.'
          });
      }


      if (
        username.length < 3
      ) {

        return res
          .status(400)
          .json({
            ok: false,
            error:
              'El usuario debe tener al menos 3 caracteres.'
          });
      }


      if (
        password.length < 4
      ) {

        return res
          .status(400)
          .json({
            ok: false,
            error:
              'La contraseña debe tener al menos 4 caracteres.'
          });
      }


      if (
        !/^[a-zA-Z0-9_.-]+$/.test(
          username
        )
      ) {

        return res
          .status(400)
          .json({
            ok: false,
            error:
              'El usuario solo puede usar letras, números, punto, guion y guion bajo.'
          });
      }


      const key =
        username.toLowerCase();


      if (
        await findUser(key)
      ) {

        return res
          .status(409)
          .json({
            ok: false,
            error:
              'Ese usuario ya existe.'
          });
      }


      try {

        await createUser(
          key,
          hashPassword(password)
        );

      } catch (e) {

        const again =
          await findUser(key)
            .catch(() => null);

        if (again) {

          return res
            .status(409)
            .json({
              ok: false,
              error:
                'Ese usuario ya existe.'
            });
        }

        throw e;
      }


      console.log(
        `Usuario guardado en Supabase: ${key}`
      );


      return res.json({
        ok: true,
        token:
          makeSession(key)
      });

    } catch (e) {

      console.error(
        'ERROR /api/register:',
        e.message
      );

      return res
        .status(500)
        .json({
          ok: false,
          error:
            'No se pudo guardar el usuario en Supabase.'
        });
    }
  }
);


// ============================================================
// LOGIN
// ============================================================

app.post(
  '/api/login',
  async (req, res) => {

    try {

      const username =
        String(
          req.body?.username || ''
        ).trim();

      const password =
        String(
          req.bo
