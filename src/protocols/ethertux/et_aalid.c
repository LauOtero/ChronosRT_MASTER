/*
 * et_aalid.c — ID local determinista para EtherTux (MAC LAA derivada del ID).
 *
 * El esquema tiene DOS niveles con responsabilidades separadas:
 *
 *   Nivel 1 (camino crítico, receptor): `ChronosRT_et_aalid_candidate()` deriva
 *   un ID de 16 bits de la MAC con CRC-32C y plegado XOR. Es una función sin
 *   ramas ni bucles, de coste constante, que sirve como FILTRO de recepción.
 *   No garantiza unicidad: con 16 bits, la paradoja del cumpleaños hace que
 *   aparezca una colisión con probabilidad > 50 % hacia los ~300 nodos.
 *
 *   Nivel 2 (solo admisión, fuera del superframe): el maestro es la AUTORIDAD.
 *   Mantiene {MAC de fábrica -> ID} en una tabla hash abierta y un bitmap de
 *   IDs ocupados; resuelve colisiones con un cursor secuencial determinista.
 *   El resultado es único en régimen permanente.
 *
 * La identidad en el cable se codifica en una MAC administrada localmente:
 *
 *     0x02 : 0x00 : 0x00 : 0x00 : ID[15:8] : ID[7:0]
 *
 * El bit U/L a 1 (LAA) y el I/G a 0 (unicast) hacen que un switch comercial
 * aprenda la entrada CAM del nodo y conmute EXCLUSIVAMENTE a su puerto. Con el
 * heartbeat periódico la entrada no expira y desaparece el flooding.
 *
 * Determinismo: ninguna decisión usa `rand()` ni temporizadores implícitos. El
 * backoff de admisión se deriva del propio CRC de la MAC, de modo que dos
 * ejecuciones con la misma secuencia de llegadas producen la misma asignación.
 */
#include "et_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ET_AALID_MAGIC    0x45544944u   /* "ETID" */
#define ET_AALID_VERSION  1u

#define ET_AALID_FREE  0u
#define ET_AALID_ACTIVE 1u
#define ET_AALID_TOMB  2u

typedef struct {
    uint8_t  mac[6];
    uint16_t id;
    uint8_t  state;
    uint8_t  pad;
    uint32_t nonce;
    uint64_t last_hb_ms;
} et_aalid_entry_t;

struct ChronosRT_et_aalid {
    ChronosRT_et_aalid_config_t cfg;
    et_aalid_entry_t*           table;    /* hash abierto {MAC -> ID}       */
    uint32_t                    mask;     /* table_slots - 1                */
    uint8_t*                    bitmap;   /* IDs ocupados (1 bit por ID)    */
    uint32_t                    bm_bytes;
    uint32_t                    id_count; /* id_max + 1                     */
    uint16_t                    next_free_hint;
    uint32_t                    occupied;
    ChronosRT_et_aalid_stats_t  stats;
};

/* ------------------------------------------------------------------ */
/* MAC local (LAA)                                                     */
/* ------------------------------------------------------------------ */

uint32_t ChronosRT_et_aalid_mac(uint16_t id, uint8_t out[6]) {
    if (out == NULL) {
        return 0u;
    }
    out[0] = 0x02u;                    /* LAA + unicast */
    out[1] = 0x00u;
    out[2] = 0x00u;
    out[3] = 0x00u;
    out[4] = (uint8_t)(id >> 8);
    out[5] = (uint8_t)(id & 0xFFu);
    return 6u;
}

uint16_t ChronosRT_et_aalid_id_from_mac(const uint8_t mac[6]) {
    if (mac == NULL) {
        return ChronosRT_ET_LOCAL_ID_NONE;
    }
    if (mac[0] != 0x02u || mac[1] != 0x00u || mac[2] != 0x00u || mac[3] != 0x00u) {
        return ChronosRT_ET_LOCAL_ID_NONE;
    }
    return (uint16_t)(((uint16_t)mac[4] << 8) | (uint16_t)mac[5]);
}

/* ------------------------------------------------------------------ */
/* Nivel 1: ID candidato (camino crítico)                              */
/* ------------------------------------------------------------------ */

uint16_t ChronosRT_et_aalid_candidate(const uint8_t mac[6]) {
    if (mac == NULL) {
        return ChronosRT_ET_LOCAL_ID_NONE;
    }
    /* CRC-32C por la HAL (instrucción si existe) y plegado XOR de las mitades:
     * la truncación simple descartaría la mitad alta del hash. */
    const uint32_t crc = ChronosRT_crc32c_raw(0xFFFFFFFFu, mac, 6u);
    return (uint16_t)(crc ^ (crc >> 16));
}

