const express = require('express');
const crypto = require('crypto');

const app = express();
const PORT = process.env.PORT || 10000;
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || 'CAMBIAR_DEVICE_TOKEN';
const REGISTRATION_KEY = process.env.REGISTRATION_KEY || 'taller';

const SUPABASE_URL = (process.env.SUPABASE_URL || '').replace(/\/+$/, '');
const SUPABASE_SERVICE_ROLE_KEY = process.env.SUPABASE_SERVICE_ROLE_KEY || '';
const SUPABASE_TABLE = 'users';

app.use(express.json({limit:'32kb'}));
app.use(express.static('public'));

let state = {
  online:false,lastSeen:0,hora:'--:--:--',
  timer:{horas:0,minutos:0,segundos:0,corriendo:false},
  modo:'clock',display:'00:00:00',wifi:'',alarms:[]
};

const commands=[];
const sessions=new Map();

function sbHeaders(extra={}){
  return {
    apikey:SUPABASE_SERVICE_ROLE_KEY,
    Authorization:`Bearer ${SUPABASE_SERVICE_ROLE_KEY}`,
    'Content-Type':'application/json',
    ...extra
  };
}

async function sb(path, options={}){
  if(!SUPABASE_URL || !SUPABASE_SERVICE_ROLE_KEY)
    throw new Error('Supabase no configurado en Render');

  const r=await fetch(`${SUPABASE_URL}/rest/v1/${path}`,{
    ...options,
    headers:sbHeaders(options.headers||{})
  });
  const text=await r.text();
  let data=null;
  try{data=text?JSON.parse(text):null}catch{data=text}
  if(!r.ok){
    const detail=typeof data==='object'&&data
      ?(data.message||data.hint||data.details||data.error||JSON.stringify(data))
      :String(data||`HTTP ${r.status}`);
    const e=new Error(`Supabase HTTP ${r.status}: ${detail}`);
    e.status=r.status;
    throw e;
  }
  return data;
}

async function findUser(username){
  const key=encodeURIComponent(String(username).toLowerCase());
  const data=await sb(`${SUPABASE_TABLE}?select=username,password&username=eq.${key}&limit=1`);
  return Array.isArray(data)&&data.length?data[0]:null;
}

async function createUser(username,passwordHash){
  return await sb(SUPABASE_TABLE,{
    method:'POST',
    headers:{Prefer:'return=representation'},
    body:JSON.stringify({username:String(username).toLowerCase(),password:passwordHash})
  });
}

function cleanSessions(){
  const now=Date.now();
  for(const [token,info] of sessions)
    if(info.expires<now)sessions.delete(token);
}

function makeSession(username){
  const token=crypto.randomBytes(32).toString('hex');
  sessions.set(token,{username,expires:Date.now()+24*60*60*1000});
  return token;
}

function hashPassword(password){
  const salt=crypto.randomBytes(16);
  const hash=crypto.scryptSync(password,salt,64);
  return salt.toString('hex')+':'+hash.toString('hex');
}

function verifyPassword(password,stored){
  try{
    const [saltHex,hashHex]=stored.split(':');
    const hash=crypto.scryptSync(password,Buffer.from(saltHex,'hex'),64);
    const expected=Buffer.from(hashHex,'hex');
    return hash.length===expected.length&&crypto.timingSafeEqual(hash,expected);
  }catch{return false}
}

function requireWebAuth(req,res,next){
  cleanSessions();
  const token=(req.get('Authorization')||'').replace(/^Bearer\s+/i,'');
  const session=sessions.get(token);
  if(!session||session.expires<Date.now())
    return res.status(401).json({error:'No autorizado'});
  next();
}

function requireDevice(req,res,next){
  const token=req.get('X-Device-Token')||req.query.token;
  if(!token||token!==DEVICE_TOKEN)
    return res.status(401).json({error:'Dispositivo no autorizado'});
  next();
}

