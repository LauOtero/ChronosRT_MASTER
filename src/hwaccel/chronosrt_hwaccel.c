/*
 * chronosrt_hwaccel.c — HAL de aceleración por hardware (CRC-32C y FEC RS).
 *
 * Implementación con despacho en tiempo de ejecución:
 *   - CRC-32C: instrucción ARMv8.1 (crc32c*) o SSE4.2 (crc32), con fallback
 *     software slice-by-8 (imprescindible en Cortex-A7, p. ej. BPI M2 Zero).
 *   - FEC RS: multiplicación en GF(2^8) por tabla de nibble + shuffle
 *     (NEON en AArch64, AVX2 en x86-64) con fallback escalar.
 *
 * Todo el estado de inicialización (tablas GF, tablas CRC, matrices del código)
 * se construye una sola vez y queda en memoria estática: el camino caliente no
 * reserva memoria.
 */
#include "chronosrt_hwaccel.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

#if defined(__linux__)
  #include <unistd.h>
  #include <sys/socket.h>
  #include <sys/ioctl.h>
  #include <net/if.h>
  #include <linux/ethtool.h>
  #include <linux/sockios.h>
  #include <linux/net_tstamp.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  #define CRT_X86 1
#else
  #define CRT_X86 0
#endif

#if defined(__aarch64__)
  #include <arm_neon.h>
  #include <sys/auxv.h>
  #ifndef HWCAP_CRC32
    #define HWCAP_CRC32 (1UL << 7)
  #endif
  #ifndef HWCAP_AES
    #define HWCAP_AES (1UL << 3)
  #endif
  #define CRT_ARM64 1
#else
  #define CRT_ARM64 0
#endif

/* ================================================================== */
/* GF(2^8) — polinomio primitivo x^8+x^4+x^3+x^2+1 (0x11D)             */
/* ================================================================== */

static uint8_t       s_gf_exp[512];
static uint8_t       s_gf_log[256];
static pthread_once_t s_gf_once = PTHREAD_ONCE_INIT;

static void gf_init(void) {
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        s_gf_exp[i] = (uint8_t)x;
        s_gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u) {
            x ^= 0x11Du;
        }
    }
    /* Duplica la tabla de exponenciales para evitar el módulo 255. */
    for (int i = 255; i < 512; i++) {
        s_gf_exp[i] = s_gf_exp[i - 255];
    }
    s_gf_log[0] = 0;   /* no se usa: la multiplicación trata el 0 aparte */
}

static inline void gf_ensure(void) {
    (void)pthread_once(&s_gf_once, gf_init);
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0u || b == 0u) {
        return 0u;
    }
    return s_gf_exp[(unsigned)s_gf_log[a] + (unsigned)s_gf_log[b]];
}

static inline uint8_t gf_inv(uint8_t a) {
    return (a == 0u) ? 0u : s_gf_exp[255u - (unsigned)s_gf_log[a]];
}

uint8_t ChronosRT_gf8_mul(uint8_t a, uint8_t b) {
    gf_ensure();
    return gf_mul(a, b);
}

uint8_t ChronosRT_gf8_inv(uint8_t a) {
    gf_ensure();
    return gf_inv(a);
}

/* ================================================================== */
/* CRC-32C software (slice-by-8)                                      */
/* ================================================================== */

static uint32_t        s_crc_tab[8][256];
static pthread_once_t  s_crc_once = PTHREAD_ONCE_INIT;

/* Tablas de "skip" para el CRC multistream: avanzan el estado por la distancia
 * fija entre flujos, de modo que la combinación cuesta 8 lecturas de L1.
 * Se construyen en las DOS arquitecturas: el esquema de 4 flujos es genérico. */
static uint32_t        s_skip16[8][16];
static uint32_t        s_skip32[8][16];
static uint32_t        s_skip48[8][16];
static uint32_t        s_skip64[8][16];
static uint32_t        s_skip96[8][16];
static uint32_t        s_skip128[8][16];
static uint32_t        s_skip192[8][16];
static uint32_t        s_skip256[8][16];
static uint32_t        s_skip384[8][16];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t crc = i;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
        s_crc_tab[0][i] = crc;
    }
    for (uint32_t t = 1; t < 8u; t++) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t prev = s_crc_tab[t - 1][i];
            s_crc_tab[t][i] = (prev >> 8) ^ s_crc_tab[0][prev & 0xFFu];
        }
    }
}

/* Avanza el estado CRC-32C (crudo) por `n` bytes cero. */
static uint32_t crc_advance_bytes(uint32_t crc, size_t n) {
    while (n-- > 0u) {
        crc = (crc >> 8) ^ s_crc_tab[0][crc & 0xFFu];
    }
    return crc;
}

/* Tabla [8 nibbles][16 valores] que aplica el avance por `n` bytes. */
static void crc_build_skip(uint32_t tab[8][16], size_t n) {
    uint32_t bitval[32];
    for (int j = 0; j < 32; j++) {
        bitval[j] = crc_advance_bytes(1u << j, n);
    }
    for (int i = 0; i < 8; i++) {
        for (int v = 0; v < 16; v++) {
            uint32_t acc = 0;
            for (int b = 0; b < 4; b++) {
                if (v & (1 << b)) {
                    acc ^= bitval[4 * i + b];
                }
            }
            tab[i][v] = acc;
        }
    }
}

