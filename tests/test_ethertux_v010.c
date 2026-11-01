/*
 * test_ethertux_v2.c — Validación de las funcionalidades v0.1.0 de EtherTux:
 *   ET-ESM (§6.1), ET-Sync + Monitor (§5), registro de nodos/ET-WKC/Hot Connect
 *   (§6.2–6.5), ET-MBX (§9), Flow Aggregator (§7.6), FRM (§8.2) y cabecera v0.1.0.
 *
 * Todo se valida sin hardware de red: son rutas puras de CPU y máquinas de
 * estado, por lo que el resultado no depende de permisos ni de la NIC.
 */
#include "chronosrt.h"
#include "chronosrt_ethertux.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("[PASS] %s\n", msg); \
    } else { \
        printf("[FAIL] %s\n", msg); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Cabecera v0.1.0 y Flow Aggregator                                       */
/* ------------------------------------------------------------------ */

static void test_frame_v2(void) {
    printf("\n[Frame v0.1.0 + Flow Aggregator]\n");

    TEST_ASSERT(ChronosRT_ET_HEADER_SIZE == 26u,
                "v0.1.0 header must be 26 B (suma de campos de la tabla §4.2)");

    const uint8_t a[] = { 1, 2, 3, 4 };
    const uint8_t b[] = { 9, 8 };
    const uint8_t c[] = { 7 };

    ChronosRT_et_flow_entry_t in[3] = {
        { 3u, (uint16_t)sizeof(a), a },
        { 5u, (uint16_t)sizeof(b), b },
        { 9u, (uint16_t)sizeof(c), c },
    };

    uint8_t payload[128];
    uint32_t plen = 0;
    TEST_ASSERT(ChronosRT_et_flowagg_build(in, 3u, payload, sizeof(payload), &plen) ==
                    ChronosRT_SUCCESS,
                "aggregation payload should be built");
    TEST_ASSERT(plen == 3u * 3u + sizeof(a) + sizeof(b) + sizeof(c),
                "aggregation payload size = entries + data");
    TEST_ASSERT(ChronosRT_et_flowagg_saved_bytes(3u, 0u) > 0u,
                "aggregation should save header bytes");

    ChronosRT_et_flow_entry_t views[4];
    uint32_t nviews = 0;
    TEST_ASSERT(ChronosRT_et_flowagg_parse(payload, plen, views, 4u, &nviews) ==
                    ChronosRT_SUCCESS,
                "aggregation payload should parse");
    TEST_ASSERT(nviews == 3u, "parse should recover three flows");
    TEST_ASSERT(views[0].flow_id == 3u && views[0].len == 4u &&
                memcmp(views[0].data, a, 4u) == 0, "first flow should round-trip");
    TEST_ASSERT(views[2].flow_id == 9u && views[2].len == 1u && views[2].data[0] == 7u,
                "last flow should round-trip");

    /* Entradas inválidas. */
    ChronosRT_et_flow_entry_t bad[1] = { { 0u, 1u, a } };
    TEST_ASSERT(ChronosRT_et_flowagg_build(bad, 1u, payload, sizeof(payload), &plen) ==
                    ChronosRT_ERROR_INVALID,
                "FlowID 0 must be rejected");
    TEST_ASSERT(ChronosRT_et_flowagg_parse(payload, 2u, views, 4u, &nviews) ==
                    ChronosRT_ERROR_CORRUPT,
                "truncated aggregation payload must be rejected");

    /* Trama v0.1.0 con agregación: FlowCount > 1. */
    ChronosRT_et_header_t h;
    memset(&h, 0, sizeof(h));
    h.type = ChronosRT_ET_TYPE_DATA;
    h.priority = ChronosRT_ET_P3;
    h.state = ChronosRT_ET_STATE_OP;
    h.flow_id = 1u;
    h.flow_count = 3u;
    h.seq_num = 11u;

    uint8_t frame[256];
    uint32_t built = ChronosRT_et_pdu_build(frame, sizeof(frame), &h, payload, (uint16_t)plen);
    TEST_ASSERT(built == ChronosRT_ET_HEADER_SIZE + plen,
                "aggregated frame should build");

    ChronosRT_et_header_t out;
    const uint8_t* out_payload = NULL;
    TEST_ASSERT(ChronosRT_et_pdu_parse(frame, built, &out, &out_payload) == built,
                "aggregated frame should parse");
    TEST_ASSERT(out.flow_count == 3u, "FlowCount should round-trip");
    TEST_ASSERT(ChronosRT_et_flowagg_parse(out_payload, (uint32_t)built - ChronosRT_ET_HEADER_SIZE,
                                           views, 4u, &nviews) == ChronosRT_SUCCESS &&
                nviews == 3u, "aggregated payload should be decodable");

    /* Las P0 se limitan a 64 B de trama (§4.2). */
    uint8_t big[128];
    memset(big, 0, sizeof(big));
    ChronosRT_et_header_t p0 = h;
    p0.priority = ChronosRT_ET_P0;
    p0.flow_count = 1u;
    TEST_ASSERT(ChronosRT_et_pdu_build(frame, sizeof(frame), &p0, big, 100u) == 0u,
                "P0 frames above 64 B must be rejected");
    TEST_ASSERT(ChronosRT_et_pdu_build(frame, sizeof(frame), &p0, big, 20u) ==
                    ChronosRT_ET_HEADER_SIZE + 20u,
                "P0 frames within 64 B must be accepted");

    /* FlowCount = 0 en el cable es inválido. */
    frame[21] = 0u;
    TEST_ASSERT(ChronosRT_et_pdu_parse(frame, built, &out, &out_payload) == 0u,
                "FlowCount = 0 must be rejected on the wire");
}

/* ------------------------------------------------------------------ */
/* ET-ESM                                                              */
/* ------------------------------------------------------------------ */

static void test_esm(void) {
    printf("\n[ET-ESM]\n");

    ChronosRT_et_esm_t* esm = ChronosRT_et_esm_create();
    TEST_ASSERT(esm != NULL, "ESM should be created");
    TEST_ASSERT(ChronosRT_et_esm_state(esm) == ChronosRT_ET_STATE_INIT,
                "initial state must be INIT");

    /* Tráfico permitido por estado. */
    TEST_ASSERT(ChronosRT_et_esm_allows_traffic(ChronosRT_ET_STATE_INIT,
                                                ChronosRT_ET_TYPE_DISCOVERY,
                                                ChronosRT_ET_P1),
                "INIT only allows Discovery");
    TEST_ASSERT(!ChronosRT_et_esm_allows_traffic(ChronosRT_ET_STATE_INIT,
                                                 ChronosRT_ET_TYPE_DATA, ChronosRT_ET_P2),
                "INIT must not allow data");
    TEST_ASSERT(ChronosRT_et_esm_allows_traffic(ChronosRT_ET_STATE_CONFIG,
                                                ChronosRT_ET_TYPE_MAILBOX_DATA,
                                                ChronosRT_ET_P4),
                "CONFIG allows mailbox");
    TEST_ASSERT(!ChronosRT_et_esm_allows_traffic(ChronosRT_ET_STATE_SAFE,
                                                 ChronosRT_ET_TYPE_DATA, ChronosRT_ET_P2),
                "SAFE blocks P2 data");
    TEST_ASSERT(ChronosRT_et_esm_allows_traffic(ChronosRT_ET_STATE_SAFE,
                                                ChronosRT_ET_TYPE_SYNC_CALIBRATED,
                                                ChronosRT_ET_P1),
                "SAFE allows P1 control");
    TEST_ASSERT(ChronosRT_et_esm_allows_traffic(ChronosRT_ET_STATE_OP,
                                                ChronosRT_ET_TYPE_DATA, ChronosRT_ET_P5),
                "OP allows all traffic");
    TEST_ASSERT(!ChronosRT_et_esm_allows_traffic(ChronosRT_ET_STATE_FAULT,
                                                 ChronosRT_ET_TYPE_DATA, ChronosRT_ET_P0),
                "FAULT quarantines data traffic");

    /* Transiciones válidas e inválidas. */
    TEST_ASSERT(!ChronosRT_et_esm_can_transition(ChronosRT_ET_STATE_INIT,
                                                 ChronosRT_ET_STATE_OP),
                "INIT -> OP is not a valid transition");
    TEST_ASSERT(ChronosRT_et_esm_request(esm, ChronosRT_ET_STATE_OP) ==
                    ChronosRT_ERROR_STATE,
                "request must reject invalid transitions");
    TEST_ASSERT(ChronosRT_et_esm_request(esm, ChronosRT_ET_STATE_CONFIG) ==
                    ChronosRT_SUCCESS, "INIT -> CONFIG should succeed");
    TEST_ASSERT(ChronosRT_et_esm_request(esm, ChronosRT_ET_STATE_SAFE) ==
                    ChronosRT_SUCCESS, "CONFIG -> SAFE should succeed");
    TEST_ASSERT(ChronosRT_et_esm_request(esm, ChronosRT_ET_STATE_OP) ==
                    ChronosRT_SUCCESS, "SAFE -> OP should succeed");
    TEST_ASSERT(ChronosRT_et_esm_request(esm, ChronosRT_ET_STATE_FAULT) ==
                    ChronosRT_SUCCESS, "OP -> FAULT should succeed");
    TEST_ASSERT(ChronosRT_et_esm_request(esm, ChronosRT_ET_STATE_INIT) ==
                    ChronosRT_SUCCESS, "FAULT -> INIT should succeed (recuperación)");

    /* StateChange en el cable. */
    uint8_t pl[2];
    TEST_ASSERT(ChronosRT_et_esm_build_state_change(ChronosRT_ET_STATE_SAFE, 0x42u, pl) == 2u,
                "StateChange payload should be 2 B");
    ChronosRT_et_state_t target = ChronosRT_ET_STATE_INIT;
    uint8_t reason = 0u;
    TEST_ASSERT(ChronosRT_et_esm_parse_state_change(pl, sizeof(pl), &target, &reason) ==
                    ChronosRT_SUCCESS && target == ChronosRT_ET_STATE_SAFE && reason == 0x42u,
                "StateChange payload should round-trip");
    TEST_ASSERT(ChronosRT_et_esm_parse_state_change(pl, 1u, &target, &reason) ==
                    ChronosRT_ERROR_INVALID,
                "short StateChange payload must be rejected");

    ChronosRT_et_state_t st = ChronosRT_ET_STATE_OP;
    TEST_ASSERT(ChronosRT_et_esm_state_from_wire(200u, &st) == false,
                "unknown wire state must be rejected");
    TEST_ASSERT(ChronosRT_et_esm_state_from_wire(ChronosRT_ET_STATE_SAFE, &st) &&
                st == ChronosRT_ET_STATE_SAFE, "valid wire state must be accepted");

    ChronosRT_et_esm_reset(esm);
    TEST_ASSERT(ChronosRT_et_esm_state(esm) == ChronosRT_ET_STATE_INIT,
                "reset should return to INIT");
    ChronosRT_et_esm_destroy(esm);
}

/* ------------------------------------------------------------------ */
/* ET-Sync + Monitor de Ventana                                        */
/* ------------------------------------------------------------------ */

static void test_sync(void) {
    printf("\n[ET-Sync + Sync Monitor]\n");

    ChronosRT_et_sync_t* sync = ChronosRT_et_sync_create();
    TEST_ASSERT(sync != NULL, "sync monitor should be created");

    ChronosRT_et_sync_config_t cfg;
    ChronosRT_et_sync_default_config(&cfg);
    TEST_ASSERT(cfg.warn_threshold_ns == 1000u &&
                cfg.out_of_sync_threshold_ns == 2000u &&
                cfg.report_period_frames == 10u,
                "default thresholds: 1 µs / 2 µs / 10 frames");
    TEST_ASSERT(ChronosRT_et_sync_configure(sync, &cfg) == ChronosRT_SUCCESS,
                "default config should be accepted");
    ChronosRT_et_sync_config_t bad = cfg;
    bad.warn_threshold_ns = cfg.out_of_sync_threshold_ns;
    TEST_ASSERT(ChronosRT_et_sync_configure(sync, &bad) == ChronosRT_ERROR_INVALID,
                "warn threshold must be below the out-of-sync threshold");

    TEST_ASSERT(ChronosRT_et_sync_set_link_delay(sync, 1u, 500u) == ChronosRT_SUCCESS,
                "calibrated switch delay should be stored");

    ChronosRT_et_sync_status_t stt = ChronosRT_ET_SYNC_OUT_OF_SYNC;
    TEST_ASSERT(ChronosRT_et_sync_process_report(sync, 1u, 500, 1000000u, &stt) ==
                    ChronosRT_SUCCESS && stt == ChronosRT_ET_SYNC_OK,
                "offset within ±1 µs should be OK");
    TEST_ASSERT(ChronosRT_et_sync_process_report(sync, 1u, 2000, 2000000u, &stt) ==
                    ChronosRT_SUCCESS && stt == ChronosRT_ET_SYNC_WARN,
                "1.5 µs corrected offset should be WARN");
    TEST_ASSERT(ChronosRT_et_sync_process_report(sync, 1u, 6100, 3000000u, &stt) ==
                    ChronosRT_SUCCESS && stt == ChronosRT_ET_SYNC_OUT_OF_SYNC,
                "5.6 µs corrected offset should be OUT_OF_SYNC");

    int64_t offset = 0;
    TEST_ASSERT(ChronosRT_et_sync_status_of(sync, 1u, &offset, &stt) == ChronosRT_SUCCESS &&
                offset == 5600 && stt == ChronosRT_ET_SYNC_OUT_OF_SYNC,
                "status should expose the corrected offset");

    int64_t drift = 0;
    TEST_ASSERT(ChronosRT_et_sync_drift_ppb(sync, 1u, &drift) == ChronosRT_SUCCESS &&
                drift > 0, "positive drift should be estimated");

    ChronosRT_et_sync_stats_t stats;
    TEST_ASSERT(ChronosRT_et_sync_stats(sync, &stats) == ChronosRT_SUCCESS &&
                stats.nodes == 1u && stats.out_of_sync == 1u,
                "stats should reflect the single out-of-sync node");

    TEST_ASSERT(ChronosRT_et_sync_stats(NULL, &stats) == ChronosRT_ERROR_INVALID,
                "stats(NULL) must be rejected");
    TEST_ASSERT(ChronosRT_et_sync_set_link_delay(sync, 0u, 1u) == ChronosRT_ERROR_INVALID,
                "node 0 is not a valid NodeID");

    /* Payloads SyncCalibrated / SyncReport. */
    uint8_t pl[8];
    uint64_t master_ns = 0;
    TEST_ASSERT(ChronosRT_et_sync_build_calibrated(987654321ull, pl) == 8u &&
                ChronosRT_et_sync_parse_calibrated(pl, sizeof(pl), &master_ns) ==
                    ChronosRT_SUCCESS && master_ns == 987654321ull,
                "SyncCalibrated payload should round-trip");
    int64_t rep = 0;
    TEST_ASSERT(ChronosRT_et_sync_build_report(-1234, pl) == 8u &&
                ChronosRT_et_sync_parse_report(pl, sizeof(pl), &rep) == ChronosRT_SUCCESS &&
                rep == -1234, "SyncReport payload should round-trip (signed)");

    ChronosRT_et_sync_destroy(sync);
}

/* ------------------------------------------------------------------ */
/* Registro de nodos, ET-WKC y Hot Connect                             */
/* ------------------------------------------------------------------ */

static void test_topology(void) {
    printf("\n[Topology + ET-WKC + Hot Connect]\n");

    ChronosRT_et_topology_t* topo = ChronosRT_et_topology_create();
    TEST_ASSERT(topo != NULL, "node registry should be created");

    ChronosRT_et_topology_config_t cfg;
    ChronosRT_et_topology_default_config(&cfg);
    TEST_ASSERT(cfg.wkc_mismatch_threshold == 3u &&
                cfg.hot_connect_period_frames == 100u &&
                cfg.heartbeat_timeout_frames == 3u,
                "defaults: 3 mismatches / 100 frames / 3 heartbeats");
    TEST_ASSERT(ChronosRT_et_topology_configure(topo, &cfg) == ChronosRT_SUCCESS,
                "default topology config should be accepted");

    ChronosRT_et_node_info_t n1;
    memset(&n1, 0, sizeof(n1));
    n1.id = 1u;
    n1.mac[5] = 0x11u;
    n1.link_mbps = 1000u;
    n1.buffer_bytes = 2048u;
    n1.fec_supported = true;
    n1.slot = ChronosRT_ET_SLOT_FREE;
    n1.state = ChronosRT_ET_STATE_OP;
    n1.expected_wkc = 2u;

    TEST_ASSERT(ChronosRT_et_topology_upsert(topo, &n1) == ChronosRT_SUCCESS,
                "node 1 should be registered");
    TEST_ASSERT(ChronosRT_et_topology_count(topo) == 1u, "registry should hold 1 node");
    TEST_ASSERT(ChronosRT_et_topology_find(topo, 1u) != NULL, "node 1 should be found");
    TEST_ASSERT(ChronosRT_et_topology_find(topo, 9u) == NULL,
                "unknown node should not be found");

    TEST_ASSERT(ChronosRT_et_topology_next_free_slot(topo) == 0u,
                "first free slot should be 0");
    TEST_ASSERT(ChronosRT_et_topology_assign_slot(topo, 1u, 5u) == ChronosRT_SUCCESS,
                "slot assignment should succeed");
    TEST_ASSERT(ChronosRT_et_topology_find(topo, 1u)->slot == 5u,
                "assigned slot should be visible");
    TEST_ASSERT(ChronosRT_et_topology_assign_slot(topo, 9u, 6u) == ChronosRT_ERROR_NOTFOUND,
                "assigning to an unknown node must fail");

    /* ET-WKC correcto. */
    ChronosRT_et_header_t h;
    memset(&h, 0, sizeof(h));
    h.state = ChronosRT_ET_STATE_OP;
    h.wkc = 2u;
    uint64_t now = 1000000u;
    TEST_ASSERT(ChronosRT_et_topology_on_frame(topo, n1.mac, &h, now) == ChronosRT_SUCCESS,
                "matching ET-WKC should be accepted");
    TEST_ASSERT(!ChronosRT_et_topology_needs_topo_check(topo, NULL),
                "no TopoCheck needed while the WKC matches");

    /* Tres discrepancias consecutivas -> TopoCheck (§6.3). */
    h.wkc = 1u;
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT(ChronosRT_et_topology_on_frame(topo, n1.mac, &h, now + (uint64_t)i) ==
                        ChronosRT_ERROR_CORRUPT,
                    "mismatched ET-WKC should be reported");
    }
    uint16_t suspect = 0u;
    TEST_ASSERT(ChronosRT_et_topology_needs_topo_check(topo, &suspect) && suspect == 1u,
                "3 mismatches should trigger a directed TopoCheck");

    /* La coincidencia vuelve a poner el contador a cero. */
    h.wkc = 2u;
    (void)ChronosRT_et_topology_on_frame(topo, n1.mac, &h, now + 10u);
    TEST_ASSERT(!ChronosRT_et_topology_needs_topo_check(topo, NULL),
                "a matching frame should clear the mismatch counter");

    /* Hot Connect: solo en múltiplos del periodo. */
    uint16_t addr = 0u;
    TEST_ASSERT(!ChronosRT_et_topology_hot_connect_due(topo, 99u, &addr),
                "Hot Connect is not due before 100 frames");
    TEST_ASSERT(ChronosRT_et_topology_hot_connect_due(topo, 100u, &addr) && addr == 2u,
                "Hot Connect probes the next free address every 100 frames");

    /* DiscoveryReply. */
    ChronosRT_et_node_info_t reply = *ChronosRT_et_topology_find(topo, 1u);
    uint8_t wire[ChronosRT_ET_DISCOVERY_REPLY_SIZE];
    TEST_ASSERT(ChronosRT_et_topology_build_reply(&n1, wire) ==
                    ChronosRT_ET_DISCOVERY_REPLY_SIZE,
                "DiscoveryReply should be 8 B");
    ChronosRT_et_node_info_t parsed;
    TEST_ASSERT(ChronosRT_et_topology_parse_reply(wire, sizeof(wire), &parsed) ==
                    ChronosRT_SUCCESS && parsed.id == 1u && parsed.link_mbps == 1000u &&
                parsed.buffer_bytes == 2048u && parsed.fec_supported,
                "DiscoveryReply should round-trip");
    (void)reply;

    /* Heartbeat: sin tramas durante 3 superframes -> FAULT y slot liberado. */
    uint32_t faulted = ChronosRT_et_topology_tick(topo, now + 10u + 3u * 1000000u + 1u,
                                                  1000000u);
    TEST_ASSERT(faulted == 1u, "node should be quarantined after 3 superframes");
    TEST_ASSERT(ChronosRT_et_topology_find(topo, 1u)->state == ChronosRT_ET_STATE_FAULT,
                "node should be in FAULT");
    TEST_ASSERT(ChronosRT_et_topology_find(topo, 1u)->slot == ChronosRT_ET_SLOT_FREE,
                "quarantined node should release its slot");

    TEST_ASSERT(ChronosRT_et_topology_remove(topo, 1u) == ChronosRT_SUCCESS &&
                ChronosRT_et_topology_count(topo) == 0u, "node should be removed");
    TEST_ASSERT(ChronosRT_et_topology_remove(topo, 1u) == ChronosRT_ERROR_NOTFOUND,
                "removing an unknown node must fail");

    ChronosRT_et_topology_destroy(topo);
}

