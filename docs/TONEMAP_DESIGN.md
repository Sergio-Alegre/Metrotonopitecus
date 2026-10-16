# ToneMap en el ESP32-S3 — diseño (fase 2)

Salida del agente `Plan` (solo lectura) sobre `TONEMAP_BRIEF.md` §2–4 y §3.6, `CLAUDE.md`, `src/*` y
M5Unified 0.2.25. Los tiempos de CPU son **estimaciones**: se miden en la fase 3. Al final, §11 recoge las
decisiones del orquestador y §12 los valores medidos (se rellenan en las fases 3 y 4).

## 0. Titulares y avisos

1. **Brief §4.1 ("`record()` falla si la cola está llena") es incorrecto en 0.2.25.** Desde una tarea normal
   `Mic_Class::_rec_raw()` **bloquea** (reintenta `xSemaphoreTake` de 1 tick) hasta que se libera un hueco; solo
   devuelve `false` al instante si se llama desde el callback de liberación o con argumentos/arranque erróneos.
   Nunca llamar a `record()` desde `loop()` salvo con `isRecording() < 2`. El afinador clásico es seguro porque
   hace `if (isRecording()) return;`.
2. **Existe un callback de liberación público**: `Mic_Class::setBufferReleaseCallback(args, func(args, data, len))`,
   invocado desde la tarea de captura cuando termina una petición; dentro se puede llamar a `record()`. Permite
   captura sin huecos independiente de `loop()` (una iteración con análisis + dibujo + push + `delay(4)` puede
   durar ~30 ms, casi un hop de 32 ms: con sondeo se moriría de hambre la cola). **Se recomienda el callback**;
   el sondeo queda como alternativa documentada (§2).
3. **τmax "~62 Hz" (brief §4.2) es demasiado alto.** C2 escrita con Bb es Bb1 de concierto = 58,3 Hz (A=442);
   con A=400, 53 Hz. Usar **maxLag = 720 a 32 kHz (44,4 Hz)**. Un lóbulo aún abierto al final del barrido necesita
   la regla del final (§3).
4. **El almacén se indexa por MIDI de concierto y los errores se guardan relativos a A=440.** Mismo
   comportamiento observable que ToneMap (la celda se desplaza con la transposición, la La re-puntúa), con
   mecanismos exactos y sin deriva (§5).
5. **Diferencias intencionadas respecto a pitchy/ToneMap:**
   el detector resta la media (offset DC del micro int16; pitchy no); el barrido NSDF para en maxLag, no en N−1;
   un lóbulo abierto al final se acepta si su pico ya quedó atrás; Unlimited es ilimitado de verdad (ToneMap poda
   a ~33 s / 10 min); el fade-in EMA de Relaxed usa el dt del tick como Δ, no el intervalo de render.
6. **Pausa:** "el mapa se congela" se implementa parando el reloj del mapa durante la pausa (los desvanecidos
   también se congelan).
7. **Reenvíos del LED a 6 y 12 ms no se pueden cumplir exactamente** (iteración de hasta ~30 ms): se disparan en
   la(s) siguiente(s) iteración(es). El LED verde dura muchos frames, es aceptable.
8. **Supuesto de hardware a verificar:** el ES8311 engancha a fs=32 kHz. En el ADV `over_sampling` es 1, el
   puerto I2S corre a 32 kHz y MCLK=BCLK. El bench mide la cadencia real de trozos frente a 31,25/s y una prueba
   de 440 Hz comprueba el reloj. Constante `kRateTrim` (1.0f).
9. **Sin dependencias nuevas.** Nada de esp-dsp. `-O3` solo para `pitch_mpm.cpp` con
   `#pragma GCC optimize("O3")` (Arduino-ESP32 compila con `-Os`); sin cambios de flags globales.
10. **Edición fuera de ficheros nuevos:** el cambio de `main.cpp` son 3 líneas (el brief decía una). `onReselect`
    es un virtual vacío, como se pidió.
11. **RAM:** no hace falta PSRAM; el ADV no la tiene (ESP32-S3FN8, `qio_qspi`). Bench y debug imprimen
    `ESP.getPsramSize()` para confirmarlo.

| Parámetro | Elección | Motivo |
|---|---|---|
| Frecuencia de muestreo | **32 kHz** | A 16 kHz B7 tiene 4 muestras/periodo y el sesgo de la parábola ronda 16 c; a 32 kHz ~2 c (tono puro). El bench decide (§8). |
| WINDOW | **2048** (64 ms) | C2 (lag 489 a 32 kHz) deja 1559 muestras de solape = 3,2 periodos. |
| HOP | **1024** (32 ms) | 31,25 ventanas/s (> 25/s). 512 saturaría la CPU. |
| Barrido de lag | **1..720** | Empezar en 1 mantiene exacta la máquina de estados de pitchy (+0,1 %). |
| Puerta de frecuencia | 14 < f < 8200 sobre el pitch sin transponer | Como ToneMap. |
| Búsqueda gruesa diezmada | **No justificada** | Cambiaría resultados K=0,9 frente a pitchy; ahorraría ~3 ms. Plan B. |
| Redibujado | ≤ 20 fps, solo si hay cambios | §6. |

## 1. Hechos de Mic_Class (verificados en 0.2.25)

- **Dos huecos.** `_rec_info[2]`; se consumen en orden de envío. `isRecording()` devuelve cuántos huecos tienen
  `length != 0` (0, 1 o 2); el hueco que se está llenando cuenta. Se libera (`length.store(0)`) al escribir la
  última muestra, antes del callback. `< 2` = hay hueco libre.
- **Búfer.** La tarea escribe directamente en el búfer del usuario; debe seguir válido e intacto hasta su
  liberación o hasta que `end()` retorne. Sin restricciones de alineación/longitud (solo puntero no nulo y
  `len > 0`). Peticiones consecutivas son continuas en muestras.
- **Tipo.** La sobrecarga `int16_t*` da PCM mono con signo, recortado a ±(32767−16).
- **Config del mic del ADV:** `over_sampling=1`, `magnification=16`, 16 kHz por defecto, DMA 8×128 tramas
  (= 32 ms a 32 kHz). Pines BCK 41, WS 43, DIN 46. Mono, canal derecho. Tarea de captura prioridad 2, sin fijar.
