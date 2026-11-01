#ifndef ChronosRT_RING_H
#define ChronosRT_RING_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "chronosrt_abi.h"

#define RING_CAPACITY 4096
#define RING_MASK (RING_CAPACITY - 1)

/*
 * Ring SPSC lock-free.
 *
 * PF-7: se añade _Alignas(64) al buffer y software prefetch de la siguiente
 * celda en push/pop para ocultar la latencia de memoria (evita false sharing
 * del array de punteros con los índices atómicos).
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
    _Alignas(CHRONOSRT_CACHELINE) void* buffer[RING_CAPACITY];
    _Alignas(CHRONOSRT_CACHELINE) _Atomic uint32_t head;
    _Atomic uint32_t tail;
    _Atomic uint32_t count;
} ChronosRT_ring_t;

static inline void ChronosRT_ring_init(ChronosRT_ring_t* ring) {
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
    atomic_init(&ring->count, 0);
    for (int i = 0; i < RING_CAPACITY; i++) {
        ring->buffer[i] = NULL;
    }
}

static inline bool ChronosRT_ring_push(ChronosRT_ring_t* ring, void* item) {
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t count = atomic_load_explicit(&ring->count, memory_order_acquire);

    if (count >= RING_CAPACITY) {
        return false;
    }

    /* Prefetch de la celda siguiente mientras se almacena la actual (PF-7). */
    ChronosRT_ring_prefetch(&ring->buffer[(head + 1) & RING_MASK]);

    ring->buffer[head & RING_MASK] = item;
    atomic_store_explicit(&ring->head, head + 1, memory_order_release);
    atomic_fetch_add_explicit(&ring->count, 1, memory_order_release);

    return true;
}

static inline void* ChronosRT_ring_pop(ChronosRT_ring_t* ring) {
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t count = atomic_load_explicit(&ring->count, memory_order_acquire);

    if (count == 0) {
        return NULL;
    }

    ChronosRT_ring_prefetch(&ring->buffer[(tail + 1) & RING_MASK]);

    void* item = ring->buffer[tail & RING_MASK];
    atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
    atomic_fetch_sub_explicit(&ring->count, 1, memory_order_release);

    return item;
}

static inline uint32_t ChronosRT_ring_count(ChronosRT_ring_t* ring) {
    return atomic_load_explicit(&ring->count, memory_order_acquire);
}

static inline bool ChronosRT_ring_is_empty(ChronosRT_ring_t* ring) {
    return atomic_load_explicit(&ring->count, memory_order_acquire) == 0;
}

static inline bool ChronosRT_ring_is_full(ChronosRT_ring_t* ring) {
    return atomic_load_explicit(&ring->count, memory_order_acquire) >= RING_CAPACITY;
}

#endif
