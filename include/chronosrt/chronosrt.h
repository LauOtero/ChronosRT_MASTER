#ifndef ChronosRT_H
#define ChronosRT_H

#include "chronosrt_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>

#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #define ChronosRT_ARCH_X86_64 1
    #define ChronosRT_CACHE_LINE 64
#elif defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define ChronosRT_ARCH_ARM64 1
    #define ChronosRT_CACHE_LINE 64
#else
    #define ChronosRT_ARCH_GENERIC 1
    #define ChronosRT_CACHE_LINE 64
#endif

#define ChronosRT_VERSION CHRONOSRT_VERSION_STRING
#define ChronosRT_MAX_CPUS 16
#define ChronosRT_MAX_EVENTS 1048576
#define ChronosRT_CACHE_ALIGNED __attribute__((aligned(ChronosRT_CACHE_LINE)))

/* Objetivo de latencia del proyecto: por encima de este umbral la muestra se
 * contabiliza como pico (`latency_spikes`). */
#define CHRONOSRT_LATENCY_TARGET_NS 10000ULL

typedef enum {
    ChronosRT_SUCCESS = 0,
    ChronosRT_ERROR_INIT = -1,
    ChronosRT_ERROR_MEMORY = -2,
    ChronosRT_ERROR_CPU = -3,
    ChronosRT_ERROR_TIMEOUT = -4,
    ChronosRT_ERROR_INVALID = -5,
    ChronosRT_ERROR_NO_HARDWARE = -6,
    ChronosRT_ERROR_NOTSUP = -7,     /* operación no soportada en este kernel/HW */
    ChronosRT_ERROR_STATE = -8,      /* estado inválido para la operación */
    ChronosRT_ERROR_IO = -9,         /* error de E/S */
    ChronosRT_ERROR_CORRUPT = -10,   /* corrupción de integridad (CRC/ECC) */
    ChronosRT_ERROR_FULL = -11,      /* recurso lleno (cola/ring) */
    ChronosRT_ERROR_BACKPRESSURE = -12, /* el productor debe reducir la tasa   */
    ChronosRT_ERROR_DROPPED = -13,      /* descartada por política best-effort */
    ChronosRT_ERROR_NOTFOUND = -14      /* nodo/flujo/canal no registrado      */
} ChronosRT_status_t;

typedef enum {
    ChronosRT_MODE_LATENCY,
    ChronosRT_MODE_THROUGHPUT,
    ChronosRT_MODE_JITTER,
    ChronosRT_MODE_STRESS,
    ChronosRT_MODE_WCET
} ChronosRT_benchmark_mode_t;

typedef enum {
    ChronosRT_PLATFORM_RPI4,
    ChronosRT_PLATFORM_RPI5,
    ChronosRT_PLATFORM_JETSON_NANO,
    ChronosRT_PLATFORM_JETSON_ORIN,
    ChronosRT_PLATFORM_INTEL_NUC,
    ChronosRT_PLATFORM_ORANGE_PI5,
    ChronosRT_PLATFORM_ROCK5B,
    ChronosRT_PLATFORM_X86_64_RT,
    ChronosRT_PLATFORM_GENERIC
} ChronosRT_platform_t;

typedef struct ChronosRT_CACHE_ALIGNED {
    volatile uint64_t sequence;
    volatile uint64_t timestamp_ns;
    volatile uint32_t cpu_id;
    volatile uint8_t  data[56];
} ChronosRT_event_t;

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
    /* Percentiles de la ventana medida (0 si el modo no los calcula). */
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t p999_ns;
    /* Muestras que superan el objetivo de 10 µs (cola del histograma). */
    uint64_t latency_spikes;
} ChronosRT_metrics_t;

typedef struct {
    uint32_t enabled_cpus[ChronosRT_MAX_CPUS];
    uint32_t num_cpus;
    uint32_t event_pool_size;
    uint32_t batch_size;
    uint32_t stack_size_kb;
    bool     use_hugepages;
    bool     use_numa_aware;
    bool     use_simd;
    bool     use_cpu_affinity;
    ChronosRT_platform_t platform;

    /* --- Extensiones ABI v1 (fases F2–F7 del INFORME_IDi) --- */
    bool     lock_memory;          /* RT-3: mlockall + prefault */
    bool     use_hugepages_2mb;    /* RT-3: MADV_HUGEPAGE en el pool */
    bool     strict_isolation;     /* RT-4: exigir isolcpus/nohz_full/rcu_nocbs */
    bool     use_deadline;         /* RT-1: SCHED_DEADLINE para hilos RT */
    bool     use_bounded_mpmc;     /* PF-3: cola bounded Vyukov (defecto: MPMC M-S) */
    bool     use_timerfd;          /* RT-8: worker con timerfd en vez de usleep */
    uint32_t rt_priority;          /* RT-1/RT-8: prioridad SCHED_FIFO (0 = auto) */
    uint64_t deadline_runtime_ns;  /* RT-1: presupuesto de ejecución */
    uint64_t deadline_period_ns;   /* RT-1: periodo */
} ChronosRT_config_t;

typedef struct ChronosRT_ring ChronosRT_ring_t;
typedef struct ChronosRT_mpmc_queue ChronosRT_mpmc_queue_t;
typedef struct ChronosRT_mem_pool ChronosRT_memory_pool_t;

typedef void* ChronosRT_handle_t;

CHRONOSRT_API ChronosRT_status_t ChronosRT_init(ChronosRT_handle_t* handle, const ChronosRT_config_t* config);
CHRONOSRT_API ChronosRT_status_t ChronosRT_shutdown(ChronosRT_handle_t handle);

CHRONOSRT_API ChronosRT_status_t ChronosRT_event_publish(ChronosRT_handle_t handle, const void* data, uint32_t size);
CHRONOSRT_API ChronosRT_status_t ChronosRT_event_subscribe(ChronosRT_handle_t handle, ChronosRT_event_t* event, uint32_t timeout_us);

CHRONOSRT_API ChronosRT_status_t ChronosRT_benchmark_run(ChronosRT_handle_t handle, ChronosRT_benchmark_mode_t mode, uint32_t duration_sec, ChronosRT_metrics_t* metrics);
CHRONOSRT_API ChronosRT_status_t ChronosRT_benchmark_run_ex(ChronosRT_handle_t handle, ChronosRT_benchmark_mode_t mode, uint32_t duration_sec, uint32_t threads, ChronosRT_metrics_t* metrics);
CHRONOSRT_API ChronosRT_status_t ChronosRT_metrics_report(const ChronosRT_metrics_t* metrics);

CHRONOSRT_API uint64_t ChronosRT_get_cycles(void);
CHRONOSRT_API uint64_t ChronosRT_get_timestamp_ns(void);
CHRONOSRT_API ChronosRT_platform_t ChronosRT_detect_platform(void);

/* Metadatos ABI (para verificación en runtime desde el consumidor). */
CHRONOSRT_API const char* ChronosRT_abi_version_string(void);
CHRONOSRT_API uint32_t    ChronosRT_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif
