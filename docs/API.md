# ChronosRT Master — Referencia de API

**Versión:** 0.1.0 · **ABI:** `0x000100` · **SONAME:** `libchronosrt.so.0` · **Lenguaje:** C11

## Convenciones

- Cabecera de inclusión plana: `#include <chronosrt/chronosrt.h>` (el directorio
  `include/chronosrt` se instala como tal).
- Todos los símbolos públicos usan el prefijo `ChronosRT_` y la macro de visibilidad
  `CHRONOSRT_API`; la biblioteca se compila con `-fvisibility=hidden`.
- Enlace: `-lchronosrt -lpthread -lm -ldl`.
- Las funciones que devuelven `int` usan `>= 0` para éxito (a menudo el número de
  esclavas/nodos afectados) y `< 0`/`-1` para error, salvo que se indique otra cosa.
- La API es **estable**: solo se añade al final, nunca se rompe el ABI.

---

## 1. Códigos de estado

`ChronosRT_status_t` (`chronosrt.h`):

| Código | Valor | Descripción |
|---|---:|---|
| `ChronosRT_SUCCESS` | 0 | Operación exitosa |
| `ChronosRT_ERROR_INIT` | -1 | Error de inicialización |
| `ChronosRT_ERROR_MEMORY` | -2 | Error de asignación de memoria |
| `ChronosRT_ERROR_CPU` | -3 | Error de configuración de CPU |
| `ChronosRT_ERROR_TIMEOUT` | -4 | Timeout |
| `ChronosRT_ERROR_INVALID` | -5 | Parámetros inválidos |
| `ChronosRT_ERROR_NO_HARDWARE` | -6 | Hardware no soportado |
| `ChronosRT_ERROR_NOTSUP` | -7 | Operación no soportada en este kernel/HW |
| `ChronosRT_ERROR_STATE` | -8 | Estado inválido para la operación |
| `ChronosRT_ERROR_IO` | -9 | Error de E/S |
| `ChronosRT_ERROR_CORRUPT` | -10 | Corrupción de integridad (CRC/ECC) |
| `ChronosRT_ERROR_FULL` | -11 | Recurso lleno (cola/ring) |
| `ChronosRT_ERROR_BACKPRESSURE` | -12 | El productor debe reducir la tasa (QoS P0–P4) |
| `ChronosRT_ERROR_DROPPED` | -13 | Trama descartada por política best-effort (P5) |
| `ChronosRT_ERROR_NOTFOUND` | -14 | Nodo/flujo/canal no registrado |

---

## 2. Núcleo: ciclo de vida, eventos y métricas

Cabecera: `chronosrt/chronosrt.h`.

### 2.1 Configuración

```c
typedef struct {
    uint32_t enabled_cpus[ChronosRT_MAX_CPUS];  /* ChronosRT_MAX_CPUS = 16 */
    uint32_t num_cpus;
    uint32_t event_pool_size;
    uint32_t batch_size;
    uint32_t stack_size_kb;
    bool     use_hugepages;
    bool     use_numa_aware;
    bool     use_simd;
    bool     use_cpu_affinity;
    ChronosRT_platform_t platform;

    /* Extensiones ABI v1 (F2–F7) */
    bool     lock_memory;          /* mlockall + prefault (RT-3)            */
    bool     use_hugepages_2mb;    /* MADV_HUGEPAGE en el pool (RT-3)       */
    bool     strict_isolation;     /* exigir isolcpus/nohz_full/rcu_nocbs   */
    bool     use_deadline;         /* SCHED_DEADLINE para hilos RT (RT-1)   */
    bool     use_bounded_mpmc;     /* cola bounded Vyukov (PF-3)            */
    bool     use_timerfd;          /* tick por timerfd en vez de usleep     */
    uint32_t rt_priority;          /* prioridad SCHED_FIFO (0 = auto)       */
    uint64_t deadline_runtime_ns;  /* presupuesto de ejecución              */
    uint64_t deadline_period_ns;   /* periodo                               */
} ChronosRT_config_t;
```

### 2.2 Ciclo de vida

```c
ChronosRT_status_t ChronosRT_init(ChronosRT_handle_t* handle,
                                  const ChronosRT_config_t* config);
ChronosRT_status_t ChronosRT_shutdown(ChronosRT_handle_t handle);
```

`ChronosRT_init` sondea el kernel (`ChronosRT_kernel_probe`), informa del nivel de
soporte y **avisa** (sin fallar) si no hay PREEMPT_RT.

```c
ChronosRT_config_t cfg = {0};
cfg.event_pool_size = 65536;
cfg.lock_memory     = true;
cfg.use_deadline    = true;
cfg.use_timerfd     = true;

ChronosRT_handle_t rt;
if (ChronosRT_init(&rt, &cfg) != ChronosRT_SUCCESS) {
    return 1;
}
```

### 2.3 Eventos

```c
ChronosRT_status_t ChronosRT_event_publish(ChronosRT_handle_t handle,
                                           const void* data, uint32_t size);
ChronosRT_status_t ChronosRT_event_subscribe(ChronosRT_handle_t handle,
                                             ChronosRT_event_t* event,
                                             uint32_t timeout_us);
```

- `data` se copia en el evento; como máximo caben **56 bytes**.
- `event_publish` **no asigna memoria**: usa el pool pre-asignado y una cola
  lock-free (cero `malloc` en el camino caliente).

```c
typedef struct ChronosRT_CACHE_ALIGNED {
    volatile uint64_t sequence;
    volatile uint64_t timestamp_ns;
    volatile uint32_t cpu_id;
    volatile uint8_t  data[56];
} ChronosRT_event_t;   /* 64 B, alineado a línea de caché */
```

### 2.4 Benchmark y métricas

```c
ChronosRT_status_t ChronosRT_benchmark_run(ChronosRT_handle_t handle,
                                           ChronosRT_benchmark_mode_t mode,
                                           uint32_t duration_sec,
                                           ChronosRT_metrics_t* metrics);
ChronosRT_status_t ChronosRT_metrics_report(const ChronosRT_metrics_t* metrics);
```

Modos: `ChronosRT_MODE_LATENCY`, `_THROUGHPUT`, `_JITTER`, `_STRESS`, `_WCET`.

```c
typedef struct ChronosRT_CACHE_ALIGNED {
    uint64_t total_events;
    uint64_t total_cycles;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    uint64_t avg_latency_ns;
    uint64_t stddev_ns;
    double   cpu_usage_percent;
    double   cache_miss_rate;
    double   branch_miss_rate;
    uint64_t memory_bandwidth_mbps;
    uint64_t wcet_ns;
    uint64_t best_case_ns;
} ChronosRT_metrics_t;
```

### 2.5 Utilidades y metadatos ABI

```c
uint64_t ChronosRT_get_cycles(void);          /* contador de ciclos robusto */
uint64_t ChronosRT_get_timestamp_ns(void);    /* CLOCK_MONOTONIC en ns      */
ChronosRT_platform_t ChronosRT_detect_platform(void);

const char* ChronosRT_abi_version_string(void);  /* "0.1.0"        */
uint32_t    ChronosRT_abi_version(void);         /* 0x000100       */
```

En x86-64 `get_cycles` usa `rdtscp` serializado; en ARM64 usa
`perf_event_open(PERF_COUNT_HW_CPU_CYCLES)` con *fallback* a `CLOCK_MONOTONIC`.

Plataformas: `RPI4`, `RPI5`, `JETSON_NANO`, `JETSON_ORIN`, `INTEL_NUC`,
`ORANGE_PI5`, `ROCK5B`, `X86_64_RT`, `GENERIC`.

---

## 3. Kernel y planificación de tiempo real

### 3.1 Sondeo del kernel — `chronosrt_kernel.h`

```c
typedef struct {
    uint32_t major, minor, patch;
    char     release[128];
    bool     preempt_rt;
    bool     isolcpus, nohz_full, rcu_nocbs;
    bool     eevdf;              /* >= 6.6  */
    bool     sched_ext;          /* >= 6.12 */
    bool     io_uring;
    bool     af_xdp;
    bool     sched_deadline;
    bool     sched_ext_enabled;  /* disponible realmente en runtime */
    bool     memfd_secret;
    uint32_t support_level;      /* 0..3; 3 = kernel 7.x pleno */
} ChronosRT_kernel_info_t;

ChronosRT_status_t ChronosRT_kernel_probe(ChronosRT_kernel_info_t* info);
void               ChronosRT_kernel_print(const ChronosRT_kernel_info_t* info);
int                ChronosRT_kernel_summary(const ChronosRT_kernel_info_t* info,
                                            char* buf, size_t len);
bool               ChronosRT_kernel_is_supported(const ChronosRT_kernel_info_t* info);
```

Soporte mínimo declarado: Linux **5.15**; soporte pleno: **7.x**. Niveles:
`0` no soportado, `1` básico, `2` tiempo real, `3` kernel 7.x pleno.

### 3.2 Planificación y aislamiento — `chronosrt_sched.h`

