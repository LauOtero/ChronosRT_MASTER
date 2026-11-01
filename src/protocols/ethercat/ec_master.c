/*
 * ec_master.c — Núcleo del maestro EtherCAT: ciclo de vida, intercambio de
 * datagramas, acceso a registros, ciclo de proceso (PDO) y diagnóstico.
 *
 * Todas las operaciones usan búferes pre-asignados en el maestro y temporización
 * monotónica; no hay reservas de memoria ni esperas indefinidas en el camino
 * caliente.
 */
#include "ec_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

ChronosRT_ec_master_t* ChronosRT_ec_master_create(void) {
    ChronosRT_ec_master_t* m = calloc(1, sizeof(ChronosRT_ec_master_t));
    if (!m) {
        return NULL;
    }
    m->port.fd = -1;
    m->port.fd_red = -1;
    m->default_timeout_us = EC_TIMEOUT_RET;
    return m;
}

ChronosRT_status_t ChronosRT_ec_master_open(ChronosRT_ec_master_t* master, const char* ifname) {
    if (!master || !ifname || ifname[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }
    if (master->is_open) {
        return ChronosRT_ERROR_STATE;
    }

    ChronosRT_status_t st = ec_socket_open(&master->port, ifname);
    if (st != ChronosRT_SUCCESS) {
        return st;
    }

    master->is_open = true;
    master->slave_count = 0;
    master->working_counter = 0;
    master->is_configured = false;
    master->process_pending = false;
    master->has_dc = false;
    strncpy(master->port.ifname, ifname, sizeof(master->port.ifname) - 1);
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_ec_master_open_redundant(ChronosRT_ec_master_t* master,
                                                      const char* ifname,
                                                      const char* redundant_ifname) {
    if (!master || !ifname || !redundant_ifname) {
        return ChronosRT_ERROR_INVALID;
    }
    if (master->is_open) {
        return ChronosRT_ERROR_STATE;
    }

    ChronosRT_status_t st = ec_socket_open_redundant(&master->port, ifname, redundant_ifname);
    if (st != ChronosRT_SUCCESS) {
        return st;
    }

    master->is_open = true;
    master->slave_count = 0;
    master->is_configured = false;
    master->process_pending = false;
    return ChronosRT_SUCCESS;
}

void ChronosRT_ec_master_close(ChronosRT_ec_master_t* master) {
    if (!master) {
        return;
    }
    ec_socket_close(&master->port);
    master->is_open = false;
    master->slave_count = 0;
    master->working_counter = 0;
    master->is_configured = false;
    master->process_pending = false;
    master->has_dc = false;
    free(master->iomap);
    master->iomap = NULL;
    master->iomap_size = 0;
}

void ChronosRT_ec_master_destroy(ChronosRT_ec_master_t* master) {
    if (!master) {
        return;
    }
    ChronosRT_ec_master_close(master);
    free(master);
}

bool ChronosRT_ec_master_is_open(const ChronosRT_ec_master_t* master) {
    return master && master->is_open;
}

const char* ChronosRT_ec_master_interface(const ChronosRT_ec_master_t* master) {
    return master ? master->port.ifname : NULL;
}

void ChronosRT_ec_master_set_timeout(ChronosRT_ec_master_t* master, uint32_t timeout_us) {
    if (master) {
        master->default_timeout_us = timeout_us;
    }
}

/* ------------------------------------------------------------------ */
/* Intercambio de UN datagrama                                         */
/* ------------------------------------------------------------------ */

int ec_call(ChronosRT_ec_master_t* master, uint8_t cmd, uint32_t address,
            const void* wdata, uint16_t len, void* rdata, uint16_t* wc) {
    if (!master || !master->is_open || len > ChronosRT_EC_MAX_DATAGRAM_DATA) {
        return -1;
    }

    uint8_t* frame = master->tx_frame;
    uint32_t datagram_cap = (uint32_t)sizeof(master->tx_frame) - EC_ETH_HDR_LEN - EC_ECAT_HDR_LEN;

    ec_frame_begin(master->port.mac, frame);
    uint32_t dl = ChronosRT_ec_datagram_build(frame + EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN,
                                              datagram_cap, cmd, 0u, address,
                                              wdata, len, false);
    if (dl == 0u) {
        return -1;
    }
    uint32_t total = ec_frame_commit(frame, dl);
    master->tx_frame_len = total;

    if (ec_socket_send(&master->port, frame, total) < 0) {
        master->frames_lost++;
        return -1;
    }
    master->frames_sent++;

    uint32_t timeout = (master->default_timeout_us > 0u) ? master->default_timeout_us : EC_TIMEOUT_RET;
    int n = ec_socket_recv(&master->port, master->rx_frame,
                           (uint32_t)sizeof(master->rx_frame), timeout);
    if (n < 0) {
        master->frames_lost++;
        return -1;
    }

    uint32_t off = EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN;
    if ((uint32_t)n <= off) {
        return -1;
    }

    ChronosRT_ec_datagram_t d;
    if (ChronosRT_ec_datagram_parse(master->rx_frame + off, (uint32_t)n - off, &d) == 0u) {
        return -1;
    }
    if (d.idx != 0u || d.cmd != cmd) {
        return -1;
    }

    master->working_counter = d.working_counter;
    if (wc) {
        *wc = d.working_counter;
    }
    if (rdata && d.data && d.data_len >= len && len > 0u) {
        memcpy(rdata, d.data, len);
    }
    return (d.working_counter > 0u) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Acceso a registros ESC                                              */
/* ------------------------------------------------------------------ */

int ChronosRT_ec_master_read_reg(ChronosRT_ec_master_t* master, uint16_t adp, uint16_t ado,
                                 void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_APRD, address, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_write_reg(ChronosRT_ec_master_t* master, uint16_t adp, uint16_t ado,
                                  const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_APWR, address, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_fp_read(ChronosRT_ec_master_t* master, uint16_t adp, uint16_t ado,
                                void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_FPRD, address, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_fp_write(ChronosRT_ec_master_t* master, uint16_t adp, uint16_t ado,
                                 const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_FPWR, address, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_broadcast_read(ChronosRT_ec_master_t* master, uint16_t ado,
                                       void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        return -1;
    }
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_BRD, (uint32_t)ado, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_broadcast_write(ChronosRT_ec_master_t* master, uint16_t ado,
                                        const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        return -1;
    }
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_BWR, (uint32_t)ado, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

/* ------------------------------------------------------------------ */
/* Direccionamiento lógico (datos de proceso)                          */
/* ------------------------------------------------------------------ */

int ChronosRT_ec_master_lrw(ChronosRT_ec_master_t* master, uint32_t logical_addr,
                            void* buf, uint16_t len) {
    if (!master || !buf || len == 0u) {
        return -1;
    }
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_LRW, logical_addr, buf, len, buf, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_lrd(ChronosRT_ec_master_t* master, uint32_t logical_addr,
                            void* out, uint16_t len) {
    if (!master || !out || len == 0u) {
        return -1;
    }
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_LRD, logical_addr, NULL, len, out, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_lwr(ChronosRT_ec_master_t* master, uint32_t logical_addr,
                            const void* in, uint16_t len) {
    if (!master || !in || len == 0u) {
        return -1;
    }
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_LWR, logical_addr, in, len, NULL, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_armw(ChronosRT_ec_master_t* master, uint16_t adp, uint16_t ado,
                             void* buf, uint16_t len) {
    if (!master || !buf || len == 0u) {
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_ARMW, address, buf, len, buf, NULL) == 0)
               ? (int)len : -1;
}

int ChronosRT_ec_master_frmw(ChronosRT_ec_master_t* master, uint16_t adp, uint16_t ado,
                             void* buf, uint16_t len) {
    if (!master || !buf || len == 0u) {
        return -1;
    }
    uint32_t address = (uint32_t)adp | ((uint32_t)ado << 16);
    return (ec_call(master, (uint8_t)ChronosRT_EC_CMD_FRMW, address, buf, len, buf, NULL) == 0)
               ? (int)len : -1;
}

/* ------------------------------------------------------------------ */
/* Ciclo de datos de proceso (PDO)                                     */
/* ------------------------------------------------------------------ */

int ChronosRT_ec_master_send_processdata(ChronosRT_ec_master_t* master) {
    if (!master || !master->is_open || !master->is_configured ||
        !master->iomap || master->iomap_size == 0u) {
        return -1;
    }
    if (master->iomap_size > ChronosRT_EC_MAX_DATAGRAM_DATA) {
        return -1;   /* bus mayor que un datagrama: fuera de alcance del ciclo simple */
    }

    uint8_t* frame = master->tx_frame;
    uint32_t datagram_cap = (uint32_t)sizeof(master->tx_frame) - EC_ETH_HDR_LEN - EC_ECAT_HDR_LEN;

    ec_frame_begin(master->port.mac, frame);
    uint32_t dl = ChronosRT_ec_datagram_build(frame + EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN,
                                              datagram_cap, (uint8_t)ChronosRT_EC_CMD_LRW,
                                              0u, 0u, master->iomap,
                                              (uint16_t)master->iomap_size, false);
    if (dl == 0u) {
        return -1;
    }
    master->tx_frame_len = ec_frame_commit(frame, dl);

    if (ec_socket_send(&master->port, frame, master->tx_frame_len) < 0) {
        master->frames_lost++;
        return -1;
    }
    master->frames_sent++;
    master->process_pending = true;
    return 0;
}

int ChronosRT_ec_master_receive_processdata(ChronosRT_ec_master_t* master, uint32_t timeout_us) {
    if (!master || !master->is_open || !master->process_pending) {
        return -1;
    }

    int n = ec_socket_recv(&master->port, master->rx_frame,
                           (uint32_t)sizeof(master->rx_frame), timeout_us);
    master->process_pending = false;
    master->process_cycles++;
    if (n < 0) {
        master->process_errors++;
        return -1;
    }

    uint32_t off = EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN;
    if ((uint32_t)n <= off) {
        master->process_errors++;
        return -1;
    }

    ChronosRT_ec_datagram_t d;
    if (ChronosRT_ec_datagram_parse(master->rx_frame + off, (uint32_t)n - off, &d) == 0u ||
        d.cmd != (uint8_t)ChronosRT_EC_CMD_LRW) {
        master->process_errors++;
        return -1;
    }

    if (d.data && d.data_len >= master->iomap_size) {
        memcpy(master->iomap, d.data, master->iomap_size);
    }

    master->working_counter = d.working_counter;
    if (d.working_counter < master->expected_wkc) {
        master->process_errors++;
        return -1;
    }
    return (int)d.working_counter;
}

/* ------------------------------------------------------------------ */
/* Diagnóstico y tramas crudas                                         */
/* ------------------------------------------------------------------ */

uint16_t ChronosRT_ec_master_working_counter(const ChronosRT_ec_master_t* master) {
    return master ? master->working_counter : 0u;
}

uint16_t ChronosRT_ec_master_expected_wkc(const ChronosRT_ec_master_t* master) {
    return master ? master->expected_wkc : 0u;
}

uint64_t ChronosRT_ec_master_frames_sent(const ChronosRT_ec_master_t* master) {
    return master ? master->frames_sent : 0u;
}

uint64_t ChronosRT_ec_master_frames_lost(const ChronosRT_ec_master_t* master) {
    return master ? master->frames_lost : 0u;
}

int ChronosRT_ec_master_send(ChronosRT_ec_master_t* master, const uint8_t* datagrams, uint32_t len) {
    if (!master || !master->is_open || !datagrams || len == 0u || len > EC_LEN_MASK) {
        return -1;
    }
    uint32_t frame_len = EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN + len;
    if (frame_len > sizeof(master->tx_frame)) {
        return -1;
    }

    ec_frame_begin(master->port.mac, master->tx_frame);
    memcpy(master->tx_frame + EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN, datagrams, len);
    ec_frame_commit(master->tx_frame, len);

    master->tx_frame_len = frame_len;
    if (ec_socket_send(&master->port, master->tx_frame, frame_len) < 0) {
        master->frames_lost++;
        return -1;
    }
    master->frames_sent++;
    return 0;
}

int ChronosRT_ec_master_recv(ChronosRT_ec_master_t* master, uint8_t* buffer, uint32_t bufsize,
                             uint32_t timeout_us) {
    if (!master || !master->is_open || !buffer || bufsize == 0u) {
        return -1;
    }

    int n = ec_socket_recv(&master->port, master->rx_frame,
                           (uint32_t)sizeof(master->rx_frame), timeout_us);
    if (n < 0) {
        return -1;
    }

    uint32_t off = EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN;
    if ((uint32_t)n <= off) {
        return -1;
    }
    uint32_t dgr_len = (uint32_t)n - off;
    if (dgr_len > bufsize) {
        dgr_len = bufsize;
    }
    memcpy(buffer, master->rx_frame + off, dgr_len);
    return (int)dgr_len;
}
