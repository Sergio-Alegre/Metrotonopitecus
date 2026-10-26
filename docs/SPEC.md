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
- **Icono de batería** en el extremo derecho de la barra, pegado al volumen (13 × 7 px, sin porcentaje; el texto `Vol` se
  desplaza a la izquierda lo que ocupa el icono, y pierde su espacio, `Vol10`, solo si no cabe junto a los modos). Precisión
  de 1/5: cinco segmentos de 20 % cada uno (`ceil(nivel / 20)`, al menos 1 mientras el nivel sea > 0, 0 con nivel 0).
  Color: normal = el del texto `Vol`; **exactamente 2 segmentos = amarillo**; **1 o 0 = rojo** (contorno y segmentos); sin verde.
  Lectura no disponible (nivel < 0 o tensión no plausible) = contorno gris sin segmentos. Si la placa informara de carga,
  el contorno sería blanco (el ADV no da ese dato, ver `TONEMAP_DESIGN.md` §14). Se lee cada ~5 s, con mediana y
  persistencia (ver §14), y solo se redibuja la barra cuando cambia el número de segmentos.
- Teclas globales: `M` metrónomo, `T` afinador, `G` juego, `A` extra (nombre por decidir; de momento "About").
- Volumen global (clic del metrónomo y nota de referencia): `-`/`_` baja, `=`/`+` sube.
- Sonido del clic del metrónomo: todos los clics a 1400 Hz; la importancia se marca con el volumen (ganancia por clic sobre el
  volumen global) y algo con la duración: primer pulso del compás 100 % y 36 ms, resto de pulsos 75 % y 30 ms,
  subdivisiones 55 % y 18 ms (constantes `kGain*`/`kMs*` en `mode_metronome.cpp`, a ajustar de oído).
- Cursores = teclas `;` (arriba) `,` (izquierda) `.` (abajo) `/` (derecha), con o sin Fn.
- Se persisten en NVS: volumen, BPM, subdivisión, nota objetivo y La de referencia.

### Metrónomo (genérico, a moldear)

- Dígitos: teclean BPM (Del borra; se confirma con Espacio, con cualquier otra tecla, al 3.º dígito o tras
  una pausa de 2,4 s). Arriba/abajo: BPM ±1 (mantener = repetición).
- `Enter`: tap tempo en dos pulsaciones y sin temporizadores, pensado para la mano derecha (con Espacio y
  los cursores). La 1.ª pulsación abre la medida (el número va en amarillo) y la 2.ª la cierra y fija el BPM
  con el tiempo entre ambas; la 3.ª abre una nueva, así que nada se arrastra de medidas anteriores. Cualquier
  otra tecla cancela la medida abierta. Un intervalo menor de 200 ms se ignora y uno mayor de 3 s cuenta como
  nueva apertura. Si había un número a medias, se confirma antes. Pista en pantalla: `Enter: tap`.
- Izquierda/derecha: subdivisión (1, 2, 3 = tresillo, 4 por pulso). Espacio: arrancar/parar.
- `B`: pulsos por compás, de 1 a 6, rotando (tras el 6 vuelve al 1). Por defecto 4; se guarda en ajustes.
  Un círculo por pulso, justificados a la izquierda en la fila inferior (con 6 caben junto a la pista
  "B: beats", a la derecha). Acento en el primero.
- LED RGB integrado (GPIO 21, brillo máximo): destello en cada pulso (no en subdivisiones), blanco en el
  acento y verde en los demás (par apto para daltonismo, ΔE mínimo simulado 57 frente a 28 de rojo/verde; los círculos de pantalla usan los mismos colores; el destello
  del acento dura 1,5 veces más, una pista que no depende del color). Se enciende justo al terminar de enviar a la pantalla el fotograma con el
  círculo iluminado, para que luz y círculo coincidan. Duración = 1/5 del pulso entre 30 y 80 ms (80 ms
  hasta 150 BPM, 40 ms a 300 BPM). Al parar, por serie sale el retardo medio clic→LED.
  Hardware: el LED va en el GPIO21 y se maneja con `neopixelWrite` (`M5.Led` no hace nada con este núcleo
  Arduino). Su alimentación es el GPIO38, el mismo pin que la retroiluminación de la pantalla (PWM), así
  que solo funciona con la pantalla a brillo alto: medido, funciona a 192 y falla a 160 (de 255). Por debajo
  de 255 el pin se corta una parte de cada periodo PWM y un comando enviado entonces se pierde (destellos que
  no terminan). El modo metrónomo pone el brillo a 255 mientras está activo y lo restaura al salir, y además
  repite cada comando del LED dos veces (a los 6 y 12 ms). El entorno
  `cardputer-adv-led-debug` añade una línea de diagnóstico y las teclas L/K para probar el LED.

