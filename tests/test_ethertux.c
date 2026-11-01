/*
 * test_ethertux.c — Validación del protocolo EtherTux.
 *
 * Cubre lo verificable SIN hardware de red:
 *   - selección de protocolo (EtherCAT / EtherTux),
 *   - construcción/parseo de la PDU y validación de CRC-32C,
 *   - modos FEC predefinidos y su overhead,
 *   - QoS: token buckets, degradación, backpressure y LAPM con histéresis,
 *   - planificador time-triggered: ventanas, admisión por ventana y ranuras,
 *   - ciclo de vida del maestro y degradación elegante sin CAP_NET_RAW.
 */
#include "chronosrt_ethertux.h"
#include "chronosrt_hwaccel.h"
#include "chronosrt_protocol.h"
#include "chronosrt.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

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

/* ------------------------------------------------------------------ */

static void test_protocol_selection(void) {
    printf("\nRunning protocol selection tests...\n");

    TEST_ASSERT(ChronosRT_protocol_default() == ChronosRT_PROTOCOL_ETHERCAT,
                "default protocol should be EtherCAT");
    TEST_ASSERT(ChronosRT_protocol_from_string("EtherTux") == ChronosRT_PROTOCOL_ETHERTUX,
                "\"EtherTux\" should select EtherTux");
    TEST_ASSERT(ChronosRT_protocol_from_string("  ethercat ") == ChronosRT_PROTOCOL_ETHERCAT,
                "whitespace and case should be normalised");
    TEST_ASSERT(ChronosRT_protocol_from_string("bogus") == ChronosRT_PROTOCOL_ETHERCAT,
                "unknown names fall back to EtherCAT");
    TEST_ASSERT(ChronosRT_protocol_from_string(NULL) == ChronosRT_PROTOCOL_ETHERCAT,
                "NULL falls back to EtherCAT");
    TEST_ASSERT(strcmp(ChronosRT_protocol_name(ChronosRT_PROTOCOL_ETHERTUX), "ethertux") == 0,
                "canonical name of EtherTux");
    TEST_ASSERT(strcmp(ChronosRT_protocol_name(ChronosRT_PROTOCOL_ETHERCAT), "ethercat") == 0,
                "canonical name of EtherCAT");

    (void)setenv("CHRONOSRT_PROTOCOL", "ethertux", 1);
    TEST_ASSERT(ChronosRT_protocol_from_env() == ChronosRT_PROTOCOL_ETHERTUX,
                "CHRONOSRT_PROTOCOL=ethertux should be honoured");
    (void)unsetenv("CHRONOSRT_PROTOCOL");
    TEST_ASSERT(ChronosRT_protocol_from_env() == ChronosRT_PROTOCOL_ETHERCAT,
                "unset CHRONOSRT_PROTOCOL falls back to EtherCAT");
}

