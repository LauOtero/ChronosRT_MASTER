/*
 * ec_internal.h — Definiciones internas del maestro EtherCAT de ChronosRT.
 *
 * NO es una cabecera pública: solo la usan los módulos de `src/network/`.
 * Contiene los límites, registros ESC/EEPROM/DC/SII, comandos y las estructuras
 * internas del puerto de red y del maestro (la estructura pública
 * `ChronosRT_ec_slave_t` forma parte de `ec_slave_t`).
 *
 * Organización de módulos:
 *   ec_frame.c    — serialización/parseo puro de datagramas y tramas.
 *   ec_socket.c   — capa de socket RT (AF_PACKET) y enumeración de interfaces.
 *   ec_slave.c    — escaneo, configuración de esclavas, SII/SM/FMMU y máquina AL.
 *   ec_dc.c       — relojes distribuidos (DC).
 *   ec_mailbox.c  — buzón (SM0/SM1) y CoE (SDO).
 *   ec_master.c   — ciclo de vida, intercambio de datagramas y ciclo de proceso.
 */
#ifndef CHRONOSRT_EC_INTERNAL_H
#define CHRONOSRT_EC_INTERNAL_H

#include "chronosrt_ethercat.h"
#include "chronosrt_abi.h"

#include <stdint.h>
#include <stdbool.h>
#include <net/if.h>

/* ------------------------------------------------------------------ */
/* Geometría de la trama                                               */
/* ------------------------------------------------------------------ */
#define EC_ETH_HDR_LEN   14u   /* cabecera Ethernet                        */
#define EC_ECAT_HDR_LEN   2u   /* cabecera EtherCAT (longitud|tipo)        */
#define EC_DGR_HDR_LEN   10u   /* cabecera de datagrama                    */
#define EC_WKC_LEN        2u   /* contador de trabajo                      */

#define EC_MORE_BIT   0x8000u  /* bit M del campo dlength                  */
#define EC_LEN_MASK   0x07FFu  /* 11 bits de longitud                      */
#define EC_TYPE_SHIFT 12u      /* tipo en bits 12..15                      */
#define EC_TYPE_VAL   1u       /* tipo 1 = comandos EtherCAT               */

/* ------------------------------------------------------------------ */
/* Registros ESC                                                       */
/* ------------------------------------------------------------------ */
#define EC_REG_TYPE          0x0000u
#define EC_REG_PORTDES       0x0007u
#define EC_REG_ESCSUP        0x0008u   /* capacidades del ESC (bit 2 = DC) */
#define EC_REG_STADR         0x0010u
#define EC_REG_ALIAS         0x0012u
#define EC_REG_DLCTL         0x0100u
#define EC_REG_DLSTAT        0x0110u
#define EC_REG_ALCTL         0x0120u
#define EC_REG_ALSTAT        0x0130u
#define EC_REG_ALSTATCODE    0x0134u
#define EC_REG_PDICTL        0x0140u
#define EC_REG_RXERR         0x0300u
#define EC_REG_CRCERR        0x0308u
#define EC_REG_WDCNT         0x0442u

#define EC_REG_EEPCFG        0x0500u   /* control de acceso EEPROM         */
#define EC_REG_EEPCTL        0x0502u   /* escritura: comando               */
#define EC_REG_EEPSTAT       0x0502u   /* lectura:   estado                */
#define EC_REG_EEPADR        0x0504u   /* dirección de palabra             */
#define EC_REG_EEPDAT        0x0508u   /* datos                            */

#define EC_REG_FMMU0         0x0600u   /* 16 B por FMMU                    */
#define EC_REG_SM0           0x0800u   /* 8 B por Sync Manager             */

