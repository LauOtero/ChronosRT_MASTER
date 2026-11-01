#ifndef CHRONOSRT_MEM_H
#define CHRONOSRT_MEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "chronosrt.h"

typedef struct ChronosRT_mem_pool ChronosRT_mem_pool_t;

/* --- Pool pre-asignado (free-list lock-free, sin ABA) --------------------- */

CHRONOSRT_API ChronosRT_status_t ChronosRT_mem_pool_init(ChronosRT_mem_pool_t* pool,
                                                         uint32_t pool_size,
                                                         uint32_t block_size);

/* Creación/liberación sobre heap (el tipo es opaco para el consumidor). */
CHRONOSRT_API ChronosRT_mem_pool_t* ChronosRT_mem_pool_create(uint32_t pool_size, uint32_t block_size);
CHRONOSRT_API ChronosRT_mem_pool_t* ChronosRT_mem_pool_create_ex(uint32_t pool_size,
                                                                uint32_t block_size,
                                                                bool hugepage,
                                                                bool numa_local);
CHRONOSRT_API void                  ChronosRT_mem_pool_delete(ChronosRT_mem_pool_t* pool);

/* RT-3 (hugepages) + PF-5 (NUMA-aware): variante extendida. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_mem_pool_init_ex(ChronosRT_mem_pool_t* pool,
                                                            uint32_t pool_size,
                                                            uint32_t block_size,
                                                            bool hugepage,
                                                            bool numa_local);

CHRONOSRT_API void*    ChronosRT_mem_pool_alloc(ChronosRT_mem_pool_t* pool);
CHRONOSRT_API void     ChronosRT_mem_pool_free(ChronosRT_mem_pool_t* pool, void* block);
CHRONOSRT_API void     ChronosRT_mem_pool_destroy(ChronosRT_mem_pool_t* pool);
CHRONOSRT_API uint32_t ChronosRT_mem_pool_available(ChronosRT_mem_pool_t* pool);
CHRONOSRT_API uint32_t ChronosRT_mem_pool_capacity(ChronosRT_mem_pool_t* pool);

/* Pre-toca todos los bloques para eliminar page faults en el camino caliente. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_mem_pool_prefault(ChronosRT_mem_pool_t* pool);

/* --- RT-3: bloqueo de memoria y hugepages --------------------------------- */

/* mlockall(MCL_CURRENT|MCL_FUTURE): elimina page faults del proceso RT. */
CHRONOSRT_API ChronosRT_status_t ChronosRT_mem_lock_all(void);
CHRONOSRT_API void               ChronosRT_mem_unlock_all(void);

/* Tamaño de la hugepage transparente del sistema (0 si no disponible). */
CHRONOSRT_API size_t ChronosRT_mem_hugepage_size(void);

/* --- PF-5: NUMA ----------------------------------------------------------- */

/* Nodo NUMA de la CPU actual (-1 si no determinable o no NUMA). */
CHRONOSRT_API int ChronosRT_mem_current_numa_node(void);
/* Número de nodos NUMA visibles (1 si no NUMA). */
CHRONOSRT_API int ChronosRT_mem_numa_nodes(void);

#endif
