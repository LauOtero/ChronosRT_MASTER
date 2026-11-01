#include "chronosrt_kernel.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <fcntl.h>

#define CHRONOSRT_KERNEL_CMD_BUF 4096

/* true si el directorio existe (para sched_ext). */
static bool dir_exists(const char* path) {
    return access(path, F_OK) == 0;
}

/* RS-7: comprueba memfd_secret sin retener el descriptor. */
static bool memfd_secret_available(void) {
#if defined(__NR_memfd_secret)
    int fd = (int)syscall(__NR_memfd_secret, 0u);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return errno != ENOSYS;
#else
    return false;
#endif
}

/* Lee hasta len-1 bytes de un fichero en buf y lo termina en '\0'.
 * Devuelve false si el fichero no se puede abrir. */
static bool read_file_contents(const char* path, char* buf, size_t len) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    size_t n = fread(buf, 1, len - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return true;
}

/* true si el fichero existe y su contenido contiene la subcadena needle. */
static bool file_contains(const char* path, const char* needle) {
    char buf[512];
    if (!read_file_contents(path, buf, sizeof(buf))) {
        return false;
    }
    return strstr(buf, needle) != NULL;
}

/* true si el fichero existe y su contenido empieza por prefix. */
static bool file_starts_with(const char* path, const char* prefix) {
    char buf[512];
    if (!read_file_contents(path, buf, sizeof(buf))) {
        return false;
    }
    return strncmp(buf, prefix, strlen(prefix)) == 0;
}

/* true si /proc/cmdline contiene el token indicado (p. ej. "isolcpus="). */
static bool cmdline_has_token(const char* token) {
    char buf[CHRONOSRT_KERNEL_CMD_BUF];
    if (!read_file_contents("/proc/cmdline", buf, sizeof(buf))) {
        return false;
    }
    return strstr(buf, token) != NULL;
}

ChronosRT_status_t ChronosRT_kernel_probe(ChronosRT_kernel_info_t* info) {
    if (!info) {
        return ChronosRT_ERROR_INVALID;
    }

    memset(info, 0, sizeof(*info));

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(info->release, sizeof(info->release), "%s", u.release);
    } else {
        info->release[0] = '\0';
    }

    if (sscanf(info->release, "%u.%u.%u", &info->major, &info->minor, &info->patch) < 1) {
        info->major = 0;
        info->minor = 0;
        info->patch = 0;
    }

    info->preempt_rt =
        file_contains("/sys/kernel/realtime", "1") ||
        file_contains("/proc/version", "PREEMPT_RT") ||
        file_starts_with("/sys/kernel/debug/sched/preempt", "full") ||
        file_starts_with("/sys/kernel/debug/sched/preempt", "rt");

    info->isolcpus   = cmdline_has_token("isolcpus=");
    info->nohz_full  = cmdline_has_token("nohz_full=");
    info->rcu_nocbs  = cmdline_has_token("rcu_nocbs=");

    info->eevdf          = (info->major > 6) || (info->major == 6 && info->minor >= 6);
    info->sched_ext      = (info->major > 6) || (info->major == 6 && info->minor >= 12);
    info->io_uring       = (info->major > 5) || (info->major == 5 && info->minor >= 1);
    info->af_xdp         = (info->major > 4) || (info->major == 4 && info->minor >= 18);
    info->sched_deadline = true;

    /* F7: sched_ext a nivel de versión + disponibilidad real en runtime. */
    info->sched_ext_enabled =
        (info->major > 6 || (info->major == 6 && info->minor >= 12)) &&
        (dir_exists("/sys/kernel/sched_ext") || dir_exists("/sys/kernel/debug/sched_ext"));

    /* RS-7: aislamiento de secretos fuera del espacio mapeable del kernel. */
    info->memfd_secret = memfd_secret_available();

    if (info->major < CHRONOSRT_KERNEL_MIN_MAJOR) {
        info->support_level = 0;
    } else if (info->major >= CHRONOSRT_KERNEL_PREFERRED_MAJOR) {
        info->support_level = 3;
    } else if (info->preempt_rt) {
        info->support_level = 2;
    } else {
        info->support_level = 1;
    }

    return ChronosRT_SUCCESS;
}

void ChronosRT_kernel_print(const ChronosRT_kernel_info_t* info) {
    if (!info) {
        return;
    }

    printf("[chronosrt] Kernel: %u.%u.%u (%s)\n",
           info->major, info->minor, info->patch, info->release);
    printf("[chronosrt] PREEMPT_RT: %s\n", info->preempt_rt ? "sí" : "no");
    printf("[chronosrt] aislamiento: isolcpus=%s nohz_full=%s rcu_nocbs=%s\n",
           info->isolcpus ? "on" : "off",
           info->nohz_full ? "on" : "off",
           info->rcu_nocbs ? "on" : "off");
    printf("[chronosrt] capacidades: eevdf=%s sched_ext=%s io_uring=%s af_xdp=%s "
           "sched_deadline=%s memfd_secret=%s\n",
           info->eevdf ? "on" : "off",
           info->sched_ext_enabled ? "on" : (info->sched_ext ? "kernel" : "off"),
           info->io_uring ? "on" : "off",
           info->af_xdp ? "on" : "off",
           info->sched_deadline ? "on" : "off",
           info->memfd_secret ? "on" : "off");

    if (info->support_level == 0) {
        printf("[chronosrt] AVISO: kernel no soportado (se requiere Linux %d.15+).\n",
               CHRONOSRT_KERNEL_MIN_MAJOR);
    } else if (info->support_level == 3) {
        printf("[chronosrt] Kernel 7.x detectado: soporte pleno\n");
    }

    if (!info->preempt_rt) {
        printf("[chronosrt] AVISO: kernel sin PREEMPT_RT; la latencia determinista puede degradarse.\n");
    }
}

int ChronosRT_kernel_summary(const ChronosRT_kernel_info_t* info, char* buf, size_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    if (!info) {
        buf[0] = '\0';
        return 0;
    }

    snprintf(buf, len, "kernel %u.%u.%u preempt_rt=%d support_level=%u",
             info->major, info->minor, info->patch,
             info->preempt_rt ? 1 : 0, info->support_level);
    return (int)strlen(buf);
}

bool ChronosRT_kernel_is_supported(const ChronosRT_kernel_info_t* info) {
    if (!info) {
        return false;
    }
    return info->support_level >= 1;
}
