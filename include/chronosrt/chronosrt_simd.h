#ifndef ChronosRT_SIMD_H
#define ChronosRT_SIMD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "chronosrt.h"

static inline uint32_t ChronosRT_simd_hash_fallback(const void* data, size_t size) {
    const uint32_t* ptr = (const uint32_t*)data;
    uint32_t hash = 0x45d9f3b;

    while (size >= 4) {
        hash ^= ptr[0];
        hash = (hash << 5) | (hash >> 27);
        hash = hash * 0x45d9f3b;
        ptr++;
        size -= 4;
    }

    return hash;
}

#if defined(ChronosRT_ARCH_X86_64)
    #include <immintrin.h>

    #if defined(__AVX512F__)
        #define ChronosRT_SIMD_AVX512 1
    #else
        #define ChronosRT_SIMD_AVX512 0
    #endif

    #if defined(__AVX2__)
        #define ChronosRT_SIMD_AVX2 1
    #else
        #define ChronosRT_SIMD_AVX2 0
    #endif

    #define ChronosRT_SIMD_SSE 1

    #if defined(__AVX512F__)
    typedef __m512i ChronosRT_vec512_t;
    #endif
    #if defined(__AVX2__)
    typedef __m256i ChronosRT_vec256_t;
    #endif

    static inline void ChronosRT_simd_memcpy_avx512(void* __restrict__ dst, const void* __restrict__ src, size_t size) {
    #if defined(__AVX512F__)
        char* d = (char*)dst;
        const char* s = (const char*)src;

        while (size >= 64) {
            __m512i v = _mm512_loadu_si512((const __m512i*)s);
            _mm512_storeu_si512((__m512i*)d, v);
            d += 64;
            s += 64;
            size -= 64;
        }

        while (size >= 32) {
            __m256i v = _mm256_loadu_si256((const __m256i*)s);
            _mm256_storeu_si256((__m256i*)d, v);
            d += 32;
            s += 32;
            size -= 32;
        }

        while (size >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i*)s);
            _mm_storeu_si128((__m128i*)d, v);
            d += 16;
            s += 16;
            size -= 16;
        }

        while (size >= 8) {
            *(uint64_t*)d = *(const uint64_t*)s;
            d += 8;
            s += 8;
            size -= 8;
        }

        while (size > 0) {
            *d++ = *s++;
            size--;
        }
    #else
        memcpy(dst, src, size);
    #endif
    }

    static inline void ChronosRT_simd_memcpy_avx2(void* __restrict__ dst, const void* __restrict__ src, size_t size) {
    #if defined(__AVX2__)
        char* d = (char*)dst;
        const char* s = (const char*)src;

        while (size >= 32) {
            __m256i v = _mm256_loadu_si256((const __m256i*)s);
            _mm256_storeu_si256((__m256i*)d, v);
            d += 32;
            s += 32;
            size -= 32;
        }

        while (size >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i*)s);
            _mm_storeu_si128((__m128i*)d, v);
            d += 16;
            s += 16;
            size -= 16;
        }

        while (size > 0) {
            *d++ = *s++;
            size--;
        }
    #else
        memcpy(dst, src, size);
    #endif
    }

    #if defined(__AVX512F__)
        #define ChronosRT_simd_memcpy ChronosRT_simd_memcpy_avx512
    #elif defined(__AVX2__)
        #define ChronosRT_simd_memcpy ChronosRT_simd_memcpy_avx2
    #else
        #define ChronosRT_simd_memcpy memcpy
    #endif

    static inline uint32_t ChronosRT_simd_hash_avx2(const void* data, size_t size) {
    #if defined(__AVX2__)
        const uint32_t* ptr = (const uint32_t*)data;
        __m256i hash_state = _mm256_set1_epi32(0x45d9f3b);

        while (size >= 32) {
            __m256i data_vec = _mm256_loadu_si256((const __m256i*)ptr);
            /* Mezcla de 32 bits en las OCHO lanes.
             * `_mm256_mul_epu32` (VPMULUDQ) solo multiplica las lanes pares y
             * descarta las impares: con ella 3/4 del buffer no influían en el
             * hash y el CRC de WAL/golden image no detectaba la corrupción. */
            hash_state = _mm256_mullo_epi32(hash_state, data_vec);
            hash_state = _mm256_xor_si256(hash_state, _mm256_srli_epi32(hash_state, 15));
            /* Reinyecta los datos: evita que una lane se colapse a 0. */
            hash_state = _mm256_add_epi32(hash_state, data_vec);
            ptr += 8;
            size -= 32;
        }

        uint32_t result[8];
        _mm256_storeu_si256((__m256i*)result, hash_state);

        /* Pliega TODAS las lanes (antes solo result[0] y result[4]). */
        uint32_t hash = result[0] ^ result[1] ^ result[2] ^ result[3] ^
                        result[4] ^ result[5] ^ result[6] ^ result[7];
        while (size >= 4) {
            hash ^= ptr[0];
            hash = (hash << 5) | (hash >> 27);
            hash = hash * 0x45d9f3b;
            ptr++;
            size -= 4;
        }

        return hash;
    #else
        return ChronosRT_simd_hash_fallback(data, size);
    #endif
    }