static void test_pdu(void) {
    printf("\nRunning PDU build/parse tests...\n");

    const char* text = "Hola EtherTux!!";
    uint16_t len = (uint16_t)strlen(text);

    ChronosRT_et_header_t h;
    memset(&h, 0, sizeof(h));
    h.type = ChronosRT_ET_TYPE_DATA;
    h.priority = ChronosRT_ET_P2;
    h.state = ChronosRT_ET_STATE_OP;
    h.flow_id = 7u;
    h.seq_num = 42u;
    h.tx_timestamp = 123456789ull;
    h.wkc = 3u;
    h.block_id = 0x0302u;
    h.flow_count = 1u;

    uint8_t buf[ChronosRT_ET_MAX_FRAME];
    uint32_t built = ChronosRT_et_pdu_build(buf, sizeof(buf), &h, text, len);
    TEST_ASSERT(built == ChronosRT_ET_HEADER_SIZE + len,
                "PDU size should be header + payload");
    TEST_ASSERT(buf[0] == ChronosRT_ET_TYPE_DATA, "type should be DATA");
    TEST_ASSERT(buf[1] == ChronosRT_ET_P2, "priority should be P2");
    TEST_ASSERT(buf[3] == ChronosRT_ET_STATE_OP, "ESM state should be preserved");
    TEST_ASSERT(buf[4] == 7u, "flow id should be preserved");
    TEST_ASSERT(buf[17] == 0x03u && buf[18] == 0x00u,
                "ET-WKC should be little-endian");
    TEST_ASSERT(buf[19] == 0x02u && buf[20] == 0x03u,
                "block id should be little-endian");
    TEST_ASSERT(buf[21] == 1u, "flow count should be 1 without aggregation");

    ChronosRT_et_header_t out;
    const uint8_t* payload = NULL;
    TEST_ASSERT(ChronosRT_et_pdu_parse(buf, built, &out, &payload) == built,
                "parse should accept a valid PDU");
    TEST_ASSERT(out.type == h.type && out.priority == h.priority &&
                out.state == h.state && out.flow_id == h.flow_id &&
                out.seq_num == h.seq_num && out.tx_timestamp == h.tx_timestamp &&
                out.wkc == h.wkc && out.block_id == h.block_id &&
                out.flow_count == h.flow_count,
                "all header fields should round-trip");
    TEST_ASSERT(memcmp(payload, text, len) == 0, "payload should round-trip");

    /* Corrupción de la carga útil -> el CRC debe rechazarla. */
    buf[ChronosRT_ET_HEADER_SIZE] ^= 0xFFu;
    TEST_ASSERT(ChronosRT_et_pdu_parse(buf, built, &out, &payload) == 0u,
                "corrupted payload should be rejected");
    buf[ChronosRT_ET_HEADER_SIZE] ^= 0xFFu;

    /* Corrupción del CRC. */
    buf[22] ^= 0x01u;
    TEST_ASSERT(ChronosRT_et_pdu_parse(buf, built, &out, &payload) == 0u,
                "corrupted CRC field should be rejected");
    buf[22] ^= 0x01u;

    /* Búfer insuficiente y longitud inválida. */
    TEST_ASSERT(ChronosRT_et_pdu_build(buf, 8u, &h, text, len) == 0u,
                "build should fail when the buffer is too small");
    TEST_ASSERT(ChronosRT_et_pdu_build(buf, sizeof(buf), &h, text,
                                       (uint16_t)(ChronosRT_ET_MAX_PAYLOAD + 1u)) == 0u,
                "build should reject payloads above the MTU");
    TEST_ASSERT(ChronosRT_et_pdu_parse(buf, 4u, &out, &payload) == 0u,
                "parse should reject a truncated PDU");
    TEST_ASSERT(ChronosRT_et_header_build(NULL, &h) == 0u,
                "header_build(NULL) should be rejected");

    TEST_ASSERT(strcmp(ChronosRT_et_type_name(ChronosRT_ET_TYPE_FEC), "FEC") == 0,
                "type names should be human readable");
    TEST_ASSERT(strcmp(ChronosRT_et_type_name(ChronosRT_ET_TYPE_LAPM), "LAPM") == 0,
                "LAPM type name");
}

