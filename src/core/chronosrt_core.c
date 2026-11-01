/*
 * chronosrt_core.c — Núcleo del runtime de tiempo real.
 *
 * Implementa las fases F2 (determinismo) y F3 (tiempo real) del INFORME_IDi:
 *   - B-6/RT-2: `event_publish` NO asigna memoria; los eventos salen de un
 *     pool pre-asignado (free-list lock-free, sin ABA).
 *   - RT-3: `mlockall` + prefault + hugepages opcionales (sin page faults).
 *   - RT-4: sondeo de aislamiento de CPU y aviso si es incompleto.
 *   - RT-8: los hilos de trabajo esperan en un `timerfd` (o
 *     `clock_nanosleep`), en lugar de `usleep` en bucle ocupado.
 *   - RT-1: política `SCHED_DEADLINE` con presupuesto, o `SCHED_FIFO`
 *     configurable.
 *   - PF-3: cola MPMC acotada (Vyukov) opcional frente a Michael-Scott.
 *   - ABI: exporta `ChronosRT_abi_version_string/_version`.
 */
#include "chronosrt.h"
#include "chronosrt_ring.h"
#include "chronosrt_mpmc.h"
#include "chronosrt_bounded.h"
#include "chronosrt_mem.h"
#include "chronosrt_simd.h"
#include "chronosrt_kernel.h"
#include "chronosrt_sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <sched.h>

#if defined(__x86_64__)
    #include <immintrin.h>
#endif

#if defined(__aarch64__)
    #include <linux/perf_event.h>
    #include <sys/syscall.h>
#endif

#define DEFAULT_EVENT_POOL_SIZE 65536
#define DEFAULT_STACK_SIZE_KB 256
#define DEFAULT_TICK_NS 1000000ULL      /* 1 ms */
#define DEFAULT_DEADLINE_RUNTIME_NS 200000ULL
#define DEFAULT_RT_PRIORITY 10

typedef struct ChronosRT_worker_arg {
    struct ChronosRT_core* core;
    int cpu;
} ChronosRT_worker_arg_t;

/* Asignación alineada portátil (memalign no está declarada en <stdlib.h>). */
static void* chronosrt_aligned_alloc(size_t align, size_t size) {
    void* p = NULL;
    if (posix_memalign(&p, align, size) != 0) {
        return NULL;
    }
    return p;
}

typedef struct ChronosRT_core {
    ChronosRT_config_t config;

    ChronosRT_ring_t*            event_ring;
    ChronosRT_mpmc_queue_t*      event_queue;     /* MPMC Michael-Scott (opcional) */
    ChronosRT_bounded_queue_t    bounded;        /* MPMC acotada Vyukov (PF-3)    */
    bool                         use_bounded;
    ChronosRT_mem_pool_t*        pool;           /* origen de bloques de evento   */

    pthread_t                    worker_threads[ChronosRT_MAX_CPUS];
    ChronosRT_worker_arg_t       worker_args[ChronosRT_MAX_CPUS];
    uint32_t                     num_workers;
    _Atomic bool                 running;

    uint64_t                     cycles_offset;
    ChronosRT_isolation_t        isolation;
    ChronosRT_kernel_info_t      kernel;
} ChronosRT_core_t;

/* ------------------------------------------------------------------------ */
/* Contadores de alta resolución                                            */
/* ------------------------------------------------------------------------ */

#if defined(__aarch64__)
static int g_cycles_fd = -1;

static void cycles_counter_init(void) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    attr.disabled = 0;
    attr.exclude_kernel = 0;
    attr.exclude_hv = 1;

    int fd = (int)syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    g_cycles_fd = (fd >= 0) ? fd : -1;
}
#endif

static inline uint64_t read_cycle_counter(void) {
#if defined(__x86_64__)
    /* rdtscp serializa la lectura respecto a la ejecución previa (B-7). */
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    (void)aux;
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    if (g_cycles_fd >= 0) {
        uint64_t val = 0;
        if (read(g_cycles_fd, &val, sizeof(val)) == (ssize_t)sizeof(val)) {
            return val;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000000000ULL + ts.tv_nsec);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000000000ULL + ts.tv_nsec);
#endif
}

static inline uint64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000000000ULL + ts.tv_nsec);
}

/* ------------------------------------------------------------------------ */
/* Hilo de trabajo RT                                                       */
/* ------------------------------------------------------------------------ */

