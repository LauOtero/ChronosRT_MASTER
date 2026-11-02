# Guía de Benchmarking — ChronosRT Master

**Versión:** 0.1.0 · **Herramienta:** `chronosrt_bench` · **Suites:** 10 (`ctest`)

## 1. Marco de métricas

| Métrica | Descripción | Herramienta |
|---|---|---|
| Latencia | Tiempo de evento end-to-end | `chronosrt_bench --mode latency` |
| Throughput | Eventos por segundo | `--mode throughput` |
| Jitter | Desviación estándar de latencia | `--mode jitter` |
| WCET | Peor caso medido | `--mode wcet`, histograma (`chronosrt_rt.h`) |
| Percentiles | p50 / p99 / p99.9 del histograma | campos `p50_ns`, `p99_ns`, `p999_ns` |
| Picos > 10 µs | Muestras fuera del objetivo, en ppm | campo `latency_spikes` |
| Coste por trama | ns por construir/parsear | `--mode frame` |
| CRC-32C | GB/s y tramas/s por backend | `--mode crc` |
| ID local | ns del candidato, filtro y admisión | `--mode aalid` |
| Cache/branch misses | `perf stat` | `perf stat …` |
| Memoria | Ancho de banda, page faults | `perf stat`, `/proc/vmstat` |
| Energía | Consumo | `powercap`/`turbostat` |

> **Distinción importante:** ChronosRT publica dos tipos de cifras. Las
> **medidas** (coste de CPU por trama, CRC-32C, FEC) se obtienen sin NIC y son
> reproducibles en cualquier host. Las **objetivo** (latencia < 10 µs, jitter
> < 1 µs, throughput > 10 M eventos/s) dependen del kernel, del aislamiento de
> CPU y del hardware; solo son válidas si se miden en ese entorno.

---

## 2. Preparación del entorno

Antes de comparar resultados, fije el entorno:

1. Kernel con **PREEMPT_RT** (mainline ≥ 6.12 con `preempt=full`) o parche `-rt`.
2. `cmdline`: `isolcpus=<núcleos> nohz_full=<núcleos> rcu_nocbs=<núcleos>`.
3. *Governor* `performance`, *turbo* y C-states bajo control (`processor.max_cstate=1`,
   `intel_idle.max_cstate=0`).
4. IRQ de la NIC fijadas a los núcleos aislados.
5. Sin cargas ajenas; mismo build (`Release`) y mismas opciones.
6. Interfaz de red real (`ip -br link`: `enp2s0`, `eno1`, `end0`, `enx<MAC>`).

Compruebe el estado con:

```bash
./build/chronosrt_bench --mode latency --duration 1
```

Salida real en el host de referencia **(AMD Ryzen 7 4800H, Linux 7.3.0 con PREEMPT_RT)**:

```
[chronosrt] Kernel: 7.3.0 (7.3.0-2-cachyos-rt-bore)
[chronosrt] PREEMPT_RT: sí
[chronosrt] aislamiento: isolcpus=on nohz_full=on rcu_nocbs=on
[chronosrt] sched_ext_enabled=on
```

---

## 3. Modos de `chronosrt_bench`

```bash
./build/chronosrt_bench --help
```

| Modo | Comando | Qué mide |
|---|---|---|
| Latencia | `--mode latency` | Round-trip publish/subscribe y métricas |
| Throughput | `--mode throughput --threads N` | Eventos/s sostenidos |
| Jitter | `--mode jitter` | Distribución y desviación |
| Estrés | `--mode stress` | Estabilidad bajo carga |
| WCET | `--mode wcet` | Peor caso |
| CRC | `--mode crc --duration S` | GB/s y tramas/s por backend |
| Trama | `--mode frame` | Construir/parsear por trama, QoS y TDMA |
| ID local | `--mode aalid` | ID candidato, filtro de destino y admisión (2 niveles) |

Opciones: `--duration S`, `--threads N`, `--pool-size N`, `--hugepages`,
`--no-simd`, `--no-affinity`, `--platform <rpi4|rpi5|jetson_nano|jetson_orin|intel_nuc|orange_pi5|rock5b|x86_64_rt>`.

### 3.0 Protocolo interno de medida

Antes de la ventana de medida, `chronosrt_bench` hace lo siguiente en cada hilo:

1. **Eleva a `SCHED_FIFO` prioridad 80** (`ChronosRT_sched_set_fifo_current`).
   Si el sistema no lo permite, lo informa y continúa en `SCHED_OTHER`; no
   falla. Con **más hilos que CPUs permitidas** la elevación se **omite** a
   propósito: un productor que nunca se bloquea hambrientearía al consumidor.
2. **Fija el hilo a una CPU distinta** del conjunto permitido
   (`sched_getaffinity` + reparto por índice), evitando que dos hilos caigan en
   el mismo núcleo.
