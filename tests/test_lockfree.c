/*
 * test_lockfree.c — Validación de las colas lock-free y la observabilidad (F6).
 *
 *   OB-4: autotest de la cola acotada (Vyukov) y del ring SPSC.
 *   OB-2: contadores atómicos.
 *   OB-1: trazas best-effort (sin fallo si el kernel no permite trace_marker).
 *   PF-3: propiedades básicas de la cola acotada (FIFO, lleno/vacío, wrap).
 */
#include "chronosrt_obs.h"
#include "chronosrt_bounded.h"
#include "chronosrt_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int test_count = 0;
static int test_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    test_count++; \
    if (cond) { \
        test_passed++; \
        printf("[PASS] %s\n", msg); \
    } else { \
        printf("[FAIL] %s\n", msg); \
    } \
} while (0)

static void test_bounded_basic(void) {
    printf("Running bounded queue basic tests...\n");

    ChronosRT_bounded_queue_t q;
    ChronosRT_status_t st = ChronosRT_bounded_init(&q, 4);
    TEST_ASSERT(st == ChronosRT_SUCCESS, "bounded_init should succeed");

    /* La capacidad se redondea a potencia de dos (4 ya lo es). */
    TEST_ASSERT(ChronosRT_bounded_capacity(&q) == 4, "capacity should be 4");
    TEST_ASSERT(ChronosRT_bounded_empty(&q), "queue should start empty");

    int items[4] = {10, 20, 30, 40};
    bool ok = true;
    for (int i = 0; i < 4; i++) {
        ok = ok && ChronosRT_bounded_push(&q, &items[i]);
    }
    TEST_ASSERT(ok, "all pushes within capacity should succeed");
    TEST_ASSERT(ChronosRT_bounded_count(&q) == 4, "count should equal capacity");

    int extra = 99;
    TEST_ASSERT(!ChronosRT_bounded_push(&q, &extra), "push beyond capacity must fail");

    void* out = NULL;
    bool fifo = true;
    for (int i = 0; i < 4; i++) {
        if (!ChronosRT_bounded_pop(&q, &out) || out != &items[i]) {
            fifo = false;
        }
    }
    TEST_ASSERT(fifo, "pop should preserve FIFO order");
    TEST_ASSERT(!ChronosRT_bounded_pop(&q, &out), "pop on empty queue must fail");

    ChronosRT_bounded_destroy(&q);
}

static void test_bounded_wrap(void) {
    printf("\nRunning bounded queue wrap-around test...\n");

    ChronosRT_bounded_queue_t q;
    ChronosRT_bounded_init(&q, 8);

    /* Más ciclos push/pop que la capacidad: ejercita el reuso de celdas. */
    uintptr_t sentinel = 0;
    bool ok = true;
    for (int cycle = 0; cycle < 100; cycle++) {
        void* p = (void*)(uintptr_t)(cycle + 1);
        if (!ChronosRT_bounded_push(&q, p)) {
            ok = false;
            break;
        }
        void* out = NULL;
        if (!ChronosRT_bounded_pop(&q, &out) || out != p) {
            ok = false;
            break;
        }
    }
    TEST_ASSERT(ok, "100 push/pop cycles should round-trip correctly");
    TEST_ASSERT(ChronosRT_bounded_empty(&q), "queue should be empty after cycles");

    (void)sentinel;
    ChronosRT_bounded_destroy(&q);
}

