/*
 * chronosrt_bounded.h — Cola MPMC acotada lock-free (algoritmo de D. Vyukov).
 *
 * Sustituye a la cola Michael-Scott (PF-3) del informe:
 *   - Sin asignación dinámica por operación (celdas pre-asignadas en init).
 *   - Sin el problema ABA del MPMC con `free` de nodos.
 *   - Índices monótonos + `sequence` por celda con memory_order acquire/release.
 *
 * El camino caliente (push/pop) no toma bloqueos ni reserva memoria.
 * Header-only: dos productores y dos consumidores concurrentes como mínimo.
 */
#ifndef CHRONOSRT_BOUNDED_H
#define CHRONOSRT_BOUNDED_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "chronosrt.h"

#if defined(__x86_64__) || defined(__i386__)
  #include <emmintrin.h>
  #define ChronosRT_prefetch_rw(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#elif defined(__aarch64__)
  #define ChronosRT_prefetch_rw(p) __builtin_prefetch((p), 1, 3)
#else
  #define ChronosRT_prefetch_rw(p) ((void)(p))
#endif

typedef struct ChronosRT_bounded_cell {
    _Atomic uint64_t sequence;
    void* data;
} ChronosRT_bounded_cell_t;

typedef struct ChronosRT_bounded_queue {
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint64_t enqueue_pos;
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint64_t dequeue_pos;
    _Alignas(CHRONOSRT_CACHELINE) ChronosRT_bounded_cell_t* cells;
    uint64_t capacity;   /* potencia de dos */
    uint64_t mask;       /* capacity - 1 */
    void* raw;           /* buffer alineado para liberar */
} ChronosRT_bounded_queue_t;

static inline uint64_t ChronosRT_bounded_next_pow2(uint64_t v) {
    if (v < 2) return 2;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v |= v >> 32;
    return v + 1;
}

/* Inicializa la cola con `capacity` (redondeada a potencia de dos). */
static inline ChronosRT_status_t ChronosRT_bounded_init(ChronosRT_bounded_queue_t* q, uint64_t capacity) {
    if (!q) return ChronosRT_ERROR_INVALID;

    capacity = ChronosRT_bounded_next_pow2(capacity);
    /* posix_memalign garantiza alineación a línea de caché del array de celdas. */
    size_t bytes = (size_t)capacity * sizeof(ChronosRT_bounded_cell_t);
    void* mem = NULL;
    if (posix_memalign(&mem, CHRONOSRT_CACHELINE, bytes) != 0 || !mem) {
        return ChronosRT_ERROR_MEMORY;
    }
    memset(mem, 0, bytes);

    q->raw = mem;
    q->cells = (ChronosRT_bounded_cell_t*)mem;
    q->capacity = capacity;
    q->mask = capacity - 1;
    atomic_init(&q->enqueue_pos, 0);
    atomic_init(&q->dequeue_pos, 0);

    for (uint64_t i = 0; i < capacity; i++) {
        atomic_init(&q->cells[i].sequence, i);
        q->cells[i].data = NULL;
    }
    return ChronosRT_SUCCESS;
}

static inline void ChronosRT_bounded_destroy(ChronosRT_bounded_queue_t* q) {
    if (!q) return;
    free(q->raw);
    q->raw = NULL;
    q->cells = NULL;
    q->capacity = 0;
    q->mask = 0;
}

/* Empuja un elemento. Devuelve false si la cola está llena. Lock-free. */
static inline bool ChronosRT_bounded_push(ChronosRT_bounded_queue_t* q, void* data) {
    ChronosRT_bounded_cell_t* cell;
    uint64_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->cells[pos & q->mask];
        uint64_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        int64_t dif = (int64_t)seq - (int64_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false; /* llena */
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        }
    }

    cell->data = data;
    atomic_store_explicit(&cell->sequence, pos + 1, memory_order_release);
    return true;
}

/* Extrae un elemento. Devuelve false si la cola está vacía. Lock-free. */
static inline bool ChronosRT_bounded_pop(ChronosRT_bounded_queue_t* q, void** out) {
    ChronosRT_bounded_cell_t* cell;
    uint64_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);

    for (;;) {
        cell = &q->cells[pos & q->mask];
        uint64_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        int64_t dif = (int64_t)seq - (int64_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->dequeue_pos, &pos, pos + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return false; /* vacía */
        } else {
            pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        }
    }

    if (out) *out = cell->data;
    cell->data = NULL;
    atomic_store_explicit(&cell->sequence, pos + q->capacity, memory_order_release);
    return true;
}

/* Nº aproximado de elementos en vuelo (no atómico entre posiciones). */
static inline uint64_t ChronosRT_bounded_count(const ChronosRT_bounded_queue_t* q) {
    uint64_t e = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
    uint64_t d = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
    return e - d;
}

static inline bool ChronosRT_bounded_empty(const ChronosRT_bounded_queue_t* q) {
    return ChronosRT_bounded_count(q) == 0;
}

static inline uint64_t ChronosRT_bounded_capacity(const ChronosRT_bounded_queue_t* q) {
    return q->capacity;
}

#endif /* CHRONOSRT_BOUNDED_H */
