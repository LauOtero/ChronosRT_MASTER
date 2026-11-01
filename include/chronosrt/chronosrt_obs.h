/*
 * chronosrt_obs.h — Observabilidad y calidad (F6).
 *
 *   OB-1: trazas de baja sobrecarga vía `trace_marker` (ftrace/user_events).
 *   OB-2: contadores atómicos de runtime consultables en vivo.
 *   OB-4: autotest de colas lock-free (detección de pérdidas y duplicados).
 */
#ifndef CHRONOSRT_OBS_H
#define CHRONOSRT_OBS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "chronosrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- OB-2 --- */

typedef struct {
    _Atomic uint64_t published;
    _Atomic uint64_t consumed;
    _Atomic uint64_t dropped;
    _Atomic uint64_t errors;
    _Atomic uint64_t forwarded;
} ChronosRT_counters_t;

CHRONOSRT_API void     ChronosRT_counters_init(ChronosRT_counters_t* c);
CHRONOSRT_API void     ChronosRT_counters_add(ChronosRT_counters_t* c, _Atomic uint64_t* field, uint64_t delta);
CHRONOSRT_API void     ChronosRT_counters_reset(ChronosRT_counters_t* c);
CHRONOSRT_API void     ChronosRT_counters_print(const ChronosRT_counters_t* c, const char* label);

/* ---------------------------------------------------------------- OB-1 --- */

typedef struct {
    int  fd;
    bool active;
} ChronosRT_trace_t;

/* Abre el `trace_marker` de ftrace si el kernel/permiso lo permite. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_trace_init(ChronosRT_trace_t* t);
/* Escribe una marca (no-op si no hay traza activa). Devuelve bytes escritos. */
CHRONOSRT_API int                ChronosRT_trace_mark(ChronosRT_trace_t* t, const char* msg);
CHRONOSRT_API void               ChronosRT_trace_destroy(ChronosRT_trace_t* t);

/* ---------------------------------------------------------------- OB-4 --- */

typedef struct {
    uint64_t produced;
    uint64_t consumed;
    uint64_t duplicates;   /* elementos recibidos más de una vez */
    uint64_t lost;         /* elementos nunca recibidos */
} ChronosRT_lockfree_stats_t;

/* Fuzzing concurrente de la cola acotada: `producers`×`consumers` sobre
 * `total` elementos únicos. Valida ausencia de pérdidas y duplicados. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_selftest_bounded_queue(uint32_t producers,
                                                                 uint32_t consumers,
                                                                 uint64_t total,
                                                                 ChronosRT_lockfree_stats_t* out);

/* Igual para el ring SPSC (1 productor, 1 consumidor). */
CHRONOSRT_API ChronosRT_status_t ChronosRT_selftest_ring(uint64_t total,
                                                         ChronosRT_lockfree_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_OBS_H */
