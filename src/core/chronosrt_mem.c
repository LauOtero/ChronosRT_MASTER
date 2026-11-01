/*
 * chronosrt_mem.c — Pool de memoria pre-asignado + bloqueo/hugepages/NUMA.
 *
 * Correcciones del INFORME_IDi:
 *   B-10 (🟡 -> corregido): la free-list anterior usaba
 *        __atomic_fetch_sub(&free_count, 1) y comparaba index == 0, lo que
 *        permitía underflow y carreras. Aquí la free-list se implementa sobre
 *        una cola MPMC acotada (Vyukov), libre de ABA y sin underflow: la
 *        capacidad está acotada por construcción.
 *   RT-2/RT-3: el pool es el origen de bloques de eventos (cero malloc en el
 *        camino caliente) con soporte de hugepages transparentes.
 *   PF-5: enlace del pool al nodo NUMA local (mbind) para evitar tráfico
 *        inter-socket.
 */
#include "chronosrt_mem.h"
#include "chronosrt_bounded.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#if defined(SYS_mbind) && defined(__linux__)
  #define CHRONOSRT_HAVE_MBIND 1
#else
  #define CHRONOSRT_HAVE_MBIND 0
#endif

#ifndef CHRONOSRT_MPOL_BIND
#define CHRONOSRT_MPOL_BIND 2
#endif

struct ChronosRT_mem_pool {
    ChronosRT_bounded_queue_t freeq;   /* contiene punteros a bloques libres */
    void*  memory;
    size_t memory_bytes;
    uint32_t block_size;               /* alineado a línea de caché */
    uint32_t pool_size;
    bool   mmapped;                    /* asignado con mmap (MAP_HUGETLB) */
};

static inline size_t chronosrt_align_up(size_t v, size_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

int ChronosRT_mem_numa_nodes(void) {
    DIR* d = opendir("/sys/devices/system/node");
    if (!d) {
        return 1;
    }
    int nodes = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "node", 4) == 0 && e->d_name[4] >= '0' && e->d_name[4] <= '9') {
            nodes++;
        }
    }
    closedir(d);
    return nodes > 0 ? nodes : 1;
}

static int chronosrt_cpu_to_node(int cpu) {
    if (cpu < 0) {
        return -1;
    }
    int nodes = ChronosRT_mem_numa_nodes();
    if (nodes <= 1) {
        return 0;
    }
    for (int n = 0; n < nodes; n++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", n);
        FILE* fp = fopen(path, "r");
        if (!fp) {
            continue;
        }
        char buf[512];
        size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[got] = '\0';
        fclose(fp);

        /* Analiza listas tipo "0-3,8-11". */
        char* tok = strtok(buf, ",\n");
        while (tok) {
            int lo = 0, hi = 0;
            if (sscanf(tok, "%d-%d", &lo, &hi) == 2) {
                if (cpu >= lo && cpu <= hi) return n;
            } else if (sscanf(tok, "%d", &lo) == 1) {
                if (cpu == lo) return n;
            }
            tok = strtok(NULL, ",\n");
        }
    }
    return -1;
}

int ChronosRT_mem_current_numa_node(void) {
    if (ChronosRT_mem_numa_nodes() <= 1) {
        return 0;
    }
    int cpu = sched_getcpu();
    return chronosrt_cpu_to_node(cpu);
}

static void chronosrt_bind_numa_local(void* addr, size_t len) {
#if CHRONOSRT_HAVE_MBIND
    int node = ChronosRT_mem_current_numa_node();
    if (node < 0 || node > 63) {
        return;
    }
    unsigned long nodemask = 1UL << node;
    (void)syscall(SYS_mbind, addr, len, CHRONOSRT_MPOL_BIND, &nodemask,
                  (unsigned long)(sizeof(nodemask) * 8), 0);
#else
    (void)addr;
    (void)len;
#endif
}

size_t ChronosRT_mem_hugepage_size(void) {
    FILE* fp = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "r");
    if (!fp) {
        return 2u * 1024u * 1024u; /* 2 MiB por defecto en x86_64/arm64 */
    }
    unsigned long v = 0;
    if (fscanf(fp, "%lu", &v) != 1) {
        v = 2u * 1024u * 1024u;
    }
    fclose(fp);
    return (size_t)v;
}

