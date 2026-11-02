/**
 * transport_ethertux.c — EtherTux Transport Adapter (AF_PACKET/SOCK_RAW)
 *
 * Implements tuxrt_transport_vtable_t for deterministic commercial Ethernet
 * communication (EtherType 0x88B5) using Linux raw packet sockets.
 *
 * This transport provides ultra-low latency, zero-copy transmission with:
 * - PACKET_TX_RING for minimal syscalls
 * - SO_TXTIME for hardware pacing (deterministic scheduling)
 * - FEC Reed-Solomon for lost packet recovery
 * - QoS hierarchical token buckets for rate shaping
 * - FRM for flow redundancy
 * - Local ID admission (Level 2 deterministic MAC derivation)
 *
 * Bridges ChronosRT's internal EtherTux master (ChronosRT_et_master_t) with
 * the TuxRT transport abstraction layer (tuxrt_transport_adapter_t).
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

#include "chronosrt_ethertux.h"
#include "chronosrt_transport.h"

/* EtherTux EtherType */
#define TUXRT_ET_ETHERTYPE 0x88B5

/* Maximum frames in batch (optimization for sendmmsg) */
#define ET_BATCH_MAX 16u

/* Transport implementation state */
typedef struct {
    ChronosRT_et_master_t* master;
    char ifname[IF_NAMESIZE];
    pthread_mutex_t mutex;
    bool configured;
} ethertux_transport_impl_t;

/* Initialize QoS and optional features from transport config.
 * Returns true on success, false on unsupported config. */
static bool ethertux_init_features(ethertux_transport_impl_t* impl,
                                  const tuxrt_transport_adapter_config_t* cfg) {
    if (!impl || !cfg) {
        return false;
    }

    /* Configure QoS from bitrate if available */
    if (cfg->bitrate > 0) {
        ChronosRT_et_qos_config_t qos_cfg;
        ChronosRT_et_qos_default_config(&qos_cfg);
        qos_cfg.bucket[TUXRT_ET_P0].rate_bytes_per_sec = cfg->bitrate / 10;      /* 10% for P0 */
        qos_cfg.bucket[TUXRT_ET_P1].rate_bytes_per_sec = cfg->bitrate / 10;      /* 10% for P1 */
        qos_cfg.bucket[TUXRT_ET_P2].rate_bytes_per_sec = cfg->bitrate / 2;       /* 50% for P2 */
        qos_cfg.bucket[TUXRT_ET_P3].rate_bytes_per_sec = cfg->bitrate / 10;      /* 10% for P3 */
        qos_cfg.bucket[TUXRT_ET_P4].rate_bytes_per_sec = cfg->bitrate / 10;      /* 10% for P4 */
        qos_cfg.bucket[TUXRT_ET_P5].rate_bytes_per_sec = cfg->bitrate / 10;      /* 10% for P5 */

        /* Enable LAPM (dynamic priority migration under load) */
        qos_cfg.lapm_enabled = true;
        qos_cfg.lapm_high_percent = 80;
        qos_cfg.lapm_low_percent = 60;

        (void)ChronosRT_et_master_set_qos(impl->master, &qos_cfg);
    }

    /* Configure FEC (Reed-Solomon) if needed */
    if (cfg->flow_control) {
        /* Enable robust FEC for lossy networks */
        (void)ChronosRT_et_master_set_fec(impl->master, ChronosRT_ET_FEC_LIGHT);
    }

    return true;
}

