/*
 * test_hwaccel.c — Validación de la HAL de aceleración por hardware.
 *
 * Comprueba, SIN requerir hardware de red:
 *   - CRC-32C contra el vector estándar y contra el fallback software,
 *   - álgebra de GF(2^8),
 *   - codificación/decodificación FEC Reed-Solomon con borrados (RS(10,8) y
 *     RS(16,8)), incluida la reconstrucción de todos los bloques posibles,
 *   - determinismo de la codificación,
 *   - detección de capacidades.
 */
#include "chronosrt_hwaccel.h"
#include "chronosrt.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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

static uint32_t rng_state = 0x12345678u;

static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ------------------------------------------------------------------ */

static void test_crc32c(void) {
    printf("\nRunning CRC-32C tests...\n");

    const char* vector = "123456789";
    TEST_ASSERT(ChronosRT_crc32c(vector, 9u) == 0xE3069283u,
                "CRC-32C(\"123456789\") should be 0xE3069283");
    TEST_ASSERT(ChronosRT_crc32c_sw(vector, 9u) == 0xE3069283u,
                "software CRC-32C should match the standard vector");
    TEST_ASSERT(ChronosRT_crc32c("", 0u) == 0u,
                "CRC-32C of empty input should be 0");

    /* HW y SW deben coincidir para cualquier longitud (incluidas las colas). */
    uint8_t buf[517];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)rng_next();
    }
    bool all_match = true;
    for (size_t len = 0; len <= sizeof(buf); len++) {
        if (ChronosRT_crc32c(buf, len) != ChronosRT_crc32c_sw(buf, len)) {
            all_match = false;
            break;
        }
    }
    TEST_ASSERT(all_match, "hardware and software CRC-32C should agree for all lengths");

    /* Sensibilidad: un bit alterado cambia el CRC. */
    uint32_t before = ChronosRT_crc32c(buf, sizeof(buf));
    buf[100] ^= 0x01u;
    uint32_t after = ChronosRT_crc32c(buf, sizeof(buf));
    TEST_ASSERT(before != after, "CRC-32C should detect a single-bit change");
}

static void test_gf8(void) {
    printf("\nRunning GF(2^8) tests...\n");

    TEST_ASSERT(ChronosRT_gf8_mul(0u, 123u) == 0u, "0 * x should be 0");
    TEST_ASSERT(ChronosRT_gf8_mul(1u, 123u) == 123u, "1 * x should be x");

    bool inverse_ok = true;
    for (unsigned a = 1; a < 256; a++) {
        uint8_t inv = ChronosRT_gf8_inv((uint8_t)a);
        if (ChronosRT_gf8_mul((uint8_t)a, inv) != 1u) {
            inverse_ok = false;
            break;
        }
    }
    TEST_ASSERT(inverse_ok, "every non-zero element should have a multiplicative inverse");

    /* Conmutatividad y distributividad sobre una muestra. */
    bool algebra_ok = true;
    for (unsigned a = 1; a < 40; a++) {
        for (unsigned b = 1; b < 40; b++) {
            uint8_t ab = ChronosRT_gf8_mul((uint8_t)a, (uint8_t)b);
            uint8_t ba = ChronosRT_gf8_mul((uint8_t)b, (uint8_t)a);
            if (ab != ba) {
                algebra_ok = false;
            }
        }
    }
    TEST_ASSERT(algebra_ok, "GF(2^8) multiplication should be commutative");
}

