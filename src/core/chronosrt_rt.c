/*
 * chronosrt_rt.c — Histograma de latencia (RT-6), watchdog (RS-4) y tick (RT-8).
 */
#include "chronosrt_rt.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

/* ------------------------------------------------------------------------ */
/* RT-6: histograma de latencia                                             */
/* ------------------------------------------------------------------------ */

void ChronosRT_hist_init(ChronosRT_hist_t* h, uint64_t bucket_ns) {
    if (!h) {
        return;
    }
    memset(h, 0, sizeof(*h));
    h->bucket_ns = bucket_ns ? bucket_ns : 100; /* 100 ns por bucket por defecto */
    h->min_ns = UINT64_MAX;
}

void ChronosRT_hist_reset(ChronosRT_hist_t* h) {
    if (!h) {
        return;
    }
    uint64_t width = h->bucket_ns;
    ChronosRT_hist_init(h, width);
}

void ChronosRT_hist_record(ChronosRT_hist_t* h, uint64_t latency_ns) {
    if (!h) {
        return;
    }
    uint64_t idx = latency_ns / h->bucket_ns;
    if (idx < CHRONOSRT_HIST_BUCKETS) {
        h->buckets[idx]++;
    } else {
        h->overflow++;
    }
    h->count++;
    h->sum_ns += latency_ns;
    h->sum_sq_ns += latency_ns * latency_ns;
    if (latency_ns < h->min_ns) {
        h->min_ns = latency_ns;
    }
    if (latency_ns > h->max_ns) {
        h->max_ns = latency_ns;
    }
}

uint64_t ChronosRT_hist_count(const ChronosRT_hist_t* h) {
    return h ? h->count : 0;
}

uint64_t ChronosRT_hist_mean_ns(const ChronosRT_hist_t* h) {
    if (!h || h->count == 0) {
        return 0;
    }
    return h->sum_ns / h->count;
}

uint64_t ChronosRT_hist_stddev_ns(const ChronosRT_hist_t* h) {
    if (!h || h->count == 0) {
        return 0;
    }
    double mean = (double)h->sum_ns / (double)h->count;
    double var  = (double)h->sum_sq_ns / (double)h->count - mean * mean;
    if (var <= 0.0) {
        return 0;
    }
    return (uint64_t)sqrt(var);
}

uint64_t ChronosRT_hist_percentile_ns(const ChronosRT_hist_t* h, double p) {
    if (!h || h->count == 0) {
        return 0;
    }
    if (p <= 0.0) p = 0.0;
    if (p >= 100.0) p = 100.0;

    uint64_t target = (uint64_t)((p / 100.0) * (double)h->count);
    if (target == 0) {
        target = 1;
    }

    uint64_t acc = 0;
    for (uint64_t i = 0; i < CHRONOSRT_HIST_BUCKETS; i++) {
        acc += h->buckets[i];
        if (acc >= target) {
            /* límite superior del bucket i */
            return (i + 1) * h->bucket_ns;
        }
    }
    return h->max_ns;
}

uint64_t ChronosRT_hist_wcet_ns(const ChronosRT_hist_t* h) {
    return h ? h->max_ns : 0;
}

uint64_t ChronosRT_hist_best_ns(const ChronosRT_hist_t* h) {
    return (h && h->count > 0) ? h->min_ns : 0;
}

void ChronosRT_hist_print(const ChronosRT_hist_t* h, const char* label) {
    if (!h) {
        return;
    }
    printf("[chronosrt] %s: n=%llu min=%llu ns mean=%llu ns stddev=%llu ns "
           "p99=%llu ns p99.9=%llu ns WCET=%llu ns (overflow=%llu)\n",
           label ? label : "latencia",
           (unsigned long long)h->count,
           (unsigned long long)ChronosRT_hist_best_ns(h),
           (unsigned long long)ChronosRT_hist_mean_ns(h),
           (unsigned long long)ChronosRT_hist_stddev_ns(h),
           (unsigned long long)ChronosRT_hist_percentile_ns(h, 99.0),
           (unsigned long long)ChronosRT_hist_percentile_ns(h, 99.9),
           (unsigned long long)ChronosRT_hist_wcet_ns(h),
           (unsigned long long)h->overflow);
}

/* ------------------------------------------------------------------------ */
/* RS-4: watchdog software con heartbeat                                    */
/* ------------------------------------------------------------------------ */

ChronosRT_status_t ChronosRT_watchdog_init(ChronosRT_watchdog_t* wd, uint64_t timeout_ns) {
    if (!wd || timeout_ns == 0) {
        return ChronosRT_ERROR_INVALID;
    }
    wd->timeout_ns = timeout_ns;
    atomic_init(&wd->last_kick_ns, ChronosRT_get_timestamp_ns());
    return ChronosRT_SUCCESS;
}

void ChronosRT_watchdog_kick(ChronosRT_watchdog_t* wd) {
    if (!wd) {
        return;
    }
    atomic_store_explicit(&wd->last_kick_ns, ChronosRT_get_timestamp_ns(),
                          memory_order_release);
}

uint64_t ChronosRT_watchdog_elapsed_ns(const ChronosRT_watchdog_t* wd) {
    if (!wd) {
        return 0;
    }
    uint64_t last = atomic_load_explicit(&wd->last_kick_ns, memory_order_acquire);
    uint64_t now  = ChronosRT_get_timestamp_ns();
    return (now >= last) ? (now - last) : 0;
}

bool ChronosRT_watchdog_expired(const ChronosRT_watchdog_t* wd) {
    if (!wd) {
        return false;
    }
    return ChronosRT_watchdog_elapsed_ns(wd) > wd->timeout_ns;
}

void ChronosRT_watchdog_destroy(ChronosRT_watchdog_t* wd) {
    if (!wd) {
        return;
    }
    wd->timeout_ns = 0;
}

bool ChronosRT_watchdog_hw_available(void) {
    return access("/dev/watchdog", W_OK) == 0;
}

/* ------------------------------------------------------------------------ */
/* RT-8: tick periódico por timerfd                                         */
/* ------------------------------------------------------------------------ */

ChronosRT_status_t ChronosRT_tick_init(ChronosRT_tick_t* tick, uint64_t period_ns) {
    if (!tick || period_ns == 0) {
        return ChronosRT_ERROR_INVALID;
    }
    tick->fd = -1;
    tick->period_ns = period_ns;

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd < 0) {
        return ChronosRT_ERROR_NOTSUP;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_sec  = (time_t)(period_ns / 1000000000ULL);
    its.it_interval.tv_nsec = (long)(period_ns % 1000000000ULL);
    its.it_value = its.it_interval;

    if (timerfd_settime(fd, 0, &its, NULL) != 0) {
        close(fd);
        return ChronosRT_ERROR_IO;
    }

    tick->fd = fd;
    return ChronosRT_SUCCESS;
}

int ChronosRT_tick_fd(const ChronosRT_tick_t* tick) {
    return tick ? tick->fd : -1;
}

int ChronosRT_tick_wait(ChronosRT_tick_t* tick) {
    if (!tick || tick->fd < 0) {
        return -1;
    }
    uint64_t expirations = 0;
    ssize_t n = read(tick->fd, &expirations, sizeof(expirations));
    if (n != (ssize_t)sizeof(expirations)) {
        return -1;
    }
    return (int)expirations;
}

void ChronosRT_tick_destroy(ChronosRT_tick_t* tick) {
    if (!tick) {
        return;
    }
    if (tick->fd >= 0) {
        close(tick->fd);
        tick->fd = -1;
    }
    tick->period_ns = 0;
}
