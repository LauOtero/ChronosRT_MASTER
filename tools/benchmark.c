#include "chronosrt.h"
#include "chronosrt_hwaccel.h"
#include "chronosrt_ethertux.h"
#include "chronosrt_ethercat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Sumidero para que el compilador no elimine los cuerpos medidos. */
static volatile uint32_t g_sink;

/* Ventana de medida por operación (80 ms) y coste del bucle vacío descontado. */
#define BENCH_WINDOW_NS 80000000ull

static double g_loop_ns;

#define BENCH(label, body) do {                                        \
    const uint64_t dl_ = now_ns() + BENCH_WINDOW_NS;                   \
    const uint64_t t0_ = now_ns();                                     \
    uint64_t n_ = 0u;                                                  \
    while (now_ns() < dl_) {                                           \
        for (unsigned k_ = 0; k_ < 128u; k_++) { body; }               \
        n_ += 128u;                                                    \
    }                                                                  \
    const double per_ = (double)(now_ns() - t0_) / (double)n_;         \
    printf("  %-40s %9.2f ns\n", label, per_ - g_loop_ns);             \
} while (0)

static double bench_loop_overhead(void) {
    const uint64_t dl = now_ns() + BENCH_WINDOW_NS;
    const uint64_t t0 = now_ns();
    uint64_t n = 0u;
    while (now_ns() < dl) {
        for (unsigned k = 0; k < 128u; k++) {
            g_sink = g_sink + 1u;
        }
        n += 128u;
    }
    return (double)(now_ns() - t0) / (double)n;
}

/*
 * Coste del esquema de ID local: Nivel 1 (camino crítico) y Nivel 2 (admisión),
 * más el filtro de destino del maestro.
 */
static void bench_aalid(void) {
    static uint8_t macs[64][6];
    uint8_t out[6];
    uint16_t id = 0u;
    uint32_t k = 0u;

    g_loop_ns = bench_loop_overhead();

    for (unsigned i = 0; i < 64u; i++) {
        macs[i][0] = 0x00u;
        macs[i][1] = 0x1Au;
        macs[i][2] = 0x2Bu;
        macs[i][3] = 0xC0u;
        macs[i][4] = (uint8_t)(i >> 8);
        macs[i][5] = (uint8_t)i;
    }

    printf("Nivel 1 (camino crítico de recepción):\n");
    BENCH("ID candidato (CRC-32C plegado)", {
        g_sink += ChronosRT_et_aalid_candidate(macs[k++ & 63u]);
    });
    BENCH("ID candidato (multiplicativo)", {
        g_sink += ChronosRT_et_aalid_candidate_mul(macs[k++ & 63u]);
    });
    BENCH("MAC local <- ID (LAA)", {
        g_sink += ChronosRT_et_aalid_mac((uint16_t)k++, out);
    });
    BENCH("ID del origen a partir de la MAC", {
        g_sink += ChronosRT_et_aalid_id_from_mac(out);
    });
    BENCH("backoff determinista (64 slots)", {
        g_sink += ChronosRT_et_aalid_backoff_slots(macs[k++ & 63u], 64u);
    });

    printf("\nNivel 2 (admisión, fuera del superframe):\n");
    ChronosRT_et_aalid_config_t acfg;
    ChronosRT_et_aalid_default_config(&acfg);
    acfg.table_slots = 8192u;          /* holgado para 4096 nodos de prueba */
    ChronosRT_et_aalid_t* aal = ChronosRT_et_aalid_create(&acfg);
    if (aal != NULL) {
        ChronosRT_et_aalid_result_t res;
        uint8_t mnuevo[6];
        memset(mnuevo, 0, sizeof(mnuevo));
        mnuevo[1] = 0x1Au;
        mnuevo[2] = 0x2Bu;
        mnuevo[3] = 0xD0u;

        /* Admisiones nuevas: se mide el total de 4096 altas (no cabe en BENCH
         * porque cada MAC solo puede admitirse una vez). */
        const uint64_t t0 = now_ns();
        for (uint32_t i = 0; i < 4096u; i++) {
            mnuevo[4] = (uint8_t)(i >> 8);
            mnuevo[5] = (uint8_t)i;
            (void)ChronosRT_et_aalid_admit(aal, mnuevo, i + 1u, 0u, &id, &res);
        }
        printf("  %-40s %9.1f ns\n", "admisión de un nodo nuevo (4096 altas)",
               (double)(now_ns() - t0) / 4096.0);

        BENCH("reclamación de un nodo conocido", {
            g_sink += ChronosRT_et_aalid_admit(aal, macs[0], 1u, 0u, &id, &res);
        });
        BENCH("búsqueda {MAC -> ID} con 4096 nodos", {
            g_sink += ChronosRT_et_aalid_lookup(aal, macs[k++ & 63u], &id);
        });

        /* Ciclo completo con liberación explícita (recorre la tabla). */
        acfg.table_slots = 256u;       /* tabla típica de una SBC */
        ChronosRT_et_aalid_t* aal2 = ChronosRT_et_aalid_create(&acfg);
        static uint8_t mciclo[6];
        memset(mciclo, 0, sizeof(mciclo));
        mciclo[1] = 0x1Au;
        mciclo[2] = 0x2Bu;
        mciclo[3] = 0xE0u;
        if (aal2 != NULL) {
            BENCH("ciclo admisión + liberación explícita", {
                mciclo[4] = (uint8_t)(k >> 8);
                mciclo[5] = (uint8_t)k;
                g_sink += ChronosRT_et_aalid_admit(aal2, mciclo, k, 0u, &id, &res);
                (void)ChronosRT_et_aalid_release(aal2, id);
                k++;
            });
            ChronosRT_et_aalid_destroy(aal2);
        }
        ChronosRT_et_aalid_destroy(aal);
    }

    ChronosRT_et_master_t* m = ChronosRT_et_master_create();
    if (m != NULL) {
        (void)ChronosRT_et_master_set_local_id(m, 0x0102u);
        printf("\nFiltro del maestro:\n");
        BENCH("filtro de destino (unicast propio)", {
            uint8_t self[6];
            (void)ChronosRT_et_aalid_mac(0x0102u, self);
            g_sink += ChronosRT_et_master_accepts_destination(m, self);
        });
        ChronosRT_et_master_destroy(m);
    }
    printf("\n");
}