- **Cambios de frecuencia.** `record(buf, n, rate)` con otra frecuencia reconstruye el puerto si no hay nada en
  vuelo. `Mic.setSampleRate(32000)` antes de `Mic.begin()` construye ya a 32 kHz. El `micCodecCallback` del repo
  escribe registros al activar y no hace nada al desactivar: reconstruir es barato y sin pops.
- **Huecos.** Sin petición pendiente, la tarea se aparca y el DMA desborda; la primera petición tras un hueco
  devuelve hasta una profundidad DMA (32 ms) de audio viejo → el primer trozo tras cualquier (re)arranque es calentamiento.
- **Restricciones del callback:** instalarlo/quitarlo solo antes del primer `record()` o tras `end()`; `record()`
  dentro del callback nunca espera hueco; nada de `printf` dentro (pila de la tarea ~2,5 KB).
- **Código existente:** `audio::micStart()` = silenciar, `stopAll`, `Speaker.end()`, `Mic.begin()`.
  `audio::micStop()` = espera hasta 300 ms a `isRecording()==0`, silenciar, `Mic.end()`, 30 ms, `Speaker.begin()`,
  reactivar sonido. El clásico graba 2048 muestras a 16000 Hz con guarda `isRecording()`.

## 2. Captura: anillo de trozos escrito directamente por la tarea del mic

- **Anillo.** `int16_t ring[kChunks=4][kHop=1024]` (8 KB). El trozo n vive siempre en el hueco `n % 4`.
- **Callback.** Al liberarse el trozo n incrementa `done` (`std::atomic<uint32_t>`) y, si `state==Run`, encola el
  trozo `nextQueue++` con `Mic.record(&ring[slot], kHop, kSampleRate)`. Siempre hay dos trozos en vuelo.
- **Regla de ventana segura.** Los trozos `done-2` y `done-1` son la ventana más reciente; el trozo n+4 empieza a
  escribirse solo cuando n+3 termina (`done ≥ n+4`), así que la ventana es estable 2 hops (64 ms).
  `takeWindow()` copia los dos trozos (~2 KB, ≈5 µs) en `win[2048]` y relee `done`; si avanzó más de 1, la ventana
  está rota y se descarta (`torn++`).
- **Calentamiento.** Tras cualquier (re)arranque el primer trozo es viejo: la primera ventana válida necesita `done ≥ 3`.
- **dt por número de muestras, no por millis:** `dtMs = (seq − prevSeq) · kHopMs`, `kHopMs = 32` (32 ms normal,
  64 ms si se saltó una ventana, como dice el brief §4.6).
- **El análisis nunca encola:** `takeWindow()` devuelve solo la ventana más reciente y cuenta las saltadas.
- **Recuperación:** `service()` (cada loop) reinicia la cola si `state==Run && isRecording()==0` (no debería pasar;
  cubre un `record()` fallido dentro del callback). Reiniciar sube `epoch`, pone `done=0` y reaplica calentamiento.
- **Parar/pausar:** `stop()` pone `state=Idle`; como mucho se encola un trozo más y los de vuelo drenan en ≤96 ms.
  `audio::micStop()` espera `isRecording()==0` (su bucle de 300 ms) antes de `Mic.end()`. La pausa usa `stop()`;
  reanudar relanza solo con `isRecording()==0`, así que nunca bloquea.
- **Alternativa (esquema A, sondeo):** mismo anillo, `record()` desde `service()` solo si `isRecording()<2`, con
  `nextQueue` en `loop()`. Habrá huecos ocasionales porque `loop()` puede durar más de 32 ms.

## 3. Detector (`pitch_mpm`) y CPU

Algoritmo (semántica pitchy 4.0.1, solo `float`):

1. `prepare()` calcula la media, escribe `work[i] = x[i] − mean` y devuelve rms y pico (unidades int16). La vista
   aplica aquí la puerta de ruido: si la ventana queda bajo la puerta se salta `findPitch()` (silencio ≈ 60 µs
   por ventana en vez de ~10 ms).
2. NSDF tipo II para lags 1..maxLag con `NSDF[0]=1` implícito. Suma directa
   `r'(τ) = Σ_{j=0}^{N-1-τ} w[j]·w[j+τ]`; `m'(0) = 2·Σw²`; `m'(τ+1) = m'(τ) − w[τ]² − w[N−1−τ]²`;
   `n(τ) = 2r'/m'`, o 0 si `m' ≤ 0`.
3. Máximos clave con la máquina de estados de pitchy desde i=1 con `nsdf[0]=1`: cruce ascendente
   `prev ≤ 0 && cur > 0` siembra `max` con esa muestra; cruce descendente `prev > 0 && cur ≤ 0` registra
   `maxIndex` si no es −1. **Diferencia intencionada:** al final del barrido, si un lóbulo sigue abierto y su
   `maxIndex ≤ maxLag-2` con `nsdf[maxIndex+1] ≤ nsdf[maxIndex]`, se registra (el pico quedó atrás). Un lóbulo
   truncado cuyo pico no se alcanzó se descarta.
4. `nMax` = mayor máximo clave; resultado = primer máximo clave ≥ 0,9·nMax (K = 0,9).
5. Refinamiento parabólico en dominio desplazado (estable en float): `δ = 0,5(ya−yc)/(ya−2yb+yc)`,
   `clarity = min(1, yb − 0,25(ya−yc)δ)`, `freq = fs/(lag+δ)`. Denominador 0 → δ=0.

Modelo de coste (MACs ≈ Σ_{τ=1}^{720}(2048−τ) ≈ 1,2 M):

| Núcleo | Ciclos/MAC | Tiempo a 240 MHz |
|---|---|---|
| Ingenuo, un acumulador (`madd.s` dependientes, 2 cargas) | ~5 | ~25 ms (incumple < 20 ms) |
| 4 lags a la vez, 4 acumuladores, 1,25 cargas/MAC (**obligatorio**) | 1,5–2,5 | **7–12 ms (central ~10 ms)** |

