/*
 * test_aalid.c — ID local determinista de EtherTux (MAC LAA de 16 bits).
 *
 * Valida los dos niveles del esquema sin hardware de red:
 *   Nivel 1: MAC <-> ID, ID candidato (CRC-32C plegado), fallback multiplicativo
 *            y backoff determinista.
 *   Nivel 2: admisión con resolución de colisiones, duplicados, detección de MAC
 *            clonada, liberación, expiración por heartbeat y persistencia.
 *   Integración: activación en el maestro, filtro de destino y admisión.
 */
#include "chronosrt.h"
#include "chronosrt_ethertux.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

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
/* Nivel 1                                                             */
/* ------------------------------------------------------------------ */

static void test_mac_local(void) {
    printf("\n[MAC local (LAA)]\n");

    uint8_t mac[6];
    TEST_ASSERT(ChronosRT_et_aalid_mac(0x1234u, mac) == 6u,
                "la MAC local debe ocupar 6 bytes");
    TEST_ASSERT(mac[0] == 0x02u, "primer octeto LAA con U/L=1 e I/G=0 (0x02)");
    TEST_ASSERT(mac[1] == 0 && mac[2] == 0 && mac[3] == 0,
                "los 3 octetos centrales deben ser cero");
    TEST_ASSERT(mac[4] == 0x12u && mac[5] == 0x34u,
                "los 16 bits finales codifican el ID en big-endian");

    TEST_ASSERT(ChronosRT_et_aalid_id_from_mac(mac) == 0x1234u,
                "el ID se recupera de la MAC local");

    const uint8_t fabrica[6] = { 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E };
    TEST_ASSERT(ChronosRT_et_aalid_id_from_mac(fabrica) == ChronosRT_ET_LOCAL_ID_NONE,
                "una MAC de fábrica no es una MAC local");

    uint8_t borde[6] = { 0x02, 0, 0, 0, 0xFF, 0xFF };
    TEST_ASSERT(ChronosRT_et_aalid_id_from_mac(borde) == 0xFFFFu,
                "el ID 0xFFFF es un valor válido de la MAC local");
    TEST_ASSERT(ChronosRT_et_aalid_mac(0xFFFFu, borde) == 6u && borde[5] == 0xFFu,
                "el ID 0xFFFF se codifica correctamente");
}

static void test_candidate(void) {
    printf("\n[ID candidato (Nivel 1)]\n");

    const uint8_t a[6] = { 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E };
    const uint8_t b[6] = { 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5F };

    const uint16_t ca = ChronosRT_et_aalid_candidate(a);
    TEST_ASSERT(ca == ChronosRT_et_aalid_candidate(a),
                "el ID candidato es determinista para la misma MAC");
    TEST_ASSERT(ca != ChronosRT_et_aalid_candidate(b),
                "MACs distintas producen candidatos distintos");
    TEST_ASSERT(ChronosRT_et_aalid_candidate_mul(a) ==
                ChronosRT_et_aalid_candidate_mul(a),
                "el fallback multiplicativo también es determinista");

    /* Backoff determinista y acotado. */
    const uint32_t slots = 64u;
    const uint32_t s1 = ChronosRT_et_aalid_backoff_slots(a, slots);
    TEST_ASSERT(s1 == ChronosRT_et_aalid_backoff_slots(a, slots),
                "el backoff derivado de la MAC es reproducible");
    TEST_ASSERT(s1 < slots, "el backoff queda dentro de [0, slots)");
    TEST_ASSERT(ChronosRT_et_aalid_backoff_slots(b, slots) != s1 ||
                ChronosRT_et_aalid_candidate(a) != ChronosRT_et_aalid_candidate(b),
                "MACs con distinto candidato tienden a backoffs distintos");
    TEST_ASSERT(ChronosRT_et_aalid_backoff_slots(a, 100u) < 100u,
                "el backoff funciona también con un número no potencia de 2");
}