/*
 * Coste de las rutas puras de trama (sin NIC): construir, construir sin copiar
 * la carga útil y parsear + validar, en ambos protocolos, más las operaciones
 * auxiliares (Flow Aggregator y decisión de admisión QoS).
 */
static void bench_frame(void) {
    static uint8_t payload[1478];
    static uint8_t pdu[ChronosRT_ET_MAX_FRAME];
    static uint8_t hdr[ChronosRT_ET_HEADER_SIZE];
    static uint8_t agg[1478];
    static const struct { uint32_t bytes; const char* label; } sizes[] = {
        {   64u, "64 B"   },
        {  512u, "512 B"  },
        { 1464u, "1464 B" },
    };

    g_loop_ns = bench_loop_overhead();

    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }

    ChronosRT_et_header_t h;
    memset(&h, 0, sizeof(h));
    h.type     = ChronosRT_ET_TYPE_DATA;
    h.priority = ChronosRT_ET_P2;
    h.flow_id  = 1u;
    h.seq_num  = 1u;
    h.state    = ChronosRT_ET_STATE_OP;
    h.flow_count = 1u;

    printf("EtherTux v0.1.0 (cabecera de 26 B):\n");
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        const uint16_t len = (uint16_t)sizes[s].bytes;
        char label[96];

        (void)snprintf(label, sizeof(label), "construir PDU (%s, con copia)", sizes[s].label);
        BENCH(label, { g_sink += ChronosRT_et_pdu_build(pdu, sizeof(pdu), &h, payload, len); });

        (void)snprintf(label, sizeof(label), "construir cabecera (%s, sin copia)", sizes[s].label);
        BENCH(label, { g_sink += ChronosRT_et_pdu_build_header(hdr, &h, payload, len); });

        const uint32_t total = ChronosRT_et_pdu_build(pdu, sizeof(pdu), &h, payload, len);
        ChronosRT_et_header_t hp;
        const uint8_t* pl = NULL;
        (void)snprintf(label, sizeof(label), "parsear + validar CRC (%s)", sizes[s].label);
        BENCH(label, { g_sink += (uint32_t)ChronosRT_et_pdu_parse(pdu, total, &hp, &pl); });
    }

    /* Flow Aggregator: 8 flujos de 32 B en una sola trama de 320 B. */
    ChronosRT_et_flow_entry_t entries[8];
    for (unsigned i = 0; i < 8u; i++) {
        entries[i].flow_id = (uint8_t)(i + 1u);
        entries[i].len     = 32u;
        entries[i].data    = payload;
    }
    uint32_t agg_len = 0u;
    BENCH("Flow Aggregator: 8 flujos x 32 B", {
        g_sink += (uint32_t)ChronosRT_et_flowagg_build(entries, 8u, agg, sizeof(agg), &agg_len);
    });
    ChronosRT_et_flow_entry_t views[8];
    uint32_t nviews = 0u;
    BENCH("Flow Aggregator: parsear 8 flujos", {
        g_sink += (uint32_t)ChronosRT_et_flowagg_parse(agg, agg_len, views, 8u, &nviews);
    });

    /* Extremo a extremo: serializar la lista y construir UNA trama, frente a
     * construir las 8 tramas por separado. */
    BENCH("Flow Aggregator: serializar + PDU agregada", {
        uint32_t l_ = 0u;
        (void)ChronosRT_et_flowagg_build(entries, 8u, agg, sizeof(agg), &l_);
        g_sink += ChronosRT_et_pdu_build(pdu, sizeof(pdu), &h, agg, (uint16_t)l_);
    });
    BENCH("8 PDU individuales de 32 B", {
        for (unsigned j_ = 0; j_ < 8u; j_++) {
            g_sink += ChronosRT_et_pdu_build(pdu, sizeof(pdu), &h, payload, 32u);
        }
    });

    /* Decisión de admisión QoS (token bucket) con reabastecimiento continuo. */
    ChronosRT_et_qos_t* qos = ChronosRT_et_qos_create();
    if (qos != NULL) {
        ChronosRT_et_qos_config_t qcfg;
        ChronosRT_et_qos_default_config(&qcfg);
        for (unsigned i = 0; i < ChronosRT_ET_PRIORITIES; i++) {
            qcfg.bucket[i].rate_bytes_per_sec = 1000000000ull;
        }
        (void)ChronosRT_et_qos_configure(qos, &qcfg);
        uint64_t t = now_ns();
        BENCH("admisión QoS (P2, 576 B)", {
            t += 1000u;
            g_sink += (uint32_t)ChronosRT_et_qos_admit(qos, ChronosRT_ET_P2, 576u, t);
        });
        ChronosRT_et_qos_destroy(qos);
    }

    /* Ventana TDMA activa en un superframe de 1 ms. */
    ChronosRT_et_sched_t* sc = ChronosRT_et_sched_create(1000000u, 1000000000ull);
    if (sc != NULL) {
        uint64_t t = 0u;
        BENCH("ventana TDMA activa (superframe 1 ms)", {
            t += 1000u;
            g_sink += ChronosRT_et_sched_window_at(sc, t);
        });
        ChronosRT_et_sched_destroy(sc);
    }

    printf("\nEtherCAT (datagrama de 10 B + datos):\n");
    static const uint16_t ec_sizes[] = { 32u, 256u, 1024u };
    for (size_t s = 0; s < sizeof(ec_sizes) / sizeof(ec_sizes[0]); s++) {
        const uint16_t len = ec_sizes[s];
        char label[96];
        static uint8_t ec[2048];
        ChronosRT_ec_datagram_t dg;

        (void)snprintf(label, sizeof(label), "construir datagrama (%u B)", len);
        BENCH(label, {
            g_sink += ChronosRT_ec_datagram_build(ec, sizeof(ec), 0x0Cu, 1u, 0x1000u,
                                                  payload, len, false);
        });
        const uint32_t ec_len = ChronosRT_ec_datagram_build(ec, sizeof(ec), 0x0Cu, 1u, 0x1000u,
                                                            payload, len, false);
        (void)snprintf(label, sizeof(label), "parsear datagrama (%u B)", len);
        BENCH(label, { g_sink += ChronosRT_ec_datagram_parse(ec, ec_len, &dg); });
    }
    printf("\n");
}

