/*
 * chronosrt_ethercat.h — Maestro EtherCAT de ChronosRT (API pública, C11).
 *
 * Maestro EtherCAT AUTÓNOMO implementado sobre Ethernet crudo (AF_PACKET,
 * EtherType 0x88A4) conforme a IEC 61158 / ETG.1000, SIN dependencias externas
 * (ni SOEM ni el maestro IgH). El diseño prioriza las cuatro propiedades del
 * proyecto:
 *
 *   - TIEMPO REAL     : sin asignaciones dinámicas en el camino caliente, todo
 *                       pre-asignado; el ciclo de proceso no bloquea ni reserva.
 *   - DETERMINISMO    : temporización monotónica (CLOCK_MONOTONIC), timeouts
 *                       acotados, sin esperas indefinidas.
 *   - RESILIENCIA     : validación por contador de trabajo, detección de
 *                       esclavas perdidas, contadores de error del ESC, reintentos
 *                       acotados y redundancia de red (doble NIC).
 *   - ULTRA RENDIMIENTO: varios datagramas por trama (bit M), buffers indexados
 *                       pre-asignados y cero copias superfluas.
 *
 * Cualquier aplicación puede enumerar las interfaces del sistema, abrir una
 * (p. ej. enp2s0 / end0, según los nombres predecibles de udev) como maestro
 * EtherCAT y comunicarse con las esclavas.
 *
 * ChronosRT ofrece DOS protocolos de red seleccionables (nunca simultáneos sobre
 * la misma interfaz): este maestro EtherCAT (hardware EtherCAT dedicado) y
 * EtherTux (Ethernet comercial determinista, `chronosrt_ethertux.h`). La
 * elección se resuelve en configuración con `chronosrt_protocol.h`.
 */
#ifndef CHRONOSRT_ETHERCAT_H
#define CHRONOSRT_ETHERCAT_H

#include "chronosrt_abi.h"
#include "chronosrt.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EtherType de EtherCAT (IEC 61158 tipo 0x88A4). */
#define ChronosRT_EC_ETHERTYPE 0x88A4

/* Límites del maestro. */
#define ChronosRT_EC_MAX_SLAVES        64     /* esclavas soportadas por bus   */
#define ChronosRT_EC_MAX_FRAME         1518   /* MTU Ethernet estándar         */
#define ChronosRT_EC_MAX_DATAGRAM_DATA 1024   /* datos máximos por datagrama   */
#define ChronosRT_EC_MAX_SM            8      /* Sync Managers por esclava     */
#define ChronosRT_EC_MAX_FMMU          4      /* FMMU por esclava              */
#define ChronosRT_EC_MAX_MAILBOX       256    /* bytes del buzón (SM0/SM1)     */
#define ChronosRT_EC_MAX_INTERFACES    16     /* interfaces enumerables        */

/* Dirección de difusión (broadcast). */
#define ChronosRT_EC_BROADCAST_ADDR 0x0000u

/* Dirección de posición base: la primera esclava recibe EC_NODEOFFSET. */
#define ChronosRT_EC_NODE_OFFSET 0x1000u

/* Comandos EtherCAT (campo `command` del datagrama, ETG.1000). */
typedef enum {
    ChronosRT_EC_CMD_NOP  = 0,
    ChronosRT_EC_CMD_APRD = 1,   /* auto-increment read  (posición)      */
    ChronosRT_EC_CMD_APWR = 2,   /* auto-increment write (posición)      */
    ChronosRT_EC_CMD_APRW = 3,   /* auto-increment read/write            */
    ChronosRT_EC_CMD_FPRD = 4,   /* configured address read              */
    ChronosRT_EC_CMD_FPWR = 5,   /* configured address write             */
    ChronosRT_EC_CMD_FPRW = 6,   /* configured address read/write        */
    ChronosRT_EC_CMD_BRD  = 7,   /* broadcast read                       */
    ChronosRT_EC_CMD_BWR  = 8,   /* broadcast write                      */
    ChronosRT_EC_CMD_BRW  = 9,   /* broadcast read/write                 */
    ChronosRT_EC_CMD_LRD  = 10,  /* logical read                         */
    ChronosRT_EC_CMD_LWR  = 11,  /* logical write                        */
    ChronosRT_EC_CMD_LRW  = 12,  /* logical read/write (PDO)             */
    ChronosRT_EC_CMD_ARMW = 13,  /* auto-inc read, multiple write (DC)   */
    ChronosRT_EC_CMD_FRMW = 14   /* configured read, multiple write (DC) */
} ChronosRT_ec_cmd_t;

/* Registros ESC de uso habitual. */
#define ChronosRT_EC_REG_TYPE           0x0000u  /* tipo/revisión del ESC      */
#define ChronosRT_EC_REG_REVISION       0x0008u  /* revisión del ESC           */
#define ChronosRT_EC_REG_STATION_ADDR   0x0010u  /* dirección de estación      */
#define ChronosRT_EC_REG_STATION_ALIAS  0x0012u  /* alias de estación          */
#define ChronosRT_EC_REG_DL_STATUS      0x0110u  /* estado del enlace (DL)     */
#define ChronosRT_EC_REG_AL_CONTROL     0x0120u  /* control AL                 */
#define ChronosRT_EC_REG_AL_STATUS      0x0130u  /* estado AL                  */
#define ChronosRT_EC_REG_AL_STATUS_CODE 0x0134u  /* código de estado AL        */
#define ChronosRT_EC_REG_RX_ERROR       0x0300u  /* contadores de error RX     */
#define ChronosRT_EC_REG_CRC_ERROR      0x0308u  /* contador de error CRC      */
#define ChronosRT_EC_REG_WD_COUNTER     0x0442u  /* contador de watchdog       */
#define ChronosRT_EC_REG_DC_SYSTEM_TIME 0x0910u  /* tiempo de sistema (DC)     */
#define ChronosRT_EC_REG_DC_SYNC_ACT    0x0981u  /* activación SYNC (DC)       */
#define ChronosRT_EC_REG_DC_START0      0x0990u  /* tiempo de arranque SYNC0   */
#define ChronosRT_EC_REG_DC_CYCLE0      0x09A0u  /* periodo de SYNC0           */

/* Estados de la máquina AL (Application Layer). */
typedef enum {
    ChronosRT_EC_STATE_NONE  = 0x00,
    ChronosRT_EC_STATE_INIT  = 0x01,
    ChronosRT_EC_STATE_PREOP = 0x02,
    ChronosRT_EC_STATE_BOOT  = 0x03,
    ChronosRT_EC_STATE_SAFEOP= 0x04,
    ChronosRT_EC_STATE_OP    = 0x08,
    ChronosRT_EC_STATE_ACK   = 0x10,   /* ACK de error (se OR-ea al estado) */
    ChronosRT_EC_STATE_ERROR = 0x10
} ChronosRT_ec_state_t;

/* Protocolos de buzón (mailbox) soportados por una esclava. */
#define ChronosRT_EC_MBX_AOE  0x0001u
#define ChronosRT_EC_MBX_EOE  0x0002u
#define ChronosRT_EC_MBX_COE  0x0004u
#define ChronosRT_EC_MBX_FOE  0x0008u
#define ChronosRT_EC_MBX_SOE  0x0010u
#define ChronosRT_EC_MBX_VOE  0x0020u

/* Interfaces de red del sistema. */
typedef struct {
    char     name[32];      /* nombre de la interfaz (p. ej. "enp2s0") */
    uint8_t  mac[6];        /* dirección MAC                          */
    int      ifindex;       /* índice del kernel                      */
    bool     is_up;         /* interfaz administrativamente activa    */
} ChronosRT_ec_interface_t;

/* Datagrama EtherCAT deserializado. */
typedef struct {
    uint8_t        cmd;             /* ChronosRT_ec_cmd_t                  */
    uint8_t        idx;             /* índice de correlación               */
    uint16_t       adp;             /* dirección de posición/estación      */
    uint16_t       ado;             /* offset (o dirección lógica baja)    */
    uint32_t       address;         /* ADP | (ADO << 16), tal cual on-wire  */
    uint16_t       data_len;        /* longitud del área de datos          */
    uint16_t       irq;             /* campo IRQ (normalmente 0)           */
    uint16_t       working_counter; /* contador de trabajo del datagrama   */
    const uint8_t* data;            /* puntero al área de datos (no propio)*/
    bool           more;            /* bit M: hay más datagramas           */
    uint32_t       next_offset;     /* offset del siguiente datagrama      */
} ChronosRT_ec_datagram_t;

/* Esclava EtherCAT descubierta y configurada. */
typedef struct {
    uint16_t position;        /* posición en el anillo (auto-incremento 1..N) */
    uint16_t station_addr;    /* dirección de estación configurada           */
    uint16_t alias;           /* alias de estación                           */
    uint16_t config_addr;     /* dirección efectiva para FPRD/FPWR           */
    uint32_t vendor_id;       /* fabricante (SII)                            */
    uint32_t product_code;    /* producto (SII)                              */
    uint32_t revision;        /* revisión (SII)                              */
    uint32_t serial;          /* número de serie (SII)                       */

    uint8_t  al_state;        /* estado AL actual                            */
    uint16_t al_status_code;  /* código de estado AL                         */
    uint16_t dl_status;       /* estado de la capa de enlace (DL)            */

    uint8_t  ports;           /* número de puertos del ESC                   */
    bool     has_dc;          /* soporta relojes distribuidos                */
    bool     is_lost;         /* esclava no responde en el último ciclo      */

    uint16_t mbx_proto;       /* protocolos de buzón soportados              */

    uint32_t i_bytes;         /* bytes de entrada (TxPDO)                    */
    uint32_t o_bytes;         /* bytes de salida (RxPDO)                     */
    uint32_t i_offset;        /* offset de entradas en el IOMap              */
    uint32_t o_offset;        /* offset de salidas en el IOMap               */

    int64_t  dc_offset;       /* offset de reloj (ns)                        */
    int32_t  dc_delay;        /* retardo de propagación (ns)                 */
} ChronosRT_ec_slave_t;

/* Maestro EtherCAT (tipo opaco). */
typedef struct ChronosRT_ec_master ChronosRT_ec_master_t;

/* ------------------------------------------------------------------ */
/* Enumeración de interfaces del sistema                               */
/* ------------------------------------------------------------------ */

/** Enumera las interfaces disponibles. Devuelve el número de interfaces. */
CHRONOSRT_API uint32_t ChronosRT_ec_interface_count(void);
/** Devuelve la interfaz en `index`, o NULL si el índice está fuera de rango. */
CHRONOSRT_API const ChronosRT_ec_interface_t* ChronosRT_ec_interface(uint32_t index);
/** Devuelve el índice de `name` (coincidencia por prefijo exacto) o -1. */
CHRONOSRT_API int ChronosRT_ec_interface_index(const char* name);

/* ------------------------------------------------------------------ */
/* Helpers puros de trama (testeables sin hardware)                    */
/* ------------------------------------------------------------------ */

/**
 * Serializa UN datagrama EtherCAT (cabecera de 10 B + datos + WC de 2 B).
 * `address` = ADP | (ADO << 16). `more` activa el bit M (quedan datagramas).
 * Devuelve los bytes escritos o 0 si el búfer es insuficiente.
 */
CHRONOSRT_API uint32_t ChronosRT_ec_datagram_build(uint8_t* out, uint32_t out_size,
                                                   uint8_t cmd, uint8_t idx,
                                                   uint32_t address,
                                                   const void* data, uint16_t len,
                                                   bool more);

/**
 * Deserializa el datagrama que comienza en `in`. Devuelve los bytes consumidos
 * (para avanzar al siguiente) o 0 si el búfer es insuficiente/mal formado.
 */
CHRONOSRT_API uint32_t ChronosRT_ec_datagram_parse(const uint8_t* in, uint32_t in_len,
                                                   ChronosRT_ec_datagram_t* out);

/**
 * Escribe la cabecera EtherCAT de 2 B (longitud de datagramas en 11 bits |
 * tipo=1 en 4 bits) y devuelve su valor de 16 bits en little-endian.
 */
CHRONOSRT_API uint16_t ChronosRT_ec_frame_header(uint8_t out[2], uint16_t datagram_bytes);

/* ------------------------------------------------------------------ */
/* Ciclo de vida del maestro                                           */
/* ------------------------------------------------------------------ */

CHRONOSRT_API ChronosRT_ec_master_t* ChronosRT_ec_master_create(void);
/** Abre `ifname` como maestro EtherCAT (AF_PACKET, EtherType 0x88A4). */
CHRONOSRT_API ChronosRT_status_t ChronosRT_ec_master_open(ChronosRT_ec_master_t* master,
                                                          const char* ifname);
/** Abre el maestro con redundancia de red (NIC primaria + secundaria). */
CHRONOSRT_API ChronosRT_status_t ChronosRT_ec_master_open_redundant(ChronosRT_ec_master_t* master,
                                                                    const char* ifname,
                                                                    const char* redundant_ifname);
CHRONOSRT_API void ChronosRT_ec_master_close(ChronosRT_ec_master_t* master);
CHRONOSRT_API void ChronosRT_ec_master_destroy(ChronosRT_ec_master_t* master);
CHRONOSRT_API bool ChronosRT_ec_master_is_open(const ChronosRT_ec_master_t* master);
CHRONOSRT_API const char* ChronosRT_ec_master_interface(const ChronosRT_ec_master_t* master);
/** Ajusta el timeout por defecto de las operaciones (µs). */
CHRONOSRT_API void ChronosRT_ec_master_set_timeout(ChronosRT_ec_master_t* master, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Escaneo y configuración del bus                                     */
/* ------------------------------------------------------------------ */

/** Enumera las esclavas presentes (auto-incremento). Devuelve el nº o -1. */
CHRONOSRT_API int ChronosRT_ec_master_scan(ChronosRT_ec_master_t* master);
/** Configura las esclavas: dirección de estación, SII/EEPROM y buzón. */
CHRONOSRT_API int ChronosRT_ec_master_config_init(ChronosRT_ec_master_t* master);
/** Configura el mapa de proceso: SM/FMMU y direcciones lógicas (IOMap). */
CHRONOSRT_API int ChronosRT_ec_master_config_map(ChronosRT_ec_master_t* master);

CHRONOSRT_API uint32_t ChronosRT_ec_master_slave_count(const ChronosRT_ec_master_t* master);
CHRONOSRT_API const ChronosRT_ec_slave_t* ChronosRT_ec_master_slave(const ChronosRT_ec_master_t* master,
                                                                    uint32_t index);
/** Puntero al mapa de proceso (IOMap) y su tamaño. */
CHRONOSRT_API const uint8_t* ChronosRT_ec_master_iomap(const ChronosRT_ec_master_t* master);
CHRONOSRT_API uint32_t ChronosRT_ec_master_iomap_size(const ChronosRT_ec_master_t* master);

/* ------------------------------------------------------------------ */
/* Acceso a registros ESC                                              */
/* ------------------------------------------------------------------ */

/* `adp` = auto-incremento (posición) o dirección de estación. `ado` = offset. */
CHRONOSRT_API int ChronosRT_ec_master_read_reg(ChronosRT_ec_master_t* master,
                                               uint16_t adp, uint16_t ado,
                                               void* out, uint16_t len);
CHRONOSRT_API int ChronosRT_ec_master_write_reg(ChronosRT_ec_master_t* master,
                                                uint16_t adp, uint16_t ado,
                                                const void* in, uint16_t len);
/* Direccionamiento por dirección de estación configurada (FPRD/FPWR). */
CHRONOSRT_API int ChronosRT_ec_master_fp_read(ChronosRT_ec_master_t* master,
                                              uint16_t adp, uint16_t ado,
                                              void* out, uint16_t len);
CHRONOSRT_API int ChronosRT_ec_master_fp_write(ChronosRT_ec_master_t* master,
                                               uint16_t adp, uint16_t ado,
                                               const void* in, uint16_t len);
CHRONOSRT_API int ChronosRT_ec_master_broadcast_read(ChronosRT_ec_master_t* master,
                                                     uint16_t ado, void* out, uint16_t len);
CHRONOSRT_API int ChronosRT_ec_master_broadcast_write(ChronosRT_ec_master_t* master,
                                                      uint16_t ado, const void* in, uint16_t len);

/* Direccionamiento lógico (datos de proceso). */
CHRONOSRT_API int ChronosRT_ec_master_lrw(ChronosRT_ec_master_t* master, uint32_t logical_addr,
                                          void* buf, uint16_t len);
CHRONOSRT_API int ChronosRT_ec_master_lrd(ChronosRT_ec_master_t* master, uint32_t logical_addr,
                                          void* out, uint16_t len);
CHRONOSRT_API int ChronosRT_ec_master_lwr(ChronosRT_ec_master_t* master, uint32_t logical_addr,
                                          const void* in, uint16_t len);

/* Lectura + escritura múltiple (relojes distribuidos). */
CHRONOSRT_API int ChronosRT_ec_master_armw(ChronosRT_ec_master_t* master,
                                           uint16_t adp, uint16_t ado, void* buf, uint16_t len);
CHRONOSRT_API int ChronosRT_ec_master_frmw(ChronosRT_ec_master_t* master,
                                           uint16_t adp, uint16_t ado, void* buf, uint16_t len);

/* ------------------------------------------------------------------ */
/* Máquina de estados AL                                               */
/* ------------------------------------------------------------------ */

CHRONOSRT_API int ChronosRT_ec_master_read_state(ChronosRT_ec_master_t* master, uint16_t adp,
                                                 uint8_t* state, uint16_t* status_code);
/** Solicita un estado y espera a alcanzarlo (con ACK de errores). */
CHRONOSRT_API int ChronosRT_ec_master_request_state(ChronosRT_ec_master_t* master, uint16_t adp,
                                                    uint8_t state, uint32_t timeout_us);
/** Comprueba que la esclava `adp` está en `req_state` (con timeout). */
CHRONOSRT_API int ChronosRT_ec_master_statecheck(ChronosRT_ec_master_t* master, uint16_t adp,
                                                 uint8_t req_state, uint32_t timeout_us);
/** Escribe el registro de control AL con ACK de error. */
CHRONOSRT_API int ChronosRT_ec_master_write_state(ChronosRT_ec_master_t* master, uint16_t adp,
                                                  uint8_t state);
/** Lleva TODAS las esclavas a `state` (BWR) y espera (degradación elegante). */
CHRONOSRT_API int ChronosRT_ec_master_request_all_states(ChronosRT_ec_master_t* master,
                                                         uint8_t state, uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Relojes distribuidos (DC)                                           */
/* ------------------------------------------------------------------ */

/** Mide retardos de propagación y compensa el offset de reloj del bus. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_ec_master_config_dc(ChronosRT_ec_master_t* master);
/** Activa/desactiva SYNC0 en la esclava `index` (1..N). */
CHRONOSRT_API int ChronosRT_ec_master_sync0(ChronosRT_ec_master_t* master, uint32_t index,
                                            bool act, uint32_t cycle_ns, int32_t shift_ns);
/** Activa/desactiva SYNC0 y SYNC1 en la esclava `index`. */
CHRONOSRT_API int ChronosRT_ec_master_sync01(ChronosRT_ec_master_t* master, uint32_t index,
                                             bool act, uint32_t cycle0_ns, int32_t shift0_ns,
                                             uint32_t cycle1_ns, int32_t shift1_ns);
/** Último tiempo de sistema DC leído del bus (ns, epoch 2000-01-01). */
CHRONOSRT_API int64_t ChronosRT_ec_master_dc_time(const ChronosRT_ec_master_t* master);

/* ------------------------------------------------------------------ */
/* Ciclo de datos de proceso (PDO)                                     */
/* ------------------------------------------------------------------ */

/** Envía una trama de proceso (LRW) con las salidas del IOMap. */
CHRONOSRT_API int ChronosRT_ec_master_send_processdata(ChronosRT_ec_master_t* master);
/** Recibe la trama de proceso y valida el contador de trabajo esperado. */
CHRONOSRT_API int ChronosRT_ec_master_receive_processdata(ChronosRT_ec_master_t* master,
                                                          uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Buzón / CoE (SDO)                                                   */
/* ------------------------------------------------------------------ */

/** Lee un objeto CoE (SDO upload). `*size` entra con el tamaño del búfer y sale con el leído. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_ec_master_sdo_read(ChronosRT_ec_master_t* master,
                                                              uint32_t slave_index,
                                                              uint16_t index, uint8_t subindex,
                                                              void* out, uint32_t* size,
                                                              uint32_t timeout_us);
/** Escribe un objeto CoE (SDO download). */
CHRONOSRT_API ChronosRT_status_t ChronosRT_ec_master_sdo_write(ChronosRT_ec_master_t* master,
                                                               uint32_t slave_index,
                                                               uint16_t index, uint8_t subindex,
                                                               const void* in, uint32_t size,
                                                               uint32_t timeout_us);

/* ------------------------------------------------------------------ */
/* Diagnóstico                                                         */
/* ------------------------------------------------------------------ */

CHRONOSRT_API uint16_t ChronosRT_ec_master_working_counter(const ChronosRT_ec_master_t* master);
CHRONOSRT_API uint16_t ChronosRT_ec_master_expected_wkc(const ChronosRT_ec_master_t* master);
CHRONOSRT_API uint64_t ChronosRT_ec_master_frames_sent(const ChronosRT_ec_master_t* master);
CHRONOSRT_API uint64_t ChronosRT_ec_master_frames_lost(const ChronosRT_ec_master_t* master);

/** Envío/recepción de datagramas EtherCAT crudos (protocolo avanzado). */
CHRONOSRT_API int ChronosRT_ec_master_send(ChronosRT_ec_master_t* master,
                                           const uint8_t* datagrams, uint32_t len);
CHRONOSRT_API int ChronosRT_ec_master_recv(ChronosRT_ec_master_t* master,
                                           uint8_t* buffer, uint32_t bufsize,
                                           uint32_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_ETHERCAT_H */