```c
typedef struct {
    uint64_t runtime_ns;
    uint64_t deadline_ns;
    uint64_t period_ns;
    int      cpu;          /* -1 = sin afinidad */
} ChronosRT_deadline_attr_t;

bool               ChronosRT_sched_deadline_supported(void);
ChronosRT_status_t ChronosRT_sched_set_deadline_current(const ChronosRT_deadline_attr_t* attr);
ChronosRT_status_t ChronosRT_sched_set_fifo_current(int priority);
ChronosRT_status_t ChronosRT_sched_set_affinity_current(int cpu);

typedef struct {
    bool     isolcpus, nohz_full, rcu_nocbs;
    uint32_t num_isolated;
    int      isolated[ChronosRT_MAX_CPUS];
    bool     strict_ok;
} ChronosRT_isolation_t;

ChronosRT_status_t ChronosRT_sched_isolation_probe(ChronosRT_isolation_t* out);
bool               ChronosRT_sched_cpu_is_isolated(const ChronosRT_isolation_t* iso, int cpu);
void               ChronosRT_sched_isolation_print(const ChronosRT_isolation_t* iso);
int                ChronosRT_sched_online_cpus(void);
```

`SCHED_DEADLINE` aplica admisión en el kernel; si el kernel lo rechaza se devuelve
`ChronosRT_ERROR_NOTSUP` y la aplicación puede caer a `SCHED_FIFO`.

---

## 4. Instrumentación de tiempo real — `chronosrt_rt.h`

### 4.1 Histograma de latencias (RT-6)

```c
#define CHRONOSRT_HIST_BUCKETS 64

typedef struct {
    uint64_t bucket_ns;
    uint64_t buckets[CHRONOSRT_HIST_BUCKETS];
    uint64_t count, sum_ns, sum_sq_ns, min_ns, max_ns, overflow;
} ChronosRT_hist_t;

void     ChronosRT_hist_init(ChronosRT_hist_t* h, uint64_t bucket_ns);
void     ChronosRT_hist_reset(ChronosRT_hist_t* h);
void     ChronosRT_hist_record(ChronosRT_hist_t* h, uint64_t latency_ns);
uint64_t ChronosRT_hist_count(const ChronosRT_hist_t* h);
uint64_t ChronosRT_hist_mean_ns(const ChronosRT_hist_t* h);
uint64_t ChronosRT_hist_stddev_ns(const ChronosRT_hist_t* h);
uint64_t ChronosRT_hist_percentile_ns(const ChronosRT_hist_t* h, double p);
uint64_t ChronosRT_hist_wcet_ns(const ChronosRT_hist_t* h);
uint64_t ChronosRT_hist_best_ns(const ChronosRT_hist_t* h);
void     ChronosRT_hist_print(const ChronosRT_hist_t* h, const char* label);
```

### 4.2 Watchdog y tick (RS-4, RT-8)

```c
typedef struct {
    _Atomic uint64_t last_kick_ns;
    uint64_t         timeout_ns;
} ChronosRT_watchdog_t;

ChronosRT_status_t ChronosRT_watchdog_init(ChronosRT_watchdog_t* wd, uint64_t timeout_ns);
void               ChronosRT_watchdog_kick(ChronosRT_watchdog_t* wd);
bool               ChronosRT_watchdog_expired(const ChronosRT_watchdog_t* wd);
uint64_t           ChronosRT_watchdog_elapsed_ns(const ChronosRT_watchdog_t* wd);
void               ChronosRT_watchdog_destroy(ChronosRT_watchdog_t* wd);
bool               ChronosRT_watchdog_hw_available(void);

typedef struct { int fd; uint64_t period_ns; } ChronosRT_tick_t;

ChronosRT_status_t ChronosRT_tick_init(ChronosRT_tick_t* tick, uint64_t period_ns);
int                ChronosRT_tick_fd(const ChronosRT_tick_t* tick);
int                ChronosRT_tick_wait(ChronosRT_tick_t* tick);
void               ChronosRT_tick_destroy(ChronosRT_tick_t* tick);
```

`ChronosRT_watchdog_hw_available()` solo comprueba la existencia de
`/dev/watchdog`; no lo abre (no arma el temporizador).

---

## 5. Memoria — `chronosrt_mem.h`

```c
ChronosRT_status_t ChronosRT_mem_pool_init(ChronosRT_mem_pool_t* pool,
                                           uint32_t pool_size, uint32_t block_size);
ChronosRT_mem_pool_t* ChronosRT_mem_pool_create(uint32_t pool_size, uint32_t block_size);
ChronosRT_mem_pool_t* ChronosRT_mem_pool_create_ex(uint32_t pool_size, uint32_t block_size,
                                                   bool hugepage, bool numa_local);
ChronosRT_status_t ChronosRT_mem_pool_init_ex(ChronosRT_mem_pool_t* pool,
                                              uint32_t pool_size, uint32_t block_size,
                                              bool hugepage, bool numa_local);
void               ChronosRT_mem_pool_delete(ChronosRT_mem_pool_t* pool);

void*              ChronosRT_mem_pool_alloc(ChronosRT_mem_pool_t* pool);
void               ChronosRT_mem_pool_free(ChronosRT_mem_pool_t* pool, void* block);
void               ChronosRT_mem_pool_destroy(ChronosRT_mem_pool_t* pool);
uint32_t           ChronosRT_mem_pool_available(ChronosRT_mem_pool_t* pool);
uint32_t           ChronosRT_mem_pool_capacity(ChronosRT_mem_pool_t* pool);
ChronosRT_status_t ChronosRT_mem_pool_prefault(ChronosRT_mem_pool_t* pool);

ChronosRT_status_t ChronosRT_mem_lock_all(void);
void               ChronosRT_mem_unlock_all(void);
size_t             ChronosRT_mem_hugepage_size(void);

int                ChronosRT_mem_current_numa_node(void);
int                ChronosRT_mem_numa_nodes(void);
```

El pool usa una *free-list* sobre una cola *bounded* lock-free: `alloc/free` son
O(1), sin ABA y sin asignación en el camino caliente.

---

## 6. Estructuras lock-free

### 6.1 Ring SPSC — `chronosrt_ring.h` (header-only)

```c
#define RING_CAPACITY 4096

typedef struct ChronosRT_ring {
    _Alignas(CHRONOSRT_CACHELINE) void* buffer[RING_CAPACITY];
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint32_t head;
    _Atomic uint32_t tail;
    _Atomic uint32_t count;
} ChronosRT_ring_t;

void       ChronosRT_ring_init(ChronosRT_ring_t* ring);
bool       ChronosRT_ring_push(ChronosRT_ring_t* ring, void* item);
void*      ChronosRT_ring_pop(ChronosRT_ring_t* ring);
uint32_t   ChronosRT_ring_count(ChronosRT_ring_t* ring);
bool       ChronosRT_ring_is_empty(ChronosRT_ring_t* ring);
bool       ChronosRT_ring_is_full(ChronosRT_ring_t* ring);
```

### 6.2 MPMC Michael-Scott — `chronosrt_mpmc.h` (header-only, referencia)

```c
void  ChronosRT_mpmc_init(ChronosRT_mpmc_queue_t* queue);
void  ChronosRT_mpmc_destroy(ChronosRT_mpmc_queue_t* queue);
bool  ChronosRT_mpmc_push(ChronosRT_mpmc_queue_t* queue, void* data);
void* ChronosRT_mpmc_pop(ChronosRT_mpmc_queue_t* queue);
uint64_t ChronosRT_mpmc_count(ChronosRT_mpmc_queue_t* queue);
```

Es la implementación de referencia; el camino caliente usa la variante *bounded*
(PF-3) porque la MPMC con `free` de nodos tiene riesgo de ABA y asigna por
operación.

### 6.3 Cola MPMC acotada (Vyukov) — `chronosrt_bounded.h` (header-only)

```c
typedef struct {
    _Atomic uint64_t sequence;
    void* data;
} ChronosRT_bounded_cell_t;

typedef struct ChronosRT_bounded_queue {
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint64_t enqueue_pos;
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint64_t dequeue_pos;
    _Alignas(CHRONOSRT_CACHELINE) ChronosRT_bounded_cell_t* cells;
    uint64_t capacity, mask;
    void* raw;
} ChronosRT_bounded_queue_t;

ChronosRT_status_t ChronosRT_bounded_init(ChronosRT_bounded_queue_t* q, uint64_t capacity);
void               ChronosRT_bounded_destroy(ChronosRT_bounded_queue_t* q);
bool               ChronosRT_bounded_push(ChronosRT_bounded_queue_t* q, void* data);
bool               ChronosRT_bounded_pop(ChronosRT_bounded_queue_t* q, void** out);
uint64_t           ChronosRT_bounded_count(const ChronosRT_bounded_queue_t* q);
bool               ChronosRT_bounded_empty(const ChronosRT_bounded_queue_t* q);
uint64_t           ChronosRT_bounded_capacity(const ChronosRT_bounded_queue_t* q);
```

Sin asignación por operación, sin ABA y con `acquire/release` explícitos.

---

## 7. Resiliencia — `chronosrt_resilience.h`

### 7.1 Snapshot A/B con seqlock (RS-1)

```c
typedef struct {
    uint32_t size;
    _Atomic uint32_t seq;
    _Atomic uint32_t active;
    uint8_t* buf[2];
} ChronosRT_snapshot_t;

ChronosRT_status_t ChronosRT_snapshot_init(ChronosRT_snapshot_t* s, uint32_t size);
ChronosRT_status_t ChronosRT_snapshot_write(ChronosRT_snapshot_t* s, const void* data, uint32_t size);
ChronosRT_status_t ChronosRT_snapshot_read(const ChronosRT_snapshot_t* s, void* out, uint32_t size);
uint32_t           ChronosRT_snapshot_sequence(const ChronosRT_snapshot_t* s);
void               ChronosRT_snapshot_destroy(ChronosRT_snapshot_t* s);
```

### 7.2 WAL con CRC y `fsync` por lotes (RS-3)

