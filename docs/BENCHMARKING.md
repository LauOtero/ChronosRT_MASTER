# Guía de Benchmarking — ChronosRT Master

**Versión:** 0.1.0 · **Herramienta:** `chronosrt_bench` · **Suites:** 9 (`ctest`)

## 1. Marco de métricas

| Métrica | Descripción | Herramienta |
|---|---|---|
| Latencia | Tiempo de evento end-to-end | `chronosrt_bench --mode latency` |
| Throughput | Eventos por segundo | `--mode throughput` |
| Jitter | Desviación estándar de latencia | `--mode jitter` |
| WCET | Peor caso medido | `--mode wcet`, histograma (`chronosrt_rt.h`) |
| Coste por trama | ns por construir/parsear | `--mode frame` |
| CRC-32C | GB/s y tramas/s por backend | `--mode crc` |
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

Salida real en un host **sin** PREEMPT_RT (AMD Ryzen 7 4800H, Linux 7.2.11):

```
[chronosrt] Kernel: 7.2.11 (7.2.11-1-cachyos-deckify)
[chronosrt] PREEMPT_RT: no
[chronosrt] aislamiento: isolcpus=off nohz_full=off rcu_nocbs=off
[chronosrt] capacidades: eevdf=on sched_ext=on io_uring=on af_xdp=on sched_deadline=on memfd_secret=on
[chronosrt] Kernel 7.x detectado: soporte pleno
[chronosrt] AVISO: kernel sin PREEMPT_RT; la latencia determinista puede degradarse.
```

Ese aviso es esperado: el framework **no falla**, degrada y lo informa.

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

Opciones: `--duration S`, `--threads N`, `--pool-size N`, `--hugepages`,
`--no-simd`, `--no-affinity`, `--platform <rpi4|rpi5|jetson_nano|jetson_orin|intel_nuc|orange_pi5|rock5b|x86_64_rt>`.

### 3.1 Salida real de `--mode latency` (host sin RT)

```
=== ChronosRT Benchmark Results ===
Total Events:        0
WCET (ns):           2046830
Best Case (ns):      1397
Min Latency (ns):    1397
Max Latency (ns):    2046830
Avg Latency (ns):    2808
StdDev (ns):         6298
CPU Usage (%):       0.00
Cache Miss Rate:     0.0000%
Branch Miss Rate:    0.0000%
================================

Status: NEEDS OPTIMIZATION
```

El WCET alto y el `Status: NEEDS OPTIMIZATION` son la respuesta correcta en un
kernel sin RT: la herramienta mide lo que hay, no lo que se desea. En un kernel
PREEMPT_RT con núcleos aislados, repita la medición y compare.

### 3.2 Salida real de `--mode throughput` (1 s, 4 hilos)

```
Total Events:        1000000
WCET (ns):           0
Best Case (ns):      0
...
Status: EXCELLENT - Jitter < 1 μs
```

El modo throughput cuenta eventos; las métricas de latencia quedan a cero porque
no se miden en esa ruta.

---

## 4. Medición de coste por trama (`--mode frame`)

Host de referencia: **AMD Ryzen 7 4800H (Zen 2)**, Linux `7.2.11`, build
`Release` (`-O3`) con `-march=native`. Ejecución representativa (dispersión
< 5 % entre ejecuciones), descontando el bucle vacío (0,35 ns).

Salida real:

```
EtherTux v0.1.0 (cabecera de 26 B):
  construir PDU (64 B, con copia)              59.77 ns
  construir cabecera (64 B, sin copia)         43.06 ns
  parsear + validar CRC (64 B)                 37.08 ns
  construir PDU (512 B, con copia)             74.10 ns
  construir cabecera (512 B, sin copia)        52.62 ns
  parsear + validar CRC (512 B)                47.93 ns
  construir PDU (1464 B, con copia)           180.77 ns
  construir cabecera (1464 B, sin copia)      140.77 ns
  parsear + validar CRC (1464 B)              138.63 ns
  Flow Aggregator: 8 flujos x 32 B             48.00 ns
  Flow Aggregator: parsear 8 flujos            14.41 ns
  Flow Aggregator: serializar + PDU agregada    121.17 ns
  8 PDU individuales de 32 B                  390.67 ns
  admisión QoS (P2, 576 B)                    16.84 ns
  ventana TDMA activa (superframe 1 ms)         4.21 ns

EtherCAT (datagrama de 10 B + datos):
  construir datagrama (32 B)                    9.95 ns
  parsear datagrama (32 B)                      3.47 ns
  construir datagrama (256 B)                  15.42 ns
  parsear datagrama (256 B)                     3.82 ns
  construir datagrama (1024 B)                 24.03 ns
  parsear datagrama (1024 B)                    3.68 ns
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
  trama mínima (P0)           60 B    2.94 GB/s     49.03 M tramas/s
  telemetría                 552 B   10.76 GB/s     19.48 M tramas/s
  MTU completa               1504 B    9.70 GB/s      6.45 M tramas/s

slice-by-8:
  trama mínima (P0)           60 B    1.56 GB/s     25.99 M tramas/s
  telemetría                 552 B    1.76 GB/s      3.18 M tramas/s
  MTU completa               1504 B    1.72 GB/s      1.14 M tramas/s
```

| Comparación | Ganancia |
|---|---|
| Trama de 60 B | ×1,9 |
| Trama de 552 B | ×6,1 |
| Trama de 1504 B | ×5,6 |

En ARMv8.1+ se usa `crc32c*`; en la Banana Pi M2 Zero (Cortex-A7) se cae al
*fallback* `slice-by-8`.

### 5.1 Techo físico del enlace

| Carga útil | Cable | 1 Gbps | 2,5 Gbps | 10 Gbps |
|---|---:|---:|---:|---:|
| 20 B (P0) | 84 B | 1 488 095 | 3 720 238 | 14 880 952 |
| 512 B | 576 B | 217 013 | 542 534 | 2 170 138 |
| 1464 B (MTU) | 1528 B | 81 806 | 204 515 | 818 062 |

El límite práctico es el **enlace**, no la CPU. Para maximizar tramas/s use
tramas pequeñas en el plano de control y reserve el MTU para datos.

---

## 6. Scripts de automatización

### 6.1 Barrido de latencia por hilos

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

> El formato exacto de las líneas cambia entre modos; revise siempre el archivo
> crudo. Los campos mostrados arriba corresponden a la salida real del modo
> `latency`.

### 6.2 Perfil del sistema

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

### 6.3 Benchmark continuo

```bash
#!/bin/bash
mkdir -p logs
while true; do
    TS=$(date +%Y%m%d_%H%M%S)
    ./build/chronosrt_bench --mode latency --duration=60 > "logs/bench_${TS}.log" 2>&1
    sleep 3600
done
```

---

## 7. Análisis con `perf`

```bash
# Contadores de CPU
perf stat -B -e cycles,instructions,cache-references,cache-misses,branch-misses \
    ./build/chronosrt_bench --mode latency --duration 30

# Perfil con llamadas
perf record -g ./build/chronosrt_bench --mode latency --duration 30
perf report --stdio

# Flame graph
perf record -F 99 -g ./build/chronosrt_bench --mode latency --duration 60
perf script | stackcollapse-perf.pl | flamegraph.pl > latency.svg
```

Para el determinismo del kernel:

```bash
rtla timerlat top -d 60s
cyclictest -t1 -p 80 -n -i 1000 -l 60000
```

---

## 8. Criterios de éxito

| Requisito | Criterio | Cómo se mide |
|---|---|---|
| Latencia | < 10 µs en p99.9 | `--mode jitter` + `rtla` en kernel RT |
| Throughput | > 10 M eventos/s | `--mode throughput` |
| Jitter | σ < 1 µs | `--mode latency`/`jitter` |
| Uso de CPU | < 5 % en reposo | `--mode stress` + `perf stat` |
| Coste por trama | Menor que el tiempo de transmisión | `--mode frame` |
| CRC-32C | Backend HW activo | `--mode crc` |