- `prepare` + máximos clave + copia ≈ 0,1 ms. Por segundo: 31 ventanas × 10 ms ≈ 310 ms con sonido; ≈ 2 ms en silencio.
- Reglas del núcleo: `#pragma GCC optimize("O3")` en `pitch_mpm.cpp`; todos los búferes estáticos en DRAM interna.
- Escala de recurso si el bench da > 15 ms: (a) revisar `-O3` y el bloqueo de 4 lags, (b) limitar el dibujo a
  15 fps, (c) maxLag 604 (53 Hz), (d) seguir el lag cerca de la nota previa (riesgo nuevo), (e) diezmar τ>256 por 2
  (ahorra solo ~30 %).
- **Precisión esperada (a medir):** sesgo de la parábola para tono puro ≈ 0,4 c en C7 y ≈ 2 c en B7 a 32 kHz.
  Rozando el ±3 c de la octava 7; si falla, "best effort" documentado (brief §4.1).

## 4. Ficheros e interfaces

```
src/pitch_mpm.h/.cpp        sin Arduino; detector
src/tonemap_model.h/.cpp    sin Arduino; estado del mapa + colores + notas (millis por parámetro)
src/tonemap_audio.h/.cpp    anillo de captura + callback sobre M5Cardputer.Mic
src/tonemap_view.h/.cpp     dibujo, teclas, LED/brillo, franja, overlay de debug
src/tonemap_bench.cpp       todo dentro de #ifdef TONEMAP_BENCH
tools/gen_tonemap_vectors.py   solo stdlib de Python; escribe test/tonemap_vectors.h
test/tonemap_vectors.h      generado, versionado
```

Todas las cabeceras llevan: `Ported from tonemap.live (MIT, (c) 2025 Jeremy Black) and pitchy 4.0.1 (MIT)`.

### 4.1 `pitch_mpm.h`

```cpp
namespace mpm {
constexpr size_t kMaxWindow = 2048;
constexpr int    kMaxLag    = 720;           // sizing cap: nsdf needs kMaxLag+2 floats
struct Levels { float mean, rms, peak; };    // int16 units, DC removed
struct Result { bool found; float freq, clarity, lag; };
Levels prepare(const int16_t* x, size_t n, float* work);   // work[n]
Result findPitch(const float* work, size_t n, float sampleRate, int maxLag, float* nsdf);
inline Result detect(const int16_t* x, size_t n, float fs, int maxLag, float* work, float* nsdf); // bench
}
```

### 4.2 `tonemap_audio.h`

```cpp
namespace tonemap_audio {
constexpr uint32_t kSampleRate = 32000;
constexpr size_t   kWindow = 2048, kHop = 1024, kRingChunks = 4;
constexpr uint32_t kHopMs = 32;
constexpr float    kRateTrim = 1.0f;         // calibrate in PARADA A
void attach();                               // setSampleRate(32000) + install callback; mic must be ended
void detach();                               // clear callback + setSampleRate(16000); after Mic.end() returned
bool start();                                // requires audio::micOn(); queues chunks 0 and 1; epoch++; done=0
void stop();                                 // state=Idle
bool running();
void service();                              // every loop (starvation restart, pending resume)
struct WindowInfo { uint32_t seq, epoch; };
bool takeWindow(int16_t* dst /*2048*/, WindowInfo& info);   // newest complete window not yet taken
struct Stats { uint32_t chunks, starved, torn, skipped, recFail; };
Stats stats();
}
```

El callback comprueba que `data` está dentro del anillo y `len == kHop`; ignora el resto. Solo usa estado estático
del fichero, no `args`.

### 4.3 `tonemap_model.h`

```cpp
namespace tonemap {
constexpr int kNoteCount = 128;              // indexed by CONCERT midi, 0..127
constexpr int kGridLo = 36, kGridHi = 107;   // WRITTEN midi shown (C2..B7)
enum class Sensitivity : uint8_t { Relaxed, Medium, Hard };
struct SensConst { float deadzone, errScale, spanMul, needleTauMs; };
extern const SensConst kSens[3];             // {0.0025,0.7,4,350} {0.0021,0.7,4,80} {0.00105,0.9,8,30}
constexpr uint32_t kHistoryMs[4]  = {1000, 3000, 30000, 0};            // 0 = unlimited
constexpr int32_t  kHoldMs[6]     = {5000, 15000, 30000, 120000, 300000, -1};  // -1 = no fade
constexpr int8_t   kTransposeSemis[4] = {0, -2, 3, 5};
struct Rgb { uint8_t r, g, b; };             // kHigh{255,170,0} kCenter{34,197,94} kLow{0,86,214}

// The single place a temperament would plug in.
float latticeOffsetCents(int concertMidi);                       // 0 in v1
float targetOffsetCents(int concertMidi, float a4);              // lattice + 1200*log2(a4/440)
float targetHz(int concertMidi, float a4);                       // 440*2^((m-69)/12)*2^(off/1200)
struct NoteHit { int concertMidi; float cents, errRatio, targetHz; };
NoteHit freqToNote(float hz, float a4);       // ±2 candidates around round(); minimise |(pos-cand)*100 - off|; ties -> lower
float   needlePct(float cents);               // (clamp(c,-50,50)+50)/100
Rgb     biasColor(Sensitivity s, float avgErrRatio);
bool    inDeadzone(Sensitivity s, float errRatio);
float   baseIntensity(uint32_t totalMs);      // min(1, total/500)

struct TickResult { bool hasNote, newNote; int concertMidi, writtenMidi; float hz, cents, displayCents, errRatio; bool inDeadzone; };

class Model {
public:
  void reset(uint32_t nowMs);
  void setSensitivity(Sensitivity, uint32_t nowMs);    // recompute: lastStain=now, anchor cleared
  void setHistory(int idx, uint32_t nowMs);             // same
  void setRefA(float a4, uint32_t nowMs);               // same (rescoring is implicit, see §5.4)
  void setTransposition(int semis, uint32_t nowMs);     // ends the active note; same recompute
  void setHold(int idx);                                // no timer restart
  TickResult tick(bool valid, float hz, uint32_t dtMs, uint32_t nowMs);
  void endNote();                                       // pause / leave
  Rgb  cellColor(int writtenMidi, uint32_t nowMs, bool* visited);  // evaluates fade; black if display <= 0.001
  bool anyFading(uint32_t nowMs) const;
  // test hooks
  float    avgErrRatio(int concertMidi) const;
  uint32_t totalMs(int concertMidi) const;
  float    displayIntensity(int concertMidi) const;
};
}
```

