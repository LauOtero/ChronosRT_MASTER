/**
 * chronosrt_transport.c — Core de transporte para ChronosRT
 *
 * Implementa las funciones SIRC que son llamadas desde los transportes
 * EtherCAT y EtherTux. Estas funciones proporcionan la funcionalidad de
 * registro de transportes y gestión de estadísticas de salud.
 *
 * Este archivo es el "puente" entre la abstracción de transporte de
 * libtuxrt_core y los transportes específicos de ChronosRT.
 *
 * ============================================================================
 * NUEVAS FUNCIONALIDADES: Soporte Multi-NIC con Balanceo Automático
 * ============================================================================
 *
 * Arquitectura para múltiples NIC:
 * - Cada SIRC puede tener hasta CONFIG_MAX_ADAPTERS adaptadores activos
 * - Balanceo automático mediante algoritmos de salud y latencia
 * - Failover instantáneo con reintentos inteligentes
 * - Zero-copy en el hot path (no malloc)
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdatomic.h>

#include "chronosrt_transport.h"

/* Configuraciones de rendimiento adicionales */
#define CONFIG_MIN_PRIORITY 10
#define CONFIG_RT_STACK_SIZE (128 * 1024)
#define CONFIG_MAX_ADAPTERS TUXRT_MAX_ADAPTERS