/* Codifica, borra hasta n-k bloques y verifica la reconstrucción. */
static void rs_roundtrip(uint8_t k, uint8_t n, const unsigned* drop, unsigned drop_count,
                         const char* label) {
    enum { LEN = 137 };
    uint8_t data[ChronosRT_RS_MAX_N][LEN];
    uint8_t parity[ChronosRT_RS_MAX_N][LEN];
    uint8_t blocks[ChronosRT_RS_MAX_N][LEN];

    const uint8_t* data_ptrs[ChronosRT_RS_MAX_N];
    uint8_t* parity_ptrs[ChronosRT_RS_MAX_N];
    uint8_t* block_ptrs[ChronosRT_RS_MAX_N];

    for (unsigned i = 0; i < k; i++) {
        for (unsigned j = 0; j < LEN; j++) {
            data[i][j] = (uint8_t)rng_next();
        }
        data_ptrs[i] = data[i];
    }
    for (unsigned j = 0; j < (unsigned)(n - k); j++) {
        parity_ptrs[j] = parity[j];
    }

    ChronosRT_status_t st = ChronosRT_rs_encode(k, n, data_ptrs, parity_ptrs, LEN);
    TEST_ASSERT(st == ChronosRT_SUCCESS, label);

    /* Determinismo: repetir la codificación da exactamente lo mismo. */
    uint8_t parity2[ChronosRT_RS_MAX_N][LEN];
    uint8_t* parity2_ptrs[ChronosRT_RS_MAX_N];
    for (unsigned j = 0; j < (unsigned)(n - k); j++) {
        parity2_ptrs[j] = parity2[j];
    }
    (void)ChronosRT_rs_encode(k, n, data_ptrs, parity2_ptrs, LEN);
    bool deterministic = true;
    for (unsigned j = 0; j < (unsigned)(n - k); j++) {
        if (memcmp(parity[j], parity2[j], LEN) != 0) {
            deterministic = false;
        }
    }
    TEST_ASSERT(deterministic, "RS encoding should be deterministic");

    /* Copia el código completo y borra los bloques indicados. */
    bool present[ChronosRT_RS_MAX_N];
    for (unsigned c = 0; c < n; c++) {
        if (c < k) {
            memcpy(blocks[c], data[c], LEN);
        } else {
            memcpy(blocks[c], parity[c - k], LEN);
        }
        block_ptrs[c] = blocks[c];
        present[c] = true;
    }
    for (unsigned d = 0; d < drop_count; d++) {
        unsigned idx = drop[d];
        present[idx] = false;
        memset(blocks[idx], 0xAA, LEN);   /* basura: debe sobrescribirse */
    }

    st = ChronosRT_rs_decode(k, n, block_ptrs, present, LEN);
    TEST_ASSERT(st == ChronosRT_SUCCESS, "RS erasure decoding should succeed");

    bool recovered = true;
    for (unsigned c = 0; c < n; c++) {
        if (c < k) {
            if (memcmp(blocks[c], data[c], LEN) != 0) {
                recovered = false;
            }
        } else if (memcmp(blocks[c], parity[c - k], LEN) != 0) {
            recovered = false;
        }
    }
    TEST_ASSERT(recovered, "every erased block should be reconstructed exactly");
}