**No compare cifras de hosts distintos.** El mismo build en un kernel sin RT
puede multiplicar el WCET por dos órdenes de magnitud; es un resultado válido,
no un fallo de la biblioteca.

### 8.1 Script de validación

```bash
#!/bin/bash
set -u
PASS=true

echo "=== Validación ChronosRT ==="

# 1. Suites
if ctest --test-dir build --output-on-failure >/tmp/ctest.log 2>&1; then
    echo "[PASS] ctest 9/9"
else
    echo "[FAIL] ctest"
    PASS=false
fi

# 2. Kernel RT
if grep -q 'PREEMPT_RT: si\|PREEMPT_RT: yes' /tmp/kernel.log 2>/dev/null; then
    echo "[PASS] PREEMPT_RT"
else
    echo "[WARN] sin PREEMPT_RT: las métricas de latencia no son concluyentes"
fi

# 3. HAL
./build/chronosrt_bench --mode crc --duration 1 > /tmp/crc.log 2>&1
if grep -q 'sse4.2-x4\|armv8-crc' /tmp/crc.log; then
    echo "[PASS] CRC-32C por instrucción"
else
    echo "[WARN] CRC-32C en fallback software"
fi

# 4. Coste por trama
./build/chronosrt_bench --mode frame > /tmp/frame.log 2>&1
if grep -q 'construir PDU' /tmp/frame.log; then
    echo "[PASS] rutas de trama medidas"
else
    echo "[FAIL] no se pudo medir el coste por trama"
    PASS=false
fi

$PASS && echo "=== OK ===" || echo "=== REVISAR ==="
```

---

## 9. Hardware de referencia y objetivos

### 9.1 Host de medida de este documento

| Dato | Valor |
|---|---|
| CPU | AMD Ryzen 7 4800H (Zen 2, 16 hilos) |
| Kernel | Linux 7.2.11-1-cachyos-deckify (`PREEMPT_DYNAMIC`) |
| Compilador | GCC 16.2.1 |
| CMake | 4.4.4 |
| CRC-32C | `sse4.2-x4` |
| FEC | `avx2` |
| Build | `Release` `-O3 -march=native` |

### 9.2 Objetivos por plataforma (especificación EtherTux, no medidos aquí)

| Plataforma | Latencia P99 | Jitter (σ) | Throughput |
|---|---|---|---|
| SBC 1 Gbps (OPi 5, RPi 5, x86-64) | < 10 µs | < 1 µs (con PHC) | 800–940 Mbps reservando QoS |
| MCU W6100 (100 Mbps) | 80–300 µs | 30–80 µs | ~50–91 Mbps |
| MCU W6300 (100 Mbps) | 50–150 µs | 20–50 µs | ~91 Mbps |

Estas cifras son **objetivos de diseño** de
[Protocolo EtherTux.md](Protocolo%20EtherTux.md) §12; deben validarse en cada
plataforma con el procedimiento de §2.

---

## 10. Reportes automatizados

```bash
#!/bin/bash
OUT="reports/$(date +%Y%m%d)"
mkdir -p "$OUT"

for mode in latency throughput jitter stress wcet frame crc; do
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

---

## 11. Verificación de las suites

```bash
ctest --test-dir build --output-on-failure
```

| Suite | Aserciones |
|---|---:|
| `chronosrt_test` | 20 |
| `chronosrt_test_lockfree` | 26 |
| `chronosrt_test_rt` | 30 |
| `chronosrt_test_resilience` | 29 |
| `chronosrt_test_integration` | 27 |
| `chronosrt_test_network` | 58 |
| `chronosrt_test_hwaccel` | 40 |
| `chronosrt_test_ethertux` | 103 |
| `chronosrt_test_ethertux_v010` | 152 |
| **Total** | **485** |

Estado del host de referencia: **9/9 suites, 100 %**, en 0,06 s.