/* ------------------------------------------------------------------ */
/* ET-MBX                                                              */
/* ------------------------------------------------------------------ */

static void test_mbx(void) {
    printf("\n[ET-MBX]\n");

    ChronosRT_et_mbx_t* mbx = ChronosRT_et_mbx_create();
    TEST_ASSERT(mbx != NULL, "mailbox should be created");

    ChronosRT_et_mbx_config_t cfg;
    ChronosRT_et_mbx_default_config(&cfg);
    TEST_ASSERT(cfg.max_retries == 3u && cfg.segment_bytes == 512u &&
                cfg.p4_degrade_percent == 70u,
                "defaults: 3 retries / 512 B / 70 %");
    TEST_ASSERT(ChronosRT_et_mbx_configure(mbx, &cfg) == ChronosRT_SUCCESS,
                "default mailbox config should be accepted");

    /* 1200 B -> 3 segmentos de 512, 512 y 176. */
    TEST_ASSERT(ChronosRT_et_mbx_begin(mbx, 0u, 1200u, false) == ChronosRT_SUCCESS,
                "transfer should start");
    ChronosRT_et_mbx_channel_status_t st;
    TEST_ASSERT(ChronosRT_et_mbx_status(mbx, 0u, &st) == ChronosRT_SUCCESS &&
                st.total_segments == 3u && st.segment == 0u,
                "1200 B should split into 3 segments");

    uint8_t data[512];
    memset(data, 0xA5, sizeof(data));
    uint8_t seg[600];
    uint32_t seglen = 0;
    uint16_t seq = st.seq;

    TEST_ASSERT(ChronosRT_et_mbx_next_segment(mbx, 0u, data, 512u, seg, sizeof(seg),
                                              &seglen) == ChronosRT_SUCCESS &&
                seglen == 512u + 8u, "first segment = 8 B header + 512 B");
    uint8_t ch = 0xFFu;
    uint16_t rseq = 0u, ridx = 0u, rtot = 0u;
    const uint8_t* rdata = NULL;
    uint16_t rlen = 0u;
    TEST_ASSERT(ChronosRT_et_mbx_parse_data(seg, seglen, &ch, &rseq, &ridx, &rtot,
                                            &rdata, &rlen) == ChronosRT_SUCCESS &&
                ch == 0u && rseq == seq && ridx == 0u && rtot == 3u && rlen == 512u,
                "MailboxData should round-trip the segment");

    TEST_ASSERT(ChronosRT_et_mbx_on_ack(mbx, 0u, seq, true, 1000u) == ChronosRT_SUCCESS,
                "ACK should advance the transfer");
    TEST_ASSERT(ChronosRT_et_mbx_status(mbx, 0u, &st) == ChronosRT_SUCCESS &&
                st.segment == 1u && st.state == ChronosRT_ET_MBX_SENDING,
                "state should be SENDING for the next segment");

    /* NACK: reintento con backoff exponencial. */
    TEST_ASSERT(ChronosRT_et_mbx_next_segment(mbx, 0u, data, 512u, seg, sizeof(seg),
                                              &seglen) == ChronosRT_SUCCESS,
                "second segment should be built");
    TEST_ASSERT(ChronosRT_et_mbx_on_ack(mbx, 0u, seq, false, 2000u) ==
                    ChronosRT_ERROR_BACKPRESSURE,
                "NACK should schedule a retry");
    TEST_ASSERT(!ChronosRT_et_mbx_retry_due(mbx, 0u, 2100u),
                "retry should respect the backoff (200 µs)");
    TEST_ASSERT(ChronosRT_et_mbx_retry_due(mbx, 0u, 2000u + 200000u),
                "retry should be due after the backoff");
    TEST_ASSERT(ChronosRT_et_mbx_status(mbx, 0u, &st) == ChronosRT_SUCCESS && st.retries == 1u,
                "retry counter should be 1");

    /* Reemisión del mismo segmento: el índice no avanza. */
    TEST_ASSERT(ChronosRT_et_mbx_next_segment(mbx, 0u, data, 512u, seg, sizeof(seg),
                                              &seglen) == ChronosRT_SUCCESS &&
                ChronosRT_et_mbx_parse_data(seg, seglen, NULL, NULL, &ridx, NULL, NULL,
                                            NULL) == ChronosRT_SUCCESS && ridx == 1u,
                "retransmission must reemit the same segment index");

    /* Agotar los reintentos -> FAILED. */
    (void)ChronosRT_et_mbx_on_ack(mbx, 0u, seq, false, 3000u);
    TEST_ASSERT(ChronosRT_et_mbx_on_ack(mbx, 0u, seq, false, 4000u) ==
                    ChronosRT_ERROR_TIMEOUT,
                "third failure should exhaust the retries");
    TEST_ASSERT(ChronosRT_et_mbx_status(mbx, 0u, &st) == ChronosRT_SUCCESS &&
                st.state == ChronosRT_ET_MBX_FAILED,
                "channel should be in FAILED");

    /* Prioridad efectiva: degradación P4 -> P5 por encima del 70 %. */
    TEST_ASSERT(ChronosRT_et_mbx_effective_priority(mbx, 10u) == ChronosRT_ET_P4,
                "mailbox runs at P4 under low occupancy");
    TEST_ASSERT(ChronosRT_et_mbx_effective_priority(mbx, 90u) == ChronosRT_ET_P5,
                "mailbox degrades to P5 above 70 % of P4");

    /* ACK en el cable. */
    uint8_t ack[ChronosRT_ET_MBX_ACK_SIZE];
    bool accepted = true;
    TEST_ASSERT(ChronosRT_et_mbx_build_ack(3u, 77u, false, ack) == ChronosRT_ET_MBX_ACK_SIZE &&
                ChronosRT_et_mbx_parse_ack(ack, sizeof(ack), &ch, &rseq, &accepted) ==
                    ChronosRT_SUCCESS && ch == 3u && rseq == 77u && !accepted,
                "MailboxAck should round-trip");
    TEST_ASSERT(ChronosRT_et_mbx_parse_ack(ack, 2u, &ch, &rseq, &accepted) ==
                    ChronosRT_ERROR_INVALID,
                "short MailboxAck must be rejected");
    TEST_ASSERT(ChronosRT_et_mbx_begin(mbx, ChronosRT_ET_MBX_CHANNELS, 1u, false) ==
                    ChronosRT_ERROR_INVALID,
                "invalid channel must be rejected");

    ChronosRT_et_mbx_destroy(mbx);
}

