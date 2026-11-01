/*
 * chronosrt_protocol.h — Selección del protocolo de red de ChronosRT.
 *
 * ChronosRT soporta dos protocolos de red MUTUAMENTE EXCLUYENTES por instancia:
 *
 *   - EtherCAT : esclavas con ASIC/FPGA dedicado, ciclos < 100 µs.
 *   - EtherTux : NICs y switches Ethernet comerciales estándar, SBC
 *                heterogéneas y MCUs con controlador Ethernet SPI/QSPI.
 *
 * La elección se resuelve EN TIEMPO DE CONFIGURACIÓN (variable de entorno
 * `CHRONOSRT_PROTOCOL`, fichero de configuración o parámetro de línea de
 * comandos) y no cambia durante la ejecución: ambos backends comparten la HAL
 * de aceleración y el modelo de prioridades, pero sus ciclos de vida son
 * independientes.
 */
#ifndef CHRONOSRT_PROTOCOL_H
#define CHRONOSRT_PROTOCOL_H

#include "chronosrt_abi.h"
#include "chronosrt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ChronosRT_PROTOCOL_ETHERCAT = 0,
    ChronosRT_PROTOCOL_ETHERTUX  = 1
} ChronosRT_protocol_t;

/* Protocolo por defecto cuando no se especifica ninguno (EtherCAT). */
CHRONOSRT_API ChronosRT_protocol_t ChronosRT_protocol_default(void);

/* Acepta "ethercat" / "ethertux" (sin distinguir mayúsculas ni espacios). */
CHRONOSRT_API ChronosRT_protocol_t ChronosRT_protocol_from_string(const char* name);

/* Nombre canónico ("ethercat" / "ethertux"); nunca NULL. */
CHRONOSRT_API const char* ChronosRT_protocol_name(ChronosRT_protocol_t protocol);

/* Lee CHRONOSRT_PROTOCOL del entorno; si no existe o no es válido, ETHERCAT. */
CHRONOSRT_API ChronosRT_protocol_t ChronosRT_protocol_from_env(void);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_PROTOCOL_H */
