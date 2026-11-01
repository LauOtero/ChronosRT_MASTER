/*
 * ec_slave.c — Escaneo del bus, configuración de esclavas (SII/SM/FMMU),
 * mapa de proceso (IOMap) y máquina de estados AL.
 *
 * Convención de Sync Managers (ETG.1000, topología estándar):
 *   SM0 → buzón de salida (escritura del maestro)
 *   SM1 → buzón de entrada (lectura del maestro)
 *   SM2 → salidas de proceso (RxPDO)
 *   SM3 → entradas de proceso (TxPDO)
 *
 * Los accesos a registros usan direccionamiento por posición (auto-incremento),
 * válido antes y después de asignar las direcciones de estación.
 */
#include "ec_internal.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Utilidades little-endian                                            */
/* ------------------------------------------------------------------ */

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
/* EEPROM / SII                                                        */
/* ------------------------------------------------------------------ */

static int ec_eep_read(ChronosRT_ec_master_t* master, int index, uint16_t word, uint16_t* out) {
    uint8_t reg[2];
    uint64_t deadline = ec_now_us() + EC_TIMEOUT_EEP;

    /* Dirección de palabra. */
    ec_put16(reg, word);
    if (ChronosRT_ec_master_write_reg(master, (uint16_t)index, EC_REG_EEPADR, reg, 2) < 0) {
        return -1;
    }

    /* Orden de lectura. */
    ec_put16(reg, EC_ECMD_READ);
    if (ChronosRT_ec_master_write_reg(master, (uint16_t)index, EC_REG_EEPCTL, reg, 2) < 0) {
        return -1;
    }

    /* Espera la finalización (BUSY) con timeout acotado. */
    uint16_t st = 0;
    for (;;) {
        if (ChronosRT_ec_master_read_reg(master, (uint16_t)index, EC_REG_EEPSTAT, reg, 2) == 2) {
            st = ec_le16(reg);
            if ((st & EC_EEPSTAT_BUSY) == 0u) {
                break;
            }
        }
        if (ec_now_us() >= deadline) {
            return -1;
        }
    }

    if ((st & (EC_EEPSTAT_ACKERR | EC_EEPSTAT_RDERR)) != 0u) {
        return -1;
    }

    if (ChronosRT_ec_master_read_reg(master, (uint16_t)index, EC_REG_EEPDAT, reg, 2) != 2) {
        return -1;
    }
    *out = ec_le16(reg);
    return 0;
}