static void test_fec_modes(void) {
    printf("\nRunning FEC mode tests...\n");

    ChronosRT_et_fec_t* light = ChronosRT_et_fec_create_mode(ChronosRT_ET_FEC_LIGHT);
    TEST_ASSERT(light != NULL, "LIGHT mode should create a codec");
    TEST_ASSERT(ChronosRT_et_fec_k(light) == 8u && ChronosRT_et_fec_n(light) == 10u,
                "LIGHT should be RS(10,8)");
    TEST_ASSERT(ChronosRT_et_fec_overhead_percent(light) == 25u,
                "LIGHT overhead should be 25 %");
    ChronosRT_et_fec_destroy(light);

    ChronosRT_et_fec_t* robust = ChronosRT_et_fec_create_mode(ChronosRT_ET_FEC_ROBUST);
    TEST_ASSERT(robust != NULL, "ROBUST mode should create a codec");
    TEST_ASSERT(ChronosRT_et_fec_k(robust) == 8u && ChronosRT_et_fec_n(robust) == 16u,
                "ROBUST should be RS(16,8)");
    TEST_ASSERT(ChronosRT_et_fec_overhead_percent(robust) == 100u,
                "ROBUST overhead should be 100 %");
    ChronosRT_et_fec_destroy(robust);

    TEST_ASSERT(ChronosRT_et_fec_create_mode(ChronosRT_ET_FEC_OFF) == NULL,
                "OFF mode should not create a codec");
    TEST_ASSERT(ChronosRT_et_fec_create(8u, 8u) == NULL, "n <= k should be rejected");
    TEST_ASSERT(ChronosRT_et_fec_create(0u, 8u) == NULL, "k = 0 should be rejected");
    ChronosRT_et_fec_destroy(NULL);   /* NULL-safe */
    TEST_ASSERT(ChronosRT_et_fec_k(NULL) == 0u && ChronosRT_et_fec_n(NULL) == 0u,
                "getters should be NULL-safe");
}

static void test_qos(void) {
    printf("\nRunning QoS tests...\n");

    ChronosRT_et_qos_t* q = ChronosRT_et_qos_create();
    TEST_ASSERT(q != NULL, "QoS should be created with defaults");

    TEST_ASSERT(ChronosRT_et_qos_admit(NULL, ChronosRT_ET_P0, 10u, 1000u)
                    == ChronosRT_ERROR_INVALID,
                "admit(NULL) should be rejected");
    TEST_ASSERT(ChronosRT_et_qos_admit(q, 9u, 10u, 1000u) == ChronosRT_ERROR_INVALID,
                "invalid priority should be rejected");

    /* P0: se agota su bucket y NO se descarta -> backpressure. */
    TEST_ASSERT(ChronosRT_et_qos_admit(q, ChronosRT_ET_P0, 4096u, 1000u) == ChronosRT_SUCCESS,
                "P0 should admit the full burst");
    TEST_ASSERT(ChronosRT_et_qos_admit(q, ChronosRT_ET_P0, 1u, 1000u)
                    == ChronosRT_ERROR_BACKPRESSURE,
                "P0 exhaustion should request backpressure, never drop");

    /* La presión de P0 pausa a P2..P5 (no a P0/P1). */
    TEST_ASSERT(ChronosRT_et_qos_backpressure(q, ChronosRT_ET_P2),
                "P0 saturation should pause P2 producers");
    TEST_ASSERT(!ChronosRT_et_qos_backpressure(q, ChronosRT_ET_P0),
                "P0 producers are never paused by their own class");

    /* Tras 1 ms el bucket se reabastece y vuelve a admitir. */
    TEST_ASSERT(ChronosRT_et_qos_admit(q, ChronosRT_ET_P0, 100u, 1000u + 1000000u)
                    == ChronosRT_SUCCESS,
                "P0 should refill over time");

    ChronosRT_et_qos_stats_t qs;
    TEST_ASSERT(ChronosRT_et_qos_stats(q, &qs) == ChronosRT_SUCCESS, "stats should be readable");
    TEST_ASSERT(qs.backpressure >= 1u && qs.dropped == 0u,
                "backpressure should be counted and nothing dropped");
    ChronosRT_et_qos_reset_stats(q);
    (void)ChronosRT_et_qos_stats(q, &qs);
    TEST_ASSERT(qs.admitted == 0u, "reset_stats should clear counters");
    ChronosRT_et_qos_destroy(q);

    /* P5: best-effort, se descarta (drop-tail) al agotarse. */
    ChronosRT_et_qos_t* q5 = ChronosRT_et_qos_create();
    TEST_ASSERT(ChronosRT_et_qos_admit(q5, ChronosRT_ET_P5, 262144u, 1000u)
                    == ChronosRT_SUCCESS,
                "P5 should admit its burst");
    TEST_ASSERT(ChronosRT_et_qos_admit(q5, ChronosRT_ET_P5, 1u, 1000u)
                    == ChronosRT_ERROR_DROPPED,
                "P5 exhaustion should drop the frame");
    ChronosRT_et_qos_destroy(q5);

    /* LAPM con histéresis sobre P3 -> P4 y vuelta a P3. */
    ChronosRT_et_qos_t* ql = ChronosRT_et_qos_create();
    ChronosRT_et_qos_config_t cfg;
    ChronosRT_et_qos_default_config(&cfg);
    for (unsigned i = 0; i < ChronosRT_ET_PRIORITIES; i++) {
        cfg.bucket[i].rate_bytes_per_sec = 1000000u;
        cfg.bucket[i].burst_bytes = 1000u;
    }
    cfg.lapm_enabled = true;
    cfg.lapm_high_percent = 80u;
    cfg.lapm_low_percent = 60u;
    TEST_ASSERT(ChronosRT_et_qos_configure(ql, &cfg) == ChronosRT_SUCCESS,
                "custom QoS configuration should be accepted");

    TEST_ASSERT(ChronosRT_et_qos_admit(ql, ChronosRT_ET_P3, 850u, 1000u) == ChronosRT_SUCCESS,
                "P3 should admit the first frame");
    TEST_ASSERT(ChronosRT_et_qos_effective_priority(ql, ChronosRT_ET_P3) == ChronosRT_ET_P3,
                "no migration before the threshold is evaluated");
    (void)ChronosRT_et_qos_admit(ql, ChronosRT_ET_P3, 1u, 1000u);
    TEST_ASSERT(ChronosRT_et_qos_effective_priority(ql, ChronosRT_ET_P3) == ChronosRT_ET_P4,
                "P3 should migrate to P4 above 80 % occupancy");
    (void)ChronosRT_et_qos_admit(ql, ChronosRT_ET_P3, 1u, 1000000001ull);
    TEST_ASSERT(ChronosRT_et_qos_effective_priority(ql, ChronosRT_ET_P3) == ChronosRT_ET_P3,
                "P3 should return after occupancy falls below 60 %");
    ChronosRT_et_qos_destroy(ql);

    /* Configuraciones inválidas. */
    ChronosRT_et_qos_config_t bad = cfg;
    bad.lapm_high_percent = 50u;
    bad.lapm_low_percent = 60u;
    TEST_ASSERT(ChronosRT_et_qos_configure(NULL, &bad) == ChronosRT_ERROR_INVALID,
                "configure(NULL) should be rejected");
}

