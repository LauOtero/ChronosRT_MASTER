# Investigación y Análisis — ChronosRT Master

**Versión:** 0.1.0 · **Fecha:** noviembre 2026

Este documento recoge el estado del arte que fundamenta el diseño de ChronosRT
Master, el análisis de SOEM que guio la reimplementación del maestro EtherCAT, las
técnicas de red determinista adoptadas en EtherTux y las mediciones reales
disponibles en el repositorio.

---

## 1. Estado del arte en tiempo real sobre Linux

### 1.1 PREEMPT_RT

El parche PREEMPT_RT convierte la mayor parte del kernel en preemptible. Desde
**Linux 6.12 está integrado en mainline** y se activa con `preempt=full`; ya no
es imprescindible instalar un kernel `-rt` binario.

| Característica | Kernel genérico | PREEMPT_RT |
|---|---|---|
| Preemption en kernel | Mínima | ~95 % |
| Latencia máxima de interrupción | ~10 ms | < 100 µs |
| Latencia máxima de planificación | Variable | < 1 ms |
| Determinismo | Bajo | Alto |

Configuración recomendada para kernel 7.x:

```
isolcpus=2-7 nohz_full=2-7 rcu_nocbs=2-7 preempt=full rcu_nocb_poll \
tsc=reliable skew_tick=1 nosoftlockup mce=ignore_ce idle=poll
```

ChronosRT **detecta** este estado (`ChronosRT_kernel_probe`) y avisa si falta
RT; no intenta aplicar el ajuste por el operador.

### 1.2 EEVDF y `sched_ext`

- **EEVDF** es el planificador por defecto de `SCHED_OTHER` desde 6.6. Irrelevante
  para tiempo real: los hilos críticos usan `SCHED_FIFO`/`SCHED_DEADLINE`.
- **`sched_ext`** (6.12+) permite cargar planificadores BPF. ChronosRT lo
  **detecta** (`sched_ext`, `sched_ext_enabled`); un planificador BPF consciente
  de EtherCAT/DC queda como línea exploratoria.

### 1.3 Otras capacidades modernas

| Capacidad | Versión | Uso en ChronosRT |
|---|---|---|
| `io_uring` | ≥ 5.1 | Detección; sustituye `epoll`/`aio` en rutas de I/O |
| AF_XDP | ≥ 4.18 | Detección; ruta *zero-copy* exploratoria |
| `memfd_secret` | ≥ 5.14 | Detección para secretos fuera del espacio mapeable |
| `SCHED_DEADLINE` | ≥ 3.14 | Planificación con presupuesto y admisión |
| `timerfd` | — | Tick periódico sin `usleep` |

### 1.4 Contadores de ciclos

- **x86-64:** `rdtscp` serializado (evita reordenamiento respecto a la medida).
- **ARM64:** `perf_event_open(PERF_COUNT_HW_CPU_CYCLES)` con *fallback* a
  `CLOCK_MONOTONIC`. Leer `pmccntr_el0` sin habilitar `PMUSERENR_EL0` provoca
  *trap*/`SIGILL`, por eso no se usa directamente.

---

## 2. Análisis de SOEM y decisiones de reimplementación

El informe [informe ethercat.md](informe%20ethercat.md) descompone SOEM en capa
de red, protocolo, máquina de estados, relojes distribuidos, buzón/CoE y PDO.
ChronosRT adopta los mecanismos imprescindibles **sin heredar** sus limitaciones:

| Mecanismo de SOEM | Decisión en ChronosRT |
|---|---|
| Socket `AF_PACKET` + `SO_DONTROUTE` + promiscuo | Adoptado: `AF_PACKET`/`0x88A4`, promiscuo, `PACKET_QDISC_BYPASS`, `SO_PRIORITY` |
| Buffers indexados TX/RX y `EC_BUF_*` | Adoptado como buffers pre-asignados por índice de datagrama |
| Índice libre con `pthread_mutex` y búsqueda lineal | Sustituido por buffers pre-asignados y contador monótono; sin mutex en el camino caliente |
| Redundancia doble NIC | Adoptada (`open_redundant`), con transmisión por ambas y primera respuesta válida |
| `malloc` de mailboxes en caliente | Eliminado: buzón de tamaño fijo (`ChronosRT_EC_MAX_MAILBOX`) |
| `pthread_mutex` con herencia de prioridad | Evitado en el camino crítico; no hay locks por datagrama |
| `poll()` + `recvmsg()` con *timeout* | Adoptado, con vencimiento **monotónico absoluto** |
| Timestamps software | `CLOCK_MONOTONIC`; PHC/Launch Time por `ETHTOOL_GET_TS_INFO` |
| Múltiples `memcpy` | Reducido a lo imprescindible; EtherTux añade construcción sin copia |
| Comandos `APRD/APWR/BRD/BWR/LRW/ARMW/FRMW` | Implementados en `chronosrt_ethercat.h` |
| ESM con ACK de error y `statecheck` | Implementado con *timeout* acotado |
| DC: latch de tiempos, retardos, SYNC0/1 | Implementado en `ec_dc.c` |
| CoE/SDO expedito y segmentado | Implementado en `ec_mailbox.c` |
| WKC esperado por grupo | Implementado y validado en cada ciclo |

**Limitaciones de SOEM que motivaron el rediseño:** asignación dinámica en
caliente, mutex en la ruta de datos, búsqueda lineal de índices, recepción con
`ppoll()` sin *busy-poll* controlado, *timestamps* software y ausencia de
*shaping* de TX. ChronosRT prioriza **determinismo**: buffers fijos, sin
`malloc`, *timeouts* acotados y una capa de socket propia.

---

## 3. Programación lock-free

### 3.1 Ring SPSC

Un productor y un consumidor con `head`/`tail`/`count` atómicos y
`memory_order_acquire/release`. El buffer y los índices se alinean a 64 B para
evitar *false sharing*; se hace *prefetch* de la celda siguiente.

### 3.2 MPMC acotada (Vyukov)

Sustituye a Michael-Scott en el camino caliente:

- celdas pre-asignadas con `sequence`;
- índices monótonos (`enqueue_pos`/`dequeue_pos`);
- sin `free` de nodos → **sin ABA**;
- sin asignación por operación.

La cola Michael-Scott se conserva como referencia; su `push` asigna un nodo y su
`pop` lo libera, lo que es inaceptable para un camino de tiempo real.

### 3.3 Memoria

- Pool pre-asignado con *free-list* lock-free.
- `mlockall(MCL_CURRENT|MCL_FUTURE)` + *prefault* para eliminar *page faults*.
- Hugepages de 2 MB (`MADV_HUGEPAGE`) y `mbind` NUMA.

---

## 4. Redes deterministas: fundamentos de EtherTux

### 4.1 Time-triggered y DetNet

Las redes *time-triggered* asignan a cada trama crítica una **ranura fija** en un
ciclo repetitivo; sin contención no hay colisiones ni encolamiento. **DetNet
(IETF RFC 8655)** establece que la garantía de QoS se logra eliminando la
congestión en el puerto de salida. EtherTux combina ambos: TDMA + control de
admisión + token buckets jerárquicos.

### 4.2 TSN: Time-Aware Shaper y Frame Preemption

- **802.1Qbv (TAS):** GCL que abre/cierra colas en instantes precisos. EtherTux
  implementa una versión software mediante su planificador de ventanas.
- **802.1Qbu/802.3br (Frame Preemption):** una trama *express* puede interrumpir
  una *preemptable*. Es la base del **antibloqueo** de EtherTux; si la NIC no lo
  soporta, el planificador no encola baja prioridad cuando hay una P0 pendiente.

### 4.3 FEC Reed-Solomon

Recuperación de paquetes perdidos **sin retransmisión**, eliminando la
dependencia del RTT. EtherTux usa RS(10,8) (25 % de overhead) y RS(16,8) (100 %),
con decodificación de **borrados** por eliminación gaussiana sobre GF(2^8)
(polinomio `0x11D`).

### 4.4 Token buckets, backpressure y LAPM

- Token bucket por clase (P0–P5).
- *Backpressure* consciente de prioridad: P0 nunca se descarta; P1→P2→P4; P5 se
  descarta (*drop-tail*).
- **LAPM** (Load-Adaptive Priority Migration): migración dinámica con histéresis
  (80 % sube, 60 % baja) para evitar oscilaciones.