```c
#define CHRONOSRT_WAL_PATH_MAX 256
#define CHRONOSRT_WAL_REC_MAX  (1u << 20)

typedef struct {
    int      fd;
    char     path[CHRONOSRT_WAL_PATH_MAX];
    uint32_t batch;
    uint32_t pending;
    uint64_t seq;
} ChronosRT_wal_t;

ChronosRT_status_t ChronosRT_wal_open(ChronosRT_wal_t* w, const char* path, uint32_t batch);
ChronosRT_status_t ChronosRT_wal_append(ChronosRT_wal_t* w, const void* rec, uint32_t len);
ChronosRT_status_t ChronosRT_wal_sync(ChronosRT_wal_t* w);
uint64_t           ChronosRT_wal_seq(const ChronosRT_wal_t* w);
void               ChronosRT_wal_close(ChronosRT_wal_t* w);

int ChronosRT_wal_replay(const char* path,
                         int (*cb)(const void* rec, uint32_t len, void* ud),
                         void* ud);
```

`wal_replay` verifica el CRC de cada entrada y se detiene en la primera corrupta
(escritura truncada).

### 7.3 Golden image (RS-6)

```c
ChronosRT_status_t ChronosRT_golden_save(const char* path, const void* data, uint32_t len);
ChronosRT_status_t ChronosRT_golden_load(const char* path, void* out, uint32_t cap, uint32_t* out_len);
uint32_t           ChronosRT_golden_crc(const void* data, uint32_t len);
```

Escritura atómica: `<path>.tmp` → `fsync` → `rename`.

---

## 8. Observabilidad — `chronosrt_obs.h`

```c
typedef struct {
    _Atomic uint64_t published, consumed, dropped, errors, forwarded;
} ChronosRT_counters_t;

void ChronosRT_counters_init(ChronosRT_counters_t* c);
void ChronosRT_counters_add(ChronosRT_counters_t* c, _Atomic uint64_t* field, uint64_t delta);
void ChronosRT_counters_reset(ChronosRT_counters_t* c);
void ChronosRT_counters_print(const ChronosRT_counters_t* c, const char* label);

typedef struct { int fd; bool active; } ChronosRT_trace_t;

ChronosRT_status_t ChronosRT_trace_init(ChronosRT_trace_t* t);
int                ChronosRT_trace_mark(ChronosRT_trace_t* t, const char* msg);
void               ChronosRT_trace_destroy(ChronosRT_trace_t* t);

typedef struct {
    uint64_t produced, consumed, duplicates, lost;
} ChronosRT_lockfree_stats_t;

ChronosRT_status_t ChronosRT_selftest_bounded_queue(uint32_t producers, uint32_t consumers,
                                                    uint64_t total,
                                                    ChronosRT_lockfree_stats_t* out);
ChronosRT_status_t ChronosRT_selftest_ring(uint64_t total, ChronosRT_lockfree_stats_t* out);
```

`trace` usa `trace_marker` de ftrace si hay permisos; si no, es un *no-op* seguro.
Los autotests validan ausencia de pérdidas y duplicados en las colas.

---

## 9. SIMD — `chronosrt_simd.h`

```c
void     ChronosRT_simd_batch_zero(void* ptr, size_t count);
void     ChronosRT_simd_batch_copy(void* dst, const void* src, size_t count);
uint32_t ChronosRT_simd_hash(const void* data, size_t size);
```

Despacho por arquitectura (AVX-512/AVX2/NEON) con *fallback* portable.

---

## 10. HAL de aceleración — `chronosrt_hwaccel.h`

### 10.1 Capacidades

```c
typedef struct {
    bool        crc32c_hw;
    const char* crc32c_backend;   /* "sse4.2-x4" | "armv8-crc" | "slice-by-8" */
    bool        fec_simd;
    const char* fec_backend;      /* "neon" | "avx2" | "scalar"               */
    bool        armv8_crypto;
    bool        phc;
    bool        launch_time;

    char        cpu_vendor[13];
    char        cpu_brand[49];
    uint32_t    cpu_family, cpu_model, cpu_stepping;

    bool isa_sse42, isa_pclmul, isa_avx, isa_avx2;
    bool isa_avx512f, isa_avx512vl, isa_avx512bw, isa_avx512dq;
    bool isa_vpclmul, isa_gfni, isa_vaes, isa_sha, isa_aes;
    bool isa_bmi2, isa_adx;
} ChronosRT_hw_caps_t;

const ChronosRT_hw_caps_t* ChronosRT_hw_caps(void);
ChronosRT_status_t ChronosRT_hw_caps_for_interface(const char* ifname,
                                                   ChronosRT_hw_caps_t* out);
```

### 10.2 CRC-32C, GF(2^8) y Reed-Solomon

```c
uint32_t ChronosRT_crc32c(const void* data, size_t len);
uint32_t ChronosRT_crc32c_raw(uint32_t seed, const void* data, size_t len);
uint32_t ChronosRT_crc32c_sw(const void* data, size_t len);
uint32_t ChronosRT_crc32c_sw_raw(uint32_t seed, const void* data, size_t len);

const char*        ChronosRT_crc32c_active_backend(void);
uint32_t           ChronosRT_crc32c_selftest(const char** first_failing);
ChronosRT_status_t ChronosRT_crc32c_force_backend(const char* name);

uint8_t ChronosRT_gf8_mul(uint8_t a, uint8_t b);
uint8_t ChronosRT_gf8_inv(uint8_t a);

ChronosRT_status_t ChronosRT_rs_encode(uint8_t k, uint8_t n,
                                       const uint8_t* const* data,
                                       uint8_t* const* parity,
                                       uint32_t len);
ChronosRT_status_t ChronosRT_rs_decode(uint8_t k, uint8_t n,
                                       uint8_t* const* blocks,
                                       const bool* present,
                                       uint32_t len);

uint64_t ChronosRT_tx_timestamp_ns(void);
```

- `ChronosRT_crc32c` usa init/xorout `0xFFFFFFFF`; `_raw` permite encadenar sin
  inversión. Vector estándar: `"123456789"` → `0xE3069283`.
- `rs_encode/decode` usan RS(N,K) sistemático sobre GF(2^8) con polinomio
  `0x11D`; `decode` reconstruye hasta N−K borrados.
- `crc32c_selftest` devuelve el número de backends que **no** coinciden con la
  referencia software (0 = todos correctos).

---

## 11. Selección de protocolo — `chronosrt_protocol.h`

```c
typedef enum {
    ChronosRT_PROTOCOL_ETHERCAT = 0,
    ChronosRT_PROTOCOL_ETHERTUX  = 1
} ChronosRT_protocol_t;

ChronosRT_protocol_t ChronosRT_protocol_default(void);
ChronosRT_protocol_t ChronosRT_protocol_from_string(const char* name);
const char*          ChronosRT_protocol_name(ChronosRT_protocol_t protocol);
ChronosRT_protocol_t ChronosRT_protocol_from_env(void);
```

La elección es de **tiempo de configuración** (`CHRONOSRT_PROTOCOL=ethercat|ethertux`)
y no cambia en caliente. Los dos backends nunca se usan a la vez sobre la misma
interfaz.

---

## 12. Maestro EtherCAT — `chronosrt_ethercat.h`

Maestro autónomo sobre `AF_PACKET`, EtherType **0x88A4**, conforme a
IEC 61158 / ETG.1000. Sin SOEM, IgH ni DPDK.

### 12.1 Límites y constantes

```c
#define ChronosRT_EC_ETHERTYPE        0x88A4
#define ChronosRT_EC_MAX_SLAVES       64
#define ChronosRT_EC_MAX_FRAME        1518
#define ChronosRT_EC_MAX_DATAGRAM_DATA 1024
#define ChronosRT_EC_MAX_SM           8
#define ChronosRT_EC_MAX_FMMU         4
#define ChronosRT_EC_MAX_MAILBOX      256
#define ChronosRT_EC_MAX_INTERFACES   16
#define ChronosRT_EC_BROADCAST_ADDR   0x0000u
#define ChronosRT_EC_NODE_OFFSET      0x1000u
```

Estados AL: `ChronosRT_EC_STATE_NONE/INIT/PREOP/BOOT/SAFEOP/OP/ACK`.
Comandos: `NOP`, `APRD/APWR/APRW`, `FPRD/FPWR/FPRW`, `BRD/BWR/BRW`,
`LRD/LWR/LRW`, `ARMW/FRMW`.

Registros ESC expuestos: `TYPE 0x0000`, `REVISION 0x0008`,
`STATION_ADDR 0x0010`, `STATION_ALIAS 0x0012`, `DL_STATUS 0x0110`,
`AL_CONTROL 0x0120`, `AL_STATUS 0x0130`, `AL_STATUS_CODE 0x0134`,
`RX_ERROR 0x0300`, `CRC_ERROR 0x0308`, `WD_COUNTER 0x0442`,
`DC_SYSTEM_TIME 0x0910`, `DC_SYNC_ACT 0x0981`, `DC_START0 0x0990`,
`DC_CYCLE0 0x09A0`.

### 12.2 Interfaces

```c
uint32_t ChronosRT_ec_interface_count(void);
const ChronosRT_ec_interface_t* ChronosRT_ec_interface(uint32_t index);
int      ChronosRT_ec_interface_index(const char* name);
```

```c
typedef struct {
    char    name[32];
    uint8_t mac[6];
    int     ifindex;
    bool    is_up;
} ChronosRT_ec_interface_t;
```

### 12.3 Helpers puros de trama

