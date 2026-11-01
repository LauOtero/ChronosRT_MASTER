# Manual Técnico para Desarrollador - ChronosRT Master

**Versión del Software:** 0.1.0  
**Fecha:** Noviembre 2026  

---

## Tabla de Contenidos

1. [Arquitectura del Sistema](#1-arquitectura-del-sistema)
2. [Protocolos de Comunicación](#2-protocolos-de-comunicación)
3. [Gestión de Esclavos](#3-gestión-de-esclavos)
4. [Configuración Avanzada de Comunicación](#4-configuración-avanzada-de-comunicación)
5. [API de Desarrollo](#5-api-de-desarrollo)
6. [Debugging y Logging](#6-debugging-y-logging)
7. [Actualización y Mantenimiento](#7-actualización-y-mantenimiento)
8. [Redundancia y Alta Disponibilidad](#8-redundancia-y-alta-disponibilidad)
9. [Análisis de Rendimiento y Optimización](#9-análisis-de-rendimiento-y-optimización)
10. [Seguridad y Cifrado](#10-seguridad-y-cifrado)
11. [Maestro EtherCAT de ChronosRT](#11-maestro-ethercat-de-chronosrt)
12. [Librerías y Dependencias](#12-librerías-y-dependencias)
13. [EtherTux y HAL de aceleración por hardware](#13-ethertux-y-hal-de-aceleración-por-hardware)

---

## 1. Arquitectura del Sistema

### 1.1 Vista General de la Arquitectura

ChronosRT Master implementa una arquitectura de capas con responsabilidades bien definidas:

```
┌──────────────────────────────────────────────────────────────────────┐
│                        CAPA DE APLICACIÓN                            │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                   │
│  │   Usuario   │  │  Benchmark  │  │   Tests     │                   │
│  │    API      │  │   Suite     │  │  Framework  │                   │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                   │
├─────────┴────────────────┴────────────────┴──────────────────────────┤
│                        CAPA DE SERVICIOS                             │
│  ┌────────────────────┐  ┌──────────────────┐  ┌──────────────────┐  │
│  │ Event Service      │  │ Memory Service   │  │ Protocolos       │  │
│  │ (Publish/Subscribe)│  │ (Pool/NUMA/Huge) │  │ EtherCAT/EtherTux│  │
│  └────────────────────┘  └──────────────────┘  └──────────────────┘  │
├──────────────────────────────────────────────────────────────────────┤
│                        CAPA DE TRANSPORTE                            │
│  ┌──────────────────┐  ┌──────────────────┐  ┌────────────────────┐  │
│  │ Ring SPSC        │  │ Bounded MPMC     │  │ HAL CRC-32C / RS   │  │
│  │ (lock-free)      │  │ (Vyukov, sin ABA)│  │ AF_PACKET + TX ring│  │
│  └──────────────────┘  └──────────────────┘  └────────────────────┘  │
├──────────────────────────────────────────────────────────────────────┤
│                        CAPA DE HARDWARE                              │
│  ┌───────────┐  ┌────────────┐  ┌───────────┐  ┌────────────┐        │
│  │ x86_64    │  │ ARM64/NEON │  │ AVX-512   │  │ NIC / ESC  │        │
│  │ SSE4.2    │  │ CRC32/SVE  │  │ AVX2      │  │ PHC        │        │
│  └───────────┘  └────────────┘  └───────────┘  └────────────┘        │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.2 Arquitectura de Componentes Internos

#### Diagrama de Componentes

```
ChronosRT Master Core
├── EventManager
│   ├── EventPool (pre-asignado)
│   ├── BoundedQueue (Vyukov, MPMC)
│   └── RingBuffer (SPSC)
├── MemoryManager
│   ├── MemoryPool (free-list lock-free)
│   ├── HugePageAllocator (mmap/MADV_HUGEPAGE)
│   └── NUMAAwareAllocator (mbind)
├── RTManager
│   ├── Scheduler (SCHED_DEADLINE / SCHED_FIFO)
│   ├── AffinityManager
│   ├── TickTimer (timerfd)
│   └── LatencyHistogram / Watchdog
├── ProtocolManager
│   ├── EtherCAT (ec_frame/socket/master/slave/dc/mailbox)
│   ├── EtherTux (et_frame/fec/qos/sched/esm/sync/topology/mbx/flowagg/frm/socket/master)
│   └── Protocol selection (chronosrt_protocol)
├── ResilienceManager
│   ├── Snapshot A/B (seqlock)
│   ├── WAL (CRC + fsync por lotes)
│   └── GoldenImage (rename atómico)
├── Observability
│   ├── Counters
│   ├── Trace (trace_marker)
│   └── Lock-free selftest
└── HWAccel
    ├── CRC-32C (SSE4.2 / ARMv8.1 / slice-by-8)
    ├── GF(2^8) y Reed-Solomon
    └── PHC / Launch Time
```

### 1.3 Análisis de Complejidad Algorítmica

| Operación | Complejidad Temporal | Complejidad Espacial | Operaciones Atómicas |
|-----------|---------------------|---------------------|---------------------|
| `event_publish()` | O(1) | O(1) | 1 push a cola bounded |
| `event_subscribe()` | O(1) | O(1) | 1 pop de cola bounded |
| `ring_push()` | O(1) | O(1) | 1 store + 1 fetch_add |
| `ring_pop()` | O(1) | O(1) | 1 store + 1 fetch_sub |
| `bounded_push()` | O(1) | O(1) | 1 CAS |
| `bounded_pop()` | O(1) | O(1) | 1 CAS |
| `mem_pool_alloc()` | O(1) | O(1) | 1 pop de free-list |
| `mem_pool_free()` | O(1) | O(1) | 1 push a free-list |

La cola Michael-Scott (`chronosrt_mpmc.h`) se conserva como referencia; su
`push`/`pop` asigna y libera nodos, por lo que **no** se usa en el camino
caliente (riesgo de ABA y `malloc` por operación).

### 1.4 Diagrama de Estados del Sistema

```
                    ┌──────────────────┐
                    │      INIT        │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │   CONFIGURING    │
                    │  (Load Config)   │
                    └────────┬─────────┘
                             │
              ┌──────────────┴──────────────┐
              │                             │
              ▼                             ▼
    ┌──────────────────┐          ┌──────────────────┐
    │  INITIALIZED     │          │    ERROR         │
    │  (Ready Start)   │          │  (Config Fail)   │
    └────────┬─────────┘          └──────────────────┘
             │
             ▼
    ┌──────────────────┐
    │    STARTING      │
    │ (Create Threads) │
    └────────┬─────────┘
             │
             ▼
    ┌──────────────────┐
    │    RUNNING       │
    │ (Processing)     │
    └────────┬─────────┘
             │
    ┌────────┴────────┐
    │                 │
    ▼                 ▼
┌─────────┐    ┌─────────────┐
│ STOPPING│    │  PAUSED     │
│(Cleanup)│    │ (Suspended) │
└────┬────┘    └─────────┬───┘
     │                   │
     ▼                   ▼
┌──────────────────┐ ┌────────────┐
│     STOPPED      │ │   RESUME   │
│   (Terminated)   │ └──────┬─────┘
└──────────────────┘        │
                            │
                      ┌─────┴─────┐
                      │  BACK TO  │
                      │  RUNNING  │
                      └───────────┘
```

---

## 2. Protocolos de Comunicación

### 2.1 Protocolo de Eventos Internos

ChronosRT Master utiliza un protocolo de eventos optimizado para mínima latencia:

```c
// Estructura de evento interno (64 bytes - cache line)
typedef struct ChronosRT_CACHE_ALIGNED {
    volatile uint64_t sequence;        //  0-7:   Número de secuencia
    volatile uint64_t timestamp_ns;    //  8-15:  Timestamp de creación
    volatile uint32_t cpu_id;          // 16-19:  CPU que creó el evento
    volatile uint8_t  data[56];        // 20-63:  Payload (56 bytes)
} ChronosRT_event_t;
```

La estructura **no** incluye prioridad ni tamaño explícito: el tamaño lo da la
llamada `ChronosRT_event_publish(handle, data, size)` y se limita a 56 bytes.


### 2.2 Protocolo de Red (maestro EtherCAT)

ChronosRT Master soporta **dos protocolos de red seleccionables** (nunca simultáneos sobre la
misma interfaz). El que se describe en este apartado es el **maestro EtherCAT** sobre Ethernet
crudo (`AF_PACKET`, EtherType **0x88A4**), conforme a IEC 61158 / ETG.1000. El otro, **EtherTux**
(EtherType **0x88B5**), se documenta en el apartado 13. La elección se resuelve en tiempo de
configuración mediante `chronosrt_protocol.h` (`CHRONOSRT_PROTOCOL=ethercat|ethertux`).

**Trama EtherCAT:**

```c
typedef struct {                       // 14 B
    uint8_t  dst_mac[6];               // difusión FF:FF:FF:FF:FF:FF en el escaneo
    uint8_t  src_mac[6];               // MAC de la interfaz del maestro
    uint16_t ether_type;               // 0x88A4
} ChronosRT_ec_eth_header_t;

/* Cabecera EtherCAT (2 B): longitud (11 bits) | tipo (4 bits = 1) */
typedef struct {                       // 2 B
    uint16_t length_type;              // (len & 0x07FF) | (1u << 12)
} ChronosRT_ec_header_t;
```

**Datagrama EtherCAT** (tras la cabecera, todos los campos little-endian):

```c
typedef struct {                       // 10 B + datos + 2 B
    uint8_t  cmd;                      // comando (APRD, LRW, …)
    uint8_t  idx;                      // índice (para casar respuesta)
    uint16_t adp;                      // dirección de posición  } address =
    uint16_t ado;                      // dirección de offset    } adp | (ado<<16)
    uint16_t dlength;                  // 11 bits de longitud + bit M (0x8000)
    uint16_t irq;                      // interrupción (0)
    /* uint8_t data[dlength & 0x07FF]; */
    /* uint16_t wkc;                    // contador de trabajo */
} ChronosRT_ec_datagram_t;
```

El **bit M** (`0x8000` del campo `dlength`) indica que siguen más datagramas en la misma trama.
El **contador de trabajo (WKC)** cuenta cuántas esclavas procesaron el datagrama; el maestro lo
valida frente al valor esperado para detectar pérdidas.

**Comandos soportados:** `NOP`, `APRD/APWR/APRW` (posición), `FPRD/FPWR/FPRW` (estación configurada),
`BRD/BWR/BRW` (difusión), `LRD/LWR/LRW` (lógico), `ARMW/FRMW` (lectura+escritura con máscara).

**API de serialización (pura y testeable sin hardware)** en `chronosrt_ethercat.h`:

```c
uint32_t ChronosRT_ec_datagram_build(uint8_t* buf, uint32_t buf_len, uint8_t cmd,
                                     uint8_t idx, uint32_t address,
                                     const void* data, uint16_t data_len, bool more);
uint32_t ChronosRT_ec_datagram_parse(const uint8_t* buf, uint32_t buf_len,
                                     ChronosRT_ec_datagram_t* out);
uint16_t ChronosRT_ec_frame_header(uint8_t out[2], uint16_t datagram_bytes);
```

### 2.3 Diagrama de Secuencia - Comunicación Maestro-Esclavo

```
┌──────────────┐      ┌──────────────┐      ┌──────────────┐
│   Master     │      │   Network    │      │    Slave     │
│  (ChronosRT) │      │   Switch     │      │   Device     │
└──────┬───────┘      └──────┬───────┘      └──────┬───────┘
       │                     │                     │
       │   ┌─── EtherCAT FPRD ───┐                 │
       │   │ (Process Data Read) │                 │
       │───▶────────────────────▶│                 │
       │   └─────────────────────┘                 │
       │                      │                    │
       │   ┌─── EtherCAT FPRW ────┐                │
       │   │ (Process Data Write) │                │
       │◀──│◀─────────────────────│                │
       │   └──────────────────────┘                │
       │                      │                    │
       │   ┌─ LRW (Logical Read Write) ─┐          │
       │──▶│◀══════════════════════════▶│─────────▶│
       │   └────────────────────────────┘          │
       │                      │                    │
       │   ┌─ Read/Write Mailbox ─┐                │
       │◀──▶│◀═══════════════════▶│◀───────────────│
       │   └──────────────────────┘                │
       │                      │                    │
       │                      │                    │
       │◀─── State Machine (ESM) ─────────────────▶│
       │                      │                    │
       │                      │                    │
       └──────────────────────┴────────────────────┘
                    OPERATION CYCLE
```

### 2.4 Máquina de Estados EtherCAT (ESM)

```
┌────────────────────────────────────────────────────────────────┐
│                    ETHERCAT STATE MACHINE (ESM)                │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐  │
│  │  Init    │───▶│ Pre-Op   │───▶│ Safe-Op  │───▶│    Op    │  │
│  └──────────┘    └──────────┘    └──────────┘    └──────────┘  │
│       ▲               ▲               ▲                ▲       │
│       │               │               │                │       │
│       └───────────────┴───────────────┴────────────────┘       │
│                    (Transition Commands)                       │
│                                                                │
│  State Descriptions:                                           │
│  ─────────────────                                             │
│  INIT:       No communication, local initialization only       │
│  PRE-OP:     Mailbox communication, no process data            │
│  SAFE-OP:    Mailbox + process data input, no output           │
│  OP:         Full operation, inputs and outputs                │
│                                                                │
│  Transitions:                                                  │
│  ───────────                                                   │
│  Init → Pre-Op:     STARTUP command                            │
│  Pre-Op → Safe-Op:  FREEZING command                           │
│  Safe-Op → Op:      RUN command                                │
│  Op → Safe-Op:      FREEZING command                           │
│  Safe-Op → Pre-Op:  STOP command                               │
│  Any → Init:        INIT command                               │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```

---

## 3. Gestión de Esclavos

### 3.1 Estructura de Datos del Esclavo

```c
// Información del esclavo EtherCAT
typedef struct ec_slave {
    uint16_t vendor_id;            // Identificador del vendor
    uint16_t product_code;         // Código del producto
    uint32_t serial_number;        // Número de serie
    uint16_t alias;                // Alias (configurable)
    char     name[EC_MAXNAME];     // Nombre del dispositivo
    uint8_t  current_state;        // Estado actual ESM
    uint8_t  requested_state;      // Estado solicitado
    uint16_t alias_address;        // Dirección de alias

    // Información de Sync Managers
    struct {
        uint16_t start_address;    // Dirección inicial
        uint16_t length;           // Longitud
        uint8_t  control_register; // Registro de control
        uint8_t  status_register;  // Registro de estado
        bool     is_legal;         // Configuración válida
    } sync_managers[EC_MAXSM];

    // FMMU (Fieldbus Memory Management Unit)
    struct {
        uint32_t logical_address; // Dirección lógica
        uint8_t  length;          // Longitud en bytes
        uint8_t  start_bit;       // Bit inicial
        uint8_t  type;            // Tipo (input/output)
        bool     active;          // Activo
    } fmmus[EC_MAXFMMU];

    // PDO Mapping
    struct {
        uint16_t index;           // Índice de PDO
        uint8_t  subindex;        // Subíndice
        uint8_t  bit_length;      // Longitud en bits
        uint32_t entry;           // Entry completo
    } pdo_mapping[EC_MAXPDO];

    uint16_t pdo_count;           // Número de PDOs mapeados
    uint16_t eeprom_size;         // Tamaño de EEPROM

    // Timestamps
    uint64_t last_heartbeat_ns;   // Último heartbeat
    uint32_t error_count;         // Contador de errores
} ec_slave_t;
```

### 3.2 Procedimiento de Inicialización de Esclavo

```c
/**
 * Inicialización completa de un esclavo EtherCAT
 *
 * @param slave Puntero al esclavo a inicializar
 * @return código de error o 0 si éxito
 */
int ec_slave_init(ec_slave_t* slave) {
    // 1. Leer información del esclavo desde EEPROM
    int ret = ec_read_slave_identity(slave);
    if (ret != 0) {
        log_error("Failed to read slave identity: %d", ret);
        return ret;
    }

    // 2. Configurar direccionamiento
    ret = ec_configure_alias_address(slave, slave->alias);
    if (ret != 0) {
        log_error("Failed to configure alias: %d", ret);
        return ret;
    }

    // 3. Inicializar Mailbox
    ret = ec_init_mailbox(slave);
    if (ret != 0) {
        log_error("Mailbox init failed: %d", ret);
        return ret;
    }

    // 4. Configurar SM (Sync Managers)
    ret = ec_configure_sync_managers(slave);
    if (ret != 0) {
        log_error("SM configuration failed: %d", ret);
        return ret;
    }

    // 5. Mapear PDOs
    ret = ec_map_pdos(slave);
    if (ret != 0) {
        log_error("PDO mapping failed: %d", ret);
        return ret;
    }

    // 6. Configurar FMMUs
    ret = ec_configure_fmmus(slave);
    if (ret != 0) {
        log_error("FMMU configuration failed: %d", ret);
        return ret;
    }

    // 7. Solicitar estado SAFE-OP
    ret = ec_set_state(slave, EC_STATE_SAFE_OP);
    if (ret != 0) {
        log_error("Failed to set SAFE-OP: %d", ret);
        return ret;
    }

    // 8. Verificar estado
    ret = ec_verify_state(slave, EC_STATE_SAFE_OP, 1000); // 1s timeout
    if (ret != 0) {
        log_error("State verification failed: %d", ret);
        return ret;
    }

    return 0;
}
```

### 3.3 Monitoreo de Esclavos

```c
// Estructura para monitoreo de estado
typedef struct {
    uint16_t slave_index;
    uint64_t last_update_ns;
    uint32_t error_count;
    uint8_t  state;
    uint16_t wkc;                  // Working Counter
    uint64_t latency_min_ns;
    uint64_t latency_max_ns;
    uint64_t latency_avg_ns;
} slave_monitor_t;

// Hilo de monitoreo de esclavos
void* slave_monitor_thread(void* arg) {
    ChronosRT_handle_t handle = (ChronosRT_handle_t)arg;

    while (g_running) {
        for (int i = 0; i < g_slave_count; i++) {
            uint64_t start = ChronosRT_get_timestamp_ns();

            // Enviar FRMW (Force Read Multiple Words)
            int wkc = ec_send_frdw(g_slave_list[i].station_address);

            uint64_t end = ChronosRT_get_timestamp_ns();
            uint64_t latency = end - start;

            // Actualizar métricas
            update_slave_metrics(i, wkc, latency);
        }

        // Reportar estado cada segundo
        report_slave_status();

        usleep(1000);  // 1ms ciclo
    }

    return NULL;
}

// Actualización de métricas
void update_slave_metrics(int slave_idx, int wkc, uint64_t latency) {
    slave_monitor_t* mon = &g_slave_monitors[slave_idx];

    mon->last_update_ns = ChronosRT_get_timestamp_ns();
    mon->wkc = wkc;

    if (wkc == 0) {
        mon->error_count++;
        log_warning("Slave %d: WKC=0 (communication error)", slave_idx);
    }

    // Actualizar latencia
    if (latency < mon->latency_min_ns) mon->latency_min_ns = latency;
    if (latency > mon->latency_max_ns) mon->latency_max_ns = latency;

    // Calcular promedio móvil
    mon->latency_avg_ns = (mon->latency_avg_ns * 7 + latency) / 8;
}
```

### 3.4 Detección y Recuperación de Errores

```c
// Códigos de error de esclavo
typedef enum {
    EC_SLAVE_OK             = 0,
    EC_SLAVE_ERROR_COMMS    = 1,    // Error de comunicación
    EC_SLAVE_ERROR_TIMEOUT  = 2,    // Timeout de respuesta
    EC_SLAVE_ERROR_STATE    = 3,    // Estado incorrecto
    EC_SLAVE_ERROR_WKC      = 4,    // Working Counter inválido
    EC_SLAVE_ERROR_SYNC     = 5,    // Error de sincronización
    EC_SLAVE_ERROR_FMMU     = 6,    // Error FMMU
    EC_SLAVE_ERROR_PDO      = 7,    // Error de PDO mapping
    EC_SLAVE_ERROR_CONFIG   = 8     // Configuración inválida
} ec_slave_error_t;

// Procedimiento de recuperación
int ec_recover_slave(ec_slave_t* slave) {
    log_info("Attempting recovery of slave %s", slave->name);

    // 1. Contador de intentos
    static int recovery_attempts[EC_MAXSLAVE] = {0};

    if (recovery_attempts[slave->index] > 3) {
        log_error("Max recovery attempts reached for slave %d", slave->index);
        recovery_attempts[slave->index] = 0;
        return -1;
    }

    // 2. Intentar transition a INIT
    int ret = ec_set_state(slave, EC_STATE_INIT);
    if (ret != 0) {
        log_error("Failed to set INIT state: %d", ret);
        recovery_attempts[slave->index]++;
        return ret;
    }

    usleep(100000);  // 100ms espera

    // 3. Re-inicializar
    ret = ec_slave_init(slave);
    if (ret != 0) {
        log_error("Re-initialization failed: %d", ret);
        recovery_attempts[slave->index]++;
        return ret;
    }

    // 4. Volver a Safe-Op
    ret = ec_set_state(slave, EC_STATE_SAFE_OP);
    if (ret != 0) {
        log_error("Failed to return to SAFE-OP: %d", ret);
        recovery_attempts[slave->index]++;
        return ret;
    }

    recovery_attempts[slave->index] = 0;
    log_info("Slave %s recovered successfully", slave->name);

    return 0;
}
```

---

## 4. Configuración Avanzada de Comunicación

### 4.1 Parámetros de Comunicación

```c
// Estructura de configuración avanzada de red
typedef struct {
    // Timeouts (en microsegundos)
    uint32_t timeout_init;         // Timeout inicialización
    uint32_t timeout_cycle;        // Timeout de ciclo
    uint32_t timeout_mailbox;       // Timeout mailbox
    uint32_t timeout_peer;          // Timeout de peer

    // Sizes
    uint16_t max_slaves;           // Máximo de esclavos
    uint16_t max_mbx_size;         // Tamaño máximo de mailbox
    uint16_t max_pdo_size;         // Tamaño máximo de PDO

    // Performance
    uint8_t  cycle_interval_ns;    // Intervalo de ciclo en ns
    uint8_t  retries;              // Número de reintentos
    bool     use_watchdog;         // Habilitar watchdog
    uint16_t watchdog_interval_ms;  // Intervalo de watchdog

    // Advanced
    bool     use_dc;               // Distributed Clock
    uint32_t dc_cycle_ns;          // Ciclo DC en ns
    uint32_t dc_sync0_offset_ns;   // Offset SYNC0
    uint32_t dc_sync1_offset_ns;   // Offset SYNC1
} ec_com_config_t;
```

### 4.2 Configuración de Distributed Clocks

```c
// Configuración de Distributed Clocks (DC)
int ec_configure_dc(ec_slave_t* slave, uint32_t cycle_ns) {
    // 1. Leer capacidad DC del esclavo
    uint32_t capabilities = ec_read_dc_capabilities(slave);
    if (!(capabilities & EC_DC_SUPPORTED)) {
        log_warning("Slave %s does not support DC", slave->name);
        return -1;
    }

    // 2. Configurar tiempo de ciclo
    int ret = ec_write_dc_cycle_time(slave, cycle_ns);
    if (ret != 0) return ret;

    // 3. Configurar offsets
    ret = ec_write_dc_sync0_time(slave, 0);  // SYNC0 al inicio del ciclo
    if (ret != 0) return ret;

    ret = ec_write_dc_sync1_time(slave, cycle_ns / 2);  // SYNC1 a mitad
    if (ret != 0) return ret;

    // 4. Habilitar DC
    ret = ec_write_dc_control(slave, EC_DC_ENABLE);
    if (ret != 0) return ret;

    // 5. Sincronizar con maestro
    ret = ec_dc_sync(slave, 1000000);  // 1ms timeout
    if (ret != 0) return ret;

    log_info("DC configured for %s: cycle=%dns", slave->name, cycle_ns);
    return 0;
}

// Algoritmo de sincronización DC
int ec_dc_sync(ec_slave_t* slave, uint32_t timeout_us) {
    uint64_t start_ns = ChronosRT_get_timestamp_ns();
    uint64_t target_time;
    int64_t offset;

    // 1. Obtener tiempo actual del maestro
    uint64_t master_time = get_master_time_ns();

    // 2. Leer tiempo del esclavo
    uint64_t slave_time = ec_read_dc_time(slave);

    // 3. Calcular offset
    offset = (int64_t)slave_time - (int64_t)master_time;

    // 4. Aplicar corrección
    if (offset > EC_DC_MAX_OFFSET) {
        // Ajuste grande - puede causar glitch
        ec_write_dc_time(slave, master_time);
        log_warning("Large DC offset correction: %lld ns", offset);
    } else if (offset > EC_DC_SMALL_OFFSET) {
        // Ajuste gradual
        uint32_t correction = (uint32_t)(offset / 2);
        ec_write_dc_time_offset(slave, correction);
    }

    // 5. Esperar y verificar
    usleep(100);  // 100us para que tome efecto

    uint64_t verify_time = ec_read_dc_time(slave);
    uint64_t new_offset = (verify_time > master_time) ?
        verify_time - master_time : master_time - verify_time;

    if (new_offset > EC_DC_MAX_OFFSET) {
        log_error("DC sync failed: offset=%llu", new_offset);
        return -1;
    }

    log_debug("DC sync complete: offset=%lld ns", (int64_t)new_offset);
    return 0;
}
```

### 4.3 Configuración de PDOs

```c
// Mapeo de PDO dinámico
int ec_configure_pdo_mapping(ec_slave_t* slave,
                              uint16_t pdo_index,
                              ec_pdo_entry_t* entries,
                              int entry_count) {

    // 1. Buscar PDO por índice
    ec_pdo_t* pdo = ec_find_pdo(slave, pdo_index);
    if (!pdo) {
        log_error("PDO 0x%04X not found", pdo_index);
        return -1;
    }

    // 2. Validar entradas
    uint16_t total_bits = 0;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].bit_length == 0 || entries[i].bit_length > 64) {
            log_error("Invalid bit length for entry %d", i);
            return -1;
        }
        total_bits += entries[i].bit_length;
    }

    if (total_bits > 64) {
        log_error("PDO mapping exceeds 64 bits: %d", total_bits);
        return -1;
    }

    // 3. Configurar entradas
    for (int i = 0; i < entry_count; i++) {
        int ret = ec_pdo_add_entry(pdo,
                                    entries[i].index,
                                    entries[i].subindex,
                                    entries[i].bit_length);
        if (ret != 0) {
            log_error("Failed to add PDO entry 0x%04X/%d",
                     entries[i].index, entries[i].subindex);
            return ret;
        }
    }

    // 4. Activar PDO
    return ec_pdo_activate(pdo);
}

// Estructura de entrada PDO
typedef struct {
    uint16_t index;       // Índice de objeto
    uint8_t  subindex;    // Subíndice
    uint8_t  bit_length;   // Longitud en bits
    void*    data_ptr;     // Puntero a datos
} ec_pdo_entry_t;
```

---

## 5. API de Desarrollo

### 5.1 API Completa de Eventos

```c
/**
 * Inicialización del sistema ChronosRT Master
 *
 * @param handle Puntero al handle de salida
 * @param config Configuración del sistema
 * @return ChronosRT_SUCCESS o código de error
 */
ChronosRT_status_t ChronosRT_init(ChronosRT_handle_t* handle,
                                   const ChronosRT_config_t* config);

/**
 * Cierre del sistema
 *
 * @param handle Handle de ChronosRT Master
 * @return ChronosRT_SUCCESS o código de error
 */
ChronosRT_status_t ChronosRT_shutdown(ChronosRT_handle_t handle);

/**
 * Publicar un evento
 *
 * @param handle Handle de ChronosRT Master
 * @param data Puntero a datos del evento
 * @param size Tamaño de datos (máx 56 bytes)
 * @return ChronosRT_SUCCESS o código de error
 */
ChronosRT_status_t ChronosRT_event_publish(ChronosRT_handle_t handle,
                                            const void* data,
                                            uint32_t size);

/**
 * Suscribirse a un evento
 *
 * @param handle Handle de ChronosRT Master
 * @param event Evento de salida
 * @param timeout_us Timeout en microsegundos
 * @return ChronosRT_SUCCESS, TIMEOUT, o código de error
 */
ChronosRT_status_t ChronosRT_event_subscribe(ChronosRT_handle_t handle,
                                              ChronosRT_event_t* event,
                                              uint32_t timeout_us);

/**
 * Ejecutar un benchmark bajo demanda
 *
 * @param handle Handle de ChronosRT Master
 * @param mode Modo (LATENCY, THROUGHPUT, JITTER, STRESS, WCET)
 * @param duration_sec Duración en segundos
 * @param metrics Estructura de métricas de salida
 * @return ChronosRT_SUCCESS o código de error
 */
ChronosRT_status_t ChronosRT_benchmark_run(ChronosRT_handle_t handle,
                                           ChronosRT_benchmark_mode_t mode,
                                           uint32_t duration_sec,
                                           ChronosRT_metrics_t* metrics);

/** Imprime un reporte formateado de las métricas. */
ChronosRT_status_t ChronosRT_metrics_report(const ChronosRT_metrics_t* metrics);

/** Contador de ciclos y timestamp monotónico. */
uint64_t ChronosRT_get_cycles(void);
uint64_t ChronosRT_get_timestamp_ns(void);
```

### 5.2 Ejemplo de Uso de la API

```c
#include <chronosrt/chronosrt.h>
#include <stdio.h>
#include <signal.h>

static volatile bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    // Configurar handler de señales
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Configuración del sistema
    ChronosRT_config_t config = {
        .event_pool_size = 65536,
        .batch_size = 256,
        .use_simd = true,
        .use_cpu_affinity = true,
        .use_hugepages = false,
        .platform = ChronosRT_PLATFORM_RPI4
    };

    // Inicializar ChronosRT Master
    ChronosRT_handle_t handle;
    ChronosRT_status_t status = ChronosRT_init(&handle, &config);
    if (status != ChronosRT_SUCCESS) {
        fprintf(stderr, "Failed to initialize: %d\n", status);
        return 1;
    }

    printf("ChronosRT Master initialized successfully\n");

    // Datos de ejemplo
    typedef struct {
        float position;
        float velocity;
        uint32_t timestamp;
    } control_data_t;

    control_data_t data = {0};
    uint64_t iteration = 0;

    // Loop principal
    while (g_running) {
        // Leer datos de sensores (simulado)
        data.position = (float)(iteration % 1000) / 10.0f;
        data.velocity = (float)((iteration % 100) / 10.0f);
        data.timestamp = (uint32_t)ChronosRT_get_timestamp_ns();

        // Publicar evento
        status = ChronosRT_event_publish(handle, &data, sizeof(data));
        if (status != ChronosRT_SUCCESS) {
            fprintf(stderr, "Publish failed: %d\n", status);
        }

        iteration++;

        // Simular control a 1 kHz.
        // En producción use ChronosRT_tick_wait() (timerfd) y SCHED_DEADLINE.
        usleep(1000);  // 1 ms = 1000 us
    }

    // Benchmark de WCET bajo demanda (no hay "get_metrics" en runtime)
    ChronosRT_metrics_t metrics;
    if (ChronosRT_benchmark_run(handle, ChronosRT_MODE_WCET, 1u, &metrics) ==
        ChronosRT_SUCCESS) {
        ChronosRT_metrics_report(&metrics);
    }

    // Apagar
    ChronosRT_shutdown(handle);
    printf("ChronosRT shut down\n");

    return 0;
}
```

### 5.3 API de planificación, memoria y tiempo real

```c
/* chronosrt_sched.h — SCHED_DEADLINE / SCHED_FIFO / afinidad / aislamiento */
bool               ChronosRT_sched_deadline_supported(void);
ChronosRT_status_t ChronosRT_sched_set_deadline_current(const ChronosRT_deadline_attr_t* attr);
ChronosRT_status_t ChronosRT_sched_set_fifo_current(int priority);
ChronosRT_status_t ChronosRT_sched_set_affinity_current(int cpu);
ChronosRT_status_t ChronosRT_sched_isolation_probe(ChronosRT_isolation_t* out);
bool               ChronosRT_sched_cpu_is_isolated(const ChronosRT_isolation_t* iso, int cpu);
void               ChronosRT_sched_isolation_print(const ChronosRT_isolation_t* iso);
int                ChronosRT_sched_online_cpus(void);

/* chronosrt_mem.h — bloqueo de memoria, hugepages y NUMA */
ChronosRT_status_t ChronosRT_mem_lock_all(void);
void               ChronosRT_mem_unlock_all(void);
size_t             ChronosRT_mem_hugepage_size(void);
int                ChronosRT_mem_current_numa_node(void);
int                ChronosRT_mem_numa_nodes(void);

/* chronosrt_rt.h — tick periódico, watchdog e histograma */
ChronosRT_status_t ChronosRT_tick_init(ChronosRT_tick_t* tick, uint64_t period_ns);
int                ChronosRT_tick_wait(ChronosRT_tick_t* tick);
void               ChronosRT_tick_destroy(ChronosRT_tick_t* tick);

ChronosRT_status_t ChronosRT_watchdog_init(ChronosRT_watchdog_t* wd, uint64_t timeout_ns);
void               ChronosRT_watchdog_kick(ChronosRT_watchdog_t* wd);
bool               ChronosRT_watchdog_expired(const ChronosRT_watchdog_t* wd);
bool               ChronosRT_watchdog_hw_available(void);

void               ChronosRT_hist_init(ChronosRT_hist_t* h, uint64_t bucket_ns);
void               ChronosRT_hist_record(ChronosRT_hist_t* h, uint64_t latency_ns);
uint64_t           ChronosRT_hist_percentile_ns(const ChronosRT_hist_t* h, double p);
uint64_t           ChronosRT_hist_wcet_ns(const ChronosRT_hist_t* h);

/* chronosrt_kernel.h — sondeo del kernel */
ChronosRT_status_t ChronosRT_kernel_probe(ChronosRT_kernel_info_t* info);
void               ChronosRT_kernel_print(const ChronosRT_kernel_info_t* info);
bool               ChronosRT_kernel_is_supported(const ChronosRT_kernel_info_t* info);
```

### 5.4 API de resiliencia y observabilidad

```c
/* chronosrt_resilience.h */
ChronosRT_status_t ChronosRT_snapshot_init(ChronosRT_snapshot_t* s, uint32_t size);
ChronosRT_status_t ChronosRT_snapshot_write(ChronosRT_snapshot_t* s, const void* data, uint32_t size);
ChronosRT_status_t ChronosRT_snapshot_read(const ChronosRT_snapshot_t* s, void* out, uint32_t size);

ChronosRT_status_t ChronosRT_wal_open(ChronosRT_wal_t* w, const char* path, uint32_t batch);
ChronosRT_status_t ChronosRT_wal_append(ChronosRT_wal_t* w, const void* rec, uint32_t len);
ChronosRT_status_t ChronosRT_wal_sync(ChronosRT_wal_t* w);
int                ChronosRT_wal_replay(const char* path,
                                        int (*cb)(const void* rec, uint32_t len, void* ud),
                                        void* ud);

ChronosRT_status_t ChronosRT_golden_save(const char* path, const void* data, uint32_t len);
ChronosRT_status_t ChronosRT_golden_load(const char* path, void* out, uint32_t cap, uint32_t* out_len);

/* chronosrt_obs.h */
void               ChronosRT_counters_init(ChronosRT_counters_t* c);
void               ChronosRT_counters_print(const ChronosRT_counters_t* c, const char* label);
ChronosRT_status_t ChronosRT_trace_init(ChronosRT_trace_t* t);
int                ChronosRT_trace_mark(ChronosRT_trace_t* t, const char* msg);
ChronosRT_status_t ChronosRT_selftest_bounded_queue(uint32_t producers, uint32_t consumers,
                                                    uint64_t total,
                                                    ChronosRT_lockfree_stats_t* out);
```

> **No existe** una API de reconfiguración en caliente (`ChronosRT_reconfigure`) ni
> de logging/callbacks globales. La configuración se fija en `ChronosRT_init`;
> para cambiar parámetros de red en caliente use `ChronosRT_et_master_set_qos`,
> `set_fec` y `set_tx_mode`.

---

## 6. Debugging y Logging

### 6.1 Observabilidad y logging

ChronosRT **no impone un sistema de logging global**. Para diagnóstico ofrece
tres mecanismos de bajo coste:

```c
#include <chronosrt/chronosrt_obs.h>

/* 1. Contadores atómicos consultables en vivo. */
ChronosRT_counters_t counters;
ChronosRT_counters_init(&counters);
ChronosRT_counters_print(&counters, "rx");
/* published, consumed, dropped, errors, forwarded */

/* 2. Traza de baja sobrecarga (trace_marker de ftrace, best-effort). */
ChronosRT_trace_t trace;
if (ChronosRT_trace_init(&trace) == ChronosRT_SUCCESS) {
    ChronosRT_trace_mark(&trace, "cycle-start");
    /* ... */
    ChronosRT_trace_destroy(&trace);
}

/* 3. Autotest de colas lock-free (pérdidas/duplicados). */
ChronosRT_lockfree_stats_t lf;
ChronosRT_selftest_bounded_queue(2u, 2u, 100000u, &lf);
```

Para mensajes de aplicación use `fprintf(stderr, ...)` o su propio logger; el
kernel ya aporta timestamps y la traza de ftrace. El detector de kernel imprime
su estado con `ChronosRT_kernel_print()` al inicializar.

### 6.2 Debugging con GDB

```bash
# Compilar con symbols de debug
cmake .. -DCMAKE_BUILD_TYPE=Debug
make

# Ejecutar con GDB
gdb ./build/chronosrt_bench

# Comandos útiles de GDB
(gdb) break ChronosRT_event_publish
(gdb) break ChronosRT_event_subscribe
(gdb) run --mode=latency --duration=10

# Ver backtrace al llegar a breakpoint
(gdb) bt

# Examinar variables
(gdb) p handle
(gdb) p *event

# Ver registros de CPU
(gdb) info registers

# Ver estado de threads
(gdb) info threads

# Cambiar a thread específico
(gdb) thread 2
```

### 6.3 Análisis con perf

```bash
# Perfilado básico
perf stat -B -e cycles,instructions,cache-references,cache-misses ./chronosrt_bench --mode=latency

# Perfilado con overhead
perf record -g ./chronosrt_bench --mode=latency --duration=30
perf report --stdio

# Flame graph
perf record -F 99 -g ./chronosrt_bench --mode=latency --duration=60
perf script | stackcollapse-perf.pl | flamegraph.pl > latency.svg

# Analizar caché
perf stat -e cache-misses,cache-references, LLC-misses ./chronosrt_bench --mode=throughput
```

---

## 7. Actualización y Mantenimiento

### 7.1 Procedimiento de Actualización

```bash
#!/bin/bash
# update_ChronosRT Master.sh

set -e

NEW_VERSION="1.1.0"
BACKUP_DIR="/var/backups/ChronosRT"
INSTALL_DIR="/opt/ChronosRT"

echo "ChronosRT Master Update Script v$NEW_VERSION"
echo "======================================="

# 1. Detener servicio actual
echo "[1/7] Stopping current service..."
sudo systemctl stop ChronosRT || true

# 2. Crear respaldo
echo "[2/7] Creating backup..."
mkdir -p "$BACKUP_DIR"
sudo cp -r "$INSTALL_DIR" "$BACKUP_DIR/ChronosRT_$(date +%Y%m%d_%H%M%S)"

# 3. Descargar nueva versión
echo "[3/7] Downloading new version..."
wget -O /tmp/ChronosRT-$NEW_VERSION.tar.gz "https://example.com/ChronosRT-$NEW_VERSION.tar.gz"

# 4. Verificar checksum
echo "[4/7] Verifying checksum..."
sha256sum /tmp/ChronosRT-$NEW_VERSION.tar.gz

# 5. Extraer e instalar
echo "[5/7] Extracting and installing..."
cd /tmp
tar -xzf ChronosRT-$NEW_VERSION.tar.gz
cd ChronosRT-$NEW_VERSION
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
sudo make install

# 6. Reiniciar servicio
echo "[6/7] Restarting service..."
sudo systemctl daemon-reload
sudo systemctl start ChronosRT

# 7. Verificar
echo "[7/7] Verifying installation..."
/opt/ChronosRT/bin/chronosrt_test

echo "Update complete!"
```

### 7.2 Script de Mantenimiento Preventivo

```bash
#!/bin/bash
# maintenance.sh - Script de mantenimiento mensual

LOG_FILE="/var/log/ChronosRT_maintenance.log"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

log "Starting monthly maintenance"

# 1. Verificar espacio en disco
log "[1/6] Checking disk space..."
df -h / | tail -1 | awk '{print "Disk usage: " $5}'

# 2. Rotar logs antiguos
log "[2/6] Rotating old logs..."
find /var/log/ChronosRT -name "*.log" -mtime +30 -exec gzip {} \;
find /var/log/ChronosRT -name "*.gz" -mtime +90 -delete

# 3. Verificar integridad de biblioteca
log "[3/6] Verifying library integrity..."
ldd /usr/local/lib/libChronosRT.so

# 4. Verificar configuración
log "[4/6] Checking configuration..."
/opt/ChronosRT/bin/chronosrt_test --validate-config

# 5. Correr tests
log "[5/6] Running tests..."
/opt/ChronosRT/bin/chronosrt_test || log "WARNING: Some tests failed"

# 6. Reporte de métricas
log "[6/6] Collecting metrics..."
/opt/ChronosRT/bin/chronosrt_bench --mode=latency --duration=30 > /var/log/ChronosRT_metrics_$(date +%Y%m%d).log

log "Maintenance complete"
```

---

## 8. Redundancia y Alta Disponibilidad

### 8.1 Arquitectura de Redundancia

```
┌───────────────────────────────────────────────────────────────────────┐
│                  ARQUITECTURA DE ALTA DISPONIBILIDAD                  │
├───────────────────────────────────────────────────────────────────────┤
│                                                                       │
│   ┌─────────────┐          ┌─────────────┐          ┌─────────────┐   │
│   │   Master    │          │   Master    │          │   Network   │   │
│   │  Primary    │◀────────▶│  Secondary  │          │   Switch    │   │
│   │ (Active)    │   HA     │ (Standby)   │          │  (Redundant)│   │
│   └──────┬──────┘   Link   └──────┬──────┘          └──────┬──────┘   │
│          │                        │                        │          │
│          ▼                        ▼                        ▼          │
│   ┌─────────────┐          ┌─────────────┐          ┌─────────────┐   │
│   │  EtherCAT   │          │  EtherCAT   │          │   Slaves    │   │
│   │   Network   │          │   Network   │          │  (EtherCAT  │   │
│   │   (Ring)    │          │   (Ring)    │          │   Devices)  │   │
│   └─────────────┘          └─────────────┘          └─────────────┘   │
│                                                                       │
│  Failover Sequence:                                                   │
│  ─────────────────                                                    │
│  1. Primary detects failure (timeout/heartbeat)                       │
│  2. Primary sends FAILOVER command to Secondary                       │
│  3. Secondary takes over network interfaces                           │
│  4. Failover time: < 100ms (typical)                                  │
└───────────────────────────────────────────────────────────────────────┘
```

### 8.2 Implementación de Redundancia

```c
// Estado del sistema HA
typedef enum {
    HA_STATE_INIT,
    HA_STATE_STANDBY,
    HA_STATE_ACTIVE,
    HA_STATE_FAILING_OVER,
    HA_STATE_FAILED
} ha_state_t;

// Estructura HA
typedef struct {
    ha_state_t state;
    uint8_t    priority;          // 0-255, mayor = preferido
    uint32_t   heartbeat_interval; // ms
    uint32_t   heartbeat_timeout;  // ms
    uint64_t   last_heartbeat_ns;
    int        socket_fd;
    struct     sockaddr_in peer_addr;
} ha_context_t;

// Hilo de heartbeat
void* ha_heartbeat_thread(void* arg) {
    ha_context_t* ctx = (ha_context_t*)arg;
    uint8_t msg[HA_MSG_SIZE];

    while (ctx->state != HA_STATE_FAILED) {
        // Enviar heartbeat
        msg[0] = HA_MSG_HEARTBEAT;
        msg[1] = ctx->priority;
        *(uint64_t*)(msg + 2) = ChronosRT_get_timestamp_ns();

        sendto(ctx->socket_fd, msg, sizeof(msg), 0,
               (struct sockaddr*)&ctx->peer_addr, sizeof(ctx->peer_addr));

        usleep(ctx->heartbeat_interval * 1000);
    }

    return NULL;
}

// Manejo de failover
void handle_failover(ha_context_t* ctx) {
    log_warning("Initiating failover");
    ctx->state = HA_STATE_ACTIVE;
    log_info("Failover complete, now active");
}
```

---

## 9. Análisis de Rendimiento y Optimización

### 9.1 Análisis WCET

ChronosRT **no publica una suma teórica de WCET**: la cota útil es la que se
mide en el hardware y kernel reales. Para ello usa el histograma de
`chronosrt_rt.h`:

```c
#include <chronosrt/chronosrt_rt.h>

ChronosRT_hist_t hist;
ChronosRT_hist_init(&hist, 100);            /* buckets de 100 ns */

while (running) {
    uint64_t t0 = ChronosRT_get_timestamp_ns();
    /* ... trabajo del ciclo ... */
    uint64_t t1 = ChronosRT_get_timestamp_ns();
    ChronosRT_hist_record(&hist, t1 - t0);
}

uint64_t wcet   = ChronosRT_hist_wcet_ns(&hist);
uint64_t p999   = ChronosRT_hist_percentile_ns(&hist, 99.9);
uint64_t sigma  = ChronosRT_hist_stddev_ns(&hist);
uint64_t mean   = ChronosRT_hist_mean_ns(&hist);

ChronosRT_hist_print(&hist, "ciclo de control");
```

`ChronosRT_hist_print()` publica media, desviación, percentiles y WCET de la
ventana medida. Repita la medición en cada plataforma: el resultado depende de
PREEMPT_RT, `isolcpus`, *governor*, C-states y de la NIC.

### 9.2 Optimización de Cache

```c
// Análisis de cache misses
typedef struct {
    uint64_t l1_misses;
    uint64_t l2_misses;
    uint64_t l3_misses;
    uint64_t total_accesses;
} cache_stats_t;

// Optimización: Prefetch de datos
void optimized_process_batch(event_batch_t* batch) {
    for (int i = 0; i < batch->count; i++) {
        process_event(&batch->events[i]);

        if (i + 1 < batch->count) {
            __builtin_prefetch(&batch->events[i + 1], 0, 3);
        }

        if (batch->events[i].data_ptr) {
            __builtin_prefetch(batch->events[i].data_ptr, 0, 0);
        }
    }
}
```

---

## 10. Seguridad y Cifrado

### 10.1 Alcance real

ChronosRT **no implementa cifrado, autenticación ni control de acceso**. Es una
biblioteca de transporte y tiempo real; la seguridad se delega en el despliegue:

| Capa | Responsabilidad | Herramienta |
|---|---|---|
| Aislamiento físico/lógico | Red exclusiva para EtherCAT/EtherTux | VLAN, switch dedicado, `macvlan` |
| Permisos | Solo el proceso RT abre `AF_PACKET` | `CAP_NET_RAW`, usuario dedicado |
| Filtrado | Listas de MAC/ puertos en el switch | Configuración del switch |
| Integridad | CRC-32C por trama (EtherTux) y WKC (EtherCAT) | Ya implementado, **no es criptográfico** |
| Confidencialidad | Opcional en el plano de gestión | Motor criptográfico del SoC, **fuera del camino crítico** |

> El CRC-32C detecta corrupción accidental, **no** autentica ni cifra. No use
> EtherTux sobre una red no confiable sin una capa externa (IPsec, MACsec o
> aislamiento físico).

### 10.2 Motores criptográficos del SoC

La HAL **detecta** `armv8_crypto` (AES/SHA) y reporta el motor del SoC, pero no
lo usa en el datapath. Los motores dedicados (`sun8i-ce`, `rk_crypto`) solo se
contemplan para cifrado opcional del plano de gestión; `rk2_crypto` aún está en
parcheo y no debe usarse en producción.

### 10.3 Recomendaciones

1. Red física o VLAN exclusiva para el bus de control.
2. Ejecutar el proceso RT con `CAP_NET_RAW` y sin privilegios adicionales.
3. Fijar MACs de esclavas/nodos y revisar `frames_lost`, WKC y ET-WKC.
4. Si se requiere confidencialidad, aplicarla **fuera** del ciclo crítico
   (gateway, IPsec/MACsec o cifrado a nivel de aplicación en P4/P5).

---

## 11. Maestro EtherCAT de ChronosRT

### 11.1 Visión general

ChronosRT Master incorpora un **maestro EtherCAT autónomo** implementado íntegramente en C11
sobre Ethernet crudo (`AF_PACKET`, EtherType **0x88A4**), conforme a IEC 61158 / ETG.1000.

Es uno de los **dos backends de red** del sistema (el otro es EtherTux, apartado 13). No existe
transporte RAW ni conmutación de protocolo en caliente: el backend se elige al configurar.
Cualquier aplicación puede enumerar las interfaces del sistema, abrir una de ellas como maestro
EtherCAT y comunicarse con las esclavas mediante la API pública `chronosrt_ethercat.h`.

Diseño guiado por los cuatro pilares del proyecto:

| Pilar | Cómo se materializa |
|---|---|
| **Tiempo real** | cero asignaciones dinámicas en el camino caliente: tramas y mapa de proceso (`IOMap`) pre-asignados; el ciclo de proceso no reserva memoria ni bloquea. |
| **Determinismo** | temporización con `CLOCK_MONOTONIC`, *timeouts* acotados y `poll()` con vencimiento absoluto; no hay esperas indefinidas. |
| **Resiliencia absoluta** | validación por contador de trabajo (WKC), detección de esclavas perdidas, contadores de error del ESC, reintentos acotados, ACK de errores AL y redundancia de red (doble NIC). |
| **Ultra rendimiento** | varios datagramas por trama (bit **M**), `PACKET_QDISC_BYPASS`, prioridad de socket elevada y cero copias superfluas. |

> **Sin dependencias externas.** A diferencia de SOEM o del maestro IgH, este maestro no enlaza
> ninguna biblioteca de terceros: habla directamente con la NIC a través del socket de paquetes
> del kernel y serializa el protocolo EtherCAT en espacio de usuario.

### 11.2 Arquitectura modular

El maestro está dividido en seis módulos con responsabilidad única
(`src/protocols/ethercat/`), más una cabecera interna compartida:

| Módulo | Responsabilidad |
|---|---|
| [`ec_internal.h`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_internal.h) | Geometría de trama, registros ESC, *structs* internas y prototipos compartidos. |
| [`ec_frame.c`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_frame.c) | Serialización/parseo puro de datagramas y tramas (testeable sin hardware). |
| [`ec_socket.c`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_socket.c) | Socket `AF_PACKET` de tiempo real, redundancia doble NIC y enumeración de interfaces. |
| [`ec_master.c`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_master.c) | Ciclo de vida, intercambio de datagramas y ciclo de datos de proceso (PDO). |
| [`ec_slave.c`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_slave.c) | Escaneo del bus, SII/EEPROM, Sync Managers, FMMU y máquina de estados AL. |
| [`ec_dc.c`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_dc.c) | Relojes distribuidos (Distributed Clocks): SYSTIME, retardos, SYNC0/SYNC1. |
| [`ec_mailbox.c`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_mailbox.c) | Buzón (SM0/SM1) y CoE: lectura/escritura de objetos SDO (expedita y segmentada). |

### 11.3 Capa de red (socket de tiempo real)

Apertura del socket en [`ec_socket.c`](file:///mnt/netac/MI_MUNDO/PROYECTOS/KLIPPER/PROYECTOS/ChronosRT_MASTER/src/protocols/ethercat/ec_socket.c):

```c
int fd = socket(AF_PACKET, SOCK_RAW, htons(ChronosRT_EC_ETHERTYPE)); /* 0x88A4 */
/* bind al ifindex (sin dirección IP) … */
```

Opciones aplicadas para latencia y determinismo:

| Opción | Efecto |
|---|---|
| `PACKET_MR_PROMISC` | Recibe todas las tramas, incluidas las destinadas a esclavas. |
| `PACKET_MR_MULTICAST` | Suscripción a direcciones de grupo (EtherCAT las usa). |
| `PACKET_QDISC_BYPASS` | Elude la disciplina de colas del kernel (menos latencia y *jitter*). |
| `SO_PRIORITY = 6` | Marca de prioridad elevada en la pila de red. |

- **Recepción:** `poll()` sobre el *fd* con **vencimiento monotónico**; se filtran las tramas por
  EtherType `0x88A4` y se descartan las ajenas.
- **Redundancia de red (doble NIC):** `ChronosRT_ec_master_open_redundant()` abre una interfaz
  primaria y otra secundaria; cada trama se **transmite por ambas** y se acepta la primera
  respuesta válida (patrón de resiliencia de EtherCAT).
- **Enumeración de interfaces:** `getifaddrs(AF_PACKET)` alimenta
  `ChronosRT_ec_interface_count/interface/interface_index`, para que la aplicación elija la NIC.

**Requisitos de despliegue:** la apertura requiere privilegio **`CAP_NET_RAW`** (o `root`) y una
interfaz de red activa. Son las únicas dependencias de ejecución del maestro.

### 11.4 Protocolo EtherCAT

**Direccionamiento** (`address = ADP | (ADO << 16)`):

- **Posición (auto-incremento)** — para el escaneo del anillo (`APRD/APWR`).
- **Dirección de estación configurada** — para `FPRD/FPWR` tras `config_init`.
- **Dirección lógica** — para los datos de proceso (`LRD/LWR/LRW`).

**Comandos** (`ChronosRT_ec_cmd_t`): `NOP`, `APRD/APWR/APRW`, `FPRD/FPWR/FPRW`, `BRD/BWR/BRW`,
`LRD/LWR/LRW` y `ARMW/FRMW` (lectura + escritura múltiple, usados por DC).

**Contador de trabajo (WKC):** cada datagrama devuelve cuántas esclavas lo procesaron. El maestro
valida la respuesta contra un **WKC esperado** calculado al configurar el mapa de proceso:

```
WKC esperado = 2 × (esclavas con salidas) + 1 × (esclavas con entradas)
```

Una respuesta con `WKC < esperado` indica esclavas ausentes o tramas perdidas en ese ciclo.

**Varios datagramas por trama:** el **bit M** (`0x8000` del campo de longitud) encadena datagramas
en una sola trama Ethernet, reduciendo el número de envíos por ciclo.

### 11.5 Escaneo y configuración de esclavas

Secuencia estándar de puesta en marcha:

1. **`ChronosRT_ec_master_scan()`** — recorre el anillo por auto-incremento leyendo
   `STATION_ADDR`/`ALIAS` y `DL_STATUS`; se detiene en la primera posición que no responde.
2. **`ChronosRT_ec_master_config_init()`** — para cada esclava: asigna la dirección de estación
   (`posición + ChronosRT_EC_NODE_OFFSET`), lee `ESCSUP` (soporte DC), `TYPE`, la **SII/EEPROM**
   (fabricante, producto, revisión, serie, buzón) y los **Sync Managers** SM0…SM7.
3. **`ChronosRT_ec_master_config_map()`** — en dos pasadas (salidas SM2 → entradas SM3) calcula
   los *offsets* del **IOMap**, programa las **FMMU** (FMMU0 = escritura de salidas, FMMU1 =
   lectura de entradas) y configura SM2/SM3; finalmente fija el `expected_wkc`.

Registros ESC empleados (`ChronosRT_EC_REG_*`): `TYPE 0x0000`, `STATION_ADDR 0x0010`,
`ALIAS 0x0012`, `DL_STATUS 0x0110`, `AL_CONTROL 0x0120`, `AL_STATUS 0x0130`,
`AL_STATUS_CODE 0x0134`, `RX_ERROR 0x0300`, `CRC_ERROR 0x0308`, `WD_COUNTER 0x0442`,
FMMU `0x0600`, SM `0x0800`, `DC_* 0x0900–0x09A4`.

**SII/EEPROM:** lectura vía `EEPADDR`/`EEPCTL`/`EEPSTAT`/`EEPDAT` con sondeo del bit *BUSY*,
según el procedimiento ETG.1000.

### 11.6 Máquina de estados AL (ESM)

Estados: `INIT` → `PREOP` → `SAFEOP` → `OP` (y `BOOT`); el bit de **ACK de error** (`0x10`) se
OR-ea para confirmar y limpiar errores.

- **`ChronosRT_ec_master_request_state()`** — escribe el registro de control AL, espera la
  transición (sondeo con *timeout*) y emite el ACK cuando la esclava reporta error.
- **`ChronosRT_ec_master_request_all_states()`** — lleva **todas** las esclavas al estado pedido
  con un `BWR` de difusión al `AL_CONTROL` y espera; **degrada con elegancia** contando las
  esclavas que sí alcanzan el estado en lugar de abortar.

### 11.7 Distributed Clocks (DC)

- **`ChronosRT_ec_master_config_dc()`** — lee `SYSTIME` de la primera esclava con DC, mide los
  retardos de propagación (`DCTIME0–3`), propaga el tiempo de referencia al bus y compensa el
  *offset* de reloj.
- **`ChronosRT_ec_master_sync0()` / `sync01()`** — programan `DCCYCLE0/1` y `DCSTART0/1` y activan
  las señales **SYNC0/SYNC1** para sincronizar el ciclo de proceso con las esclavas.
- **`ChronosRT_ec_master_dc_time()`** — devuelve el último tiempo de sistema DC leído (ns).

### 11.8 Buzón y CoE (SDO)

- **`ChronosRT_ec_master_sdo_read()`** (upload) y **`ChronosRT_ec_master_sdo_write()`** (download)
  acceden a objetos CoE por `index`/`subindex` sobre el buzón SM0/SM1.
- Soporta transferencia **expedita** (≤ 4 B) y **segmentada** (con bit *toggle*), codificando la
  cabecera de buzón (6 B) y la de CoE/SDO según ETG.1000.
- La espera de hueco en el buzón (`SM_STATUS` libre/lleno) es acotada por `EC_TIMEOUT_MBX`.

### 11.9 Ciclo de datos de proceso (PDO)

```c
/* Enviar las salidas (RxPDO) del IOMap y recibir las entradas (TxPDO) */
ChronosRT_ec_master_send_processdata(master);
ChronosRT_ec_master_receive_processdata(master, timeout_us);

const uint8_t* io = ChronosRT_ec_master_iomap(master);
/* io[o_offset .. o_offset+o_bytes) → salidas de cada esclava
   io[i_offset .. i_offset+i_bytes) → entradas de cada esclava          */
```

`send_processdata()` emite un **LRW** con el IOMap; `receive_processdata()` valida el **WKC** y
copia las entradas. Ambos son aptos para un bucle periódico de tiempo real.

### 11.10 API pública

La API (`chronosrt_ethercat.h`) se agrupa en:

| Grupo | Funciones |
|---|---|
| **Interfaces** | `interface_count`, `interface`, `interface_index` |
| **Helpers puros** | `datagram_build`, `datagram_parse`, `frame_header` |
| **Ciclo de vida** | `master_create`, `master_open`, `master_open_redundant`, `master_close`, `master_destroy`, `master_is_open`, `master_interface`, `master_set_timeout` |
| **Escaneo/config** | `master_scan`, `master_config_init`, `master_config_map`, `master_slave_count`, `master_slave`, `master_iomap`, `master_iomap_size` |
| **Registros** | `master_read_reg`, `master_write_reg`, `master_fp_read`, `master_fp_write`, `master_broadcast_read`, `master_broadcast_write` |
| **Lógicos** | `master_lrw`, `master_lrd`, `master_lwr`, `master_armw`, `master_frmw` |
| **Estados AL** | `master_read_state`, `master_request_state`, `master_statecheck`, `master_write_state`, `master_request_all_states` |
| **Relojes (DC)** | `master_config_dc`, `master_sync0`, `master_sync01`, `master_dc_time` |
| **PDO** | `master_send_processdata`, `master_receive_processdata` |
| **CoE/SDO** | `master_sdo_read`, `master_sdo_write` |
| **Diagnóstico** | `master_working_counter`, `master_expected_wkc`, `master_frames_sent`, `master_frames_lost` |
| **Crudo** | `master_send`, `master_recv` |

### 11.11 Ejemplo de uso

```c
#include "chronosrt_ethercat.h"

ChronosRT_ec_master_t* m = ChronosRT_ec_master_create();

/* 1. Abrir la NIC como maestro EtherCAT */
if (ChronosRT_ec_master_open(m, "enp2s0") != ChronosRT_SUCCESS) {
    /* degrabación elegante: sin CAP_NET_RAW o interfaz ausente */
    ChronosRT_ec_master_destroy(m);
    return -1;
}

/* 2. Descubrir y configurar el bus */
int n = ChronosRT_ec_master_scan(m);
ChronosRT_ec_master_config_init(m);
ChronosRT_ec_master_config_map(m);

/* 3. Llevar el bus a OP (degradación elegante si falta alguna esclava) */
ChronosRT_ec_master_request_all_states(m, ChronosRT_EC_STATE_OP, 50000);

/* 4. Ciclo de proceso en tiempo real */
const uint8_t* io = ChronosRT_ec_master_iomap(m);
while (running) {
    ChronosRT_ec_master_send_processdata(m);
    if (ChronosRT_ec_master_receive_processdata(m, 2000) != 0) {
        /* WKC insuficiente: esclava perdida o trama perdida */
    }
    /* leer entradas / escribir salidas en `io` */
}

ChronosRT_ec_master_close(m);
ChronosRT_ec_master_destroy(m);
```

### 11.12 Propiedades de ingeniería

- **Tiempo real:** buffers de trama, `IOMap` y estructuras de esclava se reservan una vez en
  `create`/`config_map`; el camino caliente (`send_processdata`/`receive_processdata`) no asigna.
- **Determinismo:** todos los *timeouts* se miden con `CLOCK_MONOTONIC`; `poll()` usa vencimiento
  absoluto, de modo que no hay *busy-wait* ni esperas sin cota.
- **Resiliencia:** validación de WKC por ciclo, marca `is_lost` por esclava, contadores de error del
  ESC, ACK de errores AL, reintentos acotados y redundancia de doble NIC.
- **Ultra rendimiento:** bit **M** para encadenar datagramas, `PACKET_QDISC_BYPASS` y prioridad de
  socket, con cero copias innecesarias entre trama y IOMap.
- **Degradación elegante:** cualquier operación sobre un maestro cerrado, una interfaz inexistente o
  la ausencia de `CAP_NET_RAW` devuelve un código de error (`ChronosRT_ERROR_INVALID`,
  `ChronosRT_ERROR_IO`, `ChronosRT_ERROR_NO_HARDWARE`) **sin provocar un *crash***.

### 11.13 Diagnóstico

`ChronosRT_ec_master_working_counter()` (WKC del último ciclo), `expected_wkc()`,
`frames_sent()` y `frames_lost()` permiten al operador detectar degradación del enlace o esclavas
caídas sin instrumentación externa.

## 12. Librerías y Dependencias

### 12.1 Librerías del Sistema Requeridas

ChronosRT Master depende de las siguientes librerías del sistema para su funcionamiento óptimo:

| Librería | Versión Mínima | Propósito | Impacto en Rendimiento |
|----------|----------------|-----------|------------------------|
| `glibc` | 2.31 | Funciones base de C, `mmap`, `mbind`, `sched_*` | Crítico |
| `libpthread` | 2.31 | Threading POSIX | Crítico |
| `libm` | 2.31 | `sqrt`/estadística del benchmark | Bajo |
| `libdl` | 2.31 | `dlopen` del puente con proveedores externos | Bajo |

No se enlaza `libnuma` ni `libhugetlbfs`: NUMA y hugepages se implementan con
`syscall(SYS_mbind)`, `mmap(MAP_HUGETLB)` y `madvise(MADV_HUGEPAGE)`, de modo que
la biblioteca no añade dependencias opcionales.

### 12.2 Dependencias de ejecución del maestro EtherCAT

El maestro EtherCAT es **autónomo**: no enlaza SOEM, ni el maestro IgH, ni DPDK, ni *frameworks* de
captura acelerada (`netmap`, `PF_RING`, `ONVM`). Sus únicas dependencias de ejecución son:

| Dependencia | Tipo | Propósito |
|---|---|---|
| `glibc` (`AF_PACKET`, `poll`) | Sistema | Acceso a la NIC y temporización |
| `CAP_NET_RAW` (o `root`) | Privilegio | Abrir el socket de paquetes EtherCAT |
| Interfaz de red activa | Hardware | Medio físico (p. ej. `enp2s0`) |

Esta decisión reduce la superficie de fallo y el consumo de CPU asociados a los *frameworks*
externos, y mantiene el determinismo al no introducir hilos de sondeo ajenos al maestro.

### 12.3 Coste de la ruta de red

La ruta de red del maestro es un socket `AF_PACKET` con `PACKET_QDISC_BYPASS` y prioridad de socket
elevada, sin copias intermedias entre la trama y el IOMap. EtherTux añade
`PACKET_TX_RING` (ranuras mapeadas en espacio de usuario y un único `send()` por
lote) y `SO_TXTIME` para *pacing* por hardware cuando la NIC lo soporta. No se
emplea ninguna ruta de *bypass* del kernel (DPDK/AF_XDP): el objetivo es el
**determinismo**, no el máximo *throughput* de propósito general.

---

## 13. EtherTux y HAL de aceleración por hardware

### 13.1 Visión general y selección de protocolo

**EtherTux** es el tejido de comunicación determinista de ChronosRT para **Ethernet comercial**
(NICs y switches estándar, SBC heterogéneas y MCUs con controlador Ethernet SPI/QSPI). Convive
con el maestro EtherCAT; la elección es de **tiempo de configuración** y nunca simultánea sobre
la misma interfaz.

| Protocolo | Cuándo usarlo | Determinismo típico |
|---|---|---|
| **EtherCAT** (`0x88A4`) | Esclavas con ASIC/FPGA dedicado; ciclo < 100 µs | Jitter < 1 µs |
| **EtherTux** (`0x88B5`) | NIC/switch comercial; SBC heterogéneas; MCU SPI/QSPI | Jitter < 1 µs (SBC con PHC); < 50 µs (MCU W6300) |

La selección se resuelve con `chronosrt_protocol.h`:

```c
ChronosRT_protocol_t p = ChronosRT_protocol_from_env();   /* CHRONOSRT_PROTOCOL */
/* o explícita: ChronosRT_protocol_from_string("ethertux") */
```

### 13.2 Formato de trama y CRC-32C

Ethernet II + EtherType **0x88B5** + cabecera EtherTux de **26 bytes**:

| Campo | Bytes | Descripción |
|---|---|---|
| `Type` | 1 | `0x01` Datos, `0x02` FEC, `0x03` Control, `0x04` Sync, `0x05` LAPM, `0x06` Discovery, `0x07` SyncCalibrated, `0x08` StateChange, `0x09` MailboxData, `0x0A` MailboxAck, `0x0B` SyncReport, `0x0C` TopoCheck |
| `Priority` | 1 | Clase QoS P0–P5 |
| `Flags` | 1 | bit0 FEC, bit1 preemptable, bit2 express, bit3 discovery, bit4 FRM |
| `State` | 1 | Estado ET-ESM del emisor (`INIT/CONFIG/SAFE/OP/FAULT`) |
| `FlowID` | 1 | 1–255 |
| `SeqNum` | 4 | Secuencia por flujo |
| `TxTimestamp` | 8 | ns del reloj monotónico |
| `ET-WKC` | 2 | Working Counter: nodos que procesaron la trama |
| `BlockID` | 2 | `(bloque << 8) \| índice dentro del bloque` |
| `FlowCount` | 1 | Flujos agrupados (`1` = sin agregación) |
| `CRC32` | 4 | CRC-32C de cabecera (con este campo a cero) + carga útil |
| **Total** | **26** | |

> **Corrección de erratum:** la especificación rotula la cabecera como "28 bytes",
> pero la suma de sus campos es **26**. Se implementa la tabla, que es la fuente
> de verdad (igual que se resolvió antes el erratum 20/22). El valor
> `ChronosRT_ET_HEADER_SIZE` es **26**.


**CRC-32C (Castagnoli) y no CRC-32 (IEEE)** porque es el único polinomio con aceleración por
instrucción en **ambas** arquitecturas: `crc32cb/c/h/w/x` en ARMv8.1+ y `crc32` de SSE4.2 en
x86-64. Así todos los nodos calculan el mismo valor con el menor coste posible.

### 13.3 QoS jerárquico: token buckets, backpressure y LAPM

| Nivel | Clase | Política | Al agotar su cuota |
|---|---|---|---|
| **P0** | Express | Ranura fija; preempción absoluta; trama 64 B | **Backpressure** (nunca se descarta) |
| **P1** | Express | Ranura fija (sync, heartbeat, LAPM) | Degrada a **P2** |
| **P2** | Time-triggered | Ventana dedicada (setpoints) | Degrada a **P4** |
| **P3** | Rate-constrained | Token bucket (telemetría) | Degrada a **P4** |
| **P4** | Rate-constrained | Token bucket menor (configuración) | Degrada a **P5** |
| **P5** | Best-effort | Solo ventana BE (logs, firmware) | **Descartada** (drop-tail) |

- **Reparto por defecto** para 1 Gbps sin sobre-suscripción: 5 + 5 + 50 + 30 + 20 + 15 = 125 MB/s.
- **Backpressure consciente de prioridad**: si `occupancy(P0) > 80 %` se pausa la generación de
  P2–P5; si `occupancy(P1) > 90 %`, se pausa P3–P5; si `occupancy(P2) > 95 %`, P4–P5.
- **LAPM**: por encima del 80 % de ocupación de una clase sus flujos migran a la inferior; no
  vuelven hasta bajar del 60 % (histéresis). La prioridad efectiva se propaga en el campo
  `Priority` de la trama.

API: `ChronosRT_et_qos_create/configure/admit/backpressure/effective_priority`.

### 13.4 Planificador time-triggered

Superframe de 1 ms dividido en cuatro ventanas (configurables, deben cubrir el ciclo):

| Ventana | Duración | Prioridades |
|---|---|---|
| Control | 100 µs | P0, P1 |
| Determinista | 200 µs | P2 |
| Rate-constrained | 300 µs | P3, P4 |
| Best-effort | 400 µs | P5 |

P0 es **express**: puede transmitir en cualquier ventana. El resto solo en la suya. Si la NIC no
soporta preempción por hardware, el planificador no encola tráfico de baja prioridad cuando hay
una P0 pendiente, acotando el bloqueo al tiempo de transmisión de la trama en curso
(`ChronosRT_et_tx_time_ns`, p. ej. 12,3 µs para 1518 B a 1 Gbps).

Cuando un nodo cae, `ChronosRT_et_sched_remove_node()` libera sus ranuras.

### 13.5 FEC Reed-Solomon sin retransmisión

| Modo | Código | Overhead | Pérdidas recuperables |
|---|---|---|---|
| **Ligero** | RS(10,8) | 25 % | 2 por bloque |
| **Robusto** | RS(16,8) | 100 % | 8 por bloque |

Código **sistemático** sobre GF(2^8) (polinomio 0x11D) construido con matriz de Vandermonde
(`G = Vtop⁻¹·V`). La codificación es una combinación lineal con coeficientes **constantes**, lo
que permite vectorizar la multiplicación en GF(2^8) con tablas de nibble + `shuffle`. La
decodificación es de **borrados** (se conoce qué paquetes faltan) por eliminación gaussiana: sin
RTT y sin retransmisión.

API: `ChronosRT_et_fec_create_mode`, `…_encode`, `…_decode`, y `ChronosRT_et_master_send_fec_block`
(transmite K datos + N-K paridad con el `BlockID` correcto).

### 13.6 HAL de aceleración (`chronosrt_hwaccel.h`)

Sondeo en tiempo de ejecución, sin `#if` de compilación en el código de aplicación:

| Capacidad | Detección | Fallback |
|---|---|---|
| CRC-32C | `HWCAP_CRC32` (ARMv8.1+) / `__builtin_cpu_supports("sse4.2")` | slice-by-8 |
| FEC SIMD | NEON (obligatorio en AArch64) / AVX2 | escalar con tablas |
| Crypto ARMv8 | `HWCAP_AES` | — (solo plano de gestión) |
| PHC / Launch Time | `ETHTOOL_GET_TS_INFO` (`SOF_TIMESTAMPING_TX_HARDWARE`) | envío inmediato |

```c
const ChronosRT_hw_caps_t* caps = ChronosRT_hw_caps();
/* Ejemplo en un AMD Zen2: crc32c_backend="sse4.2-x4", fec_backend="avx2" */
```

`ChronosRT_crc32c`, `ChronosRT_gf8_mul/inv`, `ChronosRT_rs_encode/decode` y
`ChronosRT_tx_timestamp_ns` son la superficie pública de la HAL.

Única SBC objetivo sin CRC32 por instrucción: **Banana Pi M2 Zero (Cortex-A7)**, que usa el
fallback software sin perder funcionalidad.

### 13.7 Arquitectura de módulos

Organización del árbol (`src/` por subsistema; `include/chronosrt/` mantiene una superficie
pública plana y estable):

| Módulo | Responsabilidad |
|---|---|
| `src/protocols/ethertux/et_frame.c` | Serialización/parseo puro de la PDU y CRC-32C |
| `src/protocols/ethertux/et_fec.c` | Bloques FEC sobre la HAL de Reed-Solomon |
| `src/protocols/ethertux/et_qos.c` | Token buckets jerárquicos, backpressure y LAPM |
| `src/protocols/ethertux/et_sched.c` | Planificador time-triggered y ranuras |
| `src/protocols/ethertux/et_esm.c` | Máquina de estados ET-ESM del nodo |
| `src/protocols/ethertux/et_sync.c` | ET-Sync y monitor de ventana de sincronización |
| `src/protocols/ethertux/et_topology.c` | Registro de nodos, ET-WKC, descubrimiento y Hot Connect |
| `src/protocols/ethertux/et_mbx.c` | Mailbox ET-MBX (8 canales, segmentos de 512 B) |
| `src/protocols/ethertux/et_flowagg.c` | Flow Aggregator (`FlowCount > 1`) |
| `src/protocols/ethertux/et_frm.c` | Redundancia lógica de flujo y deduplicación |
| `src/protocols/ethertux/et_socket.c` | `AF_PACKET`/0x88B5, `SO_TXTIME`, `PACKET_TX_RING`, recepción con *deadline* |
| `src/protocols/ethertux/et_master.c` | Ciclo de vida, envío/recepción, nodos y heartbeat |
| `src/hwaccel/chronosrt_hwaccel.c` | CRC-32C HW, GF(2^8) y RS vectorizado |
| `src/protocols/chronosrt_protocol.c` | Selección EtherCAT / EtherTux |

### 13.8 Decisiones de implementación

1. **Transporte `AF_PACKET`**, no AF_XDP: AF_XDP exige soporte del driver y libbpf, no está
   disponible en todas las SBC objetivo y no puede validarse sin hardware. El datapath queda
   aislado en `et_socket.c` para añadir un backend AF_XDP sin tocar el protocolo. Con
   `PACKET_QDISC_BYPASS` + `SO_PRIORITY` + `SO_TXTIME` + `PACKET_TX_RING` se conservan las
   propiedades críticas y se elimina la llamada al sistema por trama.
2. **CRC-32C** (no CRC-32 IEEE) por aceleración universal, ver §13.2.
3. **Cabecera de 26 B** (la tabla de la especificación suma 26; ver §13.2).
4. **Decodificación de borrados** en lugar de corrección de errores: el receptor conoce qué
   paquetes faltan; es más simple, determinista y suficiente para pérdidas de paquete.
5. Los motores criptográficos del SoC **nunca** se usan en el camino crítico.

### 13.9 Ejemplo de uso

```c
#include <chronosrt/chronosrt_ethertux.h>

ChronosRT_et_master_t* m = ChronosRT_et_master_create();
if (ChronosRT_et_master_open_ex(m, "enp2s0", 1000000000ull, 1000000u) != ChronosRT_SUCCESS) {
    /* sin CAP_NET_RAW o interfaz ausente: degradación elegante */
}

ChronosRT_et_master_set_fec(m, ChronosRT_ET_FEC_LIGHT);

ChronosRT_et_sched_t* s = ChronosRT_et_sched_create(1000000u, 1000000000ull);
ChronosRT_et_sched_add_slot(s, ChronosRT_ET_P2, 3u, 128u, 120000u);
ChronosRT_et_master_attach_scheduler(m, s);

uint8_t setpoint[16] = { 0 };
ChronosRT_status_t st = ChronosRT_et_master_send(m, ChronosRT_ET_P2,
                                                 ChronosRT_ET_TYPE_DATA, 1u,
                                                 setpoint, sizeof(setpoint));
if (st == ChronosRT_ERROR_BACKPRESSURE) {
    /* el productor debe reducir la tasa: no se pierde la trama */
}

ChronosRT_et_header_t hdr;
uint8_t payload[ChronosRT_ET_MAX_PAYLOAD];
int n = ChronosRT_et_master_recv(m, &hdr, payload, sizeof(payload), 1000u);

ChronosRT_et_master_destroy(m);   /* cierra el socket y libera QoS/FEC */
ChronosRT_et_sched_destroy(s);
```

### 13.10 Verificación

`chronosrt_test_hwaccel` (**40 aserciones**), `chronosrt_test_ethertux`
(**103**) y `chronosrt_test_ethertux_v010` (**152**) cubren CRC-32C contra el
vector estándar y contra el fallback software, álgebra de GF(2^8),
codificación/decodificación RS con borrados (incluidos todos los datos perdidos),
QoS (degradación, backpressure, LAPM con histéresis), ventanas del planificador,
ciclo de vida del maestro, ET-ESM, ET-Sync y monitor de ventana, ET-WKC y
topología, ET-MBX, Flow Aggregator, FRM y degradación elegante sin `CAP_NET_RAW`.

Estado global: **9 suites / 485 aserciones**, build `Release` sin errores ni
advertencias.

---

**Fin del Manual Técnico**

---

## Apéndice A: Referencia Completa de la API

### A.1 Estructuras de Datos Principales

#### ChronosRT_event_t (64 bytes - Cache Line Aligned)

```c
typedef struct ChronosRT_CACHE_ALIGNED {
    volatile uint64_t sequence;       // Offset 0-7:   Número de secuencia atómico
    volatile uint64_t timestamp_ns;   // Offset 8-15:  Timestamp nanosegundos creación
    volatile uint32_t cpu_id;         // Offset 16-19: CPU que originó el evento
    volatile uint8_t  data[56];       // Offset 20-63: Payload de datos (56 bytes)
} ChronosRT_event_t;
```

**Consideraciones de diseño:**
- Tamaño exacto de 64 bytes para alineación con cache line
- Campos volátiles para garantizar visibilidad entre threads
- Sin padding implícito - estructura compacta optimizada

#### ChronosRT_config_t

```c
typedef struct {
    uint32_t enabled_cpus[ChronosRT_MAX_CPUS];  // CPUs habilitadas (máx 16)
    uint32_t num_cpus;          // Número de CPUs activas
    uint32_t event_pool_size;   // Tamaño pool eventos (default: 65536)
    uint32_t batch_size;        // Tamaño de batch procesamiento (default: 256)
    uint32_t stack_size_kb;     // Stack por thread KB (default: 256)
    bool     use_hugepages;     // Usar hugepages de 2MB
    bool     use_numa_aware;    // Asignación NUMA-aware
    bool     use_simd;          // Optimizaciones SIMD (AVX/NEON)
    bool     use_cpu_affinity;  // Fijar threads a CPUs específicas
    ChronosRT_platform_t platform;  // Plataforma detectada

    /* Extensiones ABI v1 (F2–F7) */
    bool     lock_memory;          // mlockall + prefault
    bool     use_hugepages_2mb;    // MADV_HUGEPAGE en el pool
    bool     strict_isolation;     // exigir isolcpus/nohz_full/rcu_nocbs
    bool     use_deadline;         // SCHED_DEADLINE para hilos RT
    bool     use_bounded_mpmc;     // cola bounded Vyukov
    bool     use_timerfd;          // tick por timerfd en vez de usleep
    uint32_t rt_priority;          // prioridad SCHED_FIFO (0 = auto)
    uint64_t deadline_runtime_ns;  // presupuesto de ejecución
    uint64_t deadline_period_ns;   // periodo
} ChronosRT_config_t;
```

#### ChronosRT_metrics_t

```c
typedef struct ChronosRT_CACHE_ALIGNED {
    uint64_t total_events;           // Eventos totales procesados
    uint64_t total_cycles;           // Ciclos de CPU totales
    uint64_t min_latency_ns;         // Latencia mínima observada
    uint64_t max_latency_ns;         // Latencia máxima observada
    uint64_t avg_latency_ns;         // Latencia promedio aritmético
    uint64_t stddev_ns;              // Desviación estándar (jitter)
    double   cpu_usage_percent;      // Porcentaje uso CPU
    double   cache_miss_rate;        // Tasa de fallos de caché
    double   branch_miss_rate;       // Tasa de fallos de predicción ramas
    uint64_t memory_bandwidth_mbps;  // Ancho de banda memoria MB/s
    uint64_t wcet_ns;                // Worst-Case Execution Time
    uint64_t best_case_ns;           // Best-Case Execution Time
} ChronosRT_metrics_t;
```

### A.2 Funciones de Inicialización

#### ChronosRT_init()

```c
ChronosRT_status_t ChronosRT_init(
    ChronosRT_handle_t* handle,      // [out] Handle del sistema
    const ChronosRT_config_t* config // [in]  Configuración
);
```

**Retornos:**
- `ChronosRT_SUCCESS (0)`: Inicialización exitosa
- `ChronosRT_ERROR_INIT (-1)`: Fallo en inicialización
- `ChronosRT_ERROR_MEMORY (-2)`: No hay memoria disponible
- `ChronosRT_ERROR_CPU (-3)`: Error configuración CPU
- `ChronosRT_ERROR_INVALID (-5)`: Parámetros inválidos

**Proceso de inicialización:**
1. Validación de parámetros de entrada
2. Asignación de estructura core (`calloc`)
3. Copia de configuración del usuario
4. Aplicación de valores por defecto si corresponde
5. Inicialización de Ring Buffer SPSC
6. Inicialización de Cola MPMC (Michael-Scott)
7. Cálculo de offset para ciclo contador
8. Detección y configuración de afinidad CPU
9. Creación de worker threads
10. Retorno del handle al usuario

**Ejemplo de uso:**
```c
ChronosRT_config_t config = {0};
config.event_pool_size = 131072;  // 128K eventos
config.use_simd = true;            // Activar AVX/NEON
config.use_hugepages = true;       // Hugepages para TLB
config.use_cpu_affinity = true;    // Fijar a CPUs 2,3
config.enabled_cpus[0] = 2;
config.enabled_cpus[1] = 3;
config.num_cpus = 2;

ChronosRT_handle_t handle;
ChronosRT_status_t status = ChronosRT_init(&handle, &config);
if (status != ChronosRT_SUCCESS) {
    fprintf(stderr, "Init failed: %d\n", status);
    exit(1);
}
```

#### ChronosRT_shutdown()

```c
ChronosRT_status_t ChronosRT_shutdown(
    ChronosRT_handle_t handle  // [in] Handle del sistema
);
```

**Proceso de shutdown:**
1. Setear flag `running = false` para notificar threads
2. Esperar terminación de worker threads (`pthread_join`)
3. Destruir cola MPMC y liberar nodos
4. Liberar ring buffer
5. Liberar estructura core

### A.3 Funciones de Publicación/Suscripción

#### ChronosRT_event_publish()

```c
ChronosRT_status_t ChronosRT_event_publish(
    ChronosRT_handle_t handle,  // [in] Handle del sistema
    const void* data,                  // [in] Puntero a datos
    uint32_t size                      // [in] Tamaño en bytes
);
```

**Algoritmo:**
1. Validar handle y puntero de datos
2. Asignar evento (`malloc` - 64 bytes)
3. Capturar timestamp actual en nanosegundos
4. Obtener ID de CPU actual (`sched_getcpu()`)
5. Copiar datos usando SIMD si está habilitado
6. Intentar push al Ring Buffer SPSC (fast path)
7. Si falla, intentar push a Cola MPMC (slow path)
8. Si ambos fallan, liberar evento y retornar error

**Complejidad:** O(1) amortizado

**Ejemplo:**
```c
typedef struct {
    uint32_t command;
    float position[3];
    uint16_t velocity;
} motion_command_t;

motion_command_t cmd = {
    .command = 0x01,
    .position = {100.0f, 200.0f, 300.0f},
    .velocity = 1500
};

ChronosRT_status_t ret = ChronosRT_event_publish(
    handle, 
    &cmd, 
    sizeof(cmd)
);
```

#### ChronosRT_event_subscribe()

```c
ChronosRT_status_t ChronosRT_event_subscribe(
    ChronosRT_handle_t handle,  // [in] Handle del sistema
    ChronosRT_event_t* event,   // [out] Evento recibido
    uint32_t timeout_us                // [in] Timeout microsegundos
);
```

**Algoritmo:**
1. Validar handle y puntero de evento
2. Intentar pop del Ring Buffer SPSC (fast path)
3. Si está vacío, intentar pop de Cola MPMC (slow path)
4. Si ambos están vacíos:
   - Sleep por `timeout_us` microsegundos
   - Retornar `ChronosRT_ERROR_TIMEOUT`
5. Copiar evento al buffer del usuario
6. Liberar evento original
7. Retornar éxito

### A.4 Funciones de Benchmarking

#### ChronosRT_benchmark_run()

```c
ChronosRT_status_t ChronosRT_benchmark_run(
    ChronosRT_handle_t handle,        // [in] Handle
    ChronosRT_benchmark_mode_t mode,  // [in] Modo
    uint32_t duration_sec,            // [in] Duración segundos
    ChronosRT_metrics_t* metrics      // [out] Métricas
);
```

**Modos disponibles:**

| Modo | Descripción | Threads | Métricas principales |
|------|-------------|---------|---------------------|
| `ChronosRT_MODE_LATENCY` | Medición latencia round-trip | 1 | min/max/avg/stddev |
| `ChronosRT_MODE_THROUGHPUT` | Máximo throughput | 4 | events/sec |
| `ChronosRT_MODE_JITTER` | Análisis jitter | 1 | stddev, percentiles |
| `ChronosRT_MODE_STRESS` | Prueba estrés CPU | 4 | CPU load, thermal |
| `ChronosRT_MODE_WCET` | Worst-case execution | 1 | WCET medido |

**Hilos de benchmark:**

1. **bench_latency_thread**: 
   - Publica evento y mide tiempo hasta recepción
   - Calcula estadísticas en tiempo real
   - Usa afinidad CPU automática

2. **bench_throughput_thread**:
   - Publica ráfagas de 1000 eventos
   - Cuenta total eventos por segundo
   - Múltiples threads concurrentes

3. **bench_stress_thread**:
   - Ejecuta carga computacional intensiva
   - Prioridad SCHED_FIFO máxima
   - Evalúa estabilidad térmica

#### ChronosRT_metrics_report()

```c
ChronosRT_status_t ChronosRT_metrics_report(
    const ChronosRT_metrics_t* metrics  // [in] Métricas
);
```

**Salida formateada (ejemplo real en host sin PREEMPT_RT):**
```
=== ChronosRT Benchmark Results ===
Total Events:        0
WCET (ns):           2046830
Best Case (ns):      1397
Min Latency (ns):    1397
Max Latency (ns):    2046830
Avg Latency (ns):    2808
StdDev (ns):         6298
CPU Usage (%):       0.00
Cache Miss Rate:     0.0000%
Branch Miss Rate:    0.0000%
================================

Status: NEEDS OPTIMIZATION
```

Los valores dependen del host. En un kernel con PREEMPT_RT y núcleos aislados
las cifras cambian; el estado `NEEDS OPTIMIZATION` en un kernel sin RT es un
diagnóstico correcto, no un fallo de la biblioteca.

**Criterios de estado:**
- `EXCELLENT`: stddev < 1000 ns (1 μs)
- `GOOD`: stddev < 5000 ns (5 μs)
- `NEEDS OPTIMIZATION`: stddev >= 5000 ns

### A.5 Funciones Utilitarias

#### ChronosRT_get_timestamp_ns()

```c
uint64_t ChronosRT_get_timestamp_ns(void);
```

**Implementación por arquitectura:**

**x86_64:**
```c
// Usa clock_gettime(CLOCK_MONOTONIC)
// Resolución típica: 1 ns
```

**ARM64:**
```c
// Usa clock_gettime(CLOCK_MONOTONIC)
// Resolución típica: 1 ns
```

**Retorno:** Timestamp en nanosegundos desde epoch monotónico

#### ChronosRT_get_cycles()

```c
uint64_t ChronosRT_get_cycles(void);
```

**Implementación por arquitectura:**

**x86_64 (RDTSC):**
```asm
rdtsc                    ; Leer contador de ciclos
; Resultado en EDX:EAX
```

**ARM64 (PMCCNTR_EL0):**
```asm
mrs x0, pmccntr_el0      ; Leer registro de performance counter
```

**Genérico (fallback):**
```c
clock_gettime(CLOCK_MONOTONIC, &ts);
return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
```

#### ChronosRT_detect_platform()

```c
ChronosRT_platform_t ChronosRT_detect_platform(void);
```

**Método de detección:**
1. Leer `/proc/cpuinfo`
2. Buscar strings identificadores:
   - "Raspberry Pi" → RPI4/RPI5
   - "Jetson" → JETSON_NANO/JETSON_ORIN
   - "Rockchip RK3588" → ROCK5B/ORANGE_PI5
3. Para x86_64 → INTEL_NUC o X86_64_RT
4. Default → PLATFORM_GENERIC

---

## Apéndice B: Especificaciones de Plataformas

### B.1 Raspberry Pi 4

| Característica | Valor |
|----------------|-------|
| **SoC** | Broadcom BCM2711 |
| **CPU** | Quad-core Cortex-A72 @ 1.5 GHz |
| **L1 Cache** | 32 KB (I+D) por core |
| **L2 Cache** | 512 KB compartido |
| **L3 Cache** | No disponible |
| **RAM** | 2/4/8 GB LPDDR4 |
| **SIMD** | NEON (ARMv8-A) |
| **Red** | Gigabit Ethernet |
| **Uso recomendado** | Desarrollo, prototipos |

**Configuración óptima:**
```ini
[performance]
event_pool_size = 32768
batch_size = 128
use_simd = true
use_hugepages = false

[cpu]
use_cpu_affinity = true
enabled_cpus = 2,3
```

### B.2 Raspberry Pi 5

| Característica | Valor |
|----------------|-------|
| **SoC** | Broadcom BCM2712 |
| **CPU** | Quad-core Cortex-A76 @ 2.0 GHz |
| **L1 Cache** | 32 KB (I+D) por core |
| **L2 Cache** | 1 MB compartido |
| **L3 Cache** | 2 MB compartido |
| **RAM** | 4/8 GB LPDDR4X |
| **SIMD** | NEON (ARMv8.2-A) |
| **Red** | Gigabit Ethernet PoE+ |
| **Uso recomendado** | Producción embebida |

**Mejoras vs RPi4:**
- 33% más frecuencia CPU
- 2x L2 cache
- L3 cache añadido
- Mejor ancho de banda memoria

### B.3 NVIDIA Jetson Orin

| Característica | Valor |
|----------------|-------|
| **SoC** | NVIDIA AGX Orin |
| **CPU** | 12-core Cortex-A78AE @ 2.2 GHz |
| **L1 Cache** | 64 KB (I+D) por core |
| **L2 Cache** | 2 MB por cluster |
| **L3 Cache** | 4 MB compartido |
| **RAM** | 8/16/32/64 GB LPDDR5 |
| **SIMD** | NEON + SVE (Scalable Vector Extension) |
| **GPU** | 2048-core Ampere CUDA |
| **Red** | 10 GbE opcional |
| **Uso recomendado** | AI/ML edge computing |

**Configuración óptima:**
```ini
[performance]
event_pool_size = 262144
batch_size = 512
use_simd = true
use_hugepages = true

[cpu]
use_cpu_affinity = true
enabled_cpus = 8,9,10,11  # Cluster grande
```

### B.4 Intel NUC (12ma Gen)

| Característica | Valor |
|----------------|-------|
| **CPU** | Intel Core i7-1260P |
| **Arquitectura** | Alder Lake (hybrid) |
| **P-cores** | 4x Performance @ 3.5 GHz |
| **E-cores** | 8x Efficient @ 2.5 GHz |
| **L1 Cache** | 48 KB I + 32 KB D (P-core) |
| **L2 Cache** | 768 KB por P-core |
| **L3 Cache** | 18 MB compartido |
| **RAM** | Hasta 64 GB DDR4/DDR5 |
| **SIMD** | SSE4.2, AVX2, AVX-512 |
| **Red** | 2.5 GbE |
| **Uso recomendado** | Industrial PC, gateway |

**Instrucciones SIMD disponibles:**
- AVX-512F: Operaciones vectoriales 512-bit
- AVX-512DQ: Operaciones doubleword/quadword
- AVX-512BW: Operaciones byte/word
- AVX2: 256-bit integer/floating-point

### B.5 Rockchip RK3588 (Orange Pi 5 / ROCK 5B)

| Característica | Valor |
|----------------|-------|
| **CPU** | Octa-core (4xA76 + 4xA55) |
| **Frecuencia** | 2.4 GHz (big), 1.8 GHz (LITTLE) |
| **L1 Cache** | 32 KB por core |
| **L2 Cache** | 512 KB por cluster |
| **L3 Cache** | 3 MB compartido |
| **RAM** | 4/8/16/32 GB LPDDR4/LPDDR4X |
| **SIMD** | NEON (ARMv8.2-A) |
| **NPU** | 6 TOPS INT8 |
| **Red** | 2.5 GbE |
| **Uso recomendado** | Edge AI, multimedia |

---

## Apéndice C: Guía de Optimización Avanzada

### C.1 Optimización de Cache

#### Evitar False Sharing

**Problema:** Dos threads modifican variables en la misma cache line.

**Solución:** Alinear estructuras a 64 bytes:
```c
#define ChronosRT_CACHE_LINE 64
#define ChronosRT_CACHE_ALIGNED __attribute__((aligned(ChronosRT_CACHE_LINE)))

typedef struct ChronosRT_CACHE_ALIGNED {
    volatile uint64_t counter;  // 8 bytes
    // 56 bytes de padding automático
} atomic_counter_t;
```

#### Prefetching de Datos

```c
void process_batch(event_t* events, int count) {
    for (int i = 0; i < count; i++) {
        process_event(&events[i]);
        
        // Prefetch siguiente evento (lectura, temporalidad alta)
        if (i + 1 < count) {
            __builtin_prefetch(&events[i + 1], 0, 3);
        }
        
        // Prefetch datos del evento actual
        if (events[i].data_ptr) {
            __builtin_prefetch(events[i].data_ptr, 0, 0);
        }
    }
}
```

**Parámetros de `__builtin_prefetch`:**
- `addr`: Dirección a prefetchear
- `rw`: 0=read, 1=write
- `locality`: 0=no temporal, 3=high temporal

### C.2 Optimización de Memoria

#### Uso de Hugepages

**Beneficios:**
- Reduce TLB misses significativamente
- Mejora rendimiento en acceso a memoria grande
- Disminuye overhead de page table walks

**Configuración del sistema:**
```bash
# Verificar hugepages disponibles
cat /proc/meminfo | grep Huge

# Asignar 64 hugepages de 2MB (128 MB total)
sudo sysctl -w vm.nr_hugepages=64

# Hacer permanente
echo "vm.nr_hugepages=64" >> /etc/sysctl.conf

# Verificar
grep Huge /proc/meminfo
```

**En ChronosRT Master:**
```c
config.use_hugepages = true;
```

#### Allocación NUMA-Aware

Para sistemas multi-socket:
```c
#include <numa.h>

// Asignar memoria del nodo local a la CPU
int node = numa_node_of_cpu(sched_getcpu());
void* ptr = numa_alloc_onnode(size, node);
```

### C.3 Optimización de CPU

#### Aislamiento de CPUs

**Kernel boot parameters:**
```bash
# Editar /etc/default/grub
GRUB_CMDLINE_LINUX="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3"

# Actualizar GRUB
sudo update-grub
sudo reboot
```

**Explicación:**
- `isolcpus=2,3`: Aislar CPUs 2 y 3 del scheduler
- `nohz_full=2,3`: Desactivar tick scheduler en esas CPUs
- `rcu_nocbs=2,3`: Mover callbacks RCU a otras CPUs

#### Afinidad de Threads

```c
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(2, &cpuset);  // Fijar a CPU 2
CPU_SET(3, &cpuset);  // y CPU 3

pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
```

#### Prioridades de Scheduling

```c
struct sched_param param;
param.sched_priority = sched_get_priority_max(SCHED_FIFO);
pthread_setschedparam(thread, SCHED_FIFO, &param);
```

**Políticas disponibles:**
- `SCHED_FIFO`: FIFO real-time (prioridad fija)
- `SCHED_RR`: Round-robin real-time
- `SCHED_OTHER`: Time-sharing (default)

### C.4 Optimización SIMD

#### AVX-512 (x86_64)

```c
#include <immintrin.h>

// Copia de 64 bytes usando AVX-512
void simd_copy_64(void* dst, const void* src) {
    __m512i v = _mm512_loadu_si512((const __m512i*)src);
    _mm512_storeu_si512((__m512i*)dst, v);
}

// Zero-fill de 64 bytes
void simd_zero_64(void* ptr) {
    _mm512_storeu_si512((__m512i*)ptr, _mm512_setzero_si512());
}
```

#### NEON (ARM64)

```c
#include <arm_neon.h>

// Copia de 32 bytes usando NEON
void neon_copy_32(void* dst, const void* src) {
    uint64x2_t v0 = vld1q_u64((const uint64_t*)src);
    uint64x2_t v1 = vld1q_u64((const uint64_t*)(src + 16));
    vst1q_u64((uint64_t*)dst, v0);
    vst1q_u64((uint64_t*)(dst + 16), v1);
}
```

---

## Apéndice D: Troubleshooting Avanzado

### D.1 Debugging con GDB

**Compilación con debug symbols:**
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

**Sesión de debugging:**
```bash
gdb ./build/chronosrt_bench

(gdb) break ChronosRT_event_publish
(gdb) break ChronosRT_ring_push
(gdb) run --mode=latency --duration=5

# Al llegar al breakpoint
(gdb) bt full              # Backtrace completo
(gdb) p *event             # Examinar evento
(gdb) p *ring              # Examinar ring buffer
(gdb) info registers       # Registros CPU
(gdb) stepi                # Step instrucción por instrucción
```

### D.2 Análisis con perf

**Perfilado básico:**
```bash
perf stat -e cycles,instructions,cache-references,cache-misses \
    ./build/chronosrt_bench --mode=latency --duration=30
```

**Salida típica:**
```
Performance counter stats for './build/chronosrt_bench':
    45,234,567,890      cycles
    89,123,456,789      instructions              # 1.97 insn per cycle
     1,234,567,890      cache-references
            12,345,678  cache-misses              # 1.00 % of all cache refs
```

**Flame graph:**
```bash
# Grabar
perf record -F 99 -g ./build/chronosrt_bench --mode=throughput --duration=60

# Generar flame graph
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### D.3 Análisis de Memory Leaks

**Con Valgrind:**
```bash
valgrind --leak-check=full --show-leak-kinds=all \
    --track-origins=yes ./build/chronosrt_test
```

**Salida:**
```
==12345== LEAK SUMMARY:
==12345==    definitely lost: 0 bytes in 0 blocks
==12345==    indirectly lost: 0 bytes in 0 blocks
==12345==      possibly lost: 0 bytes in 0 blocks
==12345==    still reachable: 0 bytes in 0 blocks
```

### D.4 Diagnóstico de Problemas de Rendimiento

**Checklist:**

1. **Verificar PREEMPT_RT:**
```bash
zcat /proc/config.gz | grep PREEMPT_RT
# Debe mostrar: CONFIG_PREEMPT_RT=y
```

2. **Verificar aislamiento CPU:**
```bash
cat /proc/cmdline | grep isolcpus
cat /proc/interrupts | grep -v "CPU"
```

3. **Verificar hugepages:**
```bash
cat /proc/meminfo | grep Huge
# HugePages_Total: 64
# HugePages_Free: 64
```

4. **Verificar governor de CPU:**
```bash
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
# Debe ser: performance
```

5. **Verificar interrupts por segundo:**
```bash
watch -n1 'cat /proc/interrupts | head -20'
```

---

## Apéndice E: Changelog y Roadmap

### E.1 Changelog v0.1.0

**Fecha:** noviembre 2026 · **ABI:** `0x000100`

**Características:**
- ✅ Runtime de eventos con pool pre-asignado (cero `malloc` en el camino caliente)
- ✅ Ring SPSC y cola MPMC acotada (Vyukov) lock-free, sin ABA
- ✅ `SCHED_DEADLINE`/`SCHED_FIFO`, afinidad, `timerfd`, `mlockall` + *prefault*
- ✅ Histograma de latencias con percentiles y WCET medido
- ✅ Detección de kernel 5.15+ … 7.x (PREEMPT_RT, EEVDF, `sched_ext`, `io_uring`, AF_XDP, `memfd_secret`)
- ✅ Resiliencia: snapshot A/B, WAL con CRC, *golden image* atómica, watchdog
- ✅ Observabilidad: contadores, traza `trace_marker`, autotest de colas
- ✅ SIMD AVX-512/AVX2/NEON/SVE con despacho en runtime
- ✅ HAL: CRC-32C por instrucción, GF(2^8), Reed-Solomon SIMD, PHC/Launch Time
- ✅ Maestro EtherCAT autónomo (`AF_PACKET`/`0x88A4`): escaneo, SM/FMMU, ESM, DC, CoE/SDO, WKC, redundancia
- ✅ EtherTux v0.1.0 (`0x88B5`): cabecera de 26 B, QoS P0–P5, LAPM, FEC, ET-ESM, ET-Sync, ET-WKC/topología, ET-MBX, FRM, Flow Aggregator, anillo TX
- ✅ Integración como proveedor (`tuxrt_provider_descriptor`) y puente desacoplado
- ✅ 9 suites de test / 485 aserciones; 295 símbolos `ChronosRT_*`

**Medido en el host de referencia (AMD Ryzen 7 4800H, Linux 7.2.11):**
- Construir datagrama EtherCAT de 1024 B: 24,0 ns
- Construir PDU EtherTux de 1464 B: 180,8 ns (140,8 ns sin copia)
- CRC-32C `sse4.2-x4` con MTU: 9,70 GB/s (6,45 M tramas/s)

### E.2 Roadmap

**Exploratorio (no bloqueante):**
- [ ] Planificador `sched_ext` (BPF) consciente de EtherCAT/DC
- [ ] Ruta AF_XDP *zero-copy* para EtherTux
- [ ] `io_uring` con `SQPOLL`
- [ ] SVE real en kernels/compiladores que lo soporten
- [ ] Secretos en `memfd_secret`
- [ ] Validación física de EtherCAT/EtherTux en banco de pruebas


---

**Fin del Apéndice**

**Documento completado:** Noviembre 2026  
**Próxima revisión:** Noviembre 2027  
**Mantenido por:** Equipo de Desarrollo ChronosRT Master