static tuxrt_err_t ethertux_open(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) {
        return TUXRT_ERR_INVAL;
    }

    ethertux_transport_impl_t* impl = calloc(1, sizeof(ethertux_transport_impl_t));
    if (!impl) {
        return TUXRT_ERR_NOMEM;
    }

    pthread_mutex_init(&impl->mutex, NULL);

    /* Get interface name from config */
    if (adapter->config.device_path) {
        strncpy(impl->ifname, adapter->config.device_path, IF_NAMESIZE - 1);
    } else {
        /* Default to eth0 if not specified */
        strncpy(impl->ifname, "eth0", IF_NAMESIZE - 1);
    }

    /* Create EtherTux master */
    impl->master = ChronosRT_et_master_create();
    if (!impl->master) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_NOMEM;
    }

    /* Open interface with auto-detected link speed */
    ChronosRT_status_t st = ChronosRT_et_master_open_auto(impl->master, impl->ifname);
    if (st != ChronosRT_SUCCESS) {
        ChronosRT_et_master_destroy(impl->master);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }

    /* Enable TX ring for zero-syscall transmission (if supported) */
    ChronosRT_et_tx_mode_t tx_mode = ChronosRT_ET_TX_AUTO;
    if (getenv("TUXRT_ET_TX_RING") != NULL) {
        tx_mode = ChronosRT_ET_TX_RING;
    }
    (void)ChronosRT_et_master_set_tx_mode(impl->master, tx_mode);

    /* Initialize QoS and optional features */
    (void)ethertux_init_features(impl, &adapter->config);

    /* Enable local ID admission (Level 2 deterministic MAC) */
    ChronosRT_et_aalid_config_t aalid_cfg;
    ChronosRT_et_aalid_default_config(&aalid_cfg);
    aalid_cfg.id_max = 0xFFFE;  /* Reserve 0xFFFF for broadcast */
    aalid_cfg.grace_ms = 600000; /* 600 seconds heartbeat timeout */
    aalid_cfg.table_slots = 1024;
    (void)ChronosRT_et_master_enable_local_ids(impl->master, &aalid_cfg);

    /* Set a default local ID for the master (can be changed later) */
    uint16_t local_id = 0x0100;  /* Default master ID */
    (void)ChronosRT_et_master_set_local_id(impl->master, local_id);

    pthread_mutex_lock(&adapter->mutex);
    impl->configured = true;
    adapter->impl = impl;

    /* Store link speed from config or use default */
    if (adapter->config.bitrate == 0) {
        adapter->config.bitrate = 1000000000u;  /* 1 Gbps default */
    }

    pthread_mutex_unlock(&adapter->mutex);

    tuxrt_sirc_update_health_success(adapter);

    return TUXRT_OK;
}

static tuxrt_err_t ethertux_close(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) {
        return TUXRT_ERR_INVAL;
    }

    ethertux_transport_impl_t* impl = (ethertux_transport_impl_t*)adapter->impl;

    pthread_mutex_lock(&impl->mutex);

    if (impl->configured) {
        ChronosRT_et_master_close(impl->master);
        impl->configured = false;
    }

    pthread_mutex_unlock(&impl->mutex);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    adapter->impl = NULL;
    adapter->active = false;

    return TUXRT_OK;
}

static tuxrt_err_t ethertux_send(tuxrt_transport_adapter_t* adapter,
                                  const uint8_t* data, size_t len,
                                  tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !data || len == 0) {
        return TUXRT_ERR_INVAL;
    }

    ethertux_transport_impl_t* impl = (ethertux_transport_impl_t*)adapter->impl;

    /* Cap payload to maximum allowed */
    uint16_t payload_len = (len > ChronosRT_ET_MAX_PAYLOAD) ? ChronosRT_ET_MAX_PAYLOAD : (uint16_t)len;

    /* Use hardware pacing if deadline is specified and txtime is available */
    if (deadline > 0) {
        ChronosRT_status_t st = ChronosRT_et_master_send_at(impl->master,
            ChronosRT_ET_P0, ChronosRT_ET_TYPE_DATA, 1u, data, payload_len, deadline);
        if (st == ChronosRT_SUCCESS) {
            tuxrt_sirc_update_health_success(adapter);
        } else {
            tuxrt_sirc_update_health_failure(adapter);
        }
        return (st == ChronosRT_SUCCESS) ? TUXRT_OK : TUXRT_ERR_IO;
    } else {
        ChronosRT_status_t st = ChronosRT_et_master_send(impl->master,
            ChronosRT_ET_P0, ChronosRT_ET_TYPE_DATA, 1u, data, payload_len);
        if (st == ChronosRT_SUCCESS) {
            tuxrt_sirc_update_health_success(adapter);
        } else {
            tuxrt_sirc_update_health_failure(adapter);
        }
        return (st == ChronosRT_SUCCESS) ? TUXRT_OK : TUXRT_ERR_IO;
    }
}