```c
uint32_t ChronosRT_ec_datagram_build(uint8_t* out, uint32_t out_size,
                                     uint8_t cmd, uint8_t idx,
                                     uint32_t address,
                                     const void* data, uint16_t len,
                                     bool more);
uint32_t ChronosRT_ec_datagram_parse(const uint8_t* in, uint32_t in_len,
                                     ChronosRT_ec_datagram_t* out);
uint16_t ChronosRT_ec_frame_header(uint8_t out[2], uint16_t datagram_bytes);
```

`address = ADP | (ADO << 16)`. `more = true` activa el bit M.

### 12.4 Ciclo de vida

```c
ChronosRT_ec_master_t* ChronosRT_ec_master_create(void);
ChronosRT_status_t ChronosRT_ec_master_open(ChronosRT_ec_master_t* master, const char* ifname);
ChronosRT_status_t ChronosRT_ec_master_open_redundant(ChronosRT_ec_master_t* master,
                                                      const char* ifname,
                                                      const char* redundant_ifname);
void               ChronosRT_ec_master_close(ChronosRT_ec_master_t* master);
void               ChronosRT_ec_master_destroy(ChronosRT_ec_master_t* master);
bool               ChronosRT_ec_master_is_open(const ChronosRT_ec_master_t* master);
const char*        ChronosRT_ec_master_interface(const ChronosRT_ec_master_t* master);
void               ChronosRT_ec_master_set_timeout(ChronosRT_ec_master_t* master,
                                                   uint32_t timeout_us);
```

### 12.5 Escaneo y configuración

```c
int      ChronosRT_ec_master_scan(ChronosRT_ec_master_t* master);
int      ChronosRT_ec_master_config_init(ChronosRT_ec_master_t* master);
int      ChronosRT_ec_master_config_map(ChronosRT_ec_master_t* master);
uint32_t ChronosRT_ec_master_slave_count(const ChronosRT_ec_master_t* master);
const ChronosRT_ec_slave_t* ChronosRT_ec_master_slave(const ChronosRT_ec_master_t* master,
                                                      uint32_t index);
const uint8_t* ChronosRT_ec_master_iomap(const ChronosRT_ec_master_t* master);
uint32_t       ChronosRT_ec_master_iomap_size(const ChronosRT_ec_master_t* master);
```

```c
typedef struct {
    uint16_t position, station_addr, alias, config_addr;
    uint32_t vendor_id, product_code, revision, serial;
    uint8_t  al_state;
    uint16_t al_status_code, dl_status;
    uint8_t  ports;
    bool     has_dc, is_lost;
    uint16_t mbx_proto;
    uint32_t i_bytes, o_bytes;
    uint32_t i_offset, o_offset;
    int64_t  dc_offset;
    int32_t  dc_delay;
} ChronosRT_ec_slave_t;
```

### 12.6 Registros y direccionamiento

```c
int ChronosRT_ec_master_read_reg(...);        /* APRD  */
int ChronosRT_ec_master_write_reg(...);       /* APWR  */
int ChronosRT_ec_master_fp_read(...);         /* FPRD  */
int ChronosRT_ec_master_fp_write(...);        /* FPWR  */
int ChronosRT_ec_master_broadcast_read(...);  /* BRD   */
int ChronosRT_ec_master_broadcast_write(...); /* BWR   */
int ChronosRT_ec_master_lrw(...);             /* LRW   */
int ChronosRT_ec_master_lrd(...);             /* LRD   */
int ChronosRT_ec_master_lwr(...);             /* LWR   */
int ChronosRT_ec_master_armw(...);            /* ARMW  */
int ChronosRT_ec_master_frmw(...);            /* FRMW  */
```

### 12.7 Máquina de estados AL

```c
int ChronosRT_ec_master_read_state(master, adp, uint8_t* state, uint16_t* status_code);
int ChronosRT_ec_master_request_state(master, adp, uint8_t state, uint32_t timeout_us);
int ChronosRT_ec_master_statecheck(master, adp, uint8_t req_state, uint32_t timeout_us);
int ChronosRT_ec_master_write_state(master, adp, uint8_t state);
int ChronosRT_ec_master_request_all_states(master, uint8_t state, uint32_t timeout_us);
```

`request_all_states` usa `BWR` al `AL_CONTROL` y degrada elegantemente contando
las esclavas que alcanzan el estado.

### 12.8 Distributed Clocks y PDO

```c
ChronosRT_status_t ChronosRT_ec_master_config_dc(ChronosRT_ec_master_t* master);
int      ChronosRT_ec_master_sync0(master, uint32_t index, bool act,
                                   uint32_t cycle_ns, int32_t shift_ns);
int      ChronosRT_ec_master_sync01(master, uint32_t index, bool act,
                                    uint32_t cycle0_ns, int32_t shift0_ns,
                                    uint32_t cycle1_ns, int32_t shift1_ns);
int64_t  ChronosRT_ec_master_dc_time(const ChronosRT_ec_master_t* master);

int ChronosRT_ec_master_send_processdata(ChronosRT_ec_master_t* master);
int ChronosRT_ec_master_receive_processdata(ChronosRT_ec_master_t* master,
                                            uint32_t timeout_us);
```

### 12.9 Buzón / CoE (SDO)

```c
ChronosRT_status_t ChronosRT_ec_master_sdo_read(master, uint32_t slave_index,
                                                uint16_t index, uint8_t subindex,
                                                void* out, uint32_t* size,
                                                uint32_t timeout_us);
ChronosRT_status_t ChronosRT_ec_master_sdo_write(master, uint32_t slave_index,
                                                 uint16_t index, uint8_t subindex,
                                                 const void* in, uint32_t size,
                                                 uint32_t timeout_us);
```

Requiere la esclava en `PREOP` o superior (buzón activo).

### 12.10 Diagnóstico y acceso crudo

```c
uint16_t ChronosRT_ec_master_working_counter(const ChronosRT_ec_master_t* master);
uint16_t ChronosRT_ec_master_expected_wkc(const ChronosRT_ec_master_t* master);
uint64_t ChronosRT_ec_master_frames_sent(const ChronosRT_ec_master_t* master);
uint64_t ChronosRT_ec_master_frames_lost(const ChronosRT_ec_master_t* master);

int ChronosRT_ec_master_send(ChronosRT_ec_master_t* master,
                             const uint8_t* datagrams, uint32_t len);
int ChronosRT_ec_master_recv(ChronosRT_ec_master_t* master,
                             uint8_t* buffer, uint32_t bufsize,
                             uint32_t timeout_us);
```

### 12.11 Ejemplo

```c
ChronosRT_ec_master_t* m = ChronosRT_ec_master_create();
ChronosRT_ec_master_open(m, "enp2s0");
int n = ChronosRT_ec_master_scan(m);
ChronosRT_ec_master_config_init(m);
ChronosRT_ec_master_config_map(m);
ChronosRT_ec_master_request_all_states(m, ChronosRT_EC_STATE_OP, 2000000);

while (running) {
    ChronosRT_ec_master_send_processdata(m);
    if (ChronosRT_ec_master_receive_processdata(m, 1000) < 0) {
        continue;   /* WKC insuficiente o trama perdida */
    }
    /* leer/escribir el IOMap */
}

ChronosRT_ec_master_close(m);
ChronosRT_ec_master_destroy(m);
```

---

## 13. EtherTux — `chronosrt_ethertux.h`

Protocolo determinista sobre Ethernet comercial (`AF_PACKET`, EtherType
**0x88B5**). Cabecera en el cable: **26 B**.

### 13.1 Constantes y tipos de trama

```c
#define ChronosRT_ET_ETHERTYPE     0x88B5
#define ChronosRT_ET_HEADER_SIZE   26u
#define ChronosRT_ET_ETH_HDR_SIZE  14u
#define ChronosRT_ET_MAX_FRAME     1518u
#define ChronosRT_ET_MAX_PAYLOAD   1478u
#define ChronosRT_ET_MAX_NODES     64u
#define ChronosRT_ET_MAX_FLOWS     255u
#define ChronosRT_ET_PRIORITIES    6u
#define ChronosRT_ET_P0_MAX_FRAME  64u

typedef enum {
    ChronosRT_ET_TYPE_DATA            = 0x01,
    ChronosRT_ET_TYPE_FEC             = 0x02,
    ChronosRT_ET_TYPE_CONTROL         = 0x03,
    ChronosRT_ET_TYPE_SYNC            = 0x04,
    ChronosRT_ET_TYPE_LAPM            = 0x05,
    ChronosRT_ET_TYPE_DISCOVERY       = 0x06,
    ChronosRT_ET_TYPE_SYNC_CALIBRATED = 0x07,
    ChronosRT_ET_TYPE_STATE_CHANGE    = 0x08,
    ChronosRT_ET_TYPE_MAILBOX_DATA    = 0x09,
    ChronosRT_ET_TYPE_MAILBOX_ACK     = 0x0A,
    ChronosRT_ET_TYPE_SYNC_REPORT     = 0x0B,
    ChronosRT_ET_TYPE_TOPO_CHECK      = 0x0C
} ChronosRT_et_type_t;

typedef enum {
    ChronosRT_ET_P0 = 0, ... ChronosRT_ET_P5 = 5
} ChronosRT_et_priority_t;

typedef enum {
    ChronosRT_ET_STATE_INIT = 0,
    ChronosRT_ET_STATE_CONFIG,
    ChronosRT_ET_STATE_SAFE,
    ChronosRT_ET_STATE_OP,
    ChronosRT_ET_STATE_FAULT
} ChronosRT_et_state_t;
```

Flags: `FEC 0x01`, `PREEMPTABLE 0x02`, `EXPRESS 0x04`, `DISCOVERY 0x08`,
`FRM 0x10`.