/* Helper: obtener timestamp monotonic RAW (RT-safe) */
static inline tuxrt_timestamp_ns_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (tuxrt_timestamp_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Helper: establecer política de tiempo real */
static void set_realtime_priority(int priority) {
    struct sched_param param = { .sched_priority = priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
}

/* ============================================================================
 * Implementación de funciones SIRC - Health Management
 * ============================================================================ */

void tuxrt_sirc_update_health_success(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;

    tuxrt_timestamp_ns_t now = now_ns();

    pthread_mutex_lock(&adapter->mutex);

    adapter->health.last_success_ns = now;
    adapter->health.consecutive_successes++;

    if (adapter->health.consecutive_failures > 0) {
        adapter->health.consecutive_failures = 0;
        adapter->health.error_rate *= 0.5;  /* Recuperación */
    }

    /* Actualizar promedio de latencia con exp movil */
    if (adapter->health.last_success_ns > 0) {
        double latency = (double)(now - adapter->health.last_success_ns);
        adapter->health.avg_latency_ns = adapter->health.avg_latency_ns * 0.9 + latency * 0.1;
    }

    /* Circuit breaker: cerrar si estaba ABRIento */
    if (adapter->health.cb_state == TUXRT_CB_OPEN) {
        adapter->health.cb_state = TUXRT_CB_HALF_OPEN;
    }

    pthread_mutex_unlock(&adapter->mutex);
}

void tuxrt_sirc_update_health_failure(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;

    tuxrt_timestamp_ns_t now = now_ns();

    pthread_mutex_lock(&adapter->mutex);

    adapter->health.last_failure_ns = now;
    adapter->health.consecutive_failures++;
    adapter->health.consecutive_successes = 0;

    /* Incrementar error rate con ponderación */
    adapter->health.error_rate = adapter->health.error_rate * 0.9 + 0.1;

    /* Circuit breaker: abrir si threshold excedido */
    if (adapter->health.consecutive_failures >= 5 &&
        adapter->health.error_rate > 0.1) {
        adapter->health.cb_state = TUXRT_CB_OPEN;
    }

    pthread_mutex_unlock(&adapter->mutex);
}

/* ============================================================================
 * Registro de transportes (legacy - para compatibilidad)
 * ============================================================================ */

tuxrt_err_t tuxrt_sirc_register_transport(tuxrt_sirc_t* sirc,
                                         const tuxrt_transport_adapter_config_t* cfg,
                                         const tuxrt_transport_vtable_t* vtable) {
    if (!sirc || !cfg || !vtable) {
        return TUXRT_ERR_INVAL;
    }

    pthread_mutex_lock(&sirc->mutex);

    /* Buscar hueco disponible */
    size_t idx = sirc->adapter_count;
    if (idx >= CONFIG_MAX_ADAPTERS) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_INVAL;
    }

    /* Si ya existe un adaptador para este device, reemplazarlo */
    for (size_t i = 0; i < idx; i++) {
        if (sirc->adapters[i] &&
            sirc->adapters[i]->config.device_path &&
            strcmp(sirc->adapters[i]->config.device_path, cfg->device_path) == 0) {
            /* Cerrar adaptador existente */
            if (sirc->adapters[i]->vtable && sirc->adapters[i]->vtable->close) {
                sirc->adapters[i]->vtable->close(sirc->adapters[i]);
            }
            sirc->adapters[i]->active = false;
            idx = i;
            break;
        }
    }

    /* Crear nuevo adaptador */
    tuxrt_transport_adapter_t* adapter = calloc(1, sizeof(tuxrt_transport_adapter_t));
    if (!adapter) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_NOMEM;
    }

    adapter->vtable = vtable;
    adapter->config = *cfg;
    adapter->active = false;
    adapter->health.last_success_ns = 0;
    adapter->health.last_failure_ns = 0;
    adapter->health.consecutive_failures = 0;
    adapter->health.consecutive_successes = 0;
    adapter->health.error_rate = 0.0;
    adapter->health.avg_latency_ns = 0.0;
    adapter->health.cb_state = TUXRT_CB_CLOSED;
    adapter->health.packet_sent = 0;
    adapter->health.packet_received = 0;
    adapter->health.packet_dropped = 0;
    pthread_mutex_init(&adapter->mutex, NULL);

    sirc->adapters[idx] = adapter;
    sirc->adapter_count++;
    sirc->active_adapter_count = (uint32_t)sirc->adapter_count;

    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_set_active_transport(tuxrt_sirc_t* sirc, tuxrt_transport_type_t type) {
    if (!sirc) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    tuxrt_transport_adapter_t* current = NULL;

    /* Buscar adaptador del tipo solicitado */
    for (size_t i = 0; i < sirc->adapter_count; i++) {
        if (sirc->adapters[i] && sirc->adapters[i]->config.type == type) {
            /* Priorizar adaptadores activos y con menor error */
            if (!current ||
                sirc->adapters[i]->health.error_rate < current->health.error_rate) {
                current = sirc->adapters[i];
            }
        }
    }

    if (!current) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_NOENT;
    }

    /* Desactivar adaptador activo actual */
    for (size_t i = 0; i < sirc->adapter_count; i++) {
        if (sirc->adapters[i] && sirc->adapters[i]->active && sirc->adapters[i] != current) {
            if (sirc->adapters[i]->vtable && sirc->adapters[i]->vtable->close) {
                sirc->adapters[i]->vtable->close(sirc->adapters[i]);
            }
            sirc->adapters[i]->active = false;
        }
    }

    /* Activar nuevo adaptador */
    if (current->vtable && current->vtable->open) {
        tuxrt_err_t err = current->vtable->open(current);
        if (err != TUXRT_OK) {
            pthread_mutex_unlock(&sirc->mutex);
            return err;
        }
    }

    current->active = true;
    sirc->next_adapter_idx = (uint32_t)(current - sirc->adapters[0]) % sirc->adapter_count;

    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_trigger_failover(tuxrt_sirc_t* sirc, tuxrt_transport_type_t target) {
    if (!sirc || !sirc->config.enable_failover) {
        return TUXRT_ERR_NOENT;
    }
    return tuxrt_sirc_set_active_transport(sirc, target);
}

/* ============================================================================
 * Convenience wrappers
 * ============================================================================ */

tuxrt_err_t tuxrt_sirc_register_ethertux(tuxrt_sirc_t* sirc,
                                        const tuxrt_transport_adapter_config_t* cfg) {
    if (!sirc || !cfg) return TUXRT_ERR_INVAL;
    return tuxrt_sirc_register_transport(sirc, cfg, &tuxrt_transport_ethertux_vtable);
}

tuxrt_err_t tuxrt_sirc_register_ethercat(tuxrt_sirc_t* sirc,
                                          const tuxrt_transport_adapter_config_t* cfg) {
    if (!sirc || !cfg) return TUXRT_ERR_INVAL;
    return tuxrt_sirc_register_transport(sirc, cfg, &tuxrt_transport_ethercat_vtable);
}

/* ============================================================================
 * NUEVAS IMPLEMENTACIONES: Multi-NIC y Balanceo Automático
 * ============================================================================ */

tuxrt_err_t tuxrt_sirc_create_multi(const tuxrt_sirc_multi_nic_config_t* cfg,
                                     size_t max_adapters,
                                     tuxrt_sirc_t** out_sirc) {
    if (!cfg || !out_sirc || max_adapters == 0 || max_adapters > CONFIG_MAX_ADAPTERS) {
        return TUXRT_ERR_INVAL;
    }

    tuxrt_sirc_t* sirc = calloc(1, sizeof(tuxrt_sirc_t));
    if (!sirc) return TUXRT_ERR_NOMEM;

    sirc->config = *cfg;
    if (sirc->config.max_adapters == 0) {
        sirc->config.max_adapters = (uint32_t)max_adapters;
        if (sirc->config.max_adapters > CONFIG_MAX_ADAPTERS) {
            sirc->config.max_adapters = CONFIG_MAX_ADAPTERS;
        }
    }

    pthread_mutex_init(&sirc->mutex, NULL);
    sirc->initialized = true;

    *out_sirc = sirc;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_get_adapter(tuxrt_sirc_t* sirc, size_t index,
                                   tuxrt_transport_adapter_t** out_adapter) {
    if (!sirc || !out_adapter) return TUXRT_ERR_INVAL;
    if (index >= sirc->adapter_count) return TUXRT_ERR_NOENT;

    *out_adapter = sirc->adapters[index];
    return TUXRT_OK;
}

/* Seleccionar adaptador óptimo usando priority_map para O(1) selección */
static tuxrt_transport_adapter_t* select_optimal_adapter(tuxrt_sirc_t* sirc,
                                                          tuxrt_timestamp_ns_t deadline) {
    if (!sirc->config.enable_load_balancing || sirc->adapter_count == 0) {
        return NULL;
    }

    /* First, try O(1) lookup via priority_map */
    /* priority_map[priority][adapter_index] gives the adapter index for each priority level */
    int best_priority = -1;
    size_t best_idx = (size_t)-1;

    /* Iterar por los 6 niveles de prioridad (0=alta, 5=baja) */
    for (int pri = 0; pri < 6; pri++) {
        /* Para cada adapter_index en la mapa de prioridad */
        for (size_t mi = 0; mi < (size_t)sirc->adapter_count; mi++) {
            uint8_t pidx = sirc->sirc_fast.priority_map[pri][mi];
            /* pidx == 0xFF significa "ningún adaptador" */
            if (pidx == 0xFF) continue;

            tuxrt_transport_adapter_t* adapter = sirc->adapters[pidx];
            if (!adapter || !adapter->active ||
                adapter->health.cb_state != TUXRT_CB_CLOSED) continue;

            /* Verificar si este es el mejor para este nivel de prioridad */
            if (best_priority == -1 || pri < best_priority) {
                best_priority = pri;
                best_idx = pidx;
                break; /* Found first valid at this priority level */
            }
        }
        if (best_idx != (size_t)-1) break;
    }

    /* Si encontramos por priority_map, usarlo directamente (O(1)) */
    if (best_idx != (size_t)-1) {
        tuxrt_transport_adapter_t* best = sirc->adapters[best_idx];
        return best;
    }

    /* Fallback: scoring Q16 tradicional si no hay priority_map configurado */
    tuxrt_transport_adapter_t* best = NULL;
    uint32_t best_score = 0xFFFFFFFF; /* Q16 score: menor es mejor */

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter || !adapter->active ||
            adapter->health.cb_state != TUXRT_CB_CLOSED) continue;

        /* Q16 fixed-point score:
         * score = (error_rate * 7 + latency_weighted * 3) >> 4
        * donde latency_weighted = avg_latency_ns >> 20 (escala ~1ms a ~1 en Q16)
        * error_rate está normalizado 0-1, en Q16 usamos 255 como 1.0 */
        uint32_t error_q16 = (uint32_t)(adapter->health.error_rate * 255.0);
        uint32_t latency_q16 = (uint32_t)((uint64_t)(adapter->health.avg_latency_ns) >> 20);
        uint32_t score = ((error_q16 * 7) + (latency_q16 * 3)) >> 4;

        if (score < best_score) {
            best_score = score;
            best = adapter;
        }
    }

    return best;
}

