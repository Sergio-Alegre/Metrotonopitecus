# Implementation brief — vista ToneMap en el afinador

Fecha: 2026-10-16. Decidido con Sergio en conversación (tres rondas de preguntas).
Referencia portada: [crossrhythm/tonemap.live](https://github.com/crossrhythm/tonemap.live), commit `bd19db1`,
licencia MIT © 2025 Jeremy Black. Detector de referencia: `pitchy@4.0.1` (npm), MIT.

Este documento es para **Claude Code en el ordenador de Sergio**. Contiene la especificación cerrada (§2), la
lógica de ToneMap ya extraída (§3), la adaptación al ESP32-S3 (§4), el plan por fases con el agente de cada
fase y las paradas para probar en el dispositivo (§5), y los criterios de aceptación (§6).

---

## 0. Cómo ejecutar este brief (instrucciones para Claude Code)

1. Lee `CLAUDE.md` y `docs/SPEC.md` antes de nada. Sus reglas mandan sobre este brief si chocan
   (sobre todo: **no flashear por USB sin permiso**, commit + push tras cada compilación correcta, todo el texto
   del firmware en inglés, interfaz silenciosa).
2. Trabaja en la rama `feature/tonemap`, que ya existe en GitHub y contiene este brief
   (`git fetch origin && git checkout feature/tonemap`). **No fusiones a `main` sin que Sergio lo diga.**
3. Ejecuta las fases de §5 en orden. Cada fase indica qué agente lanzar, qué le pasas y qué debe devolver.
   La sesión principal es el **orquestador**: lanza agentes, revisa lo que devuelven, compila, hace commit.
4. En cada **PARADA** te detienes: compilas, dejas `firmware.bin` listo, dices a Sergio exactamente qué probar
   y qué mirar en serie, y esperas su respuesta. Si Sergio pide flashear y el Cardputer está conectado,
   flasheas según `CLAUDE.md`.
5. **Alcance:** solo se toca lo necesario para la vista ToneMap: `mode_tuner.cpp`, ficheros nuevos, `settings.*`,
   una línea de `main.cpp` y un método vacío en `ui.h` (§4.7). El metrónomo, el juego, About, `pitch.*` y
   `audio.*` no se modifican (salvo que la fase 2 demuestre que es imprescindible; entonces se pregunta).
6. Si algo de este brief resulta imposible o ambiguo en el código real, **pregunta a Sergio** en vez de decidir
   un cambio de comportamiento por tu cuenta. Las decisiones técnicas internas (estructuras, nombres) son tuyas.

---

## 1. Qué se construye

Una segunda vista dentro del afinador (`T`), inspirada en tonemap.live: una cuadrícula de 12 columnas (notas) ×
6 filas (octavas C2–B7) donde cada nota que tocas tiñe su celda según tu **tendencia** de afinación acumulada
(naranja = tiendes a alto, verde = centrado, azul = tiendes a bajo). La celda que suena lleva borde blanco y una
aguja de cents. Es un mapa de calor de intonación para practicar, no un afinador de nota objetivo.

Uso principal de Sergio: **cuerda / viento agudo** (violín, viola, flauta, clarinete…).

---

## 2. Especificación funcional (copiar a `docs/SPEC.md` en la fase 0)

### 2.1 Entrar y salir

- Estando en el afinador, **`T` alterna** entre la vista clásica (aguja + nota objetivo, la de ahora) y la vista
  **ToneMap**. Hoy `T` dentro del afinador no hace nada.
- Al llegar al afinador desde otro modo, se abre **la última vista usada** (persistida en NVS).
- La barra inferior no cambia: sigue mostrando `T:Tuner` resaltado. Las demás teclas globales siguen igual
  (`M`, `G`, `A`, volumen `-`/`=`).
- Al entrar en ToneMap con el drone sonando, el drone se para y el micro arranca (micro y altavoz son
  excluyentes en el ADV). En ToneMap **no hay drone**.

### 2.2 Distribución de pantalla (lienzo de contenido 240 × 125)

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

### 2.3 Celdas

- Líneas de cuadrícula 1 px `#374151`. Fondo `#000000`.
- **Etiqueta:** solo el nombre de la nota (`C#`, o `Db` con bemoles); la columna C lleva además la octava
  (`C4`). Font0, color `#4b5563` mientras la celda no se ha tocado nunca, blanco cuando ya se ha tocado
  (`visited`).
- **Color de fondo** = mezcla entre negro y el color de tendencia según la intensidad (§3.4).
  Paleta fija (la de ToneMap): alto `#FFAA00`, centro `rgb(34,197,94)`, bajo `#0056D6`.
  *(Sustituido el 2026-10-22: dos paletas, Safe y Extreme (antes Classic), tecla `P`; ver la nota "Cambios tras pruebas en
  dispositivo" al final y `SPEC.md`.)*
- **Celda activa** (la nota que suena ahora, tras pasar las puertas de §3.2): borde blanco interior de 2 px
  *(sustituido el 2026-10-22 por cuatro esquinas en L y después, el mismo día, por una barra blanca inferior + etiqueta blanca; ver `SPEC.md`)*,
  una línea de referencia negra de 2 px en el centro vertical de la celda y la **aguja**: barra roja
  `#ef4444` de 2 px de alto a lo ancho de la celda. Posición vertical: arriba = alto. Recorrido del 10 % al
  90 % de la altura para −50…+50 cents (fórmula en §3.5).

### 2.4 Franja de información (fila superior)

- Izquierda: nota detectada y octava escrita (`A4`), cents con signo y un decimal (`+3.2c`), frecuencia real en Hz.
  Los cents se pintan en **verde** si estás dentro de la zona muerta del modo (§3.3), blancos si no. Sin nota
  válida: `--`.
- Fuera de rango (nota escrita < C2 o > B7): en lugar de la nota, `Below range` / `Above range`.
- Derecha: transposición si no es C (`Bb`, `Eb`, `F`), `A=442` (la misma La del afinador clásico) y un
  medidor de nivel del micro (unos 30 px) con una marca en el umbral de la puerta de ruido.
- **Mensajes transitorios** al cambiar un ajuste: durante 1,5 s ocupan el sitio de nota/cents/Hz (la parte
  derecha sigue visible): `Sensitivity: Hard`,
  `History: 30 s`, `Color hold: 2 min`, `Noise gate: High`, `Transpose: Bb`, `Accidentals: flats`,
  `LED: off`, `Map cleared`. En pausa: `Paused (Space)` fijo.

### 2.5 Teclas en la vista ToneMap

| Tecla | Acción |
|---|---|
| `T` | Volver a la vista clásica |
| Arriba / Abajo | La de referencia ±1 Hz (la misma `refA` del clásico, 400–480, persistente) |
| `S` | Sensibilidad: Relaxed → Medium → Hard → … |
| `H` | History: 1 s → 3 s → 30 s → Unlimited → … |
| `C` | Color hold: 5 s → 15 s → 30 s → 2 min → 5 min → No fade → … |
| `N` | Puerta de ruido: None → Low → Med → High → … |
| `K` | Transposición: C → Bb → Eb → F → … |
| `F` | Sostenidos ↔ bemoles en etiquetas y franja |
| `L` | LED de afinado on/off |
| `P` | Paleta Safe ↔ Extreme *(añadida el 2026-10-22; antes "Classic")* |
| `R` | Borrar el mapa (todas las celdas) |
| `Space` | Pausar / reanudar la escucha (el mapa se congela; en pausa no se pinta nada) |
| Izquierda / Derecha | Sin uso en la v1 (reservadas para temperamentos en el futuro) |

Las teclas globales (`M`, `G`, `A`, `-`, `=`) las sigue capturando `main.cpp` antes que el modo: no usar esas letras.

### 2.6 Ajustes, valores por defecto y persistencia

Todos se guardan en NVS con el mecanismo diferido que ya existe (`settings::touch()`).

| Ajuste | Valores | Defecto | Clave NVS (≤15 car.) |
|---|---|---|---|
| Vista del afinador | classic / tonemap | classic | `tunerView` |
| Sensibilidad | relaxed / medium / hard | medium | `tmSens` |
| History | 1 s / 3 s / 30 s / unlimited | 3 s | `tmHist` |
| Color hold | 5 s / 15 s / 30 s / 120 s / 300 s / no fade | 30 s | `tmHold` |
| Puerta de ruido | none / low / med / high | low | `tmGate` |
| Transposición | C (0) / Bb (−2) / Eb (+3) / F (+5) | C | `tmTransp` |
| Alteraciones | sharps / flats | sharps | `tmFlats` |
| LED de afinado | on / off | on | `tmLed` |
| Paleta *(añadida el 2026-10-22)* | safe / extreme | safe | `tmPal` |
| La de referencia | 400–480 Hz | 442 (ya existe) | `refA` (ya existe) |

Los valores de transposición son los de ToneMap (semitonos; nota escrita = nota de concierto − valor, ver §3.2).
Añadir A (clarinete en La, −3) es trivial si Sergio lo pide más adelante.

### 2.7 El mapa de calor

- Comportamiento = ToneMap (§3). El mapa vive en RAM **mientras el dispositivo esté encendido**: sobrevive a
  cambiar de modo o de vista y volver; se borra con `R` o al apagar. No se guarda en flash ni en SD.
- Cambiar la La **re-puntúa** el historial (los colores se recalculan contra la nueva referencia), como en ToneMap.
- Cambiar la transposición **desplaza** el mapa (la misma nota de concierto cae en otra celda escrita), como en
  ToneMap. Lo que se salga del rango almacenado se pierde.
- Cambiar sensibilidad o History recalcula los colores de todas las celdas desde el historial guardado.

### 2.8 LED

- Con `LED` on: el LED RGB (GPIO 21, `neopixelWrite`) se enciende **verde** mientras la nota activa está dentro
  de la zona muerta del modo, y se apaga en cuanto sale, no hay nota o se pausa.
- Restricción de hardware ya conocida (ver SPEC, metrónomo): el LED se alimenta del mismo pin que la
  retroiluminación y **solo funciona con la pantalla a brillo 255**. Con LED on, la vista ToneMap guarda el brillo
  del usuario, pone 255 y lo restaura al salir de la vista o del modo, o al poner LED off.
  Repetir cada orden del LED dos veces (a los 6 y 12 ms), como hace el metrónomo.
- No refactorizar el código del LED del metrónomo; duplicar el mínimo necesario en el código de ToneMap.

### 2.9 Fuera de alcance en la v1

Temperamentos y centro tonal, stretch Railsback, Note View (Ribbons/Balloons), grabador y WAV, editor de paleta,
AGC, estadísticas, guardar el mapa en SD, cambiar el rango visible. La arquitectura no debe impedir añadir
temperamentos después (columnas de ancho variable): la conversión frecuencia → nota debe pasar por una única
función "offset de destino por nota", como en ToneMap.

---

## 3. Referencia técnica extraída de ToneMap

Todo esto está en `index.html` de tonemap.live (JS en línea, ~9000 líneas). Los números de línea cambian:
**busca por nombre de función**. Fase 1 verifica este resumen contra el original.

### 3.1 Detector (pitchy 4.0.1, `PitchDetector.findPitch`)

McLeod Pitch Method sobre una ventana de 2048 muestras (Web Audio, 44,1/48 kHz, `fftSize 2048`,
`smoothingTimeConstant 0`), ejecutado en cada `requestAnimationFrame` (~60 Hz, ventanas solapadas).

1. Autocorrelación r'(τ) para τ = 0..N−1 (pitchy la hace por FFT; el resultado es el mismo que la suma directa).
2. NSDF tipo II: `n(τ) = 2·r'(τ) / m'(τ)`, con `m'(0) = 2·r'(0)` y
   `m'(τ+1) = m'(τ) − x[τ]² − x[N−1−τ]²`. Si m' ≤ 0, el resto de la NSDF vale 0.
3. **Máximos clave**: se recorre la NSDF; tras un cruce por cero ascendente se busca el máximo hasta el
   siguiente cruce descendente; ese máximo es un máximo clave.
4. `nMax` = mayor de los máximos clave. Resultado = **el primer máximo clave ≥ 0,9 · nMax** (K = 0,9).
5. Refinamiento parabólico con los dos vecinos; `freq = sampleRate / τ_refinado`,
   `clarity = min(1, valor de la parábola en el máximo)`.

El afinador clásico de Metrotonopitecus (`pitch.cpp`) ya calcula la misma NSDF, pero solo en ±2 semitonos de la
nota objetivo y eligiendo el pico mayor. ToneMap necesita el rango completo y la selección por K = 0,9 (es lo que
evita errores de octava).

### 3.2 Puertas y conversión a nota (`tick()`, `freqToNoteData()`)

- Frame válido si `clarity > 0.8`, `14 < freq < 8200` y `rms ≥ umbral de ruido` (`loudEnough`).
- Transposición: `adjustedPitch = pitch · 2^(−transposition/12)`. Con Bb (−2) la nota escrita sale 2 semitonos
  por encima de la de concierto. Todo lo demás (celdas, etiquetas) trabaja en **notas escritas**.
- En temperamento igual sin stretch:
  `midi = round(12·log2(f/A4)) + 69`, `targetFreq = A4 · 2^((midi−69)/12)`,
  `cents = 1200·log2(f/targetFreq)`, `errRatio = (f − targetFreq)/targetFreq`.
  En ToneMap el destino se construye como `440 · 2^((midi−69)/12) · 2^(targetOffsetCents/1200)` con
  `targetOffsetCents = latticeOffset(midi) + 1200·log2(A4/440)` (en la v1, `latticeOffset = 0`). Es
  algebraicamente lo mismo; **conserva esta forma** (una función `targetOffsetCents(midi)`) para poder añadir
  temperamentos y para el re-puntuado de §3.4.
- `onset filter` (attackMs) por defecto es 0 en ToneMap: la nota se acepta en el primer frame. En la v1 no hay
  onset filter.

### 3.3 Modos de sensibilidad (constantes)

| Modo | `DEADZONE_RATIO` | `ERR_SCALE` | `NEEDLE_TAU_MS` | Zona muerta (aguja/LED) | Verde puro en color hasta | Color saturado desde |
|---|---|---|---|---|---|---|
| Relaxed | 0.0025 | 0.7 | 350 | ±4,3 c | ±6,2 c | ±31 c (span = 4·dz) |
| Medium | 0.0021 | 0.7 | 80 | ±3,6 c | ±5,2 c | ±26 c (span = 4·dz) |
| Hard | 0.00105 | 0.9 | 30 | ±1,8 c | ±2,0 c | ±18 c (span = **8**·dz) |

- **Zona muerta** (texto verde de cents, LED): `|errRatio| ≤ DEADZONE_RATIO` (sin escalar).
- **Color de tendencia** (`biasRelaxed/biasMedium/biasHard`): `e = avgErrRatio · ERR_SCALE`; si `|e| ≤ dz` →
  color centro; si no, `t = clamp((|e| − dz) / span, 0, 1)` y se interpola linealmente centro → alto (e > 0) o
  centro → bajo (e < 0). `span = 4·dz` en Relaxed y Medium, `8·dz` en Hard.
- **Aguja**: los cents mostrados se suavizan con una EMA `alpha = 1 − exp(−dt/τ)` con τ del modo. La EMA se
  reinicia cuando cambia la nota o hay silencio.

### 3.4 Historia, intensidad y desvanecido (`finalizeActiveSegment`, `getRecentWeightedErr`, `updateCellDisplayColor`)

Estado por celda (`createEmptyCellState`): `totalVoiceMs`, `segments[]`, `biasRGB`, `baseIntensity`,
`displayIntensity`, `lastStainAtMs`, `fadeAnchorIntensity`, `visited`.

- Con attackMs = 0, **cada frame válido produce un segmento** `{startMs, durMs = dt, avgErrRatio = errRatio,
  targetOffsetCents}` en la celda de su nota, y re-tiñe la celda en ese mismo frame.
- `totalVoiceMs += durMs` (acumulado desde el último reset, sin ventana).
  `baseIntensity = min(1, totalVoiceMs / 500)` (`FULL_CONF_MS = 500`).
- **Color**: media de `errRatio` **ponderada por duración** de los últimos `History` segundos **de sonido**
  (se recorren los segmentos hacia atrás sumando duraciones hasta cubrir la ventana; el silencio no cuenta).
  Cada segmento se re-basa antes de promediar:
  `rebase(err, from, to) = 2^((from − to)/1200) · (1 + err) − 1`, con `from` = su `targetOffsetCents` y
  `to` = el actual de esa nota. Así, cambiar la La re-puntúa el historial. Unlimited = todos los segmentos.
  El color resultante pasa por el `bias*` del modo (§3.3) → `biasRGB`.
- **Visualización** cada frame (`updateCellDisplayColor`, con `STAIN_MIN_FRACTION = 0.25`,
  `STAIN_FADE_SECONDS = 10`, `STAIN_FADE_IN_MS = 125`):
  - Medium y Hard: `display = baseIntensity`.
  - Relaxed: mientras se tiñe, `display` se acerca a `baseIntensity` con EMA de 125 ms; si no, se mantiene.
  - Desvanecido: hasta `holdSeconds` desde el último tinte, nada. Después, rampa lineal en 10 s desde el valor
    que tenía hasta `0.25 · baseIntensity`, y ahí se queda (no llega a 0 hasta un reset). No fade = sin rampa.
  - `colorFinal = lerp(fondo, biasRGB, display)` por canal (fondo negro).
- Cambiar sensibilidad o History → `recomputeAllCellColorsAndRender()`: recalcula `biasRGB` e intensidad de todas
  las celdas desde sus segmentos.
- Cambiar transposición → `shiftCellStateBySemitones(old − new)`: el estado de cada nota se mueve a otra celda.
- ToneMap guarda hasta 2000 segmentos por nota durante `max(History, ColorHold) + 5 s` (máx. 10 min). **No hay que
  copiar eso** en el ESP32: ver §4.4.

### 3.5 Celda activa y aguja (`highlightGridCell`, `getNeedleCellPosition`)

- En temperamento igual: `pct = (clamp(cents, −50, 50) + 50) / 100` (0 = borde grave, 1 = borde agudo).
- Aguja horizontal (la de por defecto en ToneMap): `top = (90 − pct·80) %` de la altura de la celda.
  Línea de referencia negra en `targetPct = 0.5` → 50 %.
- La celda activa se marca `visited`.

### 3.6 Correcciones verificadas contra el código fuente (fase 1, 2026-10-16)

Fase 1 comparó §3 con `tonemap.live/index.html` (bd19db1) y `pitchy@4.0.1`. Constantes, fórmulas de color, re-basado,
desvanecido, aguja y algoritmo MPM coinciden. Lo que sigue **corrige o completa** lo anterior y manda sobre ello.
Números de línea de `index.html` entre paréntesis.

**Correcciones**

- **Aguja (§3.3, último punto):** con `attackMs = 0` la EMA de la aguja solo actúa en **Relaxed** (12083-12135:
  en Medium y Hard `relaxedSmoothingActive` se pone a false en cada frame y el siguiente se resiembra con los
  cents crudos). **Decisión: se replica ToneMap tal cual**: Medium y Hard muestran los cents crudos del frame;
  Relaxed usa EMA con τ = 350 ms. Se reinicia al cambiar de nota o en un frame inválido.
- **Segmentos (§3.4, primer punto):** el **primer frame válido de cada nota** (tras silencio o cambio de nota) **no
  produce segmento** (12130-12132, 10500). El segmento de un frame lleva `dtMs` = tiempo desde el tick anterior,
  sin tope; si `dtMs ≤ 0` no hay segmento. El `avgErrRatio` del segmento es el `errRatio` crudo del frame.
- **Zona muerta (§3.3, §2.8):** la regla `|errRatio| ≤ DEADZONE_RATIO` (cruda, por frame) es correcta, pero en
  ToneMap solo resalta un texto de depuración. El texto verde de cents y el LED son diseño de Metrotonopitecus.
- **Retención (§3.4):** ToneMap poda por tiempo de reloj (`max(History, Hold)·1000 + 5000` ms, tope 10 min) y a
  2000 segmentos por nota (≈33 s de sonido a 60 seg/s). No hay que copiarlo (ver §4.4).

**Omisiones**

- **Puertas:** frame válido = `hasSignalEnergy && clarity > 0.8 && 14 < pitch < 8200`, con
  `hasSignalEnergy = rms > 1e-7 || peakAbs > 4e-6`; las cotas de Hz se aplican al pitch **sin transponer**.
  La puerta de ruido de ToneMap es RMS en escala ±1.0: `[0, 0.025, 0.05, 0.10, 0.25, 0.50]` (None/Min/Low/Med/High/Max,
  defecto 0.025). Los valores int16 de §4.3 son la adaptación del Cardputer, a calibrar en la PARADA A.
- **Un solo frame inválido termina la nota** (12157-12164): se cierra el segmento, se limpia la nota activa y se
  quita la aguja. Sin histéresis. Esto reinicia la EMA y hace que el siguiente frame válido no genere segmento.
- **Pitchy:** `findPitch` devuelve `[0, 0]` sin máximos clave. El bucle de máximos clave va de i = 1 a N−2;
  `NSDF[0] = 1` nunca cuenta como cruce ascendente; el máximo se siembra con la muestra del cruce y solo se
  registra en el siguiente cruce descendente (un lóbulo final sin cerrar se descarta); cruces con `≤ 0` y `> 0`.
  La NSDF cubre τ = 0..N−1 sin límite superior (pitchy usa FFT en Float32: en la cola de τ grandes, donde `m'` es
  pequeño, la suma directa puede diferir ligeramente; no es un fallo).
- **`freqToNoteData`:** parte de `round(semitonos desde A4) + 69` y prueba ±2 candidatos (0..127) minimizando
  `|(pos − cand)·100 − offset(cand)|`. En temperamento igual sin stretch equivale a `round()`, salvo en el empate
  exacto .5 (ToneMap elige el inferior, `Math.round` el superior). Irrelevante en la práctica.
  `getTargetOffsetCents = getLatticeOffsetCents + getA4OffsetCents`, con `getA4OffsetCents = 1200·log2(A4/440)`.
  En un futuro con stretch, éste se indexa por la nota de concierto y el temperamento por la escrita.
- **Color (`updateCellDisplayColor`):** se mezcla desde `--grid-bg` (negro en tema oscuro) con canales redondeados;
  una celda se pinta solo si `displayIntensity > 0.001`. Si `baseIntensity ≤ 0` o la celda nunca se tiñó
  (`lastStainAtMs = 0`), display = 0. En Relaxed el acercamiento `alpha = 1 − exp(−Δms/125)` usa el tiempo desde la
  última actualización de esa celda y solo se aplica en el frame en que se tiñó.
- **`getRecentWeightedErr`:** el segmento del borde de la ventana entra parcialmente (`take = min(durMs, restante)`);
  devuelve 0 si la duración total es 0.
- **`recomputeAllCellColorsAndRender`** (al cambiar History, sensibilidad, La o transposición) pone
  `lastStainAtMs = ahora` y `fadeAnchorIntensity = null` en **todas** las celdas: el temporizador de hold/desvanecido
  se reinicia. En Relaxed no tira de la intensidad hacia la base (solo Medium/Hard saltan). Cambiar solo el Color hold
  re-renderiza sin reiniciar nada.
- **Transposición:** orden de ToneMap = cerrar segmento, desplazar el estado por `old − new`, limpiar la nota activa,
  quitar la aguja. El desplazamiento descarta lo que cae fuera de MIDI 0..127; los `targetOffsetCents` guardados no
  se reescriben (se re-basan al recalcular).
- **Aguja:** los cents que se pintan son los suavizados (`displayCents`); `highlightGridCell` se llama dos veces por tick.
- **Fixture (fase 4):** `tests/fixtures/temperament-vectors.json` es un objeto con `rows[]` (5192), no un array.
  Las 264 filas `equal`/`none` son solo **44 vectores distintos** repetidos 6 veces (`pitchCenter`/`anchor` no
  influyen): midi 33, 45, 57, 60, 64, 67, 69, 72, 81, 93, 105 × cents {0, 30} × A4 {440, 441}, `transposition` = 0.
  No cubren cents negativos, recorte a ±50 ni redondeos en el límite. **La fase 4 debe añadir esos casos propios**
  (cents negativos, ±50, ±49,9, empate de redondeo, transposición, extremos MIDI) además de las 264 filas.
  `expect.needleEven = [pct, targetPct]`; el brief usa `[0]`.

---

## 4. Adaptación al ESP32-S3 (Cardputer ADV)

### 4.1 Captura continua con solape

- El clásico graba 2048 muestras, procesa y vuelve a grabar, con huecos y unas 8 lecturas por segundo. ToneMap
  necesita captura **continua** y unas **25–30 lecturas por segundo**.
- M5Unified 0.2.25 `Mic_Class` admite **dos grabaciones en cola** (`_rec_info[2]`; `isRecording()` devuelve
  0, 1 o 2; `record()` falla si la cola está llena). Esquema: trozos de `HOP` muestras en búferes distintos, dos
  siempre en cola; cuando uno termina, se copia a un anillo de `WINDOW` muestras y se encola el siguiente. Se
  analiza siempre **la ventana más reciente**; si el análisis va retrasado, se saltan ventanas (nunca se encolan
  análisis).
- Punto de partida: `WINDOW = 2048`, `HOP = WINDOW / 2`. La fase 2 lo confirma leyendo el código de M5Unified.
- **Frecuencia de muestreo: 16 kHz o 32 kHz, decide la fase 3 midiendo.**
  - A 16 kHz, B7 (3951 Hz) tiene un periodo de ~4 muestras y la precisión en la octava 7 será mala.
  - A 32 kHz el periodo de B7 es ~8 muestras, y C2 (65,4 Hz) ≈ 489 de retardo, que cabe con `WINDOW = 2048`.
  - 32 kHz es la apuesta inicial por el uso de Sergio (registro agudo). Hay que medir el error en cents en C2,
    C4, C6, C7 y B7 y el tiempo por análisis.
  - Si la octava 7 no da ±3 c ni a 32 kHz, se acepta y se documenta como "best effort".

### 4.2 Detector de rango completo

- Fichero nuevo, sin dependencias de Arduino (como `pitch.cpp`): implementación de §3.1 en `float` (el FPU del S3
  es de precisión simple; nada de `double`).
- Suma directa (O(N·τmax)) con el denominador incremental de pitchy. Limitar τ a `[sampleRate/4200, τmax]` con
  τmax para ~62 Hz. Estimación: 10–20 ms por análisis; **medirlo**.
- Si no cabe en tiempo, opciones por orden: (a) bajar `WINDOW` a 1536, (b) diezmar a 16 kHz para τ grandes,
  (c) autocorrelación por FFT con `esp-dsp`. La (c) es **una dependencia nueva: preguntar a Sergio antes**.
- No tocar `pitch.cpp`: el afinador clásico sigue con su detector. (Nota 2026-10-24: el clásico acabó usando el detector de ToneMap y `pitch.cpp/.h` se borró.)

### 4.3 Umbrales de micro

- Las amplitudes del Cardputer no son las de un navegador. El clásico usa `kMinRms = 120` (int16) y
  `kMinClarity = 0.75`.
- ToneMap usa `clarity > 0.8`. Arrancar con 0.8 y ajustarlo en la PARADA A.
- Puerta de ruido (RMS en unidades int16, a calibrar en la PARADA A): `None = 0` (solo puerta de claridad),
  `Low = 120` (la del clásico, probada), `Med ≈ 300`, `High ≈ 800`.

### 4.4 Memoria del historial (no hay que replicar 2000 segmentos por nota)

- La media ponderada y el re-basado son **afines** en `err`. Por eso se pueden **fusionar frames consecutivos**
  en trozos sin cambiar el resultado (salvo el borde de la ventana, con un error ≤ la duración de un trozo).
- Propuesta (la fase 2 la cierra):
  - Por nota guardar `totalVoiceMs`, la suma acumulada para Unlimited, un anillo **fino** (trozos de ≤ 100 ms,
    cubre ≥ 3,2 s para History 1 s y 3 s) y un anillo **grueso** (trozos de ≤ 1 s, cubre ≥ 30 s).
  - Cada trozo lleva `durMs` y `err` (con int16 en centésimas de cent basta).
  - Al cambiar la La, re-basar en el sitio todos los trozos guardados (equivale al re-basado por segmento de
    ToneMap, porque la transformación es afín).
- Rango almacenado: al menos las 72 notas visibles; idealmente MIDI 0..127 si cabe. Así la transposición no
  pierde datos.
- **Presupuesto: ≤ 40 KB para todo el estado del mapa.** Comprobar en la fase 2 si el ADV tiene PSRAM
  (`ESP.getPsramSize()` por serie). Suponer que no la tiene.
- El estado del mapa debe vivir fuera del objeto vista (estático o del modo), para sobrevivir a cambiar de modo.

### 4.5 Dibujo

- Lienzo de 240×125 a 16 bits ya existente; `draw()` con límite de ~30 ms por fotograma, como el clásico.
- Redibujar el fotograma entero es aceptable: 72 rectángulos más el texto.
- Los colores se mezclan en RGB888 y se convierten con `color565`.
- Medir en el bench el tiempo de `draw()` + `pushSprite` + análisis. Si el bucle no llega a ~25 lecturas/s
  con la pantalla a 30 fps, bajar a 20 fps.

### 4.6 Tiempo

- Usar `millis()` en el modelo pasándolo como parámetro, para que el modelo sea puro y comprobable.
- `dt` de cada segmento = duración real del salto entre ventanas analizadas (`HOP / sampleRate` si no se saltó
  ninguna).

### 4.7 Cambios fuera de los ficheros nuevos (mínimos)

- `ui.h`: añadir a `Mode` un `virtual void onReselect() {}` vacío.
- `main.cpp`: en `handleKey`, si la tecla es el hotkey del modo actual, llamar a `current->onReselect()` en vez de
  no hacer nada. Los otros modos no cambian de comportamiento, porque su `onReselect` está vacío. No mandar la
  tecla a `onKey`: el metrónomo cancelaría su medida de tap.
- `settings.h/.cpp`: los campos y claves de §2.6 (con `clampAll`, `load`, `tick`).
- `mode_tuner.cpp`: vista actual (clásica o ToneMap) y `onReselect()` que alterna. En ToneMap delega `enter`,
  `leave`, `onKey`, `update`, `draw` y `afterPush` en la vista nueva. La parte clásica no cambia.
- `platformio.ini`: entornos de bench y depuración (§5). Sin librerías nuevas.

### 4.8 Ficheros nuevos sugeridos (la fase 2 puede ajustarlos)

- `src/pitch_mpm.{h,cpp}`: detector de rango completo (sin Arduino).
- `src/tonemap_model.{h,cpp}`: notas, segmentos y trozos, colores, desvanecido, re-basado, desplazamiento por
  transposición, reset (sin Arduino; recibe `nowMs`).
- `src/tonemap_audio.{h,cpp}`: captura continua con solape sobre `M5Cardputer.Mic`.
- `src/tonemap_view.{h,cpp}`: dibujo, teclas, LED y franja; lo usa `mode_tuner.cpp`.
- `src/tonemap_bench.cpp`: solo con `-DTONEMAP_BENCH` (§5, fase 3).
- Cabecera en los ficheros portados:
  `Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)`.

---

## 5. Plan de ejecución por fases y agentes

Agentes de Claude Code:
- `Explore`: solo lectura, rápido para barrer código.
- `Plan`: arquitecto, solo lectura, devuelve un plan.
- `general-purpose`: lee, escribe y ejecuta.

Los agentes de solo lectura devuelven el texto y el orquestador lo escribe en `docs/`.

| Fase | Agente | Entrega | Parada |
|---|---|---|---|
| 0 Preparación | orquestador | rama, SPEC actualizado, referencias a mano | — |
| 1 Verificar la referencia | `Explore` (very thorough) | lista de discrepancias con §3 (o "OK") | — |
| 2 Diseño ESP32 | `Plan` | `docs/TONEMAP_DESIGN.md` | solo si contradice §2 |
| 3 Captura + detector + bench | `general-purpose` | `pitch_mpm`, `tonemap_audio`, entorno bench | — |
| 4 Modelo del mapa + bench | `general-purpose` | `tonemap_model` + pruebas de paridad | **PARADA A** |
| 5 Vista e integración | `general-purpose` | firmware con la vista ToneMap | **PARADA B** |
| 6 Revisión independiente | `general-purpose` nuevo, sin contexto, sin editar | informe de hallazgos | — |
| 7 Correcciones y ajuste fino | orquestador (+ `general-purpose` si hace falta) | firmware final, SPEC con casillas | **PARADA C** |

### Fase 0 — Preparación (orquestador)

1. `git fetch origin && git checkout feature/tonemap` (si `main` avanzó desde el 2026-10-16, `git rebase origin/main`).
2. Añadir a `docs/SPEC.md` una sección "Afinador — vista ToneMap" con el contenido de §2 de este brief, y
   enlazar este brief.
3. Referencias **fuera del repo** (p. ej. `..\refs\`):
   - `git clone --depth 1 https://github.com/crossrhythm/tonemap.live`.
   - `npm pack pitchy@4.0.1` y descomprimirlo (si no hay npm: https://unpkg.com/pitchy@4.0.1/index.js).
   - Las fuentes de M5Unified 0.2.25 ya están en `.pio/libdeps/cardputer-adv/M5Unified` tras un `pio run`.
4. Commit `docs: ToneMap section in SPEC` y push.

### Fase 1 — Verificar la referencia (`Explore`, very thorough)

Prompt base:
> Read-only. Compare section 3 of `docs/TONEMAP_BRIEF.md` against the real sources: `tonemap.live/index.html`
> (functions `tick`, `freqToNoteData`, `getTargetOffsetCents`, `biasRelaxed`, `biasMedium`, `biasHard`,
> `finalizeActiveSegment`, `getRecentWeightedErr`, `rebaseErrRatio`, `updateCellDisplayColor`,
> `recomputeAllCellColorsAndRender`, `shiftCellStateBySemitones`, `highlightGridCell`, `getNeedleCellPosition`,
> the mode/stain/history constants) and `pitchy/index.js` (`findPitch`, `_nsdf`, `getKeyMaximumIndices`,
> `refineResultIndex`). Report every formula, constant or behaviour where the brief is wrong or incomplete,
> quoting the source. If it all matches, say so. Do not summarise the rest of the app.

El orquestador corrige §3 si hay discrepancias y hace commit.

### Fase 2 — Diseño para el ESP32 (`Plan`)

Prompt base:
> Design the ESP32-S3 implementation of the ToneMap view for this repo, following `docs/TONEMAP_BRIEF.md`
> §2–4 and the current code (`src/*`, `platformio.ini`). Read M5Unified 0.2.25 `Mic_Class` in `.pio/libdeps` to
> confirm the two-slot record queue and pick the capture scheme. Deliver: file list with responsibilities and
> public interfaces; data structures of the map state with a byte count (budget ≤ 40 KB, assume no PSRAM);
> the capture/analysis/draw schedule inside the existing `loop()` with a CPU time estimate; the exact
> `onReselect`, settings and `mode_tuner.cpp` changes; the bench and debug build environments and what they
> print over serial. Keep the classic tuner, metronome, game and About untouched. Flag anything that
> contradicts the spec instead of changing it.

El orquestador guarda el resultado en `docs/TONEMAP_DESIGN.md` y hace commit. Solo para a preguntar a Sergio si
el diseño exige cambiar algo de §2 o añadir una dependencia.

### Fase 3 — Captura continua, detector y bench (`general-purpose`)

1. `pitch_mpm` según §3.1/§4.2 y `tonemap_audio` según §4.1 y el diseño.
2. Entorno `cardputer-adv-tonemap-bench` (`-DTONEMAP_BENCH`): al arrancar, en lugar de la app normal:
   - **Precisión sintética**: genera en memoria senoidales y dientes de sierra (con armónicos, como un violín)
     a 16 kHz y a 32 kHz para C2, A2, C3, C4, A4, C5, C6, C7, A7, B7 y ±20 c alrededor. Imprime por serie
     `freq real, freq detectada, error en cents, clarity` y marca si hay error de octava.
   - **Tiempo**: µs por análisis a cada frecuencia de muestreo (media y máximo de 50 análisis).
   - **Micro en vivo**: después, captura continua a la frecuencia elegida e imprime unas 10 líneas por segundo
     con `note, cents, Hz, clarity, rms, analyses/s, loop ms`.
   - Todo el texto en inglés.
3. Elegir 16 o 32 kHz con los datos (criterio en §4.1) y anotarlo en `docs/TONEMAP_DESIGN.md`.
4. Compilar los dos entornos (normal y bench), commit y push. Sin PARADA todavía: la fase 4 se prueba en la misma
   instalación.

### Fase 4 — Modelo del mapa (`general-purpose`)

1. `tonemap_model` según §3.2–3.4 y §4.4, puro y con `nowMs` como parámetro.
2. En el bench, pruebas del modelo con resultado `PASS`/`FAIL` por serie:
   - **Paridad de notas**: de `tonemap.live/tests/fixtures/temperament-vectors.json`, filtrar las filas con
     `temperament = equal` y `stretch = none` (264 filas, A4 = 440 y 441). Generar con un script de Python
     (PlatformIO ya trae Python) una cabecera `test/tonemap_vectors.h` con `inputHz, a4, expect.midi,
     expect.cents, expect.needleEven[0]`. Comprobar midi exacto, cents ±0,01 y posición de aguja ±0,001.
   - **Color**: errores conocidos por modo → color esperado según §3.3 (casos: dentro de la zona muerta, en el
     borde, a mitad de rampa, saturado, signo).
   - **Historia**: secuencias sintéticas (1 s a +10 c y luego 2 s a −10 c) → media esperada para History 1 s, 3 s,
     30 s y unlimited; tolerancia según la duración de los trozos.
   - **Desvanecido**: hold 5 s → intensidad a los 4, 10 y 20 s; no fade.
   - **Re-basado**: cambiar A4 de 442 a 440 con historial a +0 c → la media pasa a ≈ +7,85 c.
   - **Transposición**: desplazar C → Bb mueve la celda 2 semitonos arriba.
3. Compilar, commit y push.

**PARADA A**: Sergio instala el firmware de bench (WebUI o USB con permiso) y Claude lee el serie.
1. Resultados sintéticos y de paridad: todo `PASS`.
2. Micro en vivo: Sergio toca una escala lenta en su instrumento, notas largas, en C3–C7. Comprobar:
   - que no hay errores de octava;
   - que los cents son estables (±2 c en una nota sostenida);
   - que hay ≥ 25 análisis/s.
3. Calibrar con Sergio los umbrales de la puerta de ruido (habitación en silencio, hablando, tocando
   piano/forte) y la claridad.
4. Anotar los valores medidos en `docs/TONEMAP_DESIGN.md`.

### Fase 5 — Vista e integración (`general-purpose`)

1. `tonemap_view` según §2.2–2.8: cuadrícula, celda activa, aguja, franja, mensajes, teclas, LED (con el brillo),
   pausa.
2. Integración en `mode_tuner.cpp`, `onReselect` en `ui.h`/`main.cpp`, ajustes en `settings.*` (§4.7).
3. Entorno `cardputer-adv-tonemap-debug` (`-DTONEMAP_DEBUG`): una línea superpuesta con análisis/s, ms de
   análisis, ms de dibujo, rms y clarity, y log por serie dos veces por segundo.
4. Compilar todos los entornos, commit y push.

**PARADA B**: Sergio prueba en el dispositivo con la lista de §6, apartados B. Claude sigue el serie del entorno
de depuración si Sergio lo instala.

### Fase 6 — Revisión independiente (`general-purpose` recién lanzado, sin editar)

Prompt base:
> You are reviewing a finished feature you did not write. Do not edit files. Read `docs/TONEMAP_BRIEF.md`,
> `docs/TONEMAP_DESIGN.md`, `docs/SPEC.md`, `CLAUDE.md` and the diff of branch `feature/tonemap` against `main`.
> Check: (1) every requirement of brief §2 and §6 is implemented; (2) the formulas match brief §3 and the
> tonemap.live source; (3) CLAUDE.md rules (English UI text, silent UI, no new deps, classic tuner/metronome/
> game/About behaviour unchanged); (4) bugs: integer overflow in ms/chunk math, ring buffer indices, NaN/inf on
> silence, LED/brightness not restored on every exit path, mic/drone state on view and mode switches.
> Report findings ranked by severity with file:line and a concrete failure scenario.

### Fase 7 — Correcciones y ajuste fino (orquestador)

1. Corregir los hallazgos de la fase 6 y lo que haya salido en la PARADA B.
2. Compilar, commit y push.

**PARADA C**: prueba final de Sergio con §6 completo.
1. Marcar las casillas en `docs/SPEC.md`.
2. Preguntar a Sergio si se fusiona `feature/tonemap` en `main`.

---

## 6. Criterios de aceptación

**A. Bench (serie)**
- [ ] Paridad de notas con el fixture de ToneMap: 264/264 filas `PASS`.
- [ ] Sintético: sin errores de octava de C2 a B7 (sinusoidal y diente de sierra); error ≤ ±1 c de C2 a C6 y
      ≤ ±3 c en la octava 7 (o "best effort" documentado).
- [ ] Análisis completo en < 20 ms y ≥ 25 análisis/s en vivo con la pantalla dibujando.
- [ ] Pruebas de color, historia, desvanecido, re-basado y transposición: `PASS`.

**B. En el dispositivo (Sergio)**
- [ ] `T` dentro del afinador alterna las vistas; al volver de otro modo se abre la última usada (también tras
      reiniciar).
- [ ] Una escala lenta pinta cada nota en su celda; una nota tocada adrede alta se tiñe de naranja, baja de azul,
      centrada de verde.
- [ ] La aguja de la celda activa sigue un vibrato; los cents de la franja se ponen verdes al centrar.
- [ ] `S`, `H`, `C`, `N`, `K`, `F`, `L`, `R` y `Space` hacen lo de §2.5, muestran su mensaje y se recuerdan tras
      reiniciar.
- [ ] Arriba/Abajo cambian la La y es la misma que en la vista clásica. El mapa se re-puntúa al cambiarla.
- [ ] En silencio no se pinta nada con la puerta en `Low`; hablando cerca, poco o nada con `High`.
- [ ] El mapa sigue ahí tras ir al metrónomo y volver; `R` lo borra.
- [ ] LED verde al centrar, apagado si no. Con `L` off o al salir, el brillo vuelve al del usuario.
- [ ] Sin sonidos al pulsar teclas. Todo el texto en inglés.
- [ ] La vista clásica, el drone, el metrónomo, el juego y About funcionan exactamente igual que antes.

---

## 7. Riesgos conocidos

- **Graves del micro integrado:** puede que no capte bien por debajo de ~C3. Lo dice la PARADA A. Si falla, la
  fila C2 se queda vacía, pero la cuadrícula sigue siendo C2–B7, como pidió Sergio.
- **Octava 7 a 16 kHz:** imprecisa. De ahí la medición de 32 kHz.
- **CPU:** detector de rango completo + dibujo + 30 análisis/s pueden ahogar el bucle. Las salidas están en §4.2
  y §4.5.
- **LED y brillo:** el brillo debe restaurarse en todos los caminos de salida (cambio de vista, de modo, `L`).
- **Huecos de captura:** si la cola de dos grabaciones de M5Unified no se mantiene llena, los `dt` serán
  irregulares. El bench lo detecta con análisis/s.

## 8. Decisiones de la conversación (2026-10-16)

- Alcance v1: núcleo + ajustes básicos (sensibilidad, History, Color hold) + puerta de ruido, transposición,
  sostenidos/bemoles y LED.
- Instrumento principal: cuerda / viento agudo.
- Pantalla: 6 octavas C2–B7 con franja de información; etiquetas solo con la nota (octava en la columna C).
- Entrada: `T` alterna dentro del afinador; se recuerda la última vista.
- Ajustes con una tecla cada uno; `R` borra; Arriba/Abajo cambian la La.
- El mapa dura mientras el dispositivo está encendido.
- LED con tecla `L`, encendido por defecto.
- Ejecución por fases con paradas; brief en el repo (rama) y en el proyecto de claude.ai.
- Decidido por Claude (cambiable): `Space` = pausa; Izquierda/Derecha sin uso; la barra sigue diciendo `Tuner`;
  puerta por defecto `Low`; transposiciones C/Bb/Eb/F con los valores de ToneMap.

## 9. Cambios tras pruebas en dispositivo (2026-10-22)

Sergio probó la vista en el Cardputer ADV y pidió un mapa más legible de un vistazo. Cambios deliberados respecto a
tonemap.live y a las secciones anteriores (el detalle y la justificación están en `TONEMAP_DESIGN.md` §13):

- **Celda activa:** el borde blanco interior de 2 px tapaba demasiado de la celda de 20 × 18 px; ahora (segunda tanda, 2026-10-22)
  el blanco es exclusivo de la celda activa: una barra blanca de 2 px sobre el borde inferior de la celda y la etiqueta en
  blanco (sin esquinas, contorno ni sombra); las demás etiquetas, negro o gris claro `(190,190,190)` por luminancia. La línea de
  referencia negra de 2 px y la aguja no cambian (la aguja se dibuja después de la barra).
- **Paletas** (§2.3, sustituye a la paleta fija): `Safe` (por defecto; azul – casi blanco – naranja, pensada para
  daltonismo y con la luminancia ordenada centro > alto > bajo) y `Extreme` (antes `Classic`; primarios puros en un único degradado
  continuo: azul en el extremo bajo, verde justo en el centro, rojo en el alto, sin meseta ni saltos —`t_lin` lineal sobre todo
  el rango, ±15,5 c en Medium— y sin atenuar por debajo del 75 %, ver `TONEMAP_DESIGN.md` §13.7). Tecla `P`, ajuste `tmPal`, mensaje `Palette: Safe` / `Palette: Extreme`.
  Las etiquetas de las celdas tocadas pasan a negro o gris claro según la luminancia del color mostrado (blanco solo en la celda activa).
- **Rampa más estrecha** (§3.3): `span` = 2·dz en Relaxed y Medium, 4·dz en Hard (antes 4, 4 y 8). Color pleno desde
  ±18,5 c (Relaxed), ±15,5 c (Medium) y ±10,1 c (Hard); la zona de centro puro no cambia (±6,2 / ±5,2 / ±2,0 c).
- **Intensidad plena a los 250 ms** de sonido (`FULL_CONF_MS`, antes 500) y **suelo del desvanecido 0,45**
  (`STAIN_MIN_FRACTION`, antes 0,25) (§3.4).
- El firmware normal (`cardputer-adv`) no tiene ninguna línea ni superposición de depuración: eso solo existe en
  `cardputer-adv-tonemap-debug`.
- **Panel de opciones** (2026-10-22, segunda tanda): `Alt` o `Enter` abre/cierra una superposición a pantalla completa con las
  teclas y sus valores actuales (`SPEC.md`, `TONEMAP_DESIGN.md` §13.5). Pista `Alt:menu` en la franja de info cuando cabe.
- **Cents y LED con el color vivo** (2026-10-22, tercera tanda): el texto de cents de la franja y el LED muestran el color de la
  desviación instantánea de la nota que suena, en la paleta activa (Safe: con zona muerta; Extreme: degradado continuo), a
  intensidad plena. Sustituye al "texto verde / LED verde dentro de la zona muerta" de §2.4 y §2.8 (`TONEMAP_DESIGN.md` §13.8).
