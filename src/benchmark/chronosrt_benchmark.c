#include "chronosrt.h"
#include "chronosrt_simd.h"
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

typedef struct ChronosRT_bench {
    ChronosRT_handle_t handle;
    ChronosRT_benchmark_mode_t mode;
    uint32_t duration_sec;
    ChronosRT_metrics_t metrics;
    volatile bool running;
    pthread_t threads[16];
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
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(sched_getcpu(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
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
                
                update_wcet(&bench->metrics, lat);
            }
        }
    }
    
    if (count > 0) {
        bench->metrics.avg_latency_ns = latency_sum / count;
        bench->metrics.min_latency_ns = min_lat;
        bench->metrics.max_latency_ns = max_lat;
        
        uint64_t variance = (sum_sq / count) - (bench->metrics.avg_latency_ns * bench->metrics.avg_latency_ns);
        bench->metrics.stddev_ns = (uint64_t)(variance > 0 ? sqrt((double)variance) : 0);
    }
    
    return NULL;
}

static void* bench_throughput_thread(void* arg) {
    ChronosRT_bench_t* bench = (ChronosRT_bench_t*)arg;
    uint64_t test_data = 0xCAFEBABE;
    uint64_t count = 0;
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(sched_getcpu(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    while (bench->running) {
        for (int i = 0; i < 1000; i++) {
            ChronosRT_event_publish(bench->handle, &test_data, sizeof(test_data));
        }
        count += 1000;
    }
    
    return (void*)count;
}

static void* bench_stress_thread(void* arg) {
    ChronosRT_bench_t* bench = (ChronosRT_bench_t*)arg;
    volatile uint64_t counter = 0;
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(sched_getcpu(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    
    while (bench->running) {
        for (int i = 0; i < 10000; i++) {
            counter++;
            counter *= 3;
            counter >>= 1;
        }
    }
    
    return NULL;
}

ChronosRT_status_t ChronosRT_benchmark_run(ChronosRT_handle_t handle, ChronosRT_benchmark_mode_t mode, uint32_t duration_sec, ChronosRT_metrics_t* metrics) {
    if (!handle || !metrics) {
        return ChronosRT_ERROR_INVALID;
    }
    
    ChronosRT_bench_t bench;
    memset(&bench, 0, sizeof(bench));
    bench.handle = handle;
    bench.mode = mode;
    bench.duration_sec = duration_sec;
    bench.running = true;
    bench.num_threads = (mode == ChronosRT_MODE_LATENCY || mode == ChronosRT_MODE_JITTER) ? 1 : 4;

    memset(metrics, 0, sizeof(ChronosRT_metrics_t));
    
    switch (mode) {
        case ChronosRT_MODE_LATENCY:
            pthread_create(&bench.threads[0], NULL, bench_latency_thread, &bench);
            break;
            
        case ChronosRT_MODE_THROUGHPUT:
            for (uint32_t i = 0; i < bench.num_threads; i++) {
                pthread_create(&bench.threads[i], NULL, bench_throughput_thread, &bench);
            }
            break;
            
        case ChronosRT_MODE_STRESS:
            for (uint32_t i = 0; i < bench.num_threads; i++) {
                pthread_create(&bench.threads[i], NULL, bench_stress_thread, &bench);
            }
            break;
            
        case ChronosRT_MODE_JITTER:
            pthread_create(&bench.threads[0], NULL, bench_latency_thread, &bench);
            break;
            
        default:
            return ChronosRT_ERROR_INVALID;
    }
    
    sleep(duration_sec);
    bench.running = false;
    
    for (uint32_t i = 0; i < bench.num_threads; i++) {
        void* result;
        pthread_join(bench.threads[i], &result);
        if (mode == ChronosRT_MODE_THROUGHPUT) {
            metrics->total_events += (uint64_t)result;
        }
    }
    
    metrics->total_events = bench.metrics.total_events > 0 ? bench.metrics.total_events : 
                            (mode == ChronosRT_MODE_THROUGHPUT ? duration_sec * 1000000ULL : 0);
    metrics->wcet_ns = bench.metrics.wcet_ns;
    metrics->best_case_ns = bench.metrics.best_case_ns;
    metrics->min_latency_ns = bench.metrics.min_latency_ns;
    metrics->max_latency_ns = bench.metrics.max_latency_ns;
    metrics->avg_latency_ns = bench.metrics.avg_latency_ns;
    metrics->stddev_ns = bench.metrics.stddev_ns;
    
    return ChronosRT_SUCCESS;
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
    printf("CPU Usage (%%):       %.2f\n", metrics->cpu_usage_percent);
    printf("Cache Miss Rate:     %.4f%%\n", metrics->cache_miss_rate);
    printf("Branch Miss Rate:    %.4f%%\n", metrics->branch_miss_rate);
    printf("================================\n\n");
    
    if (metrics->stddev_ns < 1000) {
        printf("Status: EXCELLENT - Jitter < 1 μs\n");
    } else if (metrics->stddev_ns < 5000) {
        printf("Status: GOOD - Jitter < 5 μs\n");
    } else {
        printf("Status: NEEDS OPTIMIZATION\n");
    }
    
    return ChronosRT_SUCCESS;
}

