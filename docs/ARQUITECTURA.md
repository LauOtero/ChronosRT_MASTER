# Arquitectura de ChronosRT Master

**Versión:** 0.1.0 · **ABI:** `0x000100` · **Lenguaje:** C11

## 1. Resumen ejecutivo

ChronosRT Master es una **biblioteca de tiempo real determinista para Linux** que
convierte una SBC ARM64/ARMv7 o un equipo x86-64 en un nodo de comunicación
industrial. No es un kernel ni sustituye a PREEMPT_RT: es la capa de usuario que
explota un kernel RT ya ajustado y **degrada de forma controlada** cuando falta
aceleración por hardware o el kernel no está preparado.

Su superficie cubre cuatro pilares:

| Pilar | Mecanismos |
|---|---|
| **Tiempo real** | `SCHED_DEADLINE`/`SCHED_FIFO`, afinidad a núcleos aislados, `timerfd`, `mlockall` + *prefault*, pools pre-asignados y cero `malloc` en el camino caliente |
| **Determinismo** | `CLOCK_MONOTONIC`, *timeouts* acotados, `poll()` con vencimiento absoluto, colas lock-free sin ABA y CRC-32C por instrucción |
| **Resiliencia** | snapshot A/B con *seqlock*, WAL con CRC + `fsync` por lotes, *golden image* atómica, watchdog, validación de WKC, FEC Reed-Solomon y FRM |
| **Ultra rendimiento** | AVX-512/AVX2/NEON, `PACKET_QDISC_BYPASS`, anillo `PACKET_TX_RING`, varios datagramas por trama y *zero-copy* de la carga útil |

Estado verificado en el host de referencia (AMD Ryzen 7 4800H, Linux 7.2.11,
GCC 16.2.1, CMake 4.4.4): build `Release` **0 errores / 0 warnings**, `ctest`
**9/9 suites**, **485 aserciones**, **295 símbolos `ChronosRT_*`**.

---

## 2. Objetivos de rendimiento

| Métrica | Objetivo de diseño | Nota |
|---|---|---|
| Latencia de eventos | < 10 µs | Medible con `chronosrt_bench --mode latency`; depende del kernel/aislamiento |
| Throughput interno | > 10 M eventos/s | Objetivo del runtime de eventos |
| Jitter (σ) | < 1 µs | Requiere PREEMPT_RT + `isolcpus`/`nohz_full` |
| Uso de CPU | < 5 % nominal | En operación event-driven |
| Coste de CPU por trama | Muy por debajo del tiempo de transmisión | Medido: 0,28 % (EtherCAT) y 1,45 % (EtherTux) sobre 1 Gbps |

Los valores de latencia/jitter **no se midieron en el host de verificación**
(kernel `PREEMPT_DYNAMIC`, sin RT y sin permisos de red). El proyecto publica
mediciones reales de coste de CPU por trama y de la HAL; los objetivos de
latencia del protocolo proceden de la especificación EtherTux y se validan en
cada plataforma con `chronosrt_bench` y `rtla`/`cyclictest`.

---

## 3. Vista de componentes

```
┌──────────────────────────────────────────────────────────────────────┐
│                         Aplicación (C11)                             │
│         init/shutdown · eventos · benchmark · métricas               │
├──────────────────────────────────────────────────────────────────────┤
│                     Superficie pública (ABI C)                       │
│  chronosrt.h · kernel · sched · rt · mem · ring · mpmc · bounded     │
│  resilience · obs · simd · hwaccel · protocol · ethercat · ethertux  │
├──────────────────────────────────────────────────────────────────────┤
│                      Protocolos (mutuamente excluyentes)             │
│  ┌─────────────────────────┐   ┌─────────────────────────────────┐   │
│  │ EtherCAT  (0x88A4)      │   │ EtherTux  (0x88B5)              │   │
│  │ ec_frame/socket/master  │   │ et_frame/fec/qos/sched/esm      │   │
│  │ ec_slave/dc/mailbox     │   │ et_sync/topology/mbx/flowagg/frm│   │
│  │                         │   │ et_socket/master                │   │
│  └─────────────────────────┘   └─────────────────────────────────┘   │
├──────────────────────────────────────────────────────────────────────┤
│                       Runtime y servicios                            │
│  eventos · pools · SCHED_* · histograma/watchdog · resiliencia · obs │
├──────────────────────────────────────────────────────────────────────┤
│                  HAL de aceleración (runtime)                        │
│  CRC-32C (SSE4.2 / ARMv8.1 / slice-by-8) · RS SIMD · PHC/LaunchTime  │
├──────────────────────────────────────────────────────────────────────┤
│                 Plataforma: detección de kernel                      │
│  versión · PREEMPT_RT · isolcpus/nohz_full/rcu_nocbs · EEVDF         │
│  sched_ext · io_uring · AF_XDP · SCHED_DEADLINE · memfd_secret       │
├──────────────────────────────────────────────────────────────────────┤
│              Kernel Linux 5.15+ … 7.x · NIC · SBC/MCU                │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 4. Estructura del código

`src/` se organiza **por subsistema**; `include/chronosrt/` mantiene una
superficie pública **plana y estable** para no romper los `#include`.

