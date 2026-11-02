/*
 * chronosrt_transport.h — Abstracción de transporte para ChronosRT (C11)
 *
 * Este header define los tipos de transporte que ChronosRT exporta para
 * ser compatibles con la ABI de libtuxrt_core. Los tipos usan la convención
 * de nomenclatura `tuxrt_*` para evitar colisiones y proporcionar compatibilidad
 * directa con el SDK de TuxRT.
 *
 * El objetivo es que los transportes EtherCAT y EtherTux de ChronosRT puedan
 * usarse como backends para los adaptadores de transporte de libtuxrt_core,
 * proporcionando una conexión Ethernet ultra eficiente equivalente a USB/CAN/UART.
 */
#ifndef CHRONOSRT_TRANSPORT_H
#define CHRONOSRT_TRANSPORT_H

#include "chronosrt_abi.h"
#include "chronosrt.h"
#include "chronosrt_ethertux.h"
#include "chronosrt_ethercat.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Tipos de error (compatibles con tuxrt_err_t de libtuxrt_core)
 * ============================================================================ */
typedef enum {
    TUXRT_OK                         = 0,
    TUXRT_ERR_NOMEM                  = -1,
    TUXRT_ERR_INVAL                  = -2,
    TUXRT_ERR_NOENT                  = -3,
    TUXRT_ERR_EXIST                  = -4,
    TUXRT_ERR_BUSY                   = -5,
    TUXRT_ERR_AGAIN                  = -6,
    TUXRT_ERR_NOSYS                  = -7,
    TUXRT_ERR_PERM                   = -8,
    TUXRT_ERR_IO                     = -9,
    TUXRT_ERR_TIMEDOUT               = -10,
    TUXRT_ERR_PROTO                  = -11,
    TUXRT_ERR_OVERFLOW               = -12,
    TUXRT_ERR_NOT_INITIALIZED        = -1000,
    TUXRT_ERR_ALREADY_RUNNING        = -1001,
    TUXRT_ERR_CONFIG_INVALID         = -1002,
    TUXRT_ERR_HARDWARE_FAULT         = -1003,
    TUXRT_ERR_RT_VIOLATION           = -1004,
    TUXRT_ERR_SYNC_LOST              = -1005,
    TUXRT_ERR_PLUGIN_LOAD            = -1006,
    TUXRT_ERR_PLUGIN_API_MISMATCH    = -1007,
    TUXRT_ERR_BACKPRESSURE           = -1028,
    TUXRT_ERR_DROPPED                = -1029
} tuxrt_err_t;

/* Alias para compatibilidad */
#define ChronosRT_transport_err_t tuxrt_err_t

/* ============================================================================
 * Tipos de transporte (compatibles con libtuxrt_core)
 * ============================================================================ */

typedef enum {
    TUXRT_TRANSPORT_USB      = 0,
    TUXRT_TRANSPORT_UART     = 1,
    TUXRT_TRANSPORT_CAN      = 2,
    TUXRT_TRANSPORT_ETHERCAT = 3,
    TUXRT_TRANSPORT_SPI      = 4,
    TUXRT_TRANSPORT_I2C      = 5,
    TUXRT_TRANSPORT_ETHERTUX = 6
} tuxrt_transport_type_t;

/* Tipo de timestamp en nanosegundos */
typedef int64_t tuxrt_timestamp_ns_t;

/* ============================================================================
 * Algoritmos de balanceo de carga
 * ============================================================================ */
typedef enum {
    TUXRT_LOAD_BALANCE_ROUND_ROBIN = 0,      /* Rotación cíclica */
    TUXRT_LOAD_BALANCE_WEIGHTED = 1,         /* Basado en salud/error rate */
    TUXRT_LOAD_BALANCE_RATE_BASED = 2,       /* Basado en bandwidth por NIC */
    TUXRT_LOAD_BALANCE_ADAPTIVE = 3,         /* Combina round-robin con health check */
    TUXRT_LOAD_BALANCE_LEAST_LATENCY = 4,    /* Selecciona menor latency observada */
    TUXRT_LOAD_BALANCE_QPOLICY = 5,          /* Quality-based: prioriza por QoS global */
} tuxrt_load_balance_algo_t;

