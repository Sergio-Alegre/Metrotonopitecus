# SPEC — Metrotonopitecus

## Objetivo final

Metrónomo y afinador para Cardputer ADV, instalable desde M5Launcher.

## Fase 0 — Hello world en el flujo definitivo (esto es lo que toca ahora)

Objetivo: validar la cadena completa antes de escribir nada de la app real.

1. Proyecto PlatformIO para Cardputer ADV (placa, librerías M5Cardputer/M5Unified con versiones fijadas).
2. Programa mínimo: mostrar "Hello Metrotonopitecus" en pantalla, mostrar la última tecla pulsada,
   y emitir un pitido corto (440 Hz) al pulsar una tecla. Imprimir lo mismo por serie.
3. `pio run` compila sin errores y produce `firmware.bin` de aplicación.
4. Comprobar que el tamaño cabe en una partición OTA estándar de Launcher.
5. Sergio instala `firmware.bin` por WebUI de Launcher y confirma pantalla, teclado y sonido.

### Criterios de aceptación Fase 0

- [ ] `pio run` termina sin errores ni warnings relevantes
- [ ] El binario resultante cabe en la partición OTA de Launcher
- [ ] Instalado vía Launcher, arranca, muestra el texto y reacciona al teclado
- [ ] Se puede volver a Launcher sin problemas
- [ ] README o `docs/DEPLOY.md` con el procedimiento real de despliegue que funcionó

### Fuera de alcance en Fase 0

Metrónomo, afinador, detección de frecuencia, menús, ajustes persistentes.

## Fase 1 — Metrónomo (por definir)

BPM ajustable por teclado, click audible, indicación visual del pulso. Detalles a decidir con Sergio.

## Fase 2 — Afinador (por definir)

Captura con el micrófono, detección de frecuencia fundamental, nota más cercana y desviación en cents.
Detalles a decidir con Sergio.

## Preguntas abiertas

- ¿Automatizar el despliegue por la WebUI de Launcher (curl al endpoint de subida) o dejarlo manual?
  Investigar el endpoint real de la WebUI antes de decidir.
- ¿Qué instrumentos/afinaciones y rango de BPM?