Un `static Model` vive en `tonemap_view.cpp` (.bss, estado cero válido), así que el mapa sobrevive a cambios de modo y de vista.

### 4.4 `tonemap_view.h` (lo usa `mode_tuner.cpp`)

```cpp
namespace tonemap {
void enter();  void leave();                  // leave() idempotent
void onKey(const KeyEvent&);
void update();
bool draw(M5Canvas&, bool force);
void afterPush();
}
```

## 5. Estado del mapa y bytes

### 5.1 Rango almacenado

Indexado por **MIDI de concierto 0..127** (128 notas; coincide con el rango de ToneMap y no pierde nada al transponer).

| Rango | Notas | Bytes |
|---|---|---|
| **0..127 (recomendado)** | 128 | **37.888** |
| 24..119 (si falta heap) | 96 | 28.416 |
| 36..107 (no recomendado: pierde datos con K) | 72 | 21.312 |

El detector solo puede producir MIDI de concierto ≈ 29..121, pero 0..127 cabe en el presupuesto.

### 5.2 `NoteState` (`static_assert(sizeof==296)`)

```
offset size field
  0     8  int64  sumAll           Σ dur_ms * err_q16 over all voiced time (Unlimited)
  8     4  uint32 totalMs          voiced ms since reset (saturating)
 12     4  uint32 lastStainMs
 16     4  float  displayI         stored display intensity
 20     4  float  fadeAnchor
 24     4  int32  accFineSum       open fine chunk: Σ dur*err
 28     4  int32  accCoarseSum     open coarse chunk
 32     2  uint16 accFineMs
 34     2  uint16 accCoarseMs
 36     1  uint8  fineHead, 37 fineCnt, 38 coarseHead, 39 coarseCnt
 40     1  uint8  flags            bit0 visited, bit1 anchorValid, bit2 everStained
 41     1  pad
 42   136  Chunk fine[34]          Chunk = { uint16 durMs; int16 errQ16 }  (4 B)
178   116  Chunk coarse[29]
294 -> sizeof 296 (align 8)
```

- **Total:** 128 × 296 = 37.888 B + globales del Model (parámetros, nota activa, EMA de aguja, A4) ≤ 64 B ≈
  **37.952 B = 37,06 KiB**, bajo los 40.960 B con 3.008 B de margen.
- `errQ16 = round(errRatio · 65536)` en `int16` (rango ±0,5).
- **Rango:** los errores guardados son relativos al destino A=440 (ver §5.4), peor caso ±(0,03·1,09 + 0,09) ≈ ±0,125.
- Estado a cero = mapa vacío válido; `fadeAnchor` lleva bandera de validez, no centinela −1.
- **Otra RAM** (no es estado del mapa): anillo 8.192 + `win` 4.096 + `work` 8.192 + `nsdf` 2.888 ≈ 23,4 KB, DRAM estática. El lienzo de 60 KB ya existe.

### 5.3 Trozos fino/grueso e History

- **Añadir un frame** (válido, misma nota que el frame válido anterior, `dt>0`):
  `errQ = clamp(round(stored · 65536))`, luego `accFineSum += dt·errQ; accFineMs += dt; totalMs += dt; sumAll += dt·errQ`.
  Si `accFineMs ≥ 96` se cierra el trozo fino: se empuja `{ms, round(sum/ms)}` al anillo fino. Trozos de 96 ms
  (tres frames de 32 ms), o más largos solo si se saltaron ventanas.
- **Cascada.** Cuando el anillo fino (34 entradas, ≥ 3,26 s) está lleno, la entrada más antigua se vuelca al
  acumulador grueso abierto. Con `accCoarseMs ≥ 1000` se cierra en el anillo grueso (29 entradas ≈ 30,6 s).
  Cobertura total ≥ 32,2 s.
- **Consulta de History** de H ms, de lo más nuevo a lo más viejo: acumulador fino abierto → anillo fino →
  acumulador grueso abierto → anillo grueso. Para cada elemento `take = min(dur, restante)`; suma `take·avg`;
  `restante -= take`. El trozo del borde entra de forma fraccionada (como `Math.min(seg.durMs, remaining)` de
  ToneMap); error acotado por un trozo (≈ 96–130 ms), permitido por el brief §4.4. Duración total 0 → devuelve 0.
- History 1 s y 3 s: solo las dos primeras fuentes. 30 s: las cuatro. **Unlimited:** `sumAll / totalMs`, exacto.
- **Estado de visualización:** `visited`, `displayI`, `fadeAnchor` y `lastStainMs` siguen `updateCellDisplayColor`
  de ToneMap (`index.html` L10252-10324). Desviación: el fade-in EMA de Relaxed usa Δ = dt del tick.
  La evaluación del desvanecido es perezosa y determinista, pero el valor se materializa en `displayI` antes de
  cualquier tinte, recálculo o reset (ToneMap lo reescribe cada frame).
  Medium/Hard: `display = base`. Relaxed: EMA hacia base solo en el frame de tinte.
  Desvanecido: pasado `hold`, rampa lineal de 10 s desde el ancla hasta `0,25·base`, y se queda; no fade = sin rampa.
  `base = 0` o nunca teñida → `display = 0`.

### 5.4 Re-basado de A4 y transposición

- **Re-basado sin reescribir trozos.** El error guardado en cada trozo es relativo al destino A=440:
  `stored = (1+errRatio_actual)·(a4/440) − 1`. En lectura: `e_cur = (1+avgStored)·(440/a4_cur) − 1`.
  Es el mismo re-basado afín de ToneMap (`rebase(err, from, to)` con `from−to = 1200·log2(a4_old/a4_new)`),
  aplicado una vez al promedio; exacto porque todos los trozos comparten referencia. Sin deriva por cuantización
  tras pulsar Arriba/Abajo muchas veces (el bench lo prueba). `setRefA()` aplica los efectos laterales del
  recálculo de ToneMap (`lastStain=now`, ancla borrada, Medium/Hard saltan a base).
  **Limitación:** un temperamento futuro con offsets de retícula por segmento necesitaría un offset por trozo o re-basado in situ.
