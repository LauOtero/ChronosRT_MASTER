/*
 * chronosrt_hwaccel.h — HAL de aceleración por hardware de ChronosRT (C11).
 *
 * Capa de abstracción que detecta EN TIEMPO DE EJECUCIÓN las capacidades de la
 * plataforma y selecciona la mejor implementación disponible sin romper la
 * portabilidad. Es la base de EtherTux (CRC de trama, FEC Reed-Solomon y
 * timestamping) y puede usarla cualquier otro subsistema.
 *
 *   - CRC-32C de trama: SIEMPRE por CPU con instrucciones dedicadas.
 *       · ARMv8.1+ : crc32cb/c/h/w/x  (Cortex-A53 en adelante => todas las SBC
 *                    ARM64 objetivo; únicamente la BPI M2 Zero, Cortex-A7, no
 *                    las tiene y usa el fallback software slice-by-8).
 *       · x86-64   : instrucción CRC32 de SSE4.2 (SSE4.2 => CRC-32C).
 *     Se elige CRC-32C (Castagnoli) y no CRC-32 (IEEE) porque es la ÚNICA que
 *     tiene aceleración por instrucción en AMBAS arquitecturas, garantizando el
 *     mismo polinomio en todos los nodos del bus.
 *
 *   - FEC Reed-Solomon sobre GF(2^8): código sistemático RS(N,K) basado en
 *     matriz de Vandermonde. La codificación es una combinación lineal con
 *     coeficientes CONSTANTES, lo que permite vectorizar la multiplicación en
 *     GF(2^8) mediante tablas de nibble + shuffle (NEON / AVX2). La
 *     decodificación es de *borrados* (se conocen los paquetes perdidos), por
 *     eliminación gaussiana: sin RTT ni retransmisión.
 *
 *   - Timestamping: reloj monotónico para el sello de trama; la detección de
 *     PHC/Launch Time de una interfaz concreta se hace con ETHTOOL_GET_TS_INFO.
 *
 * Los motores criptográficos dedicados del SoC (sun8i-ce, rk_crypto, ...) NO se
 * usan en el camino crítico: solo se reportan como capacidad para el cifrado
 * opcional del plano de gestión.
 */
#ifndef CHRONOSRT_HWACCEL_H
#define CHRONOSRT_HWACCEL_H

#include "chronosrt_abi.h"
#include "chronosrt.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tamaño máximo de bloque RS soportado por esta implementación. */
#define ChronosRT_RS_MAX_N 64u

/* Capacidades de aceleración detectadas en la plataforma. */
typedef struct {
    bool        crc32c_hw;       /* CRC-32C por instrucción                  */
    const char* crc32c_backend;  /* backend CRC-32C elegido (ver abajo)      */
    bool        fec_simd;        /* FEC vectorizado (NEON / AVX2)            */
    const char* fec_backend;     /* "neon" | "avx2" | "scalar"               */
    bool        armv8_crypto;    /* extensiones AES/SHA de ARMv8             */
    bool        phc;             /* PTP Hardware Clock en la interfaz        */
    bool        launch_time;     /* pacing de TX por hardware (ETF/SO_TXTIME)*/

    /* --- Identificación de la CPU (AMD/Intel, hasta la última generación) --- */
    char        cpu_vendor[13];  /* "GenuineIntel" | "AuthenticAMD" | ...    */
    char        cpu_brand[49];   /* nombre comercial de la CPU               */
    uint32_t    cpu_family;
    uint32_t    cpu_model;
    uint32_t    cpu_stepping;

    /* --- Conjuntos de instrucciones x86-64 relevantes para el datapath --- */
    bool        isa_sse42;       /* CRC32 (CRC-32C por instrucción)          */
    bool        isa_pclmul;      /* PCLMULQDQ                                */
    bool        isa_avx;
    bool        isa_avx2;
    bool        isa_avx512f;
    bool        isa_avx512vl;
    bool        isa_avx512bw;
    bool        isa_avx512dq;
    bool        isa_vpclmul;     /* VPCLMULQDQ (folding de 256/512 bits)     */
    bool        isa_gfni;        /* GFNI (reversión de bits por hardware)    */
    bool        isa_vaes;
    bool        isa_sha;
    bool        isa_aes;
    bool        isa_bmi2;        /* MULX / RORX / PEXT / PDEP                */
    bool        isa_adx;         /* ADCX / ADOX                              */
} ChronosRT_hw_caps_t;