uint16_t ChronosRT_et_aalid_candidate_mul(const uint8_t mac[6]) {
    if (mac == NULL) {
        return ChronosRT_ET_LOCAL_ID_NONE;
    }
    uint64_t x = 0u;
    memcpy(&x, mac, 6u);   /* los dos bytes altos quedan a cero */
    return (uint16_t)((x * 0x9E3779B97F4A7C15ull) >> 48);
}

uint32_t ChronosRT_et_aalid_backoff_slots(const uint8_t mac[6], uint32_t slots) {
    if (mac == NULL || slots == 0u) {
        return 0u;
    }
    const uint32_t h = (uint32_t)ChronosRT_et_aalid_candidate(mac);
    if ((slots & (slots - 1u)) == 0u) {
        return h & (slots - 1u);       /* potencia de 2: máscara */
    }
    return h % slots;
}

/* ------------------------------------------------------------------ */
/* Bitmap de IDs                                                       */
/* ------------------------------------------------------------------ */

static inline bool bm_test(const uint8_t* bm, uint16_t id) {
    return ((bm[id >> 3] >> (id & 7u)) & 1u) != 0u;
}

static inline void bm_set(uint8_t* bm, uint16_t id) {
    bm[id >> 3] = (uint8_t)(bm[id >> 3] | (uint8_t)(1u << (id & 7u)));
}

static inline void bm_clear(uint8_t* bm, uint16_t id) {
    bm[id >> 3] = (uint8_t)(bm[id >> 3] & (uint8_t)~(1u << (id & 7u)));
}

/*
 * Primer ID libre a partir de `start`, con envoltura. El cursor hace que en el
 * caso típico (asignaciones correlativas) baste una iteración; el peor caso
 * está acotado por el número de bytes del bitmap.
 */
static uint16_t et_aalid_find_free(const ChronosRT_et_aalid_t* a, uint16_t start) {
    const uint32_t limit = a->cfg.id_max;
    const uint32_t from = ((uint32_t)start > limit) ? 0u : (uint32_t)start;
    const uint32_t first_byte = from >> 3;
    const uint8_t  from_mask = (uint8_t)(0xFFu << (from & 7u));

    /* Dos pasadas: desde el cursor hasta el final y, si no hay, desde el inicio. */
    for (uint32_t pass = 0; pass < 2u; pass++) {
        const uint32_t begin = (pass == 0u) ? first_byte : 0u;
        const uint32_t end = (pass == 0u) ? a->bm_bytes : first_byte;

        for (uint32_t i = begin; i < end; i++) {
            uint8_t inv = (uint8_t)~a->bitmap[i];
            if (pass == 0u && i == first_byte) {
                /* Descarta los IDs anteriores al cursor dentro de su byte. */
                inv = (uint8_t)(inv & from_mask);
            }
            if (inv == 0u) {
                continue;
            }
            const uint32_t id = (i << 3) + (uint32_t)__builtin_ctz((unsigned)inv);
            /* Descarta los bits de relleno del último byte. */
            return (id > limit) ? ChronosRT_ET_LOCAL_ID_NONE : (uint16_t)id;
        }
    }
    return ChronosRT_ET_LOCAL_ID_NONE;
}

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

void ChronosRT_et_aalid_default_config(ChronosRT_et_aalid_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->id_max      = (uint16_t)ChronosRT_ET_LOCAL_ID_MAX;
    cfg->grace_ms    = 600000u;   /* 10 min sin heartbeat */
    cfg->table_slots = 256u;
}

