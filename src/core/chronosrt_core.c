/*
 * chronosrt_core.c — Núcleo del runtime de tiempo real.
 *
 * Implementa las fases F2 (determinismo) y F3 (tiempo real) del INFORME_IDi:
 *   - B-6/RT-2: `event_publish` NO asigna memoria: el camino por defecto escribe
 *     directamente en una ranura del anillo (cero pool, cero punteros).
 *   - RT-3: `mlockall` + prefault + hugepages opcionales (sin page faults).
 *   - RT-4: sondeo de aislamiento de CPU y aviso si es incompleto.
 *   - RT-8: los hilos de tick esperan en un `timerfd` (o `clock_nanosleep`),
 *     con un `eventfd` de parada para que `shutdown` nunca se cuelgue.
 *   - RT-1: política `SCHED_DEADLINE` con presupuesto, o `SCHED_FIFO`.
 *   - PF-3: cola MPMC acotada (Vyukov) opcional frente al anillo SPSC.
 *   - B-9: futex como *eventcount* (secuencia monótona), sin pérdida de despertar.
 *
 * Correcciones de esta revisión (i+d+i):
 *   B-1  el futex ya no pierde despertares: el productor incrementa la secuencia
 *        y solo despierta si hay waiters; el consumidor espera con el valor
 *        observado y un plazo ABSOLUTO.
 *   B-2  no se crean hilos de tick salvo petición explícita (`num_cpus > 0`) y
 *        nunca sobre CPUs aisladas; el tamaño de pila del config se aplica.
 *   B-3  `shutdown` señaliza un `eventfd` de parada: adiós al cuelgue.
 *   B-4  ARM lee `cntvct_el0` (sin syscall) en lugar de `read(perf_fd)`.
 *   B-8  una carga útil mayor que el evento devuelve error, no se trunca.
 *   B-9  `sequence` es un contador global monótono, no basura reciclada.
 *   I-1  anillo de ranuras de evento en línea (estilo DPDK): un solo acceso.
 */
#include "chronosrt.h"
#include "chronosrt_ring.h"
#include "chronosrt_bounded.h"
#include "chronosrt_mem.h"
#include "chronosrt_simd.h"
#include "chronosrt_kernel.h"
#include "chronosrt_sched.h"
#include "chronosrt_hwaccel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#if defined(__x86_64__)
    #include <immintrin.h>
#endif

#define DEFAULT_EVENT_POOL_SIZE 65536
#define DEFAULT_STACK_SIZE_KB 256
#define DEFAULT_TICK_NS 1000000ULL      /* 1 ms */
#define DEFAULT_DEADLINE_RUNTIME_NS 200000ULL
#define DEFAULT_RT_PRIORITY 10

/* Ranuras del anillo de eventos (potencia de dos). 512 × 128 B = 64 KiB. */
#define EVENT_RING_SLOTS 512u
#define EVENT_RING_MASK  (EVENT_RING_SLOTS - 1u)

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

    /* --- Camino rápido por defecto: anillo SPSC de ranuras en línea (I-1) --- */
    ChronosRT_event_t*            slot_ring;
    _Alignas(64) _Atomic uint32_t ring_head;    /* solo productor */
    _Alignas(64) _Atomic uint32_t ring_tail;    /* solo consumidor */

    /* --- Eventcount (B-1): secuencia monótona + waiters --- */
    _Alignas(64) _Atomic uint32_t event_seq;
    _Atomic uint32_t              waiters;

    /* --- Camino MPMC opcional: pool + cola acotada (PF-3) --- */
    ChronosRT_bounded_queue_t     bounded;
    bool                          use_bounded;
    ChronosRT_mem_pool_t*         pool;

    /* --- Hilos de tick (solo si el usuario los pide) --- */
    pthread_t                     worker_threads[ChronosRT_MAX_CPUS];
    ChronosRT_worker_arg_t        worker_args[ChronosRT_MAX_CPUS];
    uint32_t                      num_workers;
    int                           stop_fd;      /* eventfd de parada (B-3) */
    _Atomic bool                  running;

    /* --- Contador monótono de eventos (B-9) --- */
    _Atomic uint64_t              seq_counter;

    ChronosRT_isolation_t         isolation;
    ChronosRT_kernel_info_t       kernel;
} ChronosRT_core_t;

/* ------------------------------------------------------------------------ */
/* Contadores de alta resolución                                            */
/* ------------------------------------------------------------------------ */

