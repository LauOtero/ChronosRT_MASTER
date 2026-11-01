#include "chronosrt.h"
#include "chronosrt_simd.h"
#include "chronosrt_sched.h"
#include "chronosrt_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#if defined(ChronosRT_ARCH_X86_64)
    #include <cpuid.h>
#endif

struct ChronosRT_bench;

typedef struct {
    struct ChronosRT_bench* bench;
    uint32_t index;
} ChronosRT_bench_arg_t;

typedef struct ChronosRT_bench {
    ChronosRT_handle_t handle;
    ChronosRT_benchmark_mode_t mode;
    uint32_t duration_sec;
    ChronosRT_metrics_t metrics;
    volatile bool running;
    pthread_t threads[16];
    ChronosRT_bench_arg_t args[16];
    uint64_t thread_events[16];
    uint64_t thread_consumed[16];
    volatile bool rt_priority_ok;
    bool rt_elevate;
    uint32_t num_threads;
} ChronosRT_bench_t;

static inline uint64_t get_cycles_per_us(void) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t c0 = ChronosRT_get_cycles();

    uint64_t elapsed_ns = 0;
    do {
        clock_gettime(CLOCK_MONOTONIC, &end);
        elapsed_ns = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL
                   + (uint64_t)(end.tv_nsec - start.tv_nsec);
    } while (elapsed_ns < 10000000ULL);

    uint64_t c1 = ChronosRT_get_cycles();
    uint64_t elapsed_us = elapsed_ns / 1000ULL;
    if (elapsed_us == 0) {
        return 1;
    }

    uint64_t cycles_per_us = (c1 - c0) / elapsed_us;
    return cycles_per_us > 0 ? cycles_per_us : 1;
}

static inline void update_wcet(ChronosRT_metrics_t* m, uint64_t latency_ns) {
    if (latency_ns > m->wcet_ns) {
        m->wcet_ns = latency_ns;
    }
    if (m->best_case_ns == 0 || latency_ns < m->best_case_ns) {
        m->best_case_ns = latency_ns;
    }
    if (latency_ns > CHRONOSRT_LATENCY_TARGET_NS) {
        m->latency_spikes++;
    }
}

static inline void bench_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    sched_yield();
#endif
}

static void bench_apply_rt(ChronosRT_bench_t* bench) {
    if (!bench->rt_elevate) {
        return;
    }
    if (ChronosRT_sched_set_fifo_current(80) == ChronosRT_SUCCESS) {
        bench->rt_priority_ok = true;
    }
}

static uint32_t bench_allowed_cpus(void) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return 0;
    }
    int n = CPU_COUNT(&allowed);
    return n > 0 ? (uint32_t)n : 0;
}

/* Fija el hilo actual a la CPU `index`-ésima del conjunto permitido. Repartir
 * los hilos evita que dos de ellos (p. ej. productor y consumidor) caigan en el
 * mismo núcleo, donde SCHED_FIFO haría que uno hambriente al otro. */
static int bench_pin_current(uint32_t index) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return -1;
    }
    int ncpu = CPU_COUNT(&allowed);
    if (ncpu <= 0) {
        return -1;
    }

    uint32_t want = index % (uint32_t)ncpu;
    uint32_t seen = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }
        if (seen == want) {
            cpu_set_t one;
            CPU_ZERO(&one);
            CPU_SET(cpu, &one);
            (void)pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
            return cpu;
        }
        seen++;
    }
    return -1;
}

static void bench_warmup(ChronosRT_bench_t* bench, uint32_t iterations) {
    uint64_t data = 0xA5A5A5A5A5A5A5A5ULL;
    ChronosRT_event_t event;

    for (uint32_t i = 0; i < iterations; i++) {
        if (ChronosRT_event_publish(bench->handle, &data, sizeof(data)) ==
            ChronosRT_SUCCESS) {
            (void)ChronosRT_event_subscribe(bench->handle, &event, 1000);
        }
    }
}

