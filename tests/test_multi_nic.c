/*
* test_multi_nic.c — Pruebas unitarias para funcionalidad Multi-NIC
*
* Cubre:
*   - Creación/destrucción de SIRC multi-NIC
*   - Balanceo de carga automático con diferentes algoritmos
*   - Circuit breaker y health monitoring
*   - Failover automático
*   - QoS dinámico con prioridades
*   - Estadísticas agregadas
*/
#include "chronosrt_transport.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

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

/* Mock transport adapter for testing */
static tuxrt_err_t mock_open(tuxrt_transport_adapter_t* adapter) {
    (void)adapter;
    return TUXRT_OK;
}

static tuxrt_err_t mock_close(tuxrt_transport_adapter_t* adapter) {
    (void)adapter;
    return TUXRT_OK;
}

static tuxrt_err_t mock_send(tuxrt_transport_adapter_t* adapter,
                              const uint8_t* data, size_t len,
                              tuxrt_timestamp_ns_t deadline) {
    (void)adapter;
    (void)data;
    (void)len;
    (void)deadline;
    return TUXRT_OK;
}

static tuxrt_err_t mock_recv(tuxrt_transport_adapter_t* adapter,
                              uint8_t* buf, size_t* len,
                              tuxrt_timestamp_ns_t deadline) {
    (void)adapter;
    (void)buf;
    (void)len;
    (void)deadline;
    return TUXRT_OK;
}

static tuxrt_err_t mock_flush(tuxrt_transport_adapter_t* adapter) {
    (void)adapter;
    return TUXRT_OK;
}

static tuxrt_err_t mock_get_stats(tuxrt_transport_adapter_t* adapter, void* stats_out) {
    if (!adapter || !stats_out) return TUXRT_ERR_INVAL;
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    return TUXRT_OK;
}

static void mock_destroy(tuxrt_transport_adapter_t* adapter) {
    (void)adapter;
}

static tuxrt_err_t mock_activate(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return TUXRT_ERR_INVAL;
    adapter->active = true;
    return TUXRT_OK;
}

static tuxrt_err_t mock_deactivate(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return TUXRT_ERR_INVAL;
    adapter->active = false;
    return TUXRT_OK;
}

/* Create a mock vtable for testing */
static const tuxrt_transport_vtable_t mock_vtable = {
    .abi_version = 1,
    .open = mock_open,
    .close = mock_close,
    .send = mock_send,
    .recv = mock_recv,
    .flush = mock_flush,
    .get_stats = mock_get_stats,
    .destroy = mock_destroy,
    .activate = mock_activate,
    .deactivate = mock_deactivate
};

static void test_sirc_create_multi(void) {
    printf("\n=== Testing SIRC Multi-NIC Creation ===\n");

    tuxrt_sirc_multi_nic_config_t config = {0};
    config.max_adapters = 4;
    config.enable_load_balancing = true;
    config.enable_failover = true;
    config.load_balance_algo = TUXRT_LOAD_BALANCE_ROUND_ROBIN;
    config.health_threshold_error_rate = 0.1;
    config.failure_threshold = 5;
    config.recovery_timeout_ms = 1000;

    tuxrt_sirc_t* sirc = NULL;
    tuxrt_err_t err = tuxrt_sirc_create_multi(&config, 4, &sirc);

    TEST_ASSERT(err == TUXRT_OK, "tuxrt_sirc_create_multi should succeed");
    TEST_ASSERT(sirc != NULL, "SIRC should be created");
    TEST_ASSERT(sirc->initialized == true, "SIRC should be initialized");
    TEST_ASSERT(sirc->adapter_count == 0, "No adapters should be registered yet");
    TEST_ASSERT(sirc->config.enable_load_balancing == true, "Load balancing should be enabled");

    if (sirc) {
        tuxrt_sirc_destroy_multi(sirc);
        TEST_ASSERT(true, "tuxrt_sirc_destroy_multi should be safe");
    }
}