ChronosRT_et_aalid_t* ChronosRT_et_aalid_create(const ChronosRT_et_aalid_config_t* cfg) {
    ChronosRT_et_aalid_config_t c;
    if (cfg != NULL) {
        c = *cfg;
    } else {
        ChronosRT_et_aalid_default_config(&c);
    }

    uint32_t slots = c.table_slots;
    if (slots == 0u) {
        slots = 256u;
    }
    if (slots < 16u) {
        slots = 16u;
    }
    if (slots > 65536u) {
        slots = 65536u;
    }
    /* Redondea a potencia de 2 para indexar con máscara. */
    uint32_t p2 = 16u;
    while (p2 < slots) {
        p2 <<= 1u;
    }
    slots = p2;

    ChronosRT_et_aalid_t* a = (ChronosRT_et_aalid_t*)calloc(1u, sizeof(*a));
    if (a == NULL) {
        return NULL;
    }

    a->cfg = c;
    a->cfg.table_slots = slots;
    if (a->cfg.id_max == 0u) {
        a->cfg.id_max = (uint16_t)ChronosRT_ET_LOCAL_ID_MAX;
    }

    a->mask = slots - 1u;
    a->table = (et_aalid_entry_t*)calloc(slots, sizeof(et_aalid_entry_t));
    if (a->table == NULL) {
        free(a);
        return NULL;
    }

    a->id_count = (uint32_t)a->cfg.id_max + 1u;
    a->bm_bytes = (a->id_count + 7u) >> 3;
    a->bitmap = (uint8_t*)calloc(a->bm_bytes, 1u);
    if (a->bitmap == NULL) {
        free(a->table);
        free(a);
        return NULL;
    }
    return a;
}

void ChronosRT_et_aalid_destroy(ChronosRT_et_aalid_t* aalid) {
    if (aalid == NULL) {
        return;
    }
    free(aalid->bitmap);
    free(aalid->table);
    free(aalid);
}

/* ------------------------------------------------------------------ */
/* Nivel 2: admisión determinista                                      */
/* ------------------------------------------------------------------ */

static void et_aalid_insert(ChronosRT_et_aalid_t* a, uint32_t idx, const uint8_t mac[6],
                            uint16_t id, uint32_t nonce, uint64_t now_ms) {
    et_aalid_entry_t* e = &a->table[idx];
    memcpy(e->mac, mac, 6u);
    e->id         = id;
    e->state      = ET_AALID_ACTIVE;
    e->nonce      = nonce;
    e->last_hb_ms = now_ms;
}

