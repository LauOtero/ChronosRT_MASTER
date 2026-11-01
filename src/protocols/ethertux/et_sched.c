/*
 * et_sched.c — Planificador time-triggered de EtherTux.
 *
 * El superframe (1 ms por defecto) se divide en cuatro ventanas dedicadas:
 *
 *   | control (P0,P1) | determinista (P2) | rate (P3,P4) | best-effort (P5) |
 *   |     100 µs      |      200 µs       |    300 µs    |      400 µs      |
 *
 * Dentro de cada ventana se reservan ranuras fijas por nodo/flujo, de modo que
 * dos tramas críticas nunca compiten por el medio: la latencia es una constante
 * más un sesgo acotado. P0 es "express" y puede transmitir en CUALQUIER ventana
 * (prioridad absoluta); el resto solo en la suya.
 *
 * Si un nodo cae, `ChronosRT_et_sched_remove_node()` libera sus ranuras para que
 * el servidor pueda reasignarlas.
 */
#include "et_internal.h"

#include <stdlib.h>
#include <string.h>

ChronosRT_et_sched_t* ChronosRT_et_sched_create(uint32_t cycle_ns, uint64_t link_bps) {
    ChronosRT_et_sched_t* s = (ChronosRT_et_sched_t*)calloc(1u, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->cycle_ns = (cycle_ns != 0u) ? cycle_ns : 1000000u;
    s->link_bps = (link_bps != 0u) ? link_bps : 1000000000ull;

    /* Reparto proporcional 10 / 20 / 30 / 40 %. */
    uint32_t c = s->cycle_ns;
    s->window_ns[ET_WINDOW_CONTROL]       = (c * 10u) / 100u;
    s->window_ns[ET_WINDOW_DETERMINISTIC] = (c * 20u) / 100u;
    s->window_ns[ET_WINDOW_RATE]          = (c * 30u) / 100u;
    s->window_ns[ET_WINDOW_BEST_EFFORT]   = c -
        (s->window_ns[ET_WINDOW_CONTROL] + s->window_ns[ET_WINDOW_DETERMINISTIC] +
         s->window_ns[ET_WINDOW_RATE]);
    return s;
}

void ChronosRT_et_sched_destroy(ChronosRT_et_sched_t* sched) {
    free(sched);
}

ChronosRT_status_t ChronosRT_et_sched_set_windows(ChronosRT_et_sched_t* sched,
                                                  uint32_t control_ns,
                                                  uint32_t deterministic_ns,
                                                  uint32_t rate_ns,
                                                  uint32_t best_effort_ns) {
    if (sched == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    uint64_t sum = (uint64_t)control_ns + deterministic_ns + rate_ns + best_effort_ns;
    if (sum != (uint64_t)sched->cycle_ns) {
        return ChronosRT_ERROR_INVALID;   /* las ventanas deben cubrir el ciclo */
    }
    sched->window_ns[ET_WINDOW_CONTROL]       = control_ns;
    sched->window_ns[ET_WINDOW_DETERMINISTIC] = deterministic_ns;
    sched->window_ns[ET_WINDOW_RATE]          = rate_ns;
    sched->window_ns[ET_WINDOW_BEST_EFFORT]   = best_effort_ns;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_sched_add_slot(ChronosRT_et_sched_t* sched,
                                               uint8_t priority, uint16_t node,
                                               uint16_t frame_bytes,
                                               uint32_t offset_ns) {
    if (sched == NULL || priority >= ChronosRT_ET_PRIORITIES || frame_bytes == 0u) {
        return ChronosRT_ERROR_INVALID;
    }
    if (offset_ns >= sched->cycle_ns) {
        return ChronosRT_ERROR_INVALID;
    }
    if (sched->slot_count >= ET_MAX_SLOTS) {
        return ChronosRT_ERROR_FULL;
    }
    ChronosRT_et_slot_t* s = &sched->slots[sched->slot_count];
    s->priority = priority;
    s->node = node;
    s->frame_bytes = frame_bytes;
    s->offset_ns = offset_ns;
    sched->slot_count++;
    return ChronosRT_SUCCESS;
}

int ChronosRT_et_sched_remove_node(ChronosRT_et_sched_t* sched, uint16_t node) {
    if (sched == NULL) {
        return 0;
    }
    uint32_t out = 0u;
    int removed = 0;
    for (uint32_t i = 0; i < sched->slot_count; i++) {
        if (sched->slots[i].node == node) {
            removed++;
            continue;
        }
        if (out != i) {
            sched->slots[out] = sched->slots[i];
        }
        out++;
    }
    sched->slot_count = out;
    return removed;
}

uint32_t ChronosRT_et_sched_slot_count(const ChronosRT_et_sched_t* sched) {
    return (sched != NULL) ? sched->slot_count : 0u;
}

bool ChronosRT_et_sched_slot(const ChronosRT_et_sched_t* sched, uint32_t index,
                             ChronosRT_et_slot_t* out) {
    if (sched == NULL || out == NULL || index >= sched->slot_count) {
        return false;
    }
    *out = sched->slots[index];
    return true;
}

uint32_t ChronosRT_et_sched_cycle_ns(const ChronosRT_et_sched_t* sched) {
    return (sched != NULL) ? sched->cycle_ns : 0u;
}

uint8_t ChronosRT_et_sched_window_at(const ChronosRT_et_sched_t* sched, uint64_t now_ns) {
    if (sched == NULL || sched->cycle_ns == 0u) {
        return ET_WINDOW_BEST_EFFORT;
    }
    uint32_t phase = (uint32_t)(now_ns % (uint64_t)sched->cycle_ns);

    uint32_t limit = sched->window_ns[ET_WINDOW_CONTROL];
    if (phase < limit) {
        return ET_WINDOW_CONTROL;
    }
    limit += sched->window_ns[ET_WINDOW_DETERMINISTIC];
    if (phase < limit) {
        return ET_WINDOW_DETERMINISTIC;
    }
    limit += sched->window_ns[ET_WINDOW_RATE];
    if (phase < limit) {
        return ET_WINDOW_RATE;
    }
    return ET_WINDOW_BEST_EFFORT;
}

bool ChronosRT_et_sched_can_transmit(const ChronosRT_et_sched_t* sched,
                                      uint8_t priority, uint64_t now_ns) {
    if (sched == NULL || priority >= ChronosRT_ET_PRIORITIES) {
        return false;
    }
    /* P0 es express: prioridad absoluta en cualquier ventana. */
    if (priority == ChronosRT_ET_P0) {
        return true;
    }

    uint8_t w = ChronosRT_et_sched_window_at(sched, now_ns);
    switch (priority) {
        case ChronosRT_ET_P1: return w == ET_WINDOW_CONTROL;
        case ChronosRT_ET_P2: return w == ET_WINDOW_DETERMINISTIC;
        case ChronosRT_ET_P3:
        case ChronosRT_ET_P4: return w == ET_WINDOW_RATE;
        case ChronosRT_ET_P5: return w == ET_WINDOW_BEST_EFFORT;
        default:              return false;
    }
}

uint64_t ChronosRT_et_tx_time_ns(uint32_t frame_bytes, uint64_t link_bps) {
    if (link_bps == 0u) {
        link_bps = 1000000000ull;
    }
    /* +20 B: preámbulo (8) + SFD (1) + IFG (12) aproximados. */
    uint64_t bits = (uint64_t)(frame_bytes + 20u) * 8ull;
    return (bits * 1000000000ull) / link_bps;
}