3. **Calienta 20 000 iteraciones** `publish` + `subscribe` (2 000 en los
   productores de throughput) antes de inicializar el histograma.
4. La memoria ya está bloqueada y prefallocada (`lock_memory = true` en el CLI:
   `mlockall` + *prefault* del pool), así que no hay page faults en caliente.

La salida incluye una línea de confirmación:

```
[chronosrt] SCHED_FIFO: aplicado (prioridad 80)
```

Si no aparece `aplicado`, las cifras de latencia no son comparables con las de
un host que sí eleva prioridad.

### 3.1 Salida real de `--mode latency` (kernel RT, núcleo aislado)

```
[chronosrt] PREEMPT_RT: sí
[chronosrt] aislamiento: isolcpus=on nohz_full=on rcu_nocbs=on
[chronosrt] SCHED_FIFO: aplicado (prioridad 80)

=== ChronosRT Benchmark Results ===
Total Events:        471934674
WCET (ns):           16900
Best Case (ns):      23
Min Latency (ns):    23
Max Latency (ns):    31999
Avg Latency (ns):    23
StdDev (ns):         51
p50 (ns):            25
p99 (ns):            50
p99.9 (ns):          50
Spikes > 10 us:      0 (4.1 ppm)
CPU Usage (%):       0.00
Cache Miss Rate:     0.0000%
Branch Miss Rate:    0.0000%
================================

Status: EXCELLENT - All metrics within target
```

Lectura: el cuerpo de la distribución es de **23 ns de media** con p99.9 de
**50 ns**; el WCET lo fija una cola de **4,1 muestras por millón**.

### 3.2 Salida real de `--mode throughput` (2 hilos, 1P+1C)

```
[chronosrt] consumidos: 92540036 (1 productores, 1 consumidores)
[chronosrt] SCHED_FIFO: aplicado (prioridad 80)

=== ChronosRT Benchmark Results ===
Total Events:        92540036
WCET (ns):           0
...
Spikes > 10 us:      0 (0.000 ppm)
================================

Status: OK - modo sin métricas de latencia
```

El modo throughput cuenta **publicaciones exitosas** y, por separado, los
eventos **consumidos**.

---

## 4. Medición de coste por trama (`--mode frame`)

Host de referencia: **AMD Ryzen 7 4800H (Zen 2)**, Linux `7.3.0-2-cachyos-rt-bore`, build
`Release` (`-O3`) con **AVX2** habilitado. Ejecución representativa (dispersión
< 5 % entre ejecuciones), descontando el bucle vacío (0,35 ns).

Salida real:

```
EtherTux v0.1.0 (cabecera de 26 B):
  construir PDU (64 B, con copia)              27.48 ns
  construir cabecera (64 B, sin copia)         19.35 ns
  parsear + validar CRC (64 B)                 16.17 ns
  construir PDU (512 B, con copia)             41.64 ns
  construir cabecera (512 B, sin copia)        31.30 ns
  parsear + validar CRC (512 B)                28.66 ns
  construir PDU (1464 B, con copia)            98.80 ns
  construir cabecera (1464 B, sin copia)       74.00 ns
  parsear + validar CRC (1464 B)               71.47 ns
  Flow Aggregator: 8 flujos x 32 B             28.31 ns
  Flow Aggregator: parsear 8 flujos             9.65 ns
  Flow Aggregator: serializar + PDU agregada     70.88 ns
  8 PDU individuales de 32 B                  173.88 ns
  admisión QoS (P2, 576 B)                    12.96 ns
  ventana TDMA activa (superframe 1 ms)         2.13 ns

EtherCAT (datagrama de 10 B + datos):
  construir datagrama (32 B)                    7.44 ns
  parsear datagrama (32 B)                      3.71 ns
  construir datagrama (256 B)                  10.58 ns
  parsear datagrama (256 B)                     3.95 ns
  construir datagrama (1024 B)                 15.75 ns
  parsear datagrama (1024 B)                    3.54 ns
```

### 4.1 Lectura

- El coste de CPU queda **muy por debajo** del tiempo de transmisión.
- EtherTux paga más CPU que EtherCAT porque calcula CRC-32C en cada envío y
  recepción (la integridad es parte del contrato); EtherCAT delega en el WKC del
  ESC.
- La construcción **sin copia** ahorra ~20–40 ns por trama grande.
- El Flow Aggregator reduce **68 % de CPU** y **44 % de bytes** en el caso de 8
  flujos de 32 B (121 ns / 320 B frente a 391 ns / 576 B).

---

## 5. Medición de la HAL (`--mode crc`)

Salida real (mismo host, backend activo `sse4.2-x4`):