/*
 * Micro-benchmark del CRC-32C: recorre cada backend disponible y mide GByte/s y
 * tramas/s para los tamaños de carga útil típicos de EtherTux. Es la métrica que
 * se cita en el README.
 */
static void bench_crc(uint32_t seconds) {
    static uint8_t buf[4096];
    static const struct { uint32_t payload; const char* label; } sizes[] = {
        { 20u,   "trama mínima (P0)" },
        { 512u,  "telemetría"        },
        { 1464u, "MTU completa"      },
    };
    static const char* names[] = { "armv8-crc", "sse4.2-x4", "slice-by-8" };

    const ChronosRT_hw_caps_t* caps = ChronosRT_hw_caps();
    printf("CPU: %s / %s (family %u, model %u, stepping %u)\n",
           caps->cpu_vendor, caps->cpu_brand,
           caps->cpu_family, caps->cpu_model, caps->cpu_stepping);
    printf("CRC-32C activo: %s   FEC: %s\n\n", caps->crc32c_backend, caps->fec_backend);

    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 31u + 7u);
    }

    const char* active = ChronosRT_crc32c_active_backend();
    for (size_t b = 0; b < sizeof(names) / sizeof(names[0]); b++) {
        if (ChronosRT_crc32c_force_backend(names[b]) != ChronosRT_SUCCESS) {
            continue;
        }
        printf("%s:\n", names[b]);
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            const size_t len = (size_t)ChronosRT_ET_ETH_HDR_SIZE
                             + (size_t)ChronosRT_ET_HEADER_SIZE
                             + (size_t)sizes[s].payload;
            const uint64_t deadline = now_ns() + (uint64_t)seconds * 1000000000ull;
            uint64_t done = 0u;
            uint32_t acc = 0u;

            const uint64_t t0 = now_ns();
            do {
                for (unsigned k = 0; k < 256u; k++) {
                    acc ^= ChronosRT_crc32c_raw(0xFFFFFFFFu, buf, len);
                }
                done += 256u;
            } while (now_ns() < deadline && (acc != 0xDEADBEEFu));

            const uint64_t dt = now_ns() - t0;
            const double secs = (double)dt / 1e9;
            const double gbs = ((double)done * (double)len) / secs / 1e9;
            const double mfps = (double)done / secs / 1e6;
            printf("  %-24s %6zu B  %6.2f GB/s  %8.2f M tramas/s\n",
                   sizes[s].label, len, gbs, mfps);
        }
        printf("\n");
    }
    (void)ChronosRT_crc32c_force_backend(active);
}