/*
 * Backends de CRC-32C (nombre reportado en `crc32c_backend`):
 *
 *   "sse4.2-x4"     — instrucción CRC32 con 4 flujos independientes: satura el
 *                     puerto de la instrucción (8 B/ciclo). Nehalem / K10 en
 *                     adelante (todo x86-64 con SSE4.2).
 *   "armv8-crc"     — CRC32 de ARMv8.1 (Cortex-A53 y posteriores).
 *   "slice-by-8"    — tablas software (portable, sin aceleración).
 *
 * NOTA: el folding con PCLMULQDQ/VPCLMULQDQ (2 CLMUL por 64 B) se implementó y
 * se midió contra la referencia software; al no reproducirla de forma exacta en
 * la calibración se retiró en lugar de arriesgar el CRC del protocolo. La
 * instrucción CRC32 en cuatro flujos ya agota el throughput de las CPUs sin
 * VPCLMULQDQ, y en las que lo tienen (Ice Lake / Zen 4) el CRC-32C deja de ser
 * el cuello de botella frente al enlace.
 */

/** Capacidades de la CPU (sin interfaz de red asociada). */
CHRONOSRT_API const ChronosRT_hw_caps_t* ChronosRT_hw_caps(void);

/**
 * Capacidades completas para una interfaz concreta (añade PHC y Launch Time
 * consultando ETHTOOL_GET_TS_INFO). `out` recibe una copia; si la interfaz no
 * existe o no soporta la consulta, se devuelven las capacidades de CPU.
 */
CHRONOSRT_API ChronosRT_status_t ChronosRT_hw_caps_for_interface(const char* ifname,
                                                                ChronosRT_hw_caps_t* out);

/* ------------------------------------------------------------------ */
/* CRC-32C (Castagnoli, polinomio reflejado 0x82F63B78)                */
/* ------------------------------------------------------------------ */

/** CRC-32C estándar (init 0xFFFFFFFF, xorout 0xFFFFFFFF). */
CHRONOSRT_API uint32_t ChronosRT_crc32c(const void* data, size_t len);
/**
 * CRC-32C incremental SIN inversión inicial/final: `seed` es el valor previo
 * (0 al empezar) y el resultado se puede encadenar. Usa HW si está disponible.
 */
CHRONOSRT_API uint32_t ChronosRT_crc32c_raw(uint32_t seed, const void* data, size_t len);
/** Equivalentes forzando la implementación software (verificación/tests). */
CHRONOSRT_API uint32_t ChronosRT_crc32c_sw(const void* data, size_t len);
CHRONOSRT_API uint32_t ChronosRT_crc32c_sw_raw(uint32_t seed, const void* data, size_t len);

/** Nombre del backend CRC-32C activo (ver la tabla de la cabecera). */
CHRONOSRT_API const char* ChronosRT_crc32c_active_backend(void);

/**
 * Verifica TODOS los backends detectados en esta CPU contra la referencia
 * software, con longitudes y semillas variadas. Devuelve el número de
 * backends que NO coinciden (0 = todos correctos) y, si hay fallos, deja en
 * `*first_failing` el nombre del primero (admite NULL).
 */
CHRONOSRT_API uint32_t ChronosRT_crc32c_selftest(const char** first_failing);

/**
 * Fuerza un backend por nombre (solo para tests y diagnóstico). Si el backend
 * no está disponible en esta CPU devuelve `ChronosRT_ERROR_NOTSUP`.
 */
CHRONOSRT_API ChronosRT_status_t ChronosRT_crc32c_force_backend(const char* name);

/* ------------------------------------------------------------------ */
/* Álgebra de GF(2^8) — polinomio primitivo 0x11D                      */
/* ------------------------------------------------------------------ */

CHRONOSRT_API uint8_t ChronosRT_gf8_mul(uint8_t a, uint8_t b);
CHRONOSRT_API uint8_t ChronosRT_gf8_inv(uint8_t a);

/* ------------------------------------------------------------------ */
/* FEC Reed-Solomon sistemático RS(N,K)                                */
/* ------------------------------------------------------------------ */

/**
 * Codifica K bloques de datos de `len` bytes en N-K bloques de paridad.
 * `data[0..K-1]` y `parity[0..N-K-1]` son búferes de exactamente `len` bytes.
 * Los primeros K bloques del código son los datos (código sistemático).
 */
CHRONOSRT_API ChronosRT_status_t ChronosRT_rs_encode(uint8_t k, uint8_t n,
                                                     const uint8_t* const* data,
                                                     uint8_t* const* parity,
                                                     uint32_t len);

/**
 * Reconstruye in situ los bloques ausentes de un código RS(N,K).
 * `blocks[0..N-1]` son búferes de `len` bytes; `present[i]` indica si el bloque
 * i llegó. Deben faltar como máximo N-K bloques. Los ausentes se rellenan.
 */
CHRONOSRT_API ChronosRT_status_t ChronosRT_rs_decode(uint8_t k, uint8_t n,
                                                     uint8_t* const* blocks,
                                                     const bool* present,
                                                     uint32_t len);

/** Sello de transmisión: reloj monotónico en nanosegundos. */
CHRONOSRT_API uint64_t ChronosRT_tx_timestamp_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* CHRONOSRT_HWACCEL_H */
