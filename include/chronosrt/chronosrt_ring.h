#ifndef ChronosRT_RING_H
#define ChronosRT_RING_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "chronosrt_abi.h"

#define RING_CAPACITY 4096
#define RING_MASK (RING_CAPACITY - 1)

/*
 * Ring SPSC lock-free de verdad.
 *
 * Corrección respecto a la versión anterior: se elimina el contador compartido
 * `count` (dos `fetch_add`/`fetch_sub` por operación sobre la MISMA línea de
 * caché). Productor y consumidor solo escriben su propio índice:
 *
 *   - el productor escribe `head` y lee `tail` (cached para no rebotar la línea),
 *   - el consumidor escribe `tail` y lee `head`.
 *
 * No hay ningún RMW atómico ni línea compartida escrita por ambos lados, que es
 * la propiedad que hace que un SPSC escale a decenas de millones de ops/s.
 */
#if defined(__x86_64__) || defined(__i386__)
  #include <emmintrin.h>
  #define ChronosRT_ring_prefetch(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#elif defined(__aarch64__)
  #define ChronosRT_ring_prefetch(p) __builtin_prefetch((p), 1, 3)
#else
  #define ChronosRT_ring_prefetch(p) ((void)(p))
#endif

typedef struct ChronosRT_ring {
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint32_t head;   /* solo productor */
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint32_t tail;   /* solo consumidor */
    _Alignas(CHRONOSRT_CACHELINE) void* buffer[RING_CAPACITY];
} ChronosRT_ring_t;

static inline void ChronosRT_ring_init(ChronosRT_ring_t* ring) {
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
    for (int i = 0; i < RING_CAPACITY; i++) {
        ring->buffer[i] = NULL;
    }
}

static inline bool ChronosRT_ring_push(ChronosRT_ring_t* ring, void* item) {
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if ((uint32_t)(head - tail) >= RING_CAPACITY) {
        return false;   /* llena */
    }

    ChronosRT_ring_prefetch(&ring->buffer[(head + 1u) & RING_MASK]);
    ring->buffer[head & RING_MASK] = item;
    atomic_store_explicit(&ring->head, head + 1u, memory_order_release);
    return true;
}

static inline void* ChronosRT_ring_pop(ChronosRT_ring_t* ring) {
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (tail == head) {
        return NULL;    /* vacío */
    }

    ChronosRT_ring_prefetch(&ring->buffer[(tail + 1u) & RING_MASK]);
    void* item = ring->buffer[tail & RING_MASK];
    atomic_store_explicit(&ring->tail, tail + 1u, memory_order_release);
    return item;
}

/* Instantánea aproximada (los índices avanzan por separado). */
static inline uint32_t ChronosRT_ring_count(ChronosRT_ring_t* ring) {
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    return head - tail;
}

static inline bool ChronosRT_ring_is_empty(ChronosRT_ring_t* ring) {
    return atomic_load_explicit(&ring->head, memory_order_acquire) ==
           atomic_load_explicit(&ring->tail, memory_order_acquire);
}

static inline bool ChronosRT_ring_is_full(ChronosRT_ring_t* ring) {
    const uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return (uint32_t)(head - tail) >= RING_CAPACITY;
}

#endif