/* Tipos de prioridad para balanceo QOS */
typedef enum {
    TUXRT_NIC_PRIORITY_CRITICAL = 0,  /* P0: emergency, ACK/NACK crítico */
    TUXRT_NIC_PRIORITY_HIGH = 1,      /* P1: sync, heartbeat */
    TUXRT_NIC_PRIORITY_NORMAL = 2,    /* P2: setpoints de control */
    TUXRT_NIC_PRIORITY_LOW = 3,       /* P3: telemetría */
    TUXRT_NIC_PRIORITY_BACKGROUND = 4 /* P4-P5: configuración, logs, bulk */
} tuxrt_nic_priority_t;

/* ============================================================================
 * Health stats (estadísticas de salud del transporte)
 * ============================================================================ */

typedef struct {
    uint64_t last_success_ns;
    uint64_t last_failure_ns;
    uint64_t last_latency_ns;
    uint64_t last_state_change_ns;  /* Para circuit breaker timeout */
    uint32_t consecutive_failures;
    uint32_t consecutive_successes;
    double   error_rate;
    double   avg_latency_ns;
    double   throughput_mbps;
    uint32_t cb_state;  /* 0=CLOSED, 1=OPEN, 2=HALF_OPEN */
    uint32_t packet_sent;
    uint32_t packet_received;
    uint32_t packet_dropped;
    uint32_t seq_counter;  /* Para detección de pérdidas */
} tuxrt_health_stats_t;

/* ============================================================================
 * Configuración del adaptador de transporte
 * ============================================================================ */

typedef struct {
    tuxrt_transport_type_t type;
    const char* device_path;     /* Ruta del dispositivo (ej: enp2s0 para Ethernet) */
    uint32_t baudrate;           /* Para UART */
    uint32_t bitrate;            /* Para CAN/EtherCAT/EtherTux */
    bool flow_control;
    bool enable_failover;
    tuxrt_transport_type_t fallback_type;
    const char* fallback_device;
    bool enable_multi_nic;       /* Habilitar múltiples NIC activas */
    double load_balance_threshold; /* Umbral de error para failover (0.0-1.0) */
    uint32_t priority;           /* Prioridad del adaptador (0=lowest) */
} tuxrt_transport_adapter_config_t;

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

typedef struct tuxrt_transport_adapter_t tuxrt_transport_adapter_t;
typedef struct tuxrt_sirc_t tuxrt_sirc_t;

/* ============================================================================
 * VTable del adaptador de transporte (interfaz abstracta - debe ir antes del struct)
 * ============================================================================ */

typedef struct tuxrt_transport_adapter_t tuxrt_transport_adapter_t;

typedef struct {
    uint32_t abi_version;
    tuxrt_err_t (*open)(tuxrt_transport_adapter_t* adapter);
    tuxrt_err_t (*close)(tuxrt_transport_adapter_t* adapter);
    tuxrt_err_t (*send)(tuxrt_transport_adapter_t* adapter,
                        const uint8_t* data, size_t len,
                        tuxrt_timestamp_ns_t deadline);
    tuxrt_err_t (*recv)(tuxrt_transport_adapter_t* adapter,
                        uint8_t* buf, size_t* len,
                        tuxrt_timestamp_ns_t deadline);
    tuxrt_err_t (*flush)(tuxrt_transport_adapter_t* adapter);
    tuxrt_err_t (*get_stats)(tuxrt_transport_adapter_t* adapter, void* stats_out);
    void (*destroy)(tuxrt_transport_adapter_t* adapter);
    /* Extensiones para multi-NIC */
    tuxrt_err_t (*activate)(tuxrt_transport_adapter_t* adapter);
    tuxrt_err_t (*deactivate)(tuxrt_transport_adapter_t* adapter);
} tuxrt_transport_vtable_t;

