/**
 * transport_ethercat.c — EtherCAT Transport Adapter
 *
 * Implements tuxrt_transport_vtable_t for deterministic real-time
 * communication via EtherCAT (EtherType 0x88A4) using the ChronosRT
 * EtherCAT master backend.
 *
 * This transport provides ultra-low latency, deterministic operation with:
 * - Pre-allocated buffers (no malloc in hot path)
 * - Cycle time synchronization
 * - PDO mapping for process data
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "chronosrt_transport.h"
#include "chronosrt_ethercat.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* Transport implementation state */
typedef struct {
    ChronosRT_ec_master_t* master;
    char ifname[32];
    pthread_mutex_t mutex;
    bool configured;
    uint32_t cycle_time_us;
} ethercat_transport_impl_t;

static tuxrt_err_t ethercat_open(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) {
        return TUXRT_ERR_INVAL;
    }

    ethercat_transport_impl_t* impl = calloc(1, sizeof(ethercat_transport_impl_t));
    if (!impl) {
        return TUXRT_ERR_NOMEM;
    }

    pthread_mutex_init(&impl->mutex, NULL);

    /* Get interface name from config */
    if (adapter->config.device_path) {
        strncpy(impl->ifname, adapter->config.device_path, sizeof(impl->ifname) - 1);
    } else {
        strncpy(impl->ifname, "eth0", sizeof(impl->ifname) - 1);
    }

    /* Create EtherCAT master */
    impl->master = ChronosRT_ec_master_create();
    if (!impl->master) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_NOMEM;
    }

    /* Open interface */
    ChronosRT_status_t st = ChronosRT_ec_master_open(impl->master, impl->ifname);
    if (st != ChronosRT_SUCCESS) {
        ChronosRT_ec_master_destroy(impl->master);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }

    /* Configure cycle time from bitrate if available */
    if (adapter->config.bitrate > 0) {
        impl->cycle_time_us = 1000000 / adapter->config.bitrate;
    } else {
        impl->cycle_time_us = 1000;  /* 1ms default cycle time */
    }

    pthread_mutex_lock(&adapter->mutex);
    impl->configured = true;
    adapter->impl = impl;
    pthread_mutex_unlock(&adapter->mutex);

    tuxrt_sirc_update_health_success(adapter);

    return TUXRT_OK;
}

static tuxrt_err_t ethercat_close(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) {
        return TUXRT_ERR_INVAL;
    }

    ethercat_transport_impl_t* impl = (ethercat_transport_impl_t*)adapter->impl;

    pthread_mutex_lock(&impl->mutex);

    if (impl->master) {
        ChronosRT_ec_master_close(impl->master);
        ChronosRT_ec_master_destroy(impl->master);
    }
    impl->configured = false;

    pthread_mutex_unlock(&impl->mutex);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    adapter->impl = NULL;
    adapter->active = false;

    return TUXRT_OK;
}

static tuxrt_err_t ethercat_send(tuxrt_transport_adapter_t* adapter,
                                 const uint8_t* data, size_t len,
                                 tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !data || len == 0) {
        return TUXRT_ERR_INVAL;
    }

    ethercat_transport_impl_t* impl = (ethercat_transport_impl_t*)adapter->impl;
    (void)deadline;

    pthread_mutex_lock(&impl->mutex);

    if (!impl->configured || !impl->master) {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_NOT_INITIALIZED;
    }

    /* Send process data (PDO) via EtherCAT */
    int ret = ChronosRT_ec_master_send_processdata(impl->master);
    if (ret < 0) {
        pthread_mutex_unlock(&impl->mutex);
        tuxrt_sirc_update_health_failure(adapter);
        return TUXRT_ERR_IO;
    }

    pthread_mutex_unlock(&impl->mutex);
    tuxrt_sirc_update_health_success(adapter);

    return TUXRT_OK;
}