- **Transposición.** Notas guardadas por MIDI de concierto. La rejilla muestra la escrita `w = concierto − t`, o sea
  `c = w + t`. Cambiar `t` solo cambia la búsqueda (equivale a `shiftCellStateBySemitones(old−new)`).
  `setTransposition` termina la nota activa y aplica los efectos del recálculo. Frecuencia→nota usa el pitch sin
  transponer y el destino de concierto; algebraicamente igual que el camino `adjustedPitch` de ToneMap (los factores
  `2^(−t/12)` se cancelan).

## 6. Calendario del bucle y presupuesto de CPU

Dentro del `loop()` existente; `main.cpp` solo cambia 3 líneas:

```
loop():  M5Cardputer.update(); input::poll()                               ~0.3 ms
         keys -> handleKey -> TunerMode::onKey -> tonemap::onKey
         settings::tick(); audio::update()
         TunerMode::update() -> tonemap::update():
              LED resends / expiry, message expiry
              tonemap_audio::service()
              if (!paused && takeWindow()):                                (new 32 ms window)
                  prepare (60 us) -> gate -> findPitch (~10 ms) -> model.tick (~20 us)
         draw(): only if dirty && now-lastDraw >= 50 ms (20 fps); if an analysis ran
                 this iteration, defer unless overdue > 80 ms     -> build frame ~4 ms
         main: pushSprite + waitDisplay ~13-15 ms -> afterPush() (measure push time)
         delay(4)
```

| Concepto por segundo (20 fps, 31 ventanas/s) | ms |
|---|---|
| Análisis NSDF (31 × ~10 ms) | ~310 |
| `prepare` (31 × 0,06 ms) | 2 |
| ticks del modelo | < 1 |
| dibujo (CPU, 20 × ~4 ms) | ~80 |
| push + `waitDisplay` (20 × ~14 ms) | ~280 |
| `delay(4)` y teclado | ~30 |
| **Total ocupado** | **~700 de 1000** |

- El núcleo ingenuo de 25 ms se pasaría (~1,1 s): por eso el de 4 lags es obligatorio.
- No se dibuja si nada cambió (en silencio y sin desvanecidos baja a ~0).
- **Eventos "sucios":** nota nueva, cents o Hz con cambio visible a 0,1, movimiento de la aguja en píxeles, del
  medidor de nivel en píxeles, inicio/fin de mensaje, una tecla, y un temporizador de 500 ms solo mientras
  `anyFading()` sea true.
- **Optimización opcional si el push limita:** `Display.setClipRect()` en `draw()` para empujar solo franja y filas
  cambiadas, y `clearClipRect()` en `afterPush()` y `leave()`. No requiere cambio en `main.cpp`, pero `drawBar()`
  nunca debe correr con un clip puesto.
- **Medidor de nivel:** `frac = clamp((20·log10(rms/32768)+60)/54, 0, 1)`; la marca del umbral usa el mismo mapeo.
- **Geometría** (como el brief): texto de franja en y=3 (Font0); separador y=15; rejilla y=17..124 (6 filas ×18 px),
  12 columnas × 20 px; líneas de celda a derecha y abajo (`#374151`). Celda activa: borde blanco interior 2 px,
  línea negra de referencia 2 px en el centro vertical, aguja roja `#ef4444` 2 px en `top = 90% − pct·80%`.
  Cents y Hz de la franja muestran los cents mostrados (suavizados en Relaxed, crudos en Medium/Hard); el verde y
  el LED usan `|errRatio| ≤ DEADZONE` crudo por frame.

## 7. Integración

### 7.1 `ui.h`
```cpp
// Called when the mode's own hotkey is pressed while it is already active.
virtual void onReselect() {}
```

### 7.2 `main.cpp` (`handleKey`)
```cpp
for (Mode* m : modes) {
    if (m->hotkey() != e.ch) continue;
    if (m == current) return current->onReselect();
    return switchTo(m);
}
```
`switchTo(next==current)` era un no-op, así que los demás modos se comportan igual (su `onReselect` está vacío).
La tecla no llega a `onKey`: la medida de tap del metrónomo no se ve afectada.

### 7.3 `settings.h` / `settings.cpp`
```cpp
int tunerView = 0;     // 0 classic, 1 tonemap
int tmSens   = 1;      // 0 relaxed 1 medium 2 hard
int tmHist   = 1;      // 0 1s 1 3s 2 30s 3 unlimited
int tmHold   = 2;      // 0 5s 1 15s 2 30s 3 2min 4 5min 5 no-fade
int tmGate   = 1;      // 0 none 1 low 2 med 3 high
int tmTransp = 0;      // semitones: 0, -2, 3, 5
int tmFlats  = 0;
int tmLed    = 1;
```
Claves NVS: `tunerView`, `tmSens`, `tmHist`, `tmHold`, `tmGate`, `tmTransp`, `tmFlats`, `tmLed` (≤ 15 car.).
`clampAll`: `constrain` de cada una; `tmTransp` se fuerza a 0 si no está en {0, −2, 3, 5} (guardar semitonos hace
trivial añadir A (−3)). `load()`: `getInt` con el valor actual como defecto. `tick()`: `putInt`. Se amplía la línea
serie de ajustes cargados. Guardado diferido sin cambios; `touch()` en cada cambio.

### 7.4 `mode_tuner.cpp` (código clásico sin cambios, solo movido)
- Incluir `tonemap_view.h`; añadir miembro `bool tm_`. Los cuerpos actuales de `enter()`/`leave()` pasan a
  `enterClassic()`/`leaveClassic()` privados, idénticos.
