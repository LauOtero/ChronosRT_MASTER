#ifndef ChronosRT_MPMC_H
#define ChronosRT_MPMC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct ChronosRT_mpmc_node {
    _Alignas(64) void* data;
    _Atomic(struct ChronosRT_mpmc_node*) next;
} ChronosRT_mpmc_node_t;

typedef struct ChronosRT_mpmc_queue {
    _Alignas(64) _Atomic(ChronosRT_mpmc_node_t*) head;
    _Alignas(64) _Atomic(ChronosRT_mpmc_node_t*) tail;
    _Alignas(64) _Atomic uint64_t count;
} ChronosRT_mpmc_queue_t;

static inline void ChronosRT_mpmc_init(ChronosRT_mpmc_queue_t* queue) {
    ChronosRT_mpmc_node_t* dummy = (ChronosRT_mpmc_node_t*)malloc(sizeof(ChronosRT_mpmc_node_t));
    dummy->data = NULL;
    dummy->next = NULL;
    
    atomic_init(&queue->head, dummy);
    atomic_init(&queue->tail, dummy);
    atomic_init(&queue->count, 0);
}

static inline void ChronosRT_mpmc_destroy(ChronosRT_mpmc_queue_t* queue) {
    ChronosRT_mpmc_node_t* current = atomic_load(&queue->head);
    while (current != NULL) {
        ChronosRT_mpmc_node_t* next = atomic_load(&current->next);
        free(current);
        current = next;
    }
}

static inline bool ChronosRT_mpmc_push(ChronosRT_mpmc_queue_t* queue, void* data) {
    ChronosRT_mpmc_node_t* node = (ChronosRT_mpmc_node_t*)malloc(sizeof(ChronosRT_mpmc_node_t));
    if (!node) return false;
    
    node->data = data;
    atomic_init(&node->next, NULL);
    
    ChronosRT_mpmc_node_t* tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    ChronosRT_mpmc_node_t* null_ptr = NULL;
    
    while (!atomic_compare_exchange_weak_explicit(
        &tail->next,
        &null_ptr,
        node,
        memory_order_release,
        memory_order_relaxed
    )) {
        null_ptr = NULL;
        if (atomic_load(&tail->next) != NULL) {
            atomic_store_explicit(&queue->tail, atomic_load(&tail->next), memory_order_release);
            tail = atomic_load(&queue->tail);
        }
    }
    
    atomic_store_explicit(&queue->tail, node, memory_order_release);
    atomic_fetch_add_explicit(&queue->count, 1, memory_order_release);
    
    return true;
}

static inline void* ChronosRT_mpmc_pop(ChronosRT_mpmc_queue_t* queue) {
    atomic_fetch_sub_explicit(&queue->count, 1, memory_order_release);
    
    ChronosRT_mpmc_node_t* head = atomic_load_explicit(&queue->head, memory_order_acquire);
    ChronosRT_mpmc_node_t* next = atomic_load_explicit(&head->next, memory_order_acquire);
    
    if (next == NULL) {
        atomic_fetch_add_explicit(&queue->count, 1, memory_order_release);
        return NULL;
    }
    
    void* data = next->data;
    atomic_store_explicit(&queue->head, next, memory_order_release);
    free(head);
    
    return data;
}

static inline uint64_t ChronosRT_mpmc_count(ChronosRT_mpmc_queue_t* queue) {
    return atomic_load_explicit(&queue->count, memory_order_acquire);
}

#endif