static void test_ring_basic(void) {
    printf("\nRunning SPSC ring tests...\n");

    /* El ring son ~32 KiB: se asigna en heap para no cargar la pila. */
    ChronosRT_ring_t* ring = (ChronosRT_ring_t*)malloc(sizeof(ChronosRT_ring_t));
    if (!ring) {
        printf("[SKIP] ring allocation failed\n");
        return;
    }
    ChronosRT_ring_init(ring);

    int a = 1, b = 2;
    TEST_ASSERT(ChronosRT_ring_push(ring, &a), "ring push should succeed");
    TEST_ASSERT(ChronosRT_ring_push(ring, &b), "ring push should succeed");
    TEST_ASSERT(ChronosRT_ring_count(ring) == 2, "ring count should be 2");
    TEST_ASSERT(ChronosRT_ring_pop(ring) == &a, "ring pop should be FIFO");
    TEST_ASSERT(ChronosRT_ring_pop(ring) == &b, "ring pop should be FIFO");
    TEST_ASSERT(ChronosRT_ring_is_empty(ring), "ring should be empty");

    free(ring);
}

static void test_counters(void) {
    printf("\nRunning observability counter tests...\n");

    ChronosRT_counters_t c;
    ChronosRT_counters_init(&c);

    ChronosRT_counters_add(&c, &c.published, 5);
    ChronosRT_counters_add(&c, &c.consumed, 3);
    ChronosRT_counters_add(&c, &c.dropped, 1);

    TEST_ASSERT(atomic_load(&c.published) == 5, "published counter should be 5");
    TEST_ASSERT(atomic_load(&c.consumed) == 3, "consumed counter should be 3");
    TEST_ASSERT(atomic_load(&c.dropped) == 1, "dropped counter should be 1");

    ChronosRT_counters_reset(&c);
    TEST_ASSERT(atomic_load(&c.published) == 0 && atomic_load(&c.consumed) == 0,
                "reset should zero all counters");

    ChronosRT_counters_print(&c, "test");
}

static void test_trace(void) {
    printf("\nRunning trace marker test (best-effort)...\n");

    ChronosRT_trace_t t;
    ChronosRT_status_t st = ChronosRT_trace_init(&t);
    TEST_ASSERT(st == ChronosRT_SUCCESS || st == ChronosRT_ERROR_NOTSUP,
                "trace_init should succeed or report not-supported");

    int n = ChronosRT_trace_mark(&t, "chronosrt: test marker\n");
    TEST_ASSERT(n >= 0, "trace_mark should never fail (no-op if inactive)");

    ChronosRT_trace_destroy(&t);
}

static void test_selftest_bounded(void) {
    printf("\nRunning concurrent bounded-queue self-test (OB-4)...\n");

    ChronosRT_lockfree_stats_t stats;
    ChronosRT_status_t st = ChronosRT_selftest_bounded_queue(4, 4, 50000, &stats);
    printf("  produced=%llu consumed=%llu duplicates=%llu lost=%llu\n",
           (unsigned long long)stats.produced,
           (unsigned long long)stats.consumed,
           (unsigned long long)stats.duplicates,
           (unsigned long long)stats.lost);
    TEST_ASSERT(st == ChronosRT_SUCCESS, "bounded self-test should report no loss/dup");
    TEST_ASSERT(stats.lost == 0, "no elements may be lost");
    TEST_ASSERT(stats.duplicates == 0, "no elements may be duplicated");
}

static void test_selftest_ring(void) {
    printf("\nRunning SPSC ring self-test (OB-4)...\n");

    ChronosRT_lockfree_stats_t stats;
    ChronosRT_status_t st = ChronosRT_selftest_ring(50000, &stats);
    printf("  produced=%llu consumed=%llu duplicates=%llu lost=%llu\n",
           (unsigned long long)stats.produced,
           (unsigned long long)stats.consumed,
           (unsigned long long)stats.duplicates,
           (unsigned long long)stats.lost);
    TEST_ASSERT(st == ChronosRT_SUCCESS, "ring self-test should report no loss/dup");
}

int main(void) {
    printf("ChronosRT Lock-free Test Suite v%s\n", ChronosRT_VERSION);
    printf("=====================================\n\n");

    test_bounded_basic();
    test_bounded_wrap();
    test_ring_basic();
    test_counters();
    test_trace();
    test_selftest_bounded();
    test_selftest_ring();

    printf("\n=====================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count ? 0 : 1;
}
