#!/usr/bin/env bash
# =============================================================================
# scripts/build.sh — Compilación de ChronosRT para las SBC de EtherTux
#
# Perfiles soportados: los de la matriz §7.5 de docs/Protocolo EtherTux.md
# (Raspberry Pi, Orange Pi, Radxa, Banana Pi, Libre Computer, Jetson, x86-64).
#
# Ejemplos:
#   ./scripts/build.sh --list                     # ver todas las SBC
#   ./scripts/build.sh --detect                   # detectar esta máquina
#   ./scripts/build.sh --sbc rpi5 --release       # cross-compilar para RPi 5
#   ./scripts/build.sh --sbc bpi-m2-zero          # ARMv7 (sin CRC32 HW)
#   ./scripts/build.sh --sbc x86-64 --native      # compilación nativa
#   ./scripts/build.sh --sbc opi-5 --print-config # ver flags sin compilar
#   ./scripts/build.sh all                        # matriz representativa
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
# shellcheck source=lib/sbc.sh
source "$SCRIPT_DIR/lib/sbc.sh"

BUILD_TYPE="RelWithDebInfo"
SBC_ID=""
CLEAN=0
JOBS="$(nproc 2>/dev/null || echo 4)"
OUTPUT_ROOT="${PROJECT_ROOT}/build"
TOOLCHAIN=""
SYSROOT=""
CROSS_MODE="auto"          # auto | cross | native
USE_SIMD="auto"
USE_HUGEPAGES=0
USE_LTO=""
USE_PROTOCOLS=""
USE_TESTS=""
USE_BENCH=""
NATIVE_CODEGEN=0
EXTRA_CFLAGS=""
DRY_RUN=0
ACTION="build"             # build | list | detect | print-config

usage() {
    cat <<'EOF'
ChronosRT build — compilación nativa y cruzada para SBC

Uso:
  build.sh [--sbc ID | --detect | --list | all] [opciones]

Selección de objetivo:
  --sbc ID         Perfil de SBC (ver --list)
  --detect         Detectar automáticamente la SBC en la que se ejecuta
  --list           Listar perfiles soportados y salir
  --print-config   Mostrar la configuración resuelta sin compilar
  all              Matriz representativa: rpi5 + bpi-m2-zero + x86-64

Compilación:
  --release | --debug     Optimización (defecto: RelWithDebInfo)
  --native                Forzar compilación nativa (sin toolchain)
  --cross                 Forzar compilación cruzada
  --toolchain FILE        Toolchain de CMake explícito
  --sysroot DIR           Sysroot para la compilación cruzada
  --cflags "FLAGS"        Flags C adicionales
  --native-codegen        -DCHRONOSRT_ENABLE_NATIVE=ON (solo build nativo)
  --jobs N                Paralelismo (defecto: nproc)
  --dir DIR               Directorio raíz de build (defecto: ./build)
  --clean                 Borra el directorio de build del objetivo
  --dry-run               Muestra los comandos sin ejecutarlos

Módulos:
  --simd | --no-simd            SIMD (defecto: auto)
  --hugepages                   Soporte de hugepages
  --lto | --no-lto              Link-time optimization
  --protocols | --no-protocols  EtherCAT + EtherTux
  --tests | --no-tests          Suites de test
  --benchmark | --no-benchmark  Ejecutable de benchmark
  -h, --help                    Esta ayuda
EOF
}