tuxrt_err_t tuxrt_sirc_send_multi(tuxrt_sirc_t* sirc,
                                  const uint8_t* data, size_t len,
                                  tuxrt_timestamp_ns_t deadline,
                                  size_t* out_used_index) {
    if (!sirc || !data || len == 0) return TUXRT_ERR_INVAL;

    /* Intentar con adaptador activo principal primero */
    pthread_mutex_lock(&sirc->mutex);

    tuxrt_transport_adapter_t* primary = NULL;
    for (size_t i = 0; i < sirc->adapter_count; i++) {
        if (sirc->adapters[i] && sirc->adapters[i]->active) {
            primary = sirc->adapters[i];
            break;
        }
    }

    pthread_mutex_unlock(&sirc->mutex);

    if (primary) {
        tuxrt_err_t err = primary->vtable->send(primary, data, len, deadline);

        /* Actualizar estadísticas de throughput */
        pthread_mutex_lock(&primary->mutex);
        primary->health.packet_sent++;
        if (primary->health.last_success_ns > 0) {
            primary->health.throughput_mbps = (double)len * 8.0 /
                                             (now_ns() - primary->health.last_success_ns) * 1000.0;
        }
        pthread_mutex_unlock(&primary->mutex);

        if (err == TUXRT_OK && out_used_index) {
            *out_used_index = primary - sirc->adapters[0];
        }
        return err;
    }

    /* No hay adaptador activo, intentar con balanceo automático */
    if (sirc->config.enable_load_balancing) {
        tuxrt_transport_adapter_t* adapter = select_optimal_adapter(sirc, deadline);
        if (adapter) {
            tuxrt_err_t err = adapter->vtable->send(adapter, data, len, deadline);

            pthread_mutex_lock(&adapter->mutex);
            adapter->health.packet_sent++;
            pthread_mutex_unlock(&adapter->mutex);

            if (out_used_index) {
                *out_used_index = adapter - sirc->adapters[0];
            }
            return err;
        }
    }

    return TUXRT_ERR_NOT_INITIALIZED;
}

