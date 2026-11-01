/*
 * et_flowagg.c — Flow Aggregator (§7.6).
 *
 * Cuando hay muchos nodos con pocos datos (típico en telemetría P3), agrupar
 * varios flujos en una sola trama EtherTux reduce el overhead de cabecera. La
 * carga útil pasa a ser una lista de entradas:
 *
 *   [FlowID 1B][longitud 2B LE][datos de longitud B] ...
 *
 * El campo `FlowCount` de la cabecera indica cuántas entradas contiene. Una
 * trama normal lleva `FlowCount = 1` y carga útil opaca.
 */
#include "et_internal.h"

#include <string.h>

#define ET_AGG_ENTRY_HEADER 3u

static inline void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

ChronosRT_status_t ChronosRT_et_flowagg_build(const ChronosRT_et_flow_entry_t* entries,
                                              uint32_t count,
                                              uint8_t* out, uint32_t out_size,
                                              uint32_t* out_len) {
    if (entries == NULL || out == NULL || out_len == NULL ||
        count == 0u || count > UINT8_MAX) {
        return ChronosRT_ERROR_INVALID;
    }

    uint32_t total = 0u;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].flow_id == 0u) {
            return ChronosRT_ERROR_INVALID;
        }
        if (entries[i].data == NULL && entries[i].len > 0u) {
            return ChronosRT_ERROR_INVALID;
        }
        total += ET_AGG_ENTRY_HEADER + (uint32_t)entries[i].len;
        if (total > out_size) {
            return ChronosRT_ERROR_FULL;
        }
    }

    uint32_t off = 0u;
    for (uint32_t i = 0; i < count; i++) {
        out[off] = entries[i].flow_id;
        put_le16(out + off + 1u, entries[i].len);
        off += ET_AGG_ENTRY_HEADER;
        if (entries[i].len > 0u) {
            memcpy(out + off, entries[i].data, entries[i].len);
            off += entries[i].len;
        }
    }
    *out_len = off;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_flowagg_parse(const uint8_t* in, uint32_t len,
                                              ChronosRT_et_flow_entry_t* views,
                                              uint32_t max_views, uint32_t* out_count) {
    if (in == NULL || views == NULL || out_count == NULL || max_views == 0u) {
        return ChronosRT_ERROR_INVALID;
    }

    uint32_t off = 0u;
    uint32_t n = 0u;
    while (off < len) {
        if (len - off < ET_AGG_ENTRY_HEADER) {
            return ChronosRT_ERROR_CORRUPT;     /* entrada truncada */
        }
        uint8_t flow_id = in[off];
        uint16_t flen = get_le16(in + off + 1u);
        off += ET_AGG_ENTRY_HEADER;

        if (flow_id == 0u || (uint32_t)flen > (len - off)) {
            return ChronosRT_ERROR_CORRUPT;
        }
        if (n >= max_views) {
            return ChronosRT_ERROR_FULL;
        }
        views[n].flow_id = flow_id;
        views[n].len = flen;
        views[n].data = in + off;
        n++;
        off += flen;
    }

    *out_count = n;
    return (n > 0u) ? ChronosRT_SUCCESS : ChronosRT_ERROR_CORRUPT;
}

uint32_t ChronosRT_et_flowagg_saved_bytes(uint32_t count, uint32_t data_bytes) {
    (void)data_bytes;   /* los datos no cambian; solo la cabecera */
    if (count < 2u) {
        return 0u;
    }
    const uint32_t per_frame = ChronosRT_ET_ETH_HDR_SIZE + ChronosRT_ET_HEADER_SIZE;
    uint32_t without = count * per_frame;
    uint32_t with = per_frame + count * ET_AGG_ENTRY_HEADER;
    return (without > with) ? (without - with) : 0u;
}
