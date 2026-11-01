/*
 * chronosrt_abi.h — Contrato ABI estable de ChronosRT (C11)
 *
 * Regla: una vez publicado, NUNCA romper el ABI. Solo añadir al final.
 * Cambios incompatibles -> nueva versión mayor + cabecera versionada.
 *
 * Convenciones de símbolos (coexistencia con libtuxrt_core):
 *   - ChronosRT exporta únicamente símbolos con prefijo `ChronosRT_`
 *     (más las funciones `chronosrt_*` en minúscula y `ChronosRT_*` de módulos).
 *   - libtuxrt_core exporta símbolos con prefijo `tuxrt_`.
 *   - Ambos espacios de nombres son disjuntos => pueden convivir en el mismo
 *     proceso y enlazarse simultáneamente sin colisión.
 */
#ifndef CHRONOSRT_ABI_H
#define CHRONOSRT_ABI_H

#define CHRONOSRT_VERSION_MAJOR 0
#define CHRONOSRT_VERSION_MINOR 1
#define CHRONOSRT_VERSION_PATCH 0
#define CHRONOSRT_VERSION_STRING "0.1.0"

/* ABI numérico (major<<16 | minor<<8 | patch) para verificación en runtime. */
#define CHRONOSRT_ABI_VERSION 0x000100UL

/* ------------------------------------------------------------------------- */
/* Macro de visibilidad: con -fvisibility=hidden solo se exporta lo marcado.  */
/* ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(CHRONOSRT_BUILDING)
    #define CHRONOSRT_API __declspec(dllexport)
  #else
    #define CHRONOSRT_API __declspec(dllimport)
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define CHRONOSRT_API __attribute__((visibility("default")))
  #else
    #define CHRONOSRT_API
  #endif
#endif

/* Alineación de línea de caché para estructuras de camino caliente. */
#define CHRONOSRT_CACHELINE 64

#endif /* CHRONOSRT_ABI_H */
