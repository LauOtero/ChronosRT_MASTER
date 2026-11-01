/*
 * chronosrt_resilience.h — Resiliencia absoluta (F4).
 *
 *   RS-1: snapshot A/B con seqlock (lectura consistente sin bloqueos).
 *   RS-3: WAL transaccional idempotente con `fsync` por lotes y CRC por entrada.
 *   RS-6: checkpoint con *golden image* de escritura atómica (`rename`).
 */
#ifndef CHRONOSRT_RESILIENCE_H
#define CHRONOSRT_RESILIENCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "chronosrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- RS-1 --- */

typedef struct {
    uint32_t          size;         /* tamaño del payload */
    _Atomic uint32_t  seq;          /* seqlock del escritor */
    _Atomic uint32_t  active;       /* buffer publicado (0 ó 1) */
    uint8_t*          buf[2];       /* doble buffer A/B */
} ChronosRT_snapshot_t;

CHRONOSRT_API ChronosRT_status_t ChronosRT_snapshot_init(ChronosRT_snapshot_t* s, uint32_t size);
CHRONOSRT_API ChronosRT_status_t ChronosRT_snapshot_write(ChronosRT_snapshot_t* s, const void* data, uint32_t size);
CHRONOSRT_API ChronosRT_status_t ChronosRT_snapshot_read(const ChronosRT_snapshot_t* s, void* out, uint32_t size);
CHRONOSRT_API uint32_t           ChronosRT_snapshot_sequence(const ChronosRT_snapshot_t* s);
CHRONOSRT_API void               ChronosRT_snapshot_destroy(ChronosRT_snapshot_t* s);

/* ---------------------------------------------------------------- RS-3 --- */

#define CHRONOSRT_WAL_PATH_MAX 256
#define CHRONOSRT_WAL_REC_MAX  (1u << 20)

typedef struct {
    int      fd;
    char     path[CHRONOSRT_WAL_PATH_MAX];
    uint32_t batch;                 /* entradas entre `fsync` */
    uint32_t pending;
    uint64_t seq;
} ChronosRT_wal_t;

CHRONOSRT_API ChronosRT_status_t ChronosRT_wal_open(ChronosRT_wal_t* w, const char* path, uint32_t batch);
CHRONOSRT_API ChronosRT_status_t ChronosRT_wal_append(ChronosRT_wal_t* w, const void* rec, uint32_t len);
CHRONOSRT_API ChronosRT_status_t ChronosRT_wal_sync(ChronosRT_wal_t* w);
CHRONOSRT_API uint64_t           ChronosRT_wal_seq(const ChronosRT_wal_t* w);
CHRONOSRT_API void               ChronosRT_wal_close(ChronosRT_wal_t* w);

/* Reproduce el WAL verificando CRC; se detiene en la primera entrada corrupta
 * (escritura truncada). Devuelve el nº de entradas válidas reproducidas. */
CHRONOSRT_API int ChronosRT_wal_replay(const char* path,
                                       int (*cb)(const void* rec, uint32_t len, void* ud),
                                       void* ud);

/* ---------------------------------------------------------------- RS-6 --- */

/* Escritura atómica: escribe en `<path>.tmp`, `fsync` y `rename` sobre `path`. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_golden_save(const char* path, const void* data, uint32_t len);
CHRONOSRT_API ChronosRT_status_t ChronosRT_golden_load(const char* path, void* out, uint32_t cap, uint32_t* out_len);
CHRONOSRT_API uint32_t           ChronosRT_golden_crc(const void* data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_RESILIENCE_H */
