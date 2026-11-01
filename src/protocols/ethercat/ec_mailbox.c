/*
 * ec_mailbox.c — Buzón (SM0/SM1) y CoE (SDO upload/download).
 *
 * Implementa el transporte de buzón definido en ETG.1000 (cabecera de 6 bytes:
 * longitud, dirección, canal/prioridad y tipo) y el servicio CoE para leer y
 * escribir objetos del diccionario de objetos (OD) de las esclavas, en modo
 * expedito, no segmentado y segmentado.
 *
 * Disposición del buzón CoE (offsets en bytes del marco de buzón):
 *   [0..1] longitud   [2..3] dirección   [4] canal/prioridad   [5] tipo (0x03)
 *   [6..7] CoE number [8] CoE service     [9] reservado
 *   [10..11] SDO index [12] subindex      [13] flags / acceso completo
 *   [14..]  datos SDO
 */
#include "ec_internal.h"

#include <string.h>

#define EC_MBX_OFF_LEN    0u
#define EC_MBX_OFF_ADDR   2u
#define EC_MBX_OFF_PRIO   4u
#define EC_MBX_OFF_TYPE   5u
#define EC_MBX_OFF_COE    6u
#define EC_MBX_OFF_SVC    8u
#define EC_MBX_OFF_RES    9u
#define EC_MBX_OFF_IDX   10u
#define EC_MBX_OFF_SUB   12u
#define EC_MBX_OFF_FLG   13u
#define EC_MBX_OFF_DATA  14u

/* Flags del byte de control del SDO. */
#define EC_SDO_FLAG_SIZE  0x01u   /* tamaño indicado      */
#define EC_SDO_FLAG_EXP   0x02u   /* transferencia expedita */
#define EC_SDO_FLAG_N     0x0Cu   /* bytes no usados (expedita) */

/* Servicios del protocolo ARQ para transferencias segmentadas. */
#define EC_SDO_SEG_UP     0x60u
#define EC_SDO_SEG_DOWN   0x0Bu
#define EC_SDO_SEG_MORE   0x01u   /* quedan segmentos   */
#define EC_SDO_SEG_TOGGLE 0x10u   /* alternancia        */

static uint16_t ec_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ec_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ec_put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void ec_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Transporte de buzón                                                 */
/* ------------------------------------------------------------------ */

int ec_mbx_send(ChronosRT_ec_master_t* master, int slave, const uint8_t* mbx, uint16_t len) {
    if (!master || slave < 1 || slave > master->slave_count || !mbx ||
        len < EC_MBX_HDR_LEN || len > ChronosRT_EC_MAX_MAILBOX) {
        return -1;
    }
    ec_slave_t* s = &master->slaves[slave];
    if (s->cfg.mbx_wr == 0u) {
        return -1;
    }

    /* Espera a que el buzón de salida quede libre (bit "full" del SM0). */
    uint16_t sm0_status = (uint16_t)(EC_REG_SM0 + EC_SM_STATUS_OFF);
    uint64_t deadline = ec_now_us() + EC_TIMEOUT_MBX;
    for (;;) {
        uint8_t st = 0;
        if (ChronosRT_ec_master_read_reg(master, (uint16_t)slave, sm0_status, &st, 1) == 1 &&
            (st & EC_SM_STATUS_FULL) == 0u) {
            break;
        }
        if (ec_now_us() >= deadline) {
            return -1;
        }
    }

    return (ChronosRT_ec_master_fp_write(master, s->pub.config_addr, s->cfg.mbx_wr,
                                         mbx, len) == (int)len)
               ? 0 : -1;
}