static inline uint32_t crc_skip(uint32_t crc, const uint32_t tab[8][16]) {
    return tab[0][crc & 0xFu]         ^ tab[1][(crc >> 4) & 0xFu]
         ^ tab[2][(crc >> 8) & 0xFu]  ^ tab[3][(crc >> 12) & 0xFu]
         ^ tab[4][(crc >> 16) & 0xFu] ^ tab[5][(crc >> 20) & 0xFu]
         ^ tab[6][(crc >> 24) & 0xFu] ^ tab[7][(crc >> 28) & 0xFu];
}

uint32_t ChronosRT_crc32c_sw_raw(uint32_t seed, const void* data, size_t len) {
    (void)pthread_once(&s_crc_once, crc_init);

    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = seed;

    while (len >= 8u) {
        uint32_t one;
        uint32_t two;
        memcpy(&one, p, sizeof(one));
        memcpy(&two, p + 4, sizeof(two));
        one ^= crc;
        crc = s_crc_tab[7][ one        & 0xFFu] ^
              s_crc_tab[6][(one >>  8) & 0xFFu] ^
              s_crc_tab[5][(one >> 16) & 0xFFu] ^
              s_crc_tab[4][(one >> 24) & 0xFFu] ^
              s_crc_tab[3][ two        & 0xFFu] ^
              s_crc_tab[2][(two >>  8) & 0xFFu] ^
              s_crc_tab[1][(two >> 16) & 0xFFu] ^
              s_crc_tab[0][(two >> 24) & 0xFFu];
        p += 8;
        len -= 8u;
    }
    while (len-- > 0u) {
        crc = (crc >> 8) ^ s_crc_tab[0][(crc ^ *p++) & 0xFFu];
    }
    return crc;
}

uint32_t ChronosRT_crc32c_sw(const void* data, size_t len) {
    return ~ChronosRT_crc32c_sw_raw(0xFFFFFFFFu, data, len);
}

/* ================================================================== */
/* CRC-32C por instrucción                                            */
/* ================================================================== */

/* ================================================================== */
/* CRC-32C por instrucción (4 flujos, x86-64 y ARMv8)                 */
/* ================================================================== */

/*
 * `crc32 r64` / `crc32cd` tienen 1/ciclo de throughput y ~3 ciclos de latencia:
 * un único flujo está limitado a 8 B / 3 ciclos ≈ 2,7 B/ciclo. Con cuatro
 * cadenas independientes se oculta la latencia y se satura el puerto de
 * ejecución (8 B/ciclo), que es el techo de la instrucción.
 *
 * El mismo esquema se aplica en ARMv8.1: antes la ruta ARM era de un solo flujo
 * (~2,7 B/ciclo), por lo que esta parte multiplica por ~3 el CRC en las SBC.
 *
 * Los flujos se combinan con tablas de salto (8 lecturas de L1 por combinar):
 * el coste es fijo y se amortiza frente a 64–512 B de datos. Por debajo de 64 B
 * NO se usa multistream: las tablas cuestan más que la propia cadena.
 */
#if CRT_X86
  #define CRC_ACC64(acc, v) ((uint32_t)_mm_crc32_u64((acc), (v)))
  #define CRC_ACC8(acc, b)  ((uint32_t)_mm_crc32_u8((acc), (b)))
  #define CRC_TARGET        __attribute__((target("sse4.2")))
  #define CRC_HW_AVAILABLE  1
#elif CRT_ARM64
  #define CRC_ACC64(acc, v) ((uint32_t)__crc32cd((acc), (v)))
  #define CRC_ACC8(acc, b)  ((uint32_t)__crc32cb((acc), (b)))
  #define CRC_TARGET        __attribute__((target("+crc")))
  #define CRC_HW_AVAILABLE  1
#else
  #define CRC_HW_AVAILABLE  0
#endif

#if CRC_HW_AVAILABLE
/* Cadena única: óptima para cargas pequeñas (< 64 B). */
CRC_TARGET
static inline uint32_t crc_hw_chain(uint32_t l, const uint8_t* p, size_t len) {
    while (len >= 8u) {
        uint64_t v;
        memcpy(&v, p, sizeof(v));
        l = CRC_ACC64(l, v);
        p += 8u;
        len -= 8u;
    }
    while (len-- > 0u) {
        l = CRC_ACC8(l, *p++);
    }
    return l;
}

/* Un "tier": cuatro flujos de `blk` bytes cada uno (4·blk por iteración). */
CRC_TARGET
static inline uint32_t crc_hw_x4_tier(uint32_t l, const uint8_t* p, size_t blk,
                                      const uint32_t t1[8][16],
                                      const uint32_t t2[8][16],
                                      const uint32_t t3[8][16]) {
    uint32_t l1 = 0u;
    uint32_t l2 = 0u;
    uint32_t l3 = 0u;
    const uint8_t* p1 = p + blk;
    const uint8_t* p2 = p + 2u * blk;
    const uint8_t* p3 = p + 3u * blk;

    for (size_t i = 0; i < blk; i += 8u) {
        uint64_t v0, v1, v2, v3;
        memcpy(&v0, p + i, sizeof(v0));
        memcpy(&v1, p1 + i, sizeof(v1));
        memcpy(&v2, p2 + i, sizeof(v2));
        memcpy(&v3, p3 + i, sizeof(v3));
        l  = CRC_ACC64(l,  v0);
        l1 = CRC_ACC64(l1, v1);
        l2 = CRC_ACC64(l2, v2);
        l3 = CRC_ACC64(l3, v3);
    }
    return crc_skip(l, t3) ^ crc_skip(l1, t2) ^ crc_skip(l2, t1) ^ l3;
}