/* ------------------------------------------------------------------ */
/* FRM                                                                 */
/* ------------------------------------------------------------------ */

static void test_frm(void) {
    printf("\n[FRM]\n");

    ChronosRT_et_frm_t* frm = ChronosRT_et_frm_create();
    TEST_ASSERT(frm != NULL, "FRM should be created");

    TEST_ASSERT(ChronosRT_et_frm_should_duplicate(frm, ChronosRT_ET_P0),
                "P0 must be duplicated");
    TEST_ASSERT(ChronosRT_et_frm_should_duplicate(frm, ChronosRT_ET_P1),
                "P1 must be duplicated");
    TEST_ASSERT(!ChronosRT_et_frm_should_duplicate(frm, ChronosRT_ET_P2),
                "P2 must not be duplicated");

    uint8_t alt = ChronosRT_et_frm_alternate_flow(1u);
    TEST_ASSERT(alt == 0x81u && alt != 1u, "alternate flow flips bit 7");
    TEST_ASSERT(ChronosRT_et_frm_alternate_flow(0x80u) == 1u,
                "alternate flow never yields FlowID 0");

    bool is_new = false;
    TEST_ASSERT(ChronosRT_et_frm_note_rx(frm, 1u, 100u, false, &is_new) ==
                    ChronosRT_SUCCESS && is_new, "first copy should be delivered");
    TEST_ASSERT(ChronosRT_et_frm_note_rx(frm, 1u, 100u, true, &is_new) ==
                    ChronosRT_SUCCESS && !is_new, "duplicate copy must be dropped");
    TEST_ASSERT(ChronosRT_et_frm_note_rx(frm, 1u, 101u, true, &is_new) ==
                    ChronosRT_SUCCESS && is_new, "new SeqNum after the duplicate is new");

    ChronosRT_et_frm_stats_t stats;
    TEST_ASSERT(ChronosRT_et_frm_stats(frm, &stats) == ChronosRT_SUCCESS &&
                stats.dropped_dup == 1u && stats.duplicates_rx == 1u,
                "stats should count duplicates");

    TEST_ASSERT(ChronosRT_et_frm_active_path(frm, 1u) == 0u, "primary path by default");
    TEST_ASSERT(ChronosRT_et_frm_fail_path(frm, 1u) == ChronosRT_SUCCESS,
                "path switch should succeed");
    TEST_ASSERT(ChronosRT_et_frm_active_path(frm, 1u) == 1u, "path should be alternate");
    TEST_ASSERT(ChronosRT_et_frm_stats(frm, &stats) == ChronosRT_SUCCESS &&
                stats.path_switches == 1u, "path switch should be counted");
    TEST_ASSERT(ChronosRT_et_frm_fail_path(frm, 0u) == ChronosRT_ERROR_INVALID,
                "node 0 is not valid");

    ChronosRT_et_frm_reset(frm);
    TEST_ASSERT(ChronosRT_et_frm_active_path(frm, 1u) == 0u,
                "reset should restore the primary path");

    ChronosRT_et_frm_destroy(frm);
}

