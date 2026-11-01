/*
 * et_internal.h — Definiciones internas del protocolo EtherTux.
 *
 * NO es una cabecera pública: solo la usan los módulos de `src/protocols/ethertux/`.
 *
 * Organización de módulos (v0.1.0):
 *   et_frame.c    — serialización/parseo de la cabecera v0.1.0 (26 B) y la PDU (CRC-32C).
 *   et_fec.c      — gestión de bloques FEC sobre la HAL de Reed-Solomon.
 *   et_qos.c      — token buckets jerárquicos, backpressure y LAPM.
 *   et_sched.c    — planificador time-triggered con ventanas dedicadas.
 *   et_esm.c      — máquina de estados ET-ESM del nodo.
 *   et_sync.c     — ET-Sync (offset/drift) y Monitor de Ventana.
 *   et_topology.c — registro de nodos, ET-WKC, descubrimiento y Hot Connect.
 *   et_mbx.c      — mailbox ET-MBX (segmentación, ACK/NACK, reintentos).
 *   et_flowagg.c  — Flow Aggregator (varios flujos por trama).
 *   et_frm.c      — redundancia lógica de flujo (FRM) y deduplicación.
 *   et_socket.c   — socket AF_PACKET (0x88B5), pacing SO_TXTIME y recepción.
 *   et_master.c   — ciclo de vida, envío/recepción, nodos y heartbeat.
 */
#ifndef CHRONOSRT_ET_INTERNAL_H
#define CHRONOSRT_ET_INTERNAL_H

#include "chronosrt_ethertux.h"
#include "chronosrt_abi.h"

#include <stdint.h>
#include <stdbool.h>
#include <net/if.h>

/* Número máximo de ranuras del superframe. */
#define ET_MAX_SLOTS 64u

/* Ventanas del superframe (índices). */
#define ET_WINDOW_CONTROL       0u
#define ET_WINDOW_DETERMINISTIC 1u
#define ET_WINDOW_RATE          2u
#define ET_WINDOW_BEST_EFFORT   3u
#define ET_WINDOW_COUNT         4u

/* Umbrales fijos de backpressure consciente de prioridad (§5.3). */
#define ET_PAUSE_P1_AT 90u
#define ET_PAUSE_P2_AT 95u

/* ------------------------------------------------------------------ */
/* Puerto de red                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int      fd;
    uint8_t  mac[6];
    int      ifindex;
    char     ifname[IF_NAMESIZE];
    bool     txtime_enabled;      /* SO_TXTIME aceptado por la NIC       */
    uint64_t frames_tx;
    uint64_t frames_rx;
    uint64_t bytes_tx;
    uint64_t bytes_rx;

    /* Anillo de transmisión PACKET_TX_RING (0 si no está activo). */
    uint8_t* tx_ring;             /* mmap del anillo                     */
    size_t   tx_ring_bytes;
    uint32_t tx_frame_size;       /* tp_frame_size                       */
    uint32_t tx_frame_nr;         /* tramas en el anillo                 */
    uint32_t tx_head;             /* siguiente ranura candidata          */
    bool     tx_ring_active;
} et_port_t;

/* ------------------------------------------------------------------ */
/* QoS                                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t  tokens;      /* bytes disponibles                               */
    uint64_t last_ns;     /* instante del último reabastecimiento            */
} et_bucket_t;

struct ChronosRT_et_qos {
    ChronosRT_et_qos_config_t cfg;
    et_bucket_t               tb[ChronosRT_ET_PRIORITIES];
    uint8_t                   eff[ChronosRT_ET_PRIORITIES];  /* LAPM         */
    bool                      lapm_active;
    ChronosRT_et_qos_stats_t  stats;
};

/* ------------------------------------------------------------------ */
/* Planificador                                                        */
/* ------------------------------------------------------------------ */
struct ChronosRT_et_sched {
    uint32_t           cycle_ns;
    uint64_t           link_bps;
    uint32_t           window_ns[ET_WINDOW_COUNT];
    ChronosRT_et_slot_t slots[ET_MAX_SLOTS];
    uint32_t           slot_count;
};

/* ------------------------------------------------------------------ */
/* FEC                                                                 */
/* ------------------------------------------------------------------ */
struct ChronosRT_et_fec {
    uint8_t k;
    uint8_t n;
};

/* ------------------------------------------------------------------ */
/* Maestro                                                             */
/* ------------------------------------------------------------------ */
struct ChronosRT_et_master {
    et_port_t port;
    bool      is_open;

    uint64_t  link_bps;
    uint32_t  cycle_ns;

    ChronosRT_et_qos_t*   qos;    /* propio                              */
    ChronosRT_et_fec_t*   fec;    /* propio; NULL = sin FEC              */
    ChronosRT_et_sched_t* sched;  /* NO propio                           */

    /* Módulos v0.1.0 asociados (NO propios; NULL = no integrado). */
    ChronosRT_et_topology_t* topo;
    ChronosRT_et_sync_t*     sync;
    ChronosRT_et_esm_t*      esm;
    ChronosRT_et_mbx_t*      mbx;
    ChronosRT_et_frm_t*      frm;

    ChronosRT_et_node_t nodes[ChronosRT_ET_MAX_NODES];
    uint32_t            node_count;

    uint32_t seq[ChronosRT_ET_MAX_FLOWS + 1u];
    uint16_t block_seq;
    uint16_t last_wkc;

    uint8_t  tx_frame[ChronosRT_ET_MAX_FRAME];
    uint8_t  rx_frame[ChronosRT_ET_MAX_FRAME];
    uint8_t  aux[ChronosRT_ET_MAX_PAYLOAD];   /* cargas agregadas / mailbox */

    ChronosRT_et_master_stats_t stats;
};

/* ------------------------------------------------------------------ */
/* et_socket.c                                                         */
/* ------------------------------------------------------------------ */
ChronosRT_status_t et_socket_open(et_port_t* port, const char* ifname);
void     et_socket_close(et_port_t* port);
int      et_socket_send(et_port_t* port, const uint8_t* frame, uint32_t len);
int      et_socket_send_iov(et_port_t* port, const void* hdr, uint32_t hdr_len,
                            const void* payload, uint32_t payload_len);
int      et_socket_send_batch(et_port_t* port, struct mmsghdr* msgs, uint32_t count);
int      et_socket_send_at(et_port_t* port, const uint8_t* frame, uint32_t len,
                           uint64_t deadline_ns);
int      et_socket_recv(et_port_t* port, uint8_t* frame, uint32_t bufsize,
                        uint32_t timeout_us);
bool     et_socket_txtime_enabled(const et_port_t* port);
/** Velocidad del enlace en bps desde sysfs; 0 si no se puede leer. */
uint64_t et_socket_link_bps(const char* ifname);

/* Anillo de transmisión sin llamada al sistema por trama (PACKET_TX_RING). */
bool     et_socket_tx_ring_enable(et_port_t* port, uint32_t frames);
void     et_socket_tx_ring_disable(et_port_t* port);
bool     et_socket_tx_ring_active(const et_port_t* port);
int      et_socket_tx_ring_kick(et_port_t* port);
int      et_socket_tx_ring_put(et_port_t* port, const uint8_t* frame, uint32_t len);
/** Ranuras del anillo (0 si el anillo no está activo). */
int      et_socket_tx_ring_space(const et_port_t* port);

/* ------------------------------------------------------------------ */
/* et_master.c                                                         */
/* ------------------------------------------------------------------ */

/** Registra/refresca el nodo identificado por su MAC. Devuelve su índice. */
int et_master_touch_node(ChronosRT_et_master_t* master, const uint8_t mac[6],
                         uint64_t now_ns);

#endif /* CHRONOSRT_ET_INTERNAL_H */
