# Metrotonopitecus

Metrónomo + afinador para el M5Stack Cardputer ADV. Primer proyecto de Sergio para Cardputer.
Idioma de trabajo: español (código y comentarios en inglés).

## Hardware y entorno

- Dispositivo: M5Stack **Cardputer ADV** (ESP32-S3). No es intercambiable con el Cardputer original:
  si algo de teclado/pantalla/audio falla, comprobar primero las diferencias ADV.
- El dispositivo tiene **M5Launcher** instalado (v2.8+ para ADV). Es el gestor de arranque.
- Desarrollo en Windows con PlatformIO (CLI) + Git. Repo en GitHub (privado).

## Regla crítica de despliegue

**Por defecto, Claude no flashea por USB** (esptool/`pio run -t upload`): pisaría M5Launcher.
Se compila el binario de **aplicación** (`.pio/build/<env>/firmware.bin`, NO el "merged") y Sergio lo
instala a través de Launcher: WebUI (opción WUI de Launcher, subida desde el navegador), tarjeta SD u OTA.

**Flasheo por USB, solo con permiso de Sergio.** Claude flashea por USB únicamente cuando Sergio lo pide:
- Una orden puntual ("flashea", "súbelo", "dame firmware nuevo" con el Cardputer conectado por USB) vale
  para ese firmware. Si pide firmware nuevo y el dispositivo está conectado, se entiende que se flashea.
- **Permiso permanente (Sergio, 2026-10-24):** si el Cardputer está conectado por USB (`pio device list` muestra
  el puerto), se flashea tras cada compilación correcta sin preguntar, usando el entorno normal `cardputer-adv`
  (no las variantes de diagnóstico salvo que se pidan). Si Sergio no quiere que se flashee algo, desconecta el
  dispositivo. Sin puerto visible, no se intenta nada.
- Sin puerto serie visible (`pio device list` vacío) no se intenta nada: se deja `firmware.bin` listo para
  subirlo por WebUI y se avisa.
- Flashear por USB sustituye a Launcher en ese slot; Sergio lo sabe y lo asume. Recuperar Launcher es cosa suya.
- Comando: `pio run -t upload --upload-port <COM>` (en este equipo, `COM4`; confirmar con `pio device list`).
  No hace falta pulsar nada en el dispositivo: esptool lo pone en modo descarga por USB nativo y lo reinicia
  al terminar. La pantalla se queda en negro unos segundos y luego arranca la app; es normal, no es un fallo.
- Tras flashear, avisar a Sergio de qué debe comprobar en pantalla/oído; Claude no puede verlo.

## Comandos

- Compilar: `pio run`
- Monitor serie (para depurar lo que Claude no puede oír/ver): `pio device monitor`
- Desplegar: `pio run` y luego subir `firmware.bin` por la WebUI de Launcher (ver `docs/DEPLOY.md`
  cuando exista; hasta entonces el primer despliegue es manual), o flashear por USB si Sergio lo pide
  (ver la regla de arriba).

## Convenciones

- Pasos pequeños, un commit por paso funcional. Compilar antes de cada commit.
- **Claude hace siempre commit y push con cada compilación de firmware** (`pio run`, también los
  entornos de diagnóstico): tras una compilación correcta, `git add` de los cambios, commit con mensaje
  descriptivo y `git push`, sin esperar a que Sergio lo pida. Si la compilación falla no se commitea. Si el
  push falla (red, autenticación), decirlo claramente y reintentar después; no dar nada por subido sin
  haberlo comprobado.
- No añadir dependencias sin decirlo. Fijar versiones de librerías en `platformio.ini`.
- Claude no puede oír el altavoz ni ver la pantalla: lo que se pueda verificar, que salga por serie.
- El diseño y los criterios de aceptación viven en `docs/SPEC.md`. Leerlo antes de trabajar.

## Interfaz del firmware

- **Todo el texto que muestra el firmware (pantalla y serie) va siempre en inglés**, aunque Sergio
  describa la funcionalidad en español. Esto incluye etiquetas, mensajes, nombres de modos y logs.
- **La interfaz es silenciosa**: sin pitidos al pulsar teclas ni sonidos en menús o juegos, para evitar
  disparos accidentales en ensayos. Solo suena lo musical: clic del metrónomo y nota de referencia (drone).
- Barra inferior fija de una línea de texto (con una línea de 1 px encima) para los modos; el resto
  de la pantalla es del modo activo. Teclas globales: `M` metrónomo, `T` afinador, `G` juego, `A` extra;
  `-`/`_` y `=`/`+` bajan/suben el volumen global (se aceptan ambas variantes para no necesitar Shift).