### 4.5 Aportaciones de EtherCAT adaptadas al switch

| Concepto EtherCAT | Adaptación EtherTux |
|---|---|
| Working Counter (WKC) | **ET-WKC** (2 B por trama): cada nodo que procesa la trama lo incrementa |
| ESM | **ET-ESM**: `INIT → CONFIG → SAFE → OP → FAULT` |
| Distributed Clocks | **ET-Sync**: modelo de retardo calibrado del switch + monitor de ventana |
| Descubrimiento por broadcast | **Descubrimiento dirigido** + `DiscoveryReply` unicast |
| Anillo/redundancia física | **FRM**: redundancia lógica de flujo con conmutación de camino |

### 4.6 Identidad local determinista y supresión del *flooding*

En una red con switches comerciales, una trama dirigida a una MAC no aprendida
se **inunda a todos los puertos**, multiplicando el tráfico y añadiendo jitter a
puertos que compiten por ancho de banda. Las entradas CAM expiran por
envejecimiento (300 s típicos), así que el problema reaparece de forma periódica.

La solución adoptada es una **identidad de 16 bits** codificada en una MAC
administrada localmente:

```
0x02 : 0x00 : 0x00 : 0x00 : ID[15:8] : ID[7:0]
```

El bit U/L a 1 y el I/G a 0 la convierten en *unicast* local, de modo que el
switch aprende la entrada y conmuta solo al puerto del nodo. Un *heartbeat* de
baja prioridad (P5, una trama cada 60 s) mantiene viva la entrada de forma
indefinida.

El esquema tiene **dos niveles** con responsabilidades separadas:

1. **Nivel 1 — receptor, camino crítico.** El ID candidato se deriva de la MAC
   con CRC-32C plegado por XOR de mitades (5,2 ns medidos, sin ramas ni bucles).
   El plegado importa: la truncación simple descarta la mitad alta del hash y
   garantiza colisiones. Este nivel es un **filtro**, no un asignador.
2. **Nivel 2 — maestro, solo admisión.** La paradoja del cumpleaños hace que con
   16 bits aparezca una colisión con probabilidad > 50 % hacia los ~300 nodos
   coexistente. El maestro mantiene `{MAC → ID}` y un bitmap de IDs, y resuelve
   la colisión con un **cursor secuencial determinista** (sin `rand()` ni
   temporizadores) fuera del superframe. El resultado es único en régimen
   permanente.

Complementos adoptados: *backoff* de admisión derivado del CRC de la MAC
(reproducible y distinto entre nodos), rechazo de MAC clonada (misma MAC con
distinto `nonce`), tiempo de gracia antes de liberar un ID sin *heartbeat* y
persistencia atómica de la tabla para conservar la identidad tras un reinicio.

La implementación usa una **tabla hash abierta** de 256 ranuras por defecto
(potencia de 2) con estados *libre*/*activo*/*tombstone* y un bitmap de IDs
ocupados: ≈ 14,4 KB en total. La búsqueda es O(1) media y la admisión O(1)
amortizada; la liberación recorre la tabla (O(slots)) y queda fuera del
superframe. El filtro de destino (6,2 ns) evita el parseo y la validación de
CRC (138,6 ns con MTU) de las tramas dirigidas a otros nodos.

---

## 5. Aceleración por hardware

### 5.1 CRC-32C frente a CRC-32 IEEE

Se elige **CRC-32C (Castagnoli)** porque es el único polinomio con aceleración
por instrucción en **ARMv8.1+ y x86-64**:

- ARM64: `crc32cb/c/h/w/x` (Cortex-A53 en adelante).
- x86-64: instrucción `crc32` de SSE4.2.

Así todos los nodos calculan el mismo valor con el menor coste. La única SBC de
la matriz sin CRC32 por hardware es la **Banana Pi M2 Zero** (Cortex-A7), que usa
*slice-by-8*.

### 5.2 Implementación del CRC en x86-64

El cuello de botella original era el CRC (197 ns de 260 ns por trama de 1464 B):
`crc32q` tiene latencia 3 ciclos y *throughput* 1/ciclo, así que un solo flujo
queda limitado a ~3 B/ciclo. La solución implementada:

1. **Cuatro flujos independientes** en tres niveles (512/128/32 B) que ocultan la
   latencia y combinan con tablas de *skip*.
2. **Despacho sin `pthread_once`**: puntero de función con escritura atómica y
   carga relajada.
3. **Construcción sin copia** (`ChronosRT_et_pdu_build_header`) y envío con dos
   iovecs.
4. **Anillo `PACKET_TX_RING`**: una llamada al sistema por lote.
5. **Envío por lotes** (`sendmmsg`, hasta 16 tramas).
6. **Velocidad real del enlace** (`open_auto` + `link_speed_bps`).

Resultado medido en el host de referencia: **1,85 GB/s → 10,98 GB/s** con MTU
completa (×5,9) y **1,23 → 7,30 M tramas/s** de capacidad de CPU.

> **Evaluado y descartado:** *folding* con PCLMULQDQ/VPCLMULQDQ. Se implementó y
> se comparó contra la referencia software; al no reproducir el CRC exacto en la
> calibración se retiró en lugar de arriesgar la integridad del protocolo. En
> Zen 2 el *throughput* de `pclmulqdq` es 1/ciclo, igual que `crc32`, por lo que
> la ganancia habría sido nula en ese host.

### 5.3 SIMD para FEC

La codificación RS es una combinación lineal con coeficientes constantes, lo que
permite vectorizar GF(2^8) con tablas de *nibble* + `shuffle` (NEON/AVX2). La
decodificación de borrados usa eliminación gaussiana.

### 5.4 Motores criptográficos del SoC

| SoC | Motor | Estado | Uso |
|---|---|---|---|
| Allwinner H616/H618 | `sun8i-ce` | Mainline | Cifrado opcional del plano de gestión |
| Rockchip RK3328/RK3399 | `rk_crypto` V1 | Mainline (≥ 6.2) | Cifrado opcional |
| Rockchip RK3588/RK3566 | `rk2_crypto` | En parcheo | **No usar** en producción |
| Broadcom BCM2712 (RPi 5) | ARMv8 Crypto | Soportado | Cifrado opcional |
| Amlogic S905X3 | Crypto Engine | Soportado | Cifrado opcional |
| Amlogic S905X | Sin extensiones ARMv8 Crypto | — | Software |

Ninguno se usa en el camino crítico.

---

## 6. Aceleración y topología de MCU

| Controlador | Throughput real | Uso recomendado |
|---|---|---|
| W5500 (SPI, 32 KB) | ~25–55 Mbps | P3/P4 (telemetría) |
| W6100 (Fast SPI / QSPI, 32 KB) | ~50 / ~91 Mbps | P2–P4 |
| W6300 (QSPI 150 MHz, 64 KB) | ~91 Mbps | P0–P2 (control de movimiento) |

Para P0–P2, **IRQ y SPI-DMA no son opcionales**: el *polling* introduce jitter
incompatible con tiempo real.

---

## 7. Análisis de complejidad y costes

### 7.1 Complejidad

| Operación | Tiempo | Espacio | Descripción |
|---|---|---|---|
| Ring push/pop | O(1) | O(1) | 3 operaciones atómicas |
| MPMC bounded push/pop | O(1) | O(1) | CAS sobre índices, sin asignación |
| Pool alloc/free | O(1) | O(1) | Free-list lock-free |
| PDU EtherTux build/parse | O(n) | O(1) | CRC-32C sobre cabecera + carga |
| Datagrama EtherCAT build/parse | O(n) | O(1) | Copia de datos + WKC |
| `qos_admit` | O(1) | O(1) | Token bucket |
| `sched_window_at` | O(1) | O(1) | Aritmética modular |
| `topology_on_frame` | O(1) | O(1) | Registro acotado (64 nodos) |
| `aalid_candidate` | O(1) | O(1) | CRC-32C de 6 B plegado, sin ramas |
| `aalid_admit` | O(1) amortizado | O(1) | Hash + cursor secuencial (solo admisión) |
| `aalid_lookup` | O(1) medio | O(1) | Tabla hash abierta con sondeo acotado |
| `aalid_release` | O(slots) | O(1) | Recorrido de la tabla (administrativa) |

### 7.2 Costes medidos (host AMD Ryzen 7 4800H, Linux 7.2.11)