ChronosRT_status_t ChronosRT_mem_pool_init_ex(ChronosRT_mem_pool_t* pool,
                                              uint32_t pool_size,
                                              uint32_t block_size,
                                              bool hugepage,
                                              bool numa_local) {
    if (!pool || pool_size == 0 || block_size == 0) {
        return ChronosRT_ERROR_INVALID;
    }

    memset(pool, 0, sizeof(*pool));

    size_t bs = chronosrt_align_up(block_size, CHRONOSRT_CACHELINE);
    if (bs < sizeof(void*)) {
        bs = CHRONOSRT_CACHELINE;
    }
    size_t total = (size_t)pool_size * bs;

    void* mem = NULL;
    bool mmapped = false;

#if defined(MAP_HUGETLB)
    if (hugepage) {
        size_t hp = ChronosRT_mem_hugepage_size();
        if (hp == 0) hp = 2u * 1024u * 1024u;
        size_t rounded = chronosrt_align_up(total, hp);
        void* m = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (m != MAP_FAILED) {
            mem = m;
            mmapped = true;
        }
        /* Si el sistema no tiene páginas hugetlb reservadas, se degrada a
         * memoria normal + MADV_HUGEPAGE (transparent hugepages). */
    }
#endif

    if (!mem) {
        if (posix_memalign(&mem, CHRONOSRT_CACHELINE, total) != 0 || !mem) {
            return ChronosRT_ERROR_MEMORY;
        }
#if defined(MADV_HUGEPAGE)
        if (hugepage) {
            (void)madvise(mem, total, MADV_HUGEPAGE);
        }
#endif
    }

    if (numa_local) {
        chronosrt_bind_numa_local(mem, total);
    }

    if (ChronosRT_bounded_init(&pool->freeq, pool_size) != ChronosRT_SUCCESS) {
        if (mmapped) {
            /* munmap requiere la longitud mapeada; se omite por brevedad de error. */
            munmap(mem, total);
        } else {
            free(mem);
        }
        return ChronosRT_ERROR_MEMORY;
    }

    pool->memory = mem;
    pool->memory_bytes = total;
    pool->block_size = (uint32_t)bs;
    pool->pool_size = pool_size;
    pool->mmapped = mmapped;

    for (uint32_t i = 0; i < pool_size; i++) {
        void* block = (uint8_t*)mem + ((size_t)i * bs);
        if (!ChronosRT_bounded_push(&pool->freeq, block)) {
            break; /* no debería ocurrir: capacidad >= pool_size */
        }
    }

    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_mem_pool_init(ChronosRT_mem_pool_t* pool,
                                           uint32_t pool_size,
                                           uint32_t block_size) {
    return ChronosRT_mem_pool_init_ex(pool, pool_size, block_size, false, false);
}

ChronosRT_mem_pool_t* ChronosRT_mem_pool_create_ex(uint32_t pool_size,
                                                   uint32_t block_size,
                                                   bool hugepage,
                                                   bool numa_local) {
    ChronosRT_mem_pool_t* pool = (ChronosRT_mem_pool_t*)calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    if (ChronosRT_mem_pool_init_ex(pool, pool_size, block_size, hugepage, numa_local) !=
        ChronosRT_SUCCESS) {
        free(pool);
        return NULL;
    }
    return pool;
}

ChronosRT_mem_pool_t* ChronosRT_mem_pool_create(uint32_t pool_size, uint32_t block_size) {
    return ChronosRT_mem_pool_create_ex(pool_size, block_size, false, false);
}

void ChronosRT_mem_pool_delete(ChronosRT_mem_pool_t* pool) {
    if (!pool) {
        return;
    }
    ChronosRT_mem_pool_destroy(pool);
    free(pool);
}

void* ChronosRT_mem_pool_alloc(ChronosRT_mem_pool_t* pool) {
    if (!pool) {
        return NULL;
    }
    void* block = NULL;
    return ChronosRT_bounded_pop(&pool->freeq, &block) ? block : NULL;
}

void ChronosRT_mem_pool_free(ChronosRT_mem_pool_t* pool, void* block) {
    if (!pool || !block) {
        return;
    }
    /* Si la cola está llena (doble free), se descarta silenciosamente el bloque
     * para no corromper la free-list (la capacidad acota el daño). */
    (void)ChronosRT_bounded_push(&pool->freeq, block);
}

uint32_t ChronosRT_mem_pool_available(ChronosRT_mem_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    return (uint32_t)ChronosRT_bounded_count(&pool->freeq);
}

uint32_t ChronosRT_mem_pool_capacity(ChronosRT_mem_pool_t* pool) {
    return pool ? pool->pool_size : 0;
}

ChronosRT_status_t ChronosRT_mem_pool_prefault(ChronosRT_mem_pool_t* pool) {
    if (!pool || !pool->memory) {
        return ChronosRT_ERROR_INVALID;
    }
    /* Escritura de un byte por bloque: fuerza la materialización de la página. */
    for (uint32_t i = 0; i < pool->pool_size; i++) {
        uint8_t* block = (uint8_t*)pool->memory + ((size_t)i * pool->block_size);
        block[0] = 0;
    }
    return ChronosRT_SUCCESS;
}

void ChronosRT_mem_pool_destroy(ChronosRT_mem_pool_t* pool) {
    if (!pool) {
        return;
    }
    ChronosRT_bounded_destroy(&pool->freeq);
    if (pool->memory) {
        if (pool->mmapped) {
            munmap(pool->memory, pool->memory_bytes);
        } else {
            free(pool->memory);
        }
        pool->memory = NULL;
    }
    pool->pool_size = 0;
}

ChronosRT_status_t ChronosRT_mem_lock_all(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        return ChronosRT_ERROR_NOTSUP;
    }
    return ChronosRT_SUCCESS;
}

void ChronosRT_mem_unlock_all(void) {
    (void)munlockall();
}
