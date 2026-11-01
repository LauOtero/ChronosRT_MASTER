#include "chronosrt_simd.h"
#include <string.h>
#include <stdlib.h>

#if defined(ChronosRT_ARCH_X86_64)
    #include <immintrin.h>

    void ChronosRT_simd_batch_zero(void* ptr, size_t count) {
        char* p = (char*)ptr;
        size_t i = 0;

    #if defined(__AVX512F__)
        for (; i + 64 <= count; i += 64) {
            _mm512_storeu_si512((__m512i*)(p + i), _mm512_setzero_si512());
        }
    #endif
    #if defined(__AVX2__)
        for (; i + 32 <= count; i += 32) {
            _mm256_storeu_si256((__m256i*)(p + i), _mm256_setzero_si256());
        }
    #endif
        for (; i + 16 <= count; i += 16) {
            _mm_storeu_si128((__m128i*)(p + i), _mm_setzero_si128());
        }
        for (; i < count; i++) {
            p[i] = 0;
        }
    }

    void ChronosRT_simd_batch_copy(void* dst, const void* src, size_t count) {
        ChronosRT_simd_memcpy(dst, src, count);
    }

#elif defined(ChronosRT_ARCH_ARM64)
    #include <arm_neon.h>

    void ChronosRT_simd_batch_zero(void* ptr, size_t count) {
        char* p = (char*)ptr;
        size_t i = 0;

        for (; i + 32 <= count; i += 32) {
            vst1q_u64((uint64_t*)(p + i), vdupq_n_u64(0));
            vst1q_u64((uint64_t*)(p + i + 16), vdupq_n_u64(0));
        }
        for (; i + 16 <= count; i += 16) {
            vst1q_u64((uint64_t*)(p + i), vdupq_n_u64(0));
        }
        for (; i < count; i++) {
            p[i] = 0;
        }
    }

    void ChronosRT_simd_batch_copy(void* dst, const void* src, size_t count) {
        ChronosRT_simd_memcpy(dst, src, count);
    }

#else

    void ChronosRT_simd_batch_zero(void* ptr, size_t count) {
        memset(ptr, 0, count);
    }

    void ChronosRT_simd_batch_copy(void* dst, const void* src, size_t count) {
        memcpy(dst, src, count);
    }

#endif

uint32_t ChronosRT_simd_hash(const void* data, size_t size) {
#if defined(ChronosRT_ARCH_X86_64)
    return ChronosRT_simd_hash_avx2(data, size);
#elif defined(ChronosRT_ARCH_ARM64)
    return ChronosRT_simd_hash_neon(data, size);
#else
    return ChronosRT_simd_hash_fallback(data, size);
#endif
}
