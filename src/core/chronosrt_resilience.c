/*
 * chronosrt_resilience.c — Snapshot A/B (RS-1), WAL (RS-3) y golden image (RS-6).
 */
#include "chronosrt_resilience.h"
#include "chronosrt_simd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------------ */
/* RS-1: snapshot A/B con seqlock                                           */
/* ------------------------------------------------------------------------ */

ChronosRT_status_t ChronosRT_snapshot_init(ChronosRT_snapshot_t* s, uint32_t size) {
    if (!s || size == 0) {
        return ChronosRT_ERROR_INVALID;
    }
    memset(s, 0, sizeof(*s));
    s->size = size;

    for (int i = 0; i < 2; i++) {
        if (posix_memalign((void**)&s->buf[i], CHRONOSRT_CACHELINE, size) != 0 || !s->buf[i]) {
            free(s->buf[0]);
            s->buf[0] = NULL;
            return ChronosRT_ERROR_MEMORY;
        }
        memset(s->buf[i], 0, size);
    }

    atomic_init(&s->seq, 0);
    atomic_init(&s->active, 0);
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_snapshot_write(ChronosRT_snapshot_t* s, const void* data, uint32_t size) {
    if (!s || !data || size > s->size) {
        return ChronosRT_ERROR_INVALID;
    }

    uint32_t cur  = atomic_load_explicit(&s->active, memory_order_relaxed);
    uint32_t next = cur ^ 1u;
    uint32_t seq  = atomic_load_explicit(&s->seq, memory_order_relaxed);

    /* Fase impar: escritores en curso. */
    atomic_store_explicit(&s->seq, seq + 1, memory_order_release);
    memcpy(s->buf[next], data, size);
    /* Fase par: fin de escritura. */
    atomic_store_explicit(&s->seq, seq + 2, memory_order_release);
    /* Publicación del nuevo buffer (release para visibilidad del payload). */
    atomic_store_explicit(&s->active, next, memory_order_release);

    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_snapshot_read(const ChronosRT_snapshot_t* s, void* out, uint32_t size) {
    if (!s || !out || size > s->size) {
        return ChronosRT_ERROR_INVALID;
    }

    for (;;) {
        uint32_t a0 = atomic_load_explicit(&s->active, memory_order_acquire);
        uint32_t s1 = atomic_load_explicit(&s->seq, memory_order_acquire);
        if (s1 & 1u) {
            continue; /* escritura en curso */
        }
        memcpy(out, s->buf[a0], size);
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&s->seq, memory_order_relaxed);
        uint32_t a1 = atomic_load_explicit(&s->active, memory_order_relaxed);
        if (s1 == s2 && a0 == a1) {
            return ChronosRT_SUCCESS; /* instantánea consistente */
        }
    }
}

uint32_t ChronosRT_snapshot_sequence(const ChronosRT_snapshot_t* s) {
    return s ? atomic_load_explicit(&s->seq, memory_order_acquire) : 0;
}

void ChronosRT_snapshot_destroy(ChronosRT_snapshot_t* s) {
    if (!s) {
        return;
    }
    free(s->buf[0]);
    free(s->buf[1]);
    s->buf[0] = NULL;
    s->buf[1] = NULL;
    s->size = 0;
}

/* ------------------------------------------------------------------------ */
/* RS-3: WAL transaccional con fsync por lotes                              */
/* ------------------------------------------------------------------------ */
/*
 * Formato de registro en disco (alineado a 1):
 *   [u32 len][u32 crc][u64 seq][payload len bytes]
 * `crc` cubre seq||payload. La recuperación es idempotente: se detiene en la
 * primera entrada cuyo CRC no cuadre (escritura truncada por fallo).
 */

#define CHRONOSRT_WAL_HDR 16u

ChronosRT_status_t ChronosRT_wal_open(ChronosRT_wal_t* w, const char* path, uint32_t batch) {
    if (!w || !path || strlen(path) >= CHRONOSRT_WAL_PATH_MAX) {
        return ChronosRT_ERROR_INVALID;
    }

    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        return ChronosRT_ERROR_IO;
    }

    memset(w, 0, sizeof(*w));
    w->fd = fd;
    w->batch = batch ? batch : 1;
    snprintf(w->path, sizeof(w->path), "%s", path);
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_wal_append(ChronosRT_wal_t* w, const void* rec, uint32_t len) {
    if (!w || w->fd < 0 || (!rec && len > 0) || len > CHRONOSRT_WAL_REC_MAX) {
        return ChronosRT_ERROR_INVALID;
    }

    uint8_t hdr[CHRONOSRT_WAL_HDR];
    uint64_t seq = w->seq + 1;

    uint32_t crc = 0;
    {
        /* CRC incremental sobre seq (8 bytes) + payload. */
        uint8_t seqle[8];
        for (int i = 0; i < 8; i++) {
            seqle[i] = (uint8_t)(seq >> (8 * i));
        }
        crc = ChronosRT_simd_hash(seqle, sizeof(seqle));
        if (len > 0) {
            crc ^= ChronosRT_simd_hash(rec, len);
        }
    }

    memcpy(hdr, &len, 4);
    memcpy(hdr + 4, &crc, 4);
    memcpy(hdr + 8, &seq, 8);

    if (write(w->fd, hdr, CHRONOSRT_WAL_HDR) != (ssize_t)CHRONOSRT_WAL_HDR) {
        return ChronosRT_ERROR_IO;
    }
    if (len > 0 && write(w->fd, rec, len) != (ssize_t)len) {
        return ChronosRT_ERROR_IO;
    }

    w->seq = seq;
    w->pending++;

    if (w->pending >= w->batch) {
        return ChronosRT_wal_sync(w);
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_wal_sync(ChronosRT_wal_t* w) {
    if (!w || w->fd < 0) {
        return ChronosRT_ERROR_INVALID;
    }
    if (w->pending == 0) {
        return ChronosRT_SUCCESS;
    }
    if (fsync(w->fd) != 0) {
        return ChronosRT_ERROR_IO;
    }
    w->pending = 0;
    return ChronosRT_SUCCESS;
}

uint64_t ChronosRT_wal_seq(const ChronosRT_wal_t* w) {
    return w ? w->seq : 0;
}

void ChronosRT_wal_close(ChronosRT_wal_t* w) {
    if (!w) {
        return;
    }
    if (w->fd >= 0) {
        (void)ChronosRT_wal_sync(w);
        close(w->fd);
        w->fd = -1;
    }
}

int ChronosRT_wal_replay(const char* path,
                         int (*cb)(const void* rec, uint32_t len, void* ud),
                         void* ud) {
    if (!path) {
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    int valid = 0;
    uint8_t* payload = (uint8_t*)malloc(CHRONOSRT_WAL_REC_MAX);
    if (!payload) {
        close(fd);
        return 0;
    }

    for (;;) {
        uint8_t hdr[CHRONOSRT_WAL_HDR];
        ssize_t got = read(fd, hdr, sizeof(hdr));
        if (got == 0) {
            break; /* fin limpio */
        }
        if (got != (ssize_t)sizeof(hdr)) {
            break; /* cabecera truncada */
        }

        uint32_t len, crc;
        uint64_t seq;
        memcpy(&len, hdr, 4);
        memcpy(&crc, hdr + 4, 4);
        memcpy(&seq, hdr + 8, 8);

        if (len > CHRONOSRT_WAL_REC_MAX) {
            break; /* cabecera corrupta */
        }

        ssize_t poff = 0;
        bool short_read = false;
        while (poff < (ssize_t)len) {
            ssize_t n = read(fd, payload + poff, (size_t)((ssize_t)len - poff));
            if (n <= 0) {
                short_read = true;
                break;
            }
            poff += n;
        }
        if (short_read) {
            break;
        }

        uint32_t check;
        {
            uint8_t seqle[8];
            (void)seq;
            for (int i = 0; i < 8; i++) {
                seqle[i] = (uint8_t)(seq >> (8 * i));
            }
            check = ChronosRT_simd_hash(seqle, sizeof(seqle));
            if (len > 0) {
                check ^= ChronosRT_simd_hash(payload, len);
            }
        }
        if (check != crc) {
            break; /* entrada corrupta: fin de la reproducción determinista */
        }

        if (cb) {
            cb(payload, len, ud);
        }
        valid++;
    }

    free(payload);
    close(fd);
    return valid;
}

/* ------------------------------------------------------------------------ */
/* RS-6: golden image de escritura atómica                                  */
/* ------------------------------------------------------------------------ */

uint32_t ChronosRT_golden_crc(const void* data, uint32_t len) {
    if (!data || len == 0) {
        return 0;
    }
    return ChronosRT_simd_hash(data, len);
}

ChronosRT_status_t ChronosRT_golden_save(const char* path, const void* data, uint32_t len) {
    if (!path || (!data && len > 0)) {
        return ChronosRT_ERROR_INVALID;
    }

    char tmp[CHRONOSRT_WAL_PATH_MAX + 8];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return ChronosRT_ERROR_INVALID;
    }

    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        return ChronosRT_ERROR_IO;
    }

    uint32_t crc = ChronosRT_golden_crc(data, len);
    ssize_t wrote = 0;
    if (len > 0) {
        wrote = write(fd, data, len);
    }
    bool ok = (len == 0) || (wrote == (ssize_t)len);

    if (ok) {
        /* Trailer con CRC y longitud para verificación en carga. */
        uint8_t trailer[8];
        memcpy(trailer, &len, 4);
        memcpy(trailer + 4, &crc, 4);
        ok = (write(fd, trailer, sizeof(trailer)) == (ssize_t)sizeof(trailer));
    }
    if (ok) {
        ok = (fsync(fd) == 0);
    }
    close(fd);

    if (!ok) {
        (void)unlink(tmp);
        return ChronosRT_ERROR_IO;
    }

    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return ChronosRT_ERROR_IO;
    }

    /* fsync del directorio para persistir el rename. */
    char dir[CHRONOSRT_WAL_PATH_MAX + 8];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
    }

    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_golden_load(const char* path, void* out, uint32_t cap, uint32_t* out_len) {
    if (!path || !out) {
        return ChronosRT_ERROR_INVALID;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return ChronosRT_ERROR_IO;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) {
        close(fd);
        return ChronosRT_ERROR_CORRUPT;
    }

    uint32_t len = (uint32_t)(st.st_size - 8);
    if (len > cap) {
        close(fd);
        return ChronosRT_ERROR_INVALID;
    }

    uint8_t* tmpbuf = (uint8_t*)malloc(len ? len : 1);
    if (!tmpbuf) {
        close(fd);
        return ChronosRT_ERROR_MEMORY;
    }

    ssize_t rd = 0;
    if (len > 0 && read(fd, tmpbuf, len) != (ssize_t)len) {
        free(tmpbuf);
        close(fd);
        return ChronosRT_ERROR_CORRUPT;
    }
    rd = (ssize_t)len;

    uint8_t trailer[8];
    if (read(fd, trailer, sizeof(trailer)) != (ssize_t)sizeof(trailer)) {
        free(tmpbuf);
        close(fd);
        return ChronosRT_ERROR_CORRUPT;
    }
    close(fd);

    uint32_t stored_len, stored_crc;
    memcpy(&stored_len, trailer, 4);
    memcpy(&stored_crc, trailer + 4, 4);

    if (stored_len != len || (uint32_t)rd != len) {
        free(tmpbuf);
        return ChronosRT_ERROR_CORRUPT;
    }

    uint32_t crc = ChronosRT_golden_crc(tmpbuf, len);
    if (crc != stored_crc) {
        free(tmpbuf);
        return ChronosRT_ERROR_CORRUPT;
    }

    if (len > 0) {
        memcpy(out, tmpbuf, len);
    }
    free(tmpbuf);

    if (out_len) {
        *out_len = len;
    }
    return ChronosRT_SUCCESS;
}