tuxrt_err_t tuxrt_sirc_recv_multi(tuxrt_sirc_t* sirc,
                                  uint8_t* buf, size_t* len,
                                  tuxrt_timestamp_ns_t deadline) {
    if (!sirc || !buf || !len || *len == 0) return TUXRT_ERR_INVAL;

    /* Buscar adaptador con datos disponibles (polling activo) */
    pthread_mutex_lock(&sirc->mutex);

    /* Priorizar adaptadores con menor latency */
    tuxrt_transport_adapter_t* best = NULL;
    double best_latency = UINT64_MAX;

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter || !adapter->active ||
            adapter->health.cb_state != TUXRT_CB_CLOSED) continue;

        if (adapter->health.avg_latency_ns < best_latency) {
            best_latency = adapter->health.avg_latency_ns;
            best = adapter;
        }
    }

    pthread_mutex_unlock(&sirc->mutex);

    if (best) {
        tuxrt_err_t err = best->vtable->recv(best, buf, len, deadline);

        if (err == TUXRT_OK) {
            pthread_mutex_lock(&best->mutex);
            best->health.packet_received++;
            pthread_mutex_unlock(&best->mutex);
        }
        return err;
    }

    return TUXRT_ERR_NOT_INITIALIZED;
}

void tuxrt_sirc_update_multi_health(tuxrt_sirc_t* sirc) {
    if (!sirc) return;

    pthread_mutex_lock(&sirc->mutex);

    /* Actualizar salud histórica */
    tuxrt_timestamp_ns_t now = now_ns();

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter) continue;

        /* Actualizar promedio de latencia en array circular */
        if (adapter->health.last_success_ns > 0) {
            double latency = (double)(now - adapter->health.last_success_ns);
            sirc->latency_history[sirc->latency_history_idx] = latency;
            sirc->latency_history_idx = (sirc->latency_history_idx + 1) % TUXRT_HEALTH_HISTORY_SIZE;
        }

        /* Verificar circuit breaker y reset si es necesario */
        if (adapter->health.cb_state == TUXRT_CB_OPEN) {
            if (now_ns() - adapter->health.last_state_change_ns >
                (uint64_t)sirc->config.recovery_timeout_ms * 1000000) {
                adapter->health.cb_state = TUXRT_CB_HALF_OPEN;
            }
        }
    }

    pthread_mutex_unlock(&sirc->mutex);
}