| Ruta | Responsabilidad |
|---|---|
| `src/core/` | `chronosrt_core` (eventos), `_mem`, `_sched`, `_rt`, `_resilience`, `_obs` |
| `src/platform/` | `chronosrt_kernel.c`: sondeo y validación del kernel |
| `src/hwaccel/` | `chronosrt_hwaccel.c`: CRC-32C, GF(2^8), Reed-Solomon, PHC |
| `src/protocols/` | `chronosrt_protocol.c` (selección) |
| `src/protocols/ethercat/` | `ec_frame`, `ec_socket`, `ec_master`, `ec_slave`, `ec_dc`, `ec_mailbox` |
| `src/protocols/ethertux/` | `et_frame`, `et_fec`, `et_qos`, `et_sched`, `et_esm`, `et_sync`, `et_topology`, `et_mbx`, `et_flowagg`, `et_frm`, `et_socket`, `et_master` |
| `src/integration/` | `chronosrt_bridge.c`: puente genérico hacia un *sink* externo |
| `src/simd/` | `chronosrt_simd.c`: *batch zero/copy* y *hash* vectorizados |
| `src/benchmark/` | `chronosrt_benchmark.c`: motor de benchmarking |
| `tools/benchmark.c` | CLI `chronosrt_bench` |
| `tests/` | 9 suites de `ctest` |

---

## 5. Análisis de complejidad

### 5.1 Runtime

| Operación | Complejidad | Justificación |
|---|---|---|
| `event_publish` | O(1) | Pool pre-asignado + push a cola lock-free; sin `malloc` |
| `event_subscribe` | O(1) amortizado | Pop de cola lock-free |
| Pool `alloc/free` | O(1) | *Free-list* sobre cola *bounded* (Vyukov), sin ABA |
| `get_cycles` / `timestamp` | O(1) | `rdtscp` serializado / `perf_event_open` / `CLOCK_MONOTONIC` |
| Snapshot A/B | O(1) + O(n) copia | *Seqlock* sobre doble búfer |
| WAL append | O(1) amortizado | CRC por entrada + `fsync` por lotes |

### 5.2 EtherCAT

| Operación | Complejidad | Nota |
|---|---|---|
| Construir/parsear datagrama | O(n) | Copia de datos; 10 B de cabecera + datos + 2 B WKC |
| `send_processdata` | O(n) | Un `LRW` con todo el IOMap |
| `receive_processdata` | O(n) | Valida WKC esperado |
| Escaneo del bus | O(N esclavas) | Auto-incremento hasta la primera que no responde |
| `config_map` | O(N esclavas) | Dos pasadas SM2/SM3 + FMMU |

### 5.3 EtherTux

| Operación | Complejidad | Nota |
|---|---|---|
| `pdu_build` | O(n) | CRC-32C sobre cabecera + carga |
| `pdu_build_header` | O(n) | CRC sin copiar la carga (dos iovecs) |
| `pdu_parse` | O(n) | Valida CRC |
| `qos_admit` | O(1) | Token bucket por clase |
| `sched_window_at` | O(1) | Aritmética modular del superframe |
| `topology_on_frame` | O(1) | Búsqueda por MAC en registro acotado (64 nodos) |
| `flowagg_build/parse` | O(k) | k flujos agregados |
| `frm_note_rx` | O(1) | Ventana de deduplicación acotada |

---

## 6. Estructuras lock-free

### 6.1 Ring SPSC (`chronosrt_ring.h`)

```c
typedef struct ChronosRT_ring {
    _Alignas(CHRONOSRT_CACHELINE) void* buffer[RING_CAPACITY];  /* 4096 */
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint32_t head;
    _Atomic uint32_t tail;
    _Atomic uint32_t count;
} ChronosRT_ring_t;
```

- `_Alignas(64)` en el buffer y en los índices evita *false sharing*.
- *Software prefetch* de la celda siguiente en `push`/`pop` (PF-7).
- Un solo productor y un solo consumidor: sin CAS.

