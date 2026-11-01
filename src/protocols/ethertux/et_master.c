/*
 * et_master.c — Maestro EtherTux: ciclo de vida, envío/recepción y nodos.
 *
 * Camino de un paquete (spec §8.2):
 *
 *   generación -> clasificación P0–P5 -> control de admisión (QoS)
 *     -> ventana time-triggered (planificador) -> construcción de trama
 *     -> CRC-32C (HAL) -> anillo de socket -> pacing SO_TXTIME -> NIC
 *
 * Todo el estado del camino caliente está pre-asignado en el maestro: los
 * búferes de TX/RX son miembros de la estructura y el envío no reserva memoria.
 * La única ruta que asigna es `send_fec_block` (los bloques de paridad se
 * calculan por bloques, no por trama) y la decodificación FEC de recuperación.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/* Nodos                                                               */
/* ------------------------------------------------------------------ */

int et_master_touch_node(ChronosRT_et_master_t* master, const uint8_t mac[6],
                         uint64_t now_ns) {
    if (master == NULL || mac == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < master->node_count; i++) {
        if (memcmp(master->nodes[i].mac, mac, 6) == 0) {
            master->nodes[i].last_seen_ns = (int64_t)now_ns;
            master->nodes[i].missed_cycles = 0u;
            master->nodes[i].lost = false;
            return (int)i;
        }
    }

    if (master->node_count >= ChronosRT_ET_MAX_NODES) {
        return -1;
    }

    ChronosRT_et_node_t* n = &master->nodes[master->node_count];
    memset(n, 0, sizeof(*n));
    memcpy(n->mac, mac, 6);
    n->id = (uint16_t)(master->node_count + 1u);
    n->last_seen_ns = (int64_t)now_ns;
    master->node_count++;
    return (int)(master->node_count - 1u);
}

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