### Afinador

- **Detección automática de la nota más cercana** en todo el rango C2..B7 (nota escrita = de concierto, sin
  transposición): el afinador oye lo que suene, busca la nota más próxima (temperamento igual, La = `refA`, la misma
  `tonemap::freqToNote` de la vista ToneMap) y la aguja marca cuántos cents por encima o por debajo estás **de esa nota**.
- Aguja sobre un arco de ~100° (estilo polímetro analógico), centro = nota correcta, franja verde ±5 cents,
  escala ±50 cents. Sin lectura válida la aguja vuelve a su posición de reposo (a la izquierda, gris).
- **Nota detectada**: en grande en el centro del dial (nombre con `#` dibujado, y octava, p. ej. `A#4`; blanca, verde
  dentro de ±5 cents; un `--` tenue mientras no se oye nada). Arriba en el centro, los cents con signo (`+3c`) y la
  frecuencia real en Hz.
- Arriba/abajo: La de referencia en Hz (por defecto 442, rango 400–480, persistente, visible en pantalla).
- Izquierda/derecha: **solo elige la nota del drone**, en saltos de semitono, octavas 3 a 5 (C3–B5); ya no limita la
  detección. Debajo del selector pone `Drone <Hz>`.
- Espacio o `P`: drone (onda senoidal) a la frecuencia de la nota elegida.
- Motor: el de la vista ToneMap (captura continua a 32 kHz de `tonemap_audio`, ventana de 2048 muestras, `pitch_mpm`
  con K híbrido y maxLag 720, **puerta de ruido automática** (rms ≥ 2,5 × el suelo de ruido de la sala, entre 20 y 80; ver
  abajo), claridad > 0,8, 14 < Hz < 8200; código
  común en `tonemap_detect.*`). Una lectura nueva por ventana (~31/s); suavizado exponencial de ~185 ms mientras la
  nota no cambia (un cambio de nota o un salto de más de 25 cents se toma al instante) y la lectura se mantiene 0,7 s
  sin ventanas válidas antes de devolver la aguja al reposo.
- **Puerta de ruido automática (vista clásica)**: sin tecla ni ajuste. El suelo de ruido es la mínima rms vista (baja al
  instante hacia una ventana más silenciosa, nunca por debajo de 1) y sube despacio (constante de tiempo de 30 s) hacia ventanas
  más fuertes; la puerta es `2,5 × suelo`, limitada a **20..80**. En una sala en silencio (suelo ~6) la puerta es 20, así que se
  detecta el instrumento con sordina (rms al tocar p5 ≈ 27); en una sala ruidosa sube hasta el 80 de antes y nunca más. Se
  actualiza con **todas** las ventanas (también las que quedan bajo la puerta) y se reinicia al entrar/salir y al encender/apagar
  el drone, no al perder cada lectura. La vista ToneMap mantiene su puerta manual (tabla abajo). Ver `TONEMAP_DESIGN.md` §16.
- En el ADV micrófono y altavoz comparten reloj I2S: con el drone sonando no se escucha (la aguja y el dial se ponen
  grises y el centro dice `Mic off (drone playing)`).