static void* bench_latency_thread(void* arg) {
    ChronosRT_bench_t* bench = (ChronosRT_bench_t*)arg;
    uint64_t start_ns, end_ns;
    uint64_t latency_sum = 0;
    uint64_t min_lat = UINT64_MAX;
    uint64_t max_lat = 0;
    uint64_t count = 0;
    uint64_t sum_sq = 0;
    
    uint64_t test_data = 0xDEADBEEF;
    ChronosRT_event_t event;
    ChronosRT_hist_t hist;
    
    (void)bench_pin_current(0);
    bench_apply_rt(bench);
    bench_warmup(bench, 20000);
    ChronosRT_hist_init(&hist, 100);
    
    while (bench->running) {
        start_ns = ChronosRT_get_timestamp_ns();
        
        ChronosRT_status_t ret = ChronosRT_event_publish(bench->handle, &test_data, sizeof(test_data));
        if (ret == ChronosRT_SUCCESS) {
            ret = ChronosRT_event_subscribe(bench->handle, &event, 1000);
            if (ret == ChronosRT_SUCCESS) {
                end_ns = ChronosRT_get_timestamp_ns();
                uint64_t lat = end_ns - start_ns;
                
                if (lat < min_lat) min_lat = lat;
                if (lat > max_lat) max_lat = lat;
                latency_sum += lat;
                sum_sq += lat * lat;
                count++;
                
                ChronosRT_hist_record(&hist, lat);
                update_wcet(&bench->metrics, lat);
            }
        }
    }
    
    if (count > 0) {
        bench->metrics.total_events = count;
        bench->metrics.avg_latency_ns = latency_sum / count;
        bench->metrics.min_latency_ns = min_lat;
        bench->metrics.max_latency_ns = max_lat;
        
        uint64_t variance = (sum_sq / count) - (bench->metrics.avg_latency_ns * bench->metrics.avg_latency_ns);
        bench->metrics.stddev_ns = (uint64_t)(variance > 0 ? sqrt((double)variance) : 0);
        bench->metrics.p50_ns = ChronosRT_hist_percentile_ns(&hist, 50.0);
        bench->metrics.p99_ns = ChronosRT_hist_percentile_ns(&hist, 99.0);
        bench->metrics.p999_ns = ChronosRT_hist_percentile_ns(&hist, 99.9);
    }
    
    return NULL;
}

static void* bench_throughput_producer(void* arg) {
    ChronosRT_bench_arg_t* a = (ChronosRT_bench_arg_t*)arg;
    ChronosRT_bench_t* bench = a->bench;
    uint64_t test_data = 0xCAFEBABE;
    uint64_t count = 0;
    
    (void)bench_pin_current(a->index);
    bench_apply_rt(bench);
    bench_warmup(bench, 2000);

    while (bench->running) {
        for (int i = 0; i < 1000; i++) {
            if (ChronosRT_event_publish(bench->handle, &test_data,
                                        sizeof(test_data)) == ChronosRT_SUCCESS) {
                count++;
            }
        }
    }

    bench->thread_events[a->index] = count;
    return NULL;
}

static void* bench_throughput_consumer(void* arg) {
    ChronosRT_bench_arg_t* a = (ChronosRT_bench_arg_t*)arg;
    ChronosRT_bench_t* bench = a->bench;
    ChronosRT_event_t event;
    uint64_t count = 0;

    (void)bench_pin_current(a->index);
    bench_apply_rt(bench);

    while (bench->running) {
        if (ChronosRT_event_subscribe(bench->handle, &event, 0) == ChronosRT_SUCCESS) {
            count++;
        } else {
            bench_cpu_relax();
        }
    }
    
    bench->thread_consumed[a->index] = count;
    return NULL;
}

static void* bench_stress_thread(void* arg) {
    ChronosRT_bench_arg_t* a = (ChronosRT_bench_arg_t*)arg;
    ChronosRT_bench_t* bench = a->bench;
    volatile uint64_t counter = 0;
    
    (void)bench_pin_current(a->index);
    bench_apply_rt(bench);
    
    while (bench->running) {
        for (int i = 0; i < 10000; i++) {
            counter++;
            counter *= 3;
            counter >>= 1;
        }
    }
    
    return NULL;
}