int ec_slave_read_sii(ChronosRT_ec_master_t* master, int index,
                      uint16_t word, uint8_t* out, int words) {
    if (!master || !out || words <= 0) {
        return -1;
    }
    for (int i = 0; i < words; i++) {
        uint16_t v = 0;
        if (ec_eep_read(master, index, (uint16_t)(word + i), &v) < 0) {
            return -1;
        }
        ec_put16(out + ((size_t)i * 2u), v);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sync Managers y FMMU                                                */
/* ------------------------------------------------------------------ */

int ec_slave_write_sm(ChronosRT_ec_master_t* master, int index, int sm,
                      uint16_t start, uint16_t len, uint32_t flags) {
    if (!master || sm < 0 || sm >= ChronosRT_EC_MAX_SM) {
        return -1;
    }
    uint16_t base = (uint16_t)(EC_REG_SM0 + (uint16_t)(sm * (int)EC_SM_LEN));
    uint8_t v[2];

    ec_put16(v, start);
    if (ChronosRT_ec_master_write_reg(master, (uint16_t)index, base, v, 2) < 0) {
        return -1;
    }
    ec_put16(v, len);
    if (ChronosRT_ec_master_write_reg(master, (uint16_t)index, (uint16_t)(base + 2u), v, 2) < 0) {
        return -1;
    }
    /* Control: modo de operación (nibble bajo) + habilitación. */
    uint8_t ctl = (uint8_t)((flags & 0x0Fu) | EC_SMFLAG_ENABLE);
    if (ChronosRT_ec_master_write_reg(master, (uint16_t)index, (uint16_t)(base + 4u), &ctl, 1) < 0) {
        return -1;
    }
    /* Activación. */
    uint8_t act = 0x01u;
    return (ChronosRT_ec_master_write_reg(master, (uint16_t)index,
                                          (uint16_t)(base + EC_SM_ACTIVATE_OFF), &act, 1) == 1)
               ? 0 : -1;
}

int ec_slave_write_fmmu(ChronosRT_ec_master_t* master, int index, int fmmu,
                        uint32_t log_start, uint16_t len, uint16_t phys_start, uint8_t type) {
    if (!master || fmmu < 0 || fmmu >= ChronosRT_EC_MAX_FMMU || len == 0u) {
        return -1;
    }
    uint8_t f[EC_FMMU_LEN];
    memset(f, 0, sizeof(f));

    ec_put32(f + 0, log_start);          /* dirección lógica de inicio */
    ec_put16(f + 4, len);                /* longitud en bytes          */
    f[6] = 0u;                           /* bit lógico inicial         */
    f[7] = 7u;                           /* bit lógico final (alineado) */
    ec_put16(f + 8, phys_start);         /* inicio físico (memoria ESC) */
    f[10] = 0u;                          /* bit físico inicial         */
    f[11] = type;                        /* 1=escritura, 2=lectura     */
    f[12] = 0x01u;                       /* activar                    */

    uint16_t ado = (uint16_t)(EC_REG_FMMU0 + (uint16_t)(fmmu * (int)EC_FMMU_LEN));
    return (ChronosRT_ec_master_write_reg(master, (uint16_t)index, ado, f, EC_FMMU_LEN) == (int)EC_FMMU_LEN)
               ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Escaneo del bus                                                     */
/* ------------------------------------------------------------------ */

int ChronosRT_ec_master_scan(ChronosRT_ec_master_t* master) {
    if (!master || !master->is_open) {
        return -1;
    }
    master->slave_count = 0;
    master->is_configured = false;

    for (uint16_t pos = 1u; pos <= (uint16_t)ChronosRT_EC_MAX_SLAVES; pos++) {
        uint8_t info[4];
        uint16_t wc = 0;

        /* 0x0010: dirección de estación (2 B) + alias (2 B). */
        if (ec_call(master, (uint8_t)ChronosRT_EC_CMD_APRD,
                    (uint32_t)pos | ((uint32_t)EC_REG_STADR << 16),
                    NULL, sizeof(info), info, &wc) != 0) {
            break;   /* no hay esclava en esta posición: fin de la cadena */
        }

        ec_slave_t* s = &master->slaves[pos];
        memset(s, 0, sizeof(*s));
        s->pub.position = pos;
        s->pub.station_addr = ec_le16(info);
        s->pub.alias = ec_le16(info + 2);
        s->pub.config_addr = s->pub.station_addr;

        /* Puertos físicos desde el estado de la capa de enlace (DLSTAT). */
        uint8_t dl = 0;
        if (ChronosRT_ec_master_read_reg(master, pos, EC_REG_DLSTAT, &dl, 1) == 1) {
            s->pub.dl_status = dl;
            uint8_t n = 0;
            for (int b = 0; b < 4; b++) {
                if ((dl & (uint8_t)(1u << b)) != 0u) {
                    n++;
                }
            }
            s->pub.ports = n;
        }

        /* Estado AL inicial. */
        uint8_t al = 0;
        uint16_t code = 0;
        (void)ec_slave_read_al(master, pos, &al, &code);

        master->slave_count++;
    }
    return master->slave_count;
}

/* ------------------------------------------------------------------ */
/* Configuración inicial (direcciones, SII, buzón)                     */
/* ------------------------------------------------------------------ */

int ChronosRT_ec_master_config_init(ChronosRT_ec_master_t* master) {
    if (!master || !master->is_open || master->slave_count <= 0) {
        return -1;
    }

    master->has_dc = false;

    for (int i = 1; i <= master->slave_count; i++) {
        ec_slave_t* s = &master->slaves[i];
        uint16_t pos = (uint16_t)i;

        /* Dirección de estación: si está libre, asigna posición + offset. */
        if (s->pub.station_addr == 0u) {
            uint16_t addr = (uint16_t)((uint16_t)i + ChronosRT_EC_NODE_OFFSET);
            uint8_t v[2];
            ec_put16(v, addr);
            if (ChronosRT_ec_master_write_reg(master, pos, EC_REG_STADR, v, 2) == 2) {
                s->pub.station_addr = addr;
            }
        }
        s->pub.config_addr = s->pub.station_addr;

        /* Capacidades DC (ESCSUP, bit 0x04). */
        uint8_t sup[2];
        if (ChronosRT_ec_master_read_reg(master, pos, EC_REG_ESCSUP, sup, 2) == 2) {
            s->pub.has_dc = (ec_le16(sup) & EC_ESCSUP_DC) != 0u;
        }
        if (s->pub.has_dc) {
            master->has_dc = true;
        }

        /* Identidad del ESC. */
        uint8_t ty[2];
        if (ChronosRT_ec_master_read_reg(master, pos, EC_REG_TYPE, ty, 2) == 2) {
            uint16_t t = ec_le16(ty);
            s->cfg.itype = (uint16_t)(t & 0x0Fu);
            s->cfg.dtype = (uint16_t)(t >> 4);
        }

        /* Identidad del esclavo (SII). */
        uint8_t sii[8];
        if (ec_slave_read_sii(master, i, EC_SII_MANUF, sii, 4) == 0) {
            s->pub.vendor_id = ec_le32(sii);
            s->pub.product_code = ec_le32(sii + 2);
        }
        if (ec_slave_read_sii(master, i, EC_SII_REV, sii, 4) == 0) {
            s->pub.revision = ec_le32(sii);
            s->pub.serial = ec_le32(sii + 2);
        }
        uint8_t mbx[6];
        if (ec_slave_read_sii(master, i, EC_SII_RXMBXADR, mbx, 3) == 0) {
            s->cfg.mbx_wr = ec_le16(mbx);
            s->cfg.mbx_rr = ec_le16(mbx + 2);
            s->pub.mbx_proto = ec_le16(mbx + 4);
        }

        /* Sync Managers: se leen los 8 registros del ESC. */
        for (int sm = 0; sm < ChronosRT_EC_MAX_SM; sm++) {
            uint8_t reg[EC_SM_LEN];
            uint16_t ado = (uint16_t)(EC_REG_SM0 + (uint16_t)(sm * (int)EC_SM_LEN));
            if (ChronosRT_ec_master_read_reg(master, pos, ado, reg, EC_SM_LEN) != (int)EC_SM_LEN) {
                break;
            }
            s->cfg.sm_start[sm] = ec_le16(reg);
            s->cfg.sm_len[sm] = ec_le16(reg + 2);
            s->cfg.sm_flags[sm] = (uint32_t)reg[4];
            s->cfg.sm_type[sm] = (sm == 0) ? EC_SMTYPE_MBXOUT
                                : (sm == 1) ? EC_SMTYPE_MBXIN
                                : (sm == 2) ? EC_SMTYPE_OUTPUT
                                : (sm == 3) ? EC_SMTYPE_INPUT
                                : EC_SMTYPE_UNUSED;
        }

        /* Buzón: SM0/SM1 si están definidos; si no, se usan los del SII. */
        if (s->cfg.sm_len[0] == 0u) {
            s->cfg.sm_start[0] = s->cfg.mbx_wr;
            s->cfg.sm_flags[0] = EC_DEFAULT_MBXSM0;
        }
        if (s->cfg.sm_len[1] == 0u) {
            s->cfg.sm_start[1] = s->cfg.mbx_rr;
            s->cfg.sm_flags[1] = EC_DEFAULT_MBXSM1;
        }
        s->cfg.mbx_wr = s->cfg.sm_start[0];
        s->cfg.mbx_rr = s->cfg.sm_start[1];
        s->cfg.mbx_wl = s->cfg.sm_len[0];
        s->cfg.mbx_rl = s->cfg.sm_len[1];

        /* Publica el resultado del buzón. */
        s->pub.o_bytes = 0u;
        s->pub.i_bytes = 0u;
    }

    return master->slave_count;
}

/* ------------------------------------------------------------------ */
/* Mapa de proceso (IOMap): direcciones lógicas, FMMU y SM de datos    */
/* ------------------------------------------------------------------ */

int ChronosRT_ec_master_config_map(ChronosRT_ec_master_t* master) {
    if (!master || !master->is_open || master->slave_count <= 0) {
        return -1;
    }

    uint32_t o_total = 0u;
    uint32_t i_total = 0u;
    uint16_t o_slaves = 0u;
    uint16_t i_slaves = 0u;

    /* Pasada 1: asigna las salidas (RxPDO), contiguas desde 0. */
    for (int i = 1; i <= master->slave_count; i++) {
        ec_slave_t* s = &master->slaves[i];
        uint32_t ob = (s->cfg.sm_type[2] == EC_SMTYPE_OUTPUT) ? s->cfg.sm_len[2] : 0u;
        s->pub.o_bytes = ob;
        s->pub.o_offset = o_total;
        o_total += ob;
        if (ob > 0u) {
            o_slaves++;
        }
    }
    /* Pasada 2: asigna las entradas (TxPDO), contiguas tras las salidas. */
    for (int i = 1; i <= master->slave_count; i++) {
        ec_slave_t* s = &master->slaves[i];
        uint32_t ib = (s->cfg.sm_type[3] == EC_SMTYPE_INPUT) ? s->cfg.sm_len[3] : 0u;
        s->pub.i_bytes = ib;
        s->pub.i_offset = o_total + i_total;
        i_total += ib;
        if (ib > 0u) {
            i_slaves++;
        }
    }

    uint32_t size = o_total + i_total;
    free(master->iomap);
    master->iomap = NULL;
    master->iomap_size = 0u;

    if (size > 0u) {
        master->iomap = calloc(1u, size);
        if (!master->iomap) {
            return -1;
        }
    }
    master->iomap_size = size;
    master->o_bytes_total = o_total;
    master->i_bytes_total = i_total;
    master->log_addr = 0u;

    /* Contador de trabajo esperado: cada esclava con salidas incrementa dos
     * veces (escritura + lectura) y cada esclava con entradas una vez. */
    master->expected_wkc = (uint16_t)((o_slaves * 2u) + i_slaves);

    /* Programa FMMU y SM de datos por esclava. */
    for (int i = 1; i <= master->slave_count; i++) {
        ec_slave_t* s = &master->slaves[i];

        if (s->pub.o_bytes > 0u) {
            if (ec_slave_write_fmmu(master, i, 0, s->pub.o_offset,
                                    (uint16_t)s->pub.o_bytes, s->cfg.sm_start[2],
                                    EC_FMMU_WRITE) < 0) {
                return -1;
            }
            if (ec_slave_write_sm(master, i, 2, s->cfg.sm_start[2],
                                  (uint16_t)s->pub.o_bytes, s->cfg.sm_flags[2]) < 0) {
                return -1;
            }
        }
        if (s->pub.i_bytes > 0u) {
            if (ec_slave_write_fmmu(master, i, 1, s->pub.i_offset,
                                    (uint16_t)s->pub.i_bytes, s->cfg.sm_start[3],
                                    EC_FMMU_READ) < 0) {
                return -1;
            }
            if (ec_slave_write_sm(master, i, 3, s->cfg.sm_start[3],
                                  (uint16_t)s->pub.i_bytes, s->cfg.sm_flags[3]) < 0) {
                return -1;
            }
        }
    }

    master->is_configured = true;
    return 0;
}

uint32_t ChronosRT_ec_master_slave_count(const ChronosRT_ec_master_t* master) {
    return (master && master->slave_count > 0) ? (uint32_t)master->slave_count : 0u;
}

const ChronosRT_ec_slave_t* ChronosRT_ec_master_slave(const ChronosRT_ec_master_t* master,
                                                      uint32_t index) {
    if (!master || master->slave_count <= 0 || index >= (uint32_t)master->slave_count) {
        return NULL;
    }
    return &master->slaves[index + 1u].pub;
}

const uint8_t* ChronosRT_ec_master_iomap(const ChronosRT_ec_master_t* master) {
    return master ? master->iomap : NULL;
}

uint32_t ChronosRT_ec_master_iomap_size(const ChronosRT_ec_master_t* master) {
    return master ? master->iomap_size : 0u;
}

/* ------------------------------------------------------------------ */
/* Máquina de estados AL                                               */
/* ------------------------------------------------------------------ */

int ec_slave_read_al(ChronosRT_ec_master_t* master, int index,
                     uint8_t* state, uint16_t* status_code) {
    if (!master || index < 1 || index > master->slave_count) {
        return -1;
    }
    uint8_t al[6];
    if (ChronosRT_ec_master_read_reg(master, (uint16_t)index, EC_REG_ALSTAT, al, 6) < 0) {
        return -1;
    }
    ec_slave_t* s = &master->slaves[index];
    s->pub.al_state = (uint8_t)(al[0] & 0x0Fu);
    s->pub.al_status_code = ec_le16(al + 4);
    if (state) {
        *state = s->pub.al_state;
    }
    if (status_code) {
        *status_code = s->pub.al_status_code;
    }
    return 0;
}

int ChronosRT_ec_master_read_state(ChronosRT_ec_master_t* master, uint16_t adp,
                                   uint8_t* state, uint16_t* status_code) {
    if (!master || !master->is_open) {
        return -1;
    }
    uint8_t al[6];
    if (ChronosRT_ec_master_read_reg(master, adp, EC_REG_ALSTAT, al, 6) < 0) {
        return -1;
    }
    if (state) {
        *state = (uint8_t)(al[0] & 0x0Fu);
    }
    if (status_code) {
        *status_code = ec_le16(al + 4);
    }
    return 0;
}

int ChronosRT_ec_master_write_state(ChronosRT_ec_master_t* master, uint16_t adp, uint8_t state) {
    if (!master || !master->is_open) {
        return -1;
    }
    uint8_t ctrl[2];
    ctrl[0] = state;   /* incluye el bit ACK (0x10) si procede */
    ctrl[1] = 0u;
    return (ChronosRT_ec_master_write_reg(master, adp, EC_REG_ALCTL, ctrl, 2) == 2) ? 0 : -1;
}

int ChronosRT_ec_master_statecheck(ChronosRT_ec_master_t* master, uint16_t adp,
                                   uint8_t req_state, uint32_t timeout_us) {
    if (!master) {
        return -1;
    }
    uint64_t deadline = ec_now_us() + (uint64_t)timeout_us;
    uint8_t want = (uint8_t)(req_state & 0x0Fu);

    for (;;) {
        uint8_t cur = 0;
        uint16_t code = 0;
        if (ChronosRT_ec_master_read_state(master, adp, &cur, &code) == 0 && cur == want) {
            return 0;
        }
        if (ec_now_us() >= deadline) {
            return -1;
        }
    }
}

int ChronosRT_ec_master_request_state(ChronosRT_ec_master_t* master, uint16_t adp,
                                      uint8_t state, uint32_t timeout_us) {
    if (!master) {
        return -1;
    }
    uint8_t want = (uint8_t)(state & 0x0Fu);

    if (ChronosRT_ec_master_write_state(master, adp, want) < 0) {
        return -1;
    }

    uint64_t deadline = ec_now_us() + (uint64_t)timeout_us;
    for (;;) {
        uint8_t cur = 0;
        uint16_t code = 0;
        if (ChronosRT_ec_master_read_state(master, adp, &cur, &code) == 0) {
            if (cur == want) {
                return 0;
            }
            /* Error latcheado: se reconoce (ACK) para desbloquear la máquina. */
            if ((cur & ChronosRT_EC_STATE_ERROR) != 0u) {
                (void)ChronosRT_ec_master_write_state(master, adp,
                                                      (uint8_t)(want | ChronosRT_EC_STATE_ACK));
            }
        }
        if (ec_now_us() >= deadline) {
            return -1;
        }
    }
}

int ChronosRT_ec_master_request_all_states(ChronosRT_ec_master_t* master,
                                           uint8_t state, uint32_t timeout_us) {
    if (!master || !master->is_open || master->slave_count <= 0) {
        return -1;
    }
    uint8_t want = (uint8_t)(state & 0x0Fu);

    /* Escritura por difusión (BWR) al registro de control AL de todas. */
    uint8_t ctrl[2];
    ctrl[0] = want;
    ctrl[1] = 0u;
    if (ChronosRT_ec_master_broadcast_write(master, EC_REG_ALCTL, ctrl, 2) < 0) {
        return -1;
    }

    uint64_t deadline = ec_now_us() + (uint64_t)timeout_us;
    for (;;) {
        uint8_t ok = 0;
        for (int i = 1; i <= master->slave_count; i++) {
            uint8_t cur = 0;
            uint16_t code = 0;
            if (ec_slave_read_al(master, i, &cur, &code) == 0) {
                if (cur == want) {
                    ok++;
                } else if ((cur & ChronosRT_EC_STATE_ERROR) != 0u) {
                    (void)ChronosRT_ec_master_write_state(master, (uint16_t)i,
                                                          (uint8_t)(want | ChronosRT_EC_STATE_ACK));
                }
            }
        }
        if ((int)ok == master->slave_count) {
            return 0;
        }
        if (ec_now_us() >= deadline) {
            return -1;
        }
    }
}