static tuxrt_err_t ethercat_recv(tuxrt_transport_adapter_t* adapter,
                                 uint8_t* buf, size_t* len,
                                 tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !buf || !len || *len == 0) {
        return TUXRT_ERR_INVAL;
    }

    ethercat_transport_impl_t* impl = (ethercat_transport_impl_t*)adapter->impl;

    uint32_t timeout_us = 0;
    if (deadline > 0) {
        int64_t now = tuxrt_now_ns();
        if (deadline > now) {
            int64_t delta_us = (deadline - now) / 1000;
            timeout_us = (delta_us > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)delta_us;
        }
    } else {
        timeout_us = impl->cycle_time_us;
    }

    pthread_mutex_lock(&impl->mutex);

    if (!impl->configured || !impl->master) {
        pthread_mutex_unlock(&impl->mutex);
        *len = 0;
        return TUXRT_ERR_NOT_INITIALIZED;
    }

    /* Receive process data */
    int ret = ChronosRT_ec_master_receive_processdata(impl->master, timeout_us);
    if (ret < 0) {
        pthread_mutex_unlock(&impl->mutex);
        *len = 0;
        tuxrt_sirc_update_health_failure(adapter);
        return TUXRT_ERR_IO;
    }

    /* Get the received data length from the master's IOMap */
    const uint8_t* iomap = ChronosRT_ec_master_iomap(impl->master);
    uint32_t iomap_size = ChronosRT_ec_master_iomap_size(impl->master);
    if (iomap && iomap_size > 0 && *len >= iomap_size) {
        memcpy(buf, iomap, iomap_size);
        *len = (size_t)iomap_size;
    } else {
        *len = 0;
    }

    pthread_mutex_unlock(&impl->mutex);
    tuxrt_sirc_update_health_success(adapter);

    return TUXRT_OK;
}

static tuxrt_err_t ethercat_flush(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) {
        return TUXRT_ERR_INVAL;
    }

    ethercat_transport_impl_t* impl = (ethercat_transport_impl_t*)adapter->impl;

    pthread_mutex_lock(&impl->mutex);

    if (!impl->configured || !impl->master) {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_NOT_INITIALIZED;
    }

    /* Force process data update */
    (void)ChronosRT_ec_master_send_processdata(impl->master);

    pthread_mutex_unlock(&impl->mutex);
    return TUXRT_OK;
}

static tuxrt_err_t ethercat_get_stats(tuxrt_transport_adapter_t* adapter,
                                       void* stats_out) {
    if (!adapter || !stats_out) {
        return TUXRT_ERR_INVAL;
    }

    pthread_mutex_lock(&adapter->mutex);
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    pthread_mutex_unlock(&adapter->mutex);
    return TUXRT_OK;
}

static void ethercat_destroy(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) {
        return;
    }
    ethercat_close(adapter);
}

/* Multi-NIC: Activate adapter for multi-NIC operation */
static tuxrt_err_t ethercat_activate(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) {
        return TUXRT_ERR_INVAL;
    }

    ethercat_transport_impl_t* impl = (ethercat_transport_impl_t*)adapter->impl;

    pthread_mutex_lock(&impl->mutex);

    if (impl->configured && impl->master) {
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_BUSY;  /* Already active */
    }

    /* Re-open if needed */
    if (!impl->master) {
        impl->master = ChronosRT_ec_master_create();
        if (!impl->master) {
            pthread_mutex_unlock(&impl->mutex);
            return TUXRT_ERR_NOMEM;
        }
    }

    ChronosRT_status_t st = ChronosRT_ec_master_open(impl->master, impl->ifname);
    if (st != ChronosRT_SUCCESS) {
        if (impl->master) {
            ChronosRT_ec_master_destroy(impl->master);
            impl->master = NULL;
        }
        pthread_mutex_unlock(&impl->mutex);
        return TUXRT_ERR_IO;
    }

    impl->configured = true;
    adapter->active = true;

    pthread_mutex_unlock(&impl->mutex);
    tuxrt_sirc_update_health_success(adapter);

    return TUXRT_OK;
}

/* Multi-NIC: Deactivate adapter */
static tuxrt_err_t ethercat_deactivate(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) {
        return TUXRT_ERR_INVAL;
    }

    ethercat_transport_impl_t* impl = (ethercat_transport_impl_t*)adapter->impl;

    pthread_mutex_lock(&impl->mutex);

    adapter->active = false;

    if (impl->master) {
        ChronosRT_ec_master_close(impl->master);
        ChronosRT_ec_master_destroy(impl->master);
        impl->master = NULL;
    }
    impl->configured = false;

    pthread_mutex_unlock(&impl->mutex);

    return TUXRT_OK;
}

/* Export the vtable for EtherCAT transport (EtherType 0x88A4) */
CHRONOSRT_API const tuxrt_transport_vtable_t tuxrt_transport_ethercat_vtable = {
    .abi_version = 1,
    .open = ethercat_open,
    .close = ethercat_close,
    .send = ethercat_send,
    .recv = ethercat_recv,
    .flush = ethercat_flush,
    .get_stats = ethercat_get_stats,
    .destroy = ethercat_destroy,
    .activate = ethercat_activate,
    .deactivate = ethercat_deactivate
};

/* End of EtherCAT transport implementation */