### 13.2 Cabecera

```c
typedef struct {
    uint8_t  type;
    uint8_t  priority;
    uint8_t  flags;
    uint8_t  state;          /* ET-ESM del emisor */
    uint8_t  flow_id;        /* 1..255            */
    uint32_t seq_num;
    uint64_t tx_timestamp;   /* ns monotónicos    */
    uint16_t wkc;            /* ET-WKC            */
    uint16_t block_id;       /* (bloque << 8) | índice */
    uint8_t  flow_count;     /* 1 = sin agregación */
    uint32_t crc32;          /* CRC-32C cabecera + carga */
} ChronosRT_et_header_t;
```

### 13.3 Trama, CRC y física del enlace

```c
uint32_t ChronosRT_et_header_build(uint8_t out[ChronosRT_ET_HEADER_SIZE],
                                   const ChronosRT_et_header_t* h);
uint32_t ChronosRT_et_pdu_build(uint8_t* out, uint32_t out_size,
                                const ChronosRT_et_header_t* h,
                                const void* payload, uint16_t len);
uint32_t ChronosRT_et_pdu_build_header(uint8_t out[ChronosRT_ET_HEADER_SIZE],
                                       const ChronosRT_et_header_t* h,
                                       const void* payload, uint16_t len);
uint32_t ChronosRT_et_pdu_parse(const uint8_t* in, uint32_t in_len,
                                ChronosRT_et_header_t* h,
                                const uint8_t** payload);
uint32_t ChronosRT_et_crc(const ChronosRT_et_header_t* h,
                          const void* payload, uint16_t len);
const char* ChronosRT_et_type_name(ChronosRT_et_type_t type);

uint32_t ChronosRT_et_wire_frame_bytes(uint32_t payload_len);
uint32_t ChronosRT_et_wire_pps(uint64_t link_bps, uint32_t payload_len);
uint32_t ChronosRT_et_payload_for_pps(uint64_t link_bps, uint32_t pps);
```

`pdu_build_header` calcula el CRC sobre cabecera + carga y devuelve **solo** los
26 B de cabecera, sin copiar la carga (envío con `sendmsg`/iovec).

### 13.4 FEC Reed-Solomon

```c
typedef enum {
    ChronosRT_ET_FEC_OFF    = 0,
    ChronosRT_ET_FEC_LIGHT  = 1,   /* RS(10,8): 25 % de overhead  */
    ChronosRT_ET_FEC_ROBUST = 2    /* RS(16,8): 100 % de overhead */
} ChronosRT_et_fec_mode_t;

ChronosRT_et_fec_t* ChronosRT_et_fec_create(uint8_t k, uint8_t n);
ChronosRT_et_fec_t* ChronosRT_et_fec_create_mode(ChronosRT_et_fec_mode_t mode);
void                ChronosRT_et_fec_destroy(ChronosRT_et_fec_t* fec);
uint8_t             ChronosRT_et_fec_k(const ChronosRT_et_fec_t* fec);
uint8_t             ChronosRT_et_fec_n(const ChronosRT_et_fec_t* fec);
uint32_t            ChronosRT_et_fec_overhead_percent(const ChronosRT_et_fec_t* fec);
ChronosRT_status_t  ChronosRT_et_fec_encode(const ChronosRT_et_fec_t* fec,
                                            const uint8_t* const* data,
                                            uint8_t* const* parity, uint32_t len);
ChronosRT_status_t  ChronosRT_et_fec_decode(const ChronosRT_et_fec_t* fec,
                                            uint8_t* const* blocks,
                                            const bool* present, uint32_t len);
```

### 13.5 QoS jerárquico, backpressure y LAPM

```c
typedef struct {
    uint64_t rate_bytes_per_sec;
    uint32_t burst_bytes;
} ChronosRT_et_bucket_cfg_t;

typedef struct {
    ChronosRT_et_bucket_cfg_t bucket[ChronosRT_ET_PRIORITIES];
    uint8_t pause_percent;
    bool    lapm_enabled;
    uint8_t lapm_high_percent;
    uint8_t lapm_low_percent;
} ChronosRT_et_qos_config_t;

typedef struct {
    uint64_t admitted, backpressure, dropped, degraded, bytes_admitted;
} ChronosRT_et_qos_stats_t;

ChronosRT_et_qos_t* ChronosRT_et_qos_create(void);
void                ChronosRT_et_qos_destroy(ChronosRT_et_qos_t* qos);
void                ChronosRT_et_qos_default_config(ChronosRT_et_qos_config_t* cfg);
ChronosRT_status_t  ChronosRT_et_qos_configure(ChronosRT_et_qos_t* qos,
                                               const ChronosRT_et_qos_config_t* cfg);
ChronosRT_status_t  ChronosRT_et_qos_admit(ChronosRT_et_qos_t* qos,
                                           uint8_t priority, uint32_t bytes,
                                           uint64_t now_ns);
bool                ChronosRT_et_qos_backpressure(const ChronosRT_et_qos_t* qos,
                                                  uint8_t priority);
uint8_t             ChronosRT_et_qos_effective_priority(const ChronosRT_et_qos_t* qos,
                                                        uint8_t priority);
ChronosRT_status_t  ChronosRT_et_qos_stats(const ChronosRT_et_qos_t* qos,
                                           ChronosRT_et_qos_stats_t* out);
void                ChronosRT_et_qos_reset_stats(ChronosRT_et_qos_t* qos);
```

`admit` devuelve `SUCCESS`, `ERROR_BACKPRESSURE` (P0–P4, no se pierde) o
`ERROR_DROPPED` (P5 best-effort).

### 13.6 Planificador time-triggered

```c
typedef struct {
    uint8_t  priority;
    uint16_t node;
    uint16_t frame_bytes;
    uint32_t offset_ns;
} ChronosRT_et_slot_t;

ChronosRT_et_sched_t* ChronosRT_et_sched_create(uint32_t cycle_ns, uint64_t link_bps);
void                  ChronosRT_et_sched_destroy(ChronosRT_et_sched_t* sched);
ChronosRT_status_t    ChronosRT_et_sched_set_windows(ChronosRT_et_sched_t* sched,
                                                     uint32_t control_ns,
                                                     uint32_t deterministic_ns,
                                                     uint32_t rate_ns,
                                                     uint32_t best_effort_ns);
ChronosRT_status_t    ChronosRT_et_sched_add_slot(ChronosRT_et_sched_t* sched,
                                                  uint8_t priority, uint16_t node,
                                                  uint16_t frame_bytes, uint32_t offset_ns);
int                   ChronosRT_et_sched_remove_node(ChronosRT_et_sched_t* sched, uint16_t node);
uint32_t              ChronosRT_et_sched_slot_count(const ChronosRT_et_sched_t* sched);
bool                  ChronosRT_et_sched_slot(const ChronosRT_et_sched_t* sched,
                                               uint32_t index, ChronosRT_et_slot_t* out);
uint32_t              ChronosRT_et_sched_cycle_ns(const ChronosRT_et_sched_t* sched);
uint8_t               ChronosRT_et_sched_window_at(const ChronosRT_et_sched_t* sched,
                                                    uint64_t now_ns);
bool                  ChronosRT_et_sched_can_transmit(const ChronosRT_et_sched_t* sched,
                                                       uint8_t priority, uint64_t now_ns);
uint64_t              ChronosRT_et_tx_time_ns(uint32_t frame_bytes, uint64_t link_bps);
```

Ventanas: `0` control, `1` determinista, `2` rate-constrained, `3` best-effort.

### 13.7 Flow Aggregator

```c
typedef struct {
    uint8_t        flow_id;
    uint16_t       len;
    const uint8_t* data;
} ChronosRT_et_flow_entry_t;

ChronosRT_status_t ChronosRT_et_flowagg_build(const ChronosRT_et_flow_entry_t* entries,
                                              uint32_t count,
                                              uint8_t* out, uint32_t out_size,
                                              uint32_t* out_len);
ChronosRT_status_t ChronosRT_et_flowagg_parse(const uint8_t* in, uint32_t len,
                                              ChronosRT_et_flow_entry_t* views,
                                              uint32_t max_views, uint32_t* out_count);
uint32_t           ChronosRT_et_flowagg_saved_bytes(uint32_t count, uint32_t data_bytes);
```

### 13.8 ET-ESM (ciclo de vida del nodo)

```c
ChronosRT_et_esm_t*     ChronosRT_et_esm_create(void);
void                    ChronosRT_et_esm_destroy(ChronosRT_et_esm_t* esm);
void                    ChronosRT_et_esm_reset(ChronosRT_et_esm_t* esm);
ChronosRT_et_state_t    ChronosRT_et_esm_state(const ChronosRT_et_esm_t* esm);
bool                    ChronosRT_et_esm_can_transition(ChronosRT_et_state_t from,
                                                        ChronosRT_et_state_t to);
ChronosRT_status_t      ChronosRT_et_esm_request(ChronosRT_et_esm_t* esm,
                                                 ChronosRT_et_state_t next);
bool                    ChronosRT_et_esm_allows_traffic(ChronosRT_et_state_t state,
                                                        uint8_t type, uint8_t priority);
uint32_t                ChronosRT_et_esm_build_state_change(ChronosRT_et_state_t target,
                                                            uint8_t reason, uint8_t out[2]);
ChronosRT_status_t      ChronosRT_et_esm_parse_state_change(const uint8_t* in, uint32_t len,
                                                            ChronosRT_et_state_t* target,
                                                            uint8_t* reason);
bool                    ChronosRT_et_esm_state_from_wire(uint8_t raw,
                                                          ChronosRT_et_state_t* out);
```

