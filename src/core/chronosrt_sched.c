/*
 * chronosrt_sched.c — SCHED_DEADLINE (RT-1) y aislamiento de CPU (RT-4).
 */
#include "chronosrt_sched.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <errno.h>

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif
#ifndef SCHED_FLAG_RECLAIM
#define SCHED_FLAG_RECLAIM 0x01
#endif

/* struct sched_attr según <linux/sched/types.h>; se define localmente para no
 * depender de esa cabecera (no siempre instalada). */
struct chronosrt_sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};

static int chronosrt_sched_setattr(pid_t pid, struct chronosrt_sched_attr* attr, unsigned int flags) {
#if defined(SYS_sched_setattr)
    return (int)syscall(SYS_sched_setattr, pid, attr, flags);
#else
    (void)pid; (void)attr; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

bool ChronosRT_sched_deadline_supported(void) {
    /* Prueba de capacidad: intenta aplicar DEADLINE al hilo actual con un
     * presupuesto holgado; si el kernel lo acepta, queda soportado. */
    struct chronosrt_sched_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.sched_policy = SCHED_DEADLINE;
    attr.sched_runtime  = 1000000ULL;    /* 1 ms */
    attr.sched_deadline = 10000000ULL;   /* 10 ms */
    attr.sched_period   = 10000000ULL;   /* 10 ms */
    if (chronosrt_sched_setattr(0, &attr, 0) == 0) {
        return true;
    }
    /* EPERM (sin CAP_SYS_NICE) no implica falta de soporte del kernel. */
    return errno == EPERM || errno == EACCES;
}

ChronosRT_status_t ChronosRT_sched_set_deadline_current(const ChronosRT_deadline_attr_t* attr) {
    if (!attr || attr->runtime_ns == 0 || attr->period_ns == 0) {
        return ChronosRT_ERROR_INVALID;
    }

    struct chronosrt_sched_attr sa;
    memset(&sa, 0, sizeof(sa));
    sa.size = sizeof(sa);
    sa.sched_policy   = SCHED_DEADLINE;
    sa.sched_runtime  = attr->runtime_ns;
    sa.sched_deadline = attr->deadline_ns ? attr->deadline_ns : attr->period_ns;
    sa.sched_period   = attr->period_ns;

    if (attr->cpu >= 0) {
        (void)ChronosRT_sched_set_affinity_current(attr->cpu);
    }

    if (chronosrt_sched_setattr(0, &sa, 0) != 0) {
        return (errno == EPERM || errno == EACCES) ? ChronosRT_ERROR_NOTSUP
                                                   : ChronosRT_ERROR_CPU;
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_sched_set_fifo_current(int priority) {
    if (priority < 1) priority = 1;
    if (priority > 99) priority = 99;

    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        return ChronosRT_ERROR_NOTSUP;
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_sched_set_affinity_current(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return ChronosRT_ERROR_CPU;
    }
    return ChronosRT_SUCCESS;
}

int ChronosRT_sched_online_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n < 1) {
        return 1;
    }
    return (int)n;
}

/* ------------------------------------------------------------------------ */
/* Parseo de listas de CPU tipo "2-7,10,12-15".                              */
/* ------------------------------------------------------------------------ */
static int chronosrt_parse_cpulist(const char* list, int* out, int max) {
    int count = 0;
    const char* p = list;
    while (*p && count < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        int lo = 0, hi = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { lo = lo * 10 + (*p - '0'); p++; digits++; }
        if (digits == 0) break;

        if (*p == '-') {
            p++;
            while (*p >= '0' && *p <= '9') { hi = hi * 10 + (*p - '0'); p++; }
        } else {
            hi = lo;
        }
        for (int c = lo; c <= hi && count < max; c++) {
            out[count++] = c;
        }
        if (*p == ',') p++;
    }
    return count;
}

/* Extrae el valor de `token` en /proc/cmdline (p. ej. "isolcpus="). */
static bool chronosrt_cmdline_value(const char* token, char* out, size_t outlen) {
    FILE* fp = fopen("/proc/cmdline", "r");
    if (!fp) {
        return false;
    }
    char buf[4096];
    size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[got] = '\0';
    fclose(fp);

    const char* pos = strstr(buf, token);
    if (!pos) {
        return false;
    }
    pos += strlen(token);
    size_t i = 0;
    while (pos[i] && pos[i] != ' ' && pos[i] != '\n' && i + 1 < outlen) {
        out[i] = pos[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

ChronosRT_status_t ChronosRT_sched_isolation_probe(ChronosRT_isolation_t* out) {
    if (!out) {
        return ChronosRT_ERROR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    char val[1024];

    if (chronosrt_cmdline_value("isolcpus=", val, sizeof(val))) {
        out->isolcpus = true;
        if (val[0] != '\0' && val[0] != 'n') { /* ignora "nohz_full" mal anclado */
            out->num_isolated = (uint32_t)chronosrt_parse_cpulist(val, out->isolated, ChronosRT_MAX_CPUS);
        }
    }
    if (chronosrt_cmdline_value("nohz_full=", val, sizeof(val))) {
        out->nohz_full = true;
    }
    if (chronosrt_cmdline_value("rcu_nocbs=", val, sizeof(val))) {
        out->rcu_nocbs = true;
    }

    out->strict_ok = out->isolcpus && out->nohz_full && out->rcu_nocbs && out->num_isolated > 0;
    return ChronosRT_SUCCESS;
}

bool ChronosRT_sched_cpu_is_isolated(const ChronosRT_isolation_t* iso, int cpu) {
    if (!iso) {
        return false;
    }
    for (uint32_t i = 0; i < iso->num_isolated; i++) {
        if (iso->isolated[i] == cpu) {
            return true;
        }
    }
    return false;
}

void ChronosRT_sched_isolation_print(const ChronosRT_isolation_t* iso) {
    if (!iso) {
        return;
    }
    printf("[chronosrt] Aislamiento CPU: isolcpus=%s nohz_full=%s rcu_nocbs=%s (%u aisladas)\n",
           iso->isolcpus ? "on" : "off",
           iso->nohz_full ? "on" : "off",
           iso->rcu_nocbs ? "on" : "off",
           iso->num_isolated);

    if (!iso->strict_ok) {
        printf("[chronosrt] AVISO: aislamiento incompleto; se recomienda "
               "isolcpus=... nohz_full=... rcu_nocbs=... preempt=full\n");
        int online = ChronosRT_sched_online_cpus();
        for (int c = 0; c < online; c++) {
            if (!ChronosRT_sched_cpu_is_isolated(iso, c)) {
                printf("[chronosrt]   CPU %d no aislada\n", c);
            }
        }
    }
}
