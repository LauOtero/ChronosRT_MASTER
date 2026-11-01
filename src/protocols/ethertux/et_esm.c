/*
 * et_esm.c — Máquina de estados del nodo EtherTux (ET-ESM, §6.1).
 *
 *   INIT → CONFIG → SAFE → OP
 *     └──────┴───────┴──────┴──→ FAULT (cuarentena)
 *   FAULT → INIT (reintroducción tras reparación, vía Discovery)
 *
 * El estado del emisor viaja en el campo `State` de cada trama y determina qué
 * tráfico puede generarse: un nodo en FAULT solo responde a Discovery, de modo
 * que un nodo defectuoso no consume ancho de banda ni provoca bloqueos.
 */
#include "et_internal.h"

#include <stdlib.h>

struct ChronosRT_et_esm {
    ChronosRT_et_state_t state;
};

ChronosRT_et_esm_t* ChronosRT_et_esm_create(void) {
    ChronosRT_et_esm_t* esm = (ChronosRT_et_esm_t*)calloc(1u, sizeof(*esm));
    if (esm != NULL) {
        esm->state = ChronosRT_ET_STATE_INIT;
    }
    return esm;
}

void ChronosRT_et_esm_destroy(ChronosRT_et_esm_t* esm) {
    free(esm);
}

void ChronosRT_et_esm_reset(ChronosRT_et_esm_t* esm) {
    if (esm != NULL) {
        esm->state = ChronosRT_ET_STATE_INIT;
    }
}

ChronosRT_et_state_t ChronosRT_et_esm_state(const ChronosRT_et_esm_t* esm) {
    return (esm != NULL) ? esm->state : ChronosRT_ET_STATE_FAULT;
}

bool ChronosRT_et_esm_can_transition(ChronosRT_et_state_t from,
                                     ChronosRT_et_state_t to) {
    switch (from) {
        case ChronosRT_ET_STATE_INIT:
            return to == ChronosRT_ET_STATE_CONFIG || to == ChronosRT_ET_STATE_FAULT;
        case ChronosRT_ET_STATE_CONFIG:
            return to == ChronosRT_ET_STATE_SAFE || to == ChronosRT_ET_STATE_FAULT ||
                   to == ChronosRT_ET_STATE_INIT;
        case ChronosRT_ET_STATE_SAFE:
            return to == ChronosRT_ET_STATE_OP || to == ChronosRT_ET_STATE_FAULT ||
                   to == ChronosRT_ET_STATE_CONFIG;
        case ChronosRT_ET_STATE_OP:
            return to == ChronosRT_ET_STATE_SAFE || to == ChronosRT_ET_STATE_FAULT;
        case ChronosRT_ET_STATE_FAULT:
            return to == ChronosRT_ET_STATE_INIT;
        default:
            return false;
    }
}

ChronosRT_status_t ChronosRT_et_esm_request(ChronosRT_et_esm_t* esm,
                                            ChronosRT_et_state_t next) {
    if (esm == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    if (!ChronosRT_et_esm_can_transition(esm->state, next)) {
        return ChronosRT_ERROR_STATE;
    }
    esm->state = next;
    return ChronosRT_SUCCESS;
}

bool ChronosRT_et_esm_allows_traffic(ChronosRT_et_state_t state,
                                     uint8_t type, uint8_t priority) {
    if (priority >= ChronosRT_ET_PRIORITIES) {
        return false;
    }
    const bool mailbox = (type == (uint8_t)ChronosRT_ET_TYPE_MAILBOX_DATA ||
                          type == (uint8_t)ChronosRT_ET_TYPE_MAILBOX_ACK);
    const bool discovery = (type == (uint8_t)ChronosRT_ET_TYPE_DISCOVERY);

    switch (state) {
        case ChronosRT_ET_STATE_INIT:
        case ChronosRT_ET_STATE_FAULT:
            return discovery;
        case ChronosRT_ET_STATE_CONFIG:
            return discovery || mailbox;
        case ChronosRT_ET_STATE_SAFE:
            return mailbox || priority == (uint8_t)ChronosRT_ET_P0 ||
                   priority == (uint8_t)ChronosRT_ET_P1;
        case ChronosRT_ET_STATE_OP:
            return true;
        default:
            return false;
    }
}

uint32_t ChronosRT_et_esm_build_state_change(ChronosRT_et_state_t target,
                                             uint8_t reason,
                                             uint8_t out[2]) {
    if (out == NULL || (uint8_t)target > (uint8_t)ChronosRT_ET_STATE_FAULT) {
        return 0u;
    }
    out[0] = (uint8_t)target;
    out[1] = reason;
    return 2u;
}

ChronosRT_status_t ChronosRT_et_esm_parse_state_change(const uint8_t* in, uint32_t len,
                                                       ChronosRT_et_state_t* target,
                                                       uint8_t* reason) {
    if (in == NULL || len < 2u) {
        return ChronosRT_ERROR_INVALID;
    }
    if (in[0] > (uint8_t)ChronosRT_ET_STATE_FAULT) {
        return ChronosRT_ERROR_INVALID;
    }
    if (target != NULL) {
        *target = (ChronosRT_et_state_t)in[0];
    }
    if (reason != NULL) {
        *reason = in[1];
    }
    return ChronosRT_SUCCESS;
}

bool ChronosRT_et_esm_state_from_wire(uint8_t raw, ChronosRT_et_state_t* out) {
    if (raw > (uint8_t)ChronosRT_ET_STATE_FAULT) {
        return false;
    }
    if (out != NULL) {
        *out = (ChronosRT_et_state_t)raw;
    }
    return true;
}
