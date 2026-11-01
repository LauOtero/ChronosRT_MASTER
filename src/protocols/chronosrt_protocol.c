/*
 * chronosrt_protocol.c — Resolución del protocolo de red (EtherCAT / EtherTux).
 */
#include "chronosrt_protocol.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

ChronosRT_protocol_t ChronosRT_protocol_default(void) {
    return ChronosRT_PROTOCOL_ETHERCAT;
}

ChronosRT_protocol_t ChronosRT_protocol_from_string(const char* name) {
    if (name == NULL) {
        return ChronosRT_protocol_default();
    }

    /* Normaliza a minúsculas descartando espacios y tabuladores. */
    char buf[16];
    size_t len = 0u;
    for (const char* p = name; *p != '\0' && len < sizeof(buf) - 1u; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') {
            continue;
        }
        buf[len++] = (char)tolower(c);
    }
    buf[len] = '\0';

    if (strcmp(buf, "ethertux") == 0) {
        return ChronosRT_PROTOCOL_ETHERTUX;
    }
    if (strcmp(buf, "ethercat") == 0) {
        return ChronosRT_PROTOCOL_ETHERCAT;
    }
    return ChronosRT_protocol_default();
}

const char* ChronosRT_protocol_name(ChronosRT_protocol_t protocol) {
    return (protocol == ChronosRT_PROTOCOL_ETHERTUX) ? "ethertux" : "ethercat";
}

ChronosRT_protocol_t ChronosRT_protocol_from_env(void) {
    const char* env = getenv("CHRONOSRT_PROTOCOL");
    if (env == NULL || env[0] == '\0') {
        return ChronosRT_protocol_default();
    }
    return ChronosRT_protocol_from_string(env);
}