static void test_scheduler(void) {
    printf("\nRunning time-triggered scheduler tests...\n");

    ChronosRT_et_sched_t* s = ChronosRT_et_sched_create(1000000u, 1000000000ull);
    TEST_ASSERT(s != NULL, "scheduler should be created");
    TEST_ASSERT(ChronosRT_et_sched_cycle_ns(s) == 1000000u, "cycle should be 1 ms");

    /* Reparto por defecto: 100 / 200 / 300 / 400 µs. */
    TEST_ASSERT(ChronosRT_et_sched_window_at(s, 0u) == 0u, "t=0 is the control window");
    TEST_ASSERT(ChronosRT_et_sched_window_at(s, 99999u) == 0u, "t<100 µs is control");
    TEST_ASSERT(ChronosRT_et_sched_window_at(s, 100000u) == 1u, "t=100 µs starts deterministic");
    TEST_ASSERT(ChronosRT_et_sched_window_at(s, 300000u) == 2u, "t=300 µs starts rate window");
    TEST_ASSERT(ChronosRT_et_sched_window_at(s, 600000u) == 3u, "t=600 µs is best-effort");
    TEST_ASSERT(ChronosRT_et_sched_window_at(s, 1000000u) == 0u, "the cycle wraps");

    /* Admisión por ventana: P0 es express en cualquier instante. */
    TEST_ASSERT(ChronosRT_et_sched_can_transmit(s, ChronosRT_ET_P0, 600000u),
                "P0 is express in any window");
    TEST_ASSERT(ChronosRT_et_sched_can_transmit(s, ChronosRT_ET_P1, 0u),
                "P1 transmits in the control window");
    TEST_ASSERT(!ChronosRT_et_sched_can_transmit(s, ChronosRT_ET_P1, 100000u),
                "P1 cannot transmit outside the control window");
    TEST_ASSERT(ChronosRT_et_sched_can_transmit(s, ChronosRT_ET_P2, 100000u),
                "P2 transmits in the deterministic window");
    TEST_ASSERT(ChronosRT_et_sched_can_transmit(s, ChronosRT_ET_P3, 300000u),
                "P3 transmits in the rate window");
    TEST_ASSERT(ChronosRT_et_sched_can_transmit(s, ChronosRT_ET_P5, 600000u),
                "P5 transmits in the best-effort window");

    TEST_ASSERT(ChronosRT_et_sched_set_windows(s, 100000u, 200000u, 300000u, 400000u)
                    == ChronosRT_SUCCESS,
                "explicit windows covering the cycle should be accepted");
    TEST_ASSERT(ChronosRT_et_sched_set_windows(s, 1u, 1u, 1u, 1u)
                    == ChronosRT_ERROR_INVALID,
                "windows that do not cover the cycle should be rejected");

    TEST_ASSERT(ChronosRT_et_sched_add_slot(s, ChronosRT_ET_P2, 3u, 64u, 120000u)
                    == ChronosRT_SUCCESS,
                "slot insertion should succeed");
    TEST_ASSERT(ChronosRT_et_sched_slot_count(s) == 1u, "one slot should be registered");

    ChronosRT_et_slot_t slot;
    TEST_ASSERT(ChronosRT_et_sched_slot(s, 0u, &slot), "slot readback should work");
    TEST_ASSERT(slot.priority == ChronosRT_ET_P2 && slot.node == 3u &&
                slot.frame_bytes == 64u && slot.offset_ns == 120000u,
                "slot fields should round-trip");

    TEST_ASSERT(ChronosRT_et_sched_add_slot(s, ChronosRT_ET_P2, 4u, 64u, 2000000u)
                    == ChronosRT_ERROR_INVALID,
                "offsets beyond the cycle should be rejected");
    TEST_ASSERT(ChronosRT_et_sched_add_slot(s, ChronosRT_ET_P2, 4u, 0u, 1000u)
                    == ChronosRT_ERROR_INVALID,
                "zero-length slots should be rejected");

    TEST_ASSERT(ChronosRT_et_sched_remove_node(s, 3u) == 1,
                "removing a failed node should free its slot");
    TEST_ASSERT(ChronosRT_et_sched_slot_count(s) == 0u, "slot table should be empty again");

    TEST_ASSERT(ChronosRT_et_tx_time_ns(1518u, 1000000000ull) == 12304ull,
                "1518 B at 1 Gbps should take ~12.3 µs");
    ChronosRT_et_sched_destroy(s);
    ChronosRT_et_sched_destroy(NULL);   /* NULL-safe */
}

