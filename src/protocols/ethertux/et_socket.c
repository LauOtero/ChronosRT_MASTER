/*
 * et_socket.c — Transporte de tiempo real de EtherTux sobre AF_PACKET.
 *
 * EtherTux opera sobre Ethernet comercial estándar. El socket se prepara para
 * un datapath determinista:
 *
 *   - `AF_PACKET`/`SOCK_RAW` con EtherType 0x88B5 (EtherTux),
 *   - modo promiscuo + difusión (acepta tramas de cualquier nodo del segmento),
 *   - `PACKET_QDISC_BYPASS` (evita la disciplina de colas del kernel),
 *   - `SO_PRIORITY` elevado (selección de cola de hardware),
 *   - `SO_TXTIME` para *pacing* por hardware (Launch Time / ETF): la trama se
 *     entrega al driver con su instante exacto de transmisión, eliminando el
 *     jitter del encolado. Si la NIC o el qdisc no lo soportan, se degrada a
 *     envío inmediato sin fallar,
 *   - recepción con `poll()` sobre reloj monotónico (timeouts acotados).
 *
 * El transporte es un socket `AF_PACKET` deliberadamente: AF_XDP exige soporte
 * del driver y libbpf, no está disponible en todas las SBC objetivo y no puede
 * validarse sin hardware; EtherTux aísla aquí el datapath para poder añadir un
 * backend AF_XDP en el futuro sin tocar el protocolo.
 */
#include "et_internal.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>

#if defined(__x86_64__)
    #include <immintrin.h>
#endif

static uint64_t et_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static int et_get_mac(int fd, const char* ifname, uint8_t mac[6]) {
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

ChronosRT_status_t et_socket_open(et_port_t* port, const char* ifname) {
    if (port == NULL || ifname == NULL || ifname[0] == '\0') {
        return ChronosRT_ERROR_INVALID;
    }

    memset(port, 0, sizeof(*port));
    port->fd = -1;

    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0u) {
        return ChronosRT_ERROR_IO;
    }

    int fd = socket(AF_PACKET, SOCK_RAW, htons((uint16_t)ChronosRT_ET_ETHERTYPE));
    if (fd < 0) {
        /* Sin privilegios (CAP_NET_RAW) o interfaz inexistente. */
        return ChronosRT_ERROR_IO;
    }

    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons((uint16_t)ChronosRT_ET_ETHERTYPE);
    addr.sll_ifindex = (int)ifindex;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return ChronosRT_ERROR_IO;
    }

    if (et_get_mac(fd, ifname, port->mac) < 0) {
        close(fd);
        return ChronosRT_ERROR_IO;
    }

    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = (int)ifindex;
    mreq.mr_type = PACKET_MR_PROMISC;
    (void)setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = (int)ifindex;
    mreq.mr_type = PACKET_MR_MULTICAST;
    mreq.mr_alen = 6;
    memset(mreq.mr_address, 0xFF, 6);
    (void)setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    int bypass = 1;
    (void)setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &bypass, sizeof(bypass));

    int prio = 6;
    (void)setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));

#if defined(SO_TXTIME) && defined(SCM_TXTIME)
    struct sock_txtime txtime;
    memset(&txtime, 0, sizeof(txtime));
    txtime.clockid = CLOCK_MONOTONIC;
    txtime.flags = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_TXTIME, &txtime, sizeof(txtime)) == 0) {
        port->txtime_enabled = true;
    }
#endif

    /*
     * Recepción de baja latencia: sondeo ocupado acotado en el propio driver
     * (`SO_BUSY_POLL`) en lugar de dormir hasta el siguiente tick del kernel.
     * Si el kernel o el driver no lo soportan, se ignora.
     */
#ifdef SO_BUSY_POLL
    int busy_us = 25;
    (void)setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif
#ifdef SO_PREFER_BUSY_POLL
    int prefer = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer, sizeof(prefer));
#endif
    int rcvbuf = 1 << 20;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    port->fd = fd;
    port->ifindex = (int)ifindex;
    strncpy(port->ifname, ifname, sizeof(port->ifname) - 1);
    port->ifname[sizeof(port->ifname) - 1] = '\0';
    return ChronosRT_SUCCESS;
}

