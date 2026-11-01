/*
 * test_network.c — Validación del maestro EtherCAT de ChronosRT.
 *
 * Cubre lo verificable sin hardware EtherCAT:
 *   - serialización/deserialización de datagramas y contador de trabajo,
 *   - cabecera de trama EtherCAT,
 *   - enumeración de interfaces del sistema,
 *   - ciclo de vida del maestro y degradación elegante ante interfaces ausentes,
 *   - rechazo de operaciones sobre un maestro cerrado.
 *
 * ChronosRT solo dispone de maestro EtherCAT: no existe ningún otro transporte.
 */
#include "chronosrt_ethercat.h"
#include "chronosrt.h"

#include <stdio.h>
#include <string.h>
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

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_datagram_build_parse(void) {
    printf("\nRunning EtherCAT datagram build/parse tests...\n");

    const uint8_t payload[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint8_t buf[64];
    uint32_t total = ChronosRT_ec_datagram_build(buf, sizeof(buf), (uint8_t)ChronosRT_EC_CMD_APRD,
                                                 3u, 0x01300001u, payload, 8u, false);

    TEST_ASSERT(total == 10u + 8u + 2u, "datagram total size should be 20 bytes");
    TEST_ASSERT(buf[0] == (uint8_t)ChronosRT_EC_CMD_APRD, "command should be APRD (1)");
    TEST_ASSERT(buf[1] == 3u, "index should be 3");
    TEST_ASSERT(le32(buf + 2) == 0x01300001u, "address should be little-endian ADP|ADO");
    TEST_ASSERT(le16(buf + 6) == 8u, "data-length field should be 8 without the M bit");
    TEST_ASSERT(buf[8] == 0u && buf[9] == 0u, "IRQ field should be zero");
    TEST_ASSERT(memcmp(buf + 10, payload, 8) == 0, "payload should be copied verbatim");
    TEST_ASSERT(buf[18] == 0u && buf[19] == 0u, "working counter should start at zero");

    /* Bit M (más datagramas). */
    uint32_t total2 = ChronosRT_ec_datagram_build(buf, sizeof(buf), (uint8_t)ChronosRT_EC_CMD_APRD,
                                                  3u, 0x01300001u, payload, 8u, true);
    TEST_ASSERT(total2 == total, "M bit must not change the size");
    TEST_ASSERT((le16(buf + 6) & 0x8000u) != 0u, "M bit should be set when more=true");

    /* Round-trip y contador de trabajo. */
    ChronosRT_ec_datagram_t d;
    buf[18] = 0x01;   /* WC bajo */
    buf[19] = 0x02;   /* WC alto  -> 0x0201 */
    uint32_t used = ChronosRT_ec_datagram_parse(buf, total, &d);
    TEST_ASSERT(used == total, "parse should consume the full datagram");
    TEST_ASSERT(d.cmd == (uint8_t)ChronosRT_EC_CMD_APRD, "parsed command should match");
    TEST_ASSERT(d.idx == 3u, "parsed index should match");
    TEST_ASSERT(d.adp == 0x0001u && d.ado == 0x0130u, "parsed ADP/ADO should split the address");
    TEST_ASSERT(d.address == 0x01300001u, "parsed address should match");
    TEST_ASSERT(d.data_len == 8u, "parsed data length should match");
    TEST_ASSERT(d.working_counter == 0x0201u, "parsed working counter should be 0x0201");
    TEST_ASSERT(d.data == buf + 10, "parsed data pointer should reference the payload");

    /* Insuficiencia de búfer. */
    TEST_ASSERT(ChronosRT_ec_datagram_build(buf, 4u, (uint8_t)ChronosRT_EC_CMD_APRD, 0u, 0u, NULL, 8u, false) == 0u,
                "build should fail when the buffer is too small");
    TEST_ASSERT(ChronosRT_ec_datagram_parse(buf, 4u, &d) == 0u,
                "parse should fail on a truncated datagram");
}

static void test_multi_datagram_walk(void) {
    printf("\nRunning multi-datagram walk test...\n");

    uint8_t buf[64];
    uint32_t a = ChronosRT_ec_datagram_build(buf, sizeof(buf), (uint8_t)ChronosRT_EC_CMD_BRD, 0u, 0u, NULL, 2u, true);
    uint32_t b = ChronosRT_ec_datagram_build(buf + a, sizeof(buf) - a, (uint8_t)ChronosRT_EC_CMD_BRD, 1u, 4u, NULL, 2u, false);
    TEST_ASSERT(a == 14u && b == 14u, "each 2-byte BRD datagram should be 14 bytes");

    ChronosRT_ec_datagram_t d0, d1;
    uint32_t u0 = ChronosRT_ec_datagram_parse(buf, a + b, &d0);
    uint32_t u1 = ChronosRT_ec_datagram_parse(buf + u0, a + b - u0, &d1);
    TEST_ASSERT(u0 == a && d0.idx == 0u && d0.more == true, "first datagram: idx 0, M bit set");
    TEST_ASSERT(u1 == b && d1.idx == 1u && d1.more == false, "second datagram: idx 1, M bit clear");
    TEST_ASSERT(d0.address == 0u && d1.address == 4u, "addresses should match");
}

static void test_frame_header(void) {
    printf("\nRunning EtherCAT frame header test...\n");

    uint8_t h[2] = { 0, 0 };
    uint16_t v = ChronosRT_ec_frame_header(h, 20u);
    TEST_ASSERT((v & 0x07FFu) == 20u, "header length field should be 20");
    TEST_ASSERT(((v >> 12) & 0x0Fu) == 1u, "header type should be 1 (EtherCAT commands)");
    TEST_ASSERT(le16(h) == v, "header bytes should be little-endian");
    TEST_ASSERT(v == 0x1014u, "encoded header value should be 0x1014");
    TEST_ASSERT(ChronosRT_EC_ETHERTYPE == 0x88A4u, "EtherType should be 0x88A4");
}

static void test_interface_enumeration(void) {
    printf("\nRunning interface enumeration tests...\n");

    uint32_t n = ChronosRT_ec_interface_count();
    TEST_ASSERT(n <= ChronosRT_EC_MAX_INTERFACES, "interface count should respect the limit");
    TEST_ASSERT(ChronosRT_ec_interface(n) == NULL, "out-of-range interface should be NULL");
    TEST_ASSERT(ChronosRT_ec_interface_index("chronosrt_zzz0") == -1,
                "unknown interface should not be found");
    TEST_ASSERT(ChronosRT_ec_interface_index(NULL) == -1, "NULL interface name should be rejected");

    if (n > 0u) {
        const ChronosRT_ec_interface_t* it = ChronosRT_ec_interface(0u);
        TEST_ASSERT(it != NULL && it->name[0] != '\0', "first interface should have a name");
        TEST_ASSERT(ChronosRT_ec_interface_index(it->name) == 0,
                    "the first interface should be found by name");
    } else {
        TEST_ASSERT(true, "no interfaces available in this environment");
    }
}

static void test_master_lifecycle(void) {
    printf("\nRunning EtherCAT master lifecycle tests...\n");

    ChronosRT_ec_master_t* m = ChronosRT_ec_master_create();
    TEST_ASSERT(m != NULL, "master creation should succeed");
    if (!m) {
        return;
    }
    TEST_ASSERT(!ChronosRT_ec_master_is_open(m), "new master should be closed");
    TEST_ASSERT(ChronosRT_ec_master_slave_count(m) == 0u, "new master should have no slaves");
    TEST_ASSERT(ChronosRT_ec_master_working_counter(m) == 0u, "initial working counter should be 0");
    TEST_ASSERT(ChronosRT_ec_master_expected_wkc(m) == 0u, "initial expected WKC should be 0");
    TEST_ASSERT(ChronosRT_ec_master_frames_sent(m) == 0u, "initial frame counter should be 0");
    TEST_ASSERT(ChronosRT_ec_master_frames_lost(m) == 0u, "initial lost counter should be 0");
    TEST_ASSERT(ChronosRT_ec_master_slave(m, 0u) == NULL, "no slave should be reachable");
    TEST_ASSERT(ChronosRT_ec_master_iomap(m) == NULL, "no IOMap before configuration");
    TEST_ASSERT(ChronosRT_ec_master_iomap_size(m) == 0u, "empty IOMap before configuration");
    TEST_ASSERT(ChronosRT_ec_master_dc_time(m) == 0, "no DC time before configuration");

    /* Operaciones sobre un maestro cerrado deben degradar elegantemente. */
    TEST_ASSERT(ChronosRT_ec_master_scan(m) == -1, "scan on a closed master should fail");
    TEST_ASSERT(ChronosRT_ec_master_config_init(m) == -1, "config_init on a closed master should fail");
    TEST_ASSERT(ChronosRT_ec_master_config_map(m) == -1, "config_map on a closed master should fail");
    TEST_ASSERT(ChronosRT_ec_master_send_processdata(m) == -1, "process cycle on a closed master should fail");
    TEST_ASSERT(ChronosRT_ec_master_request_all_states(m, ChronosRT_EC_STATE_OP, 1000u) == -1,
                "request_all_states on a closed master should fail");
    TEST_ASSERT(ChronosRT_ec_master_config_dc(m) == ChronosRT_ERROR_INVALID,
                "DC configuration on a closed master should fail");

    TEST_ASSERT(ChronosRT_ec_master_open(NULL, "eth0") == ChronosRT_ERROR_INVALID,
                "open(NULL, ...) should fail");
    TEST_ASSERT(ChronosRT_ec_master_open(m, "") == ChronosRT_ERROR_INVALID,
                "open with an empty interface should fail");
    TEST_ASSERT(ChronosRT_ec_master_open(m, NULL) == ChronosRT_ERROR_INVALID,
                "open with a NULL interface should fail");

    ChronosRT_status_t st = ChronosRT_ec_master_open(m, "chronosrt_zzz0");
    TEST_ASSERT(st != ChronosRT_SUCCESS, "open on an absent interface should fail gracefully");
    TEST_ASSERT(!ChronosRT_ec_master_is_open(m), "master should stay closed after a failed open");

    ChronosRT_ec_master_close(m);      /* NULL-safe / idempotente */
    ChronosRT_ec_master_destroy(m);
    ChronosRT_ec_master_destroy(NULL); /* NULL-safe */
    TEST_ASSERT(true, "master close/destroy should be NULL-safe");
}

int main(void) {
    printf("ChronosRT EtherCAT Master Test Suite v%s\n", ChronosRT_VERSION);
    printf("============================================\n");

    test_datagram_build_parse();
    test_multi_datagram_walk();
    test_frame_header();
    test_interface_enumeration();
    test_master_lifecycle();

    printf("\n============================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count ? 0 : 1;
}