static void test_master_lifecycle(void) {
    printf("\nRunning EtherTux master lifecycle tests...\n");

    ChronosRT_et_master_t* m = ChronosRT_et_master_create();
    TEST_ASSERT(m != NULL, "master should be created");
    TEST_ASSERT(!ChronosRT_et_master_is_open(m), "new master starts closed");
    TEST_ASSERT(ChronosRT_et_master_interface(m) == NULL, "no interface before open");

    uint8_t payload[16] = { 0 };
    TEST_ASSERT(ChronosRT_et_master_send(m, ChronosRT_ET_P2, ChronosRT_ET_TYPE_DATA,
                                         1u, payload, sizeof(payload))
                    == ChronosRT_ERROR_STATE,
                "sending on a closed master should fail with ERROR_STATE");

    TEST_ASSERT(ChronosRT_et_master_open(NULL, "lo") == ChronosRT_ERROR_INVALID,
                "open(NULL, ...) should be rejected");
    TEST_ASSERT(ChronosRT_et_master_open(m, NULL) == ChronosRT_ERROR_INVALID,
                "open with a NULL interface should be rejected");
    TEST_ASSERT(ChronosRT_et_master_open(m, "") == ChronosRT_ERROR_INVALID,
                "open with an empty interface should be rejected");

    ChronosRT_status_t st = ChronosRT_et_master_open(m, "chronosrt_et_zzz0");
    TEST_ASSERT(st != ChronosRT_SUCCESS, "open on an absent interface should fail gracefully");
    TEST_ASSERT(!ChronosRT_et_master_is_open(m), "master should stay closed after a failure");

    /* FEC: activación, rechazo de bloques mal dimensionados y desactivación. */
    TEST_ASSERT(ChronosRT_et_master_set_fec(m, ChronosRT_ET_FEC_LIGHT) == ChronosRT_SUCCESS,
                "enabling LIGHT FEC should succeed");

    uint8_t block[8][32];
    const uint8_t* ptrs[8];
    for (unsigned i = 0; i < 8u; i++) {
        memset(block[i], (int)i, sizeof(block[i]));
        ptrs[i] = block[i];
    }
    TEST_ASSERT(ChronosRT_et_master_send_fec_block(m, ChronosRT_ET_P2, 1u, ptrs, 4u,
                                                   sizeof(block[0])) == ChronosRT_ERROR_INVALID,
                "a FEC block with the wrong block count should be rejected");
    TEST_ASSERT(ChronosRT_et_master_send_fec_block(m, ChronosRT_ET_P2, 1u, ptrs, 8u,
                                                   sizeof(block[0])) == ChronosRT_ERROR_STATE,
                "a well-formed FEC block on a closed master should report ERROR_STATE");

    TEST_ASSERT(ChronosRT_et_master_set_fec(m, ChronosRT_ET_FEC_OFF) == ChronosRT_SUCCESS,
                "disabling FEC should succeed");
    TEST_ASSERT(ChronosRT_et_master_send_fec_block(m, ChronosRT_ET_P2, 1u, ptrs, 8u,
                                                   sizeof(block[0])) == ChronosRT_ERROR_NOTSUP,
                "FEC blocks without a codec should report NOTSUP");

    /* Nodos y heartbeat sin tráfico. */
    TEST_ASSERT(ChronosRT_et_master_node_count(m) == 0u, "no nodes before any traffic");
    TEST_ASSERT(ChronosRT_et_master_heartbeat(m, 1000000u) == 0u,
                "heartbeat with no nodes should report zero");

    ChronosRT_et_master_stats_t ms;
    TEST_ASSERT(ChronosRT_et_master_stats(m, &ms) == ChronosRT_SUCCESS, "stats should be readable");
    TEST_ASSERT(ms.frames_tx == 0u && ms.frames_rx == 0u, "stats should start at zero");
    ChronosRT_et_master_reset_stats(m);

    TEST_ASSERT(ChronosRT_et_master_attach_scheduler(m, NULL) == ChronosRT_SUCCESS,
                "detaching the scheduler should succeed");

    ChronosRT_et_qos_config_t cfg;
    ChronosRT_et_qos_default_config(&cfg);
    TEST_ASSERT(ChronosRT_et_master_set_qos(m, &cfg) == ChronosRT_SUCCESS,
                "QoS configuration should be applied to the master");

    ChronosRT_et_master_close(m);      /* idempotente */
    ChronosRT_et_master_destroy(m);
    ChronosRT_et_master_destroy(NULL); /* NULL-safe */
    TEST_ASSERT(true, "master close/destroy should be NULL-safe");
}

int main(void) {
    printf("ChronosRT EtherTux Test Suite v%s\n", ChronosRT_VERSION);
    printf("=================================\n");

    test_protocol_selection();
    test_pdu();
    test_fec_modes();
    test_qos();
    test_scheduler();
    test_master_lifecycle();

    printf("\n=================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count ? 0 : 1;
}