### 6.2 Cola MPMC acotada (`chronosrt_bounded.h`)

Algoritmo de **D. Vyukov**: celdas pre-asignadas con `sequence`, índices
monótonos y `acquire/release`.

- Sin asignación por operación.
- Sin ABA (no hay `free` de nodos en el camino caliente).
- Es la cola usada por el pool de memoria y por el runtime.

La cola **Michael-Scott** (`chronosrt_mpmc.h`) se conserva como implementación de
referencia y material didáctico; no se usa en el camino caliente porque asigna
un nodo por `push` y libera en `pop` (riesgo de ABA).

---

## 7. Memoria

### 7.1 Pools pre-asignados

```c
typedef struct ChronosRT_mem_pool ChronosRT_mem_pool_t;   /* opaco */

ChronosRT_mem_pool_t* ChronosRT_mem_pool_create_ex(uint32_t pool_size,
                                                   uint32_t block_size,
                                                   bool hugepage,
                                                   bool numa_local);
```

- Bloques alineados a línea de caché.
- *Free-list* lock-free (sin ABA).
- `ChronosRT_mem_pool_prefault()` toca todos los bloques para eliminar *page
  faults* en el camino caliente.
- `ChronosRT_mem_lock_all()` aplica `mlockall(MCL_CURRENT|MCL_FUTURE)`.

### 7.2 Hugepages y NUMA

- Hugepages transparentes de 2 MB (`MADV_HUGEPAGE`) y `vm.nr_hugepages` para
  páginas explícitas.
- `ChronosRT_mem_current_numa_node()` y `mbind` para asignación local al nodo.
- El objetivo es reducir TLB misses y tráfico inter-socket.

---

## 8. Planificación y aislamiento

### 8.1 Políticas

| Política | Uso | API |
|---|---|---|
| `SCHED_DEADLINE` | Hilos periódicos con presupuesto y admisión | `ChronosRT_sched_set_deadline_current` |
| `SCHED_FIFO` | *Fallback* o hilos de prioridad fija | `ChronosRT_sched_set_fifo_current` |
| Afinidad | Fijar el hilo a un núcleo aislado | `ChronosRT_sched_set_affinity_current` |
| `timerfd` | Tick periódico sin `usleep` | `ChronosRT_tick_init/wait` |

### 8.2 Aislamiento verificado

`ChronosRT_sched_isolation_probe()` lee `/proc/cmdline` y comprueba
`isolcpus`, `nohz_full` y `rcu_nocbs`; `strict_ok` exige los tres tokens con CPUs
válidas. `ChronosRT_sched_isolation_print()` avisa por cada núcleo no aislado.

### 8.3 Kernel 7.x

El módulo `chronosrt_kernel` detecta:

- versión (`uname` + `sscanf`);
- PREEMPT_RT (`/sys/kernel/realtime`, `/proc/version`,
  `/sys/kernel/debug/sched/preempt`);
- aislamiento de CPU;
- capacidades por versión: **EEVDF** (≥ 6.6), **`sched_ext`** (≥ 6.12),
  **`io_uring`**, **AF_XDP**, **`SCHED_DEADLINE`**, **`memfd_secret`**.

`support_level`: `0` no soportado, `1` básico, `2` tiempo real, `3` kernel 7.x
pleno. `ChronosRT_init` avisa si no hay PREEMPT_RT sin cambiar la semántica de
éxito.

---

## 9. EtherCAT

Maestro **autónomo** sobre `AF_PACKET`/`0x88A4`, sin SOEM, IgH ni DPDK.

| Módulo | Responsabilidad |
|---|---|
| `ec_frame.c` | Serialización/parseo puro de datagramas y tramas |
| `ec_socket.c` | Socket `AF_PACKET`, promiscuo, `PACKET_QDISC_BYPASS`, `SO_PRIORITY`, redundancia doble NIC, `poll()` con *deadline* |
| `ec_master.c` | Ciclo de vida, intercambio de datagramas (`APRD`…`FRMW`) y PDO (`LRW`) |
| `ec_slave.c` | Escaneo, SII/EEPROM, SM/FMMU, máquina AL con ACK de error |
| `ec_dc.c` | Distributed Clocks: SYSTIME, retardos, SYNC0/SYNC1 |
| `ec_mailbox.c` | Buzón SM0/SM1 y CoE/SDO (expedito y segmentado) |

Propiedades: sin `malloc` en el camino caliente, `CLOCK_MONOTONIC`, WKC
esperado, detección de esclavas perdidas, contadores de error del ESC,
reintentos acotados y bit **M** para encadenar datagramas.