log()  { printf '%s\n' "$*"; }
warn() { printf 'AVISO: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# --- Parseo de argumentos ----------------------------------------------------
# Se admite el estilo antiguo (posición 1 = plataforma) por compatibilidad.
if [ $# -gt 0 ] && [[ "$1" != -* ]]; then
    case "$1" in
        all|matrix) ACTION="matrix" ;;
        *)          SBC_ID="$1" ;;
    esac
    shift
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --sbc)            SBC_ID="${2:-}"; shift 2 ;;
        --detect)         ACTION="detect"; shift ;;
        --list)           ACTION="list"; shift ;;
        --print-config)   ACTION="print-config"; shift ;;
        --release)        BUILD_TYPE="Release"; shift ;;
        --debug)          BUILD_TYPE="Debug"; shift ;;
        --clean)          CLEAN=1; shift ;;
        --jobs)           JOBS="${2:-}"; shift 2 ;;
        --dir)            OUTPUT_ROOT="${2:-}"; shift 2 ;;
        --toolchain)      TOOLCHAIN="${2:-}"; shift 2 ;;
        --sysroot)        SYSROOT="${2:-}"; shift 2 ;;
        --native)         CROSS_MODE="native"; shift ;;
        --cross)          CROSS_MODE="cross"; shift ;;
        --simd)           USE_SIMD="ON"; shift ;;
        --no-simd)        USE_SIMD="OFF"; shift ;;
        --hugepages)      USE_HUGEPAGES=1; shift ;;
        --lto)            USE_LTO="ON"; shift ;;
        --no-lto)         USE_LTO="OFF"; shift ;;
        --protocols)      USE_PROTOCOLS="ON"; shift ;;
        --no-protocols)   USE_PROTOCOLS="OFF"; shift ;;
        --tests)          USE_TESTS="ON"; shift ;;
        --no-tests)       USE_TESTS="OFF"; shift ;;
        --benchmark)      USE_BENCH="ON"; shift ;;
        --no-benchmark)   USE_BENCH="OFF"; shift ;;
        --native-codegen) NATIVE_CODEGEN=1; shift ;;
        --cflags)         EXTRA_CFLAGS="${2:-}"; shift 2 ;;
        --dry-run)        DRY_RUN=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *)                die "opción desconocida: $1 (use --help)" ;;
    esac
done

# --- Acciones informativas ---------------------------------------------------
case "$ACTION" in
    list)
        log "SBC soportadas (docs/Protocolo EtherTux.md §7.5):"
        log ""
        while IFS= read -r id; do
            sbc_print "$id"
        done < <(sbc_ids)
        log ""
        log "Alias aceptados: rpi4, raspberry-pi-5, orange-pi5, rock5b, intel-nuc, ..."
        exit 0
        ;;
    detect)
        detected="$(sbc_detect)"
        if [ -z "$detected" ]; then
            log "No se ha identificado la SBC (arquitectura: $(uname -m))."
            log "Elija un perfil con --sbc (ver --list)."
            exit 0
        fi
        log "SBC detectada: $detected"
        sbc_print "$detected"
        exit 0
        ;;
esac

if [ -z "$SBC_ID" ] && [ "$ACTION" != "matrix" ]; then
    detected="$(sbc_detect)"
    if [ -n "$detected" ]; then
        SBC_ID="$detected"
        log "Objetivo autodetectado: $SBC_ID (use --sbc para forzarlo)"
    else
        usage
        die "indique un objetivo con --sbc (ver --list)"
    fi
fi

# --- Arquitectura del host ---------------------------------------------------
host_arch() {
    case "$(uname -m)" in
        x86_64|amd64)  echo x86_64 ;;
        aarch64|arm64) echo aarch64 ;;
        armv7l|armv6l) echo armv7 ;;
        *)             echo unknown ;;
    esac
}
HOST_ARCH="$(host_arch)"

