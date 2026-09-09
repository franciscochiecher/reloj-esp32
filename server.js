const express = require('express');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = process.env.PORT || 10000;
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || 'CAMBIAR_DEVICE_TOKEN';
const REGISTRATION_KEY = process.env.REGISTRATION_KEY || 'taller';
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, 'data');
const USERS_FILE = path.join(DATA_DIR, 'users.json');

app.use(express.json({limit:'32kb'}));
app.use(express.static('public'));

let state = {
  online:false,lastSeen:0,hora:'--:--:--',
  timer:{horas:0,minutos:0,segundos:0,corriendo:false},
  modo:'clock',display:'00:00:00',wifi:'',alarms:[]
};

const commands=[];
const sessions=new Map();
const users=new Map();

function loadUsers(){
  try{
    fs.mkdirSync(DATA_DIR,{recursive:true});
    if(!fs.existsSync(USERS_FILE)) return;
    const raw=fs.readFileSync(USERS_FILE,'utf8');
    const saved=JSON.parse(raw);
    if(saved && typeof saved==='object'){
      for(const [key,user] of Object.entries(saved)){
        if(user && user.username && user.password) users.set(key,user);
      }
    }
    console.log(`Usuarios cargados desde disco: ${users.size}`);
  }catch(e){
    console.error('ERROR al cargar usuarios:',e.message);
  }
}

function saveUsers(){
  try{
    fs.mkdirSync(DATA_DIR,{recursive:true});
    const obj=Object.fromEntries(users);
    const tmp=USERS_FILE+'.tmp';
    fs.writeFileSync(tmp,JSON.stringify(obj,null,2),'utf8');
    fs.renameSync(tmp,USERS_FILE);
    console.log(`Usuarios guardados en ${USERS_FILE}: ${users.size}`);
    return true;
  }catch(e){
    console.error('ERROR al guardar usuarios:',e.message);
    return false;
  }
}

loadUsers();

function cleanSessions(){
  const now=Date.now();
  for(const [token,info] of sessions){
    if(info.expires<now)sessions.delete(token);
  }
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
    return hash.length===expected.length && crypto.timingSafeEqual(hash,expected);
  }catch(e){return false}
}
function requireWebAuth(req,res,next){
  cleanSessions();
  const token=(req.get('Authorization')||'').replace(/^Bearer\s+/i,'');
  const session=sessions.get(token);
  if(!session || session.expires<Date.now())return res.status(401).json({error:'No autorizado'});
  next();
}
function requireDevice(req,res,next){
  const token=req.get('X-Device-Token')||req.query.token;
  if(!token||token!==DEVICE_TOKEN)return res.status(401).json({error:'Dispositivo no autorizado'});
  next();
}

app.post('/api/register',(req,res)=>{
  const username=String(req.body?.username||'').trim();
  const password=String(req.body?.password||'');
  const registrationKey=String(req.body?.registrationKey||'');
  if(registrationKey !== REGISTRATION_KEY)return res.status(403).json({ok:false,error:'Palabra de autorización incorrecta.'});
  if(username.length<3)return res.status(400).json({ok:false,error:'El usuario debe tener al menos 3 caracteres.'});
  if(password.length<4)return res.status(400).json({ok:false,error:'La contraseña debe tener al menos 4 caracteres.'});
  if(!/^[a-zA-Z0-9_.-]+$/.test(username))return res.status(400).json({ok:false,error:'El usuario solo puede usar letras, números, punto, guion y guion bajo.'});
  if(users.has(username.toLowerCase()))return res.status(409).json({ok:false,error:'Ese usuario ya existe.'});
  users.set(username.toLowerCase(),{username,password:hashPassword(password)});
  if(!saveUsers()){
    users.delete(username.toLowerCase());
    return res.status(500).json({ok:false,error:'No se pudo guardar el usuario en el servidor.'});
  }
  const token=makeSession(username);
  res.json({ok:true,token});
});

app.post('/api/login',(req,res)=>{
  const username=String(req.body?.username||'').trim();
  const password=String(req.body?.password||'');
  const user=users.get(username.toLowerCase());
  if(!user||!verifyPassword(password,user.password))return res.status(401).json({ok:false,error:'Usuario o contraseña incorrectos.'});
  res.json({ok:true,token:makeSession(user.username)});
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
  state.lastSeen=Date.now();state.online=true;
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
  state={online:true,lastSeen:Date.now(),hora:String(body.hora||'--:--:--'),
    timer:body.timer||{horas:0,minutos:0,segundos:0,corriendo:false},
    modo:body.modo==='timer'?'timer':'clock',display:String(body.display||'00:00:00'),
    wifi:String(body.wifi||''),alarms:Array.isArray(body.alarms)?body.alarms:[]};
  res.json({ok:true});
});

app.get('/health',(req,res)=>res.json({ok:true,deviceOnline:Date.now()-state.lastSeen<5000,users:users.size}));

app.listen(PORT,'0.0.0.0',()=>console.log(`Servidor escuchando en ${PORT}`));