CRC_TARGET
static uint32_t crc_hw_x4(uint32_t seed, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t l = seed;

    /* Se elige el tier mayor posible: menos combinaciones por byte. */
    while (len >= 512u) {
        l = crc_hw_x4_tier(l, p, 128u, s_skip128, s_skip256, s_skip384);
        p += 512u;
        len -= 512u;
    }
    while (len >= 256u) {
        l = crc_hw_x4_tier(l, p, 64u, s_skip64, s_skip128, s_skip192);
        p += 256u;
        len -= 256u;
    }
    while (len >= 128u) {
        l = crc_hw_x4_tier(l, p, 32u, s_skip32, s_skip64, s_skip96);
        p += 128u;
        len -= 128u;
    }
    while (len >= 64u) {
        l = crc_hw_x4_tier(l, p, 16u, s_skip16, s_skip32, s_skip48);
        p += 64u;
        len -= 64u;
    }
    return crc_hw_chain(l, p, len);
}
#endif

/* ================================================================== */
/* Detección de la CPU y registro de backends                          */
/* ================================================================== */

static ChronosRT_hw_caps_t  s_caps;
static pthread_once_t       s_caps_once = PTHREAD_ONCE_INIT;

/* Puntero de CRC resuelto una sola vez; el camino caliente lo lee con una carga
 * atómica relajada (sin `pthread_once` por trama). */
typedef uint32_t (*crc_raw_fn)(uint32_t seed, const void* data, size_t len);
static _Atomic(crc_raw_fn) s_crc_raw_fn = NULL;

/* Backend activo (para `ChronosRT_crc32c_active_backend`). */
static const char* s_crc_active = "slice-by-8";

/* ------------------------------------------------------------------ */
/* Detección de ISA por CPUID (AMD e Intel, todas las generaciones)    */
/* ------------------------------------------------------------------ */
#if CRT_X86
#include <cpuid.h>

static uint64_t xgetbv0(void) {
    unsigned lo = 0u;
    unsigned hi = 0u;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void cpu_detect_x86(ChronosRT_hw_caps_t* c) {
    unsigned eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    unsigned max_leaf = __get_cpuid_max(0, NULL);
    unsigned max_ext  = __get_cpuid_max(0x80000000u, NULL);

    if (max_leaf >= 1u) {
        __cpuid(0, eax, ebx, ecx, edx);
        memcpy(c->cpu_vendor + 0, &ebx, 4);
        memcpy(c->cpu_vendor + 4, &edx, 4);
        memcpy(c->cpu_vendor + 8, &ecx, 4);
        c->cpu_vendor[12] = '\0';

        __cpuid(1, eax, ebx, ecx, edx);
        const unsigned base_family = (eax >> 8) & 0xFu;
        const unsigned base_model  = (eax >> 4) & 0xFu;
        const unsigned ext_family  = (eax >> 20) & 0xFFu;
        const unsigned ext_model   = (eax >> 16) & 0xFu;
        c->cpu_stepping = eax & 0xFu;
        c->cpu_family   = (base_family == 0xFu) ? (base_family + ext_family) : base_family;
        c->cpu_model    = (base_family == 0x6u || base_family == 0xFu)
                          ? ((ext_model << 4) | base_model) : base_model;

        c->isa_sse42  = (ecx >> 20) & 1u;
        c->isa_pclmul = (ecx >> 1) & 1u;
        c->isa_aes    = (ecx >> 25) & 1u;
        c->isa_avx    = (ecx >> 28) & 1u;

        const bool osxsave = ((ecx >> 27) & 1u) != 0u;
        if (osxsave) {
            const uint64_t xcr0 = xgetbv0();
            const bool ymm_os   = (xcr0 & 0x06u) == 0x06u;
            const bool zmm_os   = (xcr0 & 0xE6u) == 0xE6u;
            c->isa_avx &= ymm_os;

            if (max_leaf >= 7u) {
                unsigned e7bx = 0u, e7cx = 0u, e7dx = 0u, e7ax = 0u;
                __cpuid_count(7, 0, e7ax, e7bx, e7cx, e7dx);
                (void)e7ax;
                (void)e7dx;
                c->isa_bmi2      = ((e7bx >> 8) & 1u) != 0u;
                c->isa_avx2      = ((e7bx >> 5) & 1u) != 0u && c->isa_avx;
                c->isa_avx512f   = ((e7bx >> 16) & 1u) != 0u && zmm_os;
                c->isa_avx512dq  = ((e7bx >> 17) & 1u) != 0u && zmm_os;
                c->isa_sha       = ((e7bx >> 29) & 1u) != 0u;
                c->isa_avx512bw  = ((e7bx >> 30) & 1u) != 0u && zmm_os;
                c->isa_avx512vl  = ((e7bx >> 31) & 1u) != 0u && zmm_os;
                c->isa_gfni      = ((e7cx >> 8) & 1u) != 0u;
                c->isa_vaes      = ((e7cx >> 9) & 1u) != 0u;
                c->isa_vpclmul   = ((e7cx >> 10) & 1u) != 0u && zmm_os;
            }
        }
        /* ADX está en la hoja 7 subhoja 0, EBX bit 19 (si existe). */
        if (max_leaf >= 7u) {
            unsigned e7bx = 0u, e7cx = 0u, e7dx = 0u, e7ax = 0u;
            __cpuid_count(7, 0, e7ax, e7bx, e7cx, e7dx);
            c->isa_adx = ((e7bx >> 19) & 1u) != 0u;
        }
    }

    /* Nombre comercial de la CPU (hojas extendidas 0x80000002..0x80000004). */
    if (max_ext >= 0x80000004u) {
        for (unsigned i = 0; i < 3u; i++) {
            __cpuid(0x80000002u + i, eax, ebx, ecx, edx);
            memcpy(c->cpu_brand + i * 16u + 0u, &eax, 4);
            memcpy(c->cpu_brand + i * 16u + 4u, &ebx, 4);
            memcpy(c->cpu_brand + i * 16u + 8u, &ecx, 4);
            memcpy(c->cpu_brand + i * 16u + 12u, &edx, 4);
        }
        c->cpu_brand[48] = '\0';
        /* Recorta el relleno inicial que usan algunos fabricantes. */
        size_t lead = 0u;
        while (c->cpu_brand[lead] == ' ') {
            lead++;
        }
        if (lead > 0u) {
            memmove(c->cpu_brand, c->cpu_brand + lead, strlen(c->cpu_brand + lead) + 1u);
        }
    }
}
#endif /* CRT_X86 */

/* ------------------------------------------------------------------ */
/* Registro de backends de CRC-32C                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* name;
    crc_raw_fn  fn;
    bool        available;
} crc_backend_entry_t;

/* Envoltorio sin atributos de destino: el registro guarda un puntero genérico y
 * la llamada indirecta no arrastra requisitos de ISA al llamador. */
#if CRC_HW_AVAILABLE
static uint32_t crc_be_hw(uint32_t s, const void* d, size_t n) {
    return crc_hw_x4(s, d, n);
}
#endif

#if CRT_X86
static crc_backend_entry_t s_crc_backends[] = {
    { "sse4.2-x4",    crc_be_hw,              false },
    { "slice-by-8",   ChronosRT_crc32c_sw_raw, true },
};
#elif CRT_ARM64
static crc_backend_entry_t s_crc_backends[] = {
    { "armv8-crc",    crc_be_hw,              false },
    { "slice-by-8",   ChronosRT_crc32c_sw_raw, true },
};
#else
static crc_backend_entry_t s_crc_backends[] = {
    { "slice-by-8",  ChronosRT_crc32c_sw_raw, true },
};
#endif

#define CRC_BACKEND_COUNT (sizeof(s_crc_backends) / sizeof(s_crc_backends[0]))

static crc_backend_entry_t* crc_backend_find(const char* name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < CRC_BACKEND_COUNT; i++) {
        if (strcmp(s_crc_backends[i].name, name) == 0) {
            return &s_crc_backends[i];
        }
    }
    return NULL;
}

