/*
 * ec_frame.c — Serialización y parseo de tramas/datagramas EtherCAT.
 *
 * Capa puramente funcional (sin estado, sin E/S): construye datagramas,
 * los parsea y compone la cabecera Ethernet/EtherCAT. Es la base sobre la que
 * `ec_master.c` monta el intercambio y permite validar el protocolo sin
 * hardware.
 */
#include "ec_internal.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers públicos (API: parte testeable sin hardware)                */
/* ------------------------------------------------------------------ */

uint32_t ChronosRT_ec_datagram_build(uint8_t* out, uint32_t out_size,
                                     uint8_t cmd, uint8_t idx,
                                     uint32_t address,
                                     const void* data, uint16_t len,
                                     bool more) {
    if (!out || len > ChronosRT_EC_MAX_DATAGRAM_DATA) {
        return 0;
    }

    uint32_t total = EC_DGR_HDR_LEN + (uint32_t)len + EC_WKC_LEN;
    if (out_size < total) {
        return 0;
    }

    out[0] = cmd;
    out[1] = idx;
    out[2] = (uint8_t)(address & 0xFFu);
    out[3] = (uint8_t)((address >> 8) & 0xFFu);
    out[4] = (uint8_t)((address >> 16) & 0xFFu);
    out[5] = (uint8_t)((address >> 24) & 0xFFu);

    uint16_t dl = (uint16_t)(len & EC_LEN_MASK);
    if (more) {
        dl |= EC_MORE_BIT;
    }
    out[6] = (uint8_t)(dl & 0xFFu);
    out[7] = (uint8_t)(dl >> 8);

    out[8] = 0u;   /* IRQ */
    out[9] = 0u;

    uint8_t* payload = out + EC_DGR_HDR_LEN;
    if (len > 0u) {
        if (data) {
            memcpy(payload, data, len);
        } else {
            /* En comandos de lectura el maestro deja el área en un estado
             * conocido (esclava la sobrescribe). */
            memset(payload, 0, len);
        }
    }

    /* Contador de trabajo: lo incrementan las esclavas al procesar. */
    payload[len] = 0u;
    payload[len + 1u] = 0u;

    return total;
}

uint32_t ChronosRT_ec_datagram_parse(const uint8_t* in, uint32_t in_len,
                                     ChronosRT_ec_datagram_t* out) {
    if (!in || !out || in_len < EC_DGR_HDR_LEN + EC_WKC_LEN) {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->cmd = in[0];
    out->idx = in[1];
    out->adp = (uint16_t)(in[2] | ((uint16_t)in[3] << 8));
    out->ado = (uint16_t)(in[4] | ((uint16_t)in[5] << 8));
    out->address = (uint32_t)out->adp | ((uint32_t)out->ado << 16);

    uint16_t dl = (uint16_t)(in[6] | ((uint16_t)in[7] << 8));
    out->data_len = (uint16_t)(dl & EC_LEN_MASK);
    out->more = (dl & EC_MORE_BIT) != 0u;
    out->irq = (uint16_t)(in[8] | ((uint16_t)in[9] << 8));

    if (out->data_len > ChronosRT_EC_MAX_DATAGRAM_DATA) {
        return 0;
    }

    uint32_t total = EC_DGR_HDR_LEN + (uint32_t)out->data_len + EC_WKC_LEN;
    if (in_len < total) {
        return 0;
    }

    out->data = in + EC_DGR_HDR_LEN;
    out->working_counter = (uint16_t)(in[EC_DGR_HDR_LEN + out->data_len] |
                                      ((uint16_t)in[EC_DGR_HDR_LEN + out->data_len + 1u] << 8));
    out->next_offset = total;
    return total;
}

uint16_t ChronosRT_ec_frame_header(uint8_t out[2], uint16_t datagram_bytes) {
    uint16_t hdr = (uint16_t)((datagram_bytes & EC_LEN_MASK) |
                              (EC_TYPE_VAL << EC_TYPE_SHIFT));
    if (out) {
        out[0] = (uint8_t)(hdr & 0xFFu);
        out[1] = (uint8_t)(hdr >> 8);
    }
    return hdr;
}

/* ------------------------------------------------------------------ */
/* Composición de trama (uso interno)                                  */
/* ------------------------------------------------------------------ */

void ec_frame_begin(const uint8_t mac[6], uint8_t* frame) {
    memset(frame, 0xFF, 6);                 /* destino: difusión */
    memcpy(frame + 6, mac, 6);              /* origen            */
    frame[12] = (uint8_t)((ChronosRT_EC_ETHERTYPE >> 8) & 0xFFu);
    frame[13] = (uint8_t)(ChronosRT_EC_ETHERTYPE & 0xFFu);
}

uint32_t ec_frame_commit(uint8_t* frame, uint32_t datagram_bytes) {
    ChronosRT_ec_frame_header(frame + EC_ETH_HDR_LEN, (uint16_t)datagram_bytes);
    return EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN + datagram_bytes;
}
