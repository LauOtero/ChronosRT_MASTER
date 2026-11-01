/*
 * et_sync.c — ET-Sync y Monitor de Ventana de Sincronización (§5).
 *
 * El maestro es el reloj de referencia. Cada nodo mide su offset respecto a él
 * y lo reporta (`SyncReport`, 0x0B, P1). A diferencia de EtherCAT DC, el retardo
 * del switch no es determinista, por lo que ET-Sync lo **estima y compensa**
 * con un modelo de retardo calibrado por nodo (medido en la fase `CONFIG`).
 *
 * El Monitor de Ventana clasifica cada nodo según la desviación corregida:
 *
 *   |offset| < warn_threshold        → OK
 *   |offset| < out_of_sync_threshold → WARN
 *   resto                            → OUT_OF_SYNC
 *
 * La deriva se estima como Δoffset/Δt en partes por billón (ppb), útil para
 * detectar envejecimiento del reloj por temperatura.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    bool     used;
    uint32_t link_delay_ns;     /* modelo de retardo calibrado del switch */
    int64_t  offset_ns;         /* offset corregido (T_maestro - T_local) */
    int64_t  prev_offset_ns;
    uint64_t prev_ns;
    int64_t  drift_ppb;
    uint32_t samples;
    ChronosRT_et_sync_status_t status;
} et_sync_node_t;

struct ChronosRT_et_sync {
    ChronosRT_et_sync_config_t cfg;
    et_sync_node_t             nodes[ChronosRT_ET_MAX_NODES];
};