static void print_usage(const char* program) {
    printf("ChronosRT Benchmark Tool\n");
    printf("Usage: %s [options]\n", program);
    printf("\nOptions:\n");
    printf("  --mode MODE       Benchmark mode: latency, throughput, jitter, stress, wcet,\n");
    printf("                    crc (CRC-32C), frame (construir/parsear) o aalid (ID local)\n");
    printf("  --duration SEC    Test duration in seconds (default: 10)\n");
    printf("  --threads N       Number of threads for multi-threaded tests (default: 4)\n");
    printf("  --pool-size N     Event pool size (default: 65536)\n");
    printf("  --hugepages       Use hugepages for memory\n");
    printf("  --no-simd         Disable SIMD optimizations\n");
    printf("  --no-affinity     Disable CPU affinity\n");
    printf("  --platform PLAT   Target platform: rpi4, rpi5, jetson_nano, jetson_orin, intel_nuc, orange_pi5, rock5b, x86_64_rt\n");
    printf("  --help            Show this help message\n");
    printf("\nPlatforms:\n");
    printf("  rpi4       - Raspberry Pi 4 (ARM64, Cortex-A72)\n");
    printf("  rpi5       - Raspberry Pi 5 (ARM64, Cortex-A76)\n");
    printf("  jetson_nano- NVIDIA Jetson Nano (ARM64)\n");
    printf("  jetson_orin- NVIDIA Jetson Orin (ARM64 + GPU)\n");
    printf("  intel_nuc  - Intel NUC (x86_64)\n");
    printf("  orange_pi5 - Orange Pi 5 (ARM64, RK3588)\n");
    printf("  rock5b     - ROCK 5B (ARM64, RK3588)\n");
    printf("  x86_64_rt  - Generic x86_64 with PREEMPT_RT\n");
}

static ChronosRT_platform_t parse_platform(const char* str) {
    if (strcmp(str, "rpi4") == 0) return ChronosRT_PLATFORM_RPI4;
    if (strcmp(str, "rpi5") == 0) return ChronosRT_PLATFORM_RPI5;
    if (strcmp(str, "jetson_nano") == 0) return ChronosRT_PLATFORM_JETSON_NANO;
    if (strcmp(str, "jetson_orin") == 0) return ChronosRT_PLATFORM_JETSON_ORIN;
    if (strcmp(str, "intel_nuc") == 0) return ChronosRT_PLATFORM_INTEL_NUC;
    if (strcmp(str, "orange_pi5") == 0) return ChronosRT_PLATFORM_ORANGE_PI5;
    if (strcmp(str, "rock5b") == 0) return ChronosRT_PLATFORM_ROCK5B;
    if (strcmp(str, "x86_64_rt") == 0) return ChronosRT_PLATFORM_X86_64_RT;
    return ChronosRT_PLATFORM_GENERIC;
}

static ChronosRT_benchmark_mode_t parse_mode(const char* str) {
    if (strcmp(str, "latency") == 0) return ChronosRT_MODE_LATENCY;
    if (strcmp(str, "throughput") == 0) return ChronosRT_MODE_THROUGHPUT;
    if (strcmp(str, "jitter") == 0) return ChronosRT_MODE_JITTER;
    if (strcmp(str, "stress") == 0) return ChronosRT_MODE_STRESS;
    if (strcmp(str, "wcet") == 0) return ChronosRT_MODE_WCET;
    return ChronosRT_MODE_LATENCY;
}