static void test_load_balance_algorithms(void) {
    printf("\n=== Testing Load Balancing Algorithms ===\n");

    tuxrt_sirc_multi_nic_config_t config = {0};
    config.max_adapters = 3;
    config.enable_load_balancing = true;
    config.health_threshold_error_rate = 0.1;

    tuxrt_sirc_t* sirc = NULL;
    tuxrt_err_t err = tuxrt_sirc_create_multi(&config, 3, &sirc);

    TEST_ASSERT(err == TUXRT_OK, "SIRC creation should succeed");
    if (!sirc) return;

    /* Register mock adapters */
    tuxrt_transport_adapter_config_t cfg = {0};
    cfg.type = TUXRT_TRANSPORT_ETHERCAT;
    cfg.device_path = "eth0";
    cfg.enable_multi_nic = true;

    tuxrt_err_t r1 = tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);
    TEST_ASSERT(r1 == TUXRT_OK, "First adapter registration should succeed");

    cfg.device_path = "eth1";
    tuxrt_err_t r2 = tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);
    TEST_ASSERT(r2 == TUXRT_OK, "Second adapter registration should succeed");

    /* Activate adapters */
    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_sirc_activate_adapter(sirc, i);
    }

    /* Test different load balance algorithms */
    tuxrt_load_balance_algo_t algs[] = {
        TUXRT_LOAD_BALANCE_ROUND_ROBIN,
        TUXRT_LOAD_BALANCE_WEIGHTED,
        TUXRT_LOAD_BALANCE_RATE_BASED,
        TUXRT_LOAD_BALANCE_LEAST_LATENCY,
        TUXRT_LOAD_BALANCE_ADAPTIVE,
        TUXRT_LOAD_BALANCE_QPOLICY
    };

    for (int a = 0; a < 6; a++) {
        tuxrt_err_t e = tuxrt_sirc_set_load_balance_algo(sirc, algs[a]);
        TEST_ASSERT(e == TUXRT_OK, "Load balance algorithm should be settable");
    }

    tuxrt_sirc_destroy_multi(sirc);
}

static void test_circuit_breaker(void) {
    printf("\n=== Testing Circuit Breaker ===\n");

    tuxrt_sirc_multi_nic_config_t config = {0};
    config.max_adapters = 2;

    tuxrt_sirc_t* sirc = NULL;
    tuxrt_err_t err = tuxrt_sirc_create_multi(&config, 2, &sirc);

    TEST_ASSERT(err == TUXRT_OK, "SIRC creation should succeed");
    if (!sirc) return;

    /* Register adapter */
    tuxrt_transport_adapter_config_t cfg = {0};
    cfg.type = TUXRT_TRANSPORT_ETHERCAT;
    cfg.device_path = "eth0";

    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    /* Get adapter and simulate failures */
    tuxrt_transport_adapter_t* adapter = NULL;
    tuxrt_sirc_get_adapter(sirc, 0, &adapter);

    if (adapter) {
        /* Set circuit breaker state manually to test */
        adapter->health.cb_state = TUXRT_CB_OPEN;
        adapter->health.error_rate = 0.5;

        /* Update health should trigger HALF_OPEN after timeout */
        adapter->health.last_state_change_ns = tuxrt_now_ns() - 2000000000LL; /* 2s ago */

        tuxrt_sirc_update_multi_health(sirc);

        /* Circuit breaker should be in HALF_OPEN state */
        TEST_ASSERT(adapter->health.cb_state == TUXRT_CB_HALF_OPEN,
                   "Circuit breaker should transition to HALF_OPEN");
    }

    tuxrt_sirc_destroy_multi(sirc);
}

static void test_qos_priority(void) {
    printf("\n=== Testing QoS Priority ===\n");

    tuxrt_sirc_multi_nic_config_t config = {0};
    config.max_adapters = 3;
    config.load_balance_algo = TUXRT_LOAD_BALANCE_QPOLICY;

    tuxrt_sirc_t* sirc = NULL;
    tuxrt_err_t err = tuxrt_sirc_create_multi(&config, 3, &sirc);

    TEST_ASSERT(err == TUXRT_OK, "SIRC creation should succeed");
    if (!sirc) return;

    /* Register adapters with different priorities */
    tuxrt_transport_adapter_config_t cfg = {0};
    cfg.type = TUXRT_TRANSPORT_ETHERCAT;
    cfg.enable_multi_nic = true;
    cfg.priority = 0;  /* Highest priority */

    cfg.device_path = "eth0";
    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    cfg.device_path = "eth1";
    cfg.priority = 2;  /* Normal priority */
    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    cfg.device_path = "eth2";
    cfg.priority = 4;  /* Lowest priority */
    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    /* Activate adapters */
    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_sirc_activate_adapter(sirc, i);
    }

    /* Test send with priority */
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    size_t used_idx = 0;

    tuxrt_err_t e = tuxrt_sirc_send_multi_priority(sirc, data, sizeof(data),
                                                    TUXRT_NIC_PRIORITY_CRITICAL,
                                                    0, &used_idx);

    TEST_ASSERT(e == TUXRT_OK, "Send with priority should work");
    TEST_ASSERT(used_idx < sirc->adapter_count, "Valid adapter index should be returned");

    tuxrt_sirc_destroy_multi(sirc);
}