#define EC_REG_DCTIME0       0x0900u
#define EC_REG_DCTIME1       0x0904u
#define EC_REG_DCTIME2       0x0908u
#define EC_REG_DCTIME3       0x090Cu
#define EC_REG_DCSYSTIME     0x0910u
#define EC_REG_DCSOF         0x0918u
#define EC_REG_DCSYSOFFSET   0x0920u
#define EC_REG_DCSYSDELAY    0x0928u
#define EC_REG_DCSYSDIFF     0x092Cu
#define EC_REG_DCSPEEDCNT    0x0930u
#define EC_REG_DCTIMEFILT    0x0934u
#define EC_REG_DCCUC         0x0980u
#define EC_REG_DCSYNCACT     0x0981u
#define EC_REG_DCSTART0      0x0990u
#define EC_REG_DCSTART1      0x0994u
#define EC_REG_DCCYCLE0      0x09A0u
#define EC_REG_DCCYCLE1      0x09A4u

/* Geometría de las estructuras de configuración. */
#define EC_FMMU_LEN          16u
#define EC_SM_LEN             8u
#define EC_SM_STATUS_OFF      6u   /* estado del SM dentro de sus 8 bytes  */
#define EC_SM_ACTIVATE_OFF    7u
#define EC_SM_STATUS_FULL     0x08u /* el buzón contiene datos              */

/* EEPROM (SII): comandos y bits de estado. */
#define EC_ECMD_READ         0x0100u
#define EC_ECMD_WRITE        0x0200u
#define EC_EEPSTAT_ACKERR    0x0002u
#define EC_EEPSTAT_WRERR     0x0004u
#define EC_EEPSTAT_CKERR     0x0008u
#define EC_EEPSTAT_BUSY      0x0020u
#define EC_EEPSTAT_RDERR     0x0040u
#define EC_ESTAT_R64         0x0040u   /* EEPROM capaz de lectura de 8 bytes */

/* Palabras SII estándar. */
#define EC_SII_MANUF         0x0008u   /* vendor ID (32 bits)  */
#define EC_SII_ID            0x000Au   /* product code (32)    */
#define EC_SII_REV           0x000Cu   /* revision (32)        */
#define EC_SII_SER           0x000Eu   /* serial (32)          */
#define EC_SII_RXMBXADR      0x0018u   /* mailbox escritura    */
#define EC_SII_TXMBXADR      0x001Au   /* mailbox lectura      */
#define EC_SII_MBXPROTO      0x001Cu   /* protocolos de buzón  */

/* Capacidades DC en EC_REG_ESCSUP. */
#define EC_ESCSUP_DC         0x0004u

/* Tipos de Sync Manager (campo de función en SMflags). */
#define EC_SMTYPE_UNUSED     0u
#define EC_SMTYPE_MBXOUT     1u
#define EC_SMTYPE_MBXIN      2u
#define EC_SMTYPE_OUTPUT     3u
#define EC_SMTYPE_INPUT      4u

/* Flags por defecto de los SM de buzón (ETG.1000). */
#define EC_DEFAULT_MBXSM0    0x00010026u
#define EC_DEFAULT_MBXSM1    0x00010022u

/* Bits del campo de control AL de los SM (activate). */
#define EC_SMFLAG_ENABLE     0x00000001u
#define EC_SMFLAG_OPMODE     0x00000004u

/* Tipos de FMMU. */
#define EC_FMMU_READ         0x02u
#define EC_FMMU_WRITE        0x01u

/* ------------------------------------------------------------------ */
/* Timeouts (µs) — acotados para no bloquear el ciclo RT               */
/* ------------------------------------------------------------------ */
#define EC_TIMEOUT_RET       2000u
#define EC_TIMEOUT_RET3      2000u
#define EC_TIMEOUT_SAFE      50000u
#define EC_TIMEOUT_EEP       200000u
#define EC_TIMEOUT_STATE     5000000u
#define EC_TIMEOUT_MBX       70000u
#define EC_LOCAL_DELAY       200u

/* ------------------------------------------------------------------ */
/* Buzón / CoE                                                         */
/* ------------------------------------------------------------------ */
#define EC_MBX_HDR_LEN       6u     /* cabecera de buzón            */
#define EC_MBXTYPE_COE       0x03u
#define EC_MBXTYPE_MASK      0x0Fu
#define EC_MBX_CNT_MASK      0x70u