/* ------------------------------------------------------------------ */
/* Integración en el maestro                                           */
/* ------------------------------------------------------------------ */

static void test_master_v2(void) {
    printf("\n[Master v0.1.0]\n");

    ChronosRT_et_master_t* m = ChronosRT_et_master_create();
    TEST_ASSERT(m != NULL, "master should be created");

    ChronosRT_et_esm_t* esm = ChronosRT_et_esm_create();
    ChronosRT_et_sync_t* sync = ChronosRT_et_sync_create();
    ChronosRT_et_topology_t* topo = ChronosRT_et_topology_create();
    ChronosRT_et_mbx_t* mbx = ChronosRT_et_mbx_create();
    ChronosRT_et_frm_t* frm = ChronosRT_et_frm_create();

    TEST_ASSERT(ChronosRT_et_master_attach_esm(m, esm) == ChronosRT_SUCCESS &&
                ChronosRT_et_master_attach_sync(m, sync) == ChronosRT_SUCCESS &&
                ChronosRT_et_master_attach_topology(m, topo) == ChronosRT_SUCCESS &&
                ChronosRT_et_master_attach_mbx(m, mbx) == ChronosRT_SUCCESS &&
                ChronosRT_et_master_attach_frm(m, frm) == ChronosRT_SUCCESS,
                "all v0.1.0 modules should attach to the master");
    TEST_ASSERT(ChronosRT_et_master_attach_frm(NULL, frm) == ChronosRT_ERROR_INVALID,
                "attaching to a NULL master must fail");

    /* Sin interfaz abierta el envío se rechaza con STATE (no crashea). */
    TEST_ASSERT(ChronosRT_et_master_send(m, ChronosRT_ET_P2, ChronosRT_ET_TYPE_DATA, 1u,
                                         "x", 1u) == ChronosRT_ERROR_STATE,
                "send without an open interface should fail cleanly");
    TEST_ASSERT(ChronosRT_et_master_send_mailbox(m, 0u, "x", 1u, 0u) ==
                    ChronosRT_ERROR_STATE,
                "mailbox send without an open interface should fail cleanly");
    TEST_ASSERT(ChronosRT_et_master_last_wkc(m) == 0u,
                "no WKC observed before receiving anything");

    ChronosRT_et_master_stats_t stats;
    TEST_ASSERT(ChronosRT_et_master_stats(m, &stats) == ChronosRT_SUCCESS &&
                stats.duplicates_tx == 0u && stats.wkc_mismatches == 0u,
                "v0.1.0 counters should start at zero");

    ChronosRT_et_master_destroy(m);
    ChronosRT_et_esm_destroy(esm);
    ChronosRT_et_sync_destroy(sync);
    ChronosRT_et_topology_destroy(topo);
    ChronosRT_et_mbx_destroy(mbx);
    ChronosRT_et_frm_destroy(frm);
}