Transiciones válidas: `INIT → CONFIG → SAFE → OP`; cualquiera → `FAULT`;
`FAULT → INIT`.

### 13.9 ET-Sync y monitor de ventana

```c
typedef enum {
    ChronosRT_ET_SYNC_OK = 0,
    ChronosRT_ET_SYNC_WARN = 1,
    ChronosRT_ET_SYNC_OUT_OF_SYNC = 2
} ChronosRT_et_sync_status_t;

typedef struct {
    uint32_t warn_threshold_ns;         /* def. 1 µs  */
    uint32_t out_of_sync_threshold_ns;  /* def. 2 µs  */
    uint32_t report_period_frames;      /* def. 10    */
} ChronosRT_et_sync_config_t;

typedef struct {
    uint32_t nodes, ok, warn, out_of_sync;
    uint64_t max_abs_offset_ns;
} ChronosRT_et_sync_stats_t;

ChronosRT_et_sync_t* ChronosRT_et_sync_create(void);
void                 ChronosRT_et_sync_destroy(ChronosRT_et_sync_t* sync);
void                 ChronosRT_et_sync_default_config(ChronosRT_et_sync_config_t* cfg);
ChronosRT_status_t   ChronosRT_et_sync_configure(ChronosRT_et_sync_t* sync,
                                                 const ChronosRT_et_sync_config_t* cfg);
ChronosRT_status_t   ChronosRT_et_sync_set_link_delay(ChronosRT_et_sync_t* sync,
                                                      uint16_t node_id, uint32_t delay_ns);
ChronosRT_status_t   ChronosRT_et_sync_process_report(ChronosRT_et_sync_t* sync,
                                                      uint16_t node_id,
                                                      int64_t measured_offset_ns,
                                                      uint64_t now_ns,
                                                      ChronosRT_et_sync_status_t* out_status);
ChronosRT_status_t   ChronosRT_et_sync_status_of(const ChronosRT_et_sync_t* sync,
                                                 uint16_t node_id, int64_t* offset_ns,
                                                 ChronosRT_et_sync_status_t* out_status);
ChronosRT_status_t   ChronosRT_et_sync_drift_ppb(const ChronosRT_et_sync_t* sync,
                                                 uint16_t node_id, int64_t* out_ppb);
ChronosRT_status_t   ChronosRT_et_sync_stats(const ChronosRT_et_sync_t* sync,
                                             ChronosRT_et_sync_stats_t* out);
uint32_t             ChronosRT_et_sync_build_calibrated(uint64_t master_ns, uint8_t out[8]);
ChronosRT_status_t   ChronosRT_et_sync_parse_calibrated(const uint8_t* in, uint32_t len,
                                                        uint64_t* out_master_ns);
uint32_t             ChronosRT_et_sync_build_report(int64_t offset_ns, uint8_t out[8]);
ChronosRT_status_t   ChronosRT_et_sync_parse_report(const uint8_t* in, uint32_t len,
                                                    int64_t* out_offset_ns);
```

### 13.10 Registro de nodos, ET-WKC y Hot Connect

```c
typedef struct {
    uint16_t id;
    uint8_t  mac[6];
    uint16_t link_mbps;
    uint16_t buffer_bytes;
    bool     fec_supported;
    uint16_t slot;                 /* 0xFFFF = libre */
    ChronosRT_et_state_t state;
    uint16_t last_wkc;
    uint16_t expected_wkc;
    uint8_t  wkc_mismatch_count;
    int64_t  sync_offset_ns;
    ChronosRT_et_sync_status_t sync_status;
    uint64_t last_seen_ns;
} ChronosRT_et_node_info_t;

typedef struct {
    uint8_t  wkc_mismatch_threshold;      /* def. 3   */
    uint32_t hot_connect_period_frames;   /* def. 100 */
    uint32_t heartbeat_timeout_frames;    /* def. 3   */
} ChronosRT_et_topology_config_t;

#define ChronosRT_ET_SLOT_FREE 0xFFFFu
#define ChronosRT_ET_DISCOVERY_REPLY_SIZE 8u
```

```c
ChronosRT_et_topology_t* ChronosRT_et_topology_create(void);
void                     ChronosRT_et_topology_destroy(ChronosRT_et_topology_t* topo);
void                     ChronosRT_et_topology_default_config(ChronosRT_et_topology_config_t* cfg);
ChronosRT_status_t       ChronosRT_et_topology_configure(ChronosRT_et_topology_t* topo,
                                                         const ChronosRT_et_topology_config_t* cfg);
ChronosRT_status_t       ChronosRT_et_topology_upsert(ChronosRT_et_topology_t* topo,
                                                      const ChronosRT_et_node_info_t* info);
ChronosRT_status_t       ChronosRT_et_topology_remove(ChronosRT_et_topology_t* topo,
                                                      uint16_t node_id);
const ChronosRT_et_node_info_t* ChronosRT_et_topology_find(const ChronosRT_et_topology_t* topo,
                                                           uint16_t node_id);
uint32_t                 ChronosRT_et_topology_count(const ChronosRT_et_topology_t* topo);
const ChronosRT_et_node_info_t* ChronosRT_et_topology_at(const ChronosRT_et_topology_t* topo,
                                                         uint32_t index);
uint16_t                 ChronosRT_et_topology_next_free_slot(const ChronosRT_et_topology_t* topo);
ChronosRT_status_t       ChronosRT_et_topology_assign_slot(ChronosRT_et_topology_t* topo,
                                                           uint16_t node_id, uint16_t slot);
ChronosRT_status_t       ChronosRT_et_topology_on_frame(ChronosRT_et_topology_t* topo,
                                                        const uint8_t mac[6],
                                                        const ChronosRT_et_header_t* h,
                                                        uint64_t now_ns);
bool                     ChronosRT_et_topology_needs_topo_check(const ChronosRT_et_topology_t* topo,
                                                                uint16_t* out_node_id);
uint32_t                 ChronosRT_et_topology_tick(ChronosRT_et_topology_t* topo,
                                                    uint64_t now_ns, uint32_t cycle_ns);
bool                     ChronosRT_et_topology_hot_connect_due(const ChronosRT_et_topology_t* topo,
                                                               uint64_t frame_index,
                                                               uint16_t* out_node_addr);
uint32_t                 ChronosRT_et_topology_build_reply(const ChronosRT_et_node_info_t* info,
                                                           uint8_t out[ChronosRT_ET_DISCOVERY_REPLY_SIZE]);
ChronosRT_status_t       ChronosRT_et_topology_parse_reply(const uint8_t* in, uint32_t len,
                                                           ChronosRT_et_node_info_t* out);
```

### 13.11 ET-MBX (mailbox de gestión)

```c
#define ChronosRT_ET_MBX_CHANNELS      8u
#define ChronosRT_ET_MBX_SEGMENT_BYTES 512u
#define ChronosRT_ET_MBX_MAX_RETRIES   3u
#define ChronosRT_ET_MBX_ACK_SIZE      4u

typedef enum {
    ChronosRT_ET_MBX_IDLE = 0,
    ChronosRT_ET_MBX_SENDING,
    ChronosRT_ET_MBX_WAIT_ACK,
    ChronosRT_ET_MBX_DONE,
    ChronosRT_ET_MBX_FAILED
} ChronosRT_et_mbx_state_t;

typedef struct {
    uint8_t  max_retries;
    uint16_t segment_bytes;
    uint8_t  p4_degrade_percent;   /* def. 70 */
} ChronosRT_et_mbx_config_t;

typedef struct {
    ChronosRT_et_mbx_state_t state;
    uint16_t seq, segment, total_segments;
    uint8_t  retries;
    bool     firmware;
} ChronosRT_et_mbx_channel_status_t;

ChronosRT_et_mbx_t* ChronosRT_et_mbx_create(void);
void                ChronosRT_et_mbx_destroy(ChronosRT_et_mbx_t* mbx);
void                ChronosRT_et_mbx_default_config(ChronosRT_et_mbx_config_t* cfg);
ChronosRT_status_t  ChronosRT_et_mbx_configure(ChronosRT_et_mbx_t* mbx,
                                               const ChronosRT_et_mbx_config_t* cfg);
ChronosRT_status_t  ChronosRT_et_mbx_begin(ChronosRT_et_mbx_t* mbx, uint8_t channel,
                                           uint32_t total_bytes, bool firmware);
ChronosRT_status_t  ChronosRT_et_mbx_status(const ChronosRT_et_mbx_t* mbx, uint8_t channel,
                                            ChronosRT_et_mbx_channel_status_t* out);
ChronosRT_status_t  ChronosRT_et_mbx_next_segment(ChronosRT_et_mbx_t* mbx, uint8_t channel,
                                                  const uint8_t* data, uint32_t len,
                                                  uint8_t* out, uint32_t out_size,
                                                  uint32_t* out_len);
ChronosRT_status_t  ChronosRT_et_mbx_on_ack(ChronosRT_et_mbx_t* mbx, uint8_t channel,
                                            uint16_t seq, bool accepted, uint64_t now_ns);
bool                ChronosRT_et_mbx_retry_due(const ChronosRT_et_mbx_t* mbx, uint8_t channel,
                                               uint64_t now_ns);
uint8_t             ChronosRT_et_mbx_effective_priority(const ChronosRT_et_mbx_t* mbx,
                                                        uint8_t p4_occupancy_percent);
uint32_t            ChronosRT_et_mbx_build_ack(uint8_t channel, uint16_t seq, bool accepted,
                                               uint8_t out[ChronosRT_ET_MBX_ACK_SIZE]);
ChronosRT_status_t  ChronosRT_et_mbx_parse_ack(const uint8_t* in, uint32_t len,
                                               uint8_t* channel, uint16_t* seq, bool* accepted);
ChronosRT_status_t  ChronosRT_et_mbx_parse_data(const uint8_t* in, uint32_t len,
                                                uint8_t* channel, uint16_t* seq,
                                                uint16_t* index, uint16_t* total,
                                                const uint8_t** data, uint16_t* data_len);
```