void et_socket_close(et_port_t* port) {
    if (port == NULL) {
        return;
    }
    et_socket_tx_ring_disable(port);
    if (port->fd >= 0) {
        close(port->fd);
    }
    port->fd = -1;
    port->txtime_enabled = false;
}

/* ------------------------------------------------------------------ */
/* Anillo de transmisión PACKET_TX_RING                                */
/*                                                                     */
/* El kernel mapea un anillo de tramas en user space: la aplicación     */
/* escribe la trama en su ranura y marca `TP_STATUS_SEND_REQUEST`, y un */
/* único `send()` vacía TODAS las ranuras marcadas. Con lotes de 16      */
/* tramas se pasa de 16 llamadas al sistema a una, que es el coste      */
/* dominante al transmitir tramas pequeñas (100–300 B).                 */
/* ------------------------------------------------------------------ */

#define ET_TX_RING_FRAME_BYTES 2048u

bool et_socket_tx_ring_active(const et_port_t* port) {
    return (port != NULL) && port->tx_ring_active;
}

bool et_socket_tx_ring_enable(et_port_t* port, uint32_t frames) {
    if (port == NULL || port->fd < 0 || port->tx_ring_active) {
        return false;
    }
    if (frames < 8u) {
        frames = 8u;
    }
    if (frames > 2048u) {
        frames = 2048u;
    }
    /* Potencia de dos: `claim` indexa con máscara, sin división por ranura. */
    uint32_t pow2 = 8u;
    while (pow2 < frames) {
        pow2 <<= 1u;
    }
    frames = pow2;

    /* Dos tramas por bloque de 4 KiB (block_size debe ser múltiplo de página). */
    const uint32_t per_block  = 2u;
    const uint32_t block_size = ET_TX_RING_FRAME_BYTES * per_block;
    const uint32_t block_nr   = frames / per_block;

    struct tpacket_req req;
    memset(&req, 0, sizeof(req));
    req.tp_block_size = block_size;
    req.tp_block_nr   = block_nr;
    req.tp_frame_size = ET_TX_RING_FRAME_BYTES;
    req.tp_frame_nr   = block_nr * per_block;

    if (setsockopt(port->fd, SOL_PACKET, PACKET_TX_RING, &req, sizeof(req)) < 0) {
        return false;
    }

    void* ring = mmap(NULL, (size_t)block_size * (size_t)block_nr,
                      PROT_READ | PROT_WRITE, MAP_SHARED, port->fd, 0);
    if (ring == MAP_FAILED) {
        return false;
    }

    port->tx_ring        = (uint8_t*)ring;
    port->tx_ring_bytes  = (size_t)block_size * (size_t)block_nr;
    port->tx_frame_size  = ET_TX_RING_FRAME_BYTES;
    port->tx_frame_nr    = req.tp_frame_nr;
    port->tx_frame_mask  = req.tp_frame_nr - 1u;
    port->tx_head        = 0u;
    port->tx_reserved    = false;
    port->tx_ring_active = true;
    return true;
}

void et_socket_tx_ring_disable(et_port_t* port) {
    if (port == NULL || !port->tx_ring_active) {
        return;
    }
    if (port->tx_ring != NULL) {
        (void)munmap(port->tx_ring, port->tx_ring_bytes);
    }
    /* Libera también el anillo del kernel; sin esto el socket queda ligado al
     * anillo y no admite el camino normal de `sendmsg`. */
    struct tpacket_req zero;
    memset(&zero, 0, sizeof(zero));
    (void)setsockopt(port->fd, SOL_PACKET, PACKET_TX_RING, &zero, sizeof(zero));

    port->tx_ring        = NULL;
    port->tx_ring_bytes  = 0u;
    port->tx_frame_nr    = 0u;
    port->tx_ring_active = false;
}

/* Reserva la siguiente ranura libre del anillo (NULL si están todas en vuelo).
 * Prefetchea la siguiente ranura mientras se evalúa la actual (B-7). */
