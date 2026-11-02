/*
 * chronosrt_rt.h — Instrumentación de tiempo real (F3).
 *
 *   RT-6: histograma de latencia para publicar WCET/percentiles MEDIDOS.
 *   RS-4: watchdog software con heartbeat (y detección de watchdog HW).
 *   RT-8: fuente de tick periódico por `timerfd` (sin busy-wait).
 */
#ifndef CHRONOSRT_RT_H
#define CHRONOSRT_RT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "chronosrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- RT-6 --- */

#define CHRONOSRT_HIST_BUCKETS 64

typedef struct {
    uint64_t bucket_ns;                       /* ancho de bucket en ns */
    uint64_t buckets[CHRONOSRT_HIST_BUCKETS];
    uint64_t prefix_sum[CHRONOSRT_HIST_BUCKETS]; /* prefix-sum incremental (B-7) */
    uint64_t count;
    uint64_t sum_ns;
    uint64_t sum_sq_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t overflow;                        /* muestras por encima del rango */
    bool     prefix_sum_valid;               /* cache válida para búsqueda O(log n) */
    /* Recíproco para dividir por `bucket_ns` sin `idiv` (I+D+i: el registro
     * ocurre una vez por muestra y el divisor es variable en runtime). */
    uint64_t bucket_recip;
} ChronosRT_hist_t;

CHRONOSRT_API void     ChronosRT_hist_init(ChronosRT_hist_t* h, uint64_t bucket_ns);
CHRONOSRT_API void     ChronosRT_hist_reset(ChronosRT_hist_t* h);
CHRONOSRT_API void     ChronosRT_hist_record(ChronosRT_hist_t* h, uint64_t latency_ns);
CHRONOSRT_API uint64_t ChronosRT_hist_count(const ChronosRT_hist_t* h);
CHRONOSRT_API uint64_t ChronosRT_hist_mean_ns(const ChronosRT_hist_t* h);
CHRONOSRT_API uint64_t ChronosRT_hist_stddev_ns(const ChronosRT_hist_t* h);
CHRONOSRT_API uint64_t ChronosRT_hist_percentile_ns(ChronosRT_hist_t* h, double p);
CHRONOSRT_API uint64_t ChronosRT_hist_wcet_ns(const ChronosRT_hist_t* h);
CHRONOSRT_API uint64_t ChronosRT_hist_best_ns(const ChronosRT_hist_t* h);
CHRONOSRT_API void     ChronosRT_hist_print(const ChronosRT_hist_t* h, const char* label);

/* ---------------------------------------------------------------- RS-4 --- */

typedef struct {
    _Atomic uint64_t last_kick_ns;
    uint64_t         timeout_ns;
} ChronosRT_watchdog_t;

CHRONOSRT_API ChronosRT_status_t ChronosRT_watchdog_init(ChronosRT_watchdog_t* wd, uint64_t timeout_ns);
CHRONOSRT_API void               ChronosRT_watchdog_kick(ChronosRT_watchdog_t* wd);
CHRONOSRT_API bool               ChronosRT_watchdog_expired(const ChronosRT_watchdog_t* wd);
CHRONOSRT_API uint64_t           ChronosRT_watchdog_elapsed_ns(const ChronosRT_watchdog_t* wd);
CHRONOSRT_API void               ChronosRT_watchdog_destroy(ChronosRT_watchdog_t* wd);
/* ¿Existe /dev/watchdog accesible? (no se abre para no armar el temporizador). */
CHRONOSRT_API bool               ChronosRT_watchdog_hw_available(void);

/* ---------------------------------------------------------------- RT-8 --- */

typedef struct {
    int      fd;
    uint64_t period_ns;
} ChronosRT_tick_t;

CHRONOSRT_API ChronosRT_status_t ChronosRT_tick_init(ChronosRT_tick_t* tick, uint64_t period_ns);
CHRONOSRT_API int                ChronosRT_tick_fd(const ChronosRT_tick_t* tick);
/* Bloquea hasta el siguiente tick. Devuelve nº de expiraciones o -1 en error. */
CHRONOSRT_API int                ChronosRT_tick_wait(ChronosRT_tick_t* tick);
CHRONOSRT_API void               ChronosRT_tick_destroy(ChronosRT_tick_t* tick);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_RT_H */