/* ============================================================================
 * Estructura del adaptador de transporte (implementación concreta)
 * ============================================================================ */

struct tuxrt_transport_adapter_t {
    const tuxrt_transport_vtable_t* vtable;
    void* impl;                              /* Implementación específica del protocolo */
    tuxrt_transport_adapter_config_t config;
    bool active;
    tuxrt_health_stats_t health;
    pthread_mutex_t mutex;
    uint32_t sequence;                       /* Secuencia para detección de duplicados */
    int8_t priority;                       /* Prioridad del adaptador (0=highest) */
};

/* ============================================================================
 * Multi-NIC SIRC Configuration
 * ============================================================================ */

typedef struct {
    uint32_t max_adapters;             /* Máximo adaptadores simultáneos */
    tuxrt_load_balance_algo_t load_balance_algo;
    bool enable_load_balancing;        /* Habilitar balanceo automático */
    bool enable_failover;              /* Habilitar failover automático */
    uint64_t health_check_interval_ns; /* Intervalo health check */
    double health_threshold_error_rate; /* Umbral error rate (0.0-1.0) */
    uint32_t failure_threshold;        /* Fallos consecutivos para mark DOWN */
    uint32_t recovery_timeout_ms;      /* Tiempo a esperar antes de retry */
    uint32_t priority;                 /* Prioridad global (0=highest) */
} tuxrt_sirc_multi_nic_config_t;

/* ============================================================================
 * Estructura completa del SIRC (Multi-NIC Support)
 * ============================================================================ */

/* Forward declarations for circular references */
struct tuxrt_transport_adapter_t;
struct tuxrt_sirc_t;

/* Constant for max adapters */
#define TUXRT_MAX_ADAPTERS 16
#define TUXRT_HEALTH_HISTORY_SIZE 1024

/* Circuit breaker states */
#define TUXRT_CB_CLOSED     0
#define TUXRT_CB_OPEN       1
#define TUXRT_CB_HALF_OPEN  2

/* Sistema de estadísticas agregadas */
typedef struct {
    double avg_latency_ns;
    double stddev_latency_ns;
    double error_rate;
    double throughput_mbps;
    uint64_t packet_loss_estimate;
    bool circuit_breaker_tripped;
    bool degraded;
    int8_t effective_priority;
    uint32_t latency_percentile_99;
    uint64_t last_adaptation_ns;
} tuxrt_multi_nic_aggregate_stats_internal_t;

/* Definición completa de la estructura SIRC (abi-compatible) */
struct tuxrt_sirc_t {
    /* Array de adaptadores (multi-NIC) */
    tuxrt_transport_adapter_t* adapters[TUXRT_MAX_ADAPTERS];
    uint32_t adapter_count;

    /* Sistema de balanceo de carga */
    tuxrt_sirc_multi_nic_config_t config;
    uint32_t next_adapter_idx;

    /* Array de salud histórica */
    double latency_history[TUXRT_HEALTH_HISTORY_SIZE];
    size_t latency_history_idx;

    /* Atomos para hot path */
    _Atomic uint32_t active_adapter_count;

    /* Sincronización */
    pthread_mutex_t mutex;

    /* Estado global */
    bool initialized;

    /* Extensiones para optimización multi-NIC */
    _Atomic uint8_t selected_adapter_cache;
    _Atomic uint64_t adaptation_timestamp;
    int8_t effective_priority_cache[TUXRT_MAX_ADAPTERS];
    bool failover_prepared[TUXRT_MAX_ADAPTERS];
    uint64_t circuit_breaker_count;
    uint64_t failover_count;
};

/* ============================================================================
 * Constantes EtherTux (compatibles con tuxrt_sdk.h)
 * ============================================================================ */

#define TUXRT_ET_P0        0
#define TUXRT_ET_P1        1
#define TUXRT_ET_P2        2
#define TUXRT_ET_P3        3
#define TUXRT_ET_P4        4
#define TUXRT_ET_P5        5