/* Busca dos MACs de fábrica que compartan ID candidato (colisión de 16 bits). */
static bool find_collision(uint8_t a[6], uint8_t b[6]) {
    static uint32_t first[65536];
    static bool     used[65536];
    memset(used, 0, sizeof(used));

    for (uint32_t i = 0; i < 65536u; i++) {
        uint8_t mac[6] = { 0x00u, 0x1Au, 0x2Bu,
                           (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i };
        const uint16_t c = ChronosRT_et_aalid_candidate(mac);
        if (used[c]) {
            const uint32_t j = first[c];
            memcpy(a, (uint8_t[]){ 0x00u, 0x1Au, 0x2Bu,
                                   (uint8_t)(j >> 16), (uint8_t)(j >> 8), (uint8_t)j }, 6u);
            memcpy(b, mac, 6u);
            return true;
        }
        used[c] = true;
        first[c] = i;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Nivel 2: admisión                                                   */
/* ------------------------------------------------------------------ */

static void test_admission(void) {
    printf("\n[Admisión (Nivel 2)]\n");

    ChronosRT_et_aalid_config_t cfg;
    ChronosRT_et_aalid_default_config(&cfg);
    ChronosRT_et_aalid_t* aal = ChronosRT_et_aalid_create(&cfg);
    TEST_ASSERT(aal != NULL, "el asignador debe crearse con la configuración por defecto");
    if (aal == NULL) {
        return;
    }

    const uint8_t m1[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint16_t id1 = 0u;
    ChronosRT_et_aalid_result_t res = ChronosRT_ET_AALID_INVALID;

    TEST_ASSERT(ChronosRT_et_aalid_admit(aal, m1, 0xA1u, 1000u, &id1, &res) ==
                ChronosRT_SUCCESS &&
                res == ChronosRT_ET_AALID_OK,
                "una MAC nueva recibe su ID candidato");
    TEST_ASSERT(id1 == ChronosRT_et_aalid_candidate(m1),
                "el ID final coincide con el candidato cuando está libre");

    uint16_t id1b = 0u;
    TEST_ASSERT(ChronosRT_et_aalid_admit(aal, m1, 0xA1u, 2000u, &id1b, &res) ==
                ChronosRT_SUCCESS &&
                res == ChronosRT_ET_AALID_DUPLICATE && id1b == id1,
                "la misma MAC y nonce devuelve el mismo ID (duplicado)");

    uint16_t id1c = 0u;
    TEST_ASSERT(ChronosRT_et_aalid_admit(aal, m1, 0xB2u, 3000u, &id1c, &res) ==
                ChronosRT_ERROR_STATE && res == ChronosRT_ET_AALID_CLONE,
                "misma MAC con otro nonce se rechaza como MAC clonada");

    /* Colisión de candidato: la segunda MAC debe recibir otro ID. */
    uint8_t ca[6];
    uint8_t cb[6];
    if (find_collision(ca, cb)) {
        ChronosRT_et_aalid_t* c2 = ChronosRT_et_aalid_create(&cfg);
        uint16_t ia = 0u;
        uint16_t ib = 0u;
        (void)ChronosRT_et_aalid_admit(c2, ca, 1u, 0u, &ia, &res);
        const ChronosRT_status_t stb = ChronosRT_et_aalid_admit(c2, cb, 2u, 0u, &ib, &res);
        TEST_ASSERT(stb == ChronosRT_SUCCESS && res == ChronosRT_ET_AALID_COLLISION,
                    "un candidato ocupado se resuelve por colisión");
        TEST_ASSERT(ia != ib, "las dos MACs reciben IDs distintos");
        TEST_ASSERT(ib == (uint16_t)(ia + 1u),
                    "el cursor secuencial asigna el ID libre siguiente");
        ChronosRT_et_aalid_destroy(c2);
    } else {
        TEST_ASSERT(false, "no se encontró una colisión de 16 bits (inesperado)");
    }

    /* Tabla llena. */
    ChronosRT_et_aalid_config_t small;
    ChronosRT_et_aalid_default_config(&small);
    small.id_max = 3u;   /* 4 IDs: 0..3 */
    ChronosRT_et_aalid_t* tiny = ChronosRT_et_aalid_create(&small);
    TEST_ASSERT(tiny != NULL, "el asignador admite un rango de IDs reducido");
    uint32_t ok = 0u;
    for (uint32_t i = 0; i < 5u; i++) {
        uint8_t mac[6] = { 0x00, 0xAA, 0xBB, 0xCC, 0xDD, (uint8_t)i };
        uint16_t id = 0u;
        if (ChronosRT_et_aalid_admit(tiny, mac, i + 1u, 0u, &id, &res) == ChronosRT_SUCCESS) {
            ok++;
        } else if (res == ChronosRT_ET_AALID_FULL && i == 4u) {
            TEST_ASSERT(true, "sin IDs libres la admisión devuelve FULL");
        }
    }
    TEST_ASSERT(ok == 4u, "se conceden exactamente los 4 IDs disponibles");
    ChronosRT_et_aalid_destroy(tiny);

    /* Liberación y reutilización. */
    TEST_ASSERT(ChronosRT_et_aalid_release(aal, id1) == ChronosRT_SUCCESS,
                "liberar un ID en uso debe funcionar");
    TEST_ASSERT(ChronosRT_et_aalid_release(aal, id1) == ChronosRT_ERROR_NOTFOUND,
                "liberar dos veces el mismo ID devuelve NOTFOUND");
    uint16_t tmp = 0u;
    TEST_ASSERT(ChronosRT_et_aalid_lookup(aal, m1, &tmp) == ChronosRT_ERROR_NOTFOUND,
                "tras liberar, la MAC ya no tiene ID");
    TEST_ASSERT(!ChronosRT_et_aalid_is_occupied(aal, id1),
                "el ID liberado vuelve a estar libre");

    ChronosRT_et_aalid_stats_t st;
    TEST_ASSERT(ChronosRT_et_aalid_stats(aal, &st) == ChronosRT_SUCCESS,
                "las estadísticas están disponibles");
    TEST_ASSERT(st.clone_mac == 1u, "se contabilizó la MAC clonada");
    TEST_ASSERT(st.admissions == 1u, "se contabilizó una única admisión real");
    TEST_ASSERT(st.duplicates == 1u, "se contabilizó el duplicado");
    TEST_ASSERT(st.released == 1u, "se contabilizó la liberación");

    ChronosRT_et_aalid_destroy(aal);
}

static void test_expire(void) {
    printf("\n[Expiración por heartbeat]\n");

    ChronosRT_et_aalid_config_t cfg;
    ChronosRT_et_aalid_default_config(&cfg);
    cfg.grace_ms = 1000u;

    ChronosRT_et_aalid_t* aal = ChronosRT_et_aalid_create(&cfg);
    TEST_ASSERT(aal != NULL, "asignador con tiempo de gracia configurable");

    const uint8_t m[6] = { 0x00, 0x02, 0x03, 0x04, 0x05, 0x06 };
    uint16_t id = 0u;
    (void)ChronosRT_et_aalid_admit(aal, m, 1u, 10000u, &id, NULL);

    TEST_ASSERT(ChronosRT_et_aalid_expire(aal, 10500u) == 0u,
                "dentro del tiempo de gracia el ID se conserva");
    TEST_ASSERT(ChronosRT_et_aalid_heartbeat(aal, id, 10600u) == ChronosRT_SUCCESS,
                "el heartbeat refresca la entrada");
    TEST_ASSERT(ChronosRT_et_aalid_expire(aal, 11000u) == 0u,
                "tras el heartbeat el ID sigue vivo");
    TEST_ASSERT(ChronosRT_et_aalid_expire(aal, 12000u) == 1u,
                "sin heartbeat, el ID expira al superar el tiempo de gracia");
    TEST_ASSERT(!ChronosRT_et_aalid_is_occupied(aal, id),
                "el ID expirado queda libre");

    ChronosRT_et_aalid_destroy(aal);
}

static void test_persistence(void) {
    printf("\n[Persistencia {MAC -> ID}]\n");

    char path[128];
    (void)snprintf(path, sizeof(path), "/tmp/chronosrt_aalid_%ld.bin", (long)getpid());

    ChronosRT_et_aalid_t* a = ChronosRT_et_aalid_create(NULL);
    ChronosRT_et_aalid_t* b = ChronosRT_et_aalid_create(NULL);
    TEST_ASSERT(a != NULL && b != NULL, "dos asignadores independientes");
    if (a == NULL || b == NULL) {
        ChronosRT_et_aalid_destroy(a);
        ChronosRT_et_aalid_destroy(b);
        return;
    }

    const uint8_t m1[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x11 };
    const uint8_t m2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x22 };
    uint16_t i1 = 0u;
    uint16_t i2 = 0u;
    (void)ChronosRT_et_aalid_admit(a, m1, 7u, 0u, &i1, NULL);
    (void)ChronosRT_et_aalid_admit(a, m2, 9u, 0u, &i2, NULL);

    TEST_ASSERT(ChronosRT_et_aalid_save(a, path) == ChronosRT_SUCCESS,
                "la identidad se persiste en disco");
    TEST_ASSERT(ChronosRT_et_aalid_load(b, path) == ChronosRT_SUCCESS,
                "la identidad se recupera tras un reinicio");

    uint16_t got = 0u;
    TEST_ASSERT(ChronosRT_et_aalid_lookup(b, m1, &got) == ChronosRT_SUCCESS && got == i1,
                "el ID persistido de la primera MAC se conserva");
    TEST_ASSERT(ChronosRT_et_aalid_lookup(b, m2, &got) == ChronosRT_SUCCESS && got == i2,
                "el ID persistido de la segunda MAC se conserva");

    /* Reclamar el ID tras el reinicio no consume uno nuevo. */
    uint16_t again = 0u;
    ChronosRT_et_aalid_result_t res = ChronosRT_ET_AALID_INVALID;
    TEST_ASSERT(ChronosRT_et_aalid_admit(b, m1, 7u, 5000u, &again, &res) ==
                ChronosRT_SUCCESS && res == ChronosRT_ET_AALID_DUPLICATE && again == i1,
                "una MAC conocida reclama su ID sin gastar otro");

    TEST_ASSERT(ChronosRT_et_aalid_load(b, "/tmp/no_existe_chronosrt_aalid.bin") ==
                ChronosRT_ERROR_IO,
                "cargar un fichero inexistente devuelve IO");

    (void)remove(path);
    ChronosRT_et_aalid_destroy(a);
    ChronosRT_et_aalid_destroy(b);
}

/* ------------------------------------------------------------------ */
/* Integración con el maestro                                          */
/* ------------------------------------------------------------------ */

static void test_master(void) {
    printf("\n[Integración en el maestro]\n");

    ChronosRT_et_master_t* m = ChronosRT_et_master_create();
    TEST_ASSERT(m != NULL, "el maestro se crea sin interfaz");
    if (m == NULL) {
        return;
    }

    TEST_ASSERT(ChronosRT_et_master_aalid(m) == NULL,
                "sin activación no hay estado de admisión");
    TEST_ASSERT(ChronosRT_et_master_enable_local_ids(m, NULL) == ChronosRT_SUCCESS,
                "activar el ID local sin configuración explícita");
    TEST_ASSERT(ChronosRT_et_master_aalid(m) != NULL,
                "el estado de admisión queda asociado al maestro");

    TEST_ASSERT(ChronosRT_et_master_local_id(m) == ChronosRT_ET_LOCAL_ID_NONE,
                "al arrancar no hay ID local");

    const uint8_t fabrica[6] = { 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E };
    uint16_t id = 0u;
    ChronosRT_et_aalid_result_t res = ChronosRT_ET_AALID_INVALID;
    TEST_ASSERT(ChronosRT_et_master_admit_node(m, fabrica, 42u, 100u, &id, &res) ==
                ChronosRT_SUCCESS && res == ChronosRT_ET_AALID_OK,
                "el maestro concede un ID a una MAC de fábrica");
    TEST_ASSERT(id == ChronosRT_et_aalid_candidate(fabrica),
                "el ID concedido es el candidato determinista");

    TEST_ASSERT(ChronosRT_et_master_set_local_id(m, 0x0102u) == ChronosRT_SUCCESS,
                "se puede fijar el ID local del maestro");
    TEST_ASSERT(ChronosRT_et_master_local_id(m) == 0x0102u,
                "el maestro conserva su ID local");

    uint8_t self[6];
    const uint8_t otro[6] = { 0x02, 0x00, 0x00, 0x00, 0x09, 0x99 };
    const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    (void)ChronosRT_et_aalid_mac(0x0102u, self);

    TEST_ASSERT(ChronosRT_et_master_accepts_destination(m, bcast),
                "la difusión siempre se acepta");
    TEST_ASSERT(ChronosRT_et_master_accepts_destination(m, self),
                "una trama dirigida a la MAC local se acepta");
    TEST_ASSERT(!ChronosRT_et_master_accepts_destination(m, otro),
                "una trama unicast de otro nodo se descarta");

    TEST_ASSERT(ChronosRT_et_master_expire_local_ids(m, 0u) == 0u,
                "sin tiempo transcurrido no expira ningún ID");

    /* Sin interfaz abierta no se puede transmitir. */
    TEST_ASSERT(ChronosRT_et_master_send_to(m, id, ChronosRT_ET_P2,
                                            ChronosRT_ET_TYPE_DATA, 1u, "x", 1u) ==
                ChronosRT_ERROR_STATE,
                "enviar por ID requiere la interfaz abierta");

    ChronosRT_et_master_destroy(m);
}

int main(void) {
    printf("ChronosRT ID local (EtherTux) Test Suite\n");
    printf("======================================\n");

    test_mac_local();
    test_candidate();
    test_admission();
    test_expire();
    test_persistence();
    test_master();

    printf("\n=== Resumen ===\n");
    printf("Tests ejecutados: %d\n", tests_run);
    printf("Tests superados:  %d\n", tests_passed);
    if (tests_passed != tests_run) {
        printf("FALLARON %d tests\n", tests_run - tests_passed);
        return 1;
    }
    printf("Todos los tests de ID local superados\n");
    return 0;
}