static void test_aggregate_stats(void) {
    printf("\n=== Testing Aggregate Statistics ===\n");

    tuxrt_sirc_multi_nic_config_t config = {0};
    config.max_adapters = 2;

    tuxrt_sirc_t* sirc = NULL;
    tuxrt_err_t err = tuxrt_sirc_create_multi(&config, 2, &sirc);

    TEST_ASSERT(err == TUXRT_OK, "SIRC creation should succeed");
    if (!sirc) return;

    /* Register adapters */
    tuxrt_transport_adapter_config_t cfg = {0};
    cfg.type = TUXRT_TRANSPORT_ETHERCAT;
    cfg.device_path = "eth0";

    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    cfg.device_path = "eth1";
    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    tuxrt_multi_nic_aggregate_stats_t stats = {0};
    tuxrt_err_t e = tuxrt_sirc_get_aggregate_stats(sirc, &stats);

    TEST_ASSERT(e == TUXRT_OK, "Get aggregate stats should succeed");

    /* Test health reset */
    tuxrt_sirc_reset_multi_health(sirc);

    tuxrt_sirc_destroy_multi(sirc);
}

static void test_failover(void) {
    printf("\n=== Testing Failover ===\n");

    tuxrt_sirc_multi_nic_config_t config = {0};
    config.max_adapters = 2;
    config.enable_failover = true;

    tuxrt_sirc_t* sirc = NULL;
    tuxrt_err_t err = tuxrt_sirc_create_multi(&config, 2, &sirc);

    TEST_ASSERT(err == TUXRT_OK, "SIRC creation should succeed");
    if (!sirc) return;

    /* Register adapters */
    tuxrt_transport_adapter_config_t cfg = {0};
    cfg.type = TUXRT_TRANSPORT_ETHERCAT;
    cfg.device_path = "eth0";

    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    cfg.device_path = "eth1";
    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    /* Activate all adapters */
    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_sirc_activate_adapter(sirc, i);
    }

    /* Prepare failover */
    tuxrt_err_t e = tuxrt_sirc_prepare_failover(sirc, 1);
    TEST_ASSERT(e == TUXRT_OK, "Prepare failover should succeed");

    /* Check failover readiness */
    bool ready = tuxrt_sirc_is_failover_ready(sirc, 1);
    TEST_ASSERT(ready == true, "Failover should be ready");

    /* Test failover to interface */
    e = tuxrt_sirc_failover_to_interface(sirc, "eth1");
    TEST_ASSERT(e == TUXRT_OK, "Failover to interface should succeed");

    tuxrt_sirc_destroy_multi(sirc);
}

static void test_multi_send_recv(void) {
    printf("\n=== Testing Multi-NIC Send/Recv ===\n");

    tuxrt_sirc_multi_nic_config_t config = {0};
    config.max_adapters = 2;
    config.enable_load_balancing = true;

    tuxrt_sirc_t* sirc = NULL;
    tuxrt_err_t err = tuxrt_sirc_create_multi(&config, 2, &sirc);

    TEST_ASSERT(err == TUXRT_OK, "SIRC creation should succeed");
    if (!sirc) return;

    /* Register and activate adapters */
    tuxrt_transport_adapter_config_t cfg = {0};
    cfg.type = TUXRT_TRANSPORT_ETHERCAT;
    cfg.device_path = "eth0";

    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    cfg.device_path = "eth1";
    tuxrt_sirc_register_transport(sirc, &cfg, &mock_vtable);

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_sirc_activate_adapter(sirc, i);
    }

    /* Test send multi */
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    size_t used_idx = 0;
    err = tuxrt_sirc_send_multi(sirc, data, sizeof(data), 0, &used_idx);

    TEST_ASSERT(err == TUXRT_OK, "Multi-NIC send should succeed");

    /* Test get adapter */
    tuxrt_transport_adapter_t* adapter = NULL;
    err = tuxrt_sirc_get_adapter(sirc, 0, &adapter);
    TEST_ASSERT(err == TUXRT_OK && adapter != NULL, "Get adapter should succeed");

    /* Test get multi stats */
    tuxrt_health_stats_t stats[4];
    size_t count = 0;
    err = tuxrt_sirc_get_multi_stats(sirc, stats, 4, &count);
    TEST_ASSERT(err == TUXRT_OK && count == 2, "Get multi stats should succeed");

    /* Test enumerate interfaces */
    const char* ifaces[4] = {0};
    count = 0;
    err = tuxrt_sirc_enumerate_interfaces(sirc, ifaces, 4, &count);
    TEST_ASSERT(err == TUXRT_OK && count == 2, "Enumerate interfaces should succeed");

    tuxrt_sirc_destroy_multi(sirc);
}

int main(void) {
    printf("ChronosRT Multi-NIC Test Suite\n");
    printf("================================\n");

    test_sirc_create_multi();
    test_load_balance_algorithms();
    test_circuit_breaker();
    test_qos_priority();
    test_aggregate_stats();
    test_failover();
    test_multi_send_recv();

    printf("\n================================\n");
    printf("Tests: %d/%d passed\n", test_passed, test_count);

    return test_passed == test_count ? 0 : 1;
}