```cpp
void enter() override {
    tm_ = settings::get().tunerView == 1;
    if (tm_) tonemap::enter(); else enterClassic();
}
void leave() override { if (tm_) tonemap::leave(); else leaveClassic(); }
void onKey(const KeyEvent& e) override { if (tm_) return tonemap::onKey(e); /* classic body */ }
void update() override { if (tm_) return tonemap::update(); /* classic body */ }
bool draw(M5Canvas& c, bool force) override { if (tm_) return tonemap::draw(c, force); /* classic */ }
void afterPush() override { if (tm_) tonemap::afterPush(); }
void onReselect() override {
    if (tm_) { tonemap::leave(); tm_ = false; settings::get().tunerView = 0; settings::touch(); enterClassic(); }
    else     { leaveClassic(); tm_ = true; settings::get().tunerView = 1; settings::touch(); tonemap::enter(); }
    dirty_ = true;
}
```
El `update()` clásico no debe correr con ToneMap activo: su `pollMic` pelearía con la cola de trozos.

### 7.5 Matriz de entrada/salida (mic, drone, brillo, LED)

`tonemap::enter()`, en orden:
1. `tonemap_audio::attach()` (32000 Hz, instala callback; el mic está terminado en todos los caminos de entrada).
2. `audio::micStart()`.
3. Guardar el brillo del usuario; si el LED está activado, subir a 255 si es menor y recordar que se subió.
4. Sincronizar el modelo con los ajustes, en silencio (sin reiniciar temporizadores si no cambia nada).
5. `tonemap_audio::start()`.
6. Si `micStart` falló: mostrar "Microphone unavailable" y omitir la captura.

`tonemap::leave()` (idempotente), en orden:
1. `tonemap_audio::stop()`.
2. Si el LED estaba on: enviar off tres veces con `delay(6)` entre envíos (el pin sigue a 255).
3. Restaurar brillo (solo si se subió).
4. `audio::micStop()`.
5. `tonemap_audio::detach()` (frecuencia de vuelta a 16000).
6. `model.endNote()`.

| Camino | Drone | Mic | Brillo / LED |
|---|---|---|---|
| Otro modo → Afinador (ToneMap) | off (`leave()` clásico ya lo paró) | `micStart` | subir si LED on |
| Otro modo → Afinador (clásico) | sin cambios | sin cambios | sin cambios |
| ToneMap → otro modo (M, G, A) | n/a | `leave()` para captura y `micStop` | LED off, restaurar, **antes** de que el `enter()` del siguiente modo lea el brillo (`switchTo` llama `leave()` y luego `enter()`) |
| ToneMap → clásico (T) | n/a | `leave()` completo y luego `enterClassic()` | restaurar |
| Clásico con drone → ToneMap (T) | `leaveClassic()` ejecuta `droneStop()` (fade-out) y `micStop()` | `tonemap::enter()` hace `micStart` | subir si LED on |
| `L` on | n/a | n/a | guardar brillo, subir a 255 |
| `L` off | n/a | n/a | LED off (con reenvíos), restaurar brillo |
| `Space` pausa | n/a | captura parada, mic sigue encendido | LED off, brillo sin cambios |
| `Space` reanudar | n/a | reinicia con `isRecording()==0` | sin cambios |
| `micStart` falla | n/a | sin captura | LED/brillo simétricos |
| Hambruna de captura | n/a | `service()` reinicia, epoch++ | sin cambios |

Clásico↔ToneMap pasan por el `leave()`/`enter()` completo del lado que se abandona: mismo camino de mic probado que
el on/off del drone; cuesta ~300 ms al pulsar `T`.

### 7.6 Teclas de ToneMap

| Tecla | Manejador |
|---|---|
| `S` | rota `tmSens`; `model.setSensitivity`; mensaje |
| `H` | rota `tmHist`; `model.setHistory`; mensaje |
| `C` | rota `tmHold`; `model.setHold`; mensaje |
| `N` | rota `tmGate`; mensaje |
| `K` | rota transposición; `model.setTransposition`; mensaje |
| `F` | alterna `tmFlats`; mensaje |
| `L` | alterna `tmLed`; brillo/LED como arriba; mensaje |
| `R` | `model.reset`; mensaje |
| `Space` | pausa/reanuda (el reloj del mapa se para) |
| Arriba/Abajo | `refA ± 1` (400..480); `model.setRefA` |
| Izq./Der. | sin uso |

Cada cambio llama a `settings::touch()`. Mensajes de 1,5 s (`Sensitivity: Hard`, `History: 30 s`,
`Color hold: 2 min`, `Noise gate: High`, `Transpose: Bb`, `Accidentals: flats`, `LED: off`, `Map cleared`). En pausa
la franja muestra `Paused (Space)` fijo.

**Tabla de puerta (inicial, a calibrar en la PARADA A):** `kGateRms = {0, 120, 300, 800}`, `kMinClarity = 0.8`.
La comprobación de energía es `sumSq > 0` (equivale a `rms>1e-7 || peak>4e-6` de ToneMap en unidades int16).

## 8. TONEMAP_BENCH y TONEMAP_DEBUG

```ini
[env:cardputer-adv-tonemap-bench]
extends = env:cardputer-adv
build_flags =
    ${env:cardputer-adv.build_flags}
    -DTONEMAP_BENCH=1
    -I${PROJECT_DIR}/test
build_src_filter =
    +<*>
    -<main.cpp>
    -<mode_*.cpp>
    -<tonemap_view.cpp>
    -<input.cpp>

[env:cardputer-adv-tonemap-debug]
extends = env:cardputer-adv
build_flags =
    ${env:cardputer-adv.build_flags}
    -DTONEMAP_DEBUG=1
```

`tonemap_bench.cpp` aporta `setup()`/`loop()` en el build de bench y compila a una unidad vacía en el resto. Todo el
texto serie en inglés.

### 8.1 Salida del bench

```
[bench] ToneMap bench, build <date> <time>
[bench] cpu=240MHz heap_free=... heap_max_block=... psram=0
[bench] sizeof(NoteState)=296 sizeof(Model)=... budget=40960
[synth] fs=32000 N=2048 maxLag=720 wave=saw f=440.00 off=+20c det=440.26 err=+0.01c clar=0.99 oct=ok PASS
...
[synth] SUMMARY fs=32000: 60 cases, octave errors 0, max|err| <=C6 0.4c, oct7 1.9c PASS
[synth] SUMMARY fs=16000: ...
[time] fs=32000 maxLag=720: avg 9.8 ms max 10.4 ms (50 runs, saw C3)
[time] fs=16000 maxLag=360: avg ...
[test] parity equal/none: 264/264 PASS
[test] parity extra: n/n PASS
[test] color / history / fade / rebase / transpose / chunks / first-frame / nan / budget ... PASS
[test] SUMMARY: ALL PASS | FAILURES=k
[live] note=A4 cents=+3.2 hz=442.6 clar=0.97 rms=1234 an/s=30.1 chunks/s=31.2 loop_ms=33.2 starved=0 torn=0
```