```
CPU: AuthenticAMD / AMD Ryzen 7 4800H with Radeon Graphics (family 23, model 96, stepping 1)
CRC-32C activo: sse4.2-x4   FEC: avx2

sse4.2-x4:
  trama mínima (P0)           60 B    15.06 GB/s     49.03 M tramas/s
  telemetría                 552 B   27.51 GB/s     49.80 M tramas/s
  MTU completa               1504 B   23.55 GB/s     15.70 M tramas/s

slice-by-8:
  trama mínima (P0)           60 B    3.39 GB/s     25.99 M tramas/s
  telemetría                 552 B    3.22 GB/s      3.18 M tramas/s
  MTU completa               1504 B   3.14 GB/s      1.14 M tramas/s
```

| Comparación | Ganancia |
|---|---|
| Trama de 60 B | ×4,4 |
| Trama de 552 B | ×8,5 |
| Trama de 1504 B | ×7,5 |

En ARMv8.1+ se usa `crc32c*`; en la Banana Pi M2 Zero (Cortex-A7) se cae al
*fallback* `slice-by-8`.

### 5.1 Techo físico del enlace

| Carga útil | Cable | 1 Gbps | 2,5 Gbps | 10 Gbps |
|---|---|---:|---:|---:|
| 20 B (P0) | 84 B | 1 488 095 | 3 720 238 | 14 880 952 |
| 512 B | 576 B | 217 013 | 542 534 | 2 170 138 |
| 1464 B (MTU) | 1528 B | 81 806 | 204 515 | 818 062 |

El límite práctico es el **enlace**, no la CPU. Para maximizar tramas/s use
tramas pequeñas en el plano de control y reserve el MTU para datos.

---

## 6. Medición del ID local (`--mode aalid`)

Nivel 1 (camino crítico de recepción) y Nivel 2 (admisión), medidos con
`chronosrt_bench --mode aalid`:

```
Nivel 1 (camino crítico de recepción):
  ID candidato (CRC-32C plegado)                4.67 ns
  ID candidato (multiplicativo)                 6.09 ns
  MAC local <- ID (LAA)                         1.85 ns
  ID del origen a partir de la MAC              1.50 ns
  backoff determinista (64 slots)               6.14 ns

Nivel 2 (admisión, fuera del superframe):
  admisión de un nodo nuevo (4096 altas)       45.0 ns
  reclamación de un nodo conocido              7.87 ns
  búsqueda {MAC -> ID} con 4096 nodos          9.50 ns
  ciclo admisión + liberación explícita    345.15 ns

Filtro del maestro:
  filtro de destino (unicast propio)            4.94 ns
```

| Operación | Coste | Lectura |
|---|---|---|
| ID candidato (CRC-32C) | 4,67 ns | tiempo constante, sin ramas ni bucles |
| *Fallback* multiplicativo | 6,09 ns | plataformas sin CRC-32C |
| Filtro de destino del maestro | 4,94 ns | descarte antes de parsear y validar el CRC |
| Admisión de nodo nuevo | 45 ns | O(1) amortizado; solo en admisión |
| Búsqueda `{MAC → ID}` | 9,5 ns | tabla hash abierta, independiente del número de nodos |
| Ciclo admisión + liberación | 345 ns | recorre la tabla; operación administrativa |

### 6.1 Lectura práctica

El Nivel 1 **no garantiza unicidad**: con 16 bits la paradoja del cumpleaños
supera el 50 % hacia los ~300 nodos. La unicidad la aporta el Nivel 2, que se
ejecuta fuera del superframe.

El filtro de destino cuesta 4,94 ns y descarta antes de parsear; una trama
ajena de MTU habría costado **71,47 ns** de parseo y validación de CRC (§4).

---

## 7. Scripts de automatización

### 7.1 Barrido de latencia por hilos

```bash
#!/bin/bash
OUTPUT="latency_results.csv"
echo "threads,min_ns,avg_ns,max_ns,stddev_ns,wcet_ns" > "$OUTPUT"

for threads in 1 2 4 8 16; do
    ./build/chronosrt_bench --mode latency --threads="$threads" --duration=30 \
        > "bench_latency_${threads}.txt"
    MIN=$(grep 'Min Latency' "bench_latency_${threads}.txt" | awk '{print $4}')
    AVG=$(grep 'Avg Latency' "bench_latency_${threads}.txt" | awk '{print $4}')
    MAX=$(grep 'Max Latency' "bench_latency_${threads}.txt" | awk '{print $4}')
    STD=$(grep 'StdDev'      "bench_latency_${threads}.txt" | awk '{print $3}')
    WCET=$(grep 'WCET'       "bench_latency_${threads}.txt" | awk '{print $3}')
    echo "${threads},${MIN},${AVG},${MAX},${STD},${WCET}" >> "$OUTPUT"
done
```

### 7.2 Perfil del sistema