app.post('/api/register',async(req,res)=>{
  try{
    const username=String(req.body?.username||'').trim();
    const password=String(req.body?.password||'');
    const registrationKey=String(req.body?.registrationKey||'');

    if(registrationKey!==REGISTRATION_KEY)
      return res.status(403).json({ok:false,error:'Palabra de autorización incorrecta.'});
    if(username.length<3)
      return res.status(400).json({ok:false,error:'El usuario debe tener al menos 3 caracteres.'});
    if(password.length<4)
      return res.status(400).json({ok:false,error:'La contraseña debe tener al menos 4 caracteres.'});
    if(!/^[a-zA-Z0-9_.-]+$/.test(username))
      return res.status(400).json({ok:false,error:'El usuario solo puede usar letras, números, punto, guion y guion bajo.'});

    const key=username.toLowerCase();
    if(await findUser(key))
      return res.status(409).json({ok:false,error:'Ese usuario ya existe.'});

    try{
      await createUser(key,hashPassword(password));
    }catch(e){
      const again=await findUser(key).catch(()=>null);
      if(again)
        return res.status(409).json({ok:false,error:'Ese usuario ya existe.'});
      throw e;
    }

    console.log(`Usuario guardado en Supabase: ${key}`);
    return res.json({ok:true,token:makeSession(key)});
  }catch(e){
    console.error('ERROR /api/register:',e.message);
    return res.status(500).json({ok:false,error:'No se pudo guardar el usuario en Supabase.'});
  }
});

app.post('/api/login',async(req,res)=>{
  try{
    const username=String(req.body?.username||'').trim();
    const password=String(req.body?.password||'');
    const user=await findUser(username);

    if(!user||!verifyPassword(password,user.password))
      return res.status(401).json({ok:false,error:'Usuario o contraseña incorrectos.'});

    console.log(`Login correcto: ${user.username}`);
    return res.json({ok:true,token:makeSession(user.username)});
  }catch(e){
    console.error('ERROR /api/login:',e.message);
    return res.status(500).json({ok:false,error:'No se pudo consultar la base de usuarios.'});
  }
});

app.get('/api/state',requireWebAuth,(req,res)=>{
  const copy=JSON.parse(JSON.stringify(state));
  copy.online=Date.now()-state.lastSeen<5000;
  res.json(copy);
});

app.post('/api/command',requireWebAuth,(req,res)=>{
  const allowed=new Set(['timer_start','timer_pause','timer_reset','display_mode','set_time','add_alarm','del_alarm']);
  const {type,args={}}=req.body||{};
  if(!allowed.has(type))return res.status(400).json({error:'Comando no permitido'});
  const id=crypto.randomBytes(8).toString('hex');
  commands.push({id,type,args,created:Date.now()});
  while(commands.length>30)commands.shift();
  res.json({ok:true,id});
});

app.get('/api/device/poll',requireDevice,(req,res)=>{
  state.lastSeen=Date.now();
  state.online=true;
  const out=commands.splice(0,commands.length);
  let text='';
  for(const c of out){
    const a=c.args||{};
    if(c.type==='timer_start')text+=`timer_start|${Number(a.h)||0}|${Number(a.m)||0}|${Number(a.s)||0}\n`;
    else if(c.type==='timer_pause')text+='timer_pause\n';
    else if(c.type==='timer_reset')text+='timer_reset\n';
    else if(c.type==='display_mode')text+=`display_mode|${a.mode==='timer'?'timer':'clock'}\n`;
    else if(c.type==='set_time')text+=`set_time|${Number(a.epoch)||0}\n`;
    else if(c.type==='add_alarm')text+=`add_alarm|${String(a.time||'')}\n`;
    else if(c.type==='del_alarm')text+=`del_alarm|${Number(a.id)||0}\n`;
  }
  res.type('text/plain').send(text||'NO_COMMANDS\n');
});

app.post('/api/device/state',requireDevice,(req,res)=>{
  const body=req.body||{};
  state={
    online:true,lastSeen:Date.now(),
    hora:String(body.hora||'--:--:--'),
    timer:body.timer||{horas:0,minutos:0,segundos:0,corriendo:false},
    modo:body.modo==='timer'?'timer':'clock',
    display:String(body.display||'00:00:00'),
    wifi:String(body.wifi||''),
    alarms:Array.isArray(body.alarms)?body.alarms:[]
  };
  res.json({ok:true});
});

app.get('/health',(req,res)=>res.json({
  ok:true,
  deviceOnline:Date.now()-state.lastSeen<5000,
  database:!!(SUPABASE_URL&&SUPABASE_SERVICE_ROLE_KEY)
}));

app.listen(PORT,'0.0.0.0',()=>{
  console.log(`Servidor escuchando en ${PORT}`);
  console.log(SUPABASE_URL&&SUPABASE_SERVICE_ROLE_KEY
    ? 'Supabase configurado correctamente.'
    : 'ADVERTENCIA: faltan variables de Supabase.');
});