#elif defined(ChronosRT_ARCH_ARM64)
    #include <arm_neon.h>

    typedef uint32x4_t ChronosRT_vec128_t;
    typedef uint64x2_t ChronosRT_vec256_t;

    #define ChronosRT_SIMD_NEON 1
    #define ChronosRT_SIMD_SVE 0

    static inline void ChronosRT_simd_memcpy_neon(void* __restrict__ dst, const void* __restrict__ src, size_t size) {
        char* d = (char*)dst;
        const char* s = (const char*)src;

        while (size >= 32) {
            uint64x2_t v0 = vld1q_u64((const uint64_t*)s);
            uint64x2_t v1 = vld1q_u64((const uint64_t*)(s + 16));
            vst1q_u64((uint64_t*)d, v0);
            vst1q_u64((uint64_t*)(d + 16), v1);
            d += 32;
            s += 32;
            size -= 32;
        }

        while (size >= 16) {
            uint64x2_t v = vld1q_u64((const uint64_t*)s);
            vst1q_u64((uint64_t*)d, v);
            d += 16;
            s += 16;
            size -= 16;
        }

        while (size >= 8) {
            *(uint64_t*)d = *(const uint64_t*)s;
            d += 8;
            s += 8;
            size -= 8;
        }

        while (size > 0) {
            *d++ = *s++;
            size--;
        }
    }

    #define ChronosRT_simd_memcpy ChronosRT_simd_memcpy_neon

    static inline uint32_t ChronosRT_simd_hash_neon(const void* data, size_t size) {
        const uint32_t* ptr = (const uint32_t*)data;
        uint32x4_t hash_state = vdupq_n_u32(0x45d9f3b);

        while (size >= 16) {
            uint32x4_t data_vec = vld1q_u32(ptr);
            hash_state = vmulq_u32(hash_state, data_vec);
            hash_state = veorq_u32(hash_state, vshrq_n_u32(hash_state, 15));
            /* Reinyecta los datos: evita que una lane se colapse a 0. */
            hash_state = vaddq_u32(hash_state, data_vec);
            ptr += 4;
            size -= 16;
        }

        uint32_t result[4];
        vst1q_u32(result, hash_state);

        uint32_t hash = result[0] ^ result[1] ^ result[2] ^ result[3];
        while (size >= 4) {
            hash ^= ptr[0];
            hash = (hash << 5) | (hash >> 27);
            hash = hash * 0x45d9f3b;
            ptr++;
            size -= 4;
        }

        return hash;
    }

#else
    #define ChronosRT_SIMD_NEON 0
    #define ChronosRT_SIMD_AVX512 0
    #define ChronosRT_SIMD_AVX2 0

    static inline void ChronosRT_simd_memcpy_fallback(void* __restrict__ dst, const void* __restrict__ src, size_t size) {
        memcpy(dst, src, size);
    }
    #define ChronosRT_simd_memcpy ChronosRT_simd_memcpy_fallback
#endif

CHRONOSRT_API void ChronosRT_simd_batch_zero(void* ptr, size_t count);
CHRONOSRT_API void ChronosRT_simd_batch_copy(void* dst, const void* src, size_t count);
CHRONOSRT_API uint32_t ChronosRT_simd_hash(const void* data, size_t size);

#endif