static inline uint64_t read_cycle_counter(void) {
#if defined(__x86_64__)
    /* rdtscp serializa la lectura respecto a la ejecución previa (B-7). */
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    (void)aux;
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    /* B-4: contador virtual del sistema, sin syscall. */
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* Reloj monotónico en ns. En x86_64/ARM pasa por el reloj rápido de la HAL
 * (TSC/CNTVCT calibrado) cuando la plataforma lo permite. */
static inline uint64_t get_timestamp_ns(void) {
    return ChronosRT_tx_timestamp_ns();
}

/* CPU actual cacheada por hilo: `sched_getcpu` deja de estar en el camino. */
static _Thread_local int tl_cpu_hint = -2;

static inline uint32_t current_cpu(void) {
    int c = tl_cpu_hint;
    if (__builtin_expect(c < 0, 0)) {
        c = sched_getcpu();
        tl_cpu_hint = c;
    }
    return (uint32_t)c;
}

/* ------------------------------------------------------------------------ */
/* Wait/wake: eventcount sobre futex privado                                */
/* ------------------------------------------------------------------------ */

/* Espera con plazo RELATIVO (ns). Devuelve 0 si despertó, ETIMEDOUT si venció. */
static int chronosrt_futex_wait(_Atomic uint32_t* futex, uint32_t expected, uint64_t timeout_ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(timeout_ns / 1000000000ULL);
    ts.tv_nsec = (long)(timeout_ns % 1000000000ULL);
    return (int)syscall(SYS_futex, futex, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                        expected, &ts, NULL, 0);
}

/* Despierta hasta `n` waiters (normalmente 1: sin efecto rebaño). */
static void chronosrt_futex_wake(_Atomic uint32_t* futex, int n) {
    (void)syscall(SYS_futex, futex, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, NULL, NULL, 0);
}

/* Publica la llegada de un evento: secuencia + despertar solo si hay waiters. */
static inline void chronosrt_event_signal(ChronosRT_core_t* core) {
    atomic_fetch_add_explicit(&core->event_seq, 1u, memory_order_release);
    if (__builtin_expect(atomic_load_explicit(&core->waiters, memory_order_relaxed) != 0u, 1)) {
        chronosrt_futex_wake(&core->event_seq, 1);
    }
}

/* ------------------------------------------------------------------------ */
/* Hilo de tick RT                                                          */
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
                /* B-3: se espera por timerfd Y por el eventfd de parada. */
                struct pollfd pfd[2];
                pfd[0].fd = tfd;          pfd[0].events = POLLIN; pfd[0].revents = 0;
                pfd[1].fd = core->stop_fd; pfd[1].events = POLLIN; pfd[1].revents = 0;
                const nfds_t nfds = (nfds_t)((core->stop_fd >= 0) ? 2 : 1);
                while (atomic_load_explicit(&core->running, memory_order_acquire)) {
                    int pr = poll(pfd, nfds, -1);
                    if (pr < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (nfds == 2 && (pfd[1].revents & POLLIN) != 0) {
                        break;   /* parada solicitada */
                    }
                    if ((pfd[0].revents & POLLIN) != 0) {
                        uint64_t expirations = 0;
                        (void)read(tfd, &expirations, sizeof(expirations));
                    }
                }
                close(tfd);
                return NULL;
            }
            close(tfd);
        }
        /* Si timerfd falla, se continúa con nanosleep. */
    }

    /* B-3: nanosleep con plazo absoluto, para no acumular deriva y salir rápido. */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load_explicit(&core->running, memory_order_acquire)) {
        next.tv_sec  += (time_t)(tick_ns / 1000000000ULL);
        next.tv_nsec += (long)(tick_ns % 1000000000ULL);
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec  += 1;
        }
        (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Ciclo de vida                                                            */
/* ------------------------------------------------------------------------ */

/* Filtra las CPUs aisladas: un hilo de tick ahí destruiría `nohz_full`. */
static void chronosrt_filter_worker_cpus(ChronosRT_core_t* core) {
    uint32_t out = 0;
    for (uint32_t i = 0; i < core->config.num_cpus; i++) {
        int cpu = (int)core->config.enabled_cpus[i];
        if (cpu < 0) {
            continue;
        }
        if (ChronosRT_sched_cpu_is_isolated(&core->isolation, cpu)) {
            continue;
        }
        core->config.enabled_cpus[out++] = (uint32_t)cpu;
    }
    core->config.num_cpus = out;
}

ChronosRT_status_t ChronosRT_init(ChronosRT_handle_t* handle, const ChronosRT_config_t* config) {
    if (!handle || !config) {
        return ChronosRT_ERROR_INVALID;
    }

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
    core->stop_fd = -1;
    core->use_bounded = core->config.use_bounded_mpmc;

    /* RT-3: memoria bloqueada y sin page faults en el camino caliente. */
    if (core->config.lock_memory) {
        (void)ChronosRT_mem_lock_all();
    }

    if (core->use_bounded) {
        /* Camino MPMC: pool de bloques + cola acotada. */
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
        if (ChronosRT_bounded_init(&core->bounded, core->config.event_pool_size) != ChronosRT_SUCCESS) {
            ChronosRT_mem_pool_delete(core->pool);
            free(core);
            return ChronosRT_ERROR_MEMORY;
        }
    } else {
        /* I-1: anillo de ranuras de evento en línea (sin pool ni indirección). */
        core->slot_ring = (ChronosRT_event_t*)chronosrt_aligned_alloc(
            CHRONOSRT_CACHELINE, (size_t)EVENT_RING_SLOTS * sizeof(ChronosRT_event_t));
        if (!core->slot_ring) {
            free(core);
            return ChronosRT_ERROR_MEMORY;
        }
        memset(core->slot_ring, 0, (size_t)EVENT_RING_SLOTS * sizeof(ChronosRT_event_t));
        atomic_init(&core->ring_head, 0u);
        atomic_init(&core->ring_tail, 0u);
    }

    atomic_init(&core->event_seq, 0u);
    atomic_init(&core->waiters, 0u);
    atomic_init(&core->seq_counter, 0u);
    atomic_init(&core->running, true);

    /* Sondeo de kernel (F7) + aislamiento de CPU (RT-4). */
    ChronosRT_kernel_probe(&core->kernel);
    ChronosRT_kernel_print(&core->kernel);

    ChronosRT_sched_isolation_probe(&core->isolation);
    if (core->config.strict_isolation) {
        ChronosRT_sched_isolation_print(&core->isolation);
    }

    /*
     * B-2: los hilos de tick solo se crean si el usuario los pide (num_cpus > 0)
     * y nunca sobre CPUs aisladas. El diseño anterior creaba uno por CPU para
     * dormir 1 ms: puro ruido sobre los núcleos del camino caliente.
     */
    core->num_workers = 0;
    if (core->config.num_cpus > ChronosRT_MAX_CPUS) {
        core->config.num_cpus = ChronosRT_MAX_CPUS;
    }
    if (core->config.num_cpus > 0) {
        chronosrt_filter_worker_cpus(core);
        if (core->config.num_cpus > 0) {
            core->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        }
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        size_t stack = (size_t)core->config.stack_size_kb * 1024u;
        if (stack < (size_t)PTHREAD_STACK_MIN) {
            stack = (size_t)PTHREAD_STACK_MIN;
        }
        (void)pthread_attr_setstacksize(&attr, stack);
        for (uint32_t i = 0; i < core->config.num_cpus; i++) {
            core->worker_args[i].core = core;
            core->worker_args[i].cpu  = (int)core->config.enabled_cpus[i];
            if (pthread_create(&core->worker_threads[i], &attr, ChronosRT_worker,
                               &core->worker_args[i]) == 0) {
                core->num_workers++;
            }
        }
        pthread_attr_destroy(&attr);
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

    /* Despierta a los consumidores y a los hilos de tick. */
    atomic_fetch_add_explicit(&core->event_seq, 1u, memory_order_release);
    chronosrt_futex_wake(&core->event_seq, INT_MAX);
    if (core->stop_fd >= 0) {
        uint64_t one = 1u;
        (void)write(core->stop_fd, &one, sizeof(one));
    }

    for (uint32_t i = 0; i < core->num_workers; i++) {
        pthread_join(core->worker_threads[i], NULL);
    }
    if (core->stop_fd >= 0) {
        close(core->stop_fd);
        core->stop_fd = -1;
    }

    if (core->use_bounded) {
        ChronosRT_bounded_destroy(&core->bounded);
        ChronosRT_mem_pool_delete(core->pool);
    } else {
        free(core->slot_ring);
    }

    free(core);

    if (lock_memory) {
        ChronosRT_mem_unlock_all();
    }

    return ChronosRT_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Camino caliente: publicar / suscribir (cero asignación dinámica)          */
/* ------------------------------------------------------------------------ */

/* Intenta extraer un evento del anillo SPSC (o de la cola MPMC). */
static inline bool chronosrt_core_try_pop(ChronosRT_core_t* core, ChronosRT_event_t* out) {
    if (core->use_bounded) {
        void* p = NULL;
        if (!ChronosRT_bounded_pop(&core->bounded, &p)) {
            return false;
        }
        memcpy(out, p, sizeof(ChronosRT_event_t));
        ChronosRT_mem_pool_free(core->pool, p);
        return true;
    }

    const uint32_t tail = atomic_load_explicit(&core->ring_tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&core->ring_head, memory_order_acquire);
    if (tail == head) {
        return false;
    }
    memcpy(out, &core->slot_ring[tail & EVENT_RING_MASK], sizeof(ChronosRT_event_t));
    atomic_store_explicit(&core->ring_tail, tail + 1u, memory_order_release);
    return true;
}

ChronosRT_status_t ChronosRT_event_publish(ChronosRT_handle_t handle, const void* data, uint32_t size) {
    if (!handle || (!data && size > 0)) {
        return ChronosRT_ERROR_INVALID;
    }

    ChronosRT_core_t* core = (ChronosRT_core_t*)handle;

    /* B-8: un evento no puede llevar más carga que su campo `data`. */
    if (size > (uint32_t)sizeof(((ChronosRT_event_t*)0)->data)) {
        return ChronosRT_ERROR_INVALID;
    }

    const uint64_t now = get_timestamp_ns();
    const uint64_t seq = atomic_fetch_add_explicit(&core->seq_counter, 1u,
                                                   memory_order_relaxed) + 1u;

    if (!core->use_bounded) {
        const uint32_t head = atomic_load_explicit(&core->ring_head, memory_order_relaxed);
        const uint32_t tail = atomic_load_explicit(&core->ring_tail, memory_order_acquire);
        if ((uint32_t)(head - tail) >= EVENT_RING_SLOTS) {
            return ChronosRT_ERROR_FULL;
        }

        ChronosRT_event_t* ev = &core->slot_ring[head & EVENT_RING_MASK];
        ev->timestamp_ns = now;
        ev->cpu_id = current_cpu();
        ev->sequence = seq;
        if (size > 0u) {
            if (core->config.use_simd) {
                ChronosRT_simd_memcpy((void*)ev->data, data, size);
            } else {
                memcpy((void*)ev->data, data, size);
            }
        }
        atomic_store_explicit(&core->ring_head, head + 1u, memory_order_release);
        chronosrt_event_signal(core);
        return ChronosRT_SUCCESS;
    }

    /* Camino MPMC: bloque del pool + cola acotada. */
    ChronosRT_event_t* event = (ChronosRT_event_t*)ChronosRT_mem_pool_alloc(core->pool);
    if (!event) {
        return ChronosRT_ERROR_FULL;
    }
    event->timestamp_ns = now;
    event->cpu_id = current_cpu();
    event->sequence = seq;
    if (size > 0u) {
        if (core->config.use_simd) {
            ChronosRT_simd_memcpy((void*)event->data, data, size);
        } else {
            memcpy((void*)event->data, data, size);
        }
    }
    if (!ChronosRT_bounded_push(&core->bounded, event)) {
        ChronosRT_mem_pool_free(core->pool, event);
        return ChronosRT_ERROR_FULL;
    }
    chronosrt_event_signal(core);
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_event_subscribe(ChronosRT_handle_t handle, ChronosRT_event_t* event,
                                             uint32_t timeout_us) {
    if (!handle || !event) {
        return ChronosRT_ERROR_INVALID;
    }

    ChronosRT_core_t* core = (ChronosRT_core_t*)handle;

    if (chronosrt_core_try_pop(core, event)) {
        return ChronosRT_SUCCESS;
    }
    if (timeout_us == 0u) {
        return ChronosRT_ERROR_TIMEOUT;
    }

    const uint64_t deadline = get_timestamp_ns() + (uint64_t)timeout_us * 1000u;

    for (;;) {
        /*
         * B-1 (eventcount): se lee la secuencia ANTES de comprobar la cola. Si
         * el productor publica después, incrementa la secuencia y el futex
         * devuelve EAGAIN al instante; si publica antes, la cola ya lo ve.
         */
        const uint32_t seq = atomic_load_explicit(&core->event_seq, memory_order_acquire);
        if (chronosrt_core_try_pop(core, event)) {
            return ChronosRT_SUCCESS;
        }
        if (!atomic_load_explicit(&core->running, memory_order_acquire)) {
            return ChronosRT_ERROR_TIMEOUT;
        }

        const uint64_t now = get_timestamp_ns();
        if (now >= deadline) {
            return ChronosRT_ERROR_TIMEOUT;
        }
        const uint64_t remaining = deadline - now;

        atomic_fetch_add_explicit(&core->waiters, 1u, memory_order_seq_cst);
        /* Reintento: evita la ventana entre leer `seq` y dormir. */
        if (chronosrt_core_try_pop(core, event)) {
            atomic_fetch_sub_explicit(&core->waiters, 1u, memory_order_seq_cst);
            return ChronosRT_SUCCESS;
        }
        (void)chronosrt_futex_wait(&core->event_seq, seq, remaining);
        atomic_fetch_sub_explicit(&core->waiters, 1u, memory_order_seq_cst);
    }
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