#define EC_COE_SDOREQ        0x01u
#define EC_COE_SDORES        0x03u

#define EC_SDO_DOWN_INIT     0x21u  /* descarga iniciada (expedita/tamaño) */
#define EC_SDO_DOWN_EXP      0x23u  /* descarga expedita                    */
#define EC_SDO_DOWN_SEG      0x0Bu  /* siguiente segmento de descarga       */
#define EC_SDO_UP_REQ        0x40u  /* petición de carga                    */
#define EC_SDO_UP_SEG_REQ    0x60u  /* petición de siguiente segmento       */
#define EC_SDO_UP_SEG_RES    0x00u  /* respuesta de segmento (toggle 0)     */
#define EC_SDO_ABORT         0x80u

/* ------------------------------------------------------------------ */
/* Puerto de red (capa de socket)                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    int      fd;                              /* socket primario          */
    int      fd_red;                          /* socket redundante (-1)   */
    uint8_t  mac[6];
    uint8_t  mac_red[6];
    int      ifindex;
    int      ifindex_red;
    char     ifname[IF_NAMESIZE];
    char     red_ifname[IF_NAMESIZE];
    bool     redundant;

    /* Estadísticas de enlace. */
    uint64_t frames_tx;
    uint64_t frames_rx;
    uint64_t frames_lost;
} ec_port_t;

/* ------------------------------------------------------------------ */
/* Esclava (extiende la vista pública con la configuración interna)     */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t itype;                              /* tipo de interfaz    */
    uint16_t dtype;

    uint16_t sm_start[ChronosRT_EC_MAX_SM];
    uint16_t sm_len[ChronosRT_EC_MAX_SM];
    uint32_t sm_flags[ChronosRT_EC_MAX_SM];
    uint8_t  sm_type[ChronosRT_EC_MAX_SM];

    uint16_t mbx_wr;                             /* SM0: inicio        */
    uint16_t mbx_wl;                             /* SM0: longitud      */
    uint16_t mbx_rr;                             /* SM1: inicio        */
    uint16_t mbx_rl;                             /* SM1: longitud      */
    uint8_t  mbx_cnt;                            /* contador de sesión */

    uint16_t o_bits;
    uint16_t i_bits;

    int32_t  dc_rtA, dc_rtB, dc_rtC, dc_rtD;     /* tiempos de puerto  */
} ec_slave_cfg_t;

typedef struct {
    ChronosRT_ec_slave_t pub;
    ec_slave_cfg_t       cfg;
} ec_slave_t;

/* ------------------------------------------------------------------ */
/* Maestro                                                             */
/* ------------------------------------------------------------------ */
struct ChronosRT_ec_master {
    ec_port_t port;
    bool      is_open;

    ec_slave_t slaves[ChronosRT_EC_MAX_SLAVES + 1];   /* 1-based */
    int        slave_count;

    uint8_t*   iomap;          /* mapa de proceso (asignado en config_map) */
    uint32_t   iomap_size;
    uint32_t   log_addr;       /* dirección lógica de trabajo              */

    uint32_t   o_bytes_total;  /* salidas totales del bus                  */
    uint32_t   i_bytes_total;  /* entradas totales del bus                 */
    uint16_t   expected_wkc;   /* contador de trabajo esperado del ciclo    */
    bool       has_dc;         /* alguna esclava soporta DC                 */
    bool       is_configured;  /* config_map completado                     */
    bool       process_pending;/* send_processdata sin receive             */

    uint16_t   working_counter;
    uint32_t   default_timeout_us;
    int64_t    dc_time;        /* último tiempo de sistema DC (ns)          */

    /* Métricas de resiliencia. */
    uint64_t   frames_sent;
    uint64_t   frames_lost;
    uint64_t   process_cycles;
    uint64_t   process_errors;