/* ------------------------------------------------------------------ */
/* Caminos rápidos: cero copias, lotes y tasa de enlace                */
/* ------------------------------------------------------------------ */

static void test_fast_paths(void) {
    printf("\n[Caminos rápidos v0.1.0]\n");

    /* Cabecera sola == primeros 26 B de la PDU, y el CRC valida al concatenar. */
    ChronosRT_et_header_t h;
    memset(&h, 0, sizeof(h));
    h.type = ChronosRT_ET_TYPE_DATA;
    h.priority = ChronosRT_ET_P2;
    h.state = ChronosRT_ET_STATE_OP;
    h.flow_id = 3u;
    h.seq_num = 9u;
    h.tx_timestamp = 12345u;
    h.flow_count = 1u;

    const uint8_t text[] = "payload de prueba para el CRC";
    uint16_t tlen = (uint16_t)sizeof(text);

    uint8_t full[256];
    uint32_t full_len = ChronosRT_et_pdu_build(full, sizeof(full), &h, text, tlen);
    TEST_ASSERT(full_len == ChronosRT_ET_HEADER_SIZE + tlen, "PDU completa construida");

    uint8_t hdr[ChronosRT_ET_HEADER_SIZE];
    TEST_ASSERT(ChronosRT_et_pdu_build_header(hdr, &h, text, tlen) ==
                    ChronosRT_ET_HEADER_SIZE,
                "la cabecera sola debe medir 26 B");
    TEST_ASSERT(memcmp(hdr, full, ChronosRT_ET_HEADER_SIZE) == 0,
                "la cabecera sola debe coincidir con la de la PDU completa");

    /* Validación: cabecera + carga útil en un búfer contiguo. */
    uint8_t joined[256];
    memcpy(joined, hdr, ChronosRT_ET_HEADER_SIZE);
    memcpy(joined + ChronosRT_ET_HEADER_SIZE, text, tlen);
    ChronosRT_et_header_t out;
    const uint8_t* pl = NULL;
    TEST_ASSERT(ChronosRT_et_pdu_parse(joined, ChronosRT_ET_HEADER_SIZE + tlen,
                                       &out, &pl) == ChronosRT_ET_HEADER_SIZE + tlen &&
                memcmp(pl, text, tlen) == 0,
                "cabecera cero-copia + carga debe validar el CRC");

    TEST_ASSERT(ChronosRT_et_pdu_build_header(NULL, &h, text, tlen) == 0u,
                "build_header(NULL) debe rechazarse");
    TEST_ASSERT(ChronosRT_et_pdu_build_header(hdr, NULL, text, tlen) == 0u,
                "build_header(h=NULL) debe rechazarse");

    /* Física del enlace: 14 + 26 + n + 4 FCS + 20 preámbulo/IFG = n + 64. */
    TEST_ASSERT(ChronosRT_et_wire_frame_bytes(0u) == 64u,
                "trama mínima = 64 B en el cable");
    TEST_ASSERT(ChronosRT_et_wire_frame_bytes(1464u) == 1528u,
                "1464 B de carga = 1528 B en el cable");
    TEST_ASSERT(ChronosRT_et_wire_pps(1000000000ull, 1464u) == 81806u,
                "1 Gbps y 1464 B ⇒ ~81 806 tramas/s (techo físico)");
    TEST_ASSERT(ChronosRT_et_wire_pps(1000000000ull, 20u) == 1488095u,
                "1 Gbps y 20 B ⇒ ~1 488 095 tramas/s");
    TEST_ASSERT(ChronosRT_et_wire_pps(2500000000ull, 1464u) == 204515u,
                "2.5 Gbps multiplica por 2.5 el techo de tramas");
    TEST_ASSERT(ChronosRT_et_wire_pps(0u, 64u) == 0u, "enlace 0 ⇒ 0 tramas/s");
    TEST_ASSERT(ChronosRT_et_payload_for_pps(1000000000ull, 100000u) > 0u,
                "100 k tramas/s caben con carga útil positiva");
    TEST_ASSERT(ChronosRT_et_payload_for_pps(1000000000ull, 1u) ==
                    ChronosRT_ET_MAX_PAYLOAD,
                "a 1 trama/s la carga se limita al MTU de EtherTux");
    TEST_ASSERT(ChronosRT_et_payload_for_pps(1000000000ull, 0u) == 0u,
                "0 tramas/s ⇒ 0");

    /* Velocidad de enlace: interfaz inexistente ⇒ 0, sin fallar. */
    TEST_ASSERT(ChronosRT_et_link_speed_bps(NULL) == 0u, "link_speed(NULL) == 0");
    TEST_ASSERT(ChronosRT_et_link_speed_bps("iface-inexistente0") == 0u,
                "interfaz inexistente ⇒ 0");

    /* Validación de los caminos de envío sin interfaz abierta. */
    ChronosRT_et_master_t* m = ChronosRT_et_master_create();
    TEST_ASSERT(m != NULL, "maestro creado");
    TEST_ASSERT(ChronosRT_et_master_send_zerocopy(m, ChronosRT_ET_P2,
                                                  ChronosRT_ET_TYPE_DATA, 1u,
                                                  text, tlen) == ChronosRT_ERROR_STATE,
                "zerocopy sin interfaz ⇒ STATE");
    ChronosRT_et_batch_item_t items[2] = {
        { ChronosRT_ET_P2, ChronosRT_ET_TYPE_DATA, 1u, text, tlen },
        { ChronosRT_ET_P3, ChronosRT_ET_TYPE_DATA, 2u, text, tlen },
    };
    TEST_ASSERT(ChronosRT_et_master_send_batch(m, items, 2u) == -1,
                "send_batch sin interfaz ⇒ -1");
    TEST_ASSERT(ChronosRT_et_master_send_batch(m, NULL, 2u) == -1,
                "send_batch(NULL) ⇒ -1");
    TEST_ASSERT(ChronosRT_et_master_send_batch(m, items, 0u) == -1,
                "send_batch(count=0) ⇒ -1");
    TEST_ASSERT(ChronosRT_et_master_open_auto(NULL, "lo") == ChronosRT_ERROR_INVALID,
                "open_auto(NULL) ⇒ INVALID");
    TEST_ASSERT(ChronosRT_et_master_open_auto(m, NULL) == ChronosRT_ERROR_INVALID,
                "open_auto(ifname=NULL) ⇒ INVALID");
    ChronosRT_et_master_destroy(m);
}