- **Casos sintéticos:** senoidal y diente de sierra de banda limitada (armónicos aditivos por recurrencia de
  rotación, sin `sinf`), a 16 y 32 kHz. Notas: C2, A2, C3, C4, A4, C5, C6, C7, A7, B7, cada una a −20, 0, +20 c.
  Regla: sin error de octava; ≤ 1 c hasta C6; ≤ 3 c en la octava 7 (o "best effort" documentado).
- **Tiempo:** 50 análisis por frecuencia, media y máximo en µs.
- **En vivo:** ~10 líneas/s de lo anterior. Una tecla (`d`) simula la carga de dibujo (push de sprite 240×125 cada
  50 ms) para medir `an/s` realista. Una medición de 10 s de `chunks/s` frente a 31,25 da el error real del reloj
  I2S, que fija `kRateTrim`.

### 8.2 Entorno de depuración
- Una línea superpuesta sobre la última fila (y=117, fondo negro): `a/s 30.8 an 9.6ms dr 4.1 push 13.7 rms 412 cl 0.93`.
- Serie dos veces por segundo: `[tm] an/s=30.8 an_ms=9.6/10.9 draw_ms=4.1 push_ms=13.7 fps=19.8 loop_ms=32.4 rms=412 clar=0.93 hz=441.2 note=A4 +3.1c gate=Low starved=0 torn=0 skipped=2 heap=... minheap=...`.
- El tiempo de push se mide entre el retorno de `draw()` y `afterPush()`.

## 9. Pruebas unitarias y de paridad

**Generación** (`tools/gen_tonemap_vectors.py`, stdlib de Python):
- Entrada: `refs\tonemap.live\tests\fixtures\temperament-vectors.json` (objeto con `rows[]`, 5.192 filas, 264 `equal`/`none`).
- Salida: `test/tonemap_vectors.h`, versionado; el build nunca necesita el clon externo.
- Emite `ParityRow {float hz, a4; int16 midi; float cents, needle;}` para las 264 filas
  (`inputHz, a4, expect.midi, expect.cents, expect.needleEven[0]`): el criterio "264/264" es literal.
- Emite filas extra calculadas en doble precisión con las fórmulas de ToneMap (búsqueda de candidatos, empates al
  inferior): cents negativos, ±49,9, ±50,1 (pertenece a la nota siguiente), empates exactos evitados; recorte de
  aguja a ±60 c; transposiciones (0, −2, 3, 5); MIDI extremos (0, 12, 107, 119, 127); A4 en {415, 440, 442, 466}.
- **Guarda:** antes de emitir extras, el script comprueba que sus fórmulas reproducen las 264 filas del fixture a
  1e-9, anclando los extras al fixture real.

**Ejecución:** el bench lo ejecuta todo al arrancar y imprime `PASS`/`FAIL` por serie. `pio run -e cardputer-adv-tonemap-bench`;
Sergio instala `firmware.bin` (Launcher) o se flashea por USB con su permiso; Claude lee el serie. No hay `gcc` de
host en este equipo, así que no hay entorno de pruebas nativo.

**Criterios:**
- **Paridad:** `midi` exacto; |cents − esperado| ≤ 0,01; |aguja − esperada| ≤ 0,001.
- **Color** (±1 por canal): Medium: 0 c → centro; 5,2 c → centro (límite); 15,6 c → t=0,5; ≥ 26 c → alto saturado
  (255,170,0); −40 c → bajo saturado (0,86,214). Hard: centro hasta 2,0 c; saturado a 18,2 c. Relaxed: centro hasta
  6,2 c; saturado a 30,9 c. Más mezcla sobre negro con display 0,5.
- **Historia:** 1 s a +10 c y luego 2 s a −10 c → H=1 s: −10 c; H=3 s: −3,33 c; H=30 s y Unlimited: −3,33 c.
  35 s a +10 c y 5 s a −10 c → H=30 s: +6,67 c; Unlimited: +7,5 c; H=3 s: −10 c (ejercita la expulsión del anillo).
  Tolerancia ≈ peso de un trozo (~1 c en H=3 s, ~0,1 c en H=30 s). Unlimited exacto a 1e-6.
- **Desvanecido** (hold 5 s, base=1): 1,0 a +4 s; 0,625 a +10 s; 0,25 a +20 s; no fade se queda en 1,0. Fade-in EMA
  de Relaxed tras n ticks = `1−(1−α)^n`.
- **Re-basado:** A4 442→440 con historial a +0 c → +7,85 c (±0,02); 440→442 → −7,85; extremos 400/480; 100
  pulsaciones Arriba/Abajo vuelven exactamente (sin deriva).
- **Transposición:** C→Bb mueve la celda del A4 de concierto a la escrita 71 (B4); Eb → 66; F → 64; C→F→C restaura colores.
- **Equivalencia de trozos:** 120 s de frames pseudoaleatorios frente a una referencia de fuerza bruta en RAM solo
  de bench; H=1/3/30 s dentro de la cota del trozo del borde; Unlimited exacto.
- **Regla del primer frame:** n frames válidos de la misma nota producen n−1 segmentos; un frame inválido termina
  la nota; un cambio de nota no produce segmento en su primer frame.
- **Robustez:** `hz = 0 / NaN / inf` con `valid=true` y `false`; `dtMs=0`; reset lo limpia todo.
- **Presupuesto:** `static_assert(sizeof(NoteState)==296)` y `sizeof(Model) ≤ 40960`.
- **Extras del detector:** silencio → `found=false`; ruido blanco → claridad < 0,8 (sin notas falsas).

## 10. Orden de implementación

1. Fase 3: `pitch_mpm`, `tonemap_audio`, entorno bench con secciones sintética, de tiempo y en vivo. Decidir 16 vs
   32 kHz con los datos y anotarlo aquí (§12).