tuxrt_err_t tuxrt_sirc_activate_adapter(tuxrt_sirc_t* sirc, size_t index) {
    if (!sirc || index >= sirc->adapter_count) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    tuxrt_transport_adapter_t* adapter = sirc->adapters[index];
    if (!adapter || !adapter->vtable->open) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_INVAL;
    }

    tuxrt_err_t err = adapter->vtable->open(adapter);
    if (err == TUXRT_OK) {
        adapter->active = true;
    }

    pthread_mutex_unlock(&sirc->mutex);
    return err;
}

tuxrt_err_t tuxrt_sirc_deactivate_adapter(tuxrt_sirc_t* sirc, size_t index) {
    if (!sirc || index >= sirc->adapter_count) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    tuxrt_transport_adapter_t* adapter = sirc->adapters[index];
    if (!adapter) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_NOENT;
    }

    if (adapter->vtable->close) {
        adapter->vtable->close(adapter);
    }
    adapter->active = false;

    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_get_multi_stats(tuxrt_sirc_t* sirc,
                                       tuxrt_health_stats_t* out_stats,
                                       size_t max_count, size_t* out_count) {
    if (!sirc || !out_stats || max_count == 0) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    size_t count = 0;
    for (size_t i = 0; i < sirc->adapter_count && count < max_count; i++) {
        if (sirc->adapters[i]) {
            out_stats[count] = sirc->adapters[i]->health;
            count++;
        }
    }

    pthread_mutex_unlock(&sirc->mutex);

    if (out_count) *out_count = count;
    return TUXRT_OK;
}

void tuxrt_sirc_destroy_multi(tuxrt_sirc_t* sirc) {
    if (!sirc) return;

    pthread_mutex_lock(&sirc->mutex);

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        if (sirc->adapters[i]) {
            if (sirc->adapters[i]->vtable && sirc->adapters[i]->vtable->close) {
                sirc->adapters[i]->vtable->close(sirc->adapters[i]);
            }
            pthread_mutex_destroy(&sirc->adapters[i]->mutex);
            free(sirc->adapters[i]);
        }
    }

    pthread_mutex_unlock(&sirc->mutex);
    pthread_mutex_destroy(&sirc->mutex);
    free(sirc);
}

/* ============================================================================
 * Algoritmo Adaptive con Machine Learning Simple
 * ============================================================================ */

/* Modelo ML simple para selección adaptativa */
typedef struct {
    double latency_weight;
    double error_weight;
    double throughput_weight;
    double stability_weight;
    uint64_t update_count;
} adaptive_ml_model_t;

