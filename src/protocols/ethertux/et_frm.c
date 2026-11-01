/*
 * et_frm.c — FRM: redundancia lógica de flujo (§8.2).
 *
 * La topología en estrella impide un anillo físico, así que la redundancia se
 * hace a nivel de flujo:
 *
 *   - Los flujos P0/P1 se envían **duplicados**: una copia por el camino activo
 *     y otra por el alternativo (dos `FlowID`, o dos colas de hardware si la
 *     NIC lo permite). La copia lleva `Flags[4]` (FRM) activo.
 *   - El receptor elimina duplicados por (nodo, `SeqNum`), de modo que la
 *     aplicación ve un solo evento aunque lleguen las dos copias.
 *   - Si un nodo deja de responder por el camino activo, el maestro conmuta su
 *     flujo al alternativo en el siguiente superframe.
 *
 * El estado por nodo es estático (64 nodos × ventana de 16 secuencias), sin
 * asignación dinámica en el camino caliente.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

#define ET_FRM_DEDUP_WINDOW 16u

typedef struct {
    uint32_t ring[ET_FRM_DEDUP_WINDOW];
    uint8_t  count;
    uint8_t  next;
    uint8_t  path;          /* 0 = camino primario, 1 = alternativo */
} et_frm_node_t;

struct ChronosRT_et_frm {
    et_frm_node_t             nodes[ChronosRT_ET_MAX_NODES];
    ChronosRT_et_frm_stats_t  stats;
};

ChronosRT_et_frm_t* ChronosRT_et_frm_create(void) {
    return (ChronosRT_et_frm_t*)calloc(1u, sizeof(ChronosRT_et_frm_t));
}

void ChronosRT_et_frm_destroy(ChronosRT_et_frm_t* frm) {
    free(frm);
}

void ChronosRT_et_frm_reset(ChronosRT_et_frm_t* frm) {
    if (frm == NULL) {
        return;
    }
    memset(frm->nodes, 0, sizeof(frm->nodes));
    memset(&frm->stats, 0, sizeof(frm->stats));
}

bool ChronosRT_et_frm_should_duplicate(const ChronosRT_et_frm_t* frm, uint8_t priority) {
    if (frm == NULL) {
        return false;
    }
    return priority == (uint8_t)ChronosRT_ET_P0 || priority == (uint8_t)ChronosRT_ET_P1;
}

uint8_t ChronosRT_et_frm_alternate_flow(uint8_t flow_id) {
    uint8_t alt = (uint8_t)(flow_id ^ 0x80u);
    if (alt == 0u) {
        alt = 1u;   /* 0 no es un FlowID válido */
    }
    return alt;
}

ChronosRT_status_t ChronosRT_et_frm_note_rx(ChronosRT_et_frm_t* frm, uint16_t node_id,
                                            uint32_t seq_num, bool duplicate_flag,
                                            bool* out_is_new) {
    if (frm == NULL || out_is_new == NULL || node_id == 0u ||
        node_id > ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_INVALID;
    }

    et_frm_node_t* n = &frm->nodes[node_id - 1u];

    for (uint32_t i = 0; i < n->count; i++) {
        if (n->ring[i] == seq_num) {
            frm->stats.dropped_dup++;
            *out_is_new = false;
            return ChronosRT_SUCCESS;
        }
    }

    n->ring[n->next] = seq_num;
    n->next = (uint8_t)((n->next + 1u) % ET_FRM_DEDUP_WINDOW);
    if (n->count < ET_FRM_DEDUP_WINDOW) {
        n->count++;
    }
    if (duplicate_flag) {
        frm->stats.duplicates_rx++;
    }
    *out_is_new = true;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_frm_fail_path(ChronosRT_et_frm_t* frm, uint16_t node_id) {
    if (frm == NULL || node_id == 0u || node_id > ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_INVALID;
    }
    et_frm_node_t* n = &frm->nodes[node_id - 1u];
    n->path = (uint8_t)(n->path ^ 1u);
    frm->stats.path_switches++;
    return ChronosRT_SUCCESS;
}

uint8_t ChronosRT_et_frm_active_path(const ChronosRT_et_frm_t* frm, uint16_t node_id) {
    if (frm == NULL || node_id == 0u || node_id > ChronosRT_ET_MAX_NODES) {
        return 0u;
    }
    return frm->nodes[node_id - 1u].path;
}

ChronosRT_status_t ChronosRT_et_frm_stats(const ChronosRT_et_frm_t* frm,
                                          ChronosRT_et_frm_stats_t* out) {
    if (frm == NULL || out == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    *out = frm->stats;
    return ChronosRT_SUCCESS;
}