/*
 * Verifica un backend contra la referencia software con longitudes y semillas
 * variadas. Es la garantía de que una implementación SIMD solo se usa si
 * reproduce exactamente el CRC-32C esperado en ESTA CPU.
 */
static bool crc_backend_verify(crc_raw_fn fn) {
    /* Incluye longitudes pequeñas: la ruta adaptativa cambia de estrategia en
     * 64/128/256 B y todas deben dar el mismo CRC que la referencia. */
    static const size_t   lens[]  = { 1u, 3u, 7u, 8u, 16u, 31u, 32u, 48u, 60u, 63u, 64u,
                                      65u, 127u, 128u, 192u, 256u, 320u, 1024u, 1500u };
    static const uint32_t seeds[] = { 0u, 0xFFFFFFFFu, 0x12345678u };
    uint8_t buf[1536];
    uint32_t x = 0x9E3779B9u;

    for (size_t i = 0; i < sizeof(buf); i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)x;
    }
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
            if (fn(seeds[s], buf, lens[i]) != ChronosRT_crc32c_sw_raw(seeds[s], buf, lens[i])) {
                return false;
            }
        }
    }
    return true;
}


static void crc_activate(const char* name, crc_raw_fn fn) {
    s_crc_active = name;
    s_caps.crc32c_backend = name;
    s_caps.crc32c_hw = true;
    atomic_store_explicit(&s_crc_raw_fn, fn, memory_order_relaxed);
}

static void caps_init(void) {
    memset(&s_caps, 0, sizeof(s_caps));
    s_caps.crc32c_backend = "slice-by-8";
    s_caps.fec_backend    = "scalar";

    /* Las tablas SW también son la base de las tablas de salto multistream.
     * El esquema de 4 flujos se usa en x86-64 y ARMv8, así que las tablas se
     * construyen siempre (son ~9 KiB de L1 y ahorran el `idiv` del skip). */
    (void)pthread_once(&s_crc_once, crc_init);
    atomic_store_explicit(&s_crc_raw_fn, ChronosRT_crc32c_sw_raw, memory_order_relaxed);

    crc_build_skip(s_skip16, 16u);
    crc_build_skip(s_skip32, 32u);
    crc_build_skip(s_skip48, 48u);
    crc_build_skip(s_skip64, 64u);
    crc_build_skip(s_skip96, 96u);
    crc_build_skip(s_skip128, 128u);
    crc_build_skip(s_skip192, 192u);
    crc_build_skip(s_skip256, 256u);
    crc_build_skip(s_skip384, 384u);

#if CRT_X86
    cpu_detect_x86(&s_caps);

    if (s_caps.isa_avx2) {
        s_caps.fec_simd    = true;
        s_caps.fec_backend = "avx2";
    }

    /* Instrucción CRC32 en cuatro flujos (verificada contra la referencia). */
    if (s_caps.isa_sse42) {
        crc_backend_find("sse4.2-x4")->available = true;
        if (crc_backend_verify(crc_be_hw)) {
            crc_activate("sse4.2-x4", crc_be_hw);
        }
    }

#elif CRT_ARM64
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & (unsigned long)HWCAP_CRC32) != 0UL) {
        crc_backend_find("armv8-crc")->available = true;
        if (crc_backend_verify(crc_be_hw)) {
            crc_activate("armv8-crc", crc_be_hw);
        }
    }
    /* NEON es obligatorio en AArch64: el FEC siempre se vectoriza. */
    s_caps.fec_simd    = true;
    s_caps.fec_backend = "neon";
    if ((hwcap & (unsigned long)HWCAP_AES) != 0UL) {
        s_caps.armv8_crypto = true;
    }