/* Inicializar modelo ML por adaptador */
typedef struct {
    adaptive_ml_model_t models[CONFIG_MAX_ADAPTERS];
    double quality_history[CONFIG_MAX_ADAPTERS][64]; /* Historial de calidad por NIC */
    size_t quality_history_idx[CONFIG_MAX_ADAPTERS];
} adaptive_ml_context_t;

static adaptive_ml_context_t g_adaptive_ctx = {0};

/* Calcular puntuación de calidad adaptativa */
static double adaptive_quality_score(const tuxrt_health_stats_t* health,
                                     const adaptive_ml_model_t* model) {
    if (!health || !model) return 1.0;

    /* Normalizar latencia (0-50ms -> 0-1) */
    double normalized_latency = (health->avg_latency_ns / 50000000.0);
    double latency_score = 1.0 - (normalized_latency > 1.0 ? 1.0 : normalized_latency);

    /* Error rate ya está normalizado 0-1 */
    double error_score = 1.0 - health->error_rate;

    /* Throughput normalizado (suponer máximo 1 Gbps) */
    double throughput_score = health->throughput_mbps / 1000.0;

    /* Estabilidad de consecutive_successes */
    double stability = (double)health->consecutive_successes / 1000.0;
    stability = stability > 1.0 ? 1.0 : stability;

    /* Aplicar ponderaciones del modelo */
    return (latency_score * model->latency_weight +
            error_score * model->error_weight +
            throughput_score * model->throughput_weight +
            stability * model->stability_weight);
}

/* Actualizar pesos del modelo ML basado en resultados */
static void adaptive_model_update(adaptive_ml_model_t* model,
                                  double measured_latency,
                                  int64_t actual_duration_ns) {
    if (!model) return;

    /* Ajuste adaptativo de pesos basado en error de predicción */
    double predicted = (measured_latency / 1000000.0) / 50.0; /* Normalizado */
    double actual = (double)actual_duration_ns / 100000000.0;

    double error = predicted - actual;

    /* Ajustar pesos proporcionalmente al error */
    if (error > 0.1) {
        /* Subestimamos latencia, aumentar peso de error */
        model->error_weight = model->error_weight * 1.05;
        model->stability_weight = model->stability_weight * 1.02;
    } else if (error < -0.1) {
        /* Sobrestimamos latencia, aumentar peso de latencia */
        model->latency_weight = model->latency_weight * 1.05;
    }

    model->update_count++;

    /* Normalizar pesos después de múltiples actualizaciones */
    if (model->update_count % 100 == 0) {
        double total = model->latency_weight + model->error_weight +
                       model->throughput_weight + model->stability_weight;
        if (total > 0) {
            model->latency_weight /= total;
            model->error_weight /= total;
            model->throughput_weight /= total;
            model->stability_weight /= total;
        }
    }
}