2. Fase 4: `tonemap_model` + `tools/gen_tonemap_vectors.py` + `test/tonemap_vectors.h` + pruebas del modelo en el bench.
3. Fase 5: `tonemap_view` y los 7 cambios pequeños (`ui.h`, `main.cpp`, `settings.h/.cpp`, `mode_tuner.cpp`,
   `platformio.ini`). Compilar todos los entornos, commit y push tras cada compilación correcta (`CLAUDE.md`).

## 11. Decisiones del orquestador

- El diseño **no contradice §2** ni añade dependencias → no hay parada tras la fase 2.
- Aguja en Medium/Hard sin suavizar (réplica fiel de ToneMap, brief §3.6).
- Pausa: el reloj del mapa se para (los desvanecidos se congelan) — coherente con "el mapa se congela" de §2.5.
- El cambio de `main.cpp` es de 3 líneas en vez de una; se considera dentro del espíritu de §4.7.
- Se usa el esquema de callback (§2); el sondeo queda de alternativa.

## 12. Valores medidos

### Fase 3 (bench en el Cardputer ADV, 2026-10-17, COM9)

**Decisión: 32 kHz, WINDOW 2048, HOP 1024, maxLag 720.**

- **Reloj I2S a 32 kHz:** 31,2495 trozos/s (nominal 31,25), `ratio = 0.99999` → `kRateTrim = 1.0`. El ES8311 engancha bien.
  `starved=0`, `torn=0`, `recfail=0`, `badcb=0`. Los primeros ~1,5 s de captura salen con pocos trozos (calentamiento).
- **Tiempo por análisis (32 kHz, maxLag 720):** **9,25 ms** (media y máximo 9,26 ms, 50 ejecuciones), a la
  estimación del diseño. El tiempo no depende de la señal (la suma directa es de coste fijo). `prepare` 0,2 ms.
  Ventana bajo la puerta de ruido: 0,2 ms. A 16 kHz / maxLag 360: 5,2 ms.
- **En vivo, sala en silencio, con carga de dibujo simulada (sprite 240×125 cada 50 ms):** 31,0 análisis/s
  (`skipped=2` al activar la carga), push a pantalla 12,1 ms, dibujo 2,3 ms, `loop_max` 19 ms. Falta medirlo con
  análisis completo (9 ms) + dibujo real: PARADA A / fase 5.
- **Precisión sintética (A4=440, N=2048, ±20 c):**

| fs | Onda | Errores de octava | Hasta C6 | Octava 7 |
|---|---|---|---|---|
| 32 kHz | senoidal | 0 | ≤ 0,05 c | **≤ 1,65 c** (cumple ±3 c) |
| 32 kHz | diente de sierra | 0 | ≤ 0,14 c hasta C5; C6: −0,45 / −0,96 / **+1,39 c** | C7 +1,6 / +4,6 / +2,5 c; A7 +8,7 / +4,7 / −0,7 c; B7 +7,6 / +4,5 / +0,2 c |
| 16 kHz | cualquiera | **7** (octava 7) | hasta 4,6 c | hasta 11 c |

  16 kHz queda descartado (errores de octava en la octava 7). A 32 kHz, C2–C5 es exacto (< 0,15 c).
- **Fuera de criterio (brief §4.1 y §6 A):** con diente de sierra (peor caso: armónicos de amplitud 1/n, picos
  estrechos en la NSDF) la octava 7 sale **sesgada hacia arriba hasta +8,7 c**, y C6 +20 c a +1,39 c (criterio ≤ 1 c).
  La senoidal cumple en todo el rango. El sesgo es sistemático y viene de la interpolación parabólica de pitchy
  sobre picos estrechos (el prototipo en Python lo predijo igual); en ToneMap web con 44,1/48 kHz es menor. **Se
  acepta como "best effort" en la octava 7 para timbres ricos en armónicos** y se revisa con el violín/flauta
  reales en la PARADA A. Si molesta, la mejora (desviarse de pitchy) sería refinar el pico con interpolación
  de orden superior o sobremuestrear el entorno del pico.

### Nota sobre el puerto serie
Con USB-CDC (HWCDC) hace falta `Serial.begin(115200)` explícito en el bench para que `Serial.print` salga; el
puerto se reenumera tras cada reset, así que el bench espera hasta 30 s a que el host abra el puerto. Los
registros del sistema (`E (...)`) salen aunque no se abra.

### Fase 4 (bench en el Cardputer ADV, 2026-10-17)

- `[test] SUMMARY: ALL PASS | FAILURES=0` (14 series, ~1000 comprobaciones; la fase de tests dura 1028 ms).
  Paridad con el fixture de ToneMap **264/264** (máx. |Δcents| 0,00021, |Δaguja| 2e-6) y 192/192 filas extra
  (cents negativos, ±49,9/±50,1, recorte de aguja, transposiciones, MIDI extremos, A4 415–466).
- `sizeof(NoteState)=296`, `sizeof(Model)=37912` (presupuesto 40960). `psram=0`; heap libre con el bench: 262 KB.
- Rebase 442→440: +7,854 c (esperado +7,851 c), 160 pulsaciones Arriba/Abajo sin deriva.
- Equivalencia de trozos frente a fuerza bruta: desviación máx. 5,6e-4 (H=1 s), 1,9e-4 (3 s), 6,3e-5 (30 s) en flujo aleatorio.
- Desviaciones del modelo respecto al diseño (declaradas por el implementador): `setHold(idx, nowMs)` y
  `displayIntensity(i, nowMs)` llevan `nowMs`; un frame aporta como máximo 2000 ms; la media de History se hace en el
  dominio de la razón de error (como ToneMap), de ahí −3,3076 c y no −3,3333 c en el caso de 1 s/2 s; `anyFading`
  es true hasta `hold + 10 s + 600 ms`; `fadeAnchor` queda sin uso; la frecuencia→nota rechaza el frame si el mejor
  candidato queda a más de 100 c; `-I${PROJECT_DIR}/test` pasó a `-Itest` (la ruta con `${PROJECT_DIR}` se rompía en Windows).
- `Model` tiene constructor vacío para que viva en `.bss`; nunca crearlo en la pila.