- **LED** (tecla `L`, mismo ajuste `tmLed` que la vista ToneMap, por defecto on): el LED RGB (GPIO 21) se pone **verde
  `(0,255,0)`** mientras la aguja está dentro de la franja verde (|cents| ≤ 5 con el valor suavizado que ya usa la vista para
  poner nota y cents en verde) y **apagado** el resto del tiempo (sin rojo ni otros colores): sin lectura, con el drone
  sonando (micro apagado), micro no disponible, LED off, al cambiar de vista, de modo o de pantalla. Misma restricción de
  hardware que el metrónomo: solo funciona con la pantalla a brillo 255, así que con `L` on la vista guarda el brillo del
  usuario, sube a 255 y lo restaura al salir de la vista o del modo, o al poner `L` off; cada orden se repite dos veces.
  Al pulsar `L` el pie de pantalla dice `LED: on` / `LED: off` durante 1,5 s en lugar de `Space: drone`. Código común con la
  vista ToneMap en `tuner_led.*` (`TONEMAP_DESIGN.md` §14).

### Afinador — vista ToneMap

Segunda vista del afinador, portada de [tonemap.live](https://github.com/crossrhythm/tonemap.live) (MIT).
Brief completo, referencia técnica y plan por fases: [`TONEMAP_BRIEF.md`](TONEMAP_BRIEF.md). Rama `feature/tonemap`.
Los criterios de aceptación (brief §6) se marcan al final de esta sección.

#### Entrar y salir

- Estando en el afinador, **`T` alterna** entre la vista clásica (aguja + nota detectada automáticamente) y la vista
  **ToneMap**. Hoy `T` dentro del afinador no hace nada.
- Al llegar al afinador desde otro modo, se abre **la última vista usada** (persistida en NVS).
- La barra inferior no cambia: sigue mostrando `T:Tuner` resaltado. Las demás teclas globales siguen igual
  (`M`, `G`, `A`, volumen `-`/`=`).
- Al entrar en ToneMap con el drone sonando, el drone se para y el micro arranca (micro y altavoz son
  excluyentes en el ADV). En ToneMap **no hay drone**.

#### Distribución de pantalla (lienzo de contenido 240 × 125)

```
y=0   ┌─────────────────────────────────────────────┐
      │ A4  +3.2c  442.6 Hz        Bb  A=442  ▮▮▮▯ │  franja de info, Font0 (6×8 px por carácter)
y=15  ├─────────────────────────────────────────────┤  línea 1 px gris oscuro
y=17  ├───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┤
      │C7 │C# │D  │D# │E  │F  │F# │G  │G# │A  │A# │B  │  6 filas × 18 px = 108 px (y 17..124)
      │C6 │   │   │   │   │   │   │   │   │   │   │   │  12 columnas × 20 px = 240 px
      │C5 │   │   │   │   │   │   │   │   │   │   │   │  fila de arriba = octava más aguda
      │C4 │   │   │   │▒▒ │   │   │   │   │███│   │   │
      │C3 │   │   │   │   │   │   │   │   │   │   │   │
      │C2 │   │   │   │   │   │   │   │   │   │   │   │
y=125 └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
      (barra de modos de main.cpp, sin cambios)
```

Los píxeles exactos de la franja los ajusta el implementador; la cuadrícula es fija: 20 × 18 px por celda,
C2 abajo a la izquierda, B7 arriba a la derecha. Rango fijo en la v1 (MIDI escrito 36..107).

#### Celdas

- Líneas de cuadrícula 1 px `#374151`. Fondo `#000000`.
- **Etiqueta:** solo el nombre de la nota (`C#`, o `Db` con bemoles); la columna C lleva además la octava
  (`C4`). Font0. Color `#4b5563` mientras la celda no se ha tocado nunca; una vez tocada (`visited`), **negro o
  gris claro `(190,190,190)` según la luminancia relativa del color que muestra la celda** (0,2126 R + 0,7152 G +
  0,0722 B sobre los canales sRGB codificados, 0..1; mayor que 0,5 → negro, si no gris claro), para que se lea sobre
  el casi blanco del centro de la paleta Safe y sobre las celdas oscuras (desvanecidas o poco tocadas). **El blanco
  puro solo lo usa la celda activa** (su etiqueta y su barra, ver abajo); en el resto de la cuadrícula no aparece.
- **Color de fondo** = mezcla entre negro y el color de tendencia según la intensidad (§3.4). El modelo da la
  tendencia con signo `t` en [−1, +1] (+ = alto/sostenido, 0 = centro) y una **paleta** la convierte en color
  (todos los colores a intensidad plena; la mezcla con negro por la intensidad se mantiene):
  - **Safe** (0, por defecto; pensada para daltonismo rojo-verde y azul-amarillo): bajo `rgb(0,100,255)`,
    centro `rgb(200,255,200)` (casi blanco con un toque verde), alto `rgb(255,110,0)`; interpolación lineal en
    RGB centro→alto y centro→bajo. Luminancia estrictamente ordenada centro > alto > bajo, así que también
    funciona en escala de grises.
  - **Extreme** (1; antes "Classic"; ignora el daltonismo, de eso se ocupa Safe): primarios puros lo más separados
    posible en CIELAB (ΔE76 verde–azul 259, azul–rojo 176, verde–rojo 171). Es **un único degradado continuo** sobre
    todo el rango de la celda: extremo bajo = azul puro `(0,0,255)`, justo el centro = verde puro `(0,255,0)`, extremo alto
    = rojo puro `(255,0,0)`, pasando por cian `(0,255,255)` y amarillo `(255,255,0)` a mitad de cada lado (los puntos
    medios de luminancia máxima evitan el marrón). Sin meseta de verde ni saltos: la posición es lineal,
    `t_lin = clamp(e' / (dz·(1 + span)), −1, +1)` con `e'` = error medio × `errScale` (`biasTLinear`; Safe sigue con la
    zona muerta de `biasT`). En cents: azul/rojo pleno en ±18,5 (Relaxed), ±15,5 (Medium) y ±10,1 (Hard), amarillo/cian
    en ±9,3 / ±7,8 / ±5,0. Además **sin atenuar** (una celda con sonido acumulado se pinta con intensidad
    `0,75 + 0,25·display`, nunca por debajo del 75 %; las celdas visitadas pero nunca manchadas siguen negras y las
    desvanecidas se quedan en `0,75 + 0,25·0,45` = 0,8625). Ver `TONEMAP_DESIGN.md` §13.7.
  - **Rampa** (más estrecha que en tonemap.live, decisión tras pruebas en dispositivo): centro puro hasta ±6,2 c
    (Relaxed), ±5,2 c (Medium), ±2,0 c (Hard); color pleno desde ±18,5 c, ±15,5 c y ±10,1 c respectivamente
    (`span` = 2·dz, 2·dz y 4·dz). Intensidad plena a los **250 ms** de sonido acumulado, y el desvanecido baja
    hasta **0,45** de la intensidad base (ToneMap: 500 ms y 0,25). Ver `TONEMAP_DESIGN.md` §13.
- **Celda activa** (la nota que suena ahora, tras pasar las puertas de §3.2). El blanco solo existe aquí y en dos
  sitios: (a) una **barra blanca** de 2 px de alto a lo ancho de la celda por dentro de las líneas de cuadrícula
  (19 px: x..x+18) apoyada sobre el borde inferior (filas y+15 e y+16 de la celda de 20 × 18; la línea de cuadrícula
  inferior es la fila y+17); y (b) la **etiqueta con el nombre de la nota en blanco**. Sin contorno ni sombra. Después,
  una línea de referencia negra de 2 px en el centro vertical de la celda y, la última, la **aguja**: barra roja
  `#ef4444` de 2 px de alto a lo ancho de la celda (queda visible donde cruza la barra blanca). Al dejar de ser activa,
  la etiqueta vuelve a negro / gris claro. Riesgo conocido: sobre el casi blanco del centro de Safe `(200,255,200)` la
  barra y la etiqueta blancas contrastan poco. Posición vertical: arriba = alto.
  Recorrido del 10 % al 90 % de la altura para −50…+50 cents (fórmula en §3.5).

#### Franja de información (fila superior)

- Izquierda: nota detectada y octava escrita (`A4`), cents con signo y un decimal (`+3.2c`), frecuencia real en Hz.
  Los cents se pintan con el **color vivo** de la desviación instantánea de la nota que suena (ver "LED" abajo: la
  paleta activa, Safe o Extreme, aplicada al error de ese fotograma; no al promedio de la celda), para ver de un
  vistazo si estás bajo o alto y hacia dónde corregir. El nombre de la nota sigue en blanco y los Hz en gris. Sin
  nota válida: `--`.
- Fuera de rango (nota escrita < C2 o > B7): en lugar de la nota, `Below range` / `Above range`.
- Derecha: transposición si no es C (`Bb`, `Eb`, `F`), `A=442` (la misma La del afinador clásico) y un
  medidor de nivel del micro (unos 30 px) con una marca en el umbral de la puerta de ruido.
- **Mensajes transitorios** al cambiar un ajuste: durante 1,5 s ocupan el sitio de nota/cents/Hz (la parte
  derecha sigue visible): `Sensitivity: Hard`,
  `History: 30 s`, `Color hold: 2 min`, `Noise gate: High`, `Transpose: Bb`, `Accidentals: flats`,
  `LED: off`, `Palette: Safe` / `Palette: Extreme`, `Map cleared`. En pausa: `Paused (Space)` fijo.

#### Teclas en la vista ToneMap

| Tecla | Acción |
|---|---|
| `T` | Volver a la vista clásica |
| Arriba / Abajo | La de referencia ±1 Hz (la misma `refA` del clásico, 400–480, persistente) |
| `S` | Sensibilidad: Relaxed → Medium → Hard → … |
| `H` | History: 1 s → 3 s → 30 s → Unlimited → … |
| `C` | Color hold: 5 s → 15 s → 30 s → 2 min → 5 min → No fade → … |
| `N` | Puerta de ruido: None → Min → Low → Med → High → … (solo vista ToneMap) |
| `K` | Transposición: C → Bb → Eb → F → … |
| `F` | Sostenidos ↔ bemoles en etiquetas y franja |
| `L` | LED de afinado on/off |
| `P` | Paleta de colores: Safe ↔ Extreme (solo redibuja: no reinicia temporizadores ni toca datos). No es tecla global |
| `Alt` o `Enter` | Abre / cierra el **panel de opciones** (ver abajo) |
| `R` | Borrar el mapa (todas las celdas) |
| `Space` | Pausar / reanudar la escucha (el mapa se congela; en pausa no se pinta nada) |
| Izquierda / Derecha | **Afinación** (temperamento): Equal ↔ Ligeti Hora lunga (ver "Temperamentos"). Cambiarla **borra el mapa** |

**Panel de opciones** (`Alt` o `Enter`, alterna): superposición opaca a pantalla completa (240 × 125, Font0, 13 líneas de
9 px) con el título `OPTIONS` y `Alt/Enter: close`, y una línea por tecla con su **valor actual**: `S Sensitivity`,
`H History`, `C Color hold`, `N Noise gate`, `K Transpose`, `F Accidentals`, `P Palette`, `L LED` (con los mismos textos
que los mensajes transitorios) y, sin valor, `R Clear map`, `Space Pause` / `Space Resume` (según el estado),
`Up/Down  A=442 (reference)` y `T Classic view` (esta última comparte línea con `R Clear map`), más `</> Tuning` con la afinación actual. Mientras está abierto, las teclas del mapa siguen funcionando y los valores
se actualizan al instante; la captura, el análisis, el LED y el modelo siguen corriendo debajo, y al cerrar se redibuja el
mapa entero. Solo se redibuja al pulsar una tecla (sin coste por fotograma). Siempre está cerrado al entrar en la vista;
`T`, `M`, `G`, `A` funcionan como siempre y lo cierran al salir. `Alt` solo lo recibe la vista ToneMap (`main.cpp` no se lo
pasa a ningún otro modo). **Sin verificar en el dispositivo** que el teclado informe `Alt` solo; `Enter` es el recurso seguro.
En la franja de info, a la izquierda de `A=…` (y de la transposición), aparece la pista `Alt:menu` en gris tenue, o `Alt` si no
cabe entero, o nada si choca con el bloque nota / cents / Hz o con un mensaje.

Las teclas globales (`M`, `G`, `A`, `-`, `=`) las sigue capturando `main.cpp` antes que el modo: no usar esas letras.

#### Ajustes, valores por defecto y persistencia

Todos se guardan en NVS con el mecanismo diferido que ya existe (`settings::touch()`).

| Ajuste | Valores | Defecto | Clave NVS (≤15 car.) |
|---|---|---|---|
| Vista del afinador | classic / tonemap | classic | `tunerView` |
| Sensibilidad | relaxed / medium / hard | medium | `tmSens` |
| History | 1 s / 3 s / 30 s / unlimited | 3 s | `tmHist` |
| Color hold | 5 s / 15 s / 30 s / 120 s / 300 s / no fade | 30 s | `tmHold` |
| Puerta de ruido (solo ToneMap) | none (0) / min (30) / low (80) / med (200) / high (450), rms int16 | low | `tmGate` (clave NVS `tmGateB`) |

**Migración de la puerta:** hasta ahora la clave NVS `tmGate` guardaba la escala antigua de 4 niveles (0 none, 1 low, 2 med,
3 high). La escala actual de 5 niveles se guarda en `tmGateB`. Al cargar, si `tmGateB` no existe y sí `tmGate`, se convierte
{0, 1, 2, 3} → {0, 2, 3, 4} (`settings::migrateGate`; un valor fuera de rango da Low); después solo se usa `tmGateB`. Sin ninguna
de las dos claves, el valor por defecto es Low (2). Los mensajes son `Noise gate: None` / `Min` / `Low` / `Med` / `High`.
El nivel `Min` (30) está pensado para tocar con sordina (ver `TONEMAP_DESIGN.md` §16).
| Transposición | C (0) / Bb (−2) / Eb (+3) / F (+5) | C | `tmTransp` |
| Alteraciones | sharps / flats | sharps | `tmFlats` |
| LED de afinado | on / off | on | `tmLed` |
| Paleta | safe / extreme | safe | `tmPal` (0 = Safe, 1 = Extreme) |
| Afinación (temperamento) | equal (0) / Ligeti Hora lunga (1) | equal | `tmTemper` |
| La de referencia | 400–480 Hz | 442 (ya existe) | `refA` (ya existe) |

#### Temperamentos (afinación)

Solo en la vista ToneMap (el afinador clásico sigue siempre en temperamento igual). Un temperamento es una tabla de 12
offsets en cents respecto al temperamento igual, por clase de nota de **concierto** (Do = 0, todas las octavas), aplicada
encima de la La de referencia (`tonemap::kTemperaments`, `latticeOffsetCents`). El "centro" de cada celda (línea negra de
referencia) y la aguja pasan a medirse contra la nota ya desplazada, y la búsqueda de la nota más próxima también.
Las notas desplazadas llevan una **flecha pequeña hacia abajo** en su etiqueta (4, 6 u 8 px según el desplazamiento:
< 20, < 40, ≥ 40 cents). Cambiar de temperamento borra el mapa (los errores guardados eran contra otros objetivos).

- **Equal**: todo a 0.
- **Ligeti Hora lunga**: Sonata para viola sola (1994), 1.er movimiento. Nota al pie de la partitura: tres flechas hacia abajo
  imitan la serie armónica de un Fa imaginario (la cuerda de Do sirve de modelo): **Si −49** (armónico 11), **Mi♭ −31**
  (armónico 7) y **La −14** (armónico 5, la diferencia entre la tercera mayor justa y la templada). Las demás notas, afinación
  normal. No es una escala histórica con nombre propio (búsqueda hecha): es un esquema de este movimiento. Valores = los de la
  partitura (los teóricos son −48,7 / −31,2 / −13,7).

Pendiente: más temperamentos y uno definido por el usuario (tabla de 12 offsets editable); el centro tonal queda fijo.

Los valores de transposición son los de ToneMap (semitonos; nota escrita = nota de concierto − valor, ver §3.2).
Añadir A (clarinete en La, −3) es trivial si Sergio lo pide más adelante.

#### El mapa de calor

- Comportamiento = ToneMap (§3). El mapa vive en RAM **mientras el dispositivo esté encendido**: sobrevive a
  cambiar de modo o de vista y volver; se borra con `R` o al apagar. No se guarda en flash ni en SD.
- Cambiar la La **re-puntúa** el historial (los colores se recalculan contra la nueva referencia), como en ToneMap.
- Cambiar la transposición **desplaza** el mapa (la misma nota de concierto cae en otra celda escrita), como en
  ToneMap. El mapa guarda las notas por MIDI de concierto 0..127, así que al transponer no se pierde nada: solo cambia qué ventana (C2–B7 escritas) se muestra.
- Cambiar sensibilidad o History recalcula los colores de todas las celdas desde el historial guardado.

#### LED

- Con `LED` on: el LED RGB (GPIO 21, `neopixelWrite`) muestra de forma continua el **color vivo** de la nota activa: el
  color de su desviación **instantánea** (el `errRatio` bruto del fotograma, no el promedio ponderado de la celda ni la
  aguja suavizada de Relaxed) en la **paleta activa**, a intensidad plena (sin atenuar ni mezclar con negro). Safe:
  `biasT` (con su zona muerta: dentro, el color de centro `(200,255,200)`) → `paletteColor(0, t)`; Extreme:
  `biasTLinear` (degradado continuo, 0 exacto = verde puro) → `paletteColor(1, t)`. Es la regla que sustituye al antiguo
  "verde solo dentro de la zona muerta". Se apaga si no hay nota, en pausa, con `L` off y al salir.
- Para no inundar el LED, el color nuevo se envía al empezar la nota y después solo cuando algún canal cambia 16 niveles
  o más y como mucho cada 50 ms; cada envío se repite dos veces (a los 6 y 12 ms) como antes.
- Restricción de hardware ya conocida (ver SPEC, metrónomo): el LED se alimenta del mismo pin que la
  retroiluminación y **solo funciona con la pantalla a brillo 255**. Con LED on, la vista ToneMap guarda el brillo
  del usuario, pone 255 y lo restaura al salir de la vista o del modo, o al poner LED off.
  Repetir cada orden del LED dos veces (a los 6 y 12 ms), como hace el metrónomo.
- El código del LED del metrónomo no se toca. Los primitivos del LED y del brillo (enviar con repeticiones, apagado
  bloqueante, subir/restaurar brillo) viven en `tuner_led.*`, compartidos con la vista clásica del afinador.

#### Fuera de alcance en la v1

Más temperamentos (hay Equal y Ligeti Hora lunga, ver arriba) y centro tonal, stretch Railsback, Note View (Ribbons/Balloons), grabador y WAV, editor de paleta,
AGC, estadísticas, guardar el mapa en SD, cambiar el rango visible. La arquitectura no debe impedir añadir
temperamentos después (columnas de ancho variable): la conversión frecuencia → nota debe pasar por una única
función "offset de destino por nota", como en ToneMap.

#### Criterios de aceptación

Ver brief §6. Estado (2026-10-23):

**A. Bench (serie)**
- [x] Paridad de notas con el fixture de ToneMap: 264/264 filas `PASS` (más 192 filas extra propias).
- [~] Sintético: sin errores de octava de C2 a B7 a 32 kHz. Error ≤ 1 c de C2 a C6 con senoidal; con diente de sierra C6 +20 c da +1,39 c.
      Octava 7: senoidal ≤ 1,65 c; diente de sierra hasta +8,7 c → **"best effort" documentado** (TONEMAP_DESIGN §12).
- [x] Análisis completo en < 20 ms (9,25 ms medido). ≥ 25 análisis/s con la pantalla dibujando: **31,2/s de media** (mín. 29,8)
      con análisis y dibujo reales durante 75 s tocando viola (entorno `cardputer-adv-tonemap-debug`, puerta de ruido en None);
      9,6 ms/análisis, 3,3 ms de dibujo, 12,1 ms de envío, ~15,6 fps, `starved=0`, `torn=0`, heap mínimo 216 KB.
- [x] Pruebas de color, historia, desvanecido, re-basado, transposición y color en vivo: `PASS` (14 series en el dispositivo).

**B. En el dispositivo (Sergio)**: pendiente de la PARADA C (lista en brief §6 B, más: menú Alt/Enter, paletas `P`, color en vivo de cents y LED,
afinador clásico automático, metrónomo blanco/verde con clics a 1400 Hz).

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
