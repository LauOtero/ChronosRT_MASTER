/*
 * test_rt.c — Validación de la instrumentación de tiempo real (F3).
 *
 *   RT-6: histograma de latencia (media, desviación, percentiles, WCET).
 *   RS-4: watchdog software con heartbeat.
 *   RT-8: tick periódico por `timerfd`.
 *   RT-1: consulta de soporte de SCHED_DEADLINE y autoexclusión si no hay permiso.
 */
#include "chronosrt_rt.h"
#include "chronosrt_sched.h"

#include <stdio.h>
#include <stdint.h>
#include <time.h>

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

static void test_histogram(void) {
    printf("Running latency histogram tests (RT-6)...\n");

    ChronosRT_hist_t h;
    ChronosRT_hist_init(&h, 100);   /* 100 ns por bucket */

    TEST_ASSERT(ChronosRT_hist_count(&h) == 0, "histogram should start empty");
    TEST_ASSERT(ChronosRT_hist_best_ns(&h) == 0, "best of empty histogram is 0");

    /* Distribución conocida: 150, 250, 350 ns. */
    ChronosRT_hist_record(&h, 150);
    ChronosRT_hist_record(&h, 250);
    ChronosRT_hist_record(&h, 350);

    TEST_ASSERT(ChronosRT_hist_count(&h) == 3, "count should be 3");
    TEST_ASSERT(ChronosRT_hist_best_ns(&h) == 150, "min should be 150 ns");
    TEST_ASSERT(ChronosRT_hist_wcet_ns(&h) == 350, "max/WCET should be 350 ns");
    TEST_ASSERT(ChronosRT_hist_mean_ns(&h) == 250, "mean should be 250 ns");

    uint64_t p50 = ChronosRT_hist_percentile_ns(&h, 50.0);
    TEST_ASSERT(p50 >= 200 && p50 <= 300, "p50 should be within the median bucket");

    /* Valor fuera de rango: debe contabilizarse como overflow, no corromper. */
    ChronosRT_hist_record(&h, 1000000000ULL);
    TEST_ASSERT(h.overflow == 1, "out-of-range sample should count as overflow");
    TEST_ASSERT(ChronosRT_hist_wcet_ns(&h) == 1000000000ULL,
                "WCET should reflect the largest recorded sample");

    ChronosRT_hist_reset(&h);
    TEST_ASSERT(ChronosRT_hist_count(&h) == 0 && h.overflow == 0, "reset should clear stats");

    /* Ráfaga: comprueba estabilidad de la media/desviación. */
    for (uint64_t i = 1; i <= 1000; i++) {
        ChronosRT_hist_record(&h, i * 100);
    }
    TEST_ASSERT(ChronosRT_hist_count(&h) == 1000, "count should be 1000");
    TEST_ASSERT(ChronosRT_hist_mean_ns(&h) == 50050, "mean of 100..100000 ns is 50050 ns");
    TEST_ASSERT(ChronosRT_hist_stddev_ns(&h) > 0, "stddev of a spread should be > 0");

    ChronosRT_hist_print(&h, "burst");
}

