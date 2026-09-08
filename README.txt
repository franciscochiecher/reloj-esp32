RELOJ ESP32 - ACCESO REMOTO

1) SERVIDOR
- Subir esta carpeta a GitHub.
- En Render crear un Web Service desde ese repositorio.
- Build Command: npm install
- Start Command: npm start
- Variables de entorno:
  WEB_PASSWORD = una clave para la página
  DEVICE_TOKEN = una cadena larga para identificar al ESP32

2) ESP32
- En el .ino poner REMOTE_SERVER_URL = la URL HTTPS de Render terminada en /api/device
- Poner DEVICE_TOKEN exactamente igual al de Render.
- Subir el .ino modificado.

3) FUNCIONAMIENTO
- El navegador habla solamente con el servidor público.
- El ESP32 inicia las conexiones hacia el servidor; no hace falta abrir puertos del router.
- El servidor guarda una cola de comandos y el ESP32 los ejecuta.
- La hora/cronómetro/alarma quedan físicamente en el ESP32.

IMPORTANTE
- No publiques DEVICE_TOKEN ni WEB_PASSWORD en GitHub.
- En el .ino de ejemplo reemplazá los valores antes de compilar.