#endif
}

const char* ChronosRT_crc32c_active_backend(void) {
    (void)pthread_once(&s_caps_once, caps_init);
    return s_crc_active;
}

uint32_t ChronosRT_crc32c_selftest(const char** first_failing) {
    (void)pthread_once(&s_caps_once, caps_init);
    if (first_failing != NULL) {
        *first_failing = NULL;
    }
    uint32_t failures = 0u;
    for (size_t i = 0; i < CRC_BACKEND_COUNT; i++) {
        if (!s_crc_backends[i].available) {
            continue;
        }
        if (!crc_backend_verify(s_crc_backends[i].fn)) {
            failures++;
            if (first_failing != NULL && *first_failing == NULL) {
                *first_failing = s_crc_backends[i].name;
            }
        }
    }
    return failures;
}

ChronosRT_status_t ChronosRT_crc32c_force_backend(const char* name) {
    (void)pthread_once(&s_caps_once, caps_init);
    crc_backend_entry_t* e = crc_backend_find(name);
    if (e == NULL || !e->available) {
        return ChronosRT_ERROR_NOTSUP;
    }
    s_crc_active = e->name;
    atomic_store_explicit(&s_crc_raw_fn, e->fn, memory_order_relaxed);
    return ChronosRT_SUCCESS;
}

const ChronosRT_hw_caps_t* ChronosRT_hw_caps(void) {
    (void)pthread_once(&s_caps_once, caps_init);
    return &s_caps;
}

uint32_t ChronosRT_crc32c_raw(uint32_t seed, const void* data, size_t len) {
    if (data == NULL || len == 0u) {
        return seed;
    }
    crc_raw_fn fn = atomic_load_explicit(&s_crc_raw_fn, memory_order_relaxed);
    if (__builtin_expect(fn == NULL, 0)) {
        (void)pthread_once(&s_caps_once, caps_init);
        fn = atomic_load_explicit(&s_crc_raw_fn, memory_order_relaxed);
    }
    return fn(seed, data, len);
}

uint32_t ChronosRT_crc32c(const void* data, size_t len) {
    return ~ChronosRT_crc32c_raw(0xFFFFFFFFu, data, len);
}

/* ================================================================== */
/* Multiplicación vectorial en GF(2^8) por una constante              */
/* ================================================================== */

static inline void xor_bytes(uint8_t* dst, const uint8_t* src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[i] ^= src[i];
    }
}

/* dst ^= src * coef, con coef constante para todo el bloque. */
static void xor_mul_scalar(uint8_t* dst, const uint8_t* src, size_t len,
                           const uint8_t tlo[16], const uint8_t thi[16]) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = src[i];
        dst[i] ^= (uint8_t)(tlo[b & 0x0Fu] ^ thi[b >> 4]);
    }
}

#if CRT_X86
__attribute__((target("avx2")))
static void xor_mul_avx2(uint8_t* dst, const uint8_t* src, size_t len,
                         const uint8_t tlo[16], const uint8_t thi[16]) {
    const __m256i vlo  = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)tlo));
    const __m256i vhi  = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)thi));
    const __m256i mask = _mm256_set1_epi8(0x0F);

    size_t i = 0;
    for (; i + 32u <= len; i += 32u) {
        __m256i s  = _mm256_loadu_si256((const __m256i*)(src + i));
        __m256i lo = _mm256_and_si256(s, mask);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(s, 4), mask);
        __m256i p  = _mm256_xor_si256(_mm256_shuffle_epi8(vlo, lo),
                                      _mm256_shuffle_epi8(vhi, hi));
        __m256i d  = _mm256_loadu_si256((const __m256i*)(dst + i));
        _mm256_storeu_si256((__m256i*)(dst + i), _mm256_xor_si256(d, p));
    }
    for (; i < len; i++) {
        uint8_t b = src[i];
        dst[i] ^= (uint8_t)(tlo[b & 0x0Fu] ^ thi[b >> 4]);
    }
}
#endif

#if CRT_ARM64
static void xor_mul_neon(uint8_t* dst, const uint8_t* src, size_t len,
                         const uint8_t tlo[16], const uint8_t thi[16]) {
    const uint8x16_t vlo  = vld1q_u8(tlo);
    const uint8x16_t vhi  = vld1q_u8(thi);
    const uint8x16_t mask = vdupq_n_u8(0x0F);

    size_t i = 0;
    for (; i + 16u <= len; i += 16u) {
        uint8x16_t s  = vld1q_u8(src + i);
        uint8x16_t lo = vandq_u8(s, mask);
        uint8x16_t hi = vandq_u8(vshrq_n_u8(s, 4), mask);
        uint8x16_t p  = veorq_u8(vqtbl1q_u8(vlo, lo), vqtbl1q_u8(vhi, hi));
        vst1q_u8(dst + i, veorq_u8(vld1q_u8(dst + i), p));
    }
    for (; i < len; i++) {
        uint8_t b = src[i];
        dst[i] ^= (uint8_t)(tlo[b & 0x0Fu] ^ thi[b >> 4]);
    }
}
#endif