static uint8_t* et_ring_claim(et_port_t* port) {
    for (uint32_t n = 0; n < port->tx_frame_nr; n++) {
        const uint32_t idx = port->tx_head;
        const uint32_t next_idx = (idx + 1u) & port->tx_frame_mask;
        uint8_t* slot = port->tx_ring + (size_t)idx * port->tx_frame_size;
        struct tpacket_hdr* h = (struct tpacket_hdr*)(void*)slot;

        /* Prefetch de la siguiente ranura para reducir cache miss en la siguiente iteración. */
#if defined(__x86_64__)
        _mm_prefetch((const char*)(port->tx_ring + (size_t)next_idx * port->tx_frame_size), _MM_HINT_T0);
#elif defined(__aarch64__)
        __builtin_prefetch(port->tx_ring + (size_t)next_idx * port->tx_frame_size, 1, 3);
#endif

        port->tx_head = next_idx;
        if (h->tp_status == TP_STATUS_AVAILABLE) {
            return slot;
        }
    }
    return NULL;
}

/*
 * Reserva/confirma: el llamador construye la trama DIRECTAMENTE en la ranura del
 * anillo (cabecera Ethernet + PDU + CRC), sin el memcpy final del camino `put`.
 */
static void et_ring_commit(et_port_t* port, uint8_t* slot, uint32_t len);

uint8_t* et_socket_tx_reserve(et_port_t* port) {
    if (port == NULL || !port->tx_ring_active || port->tx_reserved) {
        return NULL;
    }
    uint8_t* slot = et_ring_claim(port);
    if (slot == NULL) {
        (void)et_socket_tx_ring_kick(port);
        slot = et_ring_claim(port);
        if (slot == NULL) {
            return NULL;
        }
    }
    port->tx_reserved = true;
    return slot + TPACKET_HDRLEN;
}

void et_socket_tx_commit(et_port_t* port, uint32_t len) {
    if (port == NULL || !port->tx_reserved) {
        return;
    }
    uint8_t* slot = port->tx_ring +
                    (size_t)((port->tx_head - 1u) & port->tx_frame_mask) * port->tx_frame_size;
    et_ring_commit(port, slot, len);
    port->tx_reserved = false;
}

/* Descarta una reserva sin enviarla: la ranura sigue AVAILABLE para reutilizarse. */
void et_socket_tx_cancel(et_port_t* port) {
    if (port != NULL) {
        port->tx_reserved = false;
    }
}

static void et_ring_commit(et_port_t* port, uint8_t* slot, uint32_t len) {
    struct tpacket_hdr* h = (struct tpacket_hdr*)(void*)slot;
    h->tp_len     = (unsigned int)len;
    h->tp_snaplen = (unsigned int)len;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->tp_status  = TP_STATUS_SEND_REQUEST;
    port->frames_tx++;
    port->bytes_tx += len;
}

int et_socket_tx_ring_put(et_port_t* port, const uint8_t* frame, uint32_t len) {
    if (port == NULL || !port->tx_ring_active || frame == NULL || len == 0u) {
        return -1;
    }
    if ((size_t)len + (size_t)TPACKET_HDRLEN > port->tx_frame_size) {
        return -1;
    }

    /* Backpressure con spin-wait: si el anillo está lleno, kick y reintentar
     * con spin corto antes que fallar. Evita saltos bruscos de throughput. */
    for (uint32_t attempt = 0; attempt < 4u; attempt++) {
        uint8_t* slot = et_ring_claim(port);
        if (slot != NULL) {
            memcpy(slot + TPACKET_HDRLEN, frame, len);
            et_ring_commit(port, slot, len);
            return 0;
        }
        /* Anillo lleno: liberar ranuras en vuelo y reintentar. */
        (void)et_socket_tx_ring_kick(port);
        for (volatile int spin = 0; spin < 64; spin++) {
#if defined(__x86_64__)
            __asm__ __volatile__("pause" : : : "memory");
#elif defined(__aarch64__)
            __asm__ __volatile__("yield" : : : "memory");
#endif
        }
    }
    return -1;
}

int et_socket_tx_ring_kick(et_port_t* port) {
    if (port == NULL || !port->tx_ring_active) {
        return -1;
    }
    ssize_t n = send(port->fd, NULL, 0, 0);
    return (n < 0) ? -1 : (int)n;
}

