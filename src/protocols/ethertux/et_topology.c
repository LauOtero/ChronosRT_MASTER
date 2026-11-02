/*
 * et_topology.c — Registro de nodos, ET-WKC, descubrimiento y Hot Connect
 *                 (§6.2–6.5).
 *
 * El registro es estático (64 entradas, sin asignación dinámica) y guarda por
 * nodo: NodeID, MAC, capacidades, slot TDMA, estado ET-ESM, último ET-WKC,
 * contador de discrepancias, offset/estado de sincronización y temporizador de
 * heartbeat.
 *
 * Verificación pasiva (§6.2): cada nodo que procesa una trama incrementa el
 * ET-WKC del campo de cabecera. El maestro compara el valor recibido con el
 * esperado según el registro; N discrepancias consecutivas (3 por defecto)
 * disparan un `TopoCheck` unicast (§6.3), sin broadcast.
 *
 * Hot Connect (§6.4): cada 100 superframes se sondea una dirección libre con
 * `Discovery` unicast; nunca se usa broadcast en operación.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

struct ChronosRT_et_topology {
    ChronosRT_et_topology_config_t cfg;
    ChronosRT_et_node_info_t       nodes[ChronosRT_ET_MAX_NODES];
    uint32_t                       count;
    /* Caché directa {hash(MAC) -> índice} validada en cada acierto: el camino
     * por trama pasa de O(N) `memcmp` a un solo acceso. Al validar la MAC no
     * necesita invalidación tras `remove`/`upsert` (reordenamientos incluidos). */
    uint16_t                       mac_cache[ChronosRT_ET_MAX_NODES * 4u];
};