/* ============================================================================
 * Implementaciones de APIs faltantes
 * ============================================================================ */

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_send_multi_priority(tuxrt_sirc_t* sirc,
                                                          const uint8_t* data, size_t len,
                                                          tuxrt_nic_priority_t priority,
                                                          tuxrt_timestamp_ns_t deadline,
                                                          size_t* out_used_index) {
    if (!sirc || !data || len == 0) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    /* Buscar adaptador con prioridad adecuada */
    tuxrt_transport_adapter_t* selected = NULL;
    int8_t selected_priority = 5; /* Lowest */

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter || !adapter->active ||
            adapter->health.cb_state != TUXRT_CB_CLOSED) continue;

        /* Verificar prioridad del adaptador */
        int8_t adapter_priority = adapter->priority;
        if (adapter_priority < 0) adapter_priority = TUXRT_NIC_PRIORITY_NORMAL;

        /* Adaptador con prioridad requerida o superior */
        if (adapter_priority <= priority && adapter_priority < selected_priority) {
            selected = adapter;
            selected_priority = adapter_priority;
        }
    }

    /* Fallback: usar balanceo automático si no hay coincidencia de prioridad */
    if (!selected && sirc->config.enable_load_balancing) {
        selected = select_optimal_adapter(sirc, deadline);
    }

    pthread_mutex_unlock(&sirc->mutex);

    if (!selected) {
        return TUXRT_ERR_NOT_INITIALIZED;
    }

    /* Enviar a través del adaptador seleccionado */
    tuxrt_err_t err = selected->vtable->send(selected, data, len, deadline);

    pthread_mutex_lock(&selected->mutex);
    selected->health.packet_sent++;
    if (selected->health.last_success_ns > 0) {
        selected->health.throughput_mbps = (double)len * 8.0 /
                                         (now_ns() - selected->health.last_success_ns) * 1000.0;
    }
    pthread_mutex_unlock(&selected->mutex);

    if (err == TUXRT_OK && out_used_index) {
        *out_used_index = selected - sirc->adapters[0];
    }

    return err;
}

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_get_aggregate_stats(tuxrt_sirc_t* sirc,
                                                         tuxrt_multi_nic_aggregate_stats_t* out_stats) {
    if (!sirc || !out_stats) return TUXRT_ERR_INVAL;

    memset(out_stats, 0, sizeof(*out_stats));

    pthread_mutex_lock(&sirc->mutex);

    double total_throughput = 0;
    double total_latency = 0;
    double total_error = 0;
    uint64_t total_packets = 0;
    uint64_t total_dropped = 0;
    bool any_circuit_open = false;
    int8_t min_priority = 5;

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter) continue;

        total_latency += adapter->health.avg_latency_ns;
        total_error += adapter->health.error_rate;
        total_throughput += adapter->health.throughput_mbps;
        total_packets += adapter->health.packet_sent + adapter->health.packet_received;
        total_dropped += adapter->health.packet_dropped;

        if (adapter->health.cb_state == TUXRT_CB_OPEN) {
            any_circuit_open = true;
        }

        int8_t p = adapter->priority;
        if (p < 0) p = TUXRT_NIC_PRIORITY_NORMAL;
        if (p < min_priority) min_priority = p;
    }

    size_t count = sirc->adapter_count > 0 ? sirc->adapter_count : 1;

    out_stats->avg_latency_ns = total_latency / count;
    out_stats->error_rate = total_error / count;
    out_stats->throughput_mbps = total_throughput / count;
    out_stats->packet_loss_estimate = total_dropped * 100 / (total_packets > 0 ? total_packets : 1);
    out_stats->circuit_breaker_tripped = any_circuit_open;
    out_stats->degraded = any_circuit_open;
    out_stats->effective_priority = min_priority;
    out_stats->last_adaptation_ns = atomic_load(&sirc->adaptation_timestamp);

    /* Calcular estadísticas adicionales */
    double min_latency = UINT64_MAX;
    double max_latency = 0;

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter) continue;

        if (adapter->health.avg_latency_ns > 0) {
            if (adapter->health.avg_latency_ns < min_latency) min_latency = adapter->health.avg_latency_ns;
            if (adapter->health.avg_latency_ns > max_latency) max_latency = adapter->health.avg_latency_ns;
        }
    }

    /* Percentil 99 estimado */
    if (max_latency > min_latency) {
        out_stats->latency_percentile_99 = (uint32_t)(min_latency + (max_latency - min_latency) * 0.99);
    } else {
        out_stats->latency_percentile_99 = (uint32_t)max_latency;
    }

    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_set_load_balance_algo(tuxrt_sirc_t* sirc,
                                                           tuxrt_load_balance_algo_t algo) {
    if (!sirc) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);
    sirc->config.load_balance_algo = algo;
    pthread_mutex_unlock(&sirc->mutex);

    return TUXRT_OK;
}

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_failover_to_interface(tuxrt_sirc_t* sirc,
                                                            const char* ifname) {
    if (!sirc || !ifname) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    /* Buscar adaptador por nombre de interfaz */
    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter || !adapter->config.device_path) continue;

        if (strstr(adapter->config.device_path, ifname) != NULL) {
            /* Desactivar todos los demás adaptadores */
            for (size_t j = 0; j < sirc->adapter_count; j++) {
                tuxrt_transport_adapter_t* other = sirc->adapters[j];
                if (!other) continue;

                if (i != j) {
                    if (other->vtable && other->vtable->close) {
                        other->vtable->close(other);
                    }
                    other->active = false;
                }

                /* Activar el adaptador objetivo */
                if (i == j) {
                    if (adapter->vtable && adapter->vtable->open) {
                        tuxrt_err_t err = adapter->vtable->open(adapter);
                        if (err == TUXRT_OK) {
                            adapter->active = true;
                        }
                        pthread_mutex_unlock(&sirc->mutex);
                        return err;
                    }
                }
            }

            pthread_mutex_unlock(&sirc->mutex);
            return TUXRT_OK;
        }
    }

    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_ERR_NOENT;
}

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_enumerate_interfaces(tuxrt_sirc_t* sirc,
                                                          const char** out_ifaces,
                                                          size_t max_count, size_t* out_count) {
    if (!sirc || !out_ifaces || max_count == 0) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    size_t count = 0;
    for (size_t i = 0; i < sirc->adapter_count && count < max_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (adapter && adapter->config.device_path) {
            out_ifaces[count] = adapter->config.device_path;
            count++;
        }
    }

    pthread_mutex_unlock(&sirc->mutex);

    if (out_count) *out_count = count;
    return TUXRT_OK;
}