static void xor_mul_const(uint8_t* dst, const uint8_t* src, size_t len, uint8_t coef) {
    if (coef == 0u || len == 0u) {
        return;
    }
    if (coef == 1u) {
        xor_bytes(dst, src, len);
        return;
    }

    uint8_t tlo[16];
    uint8_t thi[16];
    for (unsigned i = 0; i < 16u; i++) {
        tlo[i] = gf_mul((uint8_t)i, coef);
        thi[i] = gf_mul((uint8_t)(i << 4), coef);
    }

    const ChronosRT_hw_caps_t* caps = ChronosRT_hw_caps();
#if CRT_X86
    if (caps->fec_simd) {
        xor_mul_avx2(dst, src, len, tlo, thi);
        return;
    }
#elif CRT_ARM64
    if (caps->fec_simd) {
        xor_mul_neon(dst, src, len, tlo, thi);
        return;
    }
#else
    (void)caps;
#endif
    xor_mul_scalar(dst, src, len, tlo, thi);
}

/* ================================================================== */
/* Matriz generadora del código RS sistemático (cacheada)             */
/* ================================================================== */

#define RS_CACHE_SLOTS 4u

typedef struct {
    bool    valid;
    uint8_t k;
    uint8_t n;
    uint8_t gen[ChronosRT_RS_MAX_N][ChronosRT_RS_MAX_N];  /* k x n */
} rs_entry_t;

static rs_entry_t      s_rs_cache[RS_CACHE_SLOTS];
static unsigned        s_rs_next = 0u;
static pthread_mutex_t s_rs_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Invierte una matriz n x n sobre GF(2^8) por Gauss-Jordan. */
static bool gf_matrix_inverse(const uint8_t* src, uint8_t* dst, unsigned n) {
    uint8_t aug[ChronosRT_RS_MAX_N][2u * ChronosRT_RS_MAX_N];

    for (unsigned r = 0; r < n; r++) {
        for (unsigned c = 0; c < n; c++) {
            aug[r][c] = src[r * n + c];
            aug[r][n + c] = (r == c) ? 1u : 0u;
        }
    }

    for (unsigned col = 0; col < n; col++) {
        unsigned piv = col;
        while (piv < n && aug[piv][col] == 0u) {
            piv++;
        }
        if (piv == n) {
            return false;   /* matriz singular */
        }
        if (piv != col) {
            for (unsigned c = 0; c < 2u * n; c++) {
                uint8_t tmp = aug[col][c];
                aug[col][c] = aug[piv][c];
                aug[piv][c] = tmp;
            }
        }
        uint8_t inv = gf_inv(aug[col][col]);
        for (unsigned c = 0; c < 2u * n; c++) {
            aug[col][c] = gf_mul(aug[col][c], inv);
        }
        for (unsigned r = 0; r < n; r++) {
            if (r == col) {
                continue;
            }
            uint8_t f = aug[r][col];
            if (f == 0u) {
                continue;
            }
            for (unsigned c = 0; c < 2u * n; c++) {
                aug[r][c] ^= gf_mul(aug[col][c], f);
            }
        }
    }

    for (unsigned r = 0; r < n; r++) {
        for (unsigned c = 0; c < n; c++) {
            dst[r * n + c] = aug[r][n + c];
        }
    }
    return true;
}

/* Construye G = Vtop^-1 · V  (k x n), sistemática: G[:, 0..k-1] = I. */
static bool rs_build_generator(uint8_t k, uint8_t n,
                               uint8_t gen[ChronosRT_RS_MAX_N][ChronosRT_RS_MAX_N]) {
    uint8_t v[ChronosRT_RS_MAX_N][ChronosRT_RS_MAX_N];
    uint8_t vtop[ChronosRT_RS_MAX_N * ChronosRT_RS_MAX_N];
    uint8_t vinv[ChronosRT_RS_MAX_N * ChronosRT_RS_MAX_N];

    for (unsigned r = 0; r < k; r++) {
        for (unsigned c = 0; c < n; c++) {
            v[r][c] = s_gf_exp[(unsigned)((r * c) % 255u)];
        }
    }
    for (unsigned r = 0; r < k; r++) {
        for (unsigned c = 0; c < k; c++) {
            vtop[r * k + c] = v[r][c];
        }
    }
    if (!gf_matrix_inverse(vtop, vinv, k)) {
        return false;
    }
    for (unsigned r = 0; r < k; r++) {
        for (unsigned c = 0; c < n; c++) {
            uint8_t acc = 0u;
            for (unsigned l = 0; l < k; l++) {
                acc ^= gf_mul(vinv[r * k + l], v[l][c]);
            }
            gen[r][c] = acc;
        }
    }
    return true;
}

/*
 * Copia bajo mutex la matriz generadora k x n a `out` (k*n bytes).
 * La copia evita que otra hebra invalide la entrada de caché mientras se usa:
 * la matriz se consulta EN EL CAMINO CALIENTE y no puede quedar compartida.
 */
static bool rs_copy_generator(uint8_t k, uint8_t n, uint8_t* out) {
    /* Las tablas de GF(2^8) son imprescindibles para construir la matriz. */
    gf_ensure();

    (void)pthread_mutex_lock(&s_rs_mutex);

    rs_entry_t* found = NULL;
    for (unsigned i = 0; i < RS_CACHE_SLOTS; i++) {
        if (s_rs_cache[i].valid && s_rs_cache[i].k == k && s_rs_cache[i].n == n) {
            found = &s_rs_cache[i];
            break;
        }
    }

    if (found == NULL) {
        rs_entry_t* e = &s_rs_cache[s_rs_next];
        s_rs_next = (s_rs_next + 1u) % RS_CACHE_SLOTS;
        e->valid = false;
        if (!rs_build_generator(k, n, e->gen)) {
            (void)pthread_mutex_unlock(&s_rs_mutex);
            return false;
        }
        e->k = k;
        e->n = n;
        e->valid = true;
        found = e;
    }

    for (unsigned r = 0; r < k; r++) {
        memcpy(out + (size_t)r * n, found->gen[r], n);
    }

    (void)pthread_mutex_unlock(&s_rs_mutex);
    return true;
}