ChronosRT_status_t ChronosRT_benchmark_run_ex(ChronosRT_handle_t handle, ChronosRT_benchmark_mode_t mode, uint32_t duration_sec, uint32_t threads, ChronosRT_metrics_t* metrics) {
    if (!handle || !metrics) {
        return ChronosRT_ERROR_INVALID;
    }
    
    ChronosRT_bench_t bench;
    memset(&bench, 0, sizeof(bench));
    bench.handle = handle;
    bench.mode = mode;
    bench.duration_sec = duration_sec;
    bench.running = true;
    if (mode == ChronosRT_MODE_LATENCY || mode == ChronosRT_MODE_JITTER ||
        mode == ChronosRT_MODE_WCET) {
        bench.num_threads = 1;
    } else {
        bench.num_threads = threads > 0 ? threads : 4;
        if (bench.num_threads > 16) {
            bench.num_threads = 16;
        }
    }

    memset(metrics, 0, sizeof(ChronosRT_metrics_t));

    /* Con más hilos que CPUs disponibles, elevar a FIFO haría que un hilo que
     * nunca se bloquea hambrientase al resto (productor frente a consumidor). */
    uint32_t allowed_cpus = bench_allowed_cpus();
    bench.rt_elevate = (mode == ChronosRT_MODE_THROUGHPUT ||
                        mode == ChronosRT_MODE_STRESS)
                       ? (allowed_cpus >= bench.num_threads)
                       : true;

    uint32_t producers = 0;
    
    switch (mode) {
        case ChronosRT_MODE_LATENCY:
            pthread_create(&bench.threads[0], NULL, bench_latency_thread, &bench);
            break;
            
        case ChronosRT_MODE_THROUGHPUT:
            /* Productores y consumidores concurrentes: sin consumidor el pool
             * se agota y se mediría backpressure, no throughput sostenido. */
            if (bench.num_threads < 2) {
                bench.num_threads = 2;
            }
            producers = (bench.num_threads + 1u) / 2u;
            for (uint32_t i = 0; i < bench.num_threads; i++) {
                bench.args[i].bench = &bench;
                bench.args[i].index = i;
                pthread_create(&bench.threads[i], NULL,
                               i < producers ? bench_throughput_producer
                                             : bench_throughput_consumer,
                               &bench.args[i]);
            }
            break;
            
        case ChronosRT_MODE_STRESS:
            for (uint32_t i = 0; i < bench.num_threads; i++) {
                bench.args[i].bench = &bench;
                bench.args[i].index = i;
                pthread_create(&bench.threads[i], NULL, bench_stress_thread,
                               &bench.args[i]);
            }
            break;
            
        case ChronosRT_MODE_JITTER:
        case ChronosRT_MODE_WCET:
            pthread_create(&bench.threads[0], NULL, bench_latency_thread, &bench);
            break;
            
        default:
            return ChronosRT_ERROR_INVALID;
    }
    
    sleep(duration_sec);
    bench.running = false;
    
    uint64_t total_events = 0;
    for (uint32_t i = 0; i < bench.num_threads; i++) {
        pthread_join(bench.threads[i], NULL);
    }
    for (uint32_t i = 0; i < producers; i++) {
        total_events += bench.thread_events[i];
    }

    if (mode == ChronosRT_MODE_THROUGHPUT) {
        /* Control de sanidad: si el consumidor no drena, el productor se
         * bloquea por backpressure y la cifra se estanca en el tamaño del pool. */
        uint64_t consumed = 0;
        for (uint32_t i = producers; i < bench.num_threads; i++) {
            consumed += bench.thread_consumed[i];
        }
        printf("[chronosrt] consumidos: %lu (%u productores, %u consumidores)\n",
               consumed, producers, bench.num_threads - producers);
    }
    
    metrics->total_events = (mode == ChronosRT_MODE_THROUGHPUT)
                            ? total_events : bench.metrics.total_events;
    metrics->wcet_ns = bench.metrics.wcet_ns;
    metrics->best_case_ns = bench.metrics.best_case_ns;
    metrics->min_latency_ns = bench.metrics.min_latency_ns;
    metrics->max_latency_ns = bench.metrics.max_latency_ns;
    metrics->avg_latency_ns = bench.metrics.avg_latency_ns;
    metrics->stddev_ns = bench.metrics.stddev_ns;
    metrics->p50_ns = bench.metrics.p50_ns;
    metrics->p99_ns = bench.metrics.p99_ns;
    metrics->p999_ns = bench.metrics.p999_ns;
    metrics->latency_spikes = bench.metrics.latency_spikes;

    if (!bench.rt_elevate) {
        printf("[chronosrt] SCHED_FIFO: omitido (%u hilos > %u CPUs disponibles)\n",
               bench.num_threads, allowed_cpus);
    } else if (bench.rt_priority_ok) {
        printf("[chronosrt] SCHED_FIFO: aplicado (prioridad 80)\n");
    } else {
        printf("[chronosrt] SCHED_FIFO: no permitido (sigue en SCHED_OTHER)\n");
    }

    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_benchmark_run(ChronosRT_handle_t handle, ChronosRT_benchmark_mode_t mode, uint32_t duration_sec, ChronosRT_metrics_t* metrics) {
    return ChronosRT_benchmark_run_ex(handle, mode, duration_sec, 4u, metrics);
}

ChronosRT_status_t ChronosRT_metrics_report(const ChronosRT_metrics_t* metrics) {
    if (!metrics) {
        return ChronosRT_ERROR_INVALID;
    }
    
    printf("\n=== ChronosRT Benchmark Results ===\n");
    printf("Total Events:        %lu\n", metrics->total_events);
    printf("WCET (ns):           %lu\n", metrics->wcet_ns);
    printf("Best Case (ns):      %lu\n", metrics->best_case_ns);
    printf("Min Latency (ns):    %lu\n", metrics->min_latency_ns);
    printf("Max Latency (ns):    %lu\n", metrics->max_latency_ns);
    printf("Avg Latency (ns):    %lu\n", metrics->avg_latency_ns);
    printf("StdDev (ns):         %lu\n", metrics->stddev_ns);
    if (metrics->p50_ns || metrics->p99_ns || metrics->p999_ns) {
        printf("p50 (ns):            %lu\n", metrics->p50_ns);
        printf("p99 (ns):            %lu\n", metrics->p99_ns);
        printf("p99.9 (ns):          %lu\n", metrics->p999_ns);
    }
    if (metrics->total_events > 0) {
        /* Tasa de picos: muestra cuantitativa de la cola de la distribución. */
        double ppm = (double)metrics->latency_spikes * 1e6
                   / (double)metrics->total_events;
        printf("Spikes > 10 us:      %lu (%.3f ppm)\n",
               metrics->latency_spikes, ppm);
    }
    printf("CPU Usage (%%):       %.2f\n", metrics->cpu_usage_percent);
    printf("Cache Miss Rate:     %.4f%%\n", metrics->cache_miss_rate);
    printf("Branch Miss Rate:    %.4f%%\n", metrics->branch_miss_rate);
    printf("================================\n\n");
    
    if (metrics->wcet_ns == 0) {
        printf("Status: OK - modo sin métricas de latencia\n");
    } else if (metrics->wcet_ns > CHRONOSRT_LATENCY_TARGET_NS) {
        printf("Status: NEEDS OPTIMIZATION - WCET > 10 μs\n");
    } else if (metrics->stddev_ns < 1000) {
        printf("Status: EXCELLENT - Jitter < 1 μs\n");
    } else if (metrics->stddev_ns < 5000) {
        printf("Status: GOOD - Jitter < 5 μs\n");
    } else {
        printf("Status: NEEDS OPTIMIZATION\n");
    }
    
    return ChronosRT_SUCCESS;
}