static void chronosrt_worker_apply_policy(ChronosRT_core_t* core, int cpu) {
    if (cpu >= 0) {
        (void)ChronosRT_sched_set_affinity_current(cpu);
    }

    if (core->config.use_deadline) {
        ChronosRT_deadline_attr_t d;
        d.runtime_ns  = core->config.deadline_runtime_ns ? core->config.deadline_runtime_ns
                                                         : DEFAULT_DEADLINE_RUNTIME_NS;
        d.period_ns   = core->config.deadline_period_ns ? core->config.deadline_period_ns
                                                        : DEFAULT_TICK_NS;
        d.deadline_ns = d.period_ns;
        d.cpu         = -1; /* la afinidad ya se fijó arriba */
        if (ChronosRT_sched_set_deadline_current(&d) != ChronosRT_SUCCESS) {
            /* Sin admisión DEADLINE: degradar a FIFO manteniendo RT. */
            (void)ChronosRT_sched_set_fifo_current(core->config.rt_priority
                                                       ? (int)core->config.rt_priority
                                                       : DEFAULT_RT_PRIORITY);
        }
    } else {
        (void)ChronosRT_sched_set_fifo_current(core->config.rt_priority
                                                   ? (int)core->config.rt_priority
                                                   : DEFAULT_RT_PRIORITY);
    }
}

static void* ChronosRT_worker(void* arg) {
    ChronosRT_worker_arg_t* wa = (ChronosRT_worker_arg_t*)arg;
    ChronosRT_core_t* core = wa->core;

    chronosrt_worker_apply_policy(core, wa->cpu);

    uint64_t tick_ns = core->config.deadline_period_ns ? core->config.deadline_period_ns
                                                       : DEFAULT_TICK_NS;

    if (core->config.use_timerfd) {
        int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tfd >= 0) {
            struct itimerspec its;
            memset(&its, 0, sizeof(its));
            its.it_interval.tv_sec  = (time_t)(tick_ns / 1000000000ULL);
            its.it_interval.tv_nsec = (long)(tick_ns % 1000000000ULL);
            its.it_value = its.it_interval;
            if (timerfd_settime(tfd, 0, &its, NULL) == 0) {
                while (atomic_load_explicit(&core->running, memory_order_acquire)) {
                    uint64_t expirations = 0;
                    ssize_t n = read(tfd, &expirations, sizeof(expirations));
                    if (n != (ssize_t)sizeof(expirations)) {
                        break; /* cerrado durante shutdown */
                    }
                }
                close(tfd);
                return NULL;
            }
            close(tfd);
        }
        /* Si timerfd falla, se continúa con nanosleep. */
    }

    struct timespec ts;
    ts.tv_sec  = (time_t)(tick_ns / 1000000000ULL);
    ts.tv_nsec = (long)(tick_ns % 1000000000ULL);
    while (atomic_load_explicit(&core->running, memory_order_acquire)) {
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Ciclo de vida                                                            */
/* ------------------------------------------------------------------------ */

static void chronosrt_detect_cpus(ChronosRT_core_t* core) {
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(cpu_set_t), &available) != 0) {
        return;
    }
    uint32_t count = 0;
    for (int i = 0; i < (int)CPU_SETSIZE && count < ChronosRT_MAX_CPUS; i++) {
        if (CPU_ISSET(i, &available)) {
            core->config.enabled_cpus[count++] = (uint32_t)i;
        }
    }
    core->config.num_cpus = count;
}