int ec_mbx_receive(ChronosRT_ec_master_t* master, int slave,
                   uint8_t* mbx, uint16_t bufsize, uint32_t timeout_us) {
    if (!master || slave < 1 || slave > master->slave_count || !mbx ||
        bufsize < EC_MBX_HDR_LEN) {
        return -1;
    }
    ec_slave_t* s = &master->slaves[slave];
    if (s->cfg.mbx_rr == 0u) {
        return -1;
    }

    uint16_t sm1_status = (uint16_t)(EC_REG_SM0 + EC_SM_LEN + EC_SM_STATUS_OFF);
    uint16_t read_len = (s->cfg.mbx_rl != 0u) ? s->cfg.mbx_rl : ChronosRT_EC_MAX_MAILBOX;
    if (read_len > bufsize) {
        read_len = bufsize;
    }

    uint64_t deadline = ec_now_us() + (uint64_t)timeout_us;
    for (;;) {
        uint8_t st = 0;
        if (ChronosRT_ec_master_read_reg(master, (uint16_t)slave, sm1_status, &st, 1) == 1 &&
            (st & EC_SM_STATUS_FULL) != 0u) {
            if (ChronosRT_ec_master_fp_read(master, s->pub.config_addr, s->cfg.mbx_rr,
                                            mbx, read_len) < 0) {
                return -1;
            }
            uint32_t total = (uint32_t)EC_MBX_HDR_LEN + ec_le16(mbx + EC_MBX_OFF_LEN);
            if (total > bufsize) {
                total = bufsize;
            }
            return (int)total;
        }
        if (ec_now_us() >= deadline) {
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* CoE: lectura (upload)                                               */
/* ------------------------------------------------------------------ */

/* Envía una petición CoE/SDO y devuelve la respuesta en `resp`. */
static int ec_sdo_transaction(ChronosRT_ec_master_t* master, int slave,
                              uint8_t service, uint16_t index, uint8_t subindex,
                              uint8_t flags, const uint8_t* data, uint16_t data_len,
                              uint8_t* resp, uint16_t resp_size, uint32_t timeout_us) {
    uint8_t req[ChronosRT_EC_MAX_MAILBOX];
    memset(req, 0, sizeof(req));

    uint16_t sdo_len = 3u + data_len;           /* index + subindex + flags + datos */
    uint16_t svc_len = 4u + sdo_len;            /* + cabecera CoE                   */
    ec_put16(req + EC_MBX_OFF_LEN, svc_len);
    ec_put16(req + EC_MBX_OFF_ADDR, 0u);
    req[EC_MBX_OFF_PRIO] = (uint8_t)((master->slaves[slave].cfg.mbx_cnt & 0x07u) << 4);
    req[EC_MBX_OFF_TYPE] = EC_MBXTYPE_COE;
    ec_put16(req + EC_MBX_OFF_COE, 0u);
    req[EC_MBX_OFF_SVC] = service;
    ec_put16(req + EC_MBX_OFF_IDX, index);
    req[EC_MBX_OFF_SUB] = subindex;
    req[EC_MBX_OFF_FLG] = flags;
    if (data && data_len > 0u) {
        memcpy(req + EC_MBX_OFF_DATA, data, data_len);
    }

    uint16_t frame_len = (uint16_t)(EC_MBX_HDR_LEN + svc_len);
    if (ec_mbx_send(master, slave, req, frame_len) < 0) {
        return -1;
    }
    if (ec_mbx_receive(master, slave, resp, resp_size, timeout_us) < 0) {
        return -1;
    }
    if ((resp[EC_MBX_OFF_TYPE] & EC_MBXTYPE_MASK) != EC_MBXTYPE_COE) {
        return -1;
    }
    return 0;
}

ChronosRT_status_t ChronosRT_ec_master_sdo_read(ChronosRT_ec_master_t* master,
                                                uint32_t slave_index,
                                                uint16_t index, uint8_t subindex,
                                                void* out, uint32_t* size,
                                                uint32_t timeout_us) {
    if (!master || !out || !size || slave_index < 1u ||
        slave_index > (uint32_t)master->slave_count) {
        return ChronosRT_ERROR_INVALID;
    }
    int slave = (int)slave_index;
    uint8_t resp[ChronosRT_EC_MAX_MAILBOX];
    uint32_t cap = *size;

    if (ec_sdo_transaction(master, slave, EC_SDO_UP_REQ, index, subindex, 0u,
                           NULL, 0u, resp, sizeof(resp), timeout_us) < 0) {
        return ChronosRT_ERROR_IO;
    }
    uint8_t svc = resp[EC_MBX_OFF_SVC];
    if ((svc & 0xE0u) == EC_SDO_ABORT) {
        return ChronosRT_ERROR_CORRUPT;
    }
    if ((svc & 0x0Fu) != EC_COE_SDORES) {
        return ChronosRT_ERROR_IO;
    }

    uint8_t flg = resp[EC_MBX_OFF_FLG];
    uint32_t total;

    if ((flg & EC_SDO_FLAG_EXP) != 0u) {
        /* Transferencia expedita: los datos viven en el propio marco. */
        total = 4u - ((uint32_t)(flg & EC_SDO_FLAG_N) >> 2);
        if (total > cap) {
            total = cap;
        }
        memcpy(out, resp + EC_MBX_OFF_DATA, total);
        *size = total;
        return ChronosRT_SUCCESS;
    }

    if ((flg & EC_SDO_FLAG_SIZE) != 0u) {
        /* Tamaño indicado en los 4 bytes de datos. */
        total = ec_le32(resp + EC_MBX_OFF_DATA);
    } else {
        total = 0u;
    }

    uint32_t copied = 0u;
    if (total <= 4u && total > 0u) {
        copied = (total > cap) ? cap : total;
        memcpy(out, resp + (EC_MBX_OFF_DATA + 4u), copied);
        *size = copied;
        return ChronosRT_SUCCESS;
    }

    /* Transferencia segmentada. */
    uint8_t toggle = 0u;
    while (copied < total && copied < cap) {
        uint8_t sflg = (uint8_t)(EC_SDO_SEG_UP | (toggle ? EC_SDO_SEG_TOGGLE : 0u));
        if (ec_sdo_transaction(master, slave, EC_SDO_UP_SEG_REQ, index, subindex,
                               sflg, NULL, 0u, resp, sizeof(resp), timeout_us) < 0) {
            return ChronosRT_ERROR_IO;
        }
        uint8_t rsvc = resp[EC_MBX_OFF_SVC];
        if ((rsvc & (EC_SDO_SEG_MORE | EC_SDO_SEG_TOGGLE)) == EC_SDO_SEG_MORE) {
            /* Fin de la secuencia. */
        }
        uint32_t chunk = 7u;
        if (chunk > (total - copied)) {
            chunk = total - copied;
        }
        if (chunk > (cap - copied)) {
            chunk = cap - copied;
        }
        memcpy((uint8_t*)out + copied, resp + EC_MBX_OFF_DATA, chunk);
        copied += chunk;
        toggle ^= 1u;
        if ((rsvc & EC_SDO_SEG_MORE) == 0u) {
            break;
        }
    }

    *size = copied;
    return (copied == total) ? ChronosRT_SUCCESS : ChronosRT_ERROR_IO;
}

/* ------------------------------------------------------------------ */
/* CoE: escritura (download)                                           */
/* ------------------------------------------------------------------ */

ChronosRT_status_t ChronosRT_ec_master_sdo_write(ChronosRT_ec_master_t* master,
                                                 uint32_t slave_index,
                                                 uint16_t index, uint8_t subindex,
                                                 const void* in, uint32_t size,
                                                 uint32_t timeout_us) {
    if (!master || !in || slave_index < 1u ||
        slave_index > (uint32_t)master->slave_count) {
        return ChronosRT_ERROR_INVALID;
    }
    int slave = (int)slave_index;
    const uint8_t* src = (const uint8_t*)in;
    uint8_t resp[ChronosRT_EC_MAX_MAILBOX];

    uint8_t service;
    uint8_t flags;
    uint8_t first[4];
    uint32_t first_len;

    if (size <= 4u) {
        /* Descarga expedita. */
        service = EC_SDO_DOWN_EXP;
        flags = (uint8_t)(EC_SDO_FLAG_SIZE | EC_SDO_FLAG_EXP |
                          (uint8_t)(((4u - size) & 0x03u) << 2));
        memset(first, 0, sizeof(first));
        memcpy(first, src, size);
        first_len = 4u;
    } else {
        /* Descarga iniciada, con tamaño, seguida de segmentos. */
        service = EC_SDO_DOWN_INIT;
        flags = EC_SDO_FLAG_SIZE;
        ec_put32(first, size);
        first_len = 4u;
    }

    if (ec_sdo_transaction(master, slave, service, index, subindex, flags,
                           first, (uint16_t)first_len, resp, sizeof(resp), timeout_us) < 0) {
        return ChronosRT_ERROR_IO;
    }
    if ((resp[EC_MBX_OFF_SVC] & 0xE0u) == EC_SDO_ABORT) {
        return ChronosRT_ERROR_CORRUPT;
    }
    if (size <= 4u) {
        return ChronosRT_SUCCESS;
    }

    /* Secuencia de segmentos. */
    uint8_t toggle = 0u;
    uint32_t sent = 0u;
    while (sent < size) {
        uint8_t seg[7];
        uint32_t chunk = size - sent;
        bool more = true;
        if (chunk > 7u) {
            chunk = 7u;
        } else {
            more = false;
        }
        memset(seg, 0, sizeof(seg));
        memcpy(seg, src + sent, chunk);
        uint8_t sflg = (uint8_t)((more ? EC_SDO_SEG_MORE : 0u) |
                                 (toggle ? EC_SDO_SEG_TOGGLE : 0u));
        if (ec_sdo_transaction(master, slave, EC_SDO_SEG_DOWN, index, subindex,
                               sflg, seg, 7u, resp, sizeof(resp), timeout_us) < 0) {
            return ChronosRT_ERROR_IO;
        }
        sent += chunk;
        toggle ^= 1u;
    }
    return ChronosRT_SUCCESS;
}