### 13.12 FRM (redundancia lógica de flujo)

```c
typedef struct {
    uint64_t duplicates_tx, duplicates_rx, dropped_dup;
    uint32_t path_switches;
} ChronosRT_et_frm_stats_t;

ChronosRT_et_frm_t* ChronosRT_et_frm_create(void);
void                ChronosRT_et_frm_destroy(ChronosRT_et_frm_t* frm);
void                ChronosRT_et_frm_reset(ChronosRT_et_frm_t* frm);
bool                ChronosRT_et_frm_should_duplicate(const ChronosRT_et_frm_t* frm,
                                                      uint8_t priority);
uint8_t             ChronosRT_et_frm_alternate_flow(uint8_t flow_id);
ChronosRT_status_t  ChronosRT_et_frm_note_rx(ChronosRT_et_frm_t* frm, uint16_t node_id,
                                             uint32_t seq_num, bool duplicate_flag,
                                             bool* out_is_new);
ChronosRT_status_t  ChronosRT_et_frm_fail_path(ChronosRT_et_frm_t* frm, uint16_t node_id);
uint8_t             ChronosRT_et_frm_active_path(const ChronosRT_et_frm_t* frm,
                                                 uint16_t node_id);
ChronosRT_status_t  ChronosRT_et_frm_stats(const ChronosRT_et_frm_t* frm,
                                           ChronosRT_et_frm_stats_t* out);
```

Solo se duplican flujos P0/P1; el receptor deduplica por `(nodo, SeqNum)`.

### 13.13 ID local determinista (MAC LAA)

Esquema de dos niveles: el receptor deriva un ID de 16 bits de la MAC (filtro de
recepción de coste constante) y el maestro es la autoridad que garantiza la
unicidad.

```c
#define ChronosRT_ET_LOCAL_ID_MAX  0xFFFFu
#define ChronosRT_ET_LOCAL_ID_NONE 0xFFFFu

/* MAC local: 02:00:00:00:ID[15:8]:ID[7:0] (U/L=1, I/G=0) */
uint32_t ChronosRT_et_aalid_mac(uint16_t id, uint8_t out[6]);
uint16_t ChronosRT_et_aalid_id_from_mac(const uint8_t mac[6]);

/* Nivel 1: ID candidato (sin ramas ni bucles) */
uint16_t ChronosRT_et_aalid_candidate(const uint8_t mac[6]);      /* CRC-32C plegado */
uint16_t ChronosRT_et_aalid_candidate_mul(const uint8_t mac[6]);  /* fallback portable */
uint32_t ChronosRT_et_aalid_backoff_slots(const uint8_t mac[6], uint32_t slots);

typedef enum {
    ChronosRT_ET_AALID_OK = 0, ChronosRT_ET_AALID_COLLISION = 1,
    ChronosRT_ET_AALID_DUPLICATE = 2, ChronosRT_ET_AALID_CLONE = 3,
    ChronosRT_ET_AALID_FULL = 4, ChronosRT_ET_AALID_INVALID = 5
} ChronosRT_et_aalid_result_t;

typedef struct {
    uint16_t id_max;        /* IDs asignables - 1 (def. 65535)          */
    uint32_t grace_ms;      /* tiempo de gracia sin heartbeat (def. 600000) */
    uint32_t table_slots;   /* potencia de 2 (def. 256; 0 => 256)       */
} ChronosRT_et_aalid_config_t;

typedef struct {
    uint64_t admissions, collisions, duplicates;
    uint64_t released, expired, clone_mac;
    uint32_t occupied;
} ChronosRT_et_aalid_stats_t;

ChronosRT_et_aalid_t* ChronosRT_et_aalid_create(const ChronosRT_et_aalid_config_t* cfg);
void                  ChronosRT_et_aalid_destroy(ChronosRT_et_aalid_t* aalid);
void                  ChronosRT_et_aalid_default_config(ChronosRT_et_aalid_config_t* cfg);

ChronosRT_status_t ChronosRT_et_aalid_admit(ChronosRT_et_aalid_t* aalid, const uint8_t mac[6],
                                            uint32_t nonce, uint64_t now_ms, uint16_t* out_id,
                                            ChronosRT_et_aalid_result_t* out_result);
ChronosRT_status_t ChronosRT_et_aalid_release(ChronosRT_et_aalid_t* aalid, uint16_t id);
ChronosRT_status_t ChronosRT_et_aalid_lookup(const ChronosRT_et_aalid_t* aalid,
                                             const uint8_t mac[6], uint16_t* out_id);
ChronosRT_status_t ChronosRT_et_aalid_heartbeat(ChronosRT_et_aalid_t* aalid, uint16_t id,
                                                uint64_t now_ms);
uint32_t           ChronosRT_et_aalid_expire(ChronosRT_et_aalid_t* aalid, uint64_t now_ms);
ChronosRT_status_t ChronosRT_et_aalid_stats(const ChronosRT_et_aalid_t* aalid,
                                            ChronosRT_et_aalid_stats_t* out);
bool               ChronosRT_et_aalid_is_occupied(const ChronosRT_et_aalid_t* aalid, uint16_t id);
ChronosRT_status_t ChronosRT_et_aalid_save(const ChronosRT_et_aalid_t* aalid, const char* path);
ChronosRT_status_t ChronosRT_et_aalid_load(ChronosRT_et_aalid_t* aalid, const char* path);
```

`admit` devuelve `ChronosRT_SUCCESS` con el ID final (también en colisión y en
duplicado), `ChronosRT_ERROR_STATE` si la MAC está clonada (misma MAC con
distinto `nonce`) y `ChronosRT_ERROR_FULL` si no quedan IDs. La asignación es
determinista: la misma secuencia de llegadas produce siempre el mismo resultado
y el *backoff* se deriva de la MAC, nunca de `rand()`.

`save`/`load` persisten `{MAC → ID}` con reemplazo atómico (`tmp` + `rename`)
para conservar la identidad tras un reinicio del maestro.

**Configuración y memoria.** `id_max` fija el rango de IDs (65 536 por
defecto) y `table_slots` el número de nodos que caben en la tabla hash
(256 por defecto; `create` lo redondea a potencia de 2 entre 16 y 65 536).
Con los valores por defecto el módulo ocupa ≈ **14,4 KB** (tabla 256 × 24 B +
bitmap de 8 KB). Para más de 256 nodos, aumente `table_slots`.

**Estados internos.** Cada ranura está *libre*, *activa* o es un *tombstone*
de una entrada liberada. El sondeo lineal continúa sobre los *tombstones*
para no romper las cadenas y los reutiliza al insertar.

**Persistencia.** Fichero con cabecera de 16 B (`ETID`, versión 1 y número de
entradas) seguida de 24 B por entrada (MAC, ID, `nonce` y `last_hb_ms`).

**Complejidad.** `admit` y `lookup` son O(1) amortizado/medio; `release` y
`expire` recorren la tabla (O(slots)) y son operaciones administrativas fuera
del superframe.

### 13.14 Maestro EtherTux

```c
typedef struct {
    uint16_t id;
    uint8_t  mac[6];
    int64_t  last_seen_ns;
    int64_t  clock_offset_ns;
    uint32_t rtt_ns;
    uint32_t missed_cycles;
    bool     lost;
} ChronosRT_et_node_t;

typedef struct {
    uint64_t frames_tx, frames_rx, bytes_tx, bytes_rx;
    uint64_t dropped, backpressure, fec_blocks, frame_errors;
    uint64_t duplicates_tx, wkc_mismatches, sync_out_of_sync, mbx_retries;
    uint64_t local_id_dropped;   /* descartadas por destino ajeno (ID local) */
} ChronosRT_et_master_stats_t;
```

```c
ChronosRT_et_master_t* ChronosRT_et_master_create(void);
void                   ChronosRT_et_master_destroy(ChronosRT_et_master_t* master);

ChronosRT_status_t ChronosRT_et_master_open(ChronosRT_et_master_t* master, const char* ifname);
ChronosRT_status_t ChronosRT_et_master_open_ex(ChronosRT_et_master_t* master, const char* ifname,
                                               uint64_t link_bps, uint32_t cycle_ns);
ChronosRT_status_t ChronosRT_et_master_open_auto(ChronosRT_et_master_t* master,
                                                 const char* ifname);
void               ChronosRT_et_master_close(ChronosRT_et_master_t* master);
bool               ChronosRT_et_master_is_open(const ChronosRT_et_master_t* master);
const char*        ChronosRT_et_master_interface(const ChronosRT_et_master_t* master);
uint64_t           ChronosRT_et_link_speed_bps(const char* ifname);

ChronosRT_status_t ChronosRT_et_master_set_qos(ChronosRT_et_master_t* master,
                                               const ChronosRT_et_qos_config_t* cfg);
ChronosRT_status_t ChronosRT_et_master_set_fec(ChronosRT_et_master_t* master,
                                               ChronosRT_et_fec_mode_t mode);
ChronosRT_status_t ChronosRT_et_master_attach_scheduler(ChronosRT_et_master_t* master,
                                                        ChronosRT_et_sched_t* sched);
ChronosRT_status_t ChronosRT_et_master_attach_topology(ChronosRT_et_master_t* master,
                                                       ChronosRT_et_topology_t* topo);
ChronosRT_status_t ChronosRT_et_master_attach_sync(ChronosRT_et_master_t* master,
                                                   ChronosRT_et_sync_t* sync);
ChronosRT_status_t ChronosRT_et_master_attach_esm(ChronosRT_et_master_t* master,
                                                  ChronosRT_et_esm_t* esm);
ChronosRT_status_t ChronosRT_et_master_attach_mbx(ChronosRT_et_master_t* master,
                                                  ChronosRT_et_mbx_t* mbx);
ChronosRT_status_t ChronosRT_et_master_attach_frm(ChronosRT_et_master_t* master,
                                                  ChronosRT_et_frm_t* frm);
```