static inline void put_le64(uint8_t* p, uint64_t v) {
    for (unsigned i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static inline uint64_t get_le64(const uint8_t* p) {
    uint64_t v = 0u;
    for (unsigned i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

ChronosRT_et_sync_t* ChronosRT_et_sync_create(void) {
    ChronosRT_et_sync_t* s = (ChronosRT_et_sync_t*)calloc(1u, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    ChronosRT_et_sync_default_config(&s->cfg);
    return s;
}

void ChronosRT_et_sync_destroy(ChronosRT_et_sync_t* sync) {
    free(sync);
}

void ChronosRT_et_sync_default_config(ChronosRT_et_sync_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->warn_threshold_ns = 1000u;          /* ±1 µs  */
    cfg->out_of_sync_threshold_ns = 2000u;   /* ±2 µs  */
    cfg->report_period_frames = 10u;         /* cada 10 superframes */
}

ChronosRT_status_t ChronosRT_et_sync_configure(ChronosRT_et_sync_t* sync,
                                               const ChronosRT_et_sync_config_t* cfg) {
    if (sync == NULL || cfg == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    if (cfg->warn_threshold_ns >= cfg->out_of_sync_threshold_ns ||
        cfg->report_period_frames == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    sync->cfg = *cfg;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_sync_set_link_delay(ChronosRT_et_sync_t* sync,
                                                    uint16_t node_id,
                                                    uint32_t delay_ns) {
    if (sync == NULL || node_id == 0u || node_id > ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_INVALID;
    }
    et_sync_node_t* n = &sync->nodes[node_id - 1u];
    n->used = true;
    n->link_delay_ns = delay_ns;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_sync_process_report(ChronosRT_et_sync_t* sync,
                                                    uint16_t node_id,
                                                    int64_t measured_offset_ns,
                                                    uint64_t now_ns,
                                                    ChronosRT_et_sync_status_t* out_status) {
    if (sync == NULL || node_id == 0u || node_id > ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_INVALID;
    }

    et_sync_node_t* n = &sync->nodes[node_id - 1u];
    n->used = true;

    /* Compensación del retardo del switch (modelo calibrado). */
    int64_t offset = measured_offset_ns - (int64_t)n->link_delay_ns;

    if (n->samples > 0u && now_ns > n->prev_ns) {
        int64_t d_offset = offset - n->prev_offset_ns;
        int64_t d_time = (int64_t)(now_ns - n->prev_ns);
        /* ppb = (Δoffset / Δt) * 1e9 */
        n->drift_ppb = (d_offset * 1000000000ll) / d_time;
    }
    n->prev_offset_ns = offset;
    n->prev_ns = now_ns;
    n->offset_ns = offset;
    if (n->samples < UINT32_MAX) {
        n->samples++;
    }

    uint64_t abs = (offset < 0) ? (uint64_t)(-offset) : (uint64_t)offset;
    if (abs < (uint64_t)sync->cfg.warn_threshold_ns) {
        n->status = ChronosRT_ET_SYNC_OK;
    } else if (abs < (uint64_t)sync->cfg.out_of_sync_threshold_ns) {
        n->status = ChronosRT_ET_SYNC_WARN;
    } else {
        n->status = ChronosRT_ET_SYNC_OUT_OF_SYNC;
    }

    if (out_status != NULL) {
        *out_status = n->status;
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_sync_status_of(const ChronosRT_et_sync_t* sync,
                                               uint16_t node_id,
                                               int64_t* offset_ns,
                                               ChronosRT_et_sync_status_t* out_status) {
    if (sync == NULL || node_id == 0u || node_id > ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_INVALID;
    }
    const et_sync_node_t* n = &sync->nodes[node_id - 1u];
    if (!n->used) {
        return ChronosRT_ERROR_NOTFOUND;
    }
    if (offset_ns != NULL) {
        *offset_ns = n->offset_ns;
    }
    if (out_status != NULL) {
        *out_status = n->status;
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_sync_drift_ppb(const ChronosRT_et_sync_t* sync,
                                               uint16_t node_id,
                                               int64_t* out_ppb) {
    if (sync == NULL || out_ppb == NULL || node_id == 0u ||
        node_id > ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_INVALID;
    }
    const et_sync_node_t* n = &sync->nodes[node_id - 1u];
    if (!n->used || n->samples < 2u) {
        return ChronosRT_ERROR_NOTFOUND;
    }
    *out_ppb = n->drift_ppb;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_sync_stats(const ChronosRT_et_sync_t* sync,
                                           ChronosRT_et_sync_stats_t* out) {
    if (sync == NULL || out == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < ChronosRT_ET_MAX_NODES; i++) {
        const et_sync_node_t* n = &sync->nodes[i];
        if (!n->used) {
            continue;
        }
        out->nodes++;
        switch (n->status) {
            case ChronosRT_ET_SYNC_OK:         out->ok++; break;
            case ChronosRT_ET_SYNC_WARN:       out->warn++; break;
            case ChronosRT_ET_SYNC_OUT_OF_SYNC: out->out_of_sync++; break;
            default: break;
        }
        uint64_t abs = (n->offset_ns < 0) ? (uint64_t)(-n->offset_ns)
                                          : (uint64_t)n->offset_ns;
        if (abs > out->max_abs_offset_ns) {
            out->max_abs_offset_ns = abs;
        }
    }
    return ChronosRT_SUCCESS;
}

uint32_t ChronosRT_et_sync_build_calibrated(uint64_t master_ns, uint8_t out[8]) {
    if (out == NULL) {
        return 0u;
    }
    put_le64(out, master_ns);
    return 8u;
}

ChronosRT_status_t ChronosRT_et_sync_parse_calibrated(const uint8_t* in, uint32_t len,
                                                      uint64_t* out_master_ns) {
    if (in == NULL || len < 8u || out_master_ns == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    *out_master_ns = get_le64(in);
    return ChronosRT_SUCCESS;
}

uint32_t ChronosRT_et_sync_build_report(int64_t offset_ns, uint8_t out[8]) {
    if (out == NULL) {
        return 0u;
    }
    put_le64(out, (uint64_t)offset_ns);
    return 8u;
}

ChronosRT_status_t ChronosRT_et_sync_parse_report(const uint8_t* in, uint32_t len,
                                                  int64_t* out_offset_ns) {
    if (in == NULL || len < 8u || out_offset_ns == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    *out_offset_ns = (int64_t)get_le64(in);
    return ChronosRT_SUCCESS;
}
