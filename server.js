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

app.use(express.json({ limit: '32kb' }));
app.use(express.static('public'));

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

const commands = [];
const sessions = new Map();


// ============================================================
// SUPABASE
// ============================================================

function sbHeaders(extra = {}) {
  return {
    apikey: SUPABASE_SERVICE_ROLE_KEY,
    Authorization: `Bearer ${SUPABASE_SERVICE_ROLE_KEY}`,
    'Content-Type': 'application/json',
    ...extra
  };
}


async function sb(path, options = {}) {

  if (!SUPABASE_URL || !SUPABASE_SERVICE_ROLE_KEY) {
    throw new Error('Supabase no configurado en Render');
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

    throw new Error(
      `Supabase HTTP ${response.status}: ${detail}`
    );
  }

  return data;
}


// ============================================================
// USUARIOS
// ============================================================

async function findUser(username) {

  const key =
    encodeURIComponent(
      String(username).toLowerCase()
    );

  const data = await sb(
    `${SUPABASE_TABLE}?select=username,password&username=eq.${key}&limit=1`
  );

  if (
    Array.isArray(data) &&
    data.length
  ) {
    return data[0];
  }

  return null;
}


async function createUser(
  username,
  passwordHash
) {

  return sb(
    SUPABASE_TABLE,
    {
      method: 'POST',

      headers: {
        Prefer: 'return=representation'
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
    crypto.randomBytes(32).toString('hex');

  sessions.set(
    token,
    {
      username: username,

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

    const parts =
      String(stored).split(':');

    if (
      parts.length !== 2
    ) {
      return false;
    }

    const hash =
      crypto.scryptSync(
        password,
        Buffer.from(
          parts[0],
          'hex'
        ),
        64
      );

    const expected =
      Buffer.from(
        parts[1],
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

    return res.status(401).json({
      error: 'No autorizado'
    });
  }

  next();
}


// ============================================================
// AUTENTICACIÓN ESP32
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

    return res.status(401).json({
      error:
        'Dispositivo no autorizado'
    });
  }

  next();
}


// ============================================================
// HEALTH
// ============================================================

app.get(
  '/health',
  (req, res) => {

    res.json({

      ok: true,

      deviceOnline:
        Date.now() -
        state.lastSeen <
        5000,

      database:
        Boolean(
          SUPABASE_URL &&
          SUPABASE_SERVICE_ROLE_KEY
        )
    });
  }
);


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

        return res.status(403).json({
          ok: false,
          error:
            'Palabra de autorización incorrecta.'
        });
      }


      if (
        username.length < 3
      ) {

        return res.status(400).json({
          ok: false,
          error:
            'El usuario debe tener al menos 3 caracteres.'
        });
      }


      if (
        password.length < 4
      ) {

        return res.status(400).json({
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

        return res.status(400).json({
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

        return res.status(409).json({
          ok: false,
          error:
            'Ese usuario ya existe.'
        });
      }


      await createUser(
        key,
        hashPassword(password)
      );


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

      return res.status(500).json({

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
          req.body?.password || ''
        );


      const user =
        await findUser(username);


      if (
        !user ||
        !verifyPassword(
          password,
          user.password
        )
      ) {

        return res.status(401).json({

          ok: false,

          error:
            'Usuario o contraseña incorrectos.'
        });
      }


      console.log(
        `Login correcto: ${user.username}`
      );


      return res.json({

        ok: true,

        token:
          makeSession(
            user.username
          )
      });

    } catch (e) {

      console.error(
        'ERROR /api/login:',
        e.message
      );

      return res.status(500).json({

        ok: false,

        error:
          'No se pudo consultar la base de usuarios.'
      });
    }
  }
);


// ============================================================
// ESTADO PARA LA WEB
// ============================================================

app.get(
  '/api/state',
  requireWebAuth,
  (req, res) => {

    const copy =
      JSON.parse(
        JSON.stringify(state)
      );

    copy.online =
      Date.now() -
      state.lastSeen <
      5000;

    res.json(copy);
  }
);


// ============================================================
// COMANDOS DESDE LA WEB
// ============================================================

app.post(
  '/api/command',
  requireWebAuth,
  (req, res) => {

    const allowed =
      new Set([
        'timer_start',
        'timer_pause',
        'timer_reset',
        'display_mode',
        'set_time',
        'add_alarm',
        'del_alarm'
      ]);


    const {
      type,
      args = {}
    } =
      req.body || {};


    if (
      !allowed.has(type)
    ) {

      return res.status(400).json({

        error:
          'Comando no permitido'
      });
    }


    const id =
      crypto.randomBytes(8)
        .toString('hex');


    commands.push({

      id,

      type,

      args,

      created:
        Date.now()
    });


    while (
      commands.length > 30
    ) {

      commands.shift();
    }


    res.json({

      ok: true,

      id
    });
  }
);


// ============================================================
// POLL DEL ESP32
// ============================================================

function handleDevicePoll(
  req,
  res
) {

  state.lastSeen =
    Date.now();

  state.online =
    true;


  const out =
    commands.splice(
      0,
      commands.length
    );


  let text = '';


  for (
    const c of out
  ) {

    const a =
      c.args || {};


    if (
      c.type ===
      'timer_start'
    ) {

      text +=
        `timer_start|${Number(a.h) || 0}|${Number(a.m) || 0}|${Number(a.s) || 0}\n`;

    } else if (
      c.type ===
      'timer_pause'
    ) {

      text +=
        'timer_pause\n';

    } else if (
      c.type ===
      'timer_reset'
    ) {

      text +=
        'timer_reset\n';

    } else if (
      c.type ===
      'display_mode'
    ) {

      text +=
        `display_mode|${
          a.mode === 'timer'
            ? 'timer'
            : 'clock'
        }\n`;

    } else if (
      c.type ===
      'set_time'
    ) {

      text +=
        `set_time|${Number(a.epoch) || 0}\n`;

    } else if (
      c.type ===
      'add_alarm'
    ) {

      text +=
        `add_alarm|${String(
          a.time || ''
        )}\n`;

    } else if (
      c.type ===
      'del_alarm'
    ) {

      text +=
        `del_alarm|${Number(a.id) || 0}\n`;
    }
  }


  res
    .type('text/plain')
    .send(
      text ||
      'NO_COMMANDS\n'
    );
}


// ============================================================
// ESTADO ENVIADO POR ESP32
// ============================================================

function handleDeviceState(
  req,
  res
) {

  const body =
    req.body || {};


  state = {

    online: true,

    lastSeen:
      Date.now(),

    hora:
      String(
        body.hora ||
        '--:--:--'
      ),

    timer:
      body.timer ||
      {
        horas: 0,
        minutos: 0,
        segundos: 0,
        corriendo: false
      },

    modo:
      body.modo === 'timer'
        ? 'timer'
        : 'clock',

    display:
      String(
        body.display ||
        '00:00:00'
      ),

    wifi:
      String(
        body.wifi ||
        ''
      ),

    alarms:
      Array.isArray(
        body.alarms
      )
        ? body.alarms
        : []
  };


  res.json({
    ok: true
  });
}


// ============================================================
// RUTAS ORIGINALES
// ============================================================

app.get(
  '/api/device/poll',
  requireDevice,
  handleDevicePoll
);


app.post(
  '/api/device/state',
  requireDevice,
  handleDeviceState
);


// ============================================================
// RUTAS QUE USA TU ESP32 ACTUAL
// ============================================================

app.get(
  '/poll',
  requireDevice,
  handleDevicePoll
);


app.post(
  '/state',
  requireDevice,
  handleDeviceState
);


// ============================================================
// INICIAR SERVIDOR
// ============================================================

app.listen(
  PORT,
  '0.0.0.0',
  () => {

    console.log(
      `Servidor escuchando en ${PORT}`
    );


    if (
      SUPABASE_URL &&
      SUPABASE_SERVICE_ROLE_KEY
    ) {

      console.log(
        'Supabase configurado correctamente.'
      );

    } else {

      console.log(
        'ADVERTENCIA: faltan variables de Supabase.'
      );
    }
  }
);
