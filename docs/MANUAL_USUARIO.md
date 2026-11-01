# Manual de Usuario Final - ChronosRT Master

**Versión del Software:** 0.1.0  
**Fecha:** Noviembre 2026  
**Tipo de Documento:** Manual de Usuario

---

## Tabla de Contenidos

1. [Introducción](#1-introducción)
2. [Requisitos del Sistema](#2-requisitos-del-sistema)
3. [Instalación](#3-instalación)
4. [Configuración Inicial](#4-configuración-inicial)
5. [Operación del Sistema](#5-operación-del-sistema)
6. [Resolución de Problemas](#6-resolución-de-problemas)
7. [Mantenimiento Preventivo](#7-mantenimiento-preventivo)
8. [Preguntas Frecuentes](#8-preguntas-frecuentes)
9. [Glosario de Términos](#9-glosario-de-términos)
10. [Maestro EtherCAT: Uso y Puesta en Marcha](#10-maestro-ethercat-uso-y-puesta-en-marcha)
11. [EtherTux: Ethernet comercial determinista](#11-ethertux-ethernet-comercial-determinista)

---

## 1. Introducción

### 1.1 ¿Qué es ChronosRT Master?

ChronosRT Master es un **framework de tiempo real determinista** diseñado para sistemas que requieren comunicación ultrarrápida y precisa. Imagina que es como un sistema de mensajería ultraveloz que garantiza que los mensajes lleguen en exactamente el tiempo esperado, sin demoras impredecibles.

**¿Para qué sirve?**

ChronosRT Master se utiliza en sistemas industriales como:
- Líneas de producción automatizadas
- Robots de fabricación
- Sistemas de control de máquinas
- Redes de comunicación industrial EtherCAT
- Redes deterministas sobre Ethernet comercial con EtherTux
- Sistemas de automatización de edificios

### 1.2 Características Principales

| Característica | Descripción |
|----------------|-------------|
| **Latencia ultra baja** | Objetivo de diseño: < 10 microsegundos con kernel RT y CPU aislada |
| **Alto rendimiento** | Objetivo: > 10 millones de eventos por segundo |
| **Jitter mínimo** | Objetivo: desviación < 1 microsegundo en entorno RT |
| **Escalabilidad** | De 1 a 16 núcleos de CPU |
| **Bajo consumo** | Operación event-driven con pools pre-asignados |
| **Dos protocolos** | Maestro **EtherCAT** (`0x88A4`) y **EtherTux** (`0x88B5`), seleccionables |
| **Detección de kernel** | Sondeo de Linux 5.15+ … 7.x (PREEMPT_RT, EEVDF, `sched_ext`, `io_uring`, AF_XDP) |
| **Resiliencia** | Snapshot A/B, WAL con CRC, golden image, watchdog, FEC y FRM |
| **Aceleración HW** | CRC-32C por instrucción y Reed-Solomon SIMD (NEON/AVX2) |

> Las cifras de latencia, jitter y throughput son **objetivos de diseño** que
> dependen del kernel y del hardware. El manual técnico y la guía de
> benchmarking explican cómo medirlas y qué se ha medido realmente.


### 1.3 Conceptos Básicos

Antes de continuar, es importante entender algunos términos:

- **Latencia**: Tiempo que tarda un mensaje en ir de un punto a otro
- **Jitter**: Variación en el tiempo de llegada de los mensajes
- **Throughput**: Cantidad de datos que se pueden procesar por segundo
- **Evento**: Una unidad de información que se transmite en el sistema

---

## 2. Requisitos del Sistema

### 2.1 Requisitos de Hardware

#### Hardware Mínimo
| Componente | Requisito Mínimo |
|------------|------------------|
| Procesador | ARM64 (AArch64), ARMv7 o x86_64 con soporte SIMD |
| Memoria RAM | 256 MB |
| Almacenamiento | 50 MB libres |
| Red | 1 Gbps (recomendado) |

#### Plataformas de Hardware Soportadas

**Computadoras de placa única (SBC):**
- Raspberry Pi Zero 2 W, 3, 4 (4GB RAM recomendado), 5, CM4 y CM5
- NVIDIA Jetson Nano
- NVIDIA Jetson Orin
- Intel NUC (i5 o superior)
- Orange Pi Zero2, Zero3, 3 LTS, 4 LTS, 5 y 5 Plus
- BigTreeTech CB1/CB2 y MKS PI/Skipr
- Radxa Rock Pi 4, Rock 3 y Rock 5
- Banana Pi M2 Zero, M4 y M5
- ROCK 5B

La matriz completa (**29 perfiles**) está en `scripts/lib/sbc.sh` y se consulta
con `./scripts/build.sh --list`. Incluye arquitectura, CRC32 por hardware,
motor criptográfico, PHC y flags de compilación.

**Sistemas de escritorio/servidor:**
- Procesadores Intel Core i5/i7/i9 (10ma generación o superior)
- Procesadores AMD Ryzen (5000 series o superior)
- Sistemas x86_64 con kernel PREEMPT_RT

### 2.2 Requisitos de Software

| Software | Versión Mínima | Recomendado |
|----------|---------------|-------------|
| Sistema Operativo | Linux 5.15 | Linux 7.x con PREEMPT_RT |
| Compilador GCC | 11.0 | 13.0+ (o Clang 15+) |
| CMake | 3.16 | 3.25+ |
| Bibliotecas | pthread, rt, m, dl | Últimas versiones estables |

> Desde Linux 6.12, PREEMPT_RT está integrado en mainline y se activa con
> `preempt=full`. ChronosRT detecta el estado real del kernel y avisa si no hay
> RT; no modifica la configuración del sistema.


### 2.3 Requisitos de Red

- **Switch industrial**: necesario para EtherTux; EtherCAT suele usar topología en línea o anillo sin switch
- **Cables**: Categoría 5e o superior (CAT6 recomendado)
- **Distancia máxima**: 100 metros entre dispositivos
- **Ancho de banda mínimo**: 100 Mbps

---

## 3. Instalación

### 3.1 Instalación Paso a Paso

#### Paso 1: Preparación del Sistema

Antes de instalar ChronosRT Master, asegúrate de que tu sistema esté actualizado:

```bash
# Para sistemas Debian/Ubuntu
sudo apt update
sudo apt upgrade -y

# Verificar versión del kernel
uname -r

# Instalar dependencias básicas
sudo apt install -y build-essential cmake git
sudo apt install -y pthread librt-dev
```

#### Paso 2: Descargar el Código Fuente

```bash
# Ir al directorio de trabajo
cd ~

# Clonar el repositorio (o copiar desde el medio de instalación)
# Si tienes el código en un archivo ZIP:
unzip ChronosRT-v0.1.0.zip
cd ChronosRT_MASTER

# O si usas git:
git clone https://github.com/LauOtero/ChronosRT_MASTER.git
cd ChronosRT_MASTER
```

#### Paso 3: Configurar la Construcción

```bash
# Crear directorio de construcción
mkdir -p build
cd build

# Configurar con CMake
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

**Opciones de configuración adicionales:**

```bash
# Habilitar optimizaciones SIMD (recomendado)
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCHRONOSRT_USE_SIMD=ON

# Habilitar soporte para hugepages (mejor rendimiento)
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCHRONOSRT_USE_HUGEPAGES=ON

# Construir con todas las opciones
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCHRONOSRT_USE_SIMD=ON \
    -DCHRONOSRT_USE_HUGEPAGES=ON \
    -DCHRONOSRT_BUILD_TESTS=ON \
    -DCHRONOSRT_BUILD_BENCHMARK=ON \
    -DCHRONOSRT_BUILD_PROTOCOLS=ON
```

También puede usar el script de ayuda, que resuelve toolchain y flags por SBC:

```bash
./scripts/build.sh --list
./scripts/build.sh --detect
./scripts/build.sh --sbc rpi5 --release
```


#### Paso 4: Compilar

```bash
# Compilar usando todos los núcleos del procesador
make -j$(nproc)

# La compilación tarda aproximadamente 2-5 minutos según el hardware
```

#### Paso 5: Instalar

```bash
# Instalar en el sistema (requiere permisos de administrador)
sudo make install

# O instalar en un directorio local (sin sudo)
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/chronosrt-install
make install
```

### 3.2 Verificación de la Instalación

Después de instalar, verifica que todo funciona correctamente:

```bash
# Ir al directorio de construcción
cd build

# Ejecutar todas las suites
ctest --output-on-failure

# Deberías ver:
# 10/10 Test #10: chronosrt_test_aalid .......... Passed
# 100% tests passed out of 10
```

Cada binario informa además su número de aserciones:

```bash
./chronosrt_test               # 20/20
./chronosrt_test_lockfree      # 26/26
./chronosrt_test_rt            # 30/30
./chronosrt_test_resilience    # 29/29
./chronosrt_test_integration   # 27/27
./chronosrt_test_network       # 58/58
./chronosrt_test_hwaccel       # 40/40
./chronosrt_test_ethertux      # 103/103
./chronosrt_test_ethertux_v010 # 152/152
./chronosrt_test_aalid         # 62/62
```

Total: **547 aserciones**.


### 3.3 Instalación en Plataformas Específicas

#### Raspberry Pi 4/5

```bash
# Instalar toolchain de cross-compilation
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Configurar para cross-compilation ARM64
cd ChronosRT_MASTER
mkdir -p build && cd build
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-linux-gnu.cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

make -j$(nproc)
```

#### NVIDIA Jetson

```bash
# Configurar para Jetson (ARM64)
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/jetson.cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCHRONOSRT_USE_SIMD=ON

make -j$(nproc)
```

---

## 4. Configuración Inicial

### 4.1 Configuración del runtime

La configuración se pasa **programáticamente** en `ChronosRT_config_t` (véase la
[referencia de API](API.md)). La biblioteca **no lee** archivos `.conf` por sí
misma; los ejemplos de este apartado son una plantilla operativa para que su
aplicación los cargue y rellene la estructura.

```
/etc/chronosrt/           # Configuración global del sistema (plantilla)
~/.config/chronosrt/      # Configuración por usuario (plantilla)
./config/                 # Configuración local del proyecto (plantilla)
```


### 4.2 Archivo de Configuración Principal

Crea un archivo llamado `chronosrt.conf` (plantilla que su aplicación carga) en tu directorio de trabajo:

```ini
# Configuración básica de ChronosRT Master

[general]
# Versión del formato de configuración
version = 1.0

# Plataforma de hardware
platform = auto  # auto, rpi4, rpi5, jetson_orin, intel_nuc, etc.

[performance]
# Tamaño del pool de eventos (número de eventos en memoria)
event_pool_size = 65536

# Tamaño del batch de procesamiento
batch_size = 256

# Usar SIMD para optimizaciones
use_simd = true

# Usar hugepages para mejor rendimiento
use_hugepages = false

[cpu]
# Habilitar afinidad de CPU (mejora el determinismo)
use_cpu_affinity = true

# CPUs a usar (lista separada por comas)
enabled_cpus = 0,1,2,3

[rt]
# mlockall + prefault: elimina page faults en el camino caliente
lock_memory = true

# MADV_HUGEPAGE en los pools
use_hugepages_2mb = true

# Exigir isolcpus/nohz_full/rcu_nocbs (avisa si faltan)
strict_isolation = true

# SCHED_DEADLINE con presupuesto (si el kernel lo soporta)
use_deadline = true
deadline_runtime_ns = 200000
deadline_period_ns = 1000000

# Cola MPMC acotada (Vyukov) en lugar de Michael-Scott
use_bounded_mpmc = true

# Tick periódico por timerfd (sin usleep)
use_timerfd = true

[memory]
# Usar asignación NUMA-aware
use_numa_aware = true

# Tamaño del stack por thread (KB)
stack_size_kb = 256

[network]
# Interfaz Ethernet dedicada al bus EtherCAT (debe estar UP)
interface = enp2s0

# Timeout de red (microsegundos)
network_timeout_us = 1000
```

### 4.3 Configuración de Parámetros de Rendimiento

#### Para Latencia Ultra Baja

```ini
[performance]
event_pool_size = 131072
batch_size = 128
use_simd = true
use_hugepages = true

[cpu]
use_cpu_affinity = true
enabled_cpus = 2,3  # Usar CPUs aisladas
```

#### Para Alto Throughput

```ini
[performance]
event_pool_size = 262144
batch_size = 512
use_simd = true
use_hugepages = true

[cpu]
use_cpu_affinity = false  # Permitir migración de threads
enabled_cpus = 0,1,2,3
```

#### Para Sistemas Embebidos (recursos limitados)

```ini
[performance]
event_pool_size = 8192
batch_size = 64
use_simd = false
use_hugepages = false

[cpu]
use_cpu_affinity = true
enabled_cpus = 0

[memory]
use_numa_aware = false
stack_size_kb = 128
```

### 4.4 Configuración de Variables de Entorno

ChronosRT Master también puede configurarse mediante variables de entorno
(convenciones de la aplicación; la biblioteca solo lee `CHRONOSRT_PROTOCOL`
directamente):

```bash
# Directorio de configuración
export ChronosRT_CONFIG_DIR=/etc/chronosrt

# Nivel de logging (0=silencioso, 1=errores, 2=info, 3=debug)
export ChronosRT_LOG_LEVEL=2

# Archivo de log
export ChronosRT_LOG_FILE=/var/log/chronosrt.log

# Habilitar modo de diagnóstico
export ChronosRT_DIAGNOSTIC=1
```

---

## 5. Operación del Sistema

### 5.1 Arranque del Sistema

#### Procedimiento de Arranque Estándar

```
┌─────────────────────────────────────────────────────────────┐
│                ARRANQUE DE ChronosRT Master                 │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   1. Verificar requisitos del sistema                       │
│      └─> ¿Hardware compatible?                              │
│      └─> ¿Dependencias instaladas?                          │
│                                                             │
│   2. Cargar configuración                                   │
│      └─> ¿Archivo de configuración existe?                  │
│      └─> ¿Parámetros válidos?                               │
│                                                             │
│   3. Inicializar componentes                                │
│      └─> ¿Memoria asignada correctamente?                   │
│      └─> ¿Threads creados?                                  │
│                                                             │
│   4. Verificar conectividad                                 │
│      └─> ¿Red funcional?                                    │
│      └─> ¿Esclavos detectados?                              │
│                                                             │
│   5. Sistema OPERATIVO                                      │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

#### Script de Arranque Manual

```bash
#!/bin/bash
# startup.sh - Script de arranque de ChronosRT Master

echo "Iniciando ChronosRT Master..."

# Verificar que el sistema cumple requisitos
echo "[1/5] Verificando sistema..."
if [ ! -f ./build/chronosrt_bench ]; then
    echo "Error: ChronosRT Master no está compilado. Ejecuta 'make' primero."
    exit 1
fi

# Cargar configuración
echo "[2/5] Cargando configuración..."
if [ -f ./config/chronosrt.conf ]; then
    source ./config/chronosrt.conf
    echo "  Configuración cargada desde ./config/chronosrt.conf"
else
    echo "  Usando configuración por defecto"
fi

# Inicializar ChronosRT Master
echo "[3/5] Inicializando ChronosRT Master..."
export ChronosRT_LOG_LEVEL=2

# Ejecutar modo de verificación
echo "[4/5] Verificando componentes..."
./build/chronosrt_bench --mode=latency --duration=5

echo "[5/5] Sistema listo."
echo ""
echo "ChronosRT Master está operativo."
echo "Ejecuta './build/chronosrt_bench' para comenzar."
```

### 5.2 Apagado del Sistema

#### Procedimiento de Apagado Seguro

```
┌─────────────────────────────────────────────────────────────┐
│                APAGADO DE ChronosRT Master                  │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   1. Detener recepción de nuevos eventos                    │
│                                                             │
│   2. Esperar a que eventos pendientes completen             │
│      (timeout de 5 segundos)                                │
│                                                             │
│   3. Liberar recursos de memoria                            │
│                                                             │
│   4. Cerrar conexiones de red                               │
│                                                             │
│   5. Terminar threads de trabajo                            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

#### Script de Apagado

```bash
#!/bin/bash
# shutdown.sh - Script de apagado de ChronosRT Master

echo "Deteniendo ChronosRT Master..."

# Enviar señal de terminación a su aplicación (sustituya el nombre)
APP="mi_aplicacion_chronosrt"
pkill -TERM "$APP"

# Esperar a que termine de forma ordenada
sleep 2

# Si quedan procesos zombie, forzar terminación
pkill -9 "$APP" 2>/dev/null || true

echo "ChronosRT Master detenido."
```

### 5.3 Monitoreo en Tiempo Real

ChronosRT Master proporciona métricas en tiempo real:

```bash
# Ver métricas durante ejecución
./build/chronosrt_bench --mode=latency --duration=60 --threads=4

# Salida real (host de ejemplo sin PREEMPT_RT):
# ChronosRT Benchmark v0.1.0
# Platform: 4
# Mode: 0
# Duration: 60s
#
# [chronosrt] Kernel: 7.2.11 (...)
# [chronosrt] PREEMPT_RT: no
# [chronosrt] AVISO: kernel sin PREEMPT_RT; la latencia determinista puede degradarse.
#
# === ChronosRT Benchmark Results ===
# Total Events:        ...
# WCET (ns):           ...
# Best Case (ns):      ...
# Min Latency (ns):    ...
# Max Latency (ns):    ...
# Avg Latency (ns):    ...
# StdDev (ns):         ...
# CPU Usage (%):       ...
# ================================
#
# Status: ...
```

Los valores concretos dependen del host. En un kernel sin RT el estado puede ser
`NEEDS OPTIMIZATION`; es la respuesta correcta, no un fallo de la biblioteca.
Consulte [BENCHMARKING.md](BENCHMARKING.md) para interpretar las cifras.


### 5.4 Rendimiento medido (referencia)

Cifras reales del host de referencia (AMD Ryzen 7 4800H, Linux 7.2.11, build
`Release`), sin NIC y por tanto reproducibles:

| Operación | Coste |
|---|---|
| EtherCAT: construir datagrama de 1024 B | 24,0 ns |
| EtherCAT: parsear datagrama | 3,5–3,8 ns |
| EtherTux: construir PDU de 1464 B | 180,8 ns |
| EtherTux: construir cabecera sin copia (1464 B) | 140,8 ns |
| EtherTux: parsear + validar CRC (1464 B) | 138,6 ns |
| Flow Aggregator 8 flujos × 32 B | 48,0 ns |
| Admisión QoS (P2, 576 B) | 16,8 ns |
| CRC-32C con MTU completa (`sse4.2-x4`) | 9,70 GB/s · 6,45 M tramas/s |
| ID local: ID candidato (CRC-32C plegado) | 5,2 ns |
| ID local: filtro de destino | 6,2 ns |
| ID local: admisión de un nodo nuevo | 62 ns |

El coste de CPU queda muy por debajo del tiempo de transmisión del enlace. El
detalle y la metodología están en [BENCHMARKING.md](BENCHMARKING.md).

### 5.5 Comandos de Operación Comunes

| Comando | Descripción |
|---------|-------------|
| `ctest --test-dir build --output-on-failure` | Ejecutar las 10 suites |
| `./chronosrt_test` | Suite de núcleo (20 aserciones) |
| `./chronosrt_bench --mode=latency` | Benchmark de latencia |
| `./chronosrt_bench --mode=throughput` | Benchmark de throughput |
| `./chronosrt_bench --mode=jitter` | Análisis de jitter |
| `./chronosrt_bench --mode=stress` | Prueba de estrés |
| `./chronosrt_bench --mode=wcet` | Análisis de WCET |
| `./chronosrt_bench --mode=crc` | Capacidad de CRC-32C por backend |
| `./chronosrt_bench --mode=frame` | Coste de construir/parsear por trama |

### 5.6 Identidad de nodos (ID local determinista)

Cada nodo puede usar una identidad de 16 bits codificada en su MAC
(`02:00:00:00:ID[15:8]:ID[7:0]`). Ventajas operativas:

- El switch aprende la MAC y **deja de inundar** la trama al resto de puertos.
- La recepción descarta en ~6 ns las tramas dirigidas a otros nodos.
- La identidad sobrevive a reinicios (la tabla `{MAC → ID}` se persiste).

```c
ChronosRT_et_master_t* m = ChronosRT_et_master_create();
ChronosRT_et_aalid_config_t cfg;
ChronosRT_et_aalid_default_config(&cfg);   /* 65 536 IDs, gracia de 10 min */

ChronosRT_et_master_enable_local_ids(m, &cfg);
ChronosRT_et_master_open_auto(m, "enp2s0");     /* detecta 1G/2.5G/10G */
ChronosRT_et_master_set_local_id(m, 0x0102);    /* ID propio del maestro */

/* Admisión de un nodo que envía su MAC de fábrica y un nonce */
uint16_t id = 0;
ChronosRT_et_aalid_result_t res;
if (ChronosRT_et_master_admit_node(m, mac_fabrica, nonce, now_ms, &id, &res) ==
    ChronosRT_SUCCESS) {
    /* enviar al nodo por su identidad (unicast, sin difusión) */
    ChronosRT_et_master_send_to(m, id, ChronosRT_ET_P2, ChronosRT_ET_TYPE_DATA,
                                1, payload, len);
}

/* Mantenimiento periódico, fuera del superframe */
ChronosRT_et_master_expire_local_ids(m, now_ms);
ChronosRT_et_aalid_save(ChronosRT_et_master_aalid(m), "/var/lib/chronosrt/aalid.bin");
```

`admit_node` devuelve además el motivo en `res`: `OK`, `COLLISION` (se asignó
otro ID porque el candidato estaba ocupado), `DUPLICATE` (la MAC ya tenía ID) o
`CLONE` (misma MAC con otro nonce: se rechaza). El maestro acepta difusión
siempre; el unicast solo si va dirigido a su MAC local.

Límites y mantenimiento:

- Rango de IDs: **65 536** (0 … 65 535). `ChronosRT_ET_LOCAL_ID_NONE` (0xFFFF)
  significa «sin ID local»; con `send_to()` equivale a difusión.
- La tabla soporta **256 nodos** por defecto (≈ 14 KB de RAM). Para más nodos,
  suba `cfg.table_slots` (potencia de 2; 1024 ⇒ ≈ 24 KB de tabla).
- `expire_local_ids()` libera los IDs sin heartbeat en `cfg.grace_ms` (10 min
  por defecto). Llámelo una vez por superframe o en el hilo de baja prioridad.
- `ChronosRT_et_aalid_release()` recorre la tabla (O(slots)); es administrativa,
  no la invoque en el camino crítico.
- El fichero de persistencia debe estar en un directorio con permiso de
  escritura; `save()` y `load()` devuelven `ChronosRT_ERROR_IO` si falla.

---

## 6. Resolución de Problemas

### 6.1 Problemas Comunes y Soluciones

#### Problema: "Command not found" al ejecutar ChronosRT Master

**Síntoma:**
```
bash: ./chronosrt_test: command not found
```

**Causa:** El programa no está compilado o no está en el directorio correcto.

**Solución:**
```bash
# Verificar que existe el archivo
ls -la build/chronosrt_test

# Si no existe, recompilar
cd build
cmake .. && make

# Verificar que es ejecutable
chmod +x build/chronosrt_test
```

#### Problema: Fallo de segmentación al iniciar

**Síntoma:**
```
Segmentation fault (core dumped)
```

**Causas posibles:**
1. Memoria insuficiente
2. Configuración inválida
3. Bug en el código

**Solución:**
```bash
# Reducir tamaño del pool de eventos
export ChronosRT_EVENT_POOL_SIZE=8192

# Verificar memoria disponible
free -h

# Ejecutar con valgrind para debug
valgrind ./build/chronosrt_test
```

#### Problema: Alta latencia o jitter elevado

**Síntoma:** Los resultados del benchmark muestran jitter > 1μs

**Causas posibles:**
1. Otras aplicaciones consumiendo CPU
2. Configuración subóptima
3. Kernel sin PREEMPT_RT

**Solución:**
```bash
# 1. Verificar que el kernel tiene PREEMPT_RT
zcat /proc/config.gz | grep PREEMPT

# 2. Aislar CPUs para ChronosRT Master
# Editar /etc/default/grub y añadir:
# GRUB_CMDLINE_LINUX="... isolcpus=2,3 ..."

# 3. Reiniciar y ejecutar con CPU affinity
taskset -c 2,3 ./build/chronosrt_bench --mode=latency
```

#### Problema: Error "Cannot allocate memory"

**Síntoma:**
```
ChronosRT_error: Cannot allocate memory
```

**Solución:**
```bash
# Reducir el tamaño del pool de eventos
# En chronosrt.conf:
[performance]
event_pool_size = 16384  # Reducir de 65536

# O habilitar hugepages
sudo sysctl -w vm.nr_hugepages=64
```

### 6.2 Tabla de Códigos de Error

| Código | Nombre | Descripción | Solución |
|--------|--------|-------------|----------|
| 0 | `ChronosRT_SUCCESS` | Operación exitosa | — |
| -1 | `ERROR_INIT` | Fallo en inicialización | Verificar configuración |
| -2 | `ERROR_MEMORY` | Fallo de asignación | Reducir `event_pool_size` |
| -3 | `ERROR_CPU` | Error de CPU | Verificar soporte SIMD |
| -4 | `ERROR_TIMEOUT` | Timeout | Aumentar `timeout_us` |
| -5 | `ERROR_INVALID` | Parámetros inválidos | Revisar configuración |
| -6 | `ERROR_NO_HARDWARE` | HW no soportado | Verificar plataforma |
| -7 | `ERROR_NOTSUP` | Operación no soportada en este kernel/HW | Actualizar kernel o usar *fallback* |
| -8 | `ERROR_STATE` | Estado inválido | Respetar la secuencia (`INIT → … → OP`) |
| -9 | `ERROR_IO` | Error de E/S | Revisar NIC/permisos |
| -10 | `ERROR_CORRUPT` | Corrupción de integridad | Revisar CRC/FEC y almacenamiento |
| -11 | `ERROR_FULL` | Cola/ring lleno | Aumentar capacidad o consumir más rápido |
| -12 | `ERROR_BACKPRESSURE` | El productor debe reducir la tasa | Bajar la tasa; la trama no se pierde |
| -13 | `ERROR_DROPPED` | Descartada por best-effort | Solo ocurre en P5 |
| -14 | `ERROR_NOTFOUND` | Nodo/flujo/canal no registrado | Registrar el nodo/flujo antes de usar |

### 6.3 Diagnóstico del Sistema

```bash
#!/bin/bash
# diagnose.sh - Script de diagnóstico

echo "=== ChronosRT Master Diagnostic Report ==="
echo "Date: $(date)"
echo ""

echo "--- System Info ---"
uname -a
echo ""

echo "--- CPU Info ---"
lscpu | grep -E "Model name|CPU\(s\)|Flags"
echo ""

echo "--- Memory Info ---"
free -h
echo ""

echo "--- Kernel PREEMPT ---"
if zcat /proc/config.gz 2>/dev/null | grep -q "CONFIG_PREEMPT_RT=y"; then
    echo "PREEMPT_RT: YES"
else
    echo "PREEMPT_RT: NO (puede afectar rendimiento)"
fi
echo ""

echo "--- Hugepages ---"
cat /proc/meminfo | grep -i huge
echo ""

echo "--- ChronosRT Master Build ---"
if [ -f ./build/chronosrt_test ]; then
    echo "Build: OK"
    ./build/chronosrt_test 2>&1 | head -5
else
    echo "Build: NOT FOUND"
fi
```

---

## 7. Mantenimiento Preventivo

### 7.1 Calendario de Mantenimiento

| Frecuencia | Tarea | Descripción |
|------------|-------|-------------|
| Diario | Verificación básica | Check de logs y métricas |
| Semanal | Limpieza de logs | Rotación de archivos de log |
| Mensual | Actualizaciones | Actualizar software y parches |
| Trimestral | Auditoría | Revisión de rendimiento histórico |
| Anual | Actualización mayor | Evaluación de hardware |

### 7.2 Tareas de Mantenimiento Diario

```bash
#!/bin/bash
# daily_check.sh - Verificación diaria

LOGFILE="/var/log/chronosrt/daily.log"

echo "=== Verificación Diaria ChronosRT Master ===" >> $LOGFILE
echo "Fecha: $(date)" >> $LOGFILE

# Verificar que el servicio está corriendo
if pgrep -x "mi_aplicacion_chronosrt" > /dev/null; then
    echo "[OK] ChronosRT Master está ejecutándose" >> $LOGFILE
else
    echo "[ALERTA] ChronosRT Master NO está ejecutándose" >> $LOGFILE
fi

# Verificar uso de memoria
MEM=$(free | grep Mem | awk '{printf("%.1f", $3/$2 * 100)}')
echo "[INFO] Uso de memoria: ${MEM}%" >> $LOGFILE

# Verificar temperatura (en Raspberry Pi)
if [ -f /sys/class/thermal/thermal_zone0/temp ]; then
    TEMP=$(cat /sys/class/thermal/thermal_zone0/temp)
    echo "[INFO] Temperatura CPU: $((TEMP/1000))°C" >> $LOGFILE
fi

echo "=== Verificación Completada ===" >> $LOGFILE
echo "" >> $LOGFILE
```

### 7.3 Script de Respaldo

```bash
#!/bin/bash
# backup.sh - Respaldo de configuración

BACKUP_DIR="/mnt/backup/chronosrt"
DATE=$(date +%Y%m%d_%H%M%S)

mkdir -p $BACKUP_DIR

# Respaldar configuración (plantillas de su aplicación)
cp -r /etc/chronosrt $BACKUP_DIR/config_$DATE 2>/dev/null || true
cp -r ~/.config/chronosrt $BACKUP_DIR/user_config_$DATE 2>/dev/null || true

# Respaldar logs (últimos 7 días)
find /var/log/chronosrt -name "*.log" -mtime -7 -exec cp {} $BACKUP_DIR/ \;

# Comprimir
cd $BACKUP_DIR
tar -czf ChronosRT_backup_$DATE.tar.gz *
rm -rf config_$DATE user_config_$DATE

echo "Respaldo guardado: $BACKUP_DIR/ChronosRT_backup_$DATE.tar.gz"
```

---

## 8. Preguntas Frecuentes

### 8.1 Preguntas Generales

**P: ¿ChronosRT Master funciona en Windows?**
R: ChronosRT Master está diseñado principalmente para sistemas Linux. Aunque puede compilarse en Windows usando WSL (Windows Subsystem for Linux), para producción se recomienda Linux con kernel PREEMPT_RT.

---

**P: ¿Cuál es la diferencia entre ChronosRT Master y SOEM?**
R: SOEM (Simple Open EtherCAT Master) es un maestro EtherCAT de propósito general. ChronosRT Master implementa su **propio maestro EtherCAT autónomo** (sockets `AF_PACKET`, EtherType `0x88A4`) conforme a IEC 61158 / ETG.1000, **sin depender de SOEM ni de ningún otro maestro externo**. Además del protocolo EtherCAT, ChronosRT Master aporta tiempo real determinista (planificación `SCHED_DEADLINE`/`SCHED_FIFO`), resiliencia (validación por WKC, detección de esclavas perdidas, redundancia de red) y rendimiento (varios datagramas por trama).

---

**P: ¿Puedo usar ChronosRT Master sin experiencia en sistemas embebidos?**
R: Sí, ChronosRT Master está diseñado para ser fácil de usar. El Manual de Usuario proporciona instrucciones paso a paso. Sin embargo, para optimización avanzada se recomienda tener conocimientos básicos de Linux y sistemas de tiempo real.

---

### 8.2 Preguntas de Rendimiento

**P: ¿Cómo puedo mejorar la latencia?**
R:
1. Usar kernel PREEMPT_RT
2. Habilitar hugepages
3. Aislar CPUs con isolcpus
4. Usar CPUs específicas con taskset
5. Desactivar servicios innecesarios

---

**P: ¿Por qué el benchmark muestra latencia diferente cada vez?**
R: Una variación pequeña es normal debido a factores del sistema. Si la variación es grande (>5μs), verifica:
- Que no haya otras aplicaciones usando CPU
- Que el kernel tenga PREEMPT_RT
- Que la memoria no esté fragmentada

---

**P: ¿Cuántos eventos puede procesar ChronosRT Master por segundo?**
R: El límite teórico es >10 millones de eventos/segundo. En la práctica, esto depende del hardware y la complejidad del procesamiento de cada evento.

---

### 8.3 Preguntas de Configuración

**P: ¿Dónde está el archivo de configuración?**
R: La biblioteca **no lee archivos de configuración**. La configuración se pasa
en `ChronosRT_config_t` al llamar a `ChronosRT_init()`. Si su aplicación usa un
archivo (`.conf`, `.ini`, JSON…), es ella quien lo carga y rellena la estructura.
La documentación incluye plantillas en `./config/`, `~/.config/chronosrt/` y
`/etc/chronosrt/` como referencia, no como mecanismo del framework.

---

**P: ¿Puedo cambiar la configuración mientras ChronosRT Master está ejecutándose?**
R: No. `ChronosRT_config_t` se consume en `ChronosRT_init()`; los cambios
requieren cerrar (`ChronosRT_shutdown`) y volver a inicializar. Los parámetros de
red que sí son ajustables en caliente tienen su propia API (por ejemplo
`ChronosRT_et_master_set_qos`, `set_fec`, `set_tx_mode`).

---

**P: ¿Qué pasa si no tengo archivo de configuración?**
R: ChronosRT Master usa valores por defecto que funcionan en la mayoría de sistemas. Los valores por defecto están optimizados para un equilibrio entre rendimiento y uso de recursos.

---

### 8.4 Preguntas de Solución de Problemas

**P: El programa compila pero falla al ejecutar**
R:
1. Verifica que tienes las bibliotecas necesarias: `ldd ./build/chronosrt_test`
2. Verifica permisos de ejecución: `ls -la ./build/chronosrt_test`
3. Intenta ejecutar con más información: `LD_DEBUG=libs ./build/chronosrt_test`

---

**P: Tengo error de "permission denied"**
R:
```bash
# Dar permisos de ejecución
chmod +x ./build/chronosrt_test

# Si el problema es de red, puede necesitar privilegios:
sudo ./build/chronosrt_bench --mode=throughput
```

---

## 9. Glosario de Términos

### Términos Técnicos

| Término | Definición |
|---------|------------|
| **API** | Application Programming Interface - Conjunto de funciones para interagir con un programa |
| **Cache Line** | Unidad de datos transferida entre memoria principal y caché de CPU (típicamente 64 bytes) |
| **CAS** | Compare-And-Swap - Operación atómica usada en programación lock-free |
| **CMake** | Herramienta de construcción de software multiplataforma |
| **Cross-compilation** | Compilar código para una plataforma diferente a la actual |
| **Determinista** | Comportamiento predecible con tiempo de ejecución conocido |
| **False Sharing** | Efecto donde dos cores modifican datos en la misma cache line |
| **Framework** | Estructura de software que proporciona funcionalidades base |
| **Hugepages** | Páginas de memoria grandes (2MB) que reducen TLB misses |
| **Jitter** | Variación en el tiempo de respuesta de un sistema |
| **Latencia** | Tiempo que tarda un dato en viajar de un punto a otro |
| **Lock-free** | Algoritmos que evitan bloqueos de mutex usando operaciones atómicas |
| **MPMC** | Multi-Producer Multi-Consumer - Cola que múltiples threads pueden usar |
| **Mutex** | Mecanismo de sincronización que previene acceso simultáneo a recursos |
| **NUMA** | Non-Uniform Memory Access - Arquitectura de memoria donde acceso a memoria tiene costos diferentes según ubicación |
| **PREEMPT_RT** | Parche para el kernel Linux que permite preemption casi total |
| **SIMD** | Single Instruction Multiple Data - Instrucciones que procesan múltiples datos simultáneamente |
| **SPSC** | Single-Producer Single-Consumer - Cola para un productor y un consumidor |
| **Syscall** | Llamada al sistema operativo desde espacio de usuario |
| **Throughput** | Cantidad de trabajo realizado por unidad de tiempo |
| **TLB** | Translation Lookaside Buffer - Memoria cache de mapeo de direcciones virtuales a físicas |
| **WCET** | Worst-Case Execution Time - Tiempo máximo de ejecución en el peor caso |

### Acrónimos

| Acrónimo | Significado |
|----------|-------------|
| ARM64 | Arquitectura de procesador ARM de 64 bits |
| CPU | Central Processing Unit |
| DMA | Direct Memory Access |
| ESC | EtherCAT Slave Controller - Controlador de la esclava EtherCAT |
| EtherCAT | Red industrial determinista sobre Ethernet (IEC 61158 / ETG.1000) |
| GPIO | General Purpose Input/Output |
| I2C | Inter-Integrated Circuit |
| MAC | Media Access Control - Dirección física de interfaz de red |
| MMIO | Memory-Mapped I/O - Técnica para comunicación con hardware |
| MTU | Maximum Transmission Unit - Tamaño máximo de paquete Ethernet |
| NIC | Network Interface Card - Tarjeta de red |
| NUMA | Non-Uniform Memory Access |
| OS | Operating System |
| PCB | Printed Circuit Board |
| PHY | Physical Layer - Capa física de red |
| RAM | Random Access Memory |
| RX | Receive - Recepción de datos |
| SBC | Single Board Computer |
| SPI | Serial Peripheral Interface |
| TSO | TCP Segmentation Offload - Descarga de segmentación TCP al hardware |
| TX | Transmit - Transmisión de datos |
| UART | Universal Asynchronous Receiver-Transmitter |
| USB | Universal Serial Bus |
| VLAN | Virtual LAN - Red local virtual |
| WKC | Working Counter - Contador usado en EtherCAT para verificar procesamiento |

---

## 10. Maestro EtherCAT: Uso y Puesta en Marcha

### 10.1 ¿Qué es el maestro EtherCAT?

ChronosRT Master habla con el bus industrial **EtherCAT** (IEC 61158 / ETG.1000) como **maestro**. La comunicación se realiza directamente sobre Ethernet crudo (sockets `AF_PACKET`, EtherType `0x88A4`), **sin depender de SOEM ni de ningún otro maestro externo**. Esto aporta las cuatro propiedades del proyecto:

| Propiedad | Cómo se consigue |
|---|---|
| **Tiempo real** | Todo el ciclo de proceso está pre-asignado; no hay `malloc` en el camino caliente |
| **Determinismo** | Temporización monotónica (`CLOCK_MONOTONIC`), timeouts acotados, sin esperas indefinidas |
| **Resiliencia** | Validación por contador de trabajo (WKC), detección de esclavas perdidas, reintentos acotados y redundancia de red (doble NIC) |
| **Ultra rendimiento** | Varios datagramas por trama (bit M), buffers indexados y cero copias superfluas |

> ChronosRT Master ofrece **dos protocolos de red seleccionables**: EtherCAT (este apartado) y
> **EtherTux** (apartado 11), pensado para Ethernet comercial estándar. No existen modos
> "raw" ni "DPDK".

### 10.2 Requisitos del sistema

1. **Interfaz de red Ethernet dedicada.** El maestro usa la interfaz en modo promiscuo; normalmente es una NIC distinta de la que usa el sistema para conectividad.
2. **Permisos.** Abrir un socket `AF_PACKET` requiere privilegios; ejecuta como `root` o con la capacidad `CAP_NET_RAW`:

```bash
# Opción A: ejecutar como root
sudo ./build/chronosrt_bench

# Opción B: conceder solo la capacidad necesaria al binario
sudo setcap cap_net_raw,cap_net_admin+ep ./build/chronosrt_bench
```

3. **Kernel Linux.** El maestro funciona sobre cualquier kernel moderno (5.15+ y 7.x). No requiere módulos propios.

### 10.3 Enumerar las interfaces disponibles

Antes de abrir el maestro conviene saber qué interfaces existen y cuáles están activas:

```c
#include <chronosrt/chronosrt_ethercat.h>
#include <stdio.h>

uint32_t n = ChronosRT_ec_interface_count();
for (uint32_t i = 0; i < n; i++) {
    const ChronosRT_ec_interface_t* itf = ChronosRT_ec_interface(i);
    if (!itf) continue;
    printf("[%u] %-16s MAC %02x:%02x:%02x:%02x:%02x:%02x  %s\n",
           i, itf->name,
           itf->mac[0], itf->mac[1], itf->mac[2],
           itf->mac[3], itf->mac[4], itf->mac[5],
           itf->is_up ? "UP" : "DOWN");
}
```

`ChronosRT_ec_interface_index("enp2s0")` devuelve el índice de una interfaz concreta (o `-1`). El maestro se abre sobre la interfaz **activa** (por ejemplo `enp2s0`); una interfaz `DOWN` no servirá.

### 10.4 Abrir el maestro e inicializar el bus

La puesta en marcha sigue siempre la misma secuencia: **abrir → escanear → configurar → mapear**.

```c
#include <chronosrt/chronosrt_ethercat.h>
#include <stdio.h>

int main(void) {
    ChronosRT_ec_master_t* m = ChronosRT_ec_master_create();
    if (!m) { fprintf(stderr, "sin memoria\n"); return 1; }

    /* 1. Abrir la interfaz (AF_PACKET, EtherType 0x88A4). */
    ChronosRT_status_t st = ChronosRT_ec_master_open(m, "enp2s0");
    if (st != ChronosRT_SUCCESS) {
        fprintf(stderr, "no se pudo abrir enp2s0: %d\n", (int)st);
        ChronosRT_ec_master_destroy(m);
        return 1;
    }

    /* 2. Escanear el bus: descubre las esclavas por auto-incremento. */
    int slaves = ChronosRT_ec_master_scan(m);
    if (slaves <= 0) {
        fprintf(stderr, "no se detectaron esclavas (%d)\n", slaves);
        ChronosRT_ec_master_close(m);
        ChronosRT_ec_master_destroy(m);
        return 1;
    }
    printf("Esclavas detectadas: %d\n", slaves);

    /* 3. Configurar estación, SII/EEPROM y buzón de cada esclava. */
    if (ChronosRT_ec_master_config_init(m) < 0) {
        fprintf(stderr, "config_init falló\n");
    }

    /* 4. Configurar SM/FMMU y construir el mapa de proceso (IOMap). */
    if (ChronosRT_ec_master_config_map(m) < 0) {
        fprintf(stderr, "config_map falló\n");
    }

    printf("IOMap: %u bytes\n", ChronosRT_ec_master_iomap_size(m));

    /* ... ciclo de proceso (sección 10.6) ... */

    ChronosRT_ec_master_close(m);
    ChronosRT_ec_master_destroy(m);
    return 0;
}
```

Con redundancia de red (NIC primaria + secundaria, para tolerancia a fallos de cable o puerto):

```c
ChronosRT_ec_master_open_redundant(m, "enp2s0", "enp3s0");
```

### 10.5 Llevar las esclavas a OPERATIONAL

Tras el mapeo, las esclavas están en estado `INIT`. El cambio de estado se hace por la máquina AL (Application Layer):

```c
/* Todas las esclavas a PRE-OP (buzón activo, PDOs aún no). */
ChronosRT_ec_master_request_all_states(m, ChronosRT_EC_STATE_PREOP, 2000000);

/* ... opcional: aquí se configuran los PDOs por CoE (sección 10.8) ... */

/* Todas las esclavas a SAFE-OP (entradas válidas, salidas en estado seguro). */
ChronosRT_ec_master_request_all_states(m, ChronosRT_EC_STATE_SAFEOP, 2000000);

/* Todas las esclavas a OP (intercambio de PDO completo). */
int rc = ChronosRT_ec_master_request_all_states(m, ChronosRT_EC_STATE_OP, 2000000);
if (rc < 0) {
    fprintf(stderr, "no se alcanzó OP en todas las esclavas\n");
}
```

Para una esclava concreta, `ChronosRT_ec_master_request_state(m, adp, estado, timeout_us)` solicita el estado y espera a alcanzarlo. `ChronosRT_ec_master_read_state()` lee el estado y el código de error actuales. Si una esclava no alcanza el estado pedido, el sistema **degrada elegantemente** (informa del fallo, no aborta).

### 10.6 Ciclo de datos de proceso (PDO)

En OPERATIONAL el intercambio se hace en dos fases con el IOMap:

```c
uint32_t cycle_us = 1000;  /* 1 ms */
const uint8_t* iomap = ChronosRT_ec_master_iomap(m);
uint32_t       iomap_size = ChronosRT_ec_master_iomap_size(m);

/* Escribir las salidas (RxPDO) en el IOMap. La disposición de cada esclava
   está en slave->o_offset/slave->o_bytes, y las entradas en
   slave->i_offset/slave->i_bytes. */

while (running) {
    /* 1. Enviar la trama de proceso (LRW) con las salidas. */
    ChronosRT_ec_master_send_processdata(m);

    /* 2. Recibir la respuesta y validar el contador de trabajo esperado. */
    int wkc = ChronosRT_ec_master_receive_processdata(m, cycle_us);
    if (wkc < 0) {
        /* Trama perdida o WKC incorrecto: continuar el bucle sin bloquear. */
        continue;
    }

    /* 3. Leer las entradas (TxPDO) del IOMap y aplicar la lógica de control. */
    (void)iomap;
    (void)iomap_size;
    control_step();
}
```

El contador de trabajo esperado se calcula como `2 × esclavas_de_salida + 1 × esclavas_de_entrada`. `ChronosRT_ec_master_receive_processdata()` lo compara con el recibido y devuelve fallo si no coincide (indicador de esclava caída o cable roto).

### 10.7 Relojes distribuidos (DC)

Para sincronización de precisión entre esclavas:

```c
/* Mide retardos de propagación y compensa el offset del reloj del bus. */
ChronosRT_ec_master_config_dc(m);

/* Activar SYNC0 con periodo de 1 ms (ciclo_ns, shift_ns). */
ChronosRT_ec_master_sync0(m, /*index=*/1, /*act=*/true, 1000000, 0);

/* Tiempo de sistema DC leído del bus (ns desde 2000-01-01). */
int64_t t = ChronosRT_ec_master_dc_time(m);
```

`ChronosRT_ec_master_sync01()` permite activar SYNC0 y SYNC1 simultáneamente en una esclava.

### 10.8 Buzón / CoE (objetos SDO)

Lectura y escritura de objetos del diccionario de objetos (CANopen over EtherCAT):

```c
/* SDO upload: leer el objeto 0x1C32:01 (ciclo de sincronización). */
uint32_t size = sizeof(uint32_t);
uint32_t val  = 0;
ChronosRT_status_t st = ChronosRT_ec_master_sdo_read(
        m, /*slave_index=*/1, 0x1C32, 0x01, &val, &size, 100000);

/* SDO download: escribir un objeto. */
uint16_t newval = 1000;
ChronosRT_ec_master_sdo_write(m, 1, 0x1C32, 0x01, &newval, sizeof(newval), 100000);
```

Los objetos CoE se leen/escriben **siempre en estado PRE-OP o superior** (el buzón debe estar activo).

### 10.9 Diagnóstico y monitorización

```c
uint16_t wkc      = ChronosRT_ec_master_working_counter(m);   /* WKC del último ciclo */
uint16_t expected = ChronosRT_ec_master_expected_wkc(m);      /* WKC esperado          */
uint64_t sent     = ChronosRT_ec_master_frames_sent(m);       /* tramas enviadas       */
uint64_t lost     = ChronosRT_ec_master_frames_lost(m);       /* tramas perdidas       */

/* Estado de cada esclava descubierta. */
uint32_t n = ChronosRT_ec_master_slave_count(m);
for (uint32_t i = 0; i < n; i++) {
    const ChronosRT_ec_slave_t* s = ChronosRT_ec_master_slave(m, i);
    if (!s) continue;
    printf("Esclava %u  vendor 0x%08X  AL 0x%02X  lost=%d\n",
           s->position, s->vendor_id, s->al_state, s->is_lost);
}
```

### 10.10 Problemas comunes

#### No se detectan esclavas

```bash
# 1. Comprobar que la interfaz existe y está UP
ip link show enp2s0

# 2. Comprobar el enlace físico (carrier)
cat /sys/class/net/enp2s0/carrier      # debe ser 1

# 3. Verificar que el cable va a la entrada IN de la primera esclava
#    (EtherCAT es un anillo/segmento lineal; respeta la dirección)
```

#### Error de permisos al abrir el maestro

`ChronosRT_ec_master_open()` devuelve error si no hay privilegios para crear el socket `AF_PACKET`:

```bash
# Ejecutar con la capacidad adecuada
sudo setcap cap_net_raw,cap_net_admin+ep ./build/chronosrt_bench
```

#### El contador de trabajo (WKC) no coincide

Síntoma típico de **esclava caída** o **cable roto**. Revisa `frames_lost` y el campo `is_lost` de cada esclava. El maestro continúa funcionando (resiliencia) y reporta el fallo sin bloquearse.

#### No se alcanza OPERATIONAL

Consulta el código de estado AL de la esclava que no llega:

```c
uint8_t state; uint16_t code;
ChronosRT_ec_master_read_state(m, adp, &state, &code);
/* `code` (registro 0x0134) describe la causa exacta del error AL. */
```

### 10.11 Buenas prácticas

1. **Dedica una NIC exclusiva** al bus EtherCAT; no compartas interfaz con el tráfico del sistema.
2. **Usa redundancia** (`open_redundant`) en entornos donde un fallo de cable no es aceptable.
3. **Pre-asigna todo** antes del bucle de proceso: el camino caliente no reserva memoria.
4. **Aísla el hilo del ciclo** con `isolcpus` y fija afinidad con `taskset` para minimizar el jitter.
5. **Activa DC** cuando necesites sincronización de sub-microsegundo entre esclavas.
6. **Monitoriza `frames_lost` y el WKC** en cada ciclo; son los indicadores tempranos de degradación del bus.
7. **Configura los PDOs por CoE en PRE-OP** antes de pasar a SAFE-OP/OP.

---

## 11. EtherTux: Ethernet comercial determinista

### 11.1 ¿Cuándo usar EtherTux en lugar de EtherCAT?

EtherTux habla **Ethernet estándar** (EtherType `0x88B5`), sin ASIC ni FPGA dedicados. Es la
opción cuando el despliegue usa:

- switches y NICs comerciales,
- SBC heterogéneas (Raspberry Pi, Orange Pi, Radxa, Banana Pi…),
- MCUs con controlador Ethernet SPI/QSPI (W5500 / W6100 / W6300),
- una red **exclusiva** para el lazo de control.

Si dispones de esclavas con hardware EtherCAT dedicado y necesitas ciclos < 100 µs, usa el maestro
EtherCAT del apartado 10. **Nunca** se usan ambos sobre la misma interfaz.

**Novedades de EtherTux v0.1.0:** cabecera de **26 bytes** en el cable (campos
`State`, `ET-WKC` y `FlowCount` incluidos) y doce tipos de mensaje. Además de
QoS, planificador y FEC, el maestro incorpora:

| Módulo | Función |
|---|---|
| **ET-ESM** | Ciclo de vida del nodo `INIT → CONFIG → SAFE → OP → FAULT` |
| **ET-Sync** | Sincronización aproximada (1–2 µs) con modelo de retardo del switch y monitor de ventana |
| **ET-WKC / topología** | Working Counter por trama, detección de cambios topológicos y Hot Connect |
| **ET-MBX** | Mailbox fiable para configuración, diagnóstico y firmware (P4/P5) |
| **FRM** | Redundancia lógica de flujo y deduplicación por `(nodo, SeqNum)` |
| **Flow Aggregator** | Agrupa varios flujos en una trama (`FlowCount > 1`) |

Cada módulo se asocia al maestro con `ChronosRT_et_master_attach_*()`. La
transmisión puede usar llamadas al sistema (`sendmmsg`, hasta 16 tramas) o un
anillo `PACKET_TX_RING`; selección con `ChronosRT_et_master_set_tx_mode()`.


### 11.2 Elegir el protocolo

```bash
# Variable de entorno leída al arrancar la aplicación
export CHRONOSRT_PROTOCOL=ethertux
```

```c
#include <chronosrt/chronosrt_protocol.h>

ChronosRT_protocol_t p = ChronosRT_protocol_from_env();   /* o from_string("ethertux") */
```

### 11.3 Puesta en marcha

```c
#include <chronosrt/chronosrt_ethertux.h>

ChronosRT_et_master_t* m = ChronosRT_et_master_create();

/* 1 Gbps y superframe de 1 ms; ajusta a tu enlace real */
if (ChronosRT_et_master_open_ex(m, "enp2s0", 1000000000ull, 1000000u) != ChronosRT_SUCCESS) {
    /* sin CAP_NET_RAW o interfaz caída: revisa 11.7 */
}

/* Alternativa: detecta la velocidad real del enlace (1G/2.5G/5G/10G) */
/* ChronosRT_et_master_open_auto(m, "enp2s0"); */
```

Permisos (igual que EtherCAT):

```bash
sudo setcap cap_net_raw,cap_net_admin+ep ./mi_aplicacion
```

### 11.4 Prioridades y control de admisión

| Nivel | Uso típico |
|---|---|
| P0 | Parada de emergencia, ACK/NACK crítico |
| P1 | Sincronización, heartbeat, mensajes LAPM |
| P2 | Setpoints de control |
| P3 | Telemetría |
| P4 | Configuración y consultas |
| P5 | Logs, firmware, transferencias masivas |

Cada envío consulta el control de admisión. Es **obligatorio** atender el resultado:

```c
ChronosRT_status_t st = ChronosRT_et_master_send(m, ChronosRT_ET_P0,
                                                 ChronosRT_ET_TYPE_CONTROL, 1u,
                                                 msg, sizeof(msg));
if (st == ChronosRT_ERROR_BACKPRESSURE) {
    /* reduce la tasa del productor; la trama NO se ha perdido */
} else if (st == ChronosRT_ERROR_DROPPED) {
    /* solo puede ocurrir en P5 (best-effort) */
}
```

P0–P4 **nunca se descartan**; P5 sí (drop-tail). Puedes consultar la presión antes de generar
tráfico con `ChronosRT_et_qos_backpressure()`.

### 11.5 Planificador time-triggered

```c
ChronosRT_et_sched_t* s = ChronosRT_et_sched_create(1000000u, 1000000000ull);
/* Ventanas por defecto: 100 / 200 / 300 / 400 µs */
ChronosRT_et_sched_add_slot(s, ChronosRT_ET_P2, /*node=*/3u, /*bytes=*/128u, /*offset=*/120000u);
ChronosRT_et_master_attach_scheduler(m, s);
```

Mientras haya planificador asociado, cada prioridad solo puede transmitir en su ventana (P0 es la
excepción: es express y puede transmitir siempre). Fuera de ventana el envío devuelve
`ChronosRT_ERROR_BACKPRESSURE`.

### 11.6 FEC Reed-Solomon (recuperación sin retransmisión)

```c
ChronosRT_et_master_set_fec(m, ChronosRT_ET_FEC_LIGHT);    /* RS(10,8): 25 % overhead  */
/* ChronosRT_ET_FEC_ROBUST -> RS(16,8): 100 % de overhead, 8 pérdidas por bloque */

const uint8_t* bloques[8] = { d0, d1, d2, d3, d4, d5, d6, d7 };   /* 8 datos */
ChronosRT_et_master_send_fec_block(m, ChronosRT_ET_P2, 1u, bloques, 8u, len);
```

El receptor reconstruye los bloques perdidos antes de entregarlos a la aplicación. La FEC no
depende del RTT: no hay retransmisión y la latencia no se degrada.

### 11.7 Problemas comunes

**`open` devuelve error**

```bash
ip link show enp2s0            # debe existir y estar UP
cat /sys/class/net/enp2s0/carrier   # 1 = cable conectado
sudo setcap cap_net_raw,cap_net_admin+ep ./mi_aplicacion
```

**Todos los envíos devuelven `BACKPRESSURE`**

- La ventana actual no corresponde a esa prioridad → revisa el planificador (11.5).
- La clase está saturada → reduce la tasa o sube `rate_bytes_per_sec` en la configuración QoS.

**Pérdidas de paquetes**

Activa FEC (`ChronosRT_ET_FEC_LIGHT` o `ROBUST`) y revisa que el switch no esté aplicando
`storm-control` ni descartes por búfer.

**Jitter alto**

- Dedica la interfaz en exclusiva a EtherTux.
- Aísla el hilo del ciclo (`isolcpus`, `nohz_full`) y fija afinidad.
- Comprueba `ChronosRT_hw_caps_for_interface()`: si la NIC soporta PHC/Launch Time, verás
  `phc = true` y el envío con `ChronosRT_et_master_send_at()` aplicará pacing por hardware.

**Rendimiento por MCU**

| Controlador | Throughput real | Uso recomendado |
|---|---|---|
| W5500 (SPI) | ~25–55 Mbps | P3/P4 (telemetría) |
| W6100 | ~50 Mbps (Fast SPI) / ~91 Mbps (QSPI) | P2–P4 |
| W6300 (QSPI, 64 KB) | ~91 Mbps | P0–P2 (control de movimiento) |

### 11.8 Diagnóstico

```c
ChronosRT_et_master_stats_t st;
ChronosRT_et_master_stats(m, &st);
/* frames_tx, frames_rx, dropped, backpressure, fec_blocks, frame_errors */

uint32_t n = ChronosRT_et_master_node_count(m);
ChronosRT_et_node_t node;
for (uint32_t i = 0; i < n; i++) {
    if (ChronosRT_et_master_node(m, i, &node)) {
        printf("nodo %u  rtt=%u ns  offset=%lld ns  lost=%d\n",
               node.id, node.rtt_ns, (long long)node.clock_offset_ns, node.lost);
    }
}

/* Marca caídos los nodos sin heartbeat en 3 superframes y libera sus ranuras */
ChronosRT_et_master_heartbeat(m, ChronosRT_tx_timestamp_ns());
```

---

## Apéndice A: Contacto y Soporte

Para soporte técnico:
- **Email:** support@chronosrt.example.com
- **Documentación:** https://docs.chronosrt.example.com
- **GitHub Issues:** https://github.com/LauOtero/ChronosRT_MASTER/issues
- **Foro de comunidad:** https://forum.chronosrt.example.com

## Apéndice B: Licencia

ChronosRT Master está bajo la **GNU Affero General Public License v3.0
(AGPL-3.0)**. Ver el archivo [LICENSE](../LICENSE) para el texto completo.

---

## Apéndice C: Guía Rápida de Comandos

### C.1 Comandos Esenciales

```bash
# Compilar proyecto
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)

# Ejecutar tests
ctest --test-dir build --output-on-failure

# Ejecutar una suite concreta
./build/chronosrt_test

# Ejecutar benchmark de latencia
./chronosrt_bench --mode=latency --duration=30

# Ejecutar benchmark de throughput
./chronosrt_bench --mode=throughput --duration=60 --threads=4

# Análisis con perf
perf stat ./chronosrt_bench --mode=latency --duration=30
```

### C.2 Comandos de Diagnóstico

```bash
# Verificar kernel PREEMPT_RT
zcat /proc/config.gz | grep PREEMPT_RT

# Ver CPUs disponibles
lscpu

# Ver hugepages
cat /proc/meminfo | grep Huge

# Ver temperatura (RPi/Jetson)
cat /sys/class/thermal/thermal_zone0/temp

# Ver governor de CPU
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### C.3 Comandos de Mantenimiento

```bash
# Limpiar build
rm -rf build/

# Instalar en sistema
sudo make install

# Desinstalar
sudo rm -rf /usr/local/lib/libchronosrt.so*
sudo rm -rf /usr/local/include/chronosrt

# Respaldar configuración (plantillas de su aplicación)
tar -czf chronosrt_backup_$(date +%Y%m%d).tar.gz \
    /etc/chronosrt ~/.config/chronosrt
```

---

## Apéndice D: Ejemplos de Código Completos

### D.1 Aplicación Mínima Funcional

```c
#include <chronosrt/chronosrt.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    // 1. Configurar
    ChronosRT_config_t config = {0};
    config.event_pool_size = 8192;
    config.use_simd = true;
    
    // 2. Inicializar
    ChronosRT_handle_t handle;
    if (ChronosRT_init(&handle, &config) != ChronosRT_SUCCESS) {
        fprintf(stderr, "Init failed\\n");
        return 1;
    }
    
    printf("ChronosRT Master initialized successfully!\\n");
    
    // 3. Publicar eventos
    for (int i = 0; i < 100; i++) {
        uint32_t data = i;
        ChronosRT_event_publish(handle, &data, sizeof(data));
    }
    printf("Published 100 events\\n");
    
    // 4. Suscribirse a eventos
    ChronosRT_event_t event;
    int received = 0;
    while (received < 100) {
        if (ChronosRT_event_subscribe(handle, &event, 1000) == 
            ChronosRT_SUCCESS) {
            received++;
        }
    }
    printf("Received %d events\\n", received);
    
    // 5. Shutdown
    ChronosRT_shutdown(handle);
    printf("ChronosRT Master shutdown complete\\n");
    
    return 0;
}
```

**Compilación:**
```bash
gcc -o ejemplo_minimo ejemplo_minimo.c \
    -I/usr/local/include \
    -L/usr/local/lib -lchronosrt -lpthread -lm -ldl
```

### D.2 Aplicación con Benchmarking

```c
#include <chronosrt/chronosrt.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    // Configuración optimizada para benchmark
    ChronosRT_config_t config = {0};
    config.event_pool_size = 131072;  // 128K eventos
    config.batch_size = 512;
    config.use_simd = true;
    config.use_hugepages = true;
    config.use_cpu_affinity = true;
    config.enabled_cpus[0] = 2;
    config.enabled_cpus[1] = 3;
    config.num_cpus = 2;
    
    // Inicializar
    ChronosRT_handle_t handle;
    if (ChronosRT_init(&handle, &config) != ChronosRT_SUCCESS) {
        fprintf(stderr, "Failed to initialize\\n");
        return 1;
    }
    
    printf("Running latency benchmark...\\n");
    
    // Ejecutar benchmark
    ChronosRT_metrics_t metrics;
    if (ChronosRT_benchmark_run(handle, 
                                        ChronosRT_MODE_LATENCY,
                                        30,  // 30 segundos
                                        &metrics) != ChronosRT_SUCCESS) {
        fprintf(stderr, "Benchmark failed\\n");
        ChronosRT_shutdown(handle);
        return 1;
    }
    
    // Reportar resultados
    ChronosRT_metrics_report(&metrics);
    
    // Evaluar resultado
    if (metrics.stddev_ns < 1000) {
        printf("✓ EXCELLENT: Jitter < 1 μs\\n");
    } else if (metrics.stddev_ns < 5000) {
        printf("✓ GOOD: Jitter < 5 μs\\n");
    } else {
        printf("✗ NEEDS OPTIMIZATION: Jitter >= 5 μs\\n");
    }
    
    ChronosRT_shutdown(handle);
    return 0;
}
```

### D.3 Aplicación Multi-thread

```c
#include <chronosrt/chronosrt.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 2

ChronosRT_handle_t g_handle;
volatile int g_running = 1;

void* producer_thread(void* arg) {
    int id = *(int*)arg;
    uint64_t count = 0;
    
    while (g_running) {
        uint64_t data = (id << 48) | count;
        if (ChronosRT_event_publish(g_handle, &data, sizeof(data)) == 
            ChronosRT_SUCCESS) {
            count++;
        }
        usleep(100);  // 100 μs entre eventos
    }
    
    printf("Producer %d: published %lu events\\n", id, count);
    return NULL;
}

void* consumer_thread(void* arg) {
    int id = *(int*)arg;
    uint64_t count = 0;
    ChronosRT_event_t event;
    
    while (g_running) {
        if (ChronosRT_event_subscribe(g_handle, &event, 1000) == 
            ChronosRT_SUCCESS) {
            count++;
            // Procesar evento...
        }
    }
    
    printf("Consumer %d: received %lu events\\n", id, count);
    return NULL;
}

int main(void) {
    pthread_t producers[NUM_PRODUCERS];
    pthread_t consumers[NUM_CONSUMERS];
    int producer_ids[NUM_PRODUCERS];
    int consumer_ids[NUM_CONSUMERS];
    
    // Inicializar ChronosRT Master
    ChronosRT_config_t config = {0};
    config.event_pool_size = 65536;
    ChronosRT_init(&g_handle, &config);
    
    // Crear threads productores
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producer_ids[i] = i;
        pthread_create(&producers[i], NULL, producer_thread, &producer_ids[i]);
    }
    
    // Crear threads consumidores
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        consumer_ids[i] = i;
        pthread_create(&consumers[i], NULL, consumer_thread, &consumer_ids[i]);
    }
    
    // Esperar 10 segundos
    sleep(10);
    g_running = 0;
    
    // Unir threads
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producers[i], NULL);
    }
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        pthread_join(consumers[i], NULL);
    }
    
    ChronosRT_shutdown(g_handle);
    return 0;
}
```

---

## Apéndice E: Solución de Problemas Comunes

### E.1 Problemas de Compilación

**Error: "immintrin.h: No such file or directory"**

**Causa:** Faltan headers de SIMD en x86_64.

**Solución:**
```bash
# Ubuntu/Debian
sudo apt install gcc-multilib

# O deshabilitar SIMD
cmake .. -DCHRONOSRT_USE_SIMD=OFF
```

**Error: "arm_neon.h: No such file or directory"**

**Causa:** Cross-compilación sin toolchain correcto.

**Solución:**
```bash
sudo apt install gcc-aarch64-linux-gnu
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-linux-gnu.cmake
```

### E.2 Problemas de Ejecución

**Error: "Cannot allocate memory"**

**Causas:**
1. Pool de eventos muy grande
2. Memoria insuficiente
3. Límite de ulimit

**Soluciones:**
```bash
# Reducir pool size
export ChronosRT_EVENT_POOL_SIZE=16384

# Verificar memoria
free -h

# Aumentar límites
ulimit -v unlimited
```

**Error: "Operation not permitted" al usar SCHED_FIFO**

**Causa:** Se necesitan privilegios de root para scheduling real-time.

**Soluciones:**
```bash
# Ejecutar como root
sudo ./chronosrt_bench

# O configurar capabilities
sudo setcap cap_sys_nice=eip ./chronosrt_bench

# O ajustar limits
echo \"@user - rtprio 99\" >> /etc/security/limits.conf
```

### E.3 Problemas de Rendimiento

**Latencia mayor a la esperada**

**Checklist:**
1. ✓ Verificar PREEMPT_RT
2. ✓ Aislar CPUs con isolcpus
3. ✓ Usar governor performance
4. ✓ Habilitar hugepages
5. ✓ Desactivar servicios innecesarios

```bash
# Script de verificación rápida
#!/bin/bash
echo \"=== Quick Performance Check ===\"
echo -n \"PREEMPT_RT: \"
zcat /proc/config.gz 2>/dev/null | grep -q PREEMPT_RT && echo \"YES\" || echo \"NO\"
echo -n \"CPU Governor: \"
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo -n \"Hugepages: \"
grep HugePages_Total /proc/meminfo | awk '{print $2}'
```

**Jitter elevado (>5 μs)**

**Causas comunes:**
1. Interrupciones del sistema
2. Otras aplicaciones consumiendo CPU
3. Thermal throttling

**Soluciones:**
```bash
# Desactivar interrupciones en CPUs aisladas
echo 2 > /proc/irq/smp_affinity_list

# Mover procesos a otras CPUs
taskset -c 0,1 -p $(pgrep problematic_app)

# Verificar temperatura
watch -n1 cat /sys/class/thermal/thermal_zone0/temp
```

---

## Apéndice F: Recursos Adicionales

### F.1 Documentación Externa

- **Linux PREEMPT_RT:** https://wiki.linuxfoundation.org/realtime/start
- **ARM NEON Programming:** https://developer.arm.com/architectures/instruction-sets/simd-isas/neon
- **Intel AVX-512:** https://www.intel.com/content/www/us/en/develop/documentation/extension-reference-manual/home.html
- **Lock-free Programming:** https://preshing.com/blog/lockless-algorithms/

### F.2 Herramientas Recomendadas

| Herramienta | Propósito | URL |
|-------------|-----------|-----|
| perf | Perfilado Linux | Incluido en kernel |
| Valgrind | Detección memory leaks | https://valgrind.org |
| FlameGraph | Visualización profiling | https://github.com/brendangregg/FlameGraph |
| stress-ng | Pruebas de estrés | https://github.com/ColinIanKing/stress-ng |
| hwloc | Topología hardware | https://www.open-mpi.org/projects/hwloc/ |

### F.3 Libros Recomendados

1. **"Systems Performance"** - Brendan Gregg
   - Análisis profundo de rendimiento de sistemas Linux
   
2. **"Real-Time Concepts for Embedded Systems"** - Qing Li
   - Fundamentos de sistemas de tiempo real
   
3. **"C++ Concurrency in Action"** - Anthony Williams
   - Programación concurrente avanzada

### F.4 Comunidades y Foros

- **Stack Overflow:** Etiquetas [real-time], [embedded], [linux-kernel]
- **Reddit:** r/embedded, r/linux, r/programming
- **Linux Foundation Forums:** https://forums.linuxfoundation.org

---

**Fin del Manual de Usuario**

**Documento completado:** Noviembre 2026  
**Próxima revisión:** Noviembre 2027  
**Mantenido por:** Equipo de Desarrollo ChronosRT Master