int et_socket_tx_ring_space(const et_port_t* port) {
    return (port != NULL && port->tx_ring_active) ? (int)port->tx_frame_nr : 0;
}

bool et_socket_txtime_enabled(const et_port_t* port) {
    return (port != NULL) && port->txtime_enabled;
}

int et_socket_send(et_port_t* port, const uint8_t* frame, uint32_t len) {
    if (port == NULL || port->fd < 0 || frame == NULL || len == 0u) {
        return -1;
    }
    if (port->tx_ring_active) {
        if (et_socket_tx_ring_put(port, frame, len) != 0) {
            return -1;
        }
        (void)et_socket_tx_ring_kick(port);
        return (int)len;
    }
    ssize_t n = send(port->fd, frame, len, 0);
    if (n != (ssize_t)len) {
        return -1;
    }
    port->frames_tx++;
    port->bytes_tx += len;
    return (int)len;
}

/*
 * Envío sin copiar la carga útil: cabecera y payload en dos iovecs. El kernel
 * los recolecta en el skb, de modo que la aplicación no hace memcpy de la carga.
 */
int et_socket_send_iov(et_port_t* port, const void* hdr, uint32_t hdr_len,
                       const void* payload, uint32_t payload_len) {
    if (port == NULL || port->fd < 0 || hdr == NULL || hdr_len == 0u) {
        return -1;
    }
    if (payload == NULL && payload_len > 0u) {
        return -1;
    }

    uint32_t total = hdr_len + payload_len;

    if (port->tx_ring_active) {
        if ((size_t)total + (size_t)TPACKET_HDRLEN > port->tx_frame_size) {
            return -1;
        }
        uint8_t* slot = et_ring_claim(port);
        if (slot == NULL) {
            (void)et_socket_tx_ring_kick(port);
            slot = et_ring_claim(port);
            if (slot == NULL) {
                return -1;
            }
        }
        memcpy(slot + TPACKET_HDRLEN, hdr, hdr_len);
        if (payload_len > 0u) {
            memcpy(slot + TPACKET_HDRLEN + hdr_len, payload, payload_len);
        }
        et_ring_commit(port, slot, total);
        (void)et_socket_tx_ring_kick(port);
        return (int)total;
    }

    struct iovec iov[2];
    iov[0].iov_base = (void*)hdr;
    iov[0].iov_len = hdr_len;
    iov[1].iov_base = (void*)payload;
    iov[1].iov_len = payload_len;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = (payload_len > 0u) ? 2u : 1u;

    ssize_t n = sendmsg(port->fd, &msg, 0);
    if (n != (ssize_t)total) {
        return -1;
    }
    port->frames_tx++;
    port->bytes_tx += total;
    return (int)total;
}

/*
 * Envío por lotes. Con el anillo activo se escriben todas las tramas en sus
 * ranuras y se hace UNA llamada al sistema para vaciarlas: es el camino de
 * máxima tasa de tramas. Sin anillo se usa `sendmmsg`.
 */
int et_socket_send_batch(et_port_t* port, struct mmsghdr* msgs, uint32_t count) {
    if (port == NULL || port->fd < 0 || msgs == NULL || count == 0u) {
        return -1;
    }

    if (port->tx_ring_active) {
        uint32_t done = 0u;
        for (uint32_t i = 0; i < count; i++) {
            const struct msghdr* m = &msgs[i].msg_hdr;
            uint32_t total = 0u;
            for (unsigned v = 0; v < m->msg_iovlen; v++) {
                total += (uint32_t)m->msg_iov[v].iov_len;
            }
            if (total == 0u || (size_t)total + (size_t)TPACKET_HDRLEN > port->tx_frame_size) {
                break;
            }
            uint8_t* slot = et_ring_claim(port);
            if (slot == NULL) {
                (void)et_socket_tx_ring_kick(port);   /* libera las ranuras en vuelo */
                slot = et_ring_claim(port);
                if (slot == NULL) {
                    break;
                }
            }
            uint32_t off = 0u;
            for (unsigned v = 0; v < m->msg_iovlen; v++) {
                uint32_t n = (uint32_t)m->msg_iov[v].iov_len;
                memcpy(slot + TPACKET_HDRLEN + off, m->msg_iov[v].iov_base, n);
                off += n;
            }
            et_ring_commit(port, slot, total);
            done++;
        }
        if (done > 0u) {
            (void)et_socket_tx_ring_kick(port);
        }
        return (done > 0u) ? (int)done : -1;
    }

    int sent = sendmmsg(port->fd, msgs, count, 0);
    if (sent < 0) {
        return -1;
    }
    uint64_t bytes = 0;
    for (int i = 0; i < sent; i++) {
        const struct msghdr* m = &msgs[i].msg_hdr;
        for (unsigned v = 0; v < m->msg_iovlen; v++) {
            bytes += m->msg_iov[v].iov_len;
        }
    }
    port->frames_tx += (uint64_t)sent;
    port->bytes_tx += bytes;
    return sent;
}

