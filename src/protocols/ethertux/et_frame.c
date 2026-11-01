/*
 * et_frame.c — Serialización y parseo de la PDU EtherTux (v0.1.0).
 *
 * Cabecera en el cable (26 B, little-endian):
 *
 *   +0  Type          1 B   (0x01–0x0C, §4.3)
 *   +1  Priority      1 B   (P0–P5)
 *   +2  Flags         1 B   (FEC | Preemptable | Express | Discovery | FRM)
 *   +3  State         1 B   (estado ET-ESM del emisor)
 *   +4  FlowID        1 B
 *   +5  SeqNum        4 B
 *   +9  TxTimestamp   8 B
 *   +17 ET-WKC        2 B   (nodos que procesaron la trama)
 *   +19 BlockID       2 B   ((bloque << 8) | índice dentro del bloque)
 *   +21 FlowCount     1 B   (1 = sin agregación)
 *   +22 CRC32         4 B   (CRC-32C de cabecera + carga útil)
 *
 * Nota de trazabilidad: la especificación §4.2 rotula la cabecera como "28 B",
 * pero la suma de los tamaños de sus campos es 26 B. Se implementa la suma de
 * los campos (la tabla es la fuente de verdad), igual que se hizo con el
 * erratum 20/22 de la versión anterior.
 *
 * El CRC se calcula con el propio campo a cero. Es CRC-32C (Castagnoli) porque
 * es el único polinomio con aceleración por instrucción tanto en ARMv8.1+
 * (crc32c*) como en x86-64 (SSE4.2), garantizando el mismo valor en todos los
 * nodos con el máximo rendimiento.
 */
#include "et_internal.h"

#include <string.h>

/* Offsets de los campos en el cable. */
#define ET_OFF_TYPE        0u
#define ET_OFF_PRIORITY    1u
#define ET_OFF_FLAGS       2u
#define ET_OFF_STATE       3u
#define ET_OFF_FLOW_ID     4u
#define ET_OFF_SEQ         5u
#define ET_OFF_TX_TS       9u
#define ET_OFF_WKC         17u
#define ET_OFF_BLOCK_ID    19u
#define ET_OFF_FLOW_COUNT  21u
#define ET_OFF_CRC         22u

static inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void put_le64(uint8_t* p, uint64_t v) {
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t get_le64(const uint8_t* p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

uint32_t ChronosRT_et_header_build(uint8_t out[ChronosRT_ET_HEADER_SIZE],
                                   const ChronosRT_et_header_t* h) {
    if (out == NULL || h == NULL) {
        return 0u;
    }
    out[ET_OFF_TYPE]       = h->type;
    out[ET_OFF_PRIORITY]   = h->priority;
    out[ET_OFF_FLAGS]      = h->flags;
    out[ET_OFF_STATE]      = h->state;
    out[ET_OFF_FLOW_ID]    = h->flow_id;
    put_le32(out + ET_OFF_SEQ, h->seq_num);
    put_le64(out + ET_OFF_TX_TS, h->tx_timestamp);
    put_le16(out + ET_OFF_WKC, h->wkc);
    put_le16(out + ET_OFF_BLOCK_ID, h->block_id);
    out[ET_OFF_FLOW_COUNT] = h->flow_count;
    put_le32(out + ET_OFF_CRC, h->crc32);
    return ChronosRT_ET_HEADER_SIZE;
}

uint32_t ChronosRT_et_crc(const ChronosRT_et_header_t* h,
                          const void* payload, uint16_t len) {
    if (h == NULL) {
        return 0u;
    }

    ChronosRT_et_header_t copy = *h;
    copy.crc32 = 0u;

    uint8_t hdr[ChronosRT_ET_HEADER_SIZE];
    (void)ChronosRT_et_header_build(hdr, &copy);

    /* CRC-32C encadenado: cabecera (crc a cero) + carga útil. */
    uint32_t c = ChronosRT_crc32c_raw(0xFFFFFFFFu, hdr, sizeof(hdr));
    if (payload != NULL && len > 0u) {
        c = ChronosRT_crc32c_raw(c, payload, len);
    }
    return ~c;
}

uint32_t ChronosRT_et_pdu_build(uint8_t* out, uint32_t out_size,
                                const ChronosRT_et_header_t* h,
                                const void* payload, uint16_t len) {
    if (out == NULL || h == NULL || len > ChronosRT_ET_MAX_PAYLOAD) {
        return 0u;
    }
    if (payload == NULL && len > 0u) {
        return 0u;
    }
    if (h->priority == (uint8_t)ChronosRT_ET_P0 &&
        (ChronosRT_ET_HEADER_SIZE + (uint32_t)len) > ChronosRT_ET_P0_MAX_FRAME) {
        return 0u;   /* las tramas P0 se limitan a 64 B (§4.2) */
    }
    uint32_t total = ChronosRT_ET_HEADER_SIZE + (uint32_t)len;
    if (out_size < total) {
        return 0u;
    }

    ChronosRT_et_header_t tmp = *h;
    tmp.crc32 = 0u;
    if (tmp.flow_count == 0u) {
        tmp.flow_count = 1u;   /* 0 = sin agregación (cabecera zero-initialized) */
    }
    (void)ChronosRT_et_header_build(out, &tmp);
    if (payload != NULL && len > 0u) {
        memcpy(out + ChronosRT_ET_HEADER_SIZE, payload, len);
    }

    uint32_t crc = ChronosRT_et_crc(&tmp, out + ChronosRT_ET_HEADER_SIZE, len);
    put_le32(out + ET_OFF_CRC, crc);
    return total;
}

uint32_t ChronosRT_et_pdu_build_header(uint8_t out[ChronosRT_ET_HEADER_SIZE],
                                       const ChronosRT_et_header_t* h,
                                       const void* payload, uint16_t len) {
    if (out == NULL || h == NULL || len > ChronosRT_ET_MAX_PAYLOAD) {
        return 0u;
    }
    if (payload == NULL && len > 0u) {
        return 0u;
    }
    if (h->priority == (uint8_t)ChronosRT_ET_P0 &&
        (ChronosRT_ET_HEADER_SIZE + (uint32_t)len) > ChronosRT_ET_P0_MAX_FRAME) {
        return 0u;
    }

    ChronosRT_et_header_t tmp = *h;
    tmp.crc32 = 0u;
    if (tmp.flow_count == 0u) {
        tmp.flow_count = 1u;
    }
    (void)ChronosRT_et_header_build(out, &tmp);

    /* CRC sobre cabecera (crc a cero) + carga útil, sin copiar la carga. */
    uint32_t c = ChronosRT_crc32c_raw(0xFFFFFFFFu, out, ChronosRT_ET_HEADER_SIZE);
    if (payload != NULL && len > 0u) {
        c = ChronosRT_crc32c_raw(c, payload, len);
    }
    put_le32(out + ET_OFF_CRC, ~c);
    return ChronosRT_ET_HEADER_SIZE;
}

/* ================================================================== */
/* Tasa de trama alcanzable                                           */
/* ================================================================== */

/* 14 (Ethernet) + 26 (EtherTux) + 4 (FCS) + 20 (preámbulo + IFG). */
#define ET_WIRE_FIXED 64u

uint32_t ChronosRT_et_wire_frame_bytes(uint32_t payload_len) {
    return payload_len + ET_WIRE_FIXED;
}

uint32_t ChronosRT_et_wire_pps(uint64_t link_bps, uint32_t payload_len) {
    if (link_bps == 0u) {
        return 0u;
    }
    uint64_t bits = (uint64_t)ChronosRT_et_wire_frame_bytes(payload_len) * 8ull;
    if (bits == 0u) {
        return 0u;
    }
    uint64_t pps = link_bps / bits;
    return (pps > UINT32_MAX) ? UINT32_MAX : (uint32_t)pps;
}

uint32_t ChronosRT_et_payload_for_pps(uint64_t link_bps, uint32_t pps) {
    if (link_bps == 0u || pps == 0u) {
        return 0u;
    }
    uint64_t bytes = (link_bps / (uint64_t)pps) / 8ull;
    if (bytes <= ET_WIRE_FIXED) {
        return 0u;
    }
    uint64_t payload = bytes - ET_WIRE_FIXED;
    if (payload > (uint64_t)ChronosRT_ET_MAX_PAYLOAD) {
        payload = ChronosRT_ET_MAX_PAYLOAD;
    }
    return (uint32_t)payload;
}

uint32_t ChronosRT_et_pdu_parse(const uint8_t* in, uint32_t in_len,
                                ChronosRT_et_header_t* h,
                                const uint8_t** payload) {
    if (in == NULL || in_len < ChronosRT_ET_HEADER_SIZE) {
        return 0u;
    }

    ChronosRT_et_header_t tmp;
    tmp.type         = in[ET_OFF_TYPE];
    tmp.priority     = in[ET_OFF_PRIORITY];
    tmp.flags        = in[ET_OFF_FLAGS];
    tmp.state        = in[ET_OFF_STATE];
    tmp.flow_id      = in[ET_OFF_FLOW_ID];
    tmp.seq_num      = get_le32(in + ET_OFF_SEQ);
    tmp.tx_timestamp = get_le64(in + ET_OFF_TX_TS);
    tmp.wkc          = get_le16(in + ET_OFF_WKC);
    tmp.block_id     = get_le16(in + ET_OFF_BLOCK_ID);
    tmp.flow_count   = in[ET_OFF_FLOW_COUNT];
    tmp.crc32        = get_le32(in + ET_OFF_CRC);

    if (tmp.flow_count == 0u) {
        return 0u;   /* FlowCount debe ser >= 1 */
    }
    if (tmp.state > (uint8_t)ChronosRT_ET_STATE_FAULT) {
        return 0u;   /* estado ET-ESM desconocido */
    }

    uint32_t body = in_len - ChronosRT_ET_HEADER_SIZE;
    if (body > ChronosRT_ET_MAX_PAYLOAD) {
        return 0u;
    }

    /* El CRC cubre la cabecera con el campo crc a cero + la carga útil. */
    static const uint8_t zero4[4] = { 0u, 0u, 0u, 0u };
    uint32_t c = ChronosRT_crc32c_raw(0xFFFFFFFFu, in, ET_OFF_CRC);
    c = ChronosRT_crc32c_raw(c, zero4, sizeof(zero4));
    if (body > 0u) {
        c = ChronosRT_crc32c_raw(c, in + ChronosRT_ET_HEADER_SIZE, body);
    }
    if (~c != tmp.crc32) {
        return 0u;   /* trama corrupta */
    }

    if (h != NULL) {
        *h = tmp;
    }
    if (payload != NULL) {
        *payload = in + ChronosRT_ET_HEADER_SIZE;
    }
    return in_len;
}

const char* ChronosRT_et_type_name(ChronosRT_et_type_t type) {
    switch (type) {
        case ChronosRT_ET_TYPE_DATA:            return "DATA";
        case ChronosRT_ET_TYPE_FEC:             return "FEC";
        case ChronosRT_ET_TYPE_CONTROL:         return "CONTROL";
        case ChronosRT_ET_TYPE_SYNC:            return "SYNC";
        case ChronosRT_ET_TYPE_LAPM:            return "LAPM";
        case ChronosRT_ET_TYPE_DISCOVERY:       return "DISCOVERY";
        case ChronosRT_ET_TYPE_SYNC_CALIBRATED: return "SYNC_CALIBRATED";
        case ChronosRT_ET_TYPE_STATE_CHANGE:    return "STATE_CHANGE";
        case ChronosRT_ET_TYPE_MAILBOX_DATA:    return "MAILBOX_DATA";
        case ChronosRT_ET_TYPE_MAILBOX_ACK:     return "MAILBOX_ACK";
        case ChronosRT_ET_TYPE_SYNC_REPORT:     return "SYNC_REPORT";
        case ChronosRT_ET_TYPE_TOPO_CHECK:      return "TOPO_CHECK";
        default:                                return "UNKNOWN";
    }
}