| Operación | Coste |
|---|---|
| EtherCAT: construir datagrama 1024 B | 24,0 ns |
| EtherCAT: parsear datagrama | 3,5–3,8 ns |
| EtherTux: construir PDU 1464 B | 180,8 ns |
| EtherTux: construir cabecera sin copia 1464 B | 140,8 ns |
| EtherTux: parsear + validar CRC 1464 B | 138,6 ns |
| Flow Aggregator 8×32 B | 48,0 ns |
| Admisión QoS P2/576 B | 16,8 ns |
| Ventana TDMA | 4,2 ns |
| ID candidato (CRC-32C plegado) | 5,2 ns |
| Filtro de destino del maestro | 6,2 ns |
| Admisión de un nodo nuevo | 62 ns |
| Búsqueda `{MAC → ID}` con 4096 nodos | 15,4 ns |

Detalle completo en [BENCHMARKING.md](BENCHMARKING.md).

---

## 8. Resiliencia y observabilidad

| Técnica | Referencia | Implementación |
|---|---|---|
| Snapshot A/B | Patrón *seqlock* | `ChronosRT_snapshot_*` |
| WAL | Registro de intenciones idempotente | `ChronosRT_wal_*` con CRC y `fsync` por lotes |
| Golden image | Escritura atómica | `<path>.tmp` → `fsync` → `rename` |
| Watchdog | Heartbeat + HW | `ChronosRT_watchdog_*`; detección de `/dev/watchdog` |
| Traza de baja sobrecarga | `trace_marker` (ftrace) | `ChronosRT_trace_*` |
| Contadores en vivo | Atómicos | `ChronosRT_counters_*` |
| Autotest de colas | *Fuzzing* concurrente | `ChronosRT_selftest_bounded_queue/ring` |

---

## 9. Validación y metodología

1. Build limpio `Release` en el host de referencia.
2. `ctest` con **10 suites / 547 aserciones**.
3. `chronosrt_bench --mode frame`, `--mode crc` y `--mode aalid` para coste de
   CPU, HAL e identidad local.
4. `rtla timerlat` / `cyclictest` en kernel PREEMPT_RT para latencia y jitter.
5. Validación física de EtherCAT/EtherTux con `CAP_NET_RAW` y NIC dedicada
   (no disponible en el entorno de verificación).

Lo que **no** se puede concluir sin hardware: latencia end-to-end del bus,
recuperación FEC con pérdidas reales, precisión de ET-Sync y comportamiento de
DC en esclavas reales.

---

## 10. Referencias

1. **ChronosRT Master (GitHub):** <https://github.com/LauOtero/ChronosRT_MASTER>
2. **Linux Kernel Preemption:** `Documentation/preempt.txt`
3. **PREEMPT_RT mainline:** integración desde Linux 6.12 (`preempt=full`)
4. **EEVDF:** planificador por defecto de `SCHED_OTHER` desde Linux 6.6
5. **`sched_ext`:** planificadores BPF desde Linux 6.12
6. **Michael-Scott Queue:** PODC 1996
7. **Vyukov Bounded MPMC Queue:** <https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue>
8. **DPDK Performance Report:** Intel Corporation
9. **AF_XDP:** `linux/if_xdp.h`, `xsk.h`
10. **AVX-512 Optimization Guide:** Intel
11. **ARM NEON Programmer's Guide:** ARM Ltd
12. **"EtherCAT: Ethernet for Semiconductor"** — ETG.1000 / IEC 61158
13. **SOEM** — análisis interno en [informe ethercat.md](informe%20ethercat.md)
14. **DetNet Architecture (RFC 8655)** — <https://datatracker.ietf.org/doc/rfc8655/>
15. **IEEE 802.1Qbv / 802.1Qbu / 802.3br** — TAS y Frame Preemption
16. **LAPM (IETF draft)** — migración dinámica de prioridad
17. **WIZnet W5500 / W6100 / W6300** — <https://wiznet.io/products/ethernet-chips>
18. **Rockchip `rk_crypto` / `rk2_crypto`** — soporte mainline y parcheo
19. **Allwinner `sun8i-ce`** — soporte mainline H616/H618
20. **`rtla` / `cyclictest`** — validación de latencia en Linux RT