ChronosRT_status_t ChronosRT_rs_encode(uint8_t k, uint8_t n,
                                       const uint8_t* const* data,
                                       uint8_t* const* parity,
                                       uint32_t len) {
    if (k == 0u || n <= k || n > ChronosRT_RS_MAX_N || data == NULL || parity == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    if (len == 0u) {
        return ChronosRT_SUCCESS;
    }

    uint8_t gen[ChronosRT_RS_MAX_N * ChronosRT_RS_MAX_N];
    if (!rs_copy_generator(k, n, gen)) {
        return ChronosRT_ERROR_INIT;
    }

    for (unsigned j = 0; j < (unsigned)(n - k); j++) {
        if (parity[j] == NULL) {
            return ChronosRT_ERROR_INVALID;
        }
        memset(parity[j], 0, len);
        for (unsigned r = 0; r < k; r++) {
            if (data[r] == NULL) {
                return ChronosRT_ERROR_INVALID;
            }
            xor_mul_const(parity[j], data[r], len, gen[r * n + k + j]);
        }
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_rs_decode(uint8_t k, uint8_t n,
                                       uint8_t* const* blocks,
                                       const bool* present,
                                       uint32_t len) {
    if (k == 0u || n <= k || n > ChronosRT_RS_MAX_N ||
        blocks == NULL || present == NULL) {
        return ChronosRT_ERROR_INVALID;
    }

    uint8_t gen[ChronosRT_RS_MAX_N * ChronosRT_RS_MAX_N];
    if (!rs_copy_generator(k, n, gen)) {
        return ChronosRT_ERROR_INIT;
    }

    /* Columnas presentes. */
    uint8_t idx[ChronosRT_RS_MAX_N];
    unsigned have = 0u;
    for (unsigned c = 0; c < n; c++) {
        if (!present[c]) {
            continue;
        }
        if (blocks[c] == NULL) {
            return ChronosRT_ERROR_INVALID;
        }
        idx[have++] = (uint8_t)c;
    }
    if (have < k) {
        return ChronosRT_ERROR_CORRUPT;   /* borrados irrecuperables */
    }
    if (have == n) {
        return ChronosRT_SUCCESS;         /* nada que reconstruir */
    }

    /* A[t][r] = G[r][idx[t]];  m = A^-1 · y */
    uint8_t a[ChronosRT_RS_MAX_N * ChronosRT_RS_MAX_N];
    uint8_t ainv[ChronosRT_RS_MAX_N * ChronosRT_RS_MAX_N];
    for (unsigned t = 0; t < k; t++) {
        for (unsigned r = 0; r < k; r++) {
            a[t * k + r] = gen[r * n + idx[t]];
        }
    }
    if (!gf_matrix_inverse(a, ainv, k)) {
        return ChronosRT_ERROR_CORRUPT;
    }

    if (len == 0u) {
        return ChronosRT_SUCCESS;
    }

    uint8_t* msg = (uint8_t*)malloc((size_t)k * (size_t)len);
    if (msg == NULL) {
        return ChronosRT_ERROR_MEMORY;
    }

    for (unsigned r = 0; r < k; r++) {
        uint8_t* mr = msg + (size_t)r * len;
        memset(mr, 0, len);
        for (unsigned t = 0; t < k; t++) {
            uint8_t coef = ainv[r * k + t];
            if (coef != 0u) {
                xor_mul_const(mr, blocks[idx[t]], len, coef);
            }
        }
    }

    for (unsigned c = 0; c < n; c++) {
        if (present[c]) {
            continue;
        }
        if (blocks[c] == NULL) {
            free(msg);
            return ChronosRT_ERROR_INVALID;
        }
        memset(blocks[c], 0, len);
        for (unsigned r = 0; r < k; r++) {
            uint8_t coef = gen[r * n + c];
            if (coef != 0u) {
                xor_mul_const(blocks[c], msg + (size_t)r * len, len, coef);
            }
        }
    }

    free(msg);
    return ChronosRT_SUCCESS;
}

/* ================================================================== */
/* Timestamping                                                       */
/* ================================================================== */

/*
 * Reloj de dominio rápido (I-3). `clock_gettime(CLOCK_MONOTONIC)` cuesta
 * 20–25 ns (vDSO) y se llama una vez por evento y por trama. Cuando el kernel
 * ya deriva CLOCK_MONOTONIC del mismo contador (clocksource `tsc` en x86,
 * `arch_sys_counter` en ARM), se puede leer el contador directamente:
 *
 *     ns = base_ns + (counter - base_counter) * ns_per_cycle
 *
 * Se calibra una sola vez (50 ms) y queda publicado en variables atómicas
 * escritas antes de activar `s_clock_fast`; los lectores solo hacen una carga
 * relaxed. Si la plataforma no cumple las condiciones, se usa `clock_gettime`.
 */
static _Atomic uint64_t s_clock_base_ns  = 0;
static _Atomic uint64_t s_clock_base_cyc = 0;
static _Atomic uint64_t s_clock_ns_q32   = 0;   /* ns/ciclo × 2^32 */
static _Atomic int      s_clock_fast     = -1;  /* -1 sin decidir, 0 no, 1 sí */
static pthread_once_t   s_clock_once     = PTHREAD_ONCE_INIT;

static inline uint64_t hw_read_counter(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    /* rdtsc sin lfence: el compilador no reordena la lectura respecto a los
     * accesos del mismo hilo con la barrera de compilador de abajo. */
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t hw_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ¿El kernel calcula CLOCK_MONOTONIC con el mismo contador que leemos? */
static bool hw_clocksource_matches(void) {
    const char* want =
#if defined(__x86_64__)
        "tsc";
#elif defined(__aarch64__)
        "arch_sys_counter";
#else
        "";
#endif
    if (want[0] == '\0') {
        return false;
    }
    FILE* fp = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
    if (!fp) {
        return false;
    }
    char buf[64] = {0};
    const size_t got = fread(buf, 1, sizeof(buf) - 1u, fp);
    (void)fclose(fp);
    if (got == 0u) {
        return false;
    }
    return strncmp(buf, want, strlen(want)) == 0;
}

static void clock_init(void) {
    atomic_store_explicit(&s_clock_fast, 0, memory_order_relaxed);

    /* Escape hatch: permite forzar la ruta lenta (depuración o plataformas
     * con TSC poco fiable). */
    const char* off = getenv("CHRONOSRT_NO_FAST_CLOCK");
    if (off != NULL && off[0] != '\0' && off[0] != '0') {
        return;
    }

    const bool source_matches = hw_clocksource_matches();

#if defined(__x86_64__)
    /* TSC invariante (CPUID.80000007H:EDX[8]). Aunque el kernel prefiera otro
     * clocksource (p. ej. `hpet` en una VM), el TSC sigue siendo un contador
     * monótono de frecuencia fija y se calibra contra CLOCK_MONOTONIC. En un
     * arranque nativo el clocksource suele ser `tsc` y el error es cero. */
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(0x80000007u), "c"(0));
    const bool tsc_invariant = (edx & (1u << 8)) != 0u;
    if (!source_matches && !tsc_invariant) {
        return;
    }
#else
    if (!source_matches) {
        return;
    }
#endif

    /* Calibración: 50 ms miden el ratio con error relativo < 1 ppm. */
    const uint64_t c0 = hw_read_counter();
    const uint64_t t0 = hw_monotonic_ns();
    uint64_t t1 = t0;
    while (t1 - t0 < 50000000ULL) {
        t1 = hw_monotonic_ns();
    }
    const uint64_t c1 = hw_read_counter();
    const uint64_t cyc = c1 - c0;
    if (cyc < 1000u || t1 <= t0) {
        return;
    }

    const uint64_t span = t1 - t0;
    /* q32 = span * 2^32 / cyc, en 128 bits para no desbordar. */
    const uint64_t q32 = (uint64_t)(((__uint128_t)span << 32) / cyc);
    if (q32 == 0u) {
        return;
    }

    atomic_store_explicit(&s_clock_base_ns, t0, memory_order_relaxed);
    atomic_store_explicit(&s_clock_base_cyc, c0, memory_order_relaxed);
    atomic_store_explicit(&s_clock_ns_q32, q32, memory_order_relaxed);
    atomic_store_explicit(&s_clock_fast, 1, memory_order_release);
}

uint64_t ChronosRT_tx_timestamp_ns(void) {
    int fast = atomic_load_explicit(&s_clock_fast, memory_order_acquire);
    if (__builtin_expect(fast < 0, 0)) {
        (void)pthread_once(&s_clock_once, clock_init);
        fast = atomic_load_explicit(&s_clock_fast, memory_order_acquire);
    }
    if (__builtin_expect(fast == 1, 1)) {
        const uint64_t cyc   = hw_read_counter();
        const uint64_t base  = atomic_load_explicit(&s_clock_base_cyc, memory_order_relaxed);
        const uint64_t q32   = atomic_load_explicit(&s_clock_ns_q32, memory_order_relaxed);
        const uint64_t ns0   = atomic_load_explicit(&s_clock_base_ns, memory_order_relaxed);
        return ns0 + (uint64_t)(((__uint128_t)(cyc - base) * q32) >> 32);
    }
    return hw_monotonic_ns();
}

/* ================================================================== */
/* Capacidades ligadas a una interfaz de red (PHC / Launch Time)      */
/* ================================================================== */

ChronosRT_status_t ChronosRT_hw_caps_for_interface(const char* ifname,
                                                   ChronosRT_hw_caps_t* out) {
    if (ifname == NULL || ifname[0] == '\0' || out == NULL) {
        return ChronosRT_ERROR_INVALID;
    }

    *out = *ChronosRT_hw_caps();
    out->phc = false;
    out->launch_time = false;

#if defined(__linux__)
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return ChronosRT_ERROR_IO;
    }

    struct ethtool_ts_info info;
    memset(&info, 0, sizeof(info));
    info.cmd = ETHTOOL_GET_TS_INFO;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void*)&info;

    ChronosRT_status_t st = ChronosRT_ERROR_NOTSUP;
    if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
        uint32_t tx_hw = (info.so_timestamping & (uint32_t)SOF_TIMESTAMPING_TX_HARDWARE);
        out->phc = (tx_hw != 0u);
        /* El pacing por hardware (ETF/SO_TXTIME) se apoya en el reloj PHC. */
        out->launch_time = out->phc;
        st = ChronosRT_SUCCESS;
    }
    close(fd);
    return st;
#else
    return ChronosRT_ERROR_NOTSUP;
#endif
}