---

## 10. EtherTux

Protocolo determinista sobre Ethernet comercial (`0x88B5`), cabecera de **26 B**
en el cable.

### 10.1 Principios

1. **Time-triggered** para P0–P2 (ranura fija por trama crítica).
2. **Antibloqueo**: P0 es *express*; el planificador no encola baja prioridad si
   hay una P0 pendiente.
3. **FEC Reed-Solomon** con decodificación de borrados (sin RTT).
4. **Cero congestión por diseño**: admisión + token buckets jerárquicos +
   *backpressure*.
5. **LAPM** con histéresis (80 % sube, 60 % baja).
6. **CRC-32C** por instrucción en todas las SBC objetivo.

### 10.2 Módulos

| Módulo | Responsabilidad |
|---|---|
| `et_frame.c` | PDU, cabecera de 26 B y CRC-32C |
| `et_fec.c` | Bloques FEC RS(10,8)/RS(16,8) |
| `et_qos.c` | Token buckets, *backpressure* y LAPM |
| `et_sched.c` | Superframe, ventanas y ranuras |
| `et_esm.c` | Ciclo de vida `INIT → CONFIG → SAFE → OP → FAULT` |
| `et_sync.c` | ET-Sync y monitor de ventana de sincronización |
| `et_topology.c` | Registro de nodos, ET-WKC, descubrimiento y Hot Connect |
| `et_mbx.c` | Mailbox ET-MBX (8 canales, segmentos de 512 B, ACK/NACK) |
| `et_flowagg.c` | Agregación de flujos (`FlowCount > 1`) |
| `et_frm.c` | Redundancia lógica de flujo y deduplicación |
| `et_socket.c` | `AF_PACKET`/`0x88B5`, `SO_TXTIME`, `PACKET_TX_RING` |
| `et_master.c` | Ciclo de vida, envío/recepción, nodos y heartbeat |

### 10.3 Ventanas del superframe (1 ms por defecto)

| Ventana | Duración | Prioridades |
|---|---:|---|
| Control | 100 µs | P0, P1 |
| Determinista | 200 µs | P2 |
| Rate-constrained | 300 µs | P3, P4 |
| Best-effort | 400 µs | P5 |

P0 puede transmitir en cualquier ventana.

### 10.4 Capa de transmisión

| Modo | Mecanismo | Uso |
|---|---|---|
| `SYSCALL` | `sendmsg`/`sendmmsg` (hasta 16 tramas) | Compatibilidad universal |
| `RING` | `PACKET_TX_RING` mapeado en usuario; un `send()` vacía el lote | Por defecto en `AUTO` si el kernel lo permite |

---

## 11. HAL de aceleración

Sondeo en tiempo de ejecución (`/proc/self/auxv`, CPUID, `ETHTOOL_GET_TS_INFO`),
sin `#if` en el código de aplicación.

| Operación | ARMv8.1+ | ARMv7 | x86-64 |
|---|---|---|---|
| CRC-32C | `crc32cb/c/h/w/x` | *slice-by-8* | `crc32` SSE4.2, 4 flujos |
| GF(2^8) / RS | NEON | escalar | AVX2 |
| PHC / Launch Time | `ETHTOOL_GET_TS_INFO` | igual | igual |

Se usa **CRC-32C (Castagnoli)** porque es el único polinomio acelerado por
instrucción en ambas arquitecturas. La única SBC de la matriz sin CRC32 por
hardware es la **Banana Pi M2 Zero** (Cortex-A7), que cae al *fallback*
software.

Capacidades publicadas: `crc32c_hw`, `crc32c_backend`, `fec_simd`,
`fec_backend`, `armv8_crypto`, `phc`, `launch_time`, vendor/marca/familia/modelo
de CPU y matriz de ISA (SSE4.2, PCLMUL, AVX/AVX2/AVX-512, VPCLMULQDQ, GFNI,
VAES, SHA, AES, BMI2, ADX).

---

## 12. Resiliencia

| Mecanismo | Implementación |
|---|---|
| Snapshot A/B | `ChronosRT_snapshot_*` con *seqlock* y doble búfer |
| WAL | CRC por entrada, `fsync` por lotes, `replay` que se detiene en la primera entrada corrupta |
| Golden image | `<path>.tmp` → `fsync` → `rename` atómico |
| Watchdog | SW con `last_kick_ns` + detección de `/dev/watchdog` |
| Red EtherCAT | WKC esperado, `is_lost`, contadores ESC, ACK AL, reintentos, doble NIC |
| Red EtherTux | FEC, FRM (duplicado + deduplicación), cuarentena `FAULT`, Hot Connect |