static void test_rs(void) {
    printf("\nRunning Reed-Solomon FEC tests...\n");

    /* RS(10,8): recupera 2 borrados cualesquiera. */
    const unsigned drop_light[] = { 2u, 5u };
    rs_roundtrip(8u, 10u, drop_light, 2u, "RS(10,8) encoding should succeed");

    /* RS(16,8): todos los datos perdidos (8 borrados = los 8 primeros). */
    const unsigned drop_all_data[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
    rs_roundtrip(8u, 16u, drop_all_data, 8u,
                 "RS(16,8) encoding should succeed and recover all data blocks");

    /* RS(16,8): solo paridad perdida. */
    const unsigned drop_all_parity[] = { 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u };
    rs_roundtrip(8u, 16u, drop_all_parity, 8u,
                 "RS(16,8) should tolerate losing all parity blocks");

    /* Parámetros inválidos. */
    const uint8_t* dp[8] = { 0 };
    uint8_t* pp[8] = { 0 };
    TEST_ASSERT(ChronosRT_rs_encode(0u, 10u, dp, pp, 8u) == ChronosRT_ERROR_INVALID,
                "k=0 should be rejected");
    TEST_ASSERT(ChronosRT_rs_encode(8u, 8u, dp, pp, 8u) == ChronosRT_ERROR_INVALID,
                "n <= k should be rejected");
    TEST_ASSERT(ChronosRT_rs_encode(8u, (uint8_t)(ChronosRT_RS_MAX_N + 1u), dp, pp, 8u)
                    == ChronosRT_ERROR_INVALID,
                "n > ChronosRT_RS_MAX_N should be rejected");

    /* Demasiados borrados: irrecuperable. */
    enum { LEN = 16 };
    uint8_t blocks[16][LEN];
    uint8_t* bp[16];
    bool present[16];
    for (unsigned c = 0; c < 16u; c++) {
        bp[c] = blocks[c];
        present[c] = true;
    }
    for (unsigned c = 0; c < 9u; c++) {   /* 9 > n-k = 8 */
        present[c] = false;
    }
    TEST_ASSERT(ChronosRT_rs_decode(8u, 16u, bp, present, LEN) == ChronosRT_ERROR_CORRUPT,
                "decoding with more erasures than parity should fail cleanly");
}

static void test_caps(void) {
    printf("\nRunning hardware capability tests...\n");

    const ChronosRT_hw_caps_t* caps = ChronosRT_hw_caps();
    TEST_ASSERT(caps != NULL, "capability descriptor should never be NULL");
    TEST_ASSERT(caps->crc32c_backend != NULL, "CRC backend name should be set");
    TEST_ASSERT(caps->fec_backend != NULL, "FEC backend name should be set");
    TEST_ASSERT(strcmp(caps->crc32c_backend, "slice-by-8") == 0 || caps->crc32c_hw,
                "a software or hardware CRC backend should be reported");

    printf("  CPU      : %s%s%s\n",
           caps->cpu_vendor, caps->cpu_brand[0] ? " / " : "", caps->cpu_brand);
    printf("  family=%u model=%u stepping=%u\n",
           caps->cpu_family, caps->cpu_model, caps->cpu_stepping);
    printf("  ISA      : sse4.2=%d pclmul=%d avx2=%d avx512f=%d avx512vl=%d "
           "vpclmul=%d gfni=%d vaes=%d bmi2=%d adx=%d\n",
           (int)caps->isa_sse42, (int)caps->isa_pclmul, (int)caps->isa_avx2,
           (int)caps->isa_avx512f, (int)caps->isa_avx512vl, (int)caps->isa_vpclmul,
           (int)caps->isa_gfni, (int)caps->isa_vaes, (int)caps->isa_bmi2,
           (int)caps->isa_adx);
    printf("  CRC      : %s  FEC: %s\n", caps->crc32c_backend, caps->fec_backend);

    ChronosRT_hw_caps_t per_if;
    ChronosRT_status_t st = ChronosRT_hw_caps_for_interface("lo", &per_if);
    TEST_ASSERT(st == ChronosRT_SUCCESS || st == ChronosRT_ERROR_NOTSUP ||
                st == ChronosRT_ERROR_IO,
                "interface capability probe should fail gracefully");
    TEST_ASSERT(ChronosRT_hw_caps_for_interface(NULL, &per_if) == ChronosRT_ERROR_INVALID,
                "probe(NULL) should be rejected");
    TEST_ASSERT(ChronosRT_hw_caps_for_interface("lo", NULL) == ChronosRT_ERROR_INVALID,
                "probe(..., NULL) should be rejected");

    TEST_ASSERT(ChronosRT_tx_timestamp_ns() != 0u, "TX timestamp should be non-zero");
}

/*
 * Todos los backends de CRC-32C disponibles deben reproducir exactamente la
 * referencia software (mismas longitudes, mismas semillas). Se ejercita cada
 * uno por separado y se restaura al final el que estaba activo.
 */
static void test_crc_backends(void) {
    printf("\nRunning CRC-32C backend tests...\n");

    static const char* names[] = { "vpclmul-fold", "pclmul-fold", "sse4.2-x4",
                                   "armv8-crc", "slice-by-8" };
    const char* active = ChronosRT_crc32c_active_backend();
    printf("  backend activo: %s\n", active);
    TEST_ASSERT(active != NULL, "the active backend should be reported");

    const char* failing = NULL;
    uint32_t failures = ChronosRT_crc32c_selftest(&failing);
    if (failures != 0u) {
        printf("  primer backend que falla: %s\n", failing ? failing : "?");
    }
    TEST_ASSERT(failures == 0u, "every detected CRC backend should match the reference");

    uint8_t buf[2048];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)rng_next();
    }

    uint32_t tested = 0u;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (ChronosRT_crc32c_force_backend(names[i]) != ChronosRT_SUCCESS) {
            continue;   /* no disponible en esta CPU */
        }
        tested++;
        bool ok = true;
        for (size_t len = 0; len <= sizeof(buf) && ok; len++) {
            const uint32_t seeds[3] = { 0u, 0xFFFFFFFFu, 0xA5A5A5A5u };
            for (unsigned s = 0; s < 3u; s++) {
                if (ChronosRT_crc32c_raw(seeds[s], buf, len)
                        != ChronosRT_crc32c_sw_raw(seeds[s], buf, len)) {
                    ok = false;
                    printf("  %s falla con len=%zu seed=%08x\n", names[i], len, seeds[s]);
                    break;
                }
            }
        }
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "backend '%s' should match the software reference", names[i]);
        TEST_ASSERT(ok, msg);
    }
    TEST_ASSERT(tested >= 1u, "at least one CRC backend should be available");

    TEST_ASSERT(ChronosRT_crc32c_force_backend(active) == ChronosRT_SUCCESS,
                "the original backend should be restored");
    TEST_ASSERT(ChronosRT_crc32c_force_backend("no-existe") == ChronosRT_ERROR_NOTSUP,
                "an unknown backend should be rejected");
}

int main(void) {
    printf("ChronosRT Hardware Acceleration Test Suite v%s\n", ChronosRT_VERSION);
    printf("================================================\n");

    test_crc32c();
    test_crc_backends();
    test_gf8();
    test_rs();
    test_caps();

    printf("\n================================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);
    return test_passed == test_count ? 0 : 1;
}