ChronosRT_status_t ChronosRT_et_aalid_admit(ChronosRT_et_aalid_t* aalid,
                                            const uint8_t mac[6],
                                            uint32_t nonce,
                                            uint64_t now_ms,
                                            uint16_t* out_id,
                                            ChronosRT_et_aalid_result_t* out_result) {
    if (out_result != NULL) {
        *out_result = ChronosRT_ET_AALID_INVALID;
    }
    if (aalid == NULL || mac == NULL || out_id == NULL) {
        return ChronosRT_ERROR_INVALID;
    }

    const uint32_t h0 = (uint32_t)ChronosRT_et_aalid_candidate(mac) & aalid->mask;
    int32_t tomb = -1;

    for (uint32_t probe = 0; probe <= aalid->mask; probe++) {
        const uint32_t idx = (h0 + probe) & aalid->mask;
        et_aalid_entry_t* e = &aalid->table[idx];

        if (e->state == ET_AALID_ACTIVE) {
            if (memcmp(e->mac, mac, 6u) == 0) {
                if (e->nonce != nonce) {
                    /* Misma MAC y distinto nonce: identidad clonada. */
                    aalid->stats.clone_mac++;
                    if (out_result != NULL) {
                        *out_result = ChronosRT_ET_AALID_CLONE;
                    }
                    return ChronosRT_ERROR_STATE;
                }
                e->last_hb_ms = now_ms;
                aalid->stats.duplicates++;
                *out_id = e->id;
                if (out_result != NULL) {
                    *out_result = ChronosRT_ET_AALID_DUPLICATE;
                }
                return ChronosRT_SUCCESS;
            }
        } else if (e->state == ET_AALID_TOMB && tomb < 0) {
            tomb = (int32_t)idx;
        } else if (e->state == ET_AALID_FREE) {
            break;   /* fin de la cadena de sondeo */
        }
    }

    /* ID final: candidato si está libre; si no, cursor secuencial determinista. */
    const uint16_t cand = ChronosRT_et_aalid_candidate(mac);
    uint16_t id = ChronosRT_ET_LOCAL_ID_NONE;
    bool collision = false;

    if ((uint32_t)cand <= aalid->cfg.id_max && !bm_test(aalid->bitmap, cand)) {
        id = cand;
    } else {
        id = et_aalid_find_free(aalid, aalid->next_free_hint);
        collision = true;
    }
    if (id == ChronosRT_ET_LOCAL_ID_NONE) {
        if (out_result != NULL) {
            *out_result = ChronosRT_ET_AALID_FULL;
        }
        return ChronosRT_ERROR_FULL;
    }

    uint32_t slot = 0u;
    if (tomb >= 0) {
        slot = (uint32_t)tomb;
    } else {
        /* Reutiliza la ranura FREE encontrada en el sondeo. */
        for (uint32_t probe = 0; probe <= aalid->mask; probe++) {
            slot = (h0 + probe) & aalid->mask;
            if (aalid->table[slot].state == ET_AALID_FREE) {
                break;
            }
        }
    }

    et_aalid_insert(aalid, slot, mac, id, nonce, now_ms);
    bm_set(aalid->bitmap, id);
    aalid->occupied++;
    aalid->next_free_hint = (uint16_t)(((uint32_t)id + 1u > aalid->cfg.id_max) ? 0u : (id + 1u));

    aalid->stats.admissions++;
    if (collision) {
        aalid->stats.collisions++;
    }
    if (out_result != NULL) {
        *out_result = collision ? ChronosRT_ET_AALID_COLLISION : ChronosRT_ET_AALID_OK;
    }
    *out_id = id;
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_aalid_release(ChronosRT_et_aalid_t* aalid, uint16_t id) {
    if (aalid == NULL || id > aalid->cfg.id_max) {
        return ChronosRT_ERROR_INVALID;
    }
    if (!bm_test(aalid->bitmap, id)) {
        return ChronosRT_ERROR_NOTFOUND;
    }
    /* Libera la entrada {MAC -> ID}: el sondeo lineal tolera tombstones. */
    for (uint32_t i = 0; i <= aalid->mask; i++) {
        et_aalid_entry_t* e = &aalid->table[i];
        if (e->state == ET_AALID_ACTIVE && e->id == id) {
            e->state = ET_AALID_TOMB;
            break;
        }
    }
    bm_clear(aalid->bitmap, id);
    aalid->occupied--;
    aalid->stats.released++;
    if ((uint32_t)id < (uint32_t)aalid->next_free_hint) {
        aalid->next_free_hint = id;   /* reutiliza el hueco cuanto antes */
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_aalid_lookup(const ChronosRT_et_aalid_t* aalid,
                                             const uint8_t mac[6], uint16_t* out_id) {
    if (aalid == NULL || mac == NULL || out_id == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    const uint32_t h0 = (uint32_t)ChronosRT_et_aalid_candidate(mac) & aalid->mask;
    for (uint32_t probe = 0; probe <= aalid->mask; probe++) {
        const et_aalid_entry_t* e = &aalid->table[(h0 + probe) & aalid->mask];
        if (e->state == ET_AALID_FREE) {
            return ChronosRT_ERROR_NOTFOUND;
        }
        if (e->state == ET_AALID_ACTIVE && memcmp(e->mac, mac, 6u) == 0) {
            *out_id = e->id;
            return ChronosRT_SUCCESS;
        }
    }
    return ChronosRT_ERROR_NOTFOUND;
}

ChronosRT_status_t ChronosRT_et_aalid_heartbeat(ChronosRT_et_aalid_t* aalid, uint16_t id,
                                                uint64_t now_ms) {
    if (aalid == NULL || id > aalid->cfg.id_max) {
        return ChronosRT_ERROR_INVALID;
    }
    for (uint32_t i = 0; i <= aalid->mask; i++) {
        et_aalid_entry_t* e = &aalid->table[i];
        if (e->state == ET_AALID_ACTIVE && e->id == id) {
            e->last_hb_ms = now_ms;
            return ChronosRT_SUCCESS;
        }
    }
    return ChronosRT_ERROR_NOTFOUND;
}

uint32_t ChronosRT_et_aalid_expire(ChronosRT_et_aalid_t* aalid, uint64_t now_ms) {
    if (aalid == NULL) {
        return 0u;
    }
    const uint64_t grace = (uint64_t)aalid->cfg.grace_ms;
    uint32_t freed = 0u;

    for (uint32_t i = 0; i <= aalid->mask; i++) {
        et_aalid_entry_t* e = &aalid->table[i];
        if (e->state != ET_AALID_ACTIVE) {
            continue;
        }
        if (now_ms <= e->last_hb_ms) {
            continue;   /* reloj no monotónico o entrada recién refrescada */
        }
        if ((now_ms - e->last_hb_ms) <= grace) {
            continue;
        }
        bm_clear(aalid->bitmap, e->id);
        e->state = ET_AALID_TOMB;
        aalid->occupied--;
        aalid->stats.expired++;
        freed++;
        if ((uint32_t)e->id < (uint32_t)aalid->next_free_hint) {
            aalid->next_free_hint = e->id;
        }
    }
    return freed;
}

ChronosRT_status_t ChronosRT_et_aalid_stats(const ChronosRT_et_aalid_t* aalid,
                                            ChronosRT_et_aalid_stats_t* out) {
    if (aalid == NULL || out == NULL) {
        return ChronosRT_ERROR_INVALID;
    }
    *out = aalid->stats;
    out->occupied = aalid->occupied;
    return ChronosRT_SUCCESS;
}

bool ChronosRT_et_aalid_is_occupied(const ChronosRT_et_aalid_t* aalid, uint16_t id) {
    if (aalid == NULL || id > aalid->cfg.id_max) {
        return false;
    }
    return bm_test(aalid->bitmap, id);
}

/* ------------------------------------------------------------------ */
/* Persistencia {MAC -> ID}                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
} et_aalid_file_hdr_t;

typedef struct {
    uint8_t  mac[6];
    uint16_t id;
    uint32_t nonce;
    uint32_t reserved;
    uint64_t last_hb_ms;
} et_aalid_file_rec_t;

ChronosRT_status_t ChronosRT_et_aalid_save(const ChronosRT_et_aalid_t* aalid, const char* path) {
    if (aalid == NULL || path == NULL || path[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }

    char tmp[1024];
    const int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return ChronosRT_ERROR_INVALID;
    }

    FILE* f = fopen(tmp, "wb");
    if (f == NULL) {
        return ChronosRT_ERROR_IO;
    }

    et_aalid_file_hdr_t hdr;
    hdr.magic    = ET_AALID_MAGIC;
    hdr.version  = ET_AALID_VERSION;
    hdr.count    = aalid->occupied;
    hdr.reserved = 0u;

    bool ok = (fwrite(&hdr, sizeof(hdr), 1u, f) == 1u);
    for (uint32_t i = 0; ok && i <= aalid->mask; i++) {
        const et_aalid_entry_t* e = &aalid->table[i];
        if (e->state != ET_AALID_ACTIVE) {
            continue;
        }
        et_aalid_file_rec_t rec;
        memcpy(rec.mac, e->mac, 6u);
        rec.id         = e->id;
        rec.nonce      = e->nonce;
        rec.reserved   = 0u;
        rec.last_hb_ms = e->last_hb_ms;
        ok = (fwrite(&rec, sizeof(rec), 1u, f) == 1u);
    }

    if (ok && fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)remove(tmp);
        return ChronosRT_ERROR_IO;
    }
    /* Reemplazo atómico: un corte no deja el fichero a medias. */
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return ChronosRT_ERROR_IO;
    }
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ChronosRT_et_aalid_load(ChronosRT_et_aalid_t* aalid, const char* path) {
    if (aalid == NULL || path == NULL || path[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return ChronosRT_ERROR_IO;
    }

    et_aalid_file_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1u, f) != 1u || hdr.magic != ET_AALID_MAGIC ||
        hdr.version != ET_AALID_VERSION) {
        (void)fclose(f);
        return ChronosRT_ERROR_CORRUPT;
    }

    /* Estado limpio antes de reconstruir la identidad persistida. */
    memset(aalid->table, 0, (size_t)(aalid->mask + 1u) * sizeof(et_aalid_entry_t));
    memset(aalid->bitmap, 0, aalid->bm_bytes);
    aalid->occupied = 0u;
    aalid->next_free_hint = 0u;

    ChronosRT_status_t st = ChronosRT_SUCCESS;
    for (uint32_t i = 0; i < hdr.count; i++) {
        et_aalid_file_rec_t rec;
        if (fread(&rec, sizeof(rec), 1u, f) != 1u) {
            st = ChronosRT_ERROR_CORRUPT;
            break;
        }
        if (rec.id > aalid->cfg.id_max) {
            continue;
        }
        const uint32_t h0 = (uint32_t)ChronosRT_et_aalid_candidate(rec.mac) & aalid->mask;
        for (uint32_t probe = 0; probe <= aalid->mask; probe++) {
            const uint32_t idx = (h0 + probe) & aalid->mask;
            if (aalid->table[idx].state != ET_AALID_ACTIVE) {
                et_aalid_insert(aalid, idx, rec.mac, rec.id, rec.nonce, rec.last_hb_ms);
                bm_set(aalid->bitmap, rec.id);
                aalid->occupied++;
                break;
            }
        }
    }
    (void)fclose(f);
    return st;
}