int main(int argc, char* argv[]) {
    ChronosRT_config_t config = {0};
    ChronosRT_benchmark_mode_t mode = ChronosRT_MODE_LATENCY;
    uint32_t duration = 10;
    uint32_t threads = 4;
    bool crc_mode = false;
    bool frame_mode = false;
    bool aalid_mode = false;
    config.event_pool_size = 65536;
    config.batch_size = 256;
    config.stack_size_kb = 256;
    config.use_hugepages = false;
    config.use_simd = true;
    config.use_cpu_affinity = true;
    config.use_numa_aware = true;
    /* Páginas bloqueadas y prefallocadas: elimina los page faults del camino
     * caliente, que son la causa típica de picos de decenas de µs. */
    config.lock_memory = true;
    /* El benchmark es multihilo: la cola SPSC no es segura con varios
     * productores. Se usa la MPMC acotada (Vyukov). */
    config.use_bounded_mpmc = true;
    config.platform = ChronosRT_detect_platform();
    
    static struct option long_options[] = {
        {"mode", required_argument, 0, 'm'},
        {"duration", required_argument, 0, 'd'},
        {"threads", required_argument, 0, 't'},
        {"pool-size", required_argument, 0, 'p'},
        {"hugepages", no_argument, 0, 'H'},
        {"no-simd", no_argument, 0, 'S'},
        {"no-affinity", no_argument, 0, 'A'},
        {"platform", required_argument, 0, 'P'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "m:d:t:p:HSAP:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'm':
                if (strcmp(optarg, "crc") == 0) {
                    crc_mode = true;
                } else if (strcmp(optarg, "frame") == 0) {
                    frame_mode = true;
                } else if (strcmp(optarg, "aalid") == 0) {
                    aalid_mode = true;
                } else {
                    mode = parse_mode(optarg);
                }
                break;
            case 'd':
                duration = atoi(optarg);
                break;
            case 't':
                threads = atoi(optarg);
                break;
            case 'p':
                config.event_pool_size = atoi(optarg);
                break;
            case 'H':
                config.use_hugepages = true;
                break;
            case 'S':
                config.use_simd = false;
                break;
            case 'A':
                config.use_cpu_affinity = false;
                break;
            case 'P':
                config.platform = parse_platform(optarg);
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return c == 'h' ? 0 : 1;
        }
    }
    
    printf("ChronosRT Benchmark v%s\n", ChronosRT_VERSION);
    printf("Platform: %d\n", config.platform);

    if (crc_mode) {
        bench_crc(duration);
        return 0;
    }
    if (frame_mode) {
        bench_frame();
        return 0;
    }
    if (aalid_mode) {
        bench_aalid();
        return 0;
    }

    printf("Mode: %d\n", mode);
    printf("Duration: %us\n", duration);
    /* Hilos efectivos: latencia/jitter/WCET usan uno; throughput exige 2. */
    {
        uint32_t effective = threads;
        if (mode == ChronosRT_MODE_LATENCY || mode == ChronosRT_MODE_JITTER ||
            mode == ChronosRT_MODE_WCET) {
            effective = 1u;
        } else if (mode == ChronosRT_MODE_THROUGHPUT && effective < 2u) {
            effective = 2u;
        }
        printf("Threads: %u\n", effective);
    }
    printf("SIMD: %s\n", config.use_simd ? "enabled" : "disabled");
    printf("CPU Affinity: %s\n", config.use_cpu_affinity ? "enabled" : "disabled");
    printf("Hugepages: %s\n", config.use_hugepages ? "enabled" : "disabled");
    printf("\n");
    
    ChronosRT_handle_t handle;
    ChronosRT_status_t status = ChronosRT_init(&handle, &config);
    if (status != ChronosRT_SUCCESS) {
        fprintf(stderr, "Failed to initialize ChronosRT: %d\n", status);
        return 1;
    }
    
    ChronosRT_metrics_t metrics;
    status = ChronosRT_benchmark_run_ex(handle, mode, duration, threads, &metrics);
    if (status != ChronosRT_SUCCESS) {
        fprintf(stderr, "Benchmark failed: %d\n", status);
        ChronosRT_shutdown(handle);
        return 1;
    }
    
    ChronosRT_metrics_report(&metrics);
    
    ChronosRT_shutdown(handle);
    
    return 0;
}