Envío y recepción:

```c
ChronosRT_status_t ChronosRT_et_master_send(master, uint8_t priority,
                                            ChronosRT_et_type_t type, uint8_t flow_id,
                                            const void* payload, uint16_t len);
ChronosRT_status_t ChronosRT_et_master_send_at(master, priority, type, flow_id,
                                               payload, len, uint64_t tx_deadline_ns);
ChronosRT_status_t ChronosRT_et_master_send_zerocopy(master, priority, type, flow_id,
                                                     const void* payload, uint16_t len);
ChronosRT_status_t ChronosRT_et_master_send_batch(master,
                                                  const ChronosRT_et_batch_item_t* items,
                                                  uint32_t count);
ChronosRT_status_t ChronosRT_et_master_send_aggregated(master, priority, flow_id,
                                                       const ChronosRT_et_flow_entry_t* entries,
                                                       uint32_t count);
ChronosRT_status_t ChronosRT_et_master_send_mailbox(master, uint8_t channel,
                                                    const void* data, uint16_t len,
                                                    uint8_t p4_occupancy_percent);
ChronosRT_status_t ChronosRT_et_master_send_fec_block(master, priority, flow_id,
                                                      const uint8_t* const* data,
                                                      uint32_t count, uint32_t len);
ChronosRT_status_t ChronosRT_et_master_send_to(master, uint16_t dest_id, uint8_t priority,
                                               ChronosRT_et_type_t type, uint8_t flow_id,
                                               const void* payload, uint16_t len);
int                ChronosRT_et_master_recv(master, ChronosRT_et_header_t* hdr,
                                            void* payload, uint32_t bufsize,
                                            uint32_t timeout_us);
```

`send_batch` admite hasta 16 tramas en una sola llamada (`sendmmsg` o anillo TX).
`send_to` usa la MAC LAA del nodo destino (unicast) en lugar de difusión.

ID local en el maestro:

```c
ChronosRT_status_t ChronosRT_et_master_enable_local_ids(master,
                                                        const ChronosRT_et_aalid_config_t* cfg);
ChronosRT_status_t ChronosRT_et_master_set_local_id(master, uint16_t id);
uint16_t           ChronosRT_et_master_local_id(const ChronosRT_et_master_t* master);
ChronosRT_et_aalid_t* ChronosRT_et_master_aalid(master);
ChronosRT_status_t ChronosRT_et_master_admit_node(master, const uint8_t mac[6],
                                                  uint32_t nonce, uint64_t now_ms,
                                                  uint16_t* out_id,
                                                  ChronosRT_et_aalid_result_t* out_result);
uint32_t           ChronosRT_et_master_expire_local_ids(master, uint64_t now_ms);
bool               ChronosRT_et_master_accepts_destination(const master, const uint8_t mac[6]);
```

Con el ID local activo, la recepción descarta antes de parsear toda trama
unicast dirigida a otro nodo (`local_id_dropped`) y los nodos se indexan por
hash de MAC (1–2 comparaciones) en lugar de un barrido lineal.

### 13.15 Modo de transmisión

```c
typedef enum {
    ChronosRT_ET_TX_AUTO    = 0,   /* anillo TX si el kernel lo permite */
    ChronosRT_ET_TX_SYSCALL = 1,   /* sendmsg / sendmmsg                 */
    ChronosRT_ET_TX_RING    = 2    /* PACKET_TX_RING obligatorio         */
} ChronosRT_et_tx_mode_t;

ChronosRT_status_t ChronosRT_et_master_set_tx_mode(ChronosRT_et_master_t* master,
                                                   ChronosRT_et_tx_mode_t mode);
bool               ChronosRT_et_master_tx_ring_active(const ChronosRT_et_master_t* master);
```

Con `PACKET_TX_RING` la trama se escribe en una ranura mapeada en espacio de
usuario y un único `send()` vacía el lote completo.

### 13.16 Nodos, heartbeat y estadísticas

```c
uint16_t ChronosRT_et_master_last_wkc(const ChronosRT_et_master_t* master);
uint32_t ChronosRT_et_master_heartbeat(ChronosRT_et_master_t* master, uint64_t now_ns);
uint32_t ChronosRT_et_master_node_count(const ChronosRT_et_master_t* master);
bool     ChronosRT_et_master_node(const ChronosRT_et_master_t* master, uint32_t index,
                                  ChronosRT_et_node_t* out);
ChronosRT_status_t ChronosRT_et_master_rtt_sync(ChronosRT_et_master_t* master,
                                                const uint8_t mac[6], uint32_t rtt_ns);
ChronosRT_status_t ChronosRT_et_master_stats(const ChronosRT_et_master_t* master,
                                             ChronosRT_et_master_stats_t* out);
void               ChronosRT_et_master_reset_stats(ChronosRT_et_master_t* master);
```

---

## 14. Integración con `libtuxrt_core` — `chronosrt_tuxrt.h`

ChronosRT se publica como **proveedor** (`tuxrt_provider_descriptor`) y **no**
enlaza contra `libtuxrt_core`; es el consumidor quien lo descubre con
`dlopen`/`dlsym`.

```c
typedef int (*ChronosRT_event_sink_fn)(const ChronosRT_event_t* event, void* user);

typedef struct {
    ChronosRT_handle_t      handle;
    ChronosRT_event_sink_fn sink;
    void*                   user;
    ChronosRT_counters_t    counters;
    ChronosRT_trace_t       trace;
    _Atomic bool            attached;
} ChronosRT_bridge_t;

ChronosRT_status_t ChronosRT_bridge_attach(ChronosRT_bridge_t* bridge,
                                           ChronosRT_handle_t handle,
                                           ChronosRT_event_sink_fn sink, void* user);
ChronosRT_status_t ChronosRT_bridge_detach(ChronosRT_bridge_t* bridge);
uint64_t           ChronosRT_bridge_pump(ChronosRT_bridge_t* bridge,
                                         uint32_t max_events, uint32_t timeout_us);
const ChronosRT_counters_t* ChronosRT_bridge_counters(const ChronosRT_bridge_t* bridge);
const char*        ChronosRT_tuxrt_namespace_info(void);
```

---

## 15. Ejemplo mínimo completo

```c
#include <chronosrt/chronosrt.h>
#include <chronosrt/chronosrt_kernel.h>
#include <chronosrt/chronosrt_protocol.h>
#include <chronosrt/chronosrt_ethertux.h>

#include <stdio.h>

int main(void) {
    ChronosRT_kernel_info_t kinfo;
    if (ChronosRT_kernel_probe(&kinfo) == ChronosRT_SUCCESS) {
        ChronosRT_kernel_print(&kinfo);
    }

    ChronosRT_config_t cfg = {0};
    cfg.num_cpus        = 2;
    cfg.event_pool_size = 65536;
    cfg.lock_memory     = true;
    cfg.use_deadline    = true;
    cfg.use_timerfd     = true;

    ChronosRT_handle_t rt;
    if (ChronosRT_init(&rt, &cfg) != ChronosRT_SUCCESS) {
        return 1;
    }

    if (ChronosRT_protocol_from_env() == ChronosRT_PROTOCOL_ETHERTUX) {
        ChronosRT_et_master_t* et = ChronosRT_et_master_create();
        if (et && ChronosRT_et_master_open(et, "enp2s0") == ChronosRT_SUCCESS) {
            (void)ChronosRT_et_master_send(et, ChronosRT_ET_P1,
                                           ChronosRT_ET_TYPE_DATA, 1u, "hola", 4u);
        }
        ChronosRT_et_master_destroy(et);
    }

    ChronosRT_metrics_t metrics;
    (void)ChronosRT_benchmark_run(rt, ChronosRT_MODE_LATENCY, 10u, &metrics);
    (void)ChronosRT_metrics_report(&metrics);

    ChronosRT_shutdown(rt);
    return 0;
}
```

Compilación: `cc app.c -lchronosrt -lpthread -lm -ldl`.

---

## 16. Verificación de la API

Estado verificado en el host de referencia (Linux 7.2.11, GCC 16.2.1, CMake 4.4.4):

- Build `Release`: **0 errores, 0 warnings**.
- `ctest`: **10/10 suites**, **547/547 aserciones**.
- `libchronosrt.so` exporta **320 símbolos `ChronosRT_*`** con visibilidad
  controlada.
- HAL detectada: `crc32c_backend = "sse4.2-x4"`, `fec_backend = "avx2"`.

La referencia ampliada del maestro EtherCAT está en el
[Manual Técnico §11](MANUAL_TECNICO.md) y la de EtherTux en
[Manual Técnico §13](MANUAL_TECNICO.md).
