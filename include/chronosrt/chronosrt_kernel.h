#ifndef ChronosRT_KERNEL_H
#define ChronosRT_KERNEL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "chronosrt.h"

#define CHRONOSRT_KERNEL_MIN_MAJOR      5   /* Linux 5.15+ soportado */
#define CHRONOSRT_KERNEL_PREFERRED_MAJOR 7  /* Linux 7.x soportado plenamente */

typedef struct {
    uint32_t major, minor, patch;
    char     release[128];      /* uname -r */
    bool     preempt_rt;        /* CONFIG_PREEMPT_RT activo */
    bool     isolcpus;          /* token isolcpus= en /proc/cmdline */
    bool     nohz_full;         /* token nohz_full= en /proc/cmdline */
    bool     rcu_nocbs;         /* token rcu_nocbs= en /proc/cmdline */
    bool     eevdf;             /* >= 6.6 (planificador EEVDF por defecto) */
    bool     sched_ext;         /* >= 6.12 (sched_ext / BPF schedulers) */
    bool     io_uring;          /* >= 5.1 */
    bool     af_xdp;            /* >= 4.18 */
    bool     sched_deadline;    /* >= 3.14 */
    bool     sched_ext_enabled; /* sched_ext realmente disponible en runtime */
    bool     memfd_secret;      /* >= 5.14 (memfd_secret disponible) */
    uint32_t support_level;     /* 0=no soportado, 1=básico, 2=tiempo real, 3=kernel 7.x pleno */
} ChronosRT_kernel_info_t;

CHRONOSRT_API ChronosRT_status_t ChronosRT_kernel_probe(ChronosRT_kernel_info_t* info);
CHRONOSRT_API void   ChronosRT_kernel_print(const ChronosRT_kernel_info_t* info);
CHRONOSRT_API int    ChronosRT_kernel_summary(const ChronosRT_kernel_info_t* info, char* buf, size_t len);
CHRONOSRT_API bool   ChronosRT_kernel_is_supported(const ChronosRT_kernel_info_t* info);
#endif
