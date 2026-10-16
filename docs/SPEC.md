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

## Fase 1 — Interfaz y modos (primer esqueleto)

Pantalla 240x135 (rotación 1). Todo el texto en inglés (ver CLAUDE.md). Interfaz silenciosa.

### Estructura

- Barra inferior de una línea de texto + línea de 1 px encima: `M:Metro T:Tuner G:Game A:About`, modo
  activo resaltado, volumen a la derecha.
- Teclas globales: `M` metrónomo, `T` afinador, `G` juego, `A` extra (nombre por decidir; de momento "About").
- Volumen global (clic del metrónomo y nota de referencia): `-`/`_` baja, `=`/`+` sube.
- Cursores = teclas `;` (arriba) `,` (izquierda) `.` (abajo) `/` (derecha), con o sin Fn.
- Se persisten en NVS: volumen, BPM, subdivisión, nota objetivo y La de referencia.

### Metrónomo (genérico, a moldear)

- Dígitos: teclean BPM (Enter confirma, Del borra). Arriba/abajo: BPM ±1 (mantener = repetición).
- Izquierda/derecha: subdivisión (1, 2, 3 = tresillo, 4 por pulso). Espacio: arrancar/parar.
- `B`: pulsos por compás, de 1 a 6, rotando (tras el 6 vuelve al 1). Por defecto 4; se guarda en ajustes.
  Un círculo por pulso, justificados a la izquierda en la fila inferior (con 6 caben junto a la pista
  "B: beats", a la derecha). Acento en el primero.

### Afinador

- Aguja sobre un arco de ~100° (estilo polímetro analógico), centro = nota correcta, franja verde ±5 cents,
  escala ±50 cents.
- Arriba/abajo: La de referencia en Hz (por defecto 442, rango 400–480, persistente, visible en pantalla).
- Izquierda/derecha: nota objetivo en saltos de semitono, octavas 3 a 5 (C3–B5).
- Espacio o `P`: drone (onda senoidal) a la frecuencia de la nota objetivo.
- Detección: micrófono, autocorrelación normalizada restringida a ±2 semitonos de la nota objetivo.
  En el ADV micrófono y altavoz comparten reloj I2S: con el drone sonando la detección se pausa.

### Juego (Snake básico)

- Cursores giran la cabeza; tubo verde; puntos rojos; muere con pared o con su cuerpo.
- Al morir parpadea la puntuación; cualquier tecla reinicia. Sin sonido.

## Fase 2 — Afinador, refinamiento (por definir)

Precisión de la detección, estabilidad de la aguja, sensibilidad del micrófono. Pendiente de pruebas en el
dispositivo.

## Preguntas abiertas

- ¿Automatizar el despliegue por la WebUI de Launcher (curl al endpoint de subida) o dejarlo manual?
  Investigar el endpoint real de la WebUI antes de decidir.
- ¿Qué instrumentos/afinaciones y rango de BPM?