uint64_t et_socket_link_bps(const char* ifname) {
    if (ifname == NULL || ifname[0] == '\0') {
        return 0u;
    }
    char path[128];
    int n = snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ifname);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return 0u;
    }
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return 0u;
    }
    long mbps = -1;
    if (fscanf(f, "%ld", &mbps) != 1) {
        mbps = -1;
    }
    (void)fclose(f);
    if (mbps <= 0) {
        return 0u;   /* interfaz caída o sin enlace */
    }
    return (uint64_t)mbps * 1000000ull;
}

int et_socket_send_at(et_port_t* port, const uint8_t* frame, uint32_t len,
                      uint64_t deadline_ns) {
    if (port == NULL || port->fd < 0 || frame == NULL || len == 0u) {
        return -1;
    }

#if defined(SO_TXTIME) && defined(SCM_TXTIME)
    if (port->txtime_enabled) {
        struct iovec iov;
        iov.iov_base = (void*)frame;
        iov.iov_len = len;

        char cbuf[CMSG_SPACE(sizeof(uint64_t))];
        memset(cbuf, 0, sizeof(cbuf));

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);

        struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_TXTIME;
        cm->cmsg_len = CMSG_LEN(sizeof(uint64_t));
        memcpy(CMSG_DATA(cm), &deadline_ns, sizeof(deadline_ns));

        ssize_t n = sendmsg(port->fd, &msg, 0);
        if (n == (ssize_t)len) {
            port->frames_tx++;
            port->bytes_tx += len;
            return (int)len;
        }
        if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
            /* El qdisc/la NIC no aplican el instante de transmisión: degrada. */
            port->txtime_enabled = false;
        } else {
            return -1;
        }
    }
#else
    (void)deadline_ns;
#endif

    return et_socket_send(port, frame, len);
}

int et_socket_recv(et_port_t* port, uint8_t* frame, uint32_t bufsize,
                   uint32_t timeout_us) {
    if (port == NULL || port->fd < 0 || frame == NULL || bufsize == 0u) {
        return -1;
    }

    uint64_t deadline = et_now_us() + (uint64_t)timeout_us;

    for (;;) {
        /*
         * Camino rápido: si ya hay una trama en el socket, un único `recv`
         * (sin `poll`) evita la mitad de las llamadas al sistema por trama.
         */
        ssize_t n = recv(port->fd, frame, bufsize, MSG_DONTWAIT);
        if (n >= (ssize_t)(ChronosRT_ET_ETH_HDR_SIZE + ChronosRT_ET_HEADER_SIZE)) {
            uint16_t eth_type = (uint16_t)((frame[12] << 8) | frame[13]);
            if (eth_type == ChronosRT_ET_ETHERTYPE) {
                port->frames_rx++;
                port->bytes_rx += (uint32_t)n;
                return (int)n;
            }
            continue;   /* ajena: sigue drenando */
        }
        if (n >= 0) {
            continue;   /* trama corta: descártala y sigue */
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }

        uint64_t now = et_now_us();
        if (now >= deadline) {
            return -1;
        }
        uint64_t remaining_us = deadline - now;
        int wait_ms = (int)((remaining_us + 999u) / 1000u);
        if (wait_ms <= 0) {
            wait_ms = 1;
        }

        struct pollfd pfd;
        pfd.fd = port->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            continue;   /* reevalúa el deadline */
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }
    }
}