    /* Buffers de trabajo pre-asignados (camino caliente sin malloc). */
    uint8_t    tx_frame[ChronosRT_EC_MAX_FRAME];
    uint8_t    rx_frame[ChronosRT_EC_MAX_FRAME];
    uint32_t   tx_frame_len;

    uint8_t    mbx_out[ChronosRT_EC_MAX_MAILBOX];
    uint8_t    mbx_in[ChronosRT_EC_MAX_MAILBOX];
};

/* ------------------------------------------------------------------ */
/* ec_frame.c — serialización pura                                     */
/* ------------------------------------------------------------------ */

/** Escribe la cabecera Ethernet (dst=difusión, src=mac, EtherType 0x88A4). */
void     ec_frame_begin(const uint8_t mac[6], uint8_t* frame);
/** Escribe la cabecera EtherCAT y devuelve la longitud total de la trama. */
uint32_t ec_frame_commit(uint8_t* frame, uint32_t datagram_bytes);

/* ------------------------------------------------------------------ */
/* ec_socket.c — capa de socket RT                                     */
/* ------------------------------------------------------------------ */
ChronosRT_status_t ec_socket_open(ec_port_t* port, const char* ifname);
ChronosRT_status_t ec_socket_open_redundant(ec_port_t* port, const char* ifname,
                                            const char* redundant_ifname);
void     ec_socket_close(ec_port_t* port);
/** Envía una trama Ethernet completa. Devuelve la longitud o -1. */
int      ec_socket_send(ec_port_t* port, const uint8_t* frame, uint32_t len);
/** Recibe una trama Ethernet completa (con cabecera). Devuelve su longitud o -1. */
int      ec_socket_recv(ec_port_t* port, uint8_t* frame, uint32_t bufsize, uint32_t timeout_us);
uint64_t ec_now_us(void);
int64_t  ec_now_ns(void);

/* ------------------------------------------------------------------ */
/* ec_master.c — intercambio de datagramas                             */
/* ------------------------------------------------------------------ */

/**
 * Intercambio de UN datagrama: envía y espera respuesta, validando el índice.
 * `address` = ADP | (ADO << 16) o dirección lógica. `wc` recibe el contador de
 * trabajo. Devuelve 0 si la esclava respondió (wc > 0) o -1 en error/timeout.
 */
int ec_call(ChronosRT_ec_master_t* master, uint8_t cmd, uint32_t address,
            const void* wdata, uint16_t len, void* rdata, uint16_t* wc);

/* ------------------------------------------------------------------ */
/* ec_slave.c — configuración y máquina AL                             */
/* ------------------------------------------------------------------ */

/** Lee el estado AL de la esclava `index` (1..N) y refresca su vista pública. */
int ec_slave_read_al(ChronosRT_ec_master_t* master, int index,
                     uint8_t* state, uint16_t* status_code);
/** Lee `count` palabras del SII/EEPROM de la esclava `index`. */
int ec_slave_read_sii(ChronosRT_ec_master_t* master, int index,
                      uint16_t word, uint8_t* out, int words);
/** Configura un Sync Manager de la esclava `index`. */
int ec_slave_write_sm(ChronosRT_ec_master_t* master, int index, int sm,
                      uint16_t start, uint16_t len, uint32_t flags);
/** Configura un FMMU de la esclava `index`. */
int ec_slave_write_fmmu(ChronosRT_ec_master_t* master, int index, int fmmu,
                        uint32_t log_start, uint16_t len, uint16_t phys_start, uint8_t type);

/* ------------------------------------------------------------------ */
/* ec_mailbox.c — buzón (SM0/SM1) y CoE (SDO)                          */
/* ------------------------------------------------------------------ */
int ec_mbx_send(ChronosRT_ec_master_t* master, int slave,
                const uint8_t* mbx, uint16_t len);
int ec_mbx_receive(ChronosRT_ec_master_t* master, int slave,
                   uint8_t* mbx, uint16_t bufsize, uint32_t timeout_us);

#endif /* CHRONOSRT_EC_INTERNAL_H */