---

## 13. Integración con `libtuxrt_core`

- Espacios de nombres disjuntos: `ChronosRT_*` frente a `tuxrt_*`.
- ChronosRT se publica como **proveedor** exportando
  `tuxrt_provider_descriptor`; **no** enlaza contra `libtuxrt_core`.
- El consumidor lo descubre con `dlopen`/`dlsym`.
- El puente `ChronosRT_bridge_*` drena eventos y los entrega a un *sink*
  genérico; no hay puente inverso.

---

## 14. WCET y costes medidos

### 14.1 Cotas de diseño (no verificadas en este host)

Valores del análisis inicial del proyecto. **No son mediciones** y no deben
usarse como certificación: el WCET real se obtiene con `ChronosRT_hist_*` y
`rtla`/`cyclictest` en la plataforma objetivo.

| Operación | Cota de diseño |
|---|---|
| Publicación de evento | ≤ 50 ns |
| Suscripción (amortizada) | ≤ 100 ns |
| Ring push/pop | ≤ 20 / 25 ns |
| MPMC push/pop (referencia) | ≤ 80 / 90 ns |
| Copia SIMD 64 B | ≤ 15 ns |
| Timestamp | ≤ 10 ns |

### 14.2 Coste medido por trama (sin NIC)

Host: AMD Ryzen 7 4800H (Zen 2), Linux 7.2.11, `Release` `-O3`, con
`-march=native`, `chronosrt_bench --mode frame`, descontando el bucle vacío.

| Operación | 64 B | 512 B | 1464 B |
|---|---:|---:|---:|
| EtherTux: construir PDU (con copia) | 59,8 ns | 74,1 ns | 180,8 ns |
| EtherTux: construir cabecera (sin copia) | 43,1 ns | 52,6 ns | 140,8 ns |
| EtherTux: parsear + validar CRC | 37,1 ns | 47,9 ns | 138,6 ns |
| EtherCAT: construir datagrama | 9,95 ns (32 B) | 15,4 ns (256 B) | 24,0 ns (1024 B) |
| EtherCAT: parsear datagrama | 3,47 ns (32 B) | 3,82 ns (256 B) | 3,68 ns (1024 B) |

Otras rutas medidas: Flow Aggregator 8×32 B = **48,0 ns** (parseo 14,4 ns;
agregado + PDU 121,2 ns frente a 390,7 ns en 8 PDUs), admisión QoS P2/576 B =
**16,8 ns**, ventana TDMA = **4,2 ns**.

### 14.3 Capacidad de la HAL (mismo host)

| Operación | Resultado |
|---|---|
| CRC-32C, trama de 60 B | 2,94 GB/s · 49,0 M tramas/s |
| CRC-32C, trama de 552 B | 10,76 GB/s · 19,5 M tramas/s |
| CRC-32C, trama de 1504 B | 9,70 GB/s · 6,45 M tramas/s |
| Backend activo | `sse4.2-x4`; FEC `avx2` |

Lectura: el límite práctico es el **enlace**, no la CPU. A 1 Gbps y MTU completa
el techo físico es ~82 k tramas/s; con tramas de control de 20 B asciende a
~1,49 M tramas/s.

---

## 15. Ajuste por plataforma

| Plataforma | Notas |
|---|---|
| **Raspberry Pi 4/5** | NEON; RPi 5 (BCM2712/A76) con extensiones criptográficas ARMv8; PHC/Launch Time con NIC i226 |
| **Jetson Nano/Orin** | ARM64; Orin (A78AE) con SVE en compiladores que lo soportan; contador de ciclos vía `perf_event_open` |
| **Orange Pi 5 / ROCK 5B** | RK3588 (A76+A55); NEON; `rk2_crypto` aún en parcheo, no usar en camino crítico |
| **Intel NUC / x86-64 RT** | AVX2 o AVX-512 según CPU; CRC-32C SSE4.2; PHC/Launch Time en i210/i225/i226 |
| **Banana Pi M2 Zero** | Cortex-A7 sin CRC32 por hardware: *fallback* software |

La matriz completa (29 perfiles) vive en `scripts/lib/sbc.sh` y se consulta con
`./scripts/build.sh --list`.

---

## 16. Verificación

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Resultado en el host de referencia:

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

Los detalles de protocolo están en el [Manual Técnico §11](MANUAL_TECNICO.md)
(EtherCAT) y §13 (EtherTux); las mediciones, en
[BENCHMARKING.md](BENCHMARKING.md).