/* Hash de 6 B barato (multiplicación-xor): no hace falta CRC aquí. */
static inline uint32_t topo_mac_hash(const uint8_t mac[6]) {
    uint32_t h = 0x9E3779B9u;
    h ^= (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
    h *= 0x85EBCA6Bu;
    h ^= (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
    h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

static inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void ChronosRT_et_topology_default_config(ChronosRT_et_topology_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->wkc_mismatch_threshold = 3u;          /* 3 superframes  */
    cfg->hot_connect_period_frames = 100u;     /* 100 ms         */
    cfg->heartbeat_timeout_frames = 3u;        /* 3 superframes  */
}

ChronosRT_et_topology_t* ChronosRT_et_topology_create(void) {
    ChronosRT_et_topology_t* t = (ChronosRT_et_topology_t*)calloc(1u, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    ChronosRT_et_topology_default_config(&t->cfg);
    for (uint32_t i = 0; i < ChronosRT_ET_MAX_NODES; i++) {
        t->nodes[i].slot = ChronosRT_ET_SLOT_FREE;
    }
    return t;
}

void ChronosRT_et_topology_destroy(ChronosRT_et_topology_t* topo) {
    free(topo);
}

ChronosRT_status_t ChronosRT_et_topology_configure(ChronosRT_et_topology_t* topo,
                                                   const ChronosRT_et_topology_config_t* cfg) {
    if (topo == NULL || cfg == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    if (cfg->wkc_mismatch_threshold == 0u || cfg->heartbeat_timeout_frames == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    topo->cfg = *cfg;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_topology_upsert(ChronosRT_et_topology_t* topo,
                                                const ChronosRT_et_node_info_t* info) {
    if (topo == NULL || info == NULL || info->id == 0u) {
        return ChronosRT_ERROR_INVALID;
    }

    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == info->id) {
            uint16_t slot = topo->nodes[i].slot;   /* conserva el slot asignado */
            topo->nodes[i] = *info;
            if (info->slot >= (uint16_t)ChronosRT_ET_MAX_NODES) {
                topo->nodes[i].slot = slot;
            }
            return ChronosRT_SUCCESS;
        }
    }

    if (topo->count >= ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_FULL;
    }
    ChronosRT_et_node_info_t* n = &topo->nodes[topo->count++];
    *n = *info;
    if (n->slot >= (uint16_t)ChronosRT_ET_MAX_NODES) {
        n->slot = ChronosRT_ET_SLOT_FREE;
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_topology_remove(ChronosRT_et_topology_t* topo,
                                                uint16_t node_id) {
    if (topo == NULL || node_id == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == node_id) {
            for (uint32_t j = i + 1u; j < topo->count; j++) {
                topo->nodes[j - 1u] = topo->nodes[j];
            }
            topo->count--;
            return ChronosRT_SUCCESS;
        }
    }
    return ChronosRT_ERROR_NOTFOUND;
}

const ChronosRT_et_node_info_t* ChronosRT_et_topology_find(const ChronosRT_et_topology_t* topo,
                                                           uint16_t node_id) {
    if (topo == NULL || node_id == 0u) {
        return NULL;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == node_id) {
            return &topo->nodes[i];
        }
    }
    return NULL;
}

uint32_t ChronosRT_et_topology_count(const ChronosRT_et_topology_t* topo) {
    return (topo != NULL) ? topo->count : 0u;
}

const ChronosRT_et_node_info_t* ChronosRT_et_topology_at(const ChronosRT_et_topology_t* topo,
                                                         uint32_t index) {
    if (topo == NULL || index >= topo->count) {
        return NULL;
    }
    return &topo->nodes[index];
}

uint16_t ChronosRT_et_topology_next_free_slot(const ChronosRT_et_topology_t* topo) {
    if (topo == NULL) {
        return ChronosRT_ET_SLOT_FREE;
    }
    for (uint16_t slot = 0u; slot < (uint16_t)ChronosRT_ET_MAX_NODES; slot++) {
        bool used = false;
        for (uint32_t i = 0; i < topo->count; i++) {
            if (topo->nodes[i].slot == slot) {
                used = true;
                break;
            }
        }
        if (!used) {
            return slot;
        }
    }
    return ChronosRT_ET_SLOT_FREE;
}

ChronosRT_status_t ChronosRT_et_topology_assign_slot(ChronosRT_et_topology_t* topo,
                                                     uint16_t node_id,
                                                     uint16_t slot) {
    if (topo == NULL || node_id == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    if (slot != ChronosRT_ET_SLOT_FREE && slot >= (uint16_t)ChronosRT_ET_MAX_NODES) {
        return ChronosRT_ERROR_INVALID;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        if (topo->nodes[i].id == node_id) {
            topo->nodes[i].slot = slot;
            return ChronosRT_SUCCESS;
        }
    }
    return ChronosRT_ERROR_NOTFOUND;
}

ChronosRT_status_t ChronosRT_et_topology_on_frame(ChronosRT_et_topology_t* topo,
                                                  const uint8_t mac[6],
                                                  const ChronosRT_et_header_t* h,
                                                  uint64_t now_ns) {
    if (topo == NULL || mac == NULL || h == NULL) {
        return ChronosRT_ERROR_INVALID;
    }

    /* Camino rápido: caché validada por MAC (1 acceso, sin barrido). */
    const uint32_t ch = topo_mac_hash(mac) & (uint32_t)(sizeof(topo->mac_cache) / sizeof(topo->mac_cache[0]) - 1u);
    const uint32_t ci = topo->mac_cache[ch];
    if (ci < topo->count && memcmp(topo->nodes[ci].mac, mac, 6u) == 0) {
        ChronosRT_et_node_info_t* n = &topo->nodes[ci];
        n->last_seen_ns = now_ns;
        ChronosRT_et_state_t st;
        if (ChronosRT_et_esm_state_from_wire(h->state, &st)) {
            n->state = st;
        }
        n->last_wkc = h->wkc;
        if (n->expected_wkc != 0u && h->wkc != n->expected_wkc) {
            if (n->wkc_mismatch_count < UINT8_MAX) {
                n->wkc_mismatch_count++;
            }
            return ChronosRT_ERROR_CORRUPT;
        }
        n->wkc_mismatch_count = 0u;
        return ChronosRT_SUCCESS;
    }

    for (uint32_t i = 0; i < topo->count; i++) {
        ChronosRT_et_node_info_t* n = &topo->nodes[i];
        if (memcmp(n->mac, mac, 6u) != 0) {
            continue;
        }
        topo->mac_cache[ch] = (uint16_t)i;   /* refresca la caché */

        n->last_seen_ns = now_ns;
        ChronosRT_et_state_t st;
        if (ChronosRT_et_esm_state_from_wire(h->state, &st)) {
            n->state = st;
        }
        n->last_wkc = h->wkc;

        /* ET-WKC esperado: 0 desactiva la verificación de ese nodo. */
        if (n->expected_wkc != 0u && h->wkc != n->expected_wkc) {
            if (n->wkc_mismatch_count < UINT8_MAX) {
                n->wkc_mismatch_count++;
            }
            return ChronosRT_ERROR_CORRUPT;
        }
        n->wkc_mismatch_count = 0u;
        return ChronosRT_SUCCESS;
    }
    return ChronosRT_ERROR_NOTFOUND;
}

bool ChronosRT_et_topology_needs_topo_check(const ChronosRT_et_topology_t* topo,
                                            uint16_t* out_node_id) {
    if (topo == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < topo->count; i++) {
        const ChronosRT_et_node_info_t* n = &topo->nodes[i];
        if (n->wkc_mismatch_count >= topo->cfg.wkc_mismatch_threshold) {
            if (out_node_id != NULL) {
                *out_node_id = n->id;
            }
            return true;
        }
    }
    return false;
}

uint32_t ChronosRT_et_topology_tick(ChronosRT_et_topology_t* topo,
                                    uint64_t now_ns, uint32_t cycle_ns) {
    if (topo == NULL || cycle_ns == 0u) {
        return 0u;
    }
    uint64_t limit = (uint64_t)cycle_ns * (uint64_t)topo->cfg.heartbeat_timeout_frames;
    uint32_t faulted = 0u;

    for (uint32_t i = 0; i < topo->count; i++) {
        ChronosRT_et_node_info_t* n = &topo->nodes[i];
        if (n->state == ChronosRT_ET_STATE_FAULT || n->last_seen_ns == 0u) {
            continue;
        }
        if (now_ns > n->last_seen_ns && (now_ns - n->last_seen_ns) > limit) {
            n->state = ChronosRT_ET_STATE_FAULT;
            n->slot = ChronosRT_ET_SLOT_FREE;   /* se libera para reasignación */
            faulted++;
        }
    }
    return faulted;
}

bool ChronosRT_et_topology_hot_connect_due(const ChronosRT_et_topology_t* topo,
                                            uint64_t frame_index,
                                            uint16_t* out_node_addr) {
    if (topo == NULL || topo->cfg.hot_connect_period_frames == 0u) {
        return false;
    }
    if (frame_index % (uint64_t)topo->cfg.hot_connect_period_frames != 0u) {
        return false;
    }
    /* Dirección libre siguiente (NodeID consecutivo al último registrado). */
    if (out_node_addr != NULL) {
        *out_node_addr = (uint16_t)(topo->count + 1u);
    }
    return true;
}

uint32_t ChronosRT_et_topology_build_reply(const ChronosRT_et_node_info_t* info,
                                            uint8_t out[ChronosRT_ET_DISCOVERY_REPLY_SIZE]) {
    if (info == NULL || out == NULL || info->id == 0u) {
        return 0u;
    }
    put_le16(out + 0u, info->id);
    out[2] = (uint8_t)info->state;
    out[3] = info->fec_supported ? 0x01u : 0x00u;
    put_le16(out + 4u, info->link_mbps);
    put_le16(out + 6u, info->buffer_bytes);
    return ChronosRT_ET_DISCOVERY_REPLY_SIZE;
}

ChronosRT_status_t ChronosRT_et_topology_parse_reply(const uint8_t* in, uint32_t len,
                                                      ChronosRT_et_node_info_t* out) {
    if (in == NULL || out == NULL || len < ChronosRT_ET_DISCOVERY_REPLY_SIZE) {
        return ChronosRT_ERROR_INVALID;
    }
    uint16_t id = get_le16(in + 0u);
    if (id == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->state = (in[2] <= (uint8_t)ChronosRT_ET_STATE_FAULT)
                     ? (ChronosRT_et_state_t)in[2]
                     : ChronosRT_ET_STATE_INIT;
    out->fec_supported = (in[3] & 0x01u) != 0u;
    out->link_mbps = get_le16(in + 4u);
    out->buffer_bytes = get_le16(in + 6u);
    out->slot = ChronosRT_ET_SLOT_FREE;
    return ChronosRT_SUCCESS;
}