static tuxrt_err_t ethertux_recv(tuxrt_transport_adapter_t* adapter,
                                  uint8_t* buf, size_t* len,
                                  tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !buf || !len || *len == 0) {
        return TUXRT_ERR_INVAL;
    }

    ethertux_transport_impl_t* impl = (ethertux_transport_impl_t*)adapter->impl;

    /* Convert deadline to microseconds for recv */
    uint32_t timeout_us = 0;
    if (deadline > 0) {
        int64_t now = tuxrt_now_ns();
        if (deadline > now) {
            int64_t delta_us = (deadline - now) / 1000;
            timeout_us = (delta_us > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)delta_us;
        }
    } else {
        timeout_us = 1000000u;  /* 1 second default timeout */
    }

    /* Receive a frame */
    ChronosRT_et_header_t hdr;
    int ret = ChronosRT_et_master_recv(impl->master, &hdr, buf, (uint32_t)*len, timeout_us);

    if (ret < 0) {
        *len = 0;
        tuxrt_sirc_update_health_failure(adapter);
        return TUXRT_ERR_IO;
    }

    *len = (size_t)ret;
    tuxrt_sirc_update_health_success(adapter);

    return TUXRT_OK;
}

static tuxrt_err_t ethertux_flush(tuxrt_transport_adapter_t* adapter) {
    (void)adapter;
    /* EtherTux master handles buffering internally; flush is a no-op */
    return TUXRT_OK;
}

static tuxrt_err_t ethertux_get_stats(tuxrt_transport_adapter_t* adapter,
                                       void* stats_out) {
    if (!adapter || !stats_out) {
        return TUXRT_ERR_INVAL;
    }

    pthread_mutex_lock(&adapter->mutex);
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    pthread_mutex_unlock(&adapter->mutex);
    return TUXRT_OK;
}

static void ethertux_destroy(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) {
        return;
    }
    ethertux_close(adapter);
}

/* Multi-NIC: Activate adapter for multi-NIC operation */
static tuxrt_err_t ethertux_activate(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) {
        return TUXRT_ERR_INVAL;
    }

    ethertux_transport_impl_t* impl = (ethertux_transport_impl_t*)adapter->impl;

    pthread_mutex_lock(&impl->mutex);

    if (impl->configured && impl->master) {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_BUSY;  /* Already active */
    }

    /* Create master if needed */
    if (!impl->master) {
        impl->master = ChronosRT_et_master_create();
        if (!impl->master) {
            pthread_mutex_unlock(&impl->mutex);
            return TUXRT_ERR_NOMEM;
        }
    }

    /* Open interface */
    ChronosRT_status_t st = ChronosRT_et_master_open_auto(impl->master, impl->ifname);
    if (st != ChronosRT_SUCCESS) {
        if (impl->master) {
            ChronosRT_et_master_destroy(impl->master);
            impl->master = NULL;
        }
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_IO;
    }

    /* Re-apply configuration */
    (void)ethertux_init_features(impl, &adapter->config);

    impl->configured = true;
    adapter->active = true;

    pthread_mutex_unlock(&impl->mutex);
    tuxrt_sirc_update_health_success(adapter);

    return TUXRT_OK;
}

/* Multi-NIC: Deactivate adapter */
static tuxrt_err_t ethertux_deactivate(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) {
        return TUXRT_ERR_INVAL;
    }

    ethertux_transport_impl_t* impl = (ethertux_transport_impl_t*)adapter->impl;

    pthread_mutex_lock(&impl->mutex);

    adapter->active = false;

    if (impl->master) {
        ChronosRT_et_master_close(impl->master);
        ChronosRT_et_master_destroy(impl->master);
        impl->master = NULL;
    }
    impl->configured = false;

    pthread_mutex_unlock(&impl->mutex);

    return TUXRT_OK;
}

/* Export the vtable for EtherTux transport (EtherType 0x88B5) */
CHRONOSRT_API const tuxrt_transport_vtable_t tuxrt_transport_ethertux_vtable = {
    .abi_version = 1,
    .open = ethertux_open,
    .close = ethertux_close,
    .send = ethertux_send,
    .recv = ethertux_recv,
    .flush = ethertux_flush,
    .get_stats = ethertux_get_stats,
    .destroy = ethertux_destroy,
    .activate = ethertux_activate,
    .deactivate = ethertux_deactivate
};

/* End of EtherTux transport implementation */