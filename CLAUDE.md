# Metrotonopitecus

Metrónomo + afinador para el M5Stack Cardputer ADV. Primer proyecto de Sergio para Cardputer.
Idioma de trabajo: español (código y comentarios en inglés).

## Hardware y entorno

- Dispositivo: M5Stack **Cardputer ADV** (ESP32-S3). No es intercambiable con el Cardputer original:
  si algo de teclado/pantalla/audio falla, comprobar primero las diferencias ADV.
- El dispositivo tiene **M5Launcher** instalado (v2.8+ para ADV). Es el gestor de arranque.
- Desarrollo en Windows con PlatformIO (CLI) + Git. Repo en GitHub (privado).

## Regla crítica de despliegue

**Nunca flashear por USB (esptool/`pio run -t upload`) sobre el dispositivo.** Sobrescribiría M5Launcher.
Se compila el binario de **aplicación** (`.pio/build/<env>/firmware.bin`, NO el "merged") y se instala
a través de Launcher: WebUI (opción WUI de Launcher, subida desde el navegador), tarjeta SD u OTA.

## Comandos

- Compilar: `pio run`
- Monitor serie (para depurar lo que Claude no puede oír/ver): `pio device monitor`
- Desplegar: `pio run` y luego subir `firmware.bin` por la WebUI de Launcher (ver `docs/DEPLOY.md`
  cuando exista; hasta entonces el primer despliegue es manual).

## Convenciones

- Pasos pequeños, un commit por paso funcional. Compilar antes de cada commit.
- No añadir dependencias sin decirlo. Fijar versiones de librerías en `platformio.ini`.
- Claude no puede oír el altavoz ni ver la pantalla: lo que se pueda verificar, que salga por serie.
- El diseño y los criterios de aceptación viven en `docs/SPEC.md`. Leerlo antes de trabajar.
