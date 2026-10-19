# Micrófonos para futuros afinadores / metrónomos con ESP32

Nota para la fase 2 del proyecto (diseños propios con ESP32 portando este código). Fecha: 2026-10-19.
Las cifras vienen de las hojas de características y fichas de producto enlazadas abajo; no las he medido yo.
Lo único medido con el hardware real es lo de la sección "Qué se midió con el Cardputer ADV".

## Qué se midió con el Cardputer ADV

- Micro: según el README del componente de Espressif para el Cardputer, el ADV usa un micro MEMS **analógico** conectado
  al ADC del códec ES8311 (el Cardputer original usa un SPM1423 PDM). **No se ha encontrado el modelo exacto del micro.**
- Con una viola a ~30 cm (rms en unidades int16): silencio 5–12, voz 26 de mediana (máx. 86), viola 190 de mediana
  (máx. 1043); notas agudas y pianos 90–200.
- El fundamental de las cuerdas graves llega débil: con el detector pitchy estándar (K = 0,9) F3 salía como F4 y los
  pizzicati E3/D3/C3 una octava arriba. Se corrigió con un K más estricto (0,97) por debajo de 400 Hz
  (ver `TONEMAP_DESIGN.md` §12). **Que la causa sea la respuesta en graves del micro/caja es una inferencia a partir de
  esos datos, no una medición directa.**
- A 16 kHz de muestreo la octava 7 falla (errores de octava); 32 kHz funciona (reloj I2S exacto).

## Requisitos para un afinador

| Requisito | Valor recomendado | Motivo |
|---|---|---|
| Respuesta en graves | Plana hasta ~60 Hz o menos | La viola llega a 130 Hz y un violonchelo a 65 Hz; un fundamental atenuado provoca errores de octava |
| Punto de sobrecarga acústica (AOP) | ≥ 120 dB SPL | El instrumento está cerca; un micro saturado distorsiona la onda |
| SNR | ≥ 64 dBA | Las notas agudas y flojas llegan con poca señal |
| Muestreo | 32–48 kHz | Medido: a 16 kHz falla la octava 7 |
| Interfaz | I²S o PDM digital | El ADC interno del ESP32 es ruidoso e impreciso; el ESP32-S3 admite ambos |
| Filtro paso-alto | Sin recorte agresivo en graves | Comprobar el del códec/micro |

## Candidatos

| Micro | Interfaz | SNR | Graves | AOP | Notas |
|---|---|---|---|---|---|
| Infineon **IM69D130** | PDM | 69 dB(A) | roll-off a 28 Hz | 130 dB SPL (distorsión < 1 % a 128 dB SPL) | Mejor en el papel; sensibilidad −36 ±1 dB; necesita placa de ruptura (breakout) o kit de evaluación; usaría la entrada PDM de la periferia I²S del ESP32-S3 |
| **INMP441** | I²S | 61 dBA | 60 Hz | – | Módulos baratos y muy documentados para ESP32 |
| Knowles **SPH0645** | I²S | 65 dB(A) | 100 Hz | – | Cifras de un revendedor, no del fabricante; peor en graves |
| TDK **ICS-43434** | I²S | 64–65 dBA | 60 Hz | 120 dB SPL | Sensibilidad −26 dBFS ±1 dB. **Evitar en diseños nuevos:** figura como "Last Time Buy" (15-06-2026) y sin stock en DigiKey; el sustituto sugerido (MMICT5837-00-012) no es compatible directo |

Recomendación: **IM69D130** para empezar; **INMP441** como alternativa fácil; no usar ICS-43434 por disponibilidad.

## Consejos que pesan más que el modelo del micro

- **Caja y puerto acústico:** un buen micro detrás de un agujero pequeño o dentro de una carcasa pierde graves; dejarlo
  libre de vibraciones.
- **Sensor de contacto:** un piezo con pinza (puente o voluta) ignora el ruido de la sala y no pierde el fundamental;
  necesita preamplificador y un ADC/códec externo (no el ADC del ESP32). Es otro diseño.
- **Antes de comprar:** grabar con el micro candidato la misma secuencia de la calibración (cuerdas al aire, escala
  grave, notas agudas flojas) con la tecla `w` del entorno de bench y analizarla offline.

## Fuentes

- [Espressif m5stack-cardputer component readme](https://components.espressif.com/components/espp/m5stack-cardputer/versions/1.3.8/readme)
- [TDK ICS-43434 product page](https://invensense.tdk.com/products/ics-43434)
- [ICS-43434 datasheet v1.2 (SparkFun mirror)](https://cdn.sparkfun.com/assets/a/9/c/9/1/DS-000069-ICS-43434-v1.2.pdf)
- [DigiKey ICS-43434 (Last Time Buy)](https://www.digikey.be/en/products/detail/tdk-invensense/ICS-43434/6140298)
- [INMP441 module (Components101)](https://components101.com/modules/inmp441-mems-omnidirectional-microphone)
- [SPH0645 module listing](https://ifuturetech.org/?p=62955)
- [Infineon IM69D130 product page](https://www.infineon.com/cms/en/product/sensor/mems-microphones/im69d130/)
- [Infineon IM69D130 datasheet](https://www.infineon.cn/content/dam/infineon/row/public/documents/24/49/infineon-im69d130-datasheet-en.pdf)
