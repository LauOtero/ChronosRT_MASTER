/*
 * ec_dc.c — Relojes distribuidos (Distributed Clocks, DC).
 *
 * Determina el tiempo de sistema del bus, mide los tiempos de recepción por
 * puerto (retardos de propagación) y configura SYNC0/SYNC1 en las esclavas para
 * un ciclo determinista. Todas las lecturas/escrituras usan el reloj monotónico
 * del maestro como referencia y no bloquean más allá de los timeouts acotados.
 */
#include "ec_internal.h"

#include <string.h>

static void ec_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void ec_put64(uint8_t* p, uint64_t v) {
    ec_put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    ec_put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t ec_get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Lee el tiempo de sistema de la esclava `index` (64 bits, ns desde 2000). */
static int ec_dc_read_systime(ChronosRT_ec_master_t* master, int index, uint64_t* out) {
    uint8_t t[8];
    if (ChronosRT_ec_master_read_reg(master, (uint16_t)index, EC_REG_DCSYSTIME, t, 8) != 8) {
        return -1;
    }
    *out = (uint64_t)ec_get32(t) | ((uint64_t)ec_get32(t + 4) << 32);
    return 0;
}

ChronosRT_status_t ChronosRT_ec_master_config_dc(ChronosRT_ec_master_t* master) {
    if (!master || !master->is_open || master->slave_count <= 0) {
        return ChronosRT_ERROR_INVALID;
    }
    if (!master->has_dc) {
        return ChronosRT_ERROR_NOTSUP;
    }

    uint64_t sys_time = 0u;
    int ref = -1;

    /* 1) Tiempo de sistema de la primera esclava con DC. */
    for (int i = 1; i <= master->slave_count; i++) {
        if (!master->slaves[i].pub.has_dc) {
            continue;
        }
        if (ec_dc_read_systime(master, i, &sys_time) == 0) {
            ref = i;
            break;
        }
    }
    if (ref < 0) {
        return ChronosRT_ERROR_NO_HARDWARE;
    }
    master->dc_time = (int64_t)sys_time;

    /* 2) Tiempos de recepción por puerto (retardos de propagación). */
    for (int i = 1; i <= master->slave_count; i++) {
        ec_slave_t* s = &master->slaves[i];
        if (!s->pub.has_dc) {
            continue;
        }
        uint8_t rt[16];
        if (ChronosRT_ec_master_read_reg(master, (uint16_t)i, EC_REG_DCTIME0, rt, 16) != 16) {
            continue;
        }
        s->cfg.dc_rtA = (int32_t)ec_get32(rt);
        s->cfg.dc_rtB = (int32_t)ec_get32(rt + 4);
        s->cfg.dc_rtC = (int32_t)ec_get32(rt + 8);
        s->cfg.dc_rtD = (int32_t)ec_get32(rt + 12);
        /* Retardo de propagación (ns) respecto a la referencia: usa el puerto
         * de recepción más temprano como cota conservadora. */
        s->pub.dc_delay = s->cfg.dc_rtA;

        uint8_t dly[4];
        ec_put32(dly, (uint32_t)s->pub.dc_delay);
        (void)ChronosRT_ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYSDELAY, dly, 4);
    }

    /* 3) Difunde el tiempo de sistema del bus (escritura múltiple). */
    uint8_t systime[8];
    ec_put64(systime, sys_time);
    (void)ChronosRT_ec_master_broadcast_write(master, EC_REG_DCSYSTIME, systime, 8);

    return ChronosRT_SUCCESS;
}

/* Escribe el tiempo de arranque (absoluto) y el periodo de un SYNC. */
static int ec_dc_write_sync(ChronosRT_ec_master_t* master, int index,
                            int unit, uint32_t cycle_ns, int32_t shift_ns) {
    uint16_t reg_start = (unit == 0) ? EC_REG_DCSTART0 : EC_REG_DCSTART1;
    uint16_t reg_cycle = (unit == 0) ? EC_REG_DCCYCLE0 : EC_REG_DCCYCLE1;

    /* Periodo: el registro almacena (periodo - 1). */
    uint8_t cyc[4];
    ec_put32(cyc, (cycle_ns > 0u) ? (cycle_ns - 1u) : 0u);
    if (ChronosRT_ec_master_write_reg(master, (uint16_t)index, reg_cycle, cyc, 4) < 0) {
        return -1;
    }

    /* Tiempo de arranque: sistema + un periodo, descontando el desfase. */
    int64_t start = master->dc_time + (int64_t)cycle_ns - (int64_t)shift_ns;
    uint8_t st[8];
    ec_put64(st, (uint64_t)start);
    if (ChronosRT_ec_master_write_reg(master, (uint16_t)index, reg_start, st, 8) < 0) {
        return -1;
    }

    /* Control de ciclo continuo (no de un solo disparo). */
    uint8_t cuc = 0x00u;
    (void)ChronosRT_ec_master_write_reg(master, (uint16_t)index, EC_REG_DCCUC, &cuc, 1);
    return 0;
}

int ChronosRT_ec_master_sync0(ChronosRT_ec_master_t* master, uint32_t index,
                              bool act, uint32_t cycle_ns, int32_t shift_ns) {
    if (!master || !master->is_open || index < 1u ||
        index > (uint32_t)master->slave_count) {
        return -1;
    }
    int i = (int)index;

    /* Desactiva antes de reprogramar. */
    uint8_t off = 0x00u;
    (void)ChronosRT_ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &off, 1);

    if (!act) {
        return 0;
    }
    if (ec_dc_write_sync(master, i, 0, cycle_ns, shift_ns) < 0) {
        return -1;
    }

    uint8_t on = 0x01u;   /* SYNC0 */
    return (ChronosRT_ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &on, 1) == 1)
               ? 0 : -1;
}

int ChronosRT_ec_master_sync01(ChronosRT_ec_master_t* master, uint32_t index,
                               bool act, uint32_t cycle0_ns, int32_t shift0_ns,
                               uint32_t cycle1_ns, int32_t shift1_ns) {
    if (!master || !master->is_open || index < 1u ||
        index > (uint32_t)master->slave_count) {
        return -1;
    }
    int i = (int)index;

    uint8_t off = 0x00u;
    (void)ChronosRT_ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &off, 1);

    if (!act) {
        return 0;
    }
    if (ec_dc_write_sync(master, i, 0, cycle0_ns, shift0_ns) < 0) {
        return -1;
    }
    if (ec_dc_write_sync(master, i, 1, cycle1_ns, shift1_ns) < 0) {
        return -1;
    }

    uint8_t on = 0x03u;   /* SYNC0 | SYNC1 */
    return (ChronosRT_ec_master_write_reg(master, (uint16_t)i, EC_REG_DCSYNCACT, &on, 1) == 1)
               ? 0 : -1;
}

int64_t ChronosRT_ec_master_dc_time(const ChronosRT_ec_master_t* master) {
    return master ? master->dc_time : 0;
}