```bash
#!/bin/bash
echo "=== System Profile ==="
echo ""
echo "CPU:"
lscpu | grep -E "Model name|CPU\(s\)|Thread|Core|Socket"
echo ""
echo "Memory:"
free -h
echo ""
echo "Hugepages:"
grep -i huge /proc/meminfo
echo ""
echo "Kernel cmdline:"
cat /proc/cmdline
echo ""
echo "PREEMPT:"
zcat /proc/config.gz 2>/dev/null | grep -E "CONFIG_PREEMPT|CONFIG_PREEMPT_RT" || true
echo ""
echo "Kernel release:"
uname -r
echo ""
echo "HAL de ChronosRT:"
./build/chronosrt_bench --mode crc --duration 1 | head -n 4
```

---

## 8. Criterios de éxito

| Requisito | Criterio | Cómo se mide |
|---|---|---|
| Latencia | p50 y p99.9 en la misma década (ruta determinista) | `--mode latency`/`jitter` + `rtla` |
| Picos > 10 µs | < 1 % de las muestras | campo `latency_spikes` del informe |
| Throughput | > 10 M eventos/s con 1P+1C | `--mode throughput --threads 2` |
| Jitter | σ < 1 µs | `--mode latency`/`jitter` |
| Uso de CPU | < 5 % en reposo | `--mode stress` + `perf stat` |
| Coste por trama | Menor que el tiempo de transmisión | `--mode frame` |
| CRC-32C | Backend HW activo | `--mode crc` |

Medido en el host de referencia con núcleos aislados y *governor* `performance`:
p50 **25 ns**, p99.9 **50 ns**, σ **51 ns**, picos **4,1 ppm** y **46,3 M eventos/s** (1P+1C).

---

## 9. Hardware de referencia y objetivos

### 9.1 Host de medida de este documento

| Dato | Valor |
|---|---|
| CPU | AMD Ryzen 7 4800H (Zen 2, 16 hilos) |
| Kernel (RT) | Linux 7.3.0-2-cachyos-rt-bore (`PREEMPT_RT`, `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0-1`) |
| Kernel (no RT) | Linux 7.3.0-2-cachyos-bore |
| Compilador | GCC 16.2.1 |
| CRC-32C | `sse4.2-x4` |
| FEC | `avx2` |
| Build | `Release` `-O3 -DNDEBUG` con LTO activado |

### 9.2 Objetivos por plataforma

| Plataforma | Latencia P99 | Jitter (σ) | Throughput |
|---|---|---|---|
| SBC 1 Gbps (OPi 5, RPi 5, x86-64) | < 10 µs | < 1 µs (con PHC) | 800–940 Mbps reservando QoS |
| MCU W6100 (100 Mbps) | 80–300 µs | 30–80 µs | ~50–91 Mbps |
| MCU W6300 (100 Mbps) | 50–150 µs | 20–50 µs | ~91 Mbps |

Estas cifras son **objetivos de diseño** del
[Protocolo EtherTux.md](Protocolo%20EtherTux.md) §12; deben validarse en cada
plataforma con el procedimiento de §2.

---

## 10. Verificación de las suites

```bash
ctest --test-dir build --output-on-failure
```

| Suite | Aserciones |
|---|---|
| `chronosrt_test` | 20 |
| `chronosrt_test_lockfree` | 26 |
| `chronosrt_test_rt` | 30 |
| `chronosrt_test_resilience` | 29 |
| `chronosrt_test_integration` | 27 |
| `chronosrt_test_network` | 58 |
| `chronosrt_test_hwaccel` | 40 |
| `chronosrt_test_ethertux` | 103 |
| `chronosrt_test_ethertux_v010` | 152 |
| `chronosrt_test_aalid` | 62 |
| **Total** | **547** |

Estado del host de referencia: **11/11 suites, 100 %**, en 0,08 s.

---

## 11. Reportes automatizados

```bash
#!/bin/bash
OUT="reports/$(date +%Y%m%d)"
mkdir -p "$OUT"

for mode in latency throughput jitter stress wcet frame crc aalid; do
    echo "Ejecutando $mode..."
    ./build/chronosrt_bench --mode="$mode" --duration=60 > "$OUT/$mode.txt" 2>&1
done

./build/chronosrt_bench --mode frame > "$OUT/frame.txt" 2>&1
ctest --test-dir build --output-on-failure > "$OUT/ctest.txt" 2>&1

{
  echo "<!DOCTYPE html><html><head><meta charset='utf-8'>"
  echo "<title>ChronosRT Benchmark</title></head><body>"
  echo "<h1>ChronosRT Benchmark — $(date)</h1>"
  for f in "$OUT"/*.txt; do
      echo "<h2>$(basename "$f")</h2><pre>"
      sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g' "$f"
      echo "</pre>"
  done
  echo "</body></html>"
} > "$OUT/index.html"
```