/*
 * Anillo de transmisión: se abre `lo`, se envía un lote y se comprueba la
 * contabilidad. Sin CAP_NET_RAW (ejecución sin privilegios) el test se omite
 * sin fallar, porque la disponibilidad del anillo depende del entorno.
 */
static void test_tx_ring(void) {
    printf("\nRunning TX ring tests...\n");

    ChronosRT_et_master_t* m = ChronosRT_et_master_create();
    TEST_ASSERT(m != NULL, "master creation should succeed");

    if (ChronosRT_et_master_open_auto(m, "lo") != ChronosRT_SUCCESS) {
        printf("  (AF_PACKET no disponible: test omitido)\n");
        TEST_ASSERT(true, "TX ring test should be skipped without CAP_NET_RAW");
        ChronosRT_et_master_destroy(m);
        return;
    }

    const bool ring = ChronosRT_et_master_tx_ring_active(m);
    printf("  anillo TX en lo: %s\n", ring ? "activo" : "no disponible");

    TEST_ASSERT(ChronosRT_et_master_set_tx_mode(m, ChronosRT_ET_TX_SYSCALL) == ChronosRT_SUCCESS,
                "switching to syscall mode should succeed");
    TEST_ASSERT(!ChronosRT_et_master_tx_ring_active(m), "the ring should be released");

    TEST_ASSERT(ChronosRT_et_master_set_tx_mode(m, (ChronosRT_et_tx_mode_t)99)
                    == ChronosRT_ERROR_INVALID,
                "an invalid TX mode should be rejected");

    ChronosRT_status_t st = ChronosRT_et_master_set_tx_mode(m, ChronosRT_ET_TX_RING);
    TEST_ASSERT(st == ChronosRT_SUCCESS || st == ChronosRT_ERROR_NOTSUP,
                "forcing the TX ring should succeed or report not supported");

    uint8_t payload[64];
    memset(payload, 0x5A, sizeof(payload));

    ChronosRT_et_batch_item_t items[8];
    for (unsigned i = 0; i < 8u; i++) {
        items[i].priority = ChronosRT_ET_P2;
        items[i].type     = ChronosRT_ET_TYPE_DATA;
        items[i].flow_id  = 1u;
        items[i].payload  = payload;
        items[i].len      = (uint16_t)sizeof(payload);
    }
    const int sent = ChronosRT_et_master_send_batch(m, items, 8u);
    TEST_ASSERT(sent == 8, "a batch of 8 frames should be accepted");

    ChronosRT_et_master_stats_t stats;
    TEST_ASSERT(ChronosRT_et_master_stats(m, &stats) == ChronosRT_SUCCESS && stats.frames_tx >= 8u,
                "frames_tx should account the whole batch");

    ChronosRT_et_master_destroy(m);
}

int main(void) {
    printf("ChronosRT EtherTux v0.1.0 Test Suite\n");
    printf("==================================\n");

    test_frame_v2();
    test_fast_paths();
    test_esm();
    test_sync();
    test_topology();
    test_mbx();
    test_frm();
    test_master_v2();
    test_tx_ring();

    printf("\n==================================\n");
    printf("Tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