# --- Resolución de flags y toolchain ----------------------------------------
resolve_flags() {
    local arch="$1"
    NEW_CROSS=0
    FLAGS="$SBC_MARCH $SBC_MCPU $SBC_MFLOAT"
    FLAGS="$(echo "$FLAGS $EXTRA_CFLAGS" | xargs)"

    if [ -n "$SYSROOT" ]; then
        FLAGS="$FLAGS --sysroot=$SYSROOT"
    fi

    case "$CROSS_MODE" in
        native) NEW_CROSS=0 ;;
        cross)  NEW_CROSS=1 ;;
        *)      [ "$arch" != "$HOST_ARCH" ] && NEW_CROSS=1 || NEW_CROSS=0 ;;
    esac

    # Sin cross-compilador disponible no se puede cruzar: se avisa y se aborta.
    # `--print-config` y `--dry-run` no compilan, así que no lo exigen.
    if [ "$NEW_CROSS" -eq 1 ] && [ -z "$TOOLCHAIN" ] && \
       [ "$ACTION" != "print-config" ] && [ "$DRY_RUN" -eq 0 ]; then
        local pkg
        pkg="$(sbc_cross_pkg "$arch")"
        local cc=""
        case "$arch" in
            aarch64) cc="aarch64-linux-gnu-gcc" ;;
            armv7)   cc="arm-linux-gnueabihf-gcc" ;;
            x86_64)  cc="x86_64-linux-gnu-gcc" ;;
        esac
        if [ -n "$cc" ] && ! command -v "$cc" >/dev/null 2>&1; then
            warn "no se encuentra '$cc' para cross-compilar a $arch."
            warn "instálelo con: sudo apt install $pkg"
            warn "o compile en la propia SBC con --native."
            exit 2
        fi
    fi
}

# Construye los argumentos de CMake para el objetivo cargado.
cmake_args() {
    local a=("-S" "$PROJECT_ROOT" "-DCMAKE_BUILD_TYPE=$BUILD_TYPE")
    a+=("-DCHRONOSRT_BUILD_PROTOCOLS=$PROTOCOLS_FINAL")
    a+=("-DCHRONOSRT_BUILD_TESTS=$TESTS_FINAL")
    a+=("-DCHRONOSRT_BUILD_BENCHMARK=$BENCH_FINAL")

    [ "$USE_SIMD" != "auto" ] && a+=("-DCHRONOSRT_USE_SIMD=$USE_SIMD")
    [ "$USE_HUGEPAGES" -eq 1 ] && a+=("-DCHRONOSRT_USE_HUGEPAGES=ON")
    [ -n "$USE_LTO" ] && a+=("-DCHRONOSRT_ENABLE_LTO=$USE_LTO")
    [ "$NATIVE_CODEGEN" -eq 1 ] && a+=("-DCHRONOSRT_ENABLE_NATIVE=ON")

    if [ "$NEW_CROSS" -eq 1 ]; then
        if [ -n "$TOOLCHAIN" ]; then
            a+=("-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN")
        else
            case "$SBC_ARCH" in
                aarch64)
                    if [ -f "$PROJECT_ROOT/cmake/aarch64-linux-gnu.cmake" ] && \
                       [ -z "$SYSROOT" ] && [ -d /usr/aarch64-linux-gnu ]; then
                        a+=("-DCMAKE_TOOLCHAIN_FILE=$PROJECT_ROOT/cmake/aarch64-linux-gnu.cmake")
                    else
                        a+=("-DCMAKE_SYSTEM_NAME=Linux" "-DCMAKE_SYSTEM_PROCESSOR=aarch64"
                            "-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc")
                    fi
                    ;;
                armv7)
                    a+=("-DCMAKE_SYSTEM_NAME=Linux" "-DCMAKE_SYSTEM_PROCESSOR=armv7l"
                        "-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc")
                    ;;
                x86_64)
                    a+=("-DCMAKE_SYSTEM_NAME=Linux" "-DCMAKE_SYSTEM_PROCESSOR=x86_64"
                        "-DCMAKE_C_COMPILER=x86_64-linux-gnu-gcc")
                    ;;
            esac
        fi
    fi

    # Tuning por microarquitectura (nunca en cross a otra arquitectura distinta).
    if [ -n "$FLAGS" ]; then
        a+=("-DCMAKE_C_FLAGS=$FLAGS")
    fi
    printf '%s\n' "${a[@]}"
}

