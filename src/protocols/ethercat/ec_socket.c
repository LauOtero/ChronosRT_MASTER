/*
 * ec_socket.c — Capa de socket de tiempo real del maestro EtherCAT.
 *
 * Abre interfaces Ethernet crudas (AF_PACKET, EtherType 0x88A4) listas para un
 * ciclo determinista:
 *
 *   - modo promiscuo + suscripción a difusión (necesario para recibir la trama
 *     devuelta por las esclavas),
 *   - `PACKET_QDISC_BYPASS` para eludir la disciplina de colas del kernel,
 *   - prioridad de socket elevada (SO_PRIORITY),
 *   - recepción con `poll()` sobre reloj monotónico (timeouts acotados),
 *   - redundancia de red (NIC primaria + secundaria) transparente.
 *
 * No hay asignaciones dinámicas: el maestro aporta los búferes.
 */
#include "ec_internal.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <ifaddrs.h>

/* ------------------------------------------------------------------ */
/* Reloj monotónico                                                    */
/* ------------------------------------------------------------------ */

uint64_t ec_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

int64_t ec_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Utilidades de interfaz                                             */
/* ------------------------------------------------------------------ */

static int ec_get_mac(int fd, const char* ifname, uint8_t mac[6]) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    if (strlen(ifname) >= IFNAMSIZ) {
        return -1;
    }
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        return -1;
    }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

static bool ec_interface_is_up(const char* ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    bool up = false;
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        up = (ifr.ifr_flags & IFF_UP) != 0;
    }
    close(fd);
    return up;
}

/* ------------------------------------------------------------------ */
/* Apertura de un socket EtherCAT sobre una interfaz                   */
/* ------------------------------------------------------------------ */

static int ec_open_one(const char* ifname, int* ifindex_out, uint8_t mac[6]) {
    if (!ifname || ifname[0] == '\0') {
        return -1;
    }

    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0u) {
        return -1;
    }

    int fd = socket(AF_PACKET, SOCK_RAW, htons((uint16_t)ChronosRT_EC_ETHERTYPE));
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons((uint16_t)ChronosRT_EC_ETHERTYPE);
    addr.sll_ifindex = (int)ifindex;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (ec_get_mac(fd, ifname, mac) < 0) {
        close(fd);
        return -1;
    }

    /* Modo promiscuo: imprescindible para capturar la trama de retorno. */
    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = (int)ifindex;
    mreq.mr_type = PACKET_MR_PROMISC;
    (void)setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    /* Suscripción a difusión (las tramas EtherCAT van a FF:FF:…). */
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = (int)ifindex;
    mreq.mr_type = PACKET_MR_MULTICAST;
    mreq.mr_alen = 6;
    memset(mreq.mr_address, 0xFF, 6);
    (void)setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    /* Eludir la disciplina de colas: menos latencia y jitter. */
    int bypass = 1;
    (void)setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &bypass, sizeof(bypass));

    /* Prioridad de socket alta (afecta a la selección de cola). */
    int prio = 6;
    (void)setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));

    *ifindex_out = (int)ifindex;
    return fd;
}