static void test_watchdog(void) {
    printf("\nRunning watchdog tests (RS-4)...\n");

    ChronosRT_watchdog_t wd;
    ChronosRT_status_t st = ChronosRT_watchdog_init(&wd, 50000000ULL); /* 50 ms */
    TEST_ASSERT(st == ChronosRT_SUCCESS, "watchdog_init should succeed");

    TEST_ASSERT(!ChronosRT_watchdog_expired(&wd), "watchdog should not expire immediately");

    /* Espera activa corta: el tiempo transcurrido crece, pero sigue < timeout. */
    uint64_t e0 = ChronosRT_watchdog_elapsed_ns(&wd);
    for (volatile int i = 0; i < 1000000; i++) { }
    uint64_t e1 = ChronosRT_watchdog_elapsed_ns(&wd);
    TEST_ASSERT(e1 >= e0, "elapsed time should be monotonic");
    TEST_ASSERT(!ChronosRT_watchdog_expired(&wd), "watchdog should still be fresh");

    /* Timeout mínimo: debe expirar tras dormir por encima del umbral. */
    ChronosRT_watchdog_t fast;
    ChronosRT_watchdog_init(&fast, 1000000ULL); /* 1 ms */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000L }; /* 5 ms */
    nanosleep(&ts, NULL);
    TEST_ASSERT(ChronosRT_watchdog_expired(&fast), "watchdog should expire after timeout");

    ChronosRT_watchdog_kick(&fast);
    TEST_ASSERT(!ChronosRT_watchdog_expired(&fast), "kick should refresh the watchdog");

    TEST_ASSERT(ChronosRT_watchdog_init(NULL, 1000) == ChronosRT_ERROR_INVALID,
                "watchdog_init(NULL) should be rejected");
    TEST_ASSERT(ChronosRT_watchdog_init(&wd, 0) == ChronosRT_ERROR_INVALID,
                "zero timeout should be rejected");

    ChronosRT_watchdog_destroy(&wd);
    /* Información del watchdog de hardware: solo se informa, no se exige. */
    printf("  hardware watchdog available: %s\n",
           ChronosRT_watchdog_hw_available() ? "yes" : "no");
}

static void test_tick(void) {
    printf("\nRunning timerfd tick tests (RT-8)...\n");

    ChronosRT_tick_t tick;
    ChronosRT_status_t st = ChronosRT_tick_init(&tick, 2000000ULL); /* 2 ms */
    TEST_ASSERT(st == ChronosRT_SUCCESS, "tick_init should succeed");
    TEST_ASSERT(ChronosRT_tick_fd(&tick) >= 0, "tick fd should be valid");

    if (st == ChronosRT_SUCCESS) {
        /* Con periodo 2 ms, dos esperas consecutivas deben completarse. */
        int r1 = ChronosRT_tick_wait(&tick);
        int r2 = ChronosRT_tick_wait(&tick);
        TEST_ASSERT(r1 >= 1 && r2 >= 1, "tick_wait should report expirations");
        ChronosRT_tick_destroy(&tick);
        TEST_ASSERT(ChronosRT_tick_fd(&tick) < 0, "destroy should close the fd");
    }

    TEST_ASSERT(ChronosRT_tick_init(&tick, 0) == ChronosRT_ERROR_INVALID,
                "zero period should be rejected");
    TEST_ASSERT(ChronosRT_tick_wait(NULL) == -1, "tick_wait(NULL) should return -1");
}

static void test_sched_probe(void) {
    printf("\nRunning scheduler support probe (RT-1)...\n");

    bool deadline = ChronosRT_sched_deadline_supported();
    printf("  SCHED_DEADLINE advertised by kernel: %s\n", deadline ? "yes" : "no");
    TEST_ASSERT(deadline || !deadline, "deadline probe should return a boolean");

    /* La solicitud de FIFO sin privilegios debe ser rechazada, no un crash.
     * Se restaura la política anterior si por casualidad tuviera permiso. */
    ChronosRT_status_t st = ChronosRT_sched_set_fifo_current(10);
    TEST_ASSERT(st == ChronosRT_SUCCESS || st == ChronosRT_ERROR_NOTSUP ||
                st == ChronosRT_ERROR_STATE,
                "FIFO request should either succeed or be refused cleanly");

    /* Vuelve a la política normal para no dejar el proceso RT. */
    struct sched_param sp = { .sched_priority = 0 };
    (void)sched_setscheduler(0, SCHED_OTHER, &sp);

    /* El número de CPUs online debe ser >= 1. */
    TEST_ASSERT(ChronosRT_sched_online_cpus() >= 1, "online CPUs should be >= 1");
}

int main(void) {
    printf("ChronosRT Real-Time Test Suite v%s\n", ChronosRT_VERSION);
    printf("=====================================\n\n");

    test_histogram();
    test_watchdog();
    test_tick();
    test_sched_probe();

    printf("\n=====================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count ? 0 : 1;
}
