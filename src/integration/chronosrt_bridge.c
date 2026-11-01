/*
 * chronosrt_bridge.c — Puente de eventos ChronosRT → sink externo.
 *
 * Genérico y desacoplado: ChronosRT no conoce ni enlaza a libtuxrt_core. El
 * consumidor (p. ej. libtuxrt_core) conecta aquí el sink de su elección. La
 * dependencia del ecosistema es UNIDIRECCIONAL y se declara con el contrato
 * TuxRT Provider ABI (símbolo `tuxrt_provider_descriptor`, ver tuxrt_provider.h).
 */
#include "chronosrt_tuxrt.h"

#include <stdio.h>
#include <string.h>

ChronosRT_status_t ChronosRT_bridge_attach(ChronosRT_bridge_t* bridge,
                                           ChronosRT_handle_t handle,
                                           ChronosRT_event_sink_fn sink,
                                           void* user) {
    if (!bridge || !handle || !sink) {
        return ChronosRT_ERROR_INVALID;
    }

    bridge->handle = handle;
    bridge->sink = sink;
    bridge->user = user;
    ChronosRT_counters_init(&bridge->counters);
    (void)ChronosRT_trace_init(&bridge->trace);
    atomic_init(&bridge->attached, true);

    ChronosRT_trace_mark(&bridge->trace, "chronosrt: bridge attach\n");
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_bridge_detach(ChronosRT_bridge_t* bridge) {
    if (!bridge) {
        return ChronosRT_ERROR_INVALID;
    }
    atomic_store_explicit(&bridge->attached, false, memory_order_release);
    ChronosRT_trace_mark(&bridge->trace, "chronosrt: bridge detach\n");
    ChronosRT_trace_destroy(&bridge->trace);
    bridge->handle = NULL;
    bridge->sink = NULL;
    bridge->user = NULL;
    return ChronosRT_SUCCESS;
}

uint64_t ChronosRT_bridge_pump(ChronosRT_bridge_t* bridge,
                               uint32_t max_events,
                               uint32_t timeout_us) {
    if (!bridge || !atomic_load_explicit(&bridge->attached, memory_order_acquire)) {
        return 0;
    }
    if (max_events == 0) {
        max_events = UINT32_MAX;
    }

    uint64_t forwarded = 0;
    for (uint32_t i = 0; i < max_events; i++) {
        ChronosRT_event_t event;
        ChronosRT_status_t st = ChronosRT_event_subscribe(
            bridge->handle, &event, (i == 0) ? timeout_us : 0);

        if (st == ChronosRT_ERROR_TIMEOUT) {
            break;
        }
        if (st != ChronosRT_SUCCESS) {
            ChronosRT_counters_add(&bridge->counters, &bridge->counters.errors, 1);
            break;
        }

        ChronosRT_counters_add(&bridge->counters, &bridge->counters.consumed, 1);

        if (bridge->sink(&event, bridge->user) == 0) {
            forwarded++;
            ChronosRT_counters_add(&bridge->counters, &bridge->counters.forwarded, 1);
        } else {
            ChronosRT_counters_add(&bridge->counters, &bridge->counters.dropped, 1);
        }
    }

    return forwarded;
}

const ChronosRT_counters_t* ChronosRT_bridge_counters(const ChronosRT_bridge_t* bridge) {
    return bridge ? &bridge->counters : NULL;
}

const char* ChronosRT_tuxrt_namespace_info(void) {
    return "chronosrt " CHRONOSRT_VERSION_STRING
           " (ABI 0x000100, prefijo ChronosRT_*) "
           "coexiste con libtuxrt_core (prefijo tuxrt_*) sin colisión de símbolos";
}

