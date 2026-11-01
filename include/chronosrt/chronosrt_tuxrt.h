/*
 * chronosrt_tuxrt.h — Coexistencia y puente de eventos hacia libtuxrt_core.
 *
 * ChronosRT y libtuxrt_core están diseñadas para convivir en el MISMO proceso:
 *   - ChronosRT exporta símbolos `ChronosRT_*` (ABI propia, `CHRONOSRT_API`).
 *   - libtuxrt_core exporta símbolos `tuxrt_*` (ABI C17 en `tuxrt_sdk.h`).
 *   - Los espacios de nombres son disjuntos => enlazado simultáneo sin colisión.
 *
 * DIRECCIÓN DE LA DEPENDENCIA (contrato TuxRT Provider ABI, `tuxrt_provider.h`):
 * el enlace es UNIDIRECCIONAL consumidor -> proveedor. ChronosRT se publica
 * como PROVEEDOR exportando el símbolo `tuxrt_provider_descriptor` y NO enlaza
 * ni conoce a libtuxrt_core. Es el consumidor (libtuxrt_core) quien lo descubre
 * con dlopen() y dirige los eventos hacia su propia API.
 *
 * Por eso este encabezado sólo declara un puente GENÉRICO (sink desacoplado):
 * ChronosRT produce eventos y los reenvía al `sink` que el consumidor conecte,
 * sin acoplarse a ningún símbolo `tuxrt_*`.
 */
#ifndef CHRONOSRT_TUXRT_H
#define CHRONOSRT_TUXRT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

#include "chronosrt.h"
#include "chronosrt_obs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sink genérico: devuelve 0 si consume el evento, distinto de 0 si lo rechaza. */
typedef int (*ChronosRT_event_sink_fn)(const ChronosRT_event_t* event, void* user);

typedef struct {
    ChronosRT_handle_t        handle;
    ChronosRT_event_sink_fn   sink;
    void*                     user;
    ChronosRT_counters_t      counters;
    ChronosRT_trace_t         trace;
    _Atomic bool              attached;
} ChronosRT_bridge_t;

/* Conecta un handle de ChronosRT con un sink de destino (p. ej. tuxrt_*). */
CHRONOSRT_API ChronosRT_status_t ChronosRT_bridge_attach(ChronosRT_bridge_t* bridge,
                                                         ChronosRT_handle_t handle,
                                                         ChronosRT_event_sink_fn sink,
                                                         void* user);

CHRONOSRT_API ChronosRT_status_t ChronosRT_bridge_detach(ChronosRT_bridge_t* bridge);

/* Drena hasta `max_events` eventos del handle y los reenvía al sink.
 * `timeout_us` se aplica a la primera lectura (0 = no bloqueante).
 * Devuelve el nº de eventos reenviados con éxito. */
CHRONOSRT_API uint64_t ChronosRT_bridge_pump(ChronosRT_bridge_t* bridge,
                                             uint32_t max_events,
                                             uint32_t timeout_us);

CHRONOSRT_API const ChronosRT_counters_t* ChronosRT_bridge_counters(const ChronosRT_bridge_t* bridge);

/* Descripción textual de la coexistencia de espacios de nombres. */
CHRONOSRT_API const char* ChronosRT_tuxrt_namespace_info(void);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_TUXRT_H */