ChronosRT_et_master_t* ChronosRT_et_master_create(void) {
    ChronosRT_et_master_t* m = (ChronosRT_et_master_t*)calloc(1u, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->port.fd = -1;
    m->link_bps = 1000000000ull;
    m->cycle_ns = 1000000u;

    m->qos = ChronosRT_et_qos_create();
    if (m->qos == NULL) {
        free(m);
        return NULL;
    }
    return m;
}

void ChronosRT_et_master_destroy(ChronosRT_et_master_t* master) {
    if (master == NULL) {
        return;
    }
    ChronosRT_et_master_close(master);
    ChronosRT_et_qos_destroy(master->qos);
    ChronosRT_et_fec_destroy(master->fec);
    free(master);
}

ChronosRT_status_t ChronosRT_et_master_open_ex(ChronosRT_et_master_t* master,
                                               const char* ifname,
                                               uint64_t link_bps,
                                               uint32_t cycle_ns) {
    if (master == NULL || ifname == NULL || ifname[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }
    if (master->is_open) {
        return ChronosRT_ERROR_STATE;
    }

    ChronosRT_status_t st = et_socket_open(&master->port, ifname);
    if (st != ChronosRT_SUCCESS) {
        return st;
    }
    /* AUTO: se intenta el anillo de transmisión; si el kernel no lo permite,
     * se continúa con llamadas al sistema sin alterar el comportamiento. */
    (void)et_socket_tx_ring_enable(&master->port, 64u);

    master->link_bps = (link_bps != 0u) ? link_bps : 1000000000ull;
    master->cycle_ns = (cycle_ns != 0u) ? cycle_ns : 1000000u;
    master->is_open = true;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_master_set_tx_mode(ChronosRT_et_master_t* master,
                                                   ChronosRT_et_tx_mode_t mode) {
    if (master == NULL || !master->is_open) {
        return ChronosRT_ERROR_STATE;
    }
    switch (mode) {
        case ChronosRT_ET_TX_SYSCALL:
            et_socket_tx_ring_disable(&master->port);
            return ChronosRT_SUCCESS;
        case ChronosRT_ET_TX_AUTO:
            if (!et_socket_tx_ring_active(&master->port)) {
                (void)et_socket_tx_ring_enable(&master->port, 64u);
            }
            return ChronosRT_SUCCESS;
        case ChronosRT_ET_TX_RING:
            if (et_socket_tx_ring_active(&master->port)) {
                return ChronosRT_SUCCESS;
            }
            return et_socket_tx_ring_enable(&master->port, 64u)
                   ? ChronosRT_SUCCESS : ChronosRT_ERROR_NOTSUP;
        default:
            return ChronosRT_ERROR_INVALID;
    }
}

bool ChronosRT_et_master_tx_ring_active(const ChronosRT_et_master_t* master) {
    return (master != NULL) && et_socket_tx_ring_active(&master->port);
}

ChronosRT_status_t ChronosRT_et_master_open(ChronosRT_et_master_t* master,
                                            const char* ifname) {
    return ChronosRT_et_master_open_ex(master, ifname, 0u, 0u);
}

void ChronosRT_et_master_close(ChronosRT_et_master_t* master) {
    if (master == NULL) {
        return;
    }
    et_socket_close(&master->port);
    master->is_open = false;
}

bool ChronosRT_et_master_is_open(const ChronosRT_et_master_t* master) {
    return (master != NULL) && master->is_open;
}

const char* ChronosRT_et_master_interface(const ChronosRT_et_master_t* master) {
    if (master == NULL || !master->is_open) {
        return NULL;
    }
    return master->port.ifname;
}

ChronosRT_status_t ChronosRT_et_master_set_qos(ChronosRT_et_master_t* master,
                                               const ChronosRT_et_qos_config_t* cfg) {
    if (master == NULL || cfg == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    return ChronosRT_et_qos_configure(master->qos, cfg);
}

ChronosRT_status_t ChronosRT_et_master_set_fec(ChronosRT_et_master_t* master,
                                               ChronosRT_et_fec_mode_t mode) {
    if (master == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    ChronosRT_et_fec_destroy(master->fec);
    master->fec = NULL;

    if (mode == ChronosRT_ET_FEC_OFF) {
        return ChronosRT_SUCCESS;
    }
    master->fec = ChronosRT_et_fec_create_mode(mode);
    return (master->fec != NULL) ? ChronosRT_SUCCESS : ChronosRT_ERROR_INVALID;
}

ChronosRT_status_t ChronosRT_et_master_attach_scheduler(ChronosRT_et_master_t* master,
                                                        ChronosRT_et_sched_t* sched) {
    if (master == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    master->sched = sched;   /* sin propiedad */
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_master_attach_topology(ChronosRT_et_master_t* master,
                                                       ChronosRT_et_topology_t* topo) {
    if (master == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    master->topo = topo;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_master_attach_sync(ChronosRT_et_master_t* master,
                                                   ChronosRT_et_sync_t* sync) {
    if (master == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    master->sync = sync;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_master_attach_esm(ChronosRT_et_master_t* master,
                                                  ChronosRT_et_esm_t* esm) {
    if (master == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    master->esm = esm;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_master_attach_mbx(ChronosRT_et_master_t* master,
                                                  ChronosRT_et_mbx_t* mbx) {
    if (master == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    master->mbx = mbx;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_master_attach_frm(ChronosRT_et_master_t* master,
                                                  ChronosRT_et_frm_t* frm) {
    if (master == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    master->frm = frm;
    return ChronosRT_SUCCESS;
}

uint16_t ChronosRT_et_master_last_wkc(const ChronosRT_et_master_t* master) {
    return (master != NULL) ? master->last_wkc : 0u;
}

/* ------------------------------------------------------------------ */
/* Envío                                                               */
/* ------------------------------------------------------------------ */

/* Construye la trama (Ethernet II + PDU) en el búfer del maestro y la envía. */
static ChronosRT_status_t et_transmit_pdu(ChronosRT_et_master_t* master,
                                          const ChronosRT_et_header_t* h,
                                          const void* payload, uint16_t len,
                                          bool paced, uint64_t deadline_ns) {
    uint8_t* frame = master->tx_frame;

    /* Cabecera Ethernet II (destino difusión: el segmento EtherTux es cerrado). */
    memset(frame, 0xFF, 6);
    memcpy(frame + 6, master->port.mac, 6);
    uint16_t eth_type = htons((uint16_t)ChronosRT_ET_ETHERTYPE);
    memcpy(frame + 12, &eth_type, sizeof(eth_type));

    uint32_t body = ChronosRT_et_pdu_build(frame + ChronosRT_ET_ETH_HDR_SIZE,
                                           ChronosRT_ET_MAX_FRAME - ChronosRT_ET_ETH_HDR_SIZE,
                                           h, payload, len);
    if (body == 0u) {
        return ChronosRT_ERROR_INVALID;
    }

    uint32_t total = ChronosRT_ET_ETH_HDR_SIZE + body;
    int rc = paced ? et_socket_send_at(&master->port, frame, total, deadline_ns)
                   : et_socket_send(&master->port, frame, total);
    if (rc < 0) {
        return ChronosRT_ERROR_IO;
    }

    master->stats.frames_tx++;
    master->stats.bytes_tx += (uint64_t)len;
    return ChronosRT_SUCCESS;
}

/* Rellena la cabecera EtherTux con las reglas comunes de flags y secuencia. */
static void et_fill_header(ChronosRT_et_master_t* master, ChronosRT_et_header_t* h,
                           uint8_t priority, ChronosRT_et_type_t type, uint8_t flow_id,
                           ChronosRT_et_state_t state, uint64_t now,
                           uint8_t flow_count, uint16_t block_id) {
    memset(h, 0, sizeof(*h));
    h->type = (uint8_t)type;
    h->priority = ChronosRT_et_qos_effective_priority(master->qos, priority);
    h->state = (uint8_t)state;
    h->flow_id = flow_id;
    h->seq_num = ++master->seq[flow_id];
    h->tx_timestamp = now;
    h->wkc = 0u;              /* cada nodo que la procese lo incrementa */
    h->block_id = block_id;
    h->flow_count = flow_count;

    if (priority == ChronosRT_ET_P0) {
        h->flags |= ChronosRT_ET_FLAG_EXPRESS;
    } else {
        h->flags |= ChronosRT_ET_FLAG_PREEMPTABLE;
    }
    if (type == ChronosRT_ET_TYPE_FEC) {
        h->flags |= ChronosRT_ET_FLAG_FEC;
    }
    if (type == ChronosRT_ET_TYPE_DISCOVERY) {
        h->flags |= ChronosRT_ET_FLAG_DISCOVERY;
    }
}

static ChronosRT_status_t et_send_frame_ex(ChronosRT_et_master_t* master,
                                           uint8_t priority,
                                           ChronosRT_et_type_t type,
                                           uint8_t flow_id,
                                           const void* payload, uint16_t len,
                                           uint8_t flow_count,
                                           bool paced, uint64_t deadline_ns,
                                           uint16_t block_id) {
    if (master == NULL || !master->is_open) {
        return ChronosRT_ERROR_STATE;
    }
    if (len > ChronosRT_ET_MAX_PAYLOAD || flow_count == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    if (priority >= ChronosRT_ET_PRIORITIES) {
        return ChronosRT_ERROR_INVALID;
    }

    /* ET-ESM: el estado del nodo emisor limita el tráfico permitido (§6.1). */
    ChronosRT_et_state_t state = (master->esm != NULL) ? ChronosRT_et_esm_state(master->esm)
                                                       : ChronosRT_ET_STATE_OP;
    if (!ChronosRT_et_esm_allows_traffic(state, (uint8_t)type, priority)) {
        return ChronosRT_ERROR_STATE;
    }

    uint64_t now = ChronosRT_tx_timestamp_ns();
    uint32_t wire = ChronosRT_ET_ETH_HDR_SIZE + ChronosRT_ET_HEADER_SIZE + len;

    ChronosRT_status_t st = ChronosRT_et_qos_admit(master->qos, priority, wire, now);
    if (st != ChronosRT_SUCCESS) {
        if (st == ChronosRT_ERROR_DROPPED) {
            master->stats.dropped++;
        } else {
            master->stats.backpressure++;
        }
        return st;
    }

    /* Ventana time-triggered (si hay planificador asociado). P0 siempre pasa. */
    if (master->sched != NULL &&
        !ChronosRT_et_sched_can_transmit(master->sched, priority, now)) {
        master->stats.backpressure++;
        return ChronosRT_ERROR_BACKPRESSURE;
    }

    ChronosRT_et_header_t h;
    et_fill_header(master, &h, priority, type, flow_id, state, now, flow_count, block_id);

    st = et_transmit_pdu(master, &h, payload, len, paced, deadline_ns);
    if (st != ChronosRT_SUCCESS) {
        return st;
    }

    /*
     * FRM (§8.2): los flujos P0/P1 se duplican por el camino alternativo. El
     * duplicado conserva el SeqNum y marca `Flags[4]`; el receptor lo descarta
     * con `ChronosRT_et_frm_note_rx`.
     */
    if (master->frm != NULL && flow_count == 1u &&
        ChronosRT_et_frm_should_duplicate(master->frm, priority)) {
        ChronosRT_et_header_t dup = h;
        dup.flow_id = ChronosRT_et_frm_alternate_flow(flow_id);
        dup.flags = (uint8_t)(h.flags | ChronosRT_ET_FLAG_FRM);
        if (et_transmit_pdu(master, &dup, payload, len, paced, deadline_ns) ==
            ChronosRT_SUCCESS) {
            master->stats.duplicates_tx++;
        }
    }

    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_master_send(ChronosRT_et_master_t* master,
                                            uint8_t priority,
                                            ChronosRT_et_type_t type,
                                            uint8_t flow_id,
                                            const void* payload, uint16_t len) {
    return et_send_frame_ex(master, priority, type, flow_id, payload, len,
                            1u, false, 0u, 0u);
}

ChronosRT_status_t ChronosRT_et_master_send_at(ChronosRT_et_master_t* master,
                                               uint8_t priority,
                                               ChronosRT_et_type_t type,
                                               uint8_t flow_id,
                                               const void* payload, uint16_t len,
                                               uint64_t tx_deadline_ns) {
    return et_send_frame_ex(master, priority, type, flow_id, payload, len,
                            1u, true, tx_deadline_ns, 0u);
}

ChronosRT_status_t ChronosRT_et_master_send_aggregated(ChronosRT_et_master_t* master,
                                                       uint8_t priority,
                                                       uint8_t flow_id,
                                                       const ChronosRT_et_flow_entry_t* entries,
                                                       uint32_t count) {
    if (master == NULL || entries == NULL || count < 2u || count > UINT8_MAX) {
        return ChronosRT_ERROR_INVALID;
    }
    uint32_t plen = 0u;
    ChronosRT_status_t st = ChronosRT_et_flowagg_build(entries, count, master->aux,
                                                       sizeof(master->aux), &plen);
    if (st != ChronosRT_SUCCESS) {
        return st;
    }
    return et_send_frame_ex(master, priority, ChronosRT_ET_TYPE_DATA, flow_id,
                            master->aux, (uint16_t)plen, (uint8_t)count,
                            false, 0u, 0u);
}

ChronosRT_status_t ChronosRT_et_master_send_mailbox(ChronosRT_et_master_t* master,
                                                    uint8_t channel,
                                                    const void* data, uint16_t len,
                                                    uint8_t p4_occupancy_percent) {
    if (master == NULL || master->mbx == NULL || data == NULL || len == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    uint32_t plen = 0u;
    ChronosRT_status_t st = ChronosRT_et_mbx_next_segment(master->mbx, channel,
                                                          (const uint8_t*)data, len,
                                                          master->aux, sizeof(master->aux),
                                                          &plen);
    if (st != ChronosRT_SUCCESS) {
        return st;
    }
    uint8_t prio = ChronosRT_et_mbx_effective_priority(master->mbx, p4_occupancy_percent);
    return et_send_frame_ex(master, prio, ChronosRT_ET_TYPE_MAILBOX_DATA, 255u,
                            master->aux, (uint16_t)plen, 1u, false, 0u, 0u);
}

ChronosRT_status_t ChronosRT_et_master_send_fec_block(ChronosRT_et_master_t* master,
                                                      uint8_t priority,
                                                      uint8_t flow_id,
                                                      const uint8_t* const* data,
                                                      uint32_t count,
                                                      uint32_t len) {
    if (master == NULL || data == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    if (master->fec == NULL) {
        return ChronosRT_ERROR_NOTSUP;
    }
    if (count != (uint32_t)master->fec->k || len == 0u ||
        len > ChronosRT_ET_MAX_PAYLOAD) {
        return ChronosRT_ERROR_INVALID;
    }

    uint8_t k = master->fec->k;
    uint8_t n = master->fec->n;
    uint32_t parity_bytes = (uint32_t)(n - k) * len;

    uint8_t* parity_store = (uint8_t*)malloc(parity_bytes);
    if (parity_store == NULL) {
        return ChronosRT_ERROR_MEMORY;
    }

    uint8_t* par_ptrs[ChronosRT_RS_MAX_N];
    for (uint32_t j = 0; j < (uint32_t)(n - k); j++) {
        par_ptrs[j] = parity_store + (size_t)j * len;
    }

    ChronosRT_status_t st = ChronosRT_et_fec_encode(master->fec, data, par_ptrs, len);
    if (st != ChronosRT_SUCCESS) {
        free(parity_store);
        return st;
    }

    uint16_t base = (uint16_t)(((uint32_t)(master->block_seq & 0x00FFu)) << 8);

    for (uint32_t i = 0; i < k; i++) {
        st = et_send_frame_ex(master, priority, ChronosRT_ET_TYPE_DATA, flow_id,
                              data[i], (uint16_t)len, 1u, false, 0u,
                              (uint16_t)(base | (uint16_t)i));
        if (st != ChronosRT_SUCCESS) {
            free(parity_store);
            return st;
        }
    }
    for (uint32_t j = 0; j < (uint32_t)(n - k); j++) {
        st = et_send_frame_ex(master, priority, ChronosRT_ET_TYPE_FEC, flow_id,
                              par_ptrs[j], (uint16_t)len, 1u, false, 0u,
                              (uint16_t)(base | (uint16_t)(k + j)));
        if (st != ChronosRT_SUCCESS) {
            free(parity_store);
            return st;
        }
    }

    master->block_seq++;
    master->stats.fec_blocks++;
    free(parity_store);
    return ChronosRT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Camino rápido: cero copias, lotes y velocidad de enlace             */
/* ------------------------------------------------------------------ */

uint64_t ChronosRT_et_link_speed_bps(const char* ifname) {
    return et_socket_link_bps(ifname);
}

ChronosRT_status_t ChronosRT_et_master_open_auto(ChronosRT_et_master_t* master,
                                                 const char* ifname) {
    if (master == NULL || ifname == NULL || ifname[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }
    uint64_t bps = et_socket_link_bps(ifname);
    if (bps == 0u) {
        bps = 1000000000ull;   /* enlace caído o no legible: 1 Gbps */
    }
    return ChronosRT_et_master_open_ex(master, ifname, bps, 0u);
}

/* Cabecera Ethernet + EtherTux contiguas (14 + 26 B) con el CRC ya calculado. */
static uint32_t et_build_wire_header(ChronosRT_et_master_t* master, uint8_t* out,
                                     const ChronosRT_et_header_t* h,
                                     const void* payload, uint16_t len) {
    memset(out, 0xFF, 6);
    memcpy(out + 6, master->port.mac, 6);
    uint16_t eth_type = htons((uint16_t)ChronosRT_ET_ETHERTYPE);
    memcpy(out + 12, &eth_type, sizeof(eth_type));
    return ChronosRT_et_pdu_build_header(out + ChronosRT_ET_ETH_HDR_SIZE, h, payload, len);
}

ChronosRT_status_t ChronosRT_et_master_send_zerocopy(ChronosRT_et_master_t* master,
                                                     uint8_t priority,
                                                     ChronosRT_et_type_t type,
                                                     uint8_t flow_id,
                                                     const void* payload, uint16_t len) {
    if (master == NULL || !master->is_open) {
        return ChronosRT_ERROR_STATE;
    }
    if (len > ChronosRT_ET_MAX_PAYLOAD || (payload == NULL && len > 0u)) {
        return ChronosRT_ERROR_INVALID;
    }
    if (priority >= ChronosRT_ET_PRIORITIES) {
        return ChronosRT_ERROR_INVALID;
    }

    ChronosRT_et_state_t state = (master->esm != NULL) ? ChronosRT_et_esm_state(master->esm)
                                                       : ChronosRT_ET_STATE_OP;
    if (!ChronosRT_et_esm_allows_traffic(state, (uint8_t)type, priority)) {
        return ChronosRT_ERROR_STATE;
    }

    uint64_t now = ChronosRT_tx_timestamp_ns();
    uint32_t wire = ChronosRT_ET_ETH_HDR_SIZE + ChronosRT_ET_HEADER_SIZE + len;
    ChronosRT_status_t st = ChronosRT_et_qos_admit(master->qos, priority, wire, now);
    if (st != ChronosRT_SUCCESS) {
        if (st == ChronosRT_ERROR_DROPPED) {
            master->stats.dropped++;
        } else {
            master->stats.backpressure++;
        }
        return st;
    }
    if (master->sched != NULL &&
        !ChronosRT_et_sched_can_transmit(master->sched, priority, now)) {
        master->stats.backpressure++;
        return ChronosRT_ERROR_BACKPRESSURE;
    }

    ChronosRT_et_header_t h;
    et_fill_header(master, &h, priority, type, flow_id, state, now, 1u, 0u);

    uint8_t* hdr = master->tx_frame;   /* solo los primeros 40 B */
    if (et_build_wire_header(master, hdr, &h, payload, len) == 0u) {
        return ChronosRT_ERROR_INVALID;
    }

    uint32_t hdr_len = ChronosRT_ET_ETH_HDR_SIZE + ChronosRT_ET_HEADER_SIZE;
    if (et_socket_send_iov(&master->port, hdr, hdr_len, payload, len) < 0) {
        return ChronosRT_ERROR_IO;
    }
    master->stats.frames_tx++;
    master->stats.bytes_tx += (uint64_t)len;
    return ChronosRT_SUCCESS;
}

int ChronosRT_et_master_send_batch(ChronosRT_et_master_t* master,
                                   const ChronosRT_et_batch_item_t* items,
                                   uint32_t count) {
    enum { ET_BATCH_MAX = 16u };

    if (master == NULL || !master->is_open || items == NULL || count == 0u) {
        return -1;
    }
    if (count > ET_BATCH_MAX) {
        count = ET_BATCH_MAX;
    }

    /* Estado pre-asignado del lote (el maestro no es thread-safe). */
    static struct mmsghdr msgs[ET_BATCH_MAX];
    static struct iovec   iovs[ET_BATCH_MAX][2];
    static uint8_t        hdrs[ET_BATCH_MAX][ChronosRT_ET_ETH_HDR_SIZE +
                                            ChronosRT_ET_HEADER_SIZE];

    const ChronosRT_et_state_t state = (master->esm != NULL)
                                           ? ChronosRT_et_esm_state(master->esm)
                                           : ChronosRT_ET_STATE_OP;
    uint64_t now = ChronosRT_tx_timestamp_ns();
    uint64_t bytes = 0u;
    uint32_t n = 0u;

    for (uint32_t i = 0; i < count; i++) {
        const ChronosRT_et_batch_item_t* it = &items[i];
        if (it->priority >= ChronosRT_ET_PRIORITIES ||
            it->len > ChronosRT_ET_MAX_PAYLOAD ||
            (it->payload == NULL && it->len > 0u)) {
            continue;
        }
        if (!ChronosRT_et_esm_allows_traffic(state, (uint8_t)it->type, it->priority)) {
            continue;
        }

        uint32_t wire = ChronosRT_ET_ETH_HDR_SIZE + ChronosRT_ET_HEADER_SIZE + it->len;
        ChronosRT_status_t st = ChronosRT_et_qos_admit(master->qos, it->priority, wire, now);
        if (st != ChronosRT_SUCCESS) {
            if (st == ChronosRT_ERROR_DROPPED) {
                master->stats.dropped++;
            } else {
                master->stats.backpressure++;
            }
            continue;
        }

        ChronosRT_et_header_t h;
        et_fill_header(master, &h, it->priority, it->type, it->flow_id, state,
                       now, 1u, 0u);
        if (et_build_wire_header(master, hdrs[n], &h, it->payload, it->len) == 0u) {
            continue;
        }

        iovs[n][0].iov_base = hdrs[n];
        iovs[n][0].iov_len = ChronosRT_ET_ETH_HDR_SIZE + ChronosRT_ET_HEADER_SIZE;
        iovs[n][1].iov_base = (void*)it->payload;
        iovs[n][1].iov_len = it->len;

        memset(&msgs[n], 0, sizeof(msgs[n]));
        msgs[n].msg_hdr.msg_iov = iovs[n];
        msgs[n].msg_hdr.msg_iovlen = (it->len > 0u) ? 2u : 1u;

        bytes += wire;
        n++;
    }

    if (n == 0u) {
        return 0;
    }
    int sent = et_socket_send_batch(&master->port, msgs, n);
    if (sent < 0) {
        return -1;
    }
    master->stats.frames_tx += (uint64_t)sent;
    master->stats.bytes_tx += bytes;
    return sent;
}

/* ------------------------------------------------------------------ */
/* Recepción                                                           */
/* ------------------------------------------------------------------ */

int ChronosRT_et_master_recv(ChronosRT_et_master_t* master,
                             ChronosRT_et_header_t* hdr,
                             void* payload, uint32_t bufsize,
                             uint32_t timeout_us) {
    if (master == NULL || !master->is_open) {
        return -1;
    }

    /* Se leen hasta 8 tramas para descartar duplicados FRM sin bloquear. */
    for (unsigned attempt = 0; attempt < 8u; attempt++) {
        int n = et_socket_recv(&master->port, master->rx_frame,
                               ChronosRT_ET_MAX_FRAME,
                               (attempt == 0u) ? timeout_us : 0u);
        if (n < 0) {
            return -1;
        }

        uint32_t body_len = (uint32_t)n - ChronosRT_ET_ETH_HDR_SIZE;
        ChronosRT_et_header_t h;
        const uint8_t* pl = NULL;
        uint32_t used = ChronosRT_et_pdu_parse(master->rx_frame + ChronosRT_ET_ETH_HDR_SIZE,
                                               body_len, &h, &pl);
        if (used == 0u) {
            master->stats.frame_errors++;
            return -1;
        }

        uint32_t pl_len = used - ChronosRT_ET_HEADER_SIZE;
        if (pl_len > bufsize) {
            master->stats.frame_errors++;
            return -1;
        }

        const uint8_t* src_mac = master->rx_frame + 6;
        uint64_t now = ChronosRT_tx_timestamp_ns();
        int idx = et_master_touch_node(master, src_mac, now);
        uint16_t node_id = (idx >= 0) ? (uint16_t)(idx + 1) : 0u;

        master->last_wkc = h.wkc;

        /* FRM (§8.2): la segunda copia de un flujo crítico se descarta. */
        if (master->frm != NULL && node_id != 0u) {
            bool is_new = true;
            (void)ChronosRT_et_frm_note_rx(master->frm, node_id, h.seq_num,
                                           (h.flags & ChronosRT_ET_FLAG_FRM) != 0u,
                                           &is_new);
            if (!is_new) {
                continue;   /* duplicado: no se entrega a la aplicación */
            }
        }

        /* ET-WKC (§6.2): comparación con el valor esperado del registro. */
        if (master->topo != NULL) {
            if (ChronosRT_et_topology_on_frame(master->topo, src_mac, &h, now) ==
                ChronosRT_ERROR_CORRUPT) {
                master->stats.wkc_mismatches++;
            }
        }

        /* ET-Sync (§5): reporte de offset del nodo y clasificación. */
        if (master->sync != NULL && node_id != 0u &&
            h.type == (uint8_t)ChronosRT_ET_TYPE_SYNC_REPORT) {
            int64_t off = 0;
            if (ChronosRT_et_sync_parse_report(pl, pl_len, &off) == ChronosRT_SUCCESS) {
                ChronosRT_et_sync_status_t stt = ChronosRT_ET_SYNC_OK;
                (void)ChronosRT_et_sync_process_report(master->sync, node_id, off, now, &stt);
                if (stt == ChronosRT_ET_SYNC_OUT_OF_SYNC) {
                    master->stats.sync_out_of_sync++;
                }
            }
        }

        /* ET-MBX (§9): ACK/NACK del mailbox. */
        if (master->mbx != NULL && h.type == (uint8_t)ChronosRT_ET_TYPE_MAILBOX_ACK) {
            uint8_t ch = 0u;
            uint16_t seq = 0u;
            bool accepted = false;
            if (ChronosRT_et_mbx_parse_ack(pl, pl_len, &ch, &seq, &accepted) ==
                ChronosRT_SUCCESS) {
                (void)ChronosRT_et_mbx_on_ack(master->mbx, ch, seq, accepted, now);
            }
        }

        if (payload != NULL && pl_len > 0u) {
            memcpy(payload, pl, pl_len);
        }
        if (hdr != NULL) {
            *hdr = h;
        }

        master->stats.frames_rx++;
        master->stats.bytes_rx += (uint64_t)pl_len;
        return (int)pl_len;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Heartbeat, nodos y sincronización                                   */
/* ------------------------------------------------------------------ */

uint32_t ChronosRT_et_master_heartbeat(ChronosRT_et_master_t* master, uint64_t now_ns) {
    if (master == NULL) {
        return 0u;
    }

    /* Con registro de nodos asociado, él es la autoridad del ciclo de vida. */
    if (master->topo != NULL) {
        return ChronosRT_et_topology_tick(master->topo, now_ns, master->cycle_ns);
    }

    uint64_t limit = (uint64_t)master->cycle_ns * 3ull;
    uint32_t newly_lost = 0u;

    for (uint32_t i = 0; i < master->node_count; i++) {
        ChronosRT_et_node_t* n = &master->nodes[i];
        if (n->lost || n->last_seen_ns <= 0) {
            continue;
        }
        uint64_t seen = (uint64_t)n->last_seen_ns;
        if (now_ns > seen && (now_ns - seen) > limit) {
            n->lost = true;
            newly_lost++;
            if (master->sched != NULL) {
                (void)ChronosRT_et_sched_remove_node(master->sched, n->id);
            }
        }
    }
    return newly_lost;
}

uint32_t ChronosRT_et_master_node_count(const ChronosRT_et_master_t* master) {
    return (master != NULL) ? master->node_count : 0u;
}

bool ChronosRT_et_master_node(const ChronosRT_et_master_t* master, uint32_t index,
                              ChronosRT_et_node_t* out) {
    if (master == NULL || out == NULL || index >= master->node_count) {
        return false;
    }
    *out = master->nodes[index];
    return true;
}

ChronosRT_status_t ChronosRT_et_master_rtt_sync(ChronosRT_et_master_t* master,
                                                const uint8_t mac[6],
                                                uint32_t rtt_ns) {
    if (master == NULL || mac == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    for (uint32_t i = 0; i < master->node_count; i++) {
        if (memcmp(master->nodes[i].mac, mac, 6) == 0) {
            master->nodes[i].rtt_ns = rtt_ns;
            /* Hipótesis de simetría: el reloj del nodo va RTT/2 por detrás. */
            master->nodes[i].clock_offset_ns = -(int64_t)(rtt_ns / 2u);
            return ChronosRT_SUCCESS;
        }
    }
    return ChronosRT_ERROR_INVALID;
}

ChronosRT_status_t ChronosRT_et_master_stats(const ChronosRT_et_master_t* master,
                                             ChronosRT_et_master_stats_t* out) {
    if (master == NULL || out == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    *out = master->stats;
    return ChronosRT_SUCCESS;
}

void ChronosRT_et_master_reset_stats(ChronosRT_et_master_t* master) {
    if (master != NULL) {
        memset(&master->stats, 0, sizeof(master->stats));
    }
}