ChronosRT_status_t ChronosRT_init(ChronosRT_handle_t* handle, const ChronosRT_config_t* config) {
    if (!handle || !config) {
        return ChronosRT_ERROR_INVALID;
    }

#if defined(__aarch64__)
    cycles_counter_init();
#endif

    ChronosRT_core_t* core = (ChronosRT_core_t*)calloc(1, sizeof(ChronosRT_core_t));
    if (!core) {
        return ChronosRT_ERROR_MEMORY;
    }

    memcpy(&core->config, config, sizeof(ChronosRT_config_t));
    if (core->config.event_pool_size == 0) {
        core->config.event_pool_size = DEFAULT_EVENT_POOL_SIZE;
    }
    if (core->config.stack_size_kb == 0) {
        core->config.stack_size_kb = DEFAULT_STACK_SIZE_KB;
    }

    /* RT-3: memoria bloqueada y sin page faults en el camino caliente. */
    if (core->config.lock_memory) {
        (void)ChronosRT_mem_lock_all();
    }

    core->pool = ChronosRT_mem_pool_create_ex(core->config.event_pool_size,
                                             (uint32_t)sizeof(ChronosRT_event_t),
                                             core->config.use_hugepages || core->config.use_hugepages_2mb,
                                             core->config.use_numa_aware);
    if (!core->pool) {
        free(core);
        return ChronosRT_ERROR_MEMORY;
    }
    if (core->config.lock_memory) {
        (void)ChronosRT_mem_pool_prefault(core->pool);
    }

    core->event_ring = (ChronosRT_ring_t*)chronosrt_aligned_alloc(CHRONOSRT_CACHELINE,
                                                                  sizeof(ChronosRT_ring_t));
    if (!core->event_ring) {
        ChronosRT_mem_pool_delete(core->pool);
        free(core);
        return ChronosRT_ERROR_MEMORY;
    }
    ChronosRT_ring_init(core->event_ring);

    core->use_bounded = core->config.use_bounded_mpmc;
    if (core->use_bounded) {
        if (ChronosRT_bounded_init(&core->bounded, core->config.event_pool_size) != ChronosRT_SUCCESS) {
            free(core->event_ring);
            ChronosRT_mem_pool_delete(core->pool);
            free(core);
            return ChronosRT_ERROR_MEMORY;
        }
    } else {
        core->event_queue = (ChronosRT_mpmc_queue_t*)chronosrt_aligned_alloc(CHRONOSRT_CACHELINE,
                                                                            sizeof(ChronosRT_mpmc_queue_t));
        if (!core->event_queue) {
            free(core->event_ring);
            ChronosRT_mem_pool_delete(core->pool);
            free(core);
            return ChronosRT_ERROR_MEMORY;
        }
        ChronosRT_mpmc_init(core->event_queue);
    }

    core->cycles_offset = read_cycle_counter() - get_timestamp_ns();
    atomic_init(&core->running, true);

    if (core->config.use_cpu_affinity) {
        chronosrt_detect_cpus(core);
    }

    /* Sondeo de kernel (F7) + aislamiento de CPU (RT-4). */
    ChronosRT_kernel_probe(&core->kernel);
    ChronosRT_kernel_print(&core->kernel);

    ChronosRT_sched_isolation_probe(&core->isolation);
    if (core->config.strict_isolation) {
        ChronosRT_sched_isolation_print(&core->isolation);
    }

    core->num_workers = 0;
    for (uint32_t i = 0; i < core->config.num_cpus; i++) {
        core->worker_args[i].core = core;
        core->worker_args[i].cpu  = (int)core->config.enabled_cpus[i];
        if (pthread_create(&core->worker_threads[i], NULL, ChronosRT_worker,
                           &core->worker_args[i]) == 0) {
            core->num_workers++;
        }
    }

    *handle = (ChronosRT_handle_t)core;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_shutdown(ChronosRT_handle_t handle) {
    if (!handle) {
        return ChronosRT_ERROR_INVALID;
    }

    ChronosRT_core_t* core = (ChronosRT_core_t*)handle;
    bool lock_memory = core->config.lock_memory;
    atomic_store_explicit(&core->running, false, memory_order_release);

    for (uint32_t i = 0; i < core->num_workers; i++) {
        pthread_join(core->worker_threads[i], NULL);
    }

    if (core->use_bounded) {
        ChronosRT_bounded_destroy(&core->bounded);
    } else if (core->event_queue) {
        ChronosRT_mpmc_destroy(core->event_queue);
        free(core->event_queue);
    }

    ChronosRT_mem_pool_delete(core->pool);

    free(core->event_ring);
    free(core);

    if (lock_memory) {
        ChronosRT_mem_unlock_all();
    }

    return ChronosRT_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Camino caliente: publicar / suscribir (cero asignación dinámica)          */
/* ------------------------------------------------------------------------ */

static inline bool chronosrt_core_enqueue(ChronosRT_core_t* core, void* ptr) {
    /* Con MPMC acotada se evita el ring SPSC, que no es seguro con varios
     * productores. El ring queda como camino rápido del modo SPSC. */
    if (core->use_bounded) {
        return ChronosRT_bounded_push(&core->bounded, ptr);
    }
    if (ChronosRT_ring_push(core->event_ring, ptr)) {
        return true;
    }
    return ChronosRT_mpmc_push(core->event_queue, ptr);
}

static inline void* chronosrt_core_dequeue(ChronosRT_core_t* core) {
    if (core->use_bounded) {
        void* out = NULL;
        return ChronosRT_bounded_pop(&core->bounded, &out) ? out : NULL;
    }
    void* ptr = ChronosRT_ring_pop(core->event_ring);
    if (ptr) {
        return ptr;
    }
    return ChronosRT_mpmc_pop(core->event_queue);
}

ChronosRT_status_t ChronosRT_event_publish(ChronosRT_handle_t handle, const void* data, uint32_t size) {
    if (!handle || (!data && size > 0)) {
        return ChronosRT_ERROR_INVALID;
    }

    ChronosRT_core_t* core = (ChronosRT_core_t*)handle;

    ChronosRT_event_t* event = (ChronosRT_event_t*)ChronosRT_mem_pool_alloc(core->pool);
    if (!event) {
        return ChronosRT_ERROR_FULL;
    }

    event->timestamp_ns = get_timestamp_ns();
    event->cpu_id = (uint32_t)sched_getcpu();
    event->sequence++;

    if (size > sizeof(event->data)) {
        size = (uint32_t)sizeof(event->data);
    }
    if (size > 0) {
        if (core->config.use_simd) {
            ChronosRT_simd_memcpy((void*)event->data, data, size);
        } else {
            memcpy((void*)event->data, data, size);
        }
    }

    if (!chronosrt_core_enqueue(core, event)) {
        ChronosRT_mem_pool_free(core->pool, event);
        return ChronosRT_ERROR_FULL;
    }

    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_event_subscribe(ChronosRT_handle_t handle, ChronosRT_event_t* event, uint32_t timeout_us) {
    if (!handle || !event) {
        return ChronosRT_ERROR_INVALID;
    }

    ChronosRT_core_t* core = (ChronosRT_core_t*)handle;

    ChronosRT_event_t* e = (ChronosRT_event_t*)chronosrt_core_dequeue(core);
    if (!e && timeout_us > 0) {
        struct timespec ts;
        ts.tv_sec  = timeout_us / 1000000u;
        ts.tv_nsec = (long)(timeout_us % 1000000u) * 1000L;
        nanosleep(&ts, NULL);
        e = (ChronosRT_event_t*)chronosrt_core_dequeue(core);
    }
    if (!e) {
        return ChronosRT_ERROR_TIMEOUT;
    }

    memcpy(event, e, sizeof(ChronosRT_event_t));
    ChronosRT_mem_pool_free(core->pool, e);
    return ChronosRT_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Utilidades                                                               */
/* ------------------------------------------------------------------------ */

uint64_t ChronosRT_get_cycles(void) {
    return read_cycle_counter();
}

uint64_t ChronosRT_get_timestamp_ns(void) {
    return get_timestamp_ns();
}

ChronosRT_platform_t ChronosRT_detect_platform(void) {
#if defined(__aarch64__)
    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "Raspberry Pi")) {
                fclose(fp);
                return strstr(line, "5") ? ChronosRT_PLATFORM_RPI5 : ChronosRT_PLATFORM_RPI4;
            }
            if (strstr(line, "Jetson")) {
                fclose(fp);
                return strstr(line, "Orin") ? ChronosRT_PLATFORM_JETSON_ORIN
                                            : ChronosRT_PLATFORM_JETSON_NANO;
            }
            if (strstr(line, "Rockchip")) {
                fclose(fp);
                return strstr(line, "RK3588") ? ChronosRT_PLATFORM_ROCK5B
                                              : ChronosRT_PLATFORM_ORANGE_PI5;
            }
        }
        fclose(fp);
    }
    return ChronosRT_PLATFORM_RPI4;
#elif defined(__x86_64__)
    return ChronosRT_PLATFORM_INTEL_NUC;
#else
    return ChronosRT_PLATFORM_GENERIC;
#endif
}

const char* ChronosRT_abi_version_string(void) {
    return CHRONOSRT_VERSION_STRING;
}

uint32_t ChronosRT_abi_version(void) {
    return (uint32_t)CHRONOSRT_ABI_VERSION;
}