ChronosRT_status_t ec_socket_open(ec_port_t* port, const char* ifname) {
    if (!port || !ifname || ifname[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }

    memset(port, 0, sizeof(*port));
    port->fd = -1;
    port->fd_red = -1;

    int ifindex = 0;
    int fd = ec_open_one(ifname, &ifindex, port->mac);
    if (fd < 0) {
        return ChronosRT_ERROR_IO;
    }

    port->fd = fd;
    port->ifindex = ifindex;
    strncpy(port->ifname, ifname, sizeof(port->ifname) - 1);
    return ChronosRT_SUCCESS;
}

ChronosRT_status_t ec_socket_open_redundant(ec_port_t* port, const char* ifname,
                                            const char* redundant_ifname) {
    if (!port || !ifname || !redundant_ifname ||
        ifname[0] == '\0' || redundant_ifname[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }

    ChronosRT_status_t st = ec_socket_open(port, ifname);
    if (st != ChronosRT_SUCCESS) {
        return st;
    }

    int ifindex = 0;
    int fd = ec_open_one(redundant_ifname, &ifindex, port->mac_red);
    if (fd < 0) {
        ec_socket_close(port);
        return ChronosRT_ERROR_IO;
    }

    port->fd_red = fd;
    port->ifindex_red = ifindex;
    port->redundant = true;
    strncpy(port->red_ifname, redundant_ifname, sizeof(port->red_ifname) - 1);
    return ChronosRT_SUCCESS;
}

void ec_socket_close(ec_port_t* port) {
    if (!port) {
        return;
    }
    if (port->fd >= 0) {
        close(port->fd);
    }
    if (port->fd_red >= 0) {
        close(port->fd_red);
    }
    port->fd = -1;
    port->fd_red = -1;
    port->redundant = false;
}

/* ------------------------------------------------------------------ */
/* Envío / recepción                                                   */
/* ------------------------------------------------------------------ */

int ec_socket_send(ec_port_t* port, const uint8_t* frame, uint32_t len) {
    if (!port || port->fd < 0 || !frame || len == 0u) {
        return -1;
    }

    ssize_t n = send(port->fd, frame, len, 0);
    if (n != (ssize_t)len) {
        port->frames_lost++;
        return -1;
    }
    port->frames_tx++;

    /* Redundancia: la MISMA trama por la NIC secundaria. */
    if (port->redundant && port->fd_red >= 0) {
        (void)send(port->fd_red, frame, len, 0);
    }
    return (int)len;
}

int ec_socket_recv(ec_port_t* port, uint8_t* frame, uint32_t bufsize, uint32_t timeout_us) {
    if (!port || port->fd < 0 || !frame || bufsize == 0u) {
        return -1;
    }

    uint64_t deadline = ec_now_us() + (uint64_t)timeout_us;

    for (;;) {
        struct pollfd pfd[2];
        nfds_t nfds = 0u;
        pfd[nfds].fd = port->fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
        if (port->redundant && port->fd_red >= 0) {
            pfd[nfds].fd = port->fd_red;
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }

        uint64_t now = ec_now_us();
        if (now >= deadline) {
            port->frames_lost++;
            return -1;
        }
        int wait_ms = (int)((deadline - now + 999u) / 1000u);
        if (wait_ms <= 0) {
            wait_ms = 1;
        }

        int pr = poll(pfd, nfds, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            continue;   /* expirado el quantum: reevalúa el deadline */
        }

        for (nfds_t i = 0; i < nfds; i++) {
            if ((pfd[i].revents & POLLIN) == 0) {
                continue;
            }
            ssize_t n = recv(pfd[i].fd, frame, bufsize, 0);
            if (n < (ssize_t)(EC_ETH_HDR_LEN + EC_ECAT_HDR_LEN)) {
                continue;   /* trama corta o ajena: descártala */
            }
            uint16_t eth_type = (uint16_t)((frame[12] << 8) | frame[13]);
            if (eth_type != ChronosRT_EC_ETHERTYPE) {
                continue;   /* no es EtherCAT */
            }
            port->frames_rx++;
            return (int)n;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Enumeración de interfaces (API pública)                             */
/* ------------------------------------------------------------------ */

static ChronosRT_ec_interface_t s_ifaces[ChronosRT_EC_MAX_INTERFACES];
static uint32_t s_iface_count = 0u;
static bool     s_iface_enumerated = false;

static void ec_enumerate_interfaces(void) {
    if (s_iface_enumerated) {
        return;
    }
    s_iface_enumerated = true;
    s_iface_count = 0u;

    struct ifaddrs* list = NULL;
    if (getifaddrs(&list) != 0 || !list) {
        return;
    }

    for (struct ifaddrs* it = list; it && s_iface_count < ChronosRT_EC_MAX_INTERFACES; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_PACKET) {
            continue;
        }
        const char* name = it->ifa_name;

        bool seen = false;
        for (uint32_t i = 0; i < s_iface_count; i++) {
            if (strncmp(s_ifaces[i].name, name, sizeof(s_ifaces[i].name)) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        ChronosRT_ec_interface_t* e = &s_ifaces[s_iface_count];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->ifindex = (int)if_nametoindex(name);
        e->is_up = ec_interface_is_up(name);

        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            (void)ec_get_mac(fd, name, e->mac);
            close(fd);
        }
        s_iface_count++;
    }

    freeifaddrs(list);
}

uint32_t ChronosRT_ec_interface_count(void) {
    ec_enumerate_interfaces();
    return s_iface_count;
}

const ChronosRT_ec_interface_t* ChronosRT_ec_interface(uint32_t index) {
    ec_enumerate_interfaces();
    if (index >= s_iface_count) {
        return NULL;
    }
    return &s_ifaces[index];
}

int ChronosRT_ec_interface_index(const char* name) {
    if (!name) {
        return -1;
    }
    ec_enumerate_interfaces();
    for (uint32_t i = 0; i < s_iface_count; i++) {
        if (strcmp(s_ifaces[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}