# --- Compilación de un objetivo ---------------------------------------------
build_one() {
    local id="$1"
    sbc_load "$id" || die "perfil desconocido: $id"

    resolve_flags "$SBC_ARCH"

    PROTOCOLS_FINAL="${USE_PROTOCOLS:-ON}"
    TESTS_FINAL="${USE_TESTS:-ON}"
    BENCH_FINAL="${USE_BENCH:-ON}"

    local dir="$OUTPUT_ROOT/$SBC_ID"

    log "=============================================================="
    log " ChronosRT — $SBC_NAME ($SBC_ID)"
    log "=============================================================="
    log " SoC        : $SBC_SOC / $SBC_CPU ($SBC_ARCH, ${SBC_CORES} cores)"
    log " CRC32 HW   : $SBC_CRC32    Crypto: $SBC_CRYPTO"
    log " Build type : $BUILD_TYPE    Modo: $([ "$NEW_CROSS" -eq 1 ] && echo cruzado || echo nativo)"
    log " Flags      : ${FLAGS:-<por defecto>}"
    log " Directorio : $dir"
    [ -n "$SBC_NOTES" ] && log " Nota       : $SBC_NOTES"
    log ""

    if [ "$ACTION" = "print-config" ]; then
        log "cmake $(cmake_args | tr '\n' ' ')"
        log "cmake --build $dir --parallel $JOBS"
        log ""
        log "Runtime recomendado:"
        log "  cmdline : $(sbc_cmdline)"
        log "  entorno : CHRONOSRT_PROTOCOL=ethertux"
        if [ "$SBC_CRC32" = "no" ]; then
            log "  aviso   : sin CRC32 por instrucción; la HAL usará slice-by-8"
        fi
        return 0
    fi

    [ "$CLEAN" -eq 1 ] && rm -rf "$dir"
    mkdir -p "$dir"

    local -a args
    mapfile -t args < <(cmake_args)
    args+=("-B" "$dir")

    if [ "$DRY_RUN" -eq 1 ]; then
        log "cmake ${args[*]}"
        log "cmake --build $dir --parallel $JOBS"
        return 0
    fi

    cmake "${args[@]}"
    cmake --build "$dir" --parallel "$JOBS"

    log ""
    log "Compilación completada: $dir"
    if [ "$SBC_ARCH" = "$HOST_ARCH" ] || [ "$NEW_CROSS" -eq 0 ]; then
        log "Ejecute las pruebas con: ./scripts/test_runner.sh --sbc $SBC_ID"
    else
        log "Copie '$dir' a la SBC y ejecute: ./scripts/test_runner.sh --sbc $SBC_ID"
    fi
    log ""
    log "Checklist de tiempo real para $SBC_NAME:"
    log "  kernel cmdline: $(sbc_cmdline)"
    log "  governor      : performance"
    log "  IRQ Ethernet  : fijar a los núcleos aislados (chrt -f 90)"
    log "  protocolo     : export CHRONOSRT_PROTOCOL=ethertux"
    [ "$SBC_CRC32" = "no" ] && log "  CRC32         : fallback software (slice-by-8)"
    [ -n "$SBC_NOTES" ] && log "  nota          : $SBC_NOTES"
    log ""
}

# --- Ejecución ---------------------------------------------------------------
case "$ACTION" in
    matrix)
        # Un objetivo por arquitectura; se omiten los que no tengan compilador.
        for id in rpi5 bpi-m2-zero x86-64; do
            sbc_load "$id" || continue
            pkg="$(sbc_cross_pkg "$SBC_ARCH")"
            cc=""
            case "$SBC_ARCH" in
                aarch64) cc="aarch64-linux-gnu-gcc" ;;
                armv7)   cc="arm-linux-gnueabihf-gcc" ;;
                x86_64)  cc="cc" ;;
            esac
            if [ "$SBC_ARCH" != "$HOST_ARCH" ] && ! command -v "$cc" >/dev/null 2>&1; then
                warn "omitido $id: falta '$cc' (sudo apt install $pkg)"
                continue
            fi
            build_one "$id"
        done
        ;;
    build|print-config)
        build_one "$SBC_ID"
        ;;
    *)
        die "acción no soportada: $ACTION"
        ;;
esac
