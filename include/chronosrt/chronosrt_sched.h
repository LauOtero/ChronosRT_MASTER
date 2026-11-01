#ifndef CHRONOSRT_SCHED_H
#define CHRONOSRT_SCHED_H

#include "chronosrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- RT-1 --- */
/* Política de planificación de tiempo real con presupuesto de admisión. */

typedef struct {
    uint64_t runtime_ns;   /* presupuesto de ejecución por periodo */
    uint64_t deadline_ns;  /* deadline relativo al periodo */
    uint64_t period_ns;    /* periodo */
    int      cpu;          /* -1 = sin fijación de afinidad */
} ChronosRT_deadline_attr_t;

/* ¿El kernel soporta SCHED_DEADLINE? */
CHRONOSRT_API bool ChronosRT_sched_deadline_supported(void);

/* Aplica SCHED_DEADLINE al hilo actual (admisión garantizada por el kernel).
 * Devuelve ChronosRT_ERROR_NOTSUP si el kernel lo rechaza (p. ej. sin -rt). */
CHRONOSRT_API ChronosRT_status_t ChronosRT_sched_set_deadline_current(const ChronosRT_deadline_attr_t* attr);

/* Aplica SCHED_FIFO con prioridad [1..99] al hilo actual. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_sched_set_fifo_current(int priority);

/* Fija la afinidad del hilo actual a `cpu`. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_sched_set_affinity_current(int cpu);

/* ---------------------------------------------------------------- RT-4 --- */
/* Aislamiento de CPU verificado (isolcpus / nohz_full / rcu_nocbs). */

typedef struct {
    bool     isolcpus;
    bool     nohz_full;
    bool     rcu_nocbs;
    uint32_t num_isolated;
    int      isolated[ChronosRT_MAX_CPUS];
    bool     strict_ok;   /* los tres tokens presentes y con CPUs válidas */
} ChronosRT_isolation_t;

CHRONOSRT_API ChronosRT_status_t ChronosRT_sched_isolation_probe(ChronosRT_isolation_t* out);
CHRONOSRT_API bool ChronosRT_sched_cpu_is_isolated(const ChronosRT_isolation_t* iso, int cpu);

/* Imprime por stdout el estado de aislamiento y avisa por núcleo no aislado. */
CHRONOSRT_API void ChronosRT_sched_isolation_print(const ChronosRT_isolation_t* iso);

/* Rango de CPUs disponibles en el sistema. */
CHRONOSRT_API int ChronosRT_sched_online_cpus(void);

#ifdef __cplusplus
}
#endif

#endif