#define TUXRT_ET_TYPE_DATA            0x01
#define TUXRT_ET_TYPE_FEC             0x02
#define TUXRT_ET_TYPE_CONTROL         0x03
#define TUXRT_ET_TYPE_SYNC            0x04
#define TUXRT_ET_TYPE_LAPM            0x05
#define TUXRT_ET_TYPE_DISCOVERY       0x06

#define TUXRT_ET_MAX_PAYLOAD 1478u

/* ============================================================================
 * Constantes EtherCAT
 * ============================================================================ */

#define TUXRT_EC_ETHERTYPE 0x88A4

/* ============================================================================
 * Tipos EtherTux (alias de los tipos de ChronosRT con prefijo TUXRT)
 * ============================================================================ */

typedef ChronosRT_et_type_t tuxrt_et_type_t;
typedef ChronosRT_et_priority_t tuxrt_et_priority_t;
typedef ChronosRT_et_state_t tuxrt_et_state_t;
typedef ChronosRT_et_header_t tuxrt_et_header_t;
typedef ChronosRT_et_qos_config_t tuxrt_et_qos_config_t;
typedef ChronosRT_et_bucket_cfg_t tuxrt_et_bucket_cfg_t;
typedef ChronosRT_et_qos_stats_t tuxrt_et_qos_stats_t;
typedef ChronosRT_et_sched_t tuxrt_et_sched_t;
typedef ChronosRT_et_slot_t tuxrt_et_slot_t;
typedef ChronosRT_et_fec_mode_t tuxrt_et_fec_mode_t;
typedef ChronosRT_et_master_t tuxrt_et_master_t;
typedef ChronosRT_et_node_t tuxrt_et_node_t;
typedef ChronosRT_et_tx_mode_t tuxrt_et_tx_mode_t;
typedef ChronosRT_et_aalid_config_t tuxrt_et_aalid_config_t;

/* ============================================================================
 * Helper functions (alias de funciones de ChronosRT)
 * ============================================================================ */