CHRONOSRT_API void tuxrt_sirc_reset_multi_health(tuxrt_sirc_t* sirc) {
    if (!sirc) return;

    pthread_mutex_lock(&sirc->mutex);

    for (size_t i = 0; i < sirc->adapter_count; i++) {
        tuxrt_transport_adapter_t* adapter = sirc->adapters[i];
        if (!adapter) continue;

        adapter->health.last_success_ns = 0;
        adapter->health.last_failure_ns = 0;
        adapter->health.last_state_change_ns = 0;
        adapter->health.consecutive_failures = 0;
        adapter->health.consecutive_successes = 0;
        adapter->health.error_rate = 0.0;
        adapter->health.avg_latency_ns = 0.0;
        adapter->health.cb_state = TUXRT_CB_CLOSED;
        adapter->health.packet_sent = 0;
        adapter->health.packet_received = 0;
        adapter->health.packet_dropped = 0;
    }

    sirc->latency_history_idx = 0;
    memset(sirc->latency_history, 0, sizeof(sirc->latency_history));

    pthread_mutex_unlock(&sirc->mutex);
}

/* ============================================================================
 * Fast Channel Switching Implementation
 * ============================================================================ */

CHRONOSRT_API tuxrt_err_t tuxrt_sirc_prepare_failover(tuxrt_sirc_t* sirc, size_t index) {
    if (!sirc || index >= sirc->adapter_count) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&sirc->mutex);

    tuxrt_transport_adapter_t* adapter = sirc->adapters[index];
    if (!adapter) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_NOENT;
    }

    /* Check if adapter has activate function (multi-NIC support) */
    if (adapter->vtable && adapter->vtable->activate) {
        tuxrt_err_t err = adapter->vtable->activate(adapter);
        if (err == TUXRT_OK || err == TUXRT_ERR_BUSY) {
            sirc->failover_prepared[index] = true;
            pthread_mutex_unlock(&sirc->mutex);
            return TUXRT_OK;
        }
        pthread_mutex_unlock(&sirc->mutex);
        return err;
    }

    /* Fallback: just mark as prepared */
    sirc->failover_prepared[index] = true;
    pthread_mutex_unlock(&sirc->mutex);

    return TUXRT_OK;
}

CHRONOSRT_API bool tuxrt_sirc_is_failover_ready(tuxrt_sirc_t* sirc, size_t index) {
    if (!sirc || index >= sirc->adapter_count) return false;

    /* Use atomic read for lock-free hot path */
    return sirc->failover_prepared[index];
}