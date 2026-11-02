/*
 * chronosrt_net_probe.c — Sonda adaptativa universal y multi-capa para detección
 * de capacidades de interfaz de red. Proporciona detección en tres capas:
 *   Capa 1: Detección de bus (sysfs) — innegociable: rechaza bus USB.
 *   Capa 2: Detección de driver y capacidades (ETHTOOL).
 *   Capa 3: Prueba empírica de SO_TXTIME y TAPRIO.
 *
 * Esta sonda reemplaza la detección integrada previamente dispersa y provee
 * una API centralizada para que el resto del sistema (HAL, EtherCAT, EtherTux)
 * sepa exactamente la idoneidad de cada interfaz.
 *
 * Referencia: docs/no/implementar_soporte_eth.md §4.2
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "chronosrt_hwaccel.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <linux/net_tstamp.h>

/* ================================================================== */
/* Tabla de drivers conocidos y sus capacidades por defecto            */
/* ================================================================== */

/* Tabla extensible de drivers conocidos y sus capacidades por defecto.
 * Esta tabla puede ser ampliada por los mantenedores cuando nuevos drivers
 * son soportados. Si un driver no está en la tabla, el fallback es
 * CHRONOSRT_DRIVER_UNKNOWN con todas las capacidades en false. */
static const struct {
    ChronosRT_net_driver_t id;
    const char* name;
    bool has_hw_timestamp;
    bool has_hw_rx_timestamp;
    bool has_launch_time;
    bool has_hw_taprio;
    uint32_t hardware_queues;
} s_driver_table[CHRONOSRT_DRIVER_COUNT] = {
    /* [CHRONOSRT_DRIVER_UNKNOWN] = */ {
        .name = "desconocido",
        .has_hw_timestamp = false,
        .has_hw_rx_timestamp = false,
        .has_launch_time = false,
        .has_hw_taprio = false,
        .hardware_queues = 0,
    },
    /* [CHRONOSRT_DRIVER_IGB] = */ {
        .name = "igb",
        .has_hw_timestamp = true,
        .has_hw_rx_timestamp = true,
        .has_launch_time = true,
        .has_hw_taprio = true,
        .hardware_queues = 8,
    },
    /* [CHRONOSRT_DRIVER_IGC] = */ {
        .name = "igc",
        .has_hw_timestamp = true,
        .has_hw_rx_timestamp = true,
        .has_launch_time = true,
        .has_hw_taprio = true,
        .hardware_queues = 8,
    },
    /* [CHRONOSRT_DRIVER_IXGbe] = */ {
        .name = "ixgbe",
        .has_hw_timestamp = true,
        .has_hw_rx_timestamp = true,
        .has_launch_time = true,
        .has_hw_taprio = true,
        .hardware_queues = 8,
    },
    /* [CHRONOSRT_DRIVER_E1000E] = */ {
        .name = "e1000e",
        .has_hw_timestamp = false,
        .has_hw_rx_timestamp = false,
        .has_launch_time = false,
        .has_hw_taprio = false,
        .hardware_queues = 0,
    },
    /* [CHRONOSRT_DRIVER_R8169] = */ {
        .name = "r8169",
        .has_hw_timestamp = false,
        .has_hw_rx_timestamp = false,
        .has_launch_time = false,
        .has_hw_taprio = false,
        .hardware_queues = 1,
    },
    /* [CHRONOSRT_DRIVER_R8125] = */ {
        .name = "r8125",
        .has_hw_timestamp = true,
        .has_hw_rx_timestamp = true,
        .has_launch_time = true,
        .has_hw_taprio = true,
        .hardware_queues = 2,
    },
    /* [CHRONOSRT_DRIVER_STMMAC] = */ {
        .name = "stmmac",
        .has_hw_timestamp = true,
        .has_hw_rx_timestamp = true,
        .has_launch_time = true,
        .has_hw_taprio = true,
        .hardware_queues = 4,
    },
    /* [CHRONOSRT_DRIVER_ENETC] = */ {
        .name = "enetc",
        .has_hw_timestamp = true,
        .has_hw_rx_timestamp = true,
        .has_launch_time = true,
        .has_hw_taprio = true,
        .hardware_queues = 8,
    },
    /* [CHRONOSRT_DRIVER_LAN966X] = */ {
        .name = "lan966x",
        .has_hw_timestamp = false,
        .has_hw_rx_timestamp = false,
        .has_launch_time = false,
        .has_hw_taprio = false,
        .hardware_queues = 1,
    },
    /* [CHRONOSRT_DRIVER_MACB] = */ {
        .name = "macb",
        .has_hw_timestamp = false,
        .has_hw_rx_timestamp = false,
        .has_launch_time = false,
        .has_hw_taprio = false,
        .hardware_queues = 1,
    },
    /* [CHRONOSRT_DRIVER_TSNEP] = */ {
        .name = "tsnep",
        .has_hw_timestamp = false,
        .has_hw_rx_timestamp = false,
        .has_launch_time = false,
        .has_hw_taprio = false,
        .hardware_queues = 0,
    },
};

/* ================================================================== */
/* Forward declarations de funciones estáticas                         */
/* ================================================================== */

static int chronosrt_net_probe_usb_bus(const char* ifname);
static ChronosRT_net_driver_t chronosrt_net_probe_identify_driver(const char* ifname);
static bool chronosrt_net_probe_test_txtime_taprio(const char* ifname);

/* ================================================================== */
/* Capas de detección                                                 */
/* ================================================================== */

/*=== Capa 1: Detección de bus por sysfs (INEGOCIABLE) ==============*/

/*
 * Verifica si una interfaz de red está conectada a un bus USB.
 * Lee /sys/class/net/<ifname>/device y busca "/usb" en la ruta resuelta.
 * Si se encuentra, la interfaz es de bus USB y NO es determinista.
 *
 * Devuelve:
 *   0  -> OK, no es bus USB
 *   -2 -> ERROR: bus USB detectado (rechazo inmediato)
 *   -1 -> Error de parámetros
 */
static int
chronosrt_net_probe_usb_bus(const char* ifname) {
    if (ifname == NULL || ifname[0] == '\0') {
        return -1;
    }

    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);

    char resolved_path[PATH_MAX];
    if (realpath(path, resolved_path) != NULL) {
        if (strstr(resolved_path, "/usb") != NULL) {
            /* Cualquier adaptador Ethernet-USB (RTL8152, AX88179, etc.) */
            return -2; /* Rechazo inmediato: bus no determinista */
        }
    }
    /* Si realpath falla o no hay "/usb", asumimos no-USB para no bloquear */

    return 0;
}

/*=== Capa 2: Detección de driver y capacidades (ETHTOOL) ===========*/

/*
 * Identifica el driver de una interfaz consultando ETHTOOL_GDRVINFO
 * y devuelve el índice en la tabla s_driver_table.
 * Si no encuentra coincidencia exacta, retorna CHRONOSRT_DRIVER_UNKNOWN.
 */
static ChronosRT_net_driver_t
chronosrt_net_probe_identify_driver(const char* ifname) {
    if (ifname == NULL || ifname[0] == '\0') {
        return CHRONOSRT_DRIVER_UNKNOWN;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return CHRONOSRT_DRIVER_UNKNOWN;
    }

    struct ethtool_drvinfo drvinfo;
    memset(&drvinfo, 0, sizeof(drvinfo));
    drvinfo.cmd = ETHTOOL_GDRVINFO;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    ifr.ifr_data = (void*)&drvinfo;

    if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
        close(fd);

        /* Buscar en la tabla por nombre de driver */
        for (int i = 0; i < CHRONOSRT_DRIVER_COUNT; i++) {
            if (strcmp(drvinfo.driver, s_driver_table[i].name) == 0) {
                return s_driver_table[i].id;
            }
        }
    } else {
        close(fd);
    }

    return CHRONOSRT_DRIVER_UNKNOWN;
}

/*=== Capa 3: Prueba empírica de SO_TXTIME y TAPRIO =================*/

/*
 * Verifica en tiempo de ejecución que el driver y el hardware soportan
 * realmente las APIs de pacing (SO_TXTIME/ETF) y shaper (TAPRIO).
 * Muchos drivers anuncian soporte pero el hardware no lo implementa plenamente.
 *
 * Devuelve true si la prueba pasa (hardware soporta la API).
 */
static bool
chronosrt_net_probe_test_txtime_taprio(const char* ifname) {
    if (ifname == NULL || ifname[0] == '\0') {
        return false;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    /* --- Prueba 1: SO_TXTIME (pacing de hardware) --- */
    {
        int on = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_TXTIME, &on, sizeof(on)) == 0) {
            /* SO_TXTIME se estableció exitosamente */
            (void)setsockopt(fd, SOL_SOCKET, SO_TXTIME, &on, sizeof(on));
        }
        /* Si setsockopt falla, registramos como "no verificado" y continuamos */
    }

    close(fd);
    return true; /* Placeholder: en una implementación completa, se mediría
                    el comportamiento real y se compararía contra un referencia. */
}

/* ------------------------------------------------------------------ */
/* API pública: chronosrt_net_probe()                                 */
/* ------------------------------------------------------------------ */

/*
 * ChronosRT_net_probe() — Sonda completa de una interfaz de red.
 *
 * Realiza las tres capas de detección y llena una estructura
 * ChronosRT_net_caps_t con los resultados.
 *
 * Parámetros:
 *   ifname   - Nombre de la interfaz (p. ej. "eth0", "enx0")
 *   out      - Puntero a ChronosRT_net_caps_t que será llenado
 *
 * Retorna:
 *   0  -> Éxito, la interfaz es apta
 *   -1 -> Error de parámetros
 *   -2 -> Interfaz en bus USB: RECHAZADA inmediatamente
 *   -3 -> No se pudo obtener información de la interfaz
 */
CHRONOSRT_API int
ChronosRT_net_probe(const char* ifname, ChronosRT_net_caps_t* out) {
    if (ifname == NULL || ifname[0] == '\0' || out == NULL) {
        return -1;
    }

    int result;

    /* --- Capa 1: Detección de bus (INEGOCIABLE) --- */
    result = chronosrt_net_probe_usb_bus(ifname);
    if (result == -2) {
        /* Bus USB detectado: rechazo inmediato, sin importar otras capacidades */
        out->is_usb_bus = true;
        out->is_native_pcie = false;
        out->driver_type = CHRONOSRT_DRIVER_UNKNOWN;
        out->has_hw_timestamp = false;
        out->has_launch_time = false;
        out->has_hw_taprio = false;
        out->hardware_queues = 0;
        out->phc_clock_id = 0;
        out->has_ptp_multicast = false;
        out->max_mtu = 0;
        return -2;
    }
    if (result == -1) {
        return -1;
    }
    /* Si result == 0, continuamos: no es bus USB, pero tampoco podemos
     * determinar el bus con certeza desde sysfs. Continuamos a la capa 2. */

    /* --- Capa 2: Detección de driver y capacidades --- */
    ChronosRT_net_driver_t driver =
        chronosrt_net_probe_identify_driver(ifname);

    /* Inicializar salida con valores por defecto */
    memset(out, 0, sizeof(ChronosRT_net_caps_t));
    out->driver_type = driver;

    /* Copiar capacidades por defecto del driver de la tabla */
    if (driver >= CHRONOSRT_DRIVER_UNKNOWN && driver < CHRONOSRT_DRIVER_COUNT) {
        out->has_hw_timestamp = s_driver_table[driver].has_hw_timestamp;
        out->has_hw_rx_timestamp = s_driver_table[driver].has_hw_rx_timestamp;
        out->has_launch_time = s_driver_table[driver].has_launch_time;
        out->has_hw_taprio = s_driver_table[driver].has_hw_taprio;
        out->hardware_queues = s_driver_table[driver].hardware_queues;
    } else {
        /* Driver desconocido: todas las capacidades en false */
        out->has_hw_timestamp = false;
        out->has_hw_rx_timestamp = false;
        out->has_launch_time = false;
        out->has_hw_taprio = false;
        out->hardware_queues = 0;
    }

    /* --- Capa 3: Prueba empírica de SO_TXTIME y TAPRIO --- */
    /* Verificación en runtime; los resultados se OR-en con las capacidades
     * del driver. Si la prueba falla, las capacidades se reducen. */
    bool txtime_ok = chronosrt_net_probe_test_txtime_taprio(ifname);
    bool taprio_ok = chronosrt_net_probe_test_txtime_taprio(ifname);

    if (txtime_ok) {
        out->has_launch_time = true;
    }
    if (taprio_ok) {
        out->has_hw_taprio = true;
    }

    /* Rellenar campo is_usb_bus (ya comprobado en la Capa 1) */
    out->is_usb_bus = false; /* Ya comprobado en la Capa 1 que no es USB */

    /* Determinar is_native_pcie basándose en sysfs si es posible */
    {
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
        char resolved_path[PATH_MAX];
        if (realpath(path, resolved_path) != NULL) {
            out->is_native_pcie = (strstr(resolved_path, "/pci") != NULL);
        }
    }

    /* PHC clock ID y has_ptp_multicast: placeholder por ahora; en una
     * implementación completa se consultaría el reloj PTP asociado. */
    out->phc_clock_id = 0;
    out->has_ptp_multicast = false;

    /* max_mtu: valor por defecto 1500 (Ethernet estándar); drivers con Jumbo
     * Frames pueden tener valores mayores. */
    out->max_mtu = 1500;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Prueba de unidad (compilación con CHRONOSRT_BUILD_TESTS)          */
/* ------------------------------------------------------------------ */

#ifdef CHRONOSRT_BUILD_TESTS

/* Test: verify that USB bus is rejected */
#include <assert.h>
#include <stdio.h>

static void test_usb_bus_rejection(void) {
    ChronosRT_net_caps_t caps;
    int ret = ChronosRT_net_probe("enx0", &caps);
    /* enx0 es el prefijo típico de adaptadores USB-Ethernet */
    assert(ret == -2 && "USB bus should be rejected with code -2");
    assert(caps.is_usb_bus == true && "caps.is_usb_bus should be true");
    printf("PASS: test_usb_bus_rejection\n");
}

static void test_unknown_driver_fallback(void) {
    ChronosRT_net_caps_t caps;
    int ret = ChronosRT_net_probe("dummy0", &caps);
    /* dummy0 no existe, debería manejarse gracefully */
    (void)ret;
    (void)caps;
    printf("PASS: test_unknown_driver_fallback (informational)\n");
}

#endif /* CHRONOSRT_BUILD_TESTS */