static inline tuxrt_timestamp_ns_t tuxrt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (tuxrt_timestamp_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Notas: tuxrt_sirc_update_health_success/failure son implementados en transport.c */
CHRONOSRT_API void tuxrt_sirc_update_health_success(tuxrt_transport_adapter_t* adapter);
CHRONOSRT_API void tuxrt_sirc_update_health_failure(tuxrt_transport_adapter_t* adapter);

/* ============================================================================
 * Registro de transportes (legacy - para compatibilidad)
 * ============================================================================ */

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_register_transport(tuxrt_sirc_t* sirc,
                                                          const tuxrt_transport_adapter_config_t* cfg,
                                                          const tuxrt_transport_vtable_t* vtable);

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_set_active_transport(tuxrt_sirc_t* sirc, tuxrt_transport_type_t type);

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_trigger_failover(tuxrt_sirc_t* sirc, tuxrt_transport_type_t target);

/* VTables exportados por los transportes */
extern const tuxrt_transport_vtable_t tuxrt_transport_ethertux_vtable;
#ifndef BUILDING_ETHERCAT_TRANSPORT
extern const tuxrt_transport_vtable_t tuxrt_transport_ethercat_vtable;
#endif

/* Convenience wrappers */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_register_ethertux(tuxrt_sirc_t* sirc,
                                                        const tuxrt_transport_adapter_config_t* cfg);

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_register_ethercat(tuxrt_sirc_t* sirc,
                                                        const tuxrt_transport_adapter_config_t* cfg);

/* ============================================================================
 * NUEVAS APIs: Soporte Multi-NIC y Balanceo Automático
 * ============================================================================ */

/* Crear SIRC con configuración multi-NIC */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_create_multi(const tuxrt_sirc_multi_nic_config_t* cfg,
                                                     size_t max_adapters,
                                                     tuxrt_sirc_t** out_sirc);

/* Obtener adaptador por índice (para multi-NIC) */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_get_adapter(tuxrt_sirc_t* sirc, size_t index,
                                                   tuxrt_transport_adapter_t** out_adapter);

/* Enviar a través del adaptador más óptimo (balanceo automático) */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_send_multi(tuxrt_sirc_t* sirc,
                                                  const uint8_t* data, size_t len,
                                                  tuxrt_timestamp_ns_t deadline,
                                                  size_t* out_used_index);

/* Recibir del adaptador activo con menor latencia */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_recv_multi(tuxrt_sirc_t* sirc,
                                                  uint8_t* buf, size_t* len,
                                                  tuxrt_timestamp_ns_t deadline);

/* Actualizar health de todos los adaptadores */
CHRONOSRT_API void tuxrt_sirc_update_multi_health(tuxrt_sirc_t* sirc);

/* Activar/desactivar adaptador específico */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_activate_adapter(tuxrt_sirc_t* sirc, size_t index);
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_deactivate_adapter(tuxrt_sirc_t* sirc, size_t index);

/* Obtener estadísticas de todos los adaptadores */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_get_multi_stats(tuxrt_sirc_t* sirc,
                                                       tuxrt_health_stats_t* out_stats,
                                                       size_t max_count, size_t* out_count);

/* Destruir SIRC multi-NIC */
CHRONOSRT_API void tuxrt_sirc_destroy_multi(tuxrt_sirc_t* sirc);

/* ============================================================================
 * APIs Avanzadas: Balanceo Adaptive con ML simple y QoS dinámico
 * ============================================================================ */

/* Estadísticas agregadas de multi-NIC para toma de decisiones */
typedef struct {
    double avg_latency_ns;
    double stddev_latency_ns;
    double error_rate;
    double throughput_mbps;
    uint64_t packet_loss_estimate;     /* Estimación de pérdida extrapolada */
    bool circuit_breaker_tripped;
    bool degraded;                     /* true si QoS degrada prioridad */
    int8_t effective_priority;       /* Prioridad efectiva tras LAPM */
    uint32_t latency_percentile_99;  /* Percentil 99 de latency observada */
    uint64_t last_adaptation_ns;       /* Cuándo se actualizó el modelo */
} tuxrt_multi_nic_aggregate_stats_t;

/* Enviar con prioridad explícita (para QoS global) */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_send_multi_priority(tuxrt_sirc_t* sirc,
                                                          const uint8_t* data, size_t len,
                                                          tuxrt_nic_priority_t priority,
                                                          tuxrt_timestamp_ns_t deadline,
                                                          size_t* out_used_index);

/* Obtener estadísticas agregadas para decisiones de balanceo */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_get_aggregate_stats(tuxrt_sirc_t* sirc,
                                                        tuxrt_multi_nic_aggregate_stats_t* out_stats);

/* Reconfigurar algoritmo de balanceo en tiempo de ejecución */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_set_load_balance_algo(tuxrt_sirc_t* sirc,
                                                           tuxrt_load_balance_algo_t algo);

/* Forzar failover a un adaptador específico por nombre de interfaz */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_failover_to_interface(tuxrt_sirc_t* sirc,
                                                              const char* ifname);

/* Obtener todas las interfaces disponibles */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_enumerate_interfaces(tuxrt_sirc_t* sirc,
                                                           const char** out_ifaces,
                                                           size_t max_count, size_t* out_count);

/* Resetear contadores de salud de todos los adaptadores */
CHRONOSRT_API void tuxrt_sirc_reset_multi_health(tuxrt_sirc_t* sirc);

/* ============================================================================
 * Fast Channel Switching -Cambio rápido entre canales
 * ============================================================================ */

/* Pre-activar un adaptador para failover ultra-rápido (warm standby) */
CHRONOSRT_API tuxrt_err_t tuxrt_sirc_prepare_failover(tuxrt_sirc_t* sirc, size_t index);

/* Verificar si failover está listo */
CHRONOSRT_API bool tuxrt_sirc_is_failover_ready(tuxrt_sirc_t* sirc, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_TRANSPORT_H */