#!/usr/bin/env bash
# =============================================================================
# scripts/test_runner.sh — Pruebas y auditoría de tiempo real por SBC
#
# Ejecuta las 8 suites (vía ctest), los benchmarks y una auditoría del entorno
# adaptada al perfil de la SBC (docs/Protocolo EtherTux.md §7.5 y §10).
#
# Ejemplos:
#   ./scripts/test_runner.sh --sbc rpi5
#   ./scripts/test_runner.sh --detect --mode full --duration 30
#   ./scripts/test_runner.sh --sbc bpi-m2-zero --mode quick
#   ./scripts/test_runner.sh --sbc x86-64 --ctest-only
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
# shellcheck source=lib/sbc.sh
source "$SCRIPT_DIR/lib/sbc.sh"

PLATFORM=""
MODE="quick"
DURATION=60
OUTPUT_DIR="${PROJECT_ROOT}/test_results"
IFACE=""
RUN_BENCH=1
CTEST_ONLY=0

usage() {
    cat <<'EOF'
ChronosRT test runner

Uso:
  test_runner.sh [--sbc ID | --detect | --list] [opciones]

Objetivo:
  --sbc ID           Perfil de SBC (por defecto: autodetección)
  --detect           Detectar la SBC actual y continuar con ella
  --list             Listar perfiles soportados y salir

Ejecución:
  --mode MODE        quick | full | stress   (defecto: quick)
  --duration SEC     Duración de cada benchmark (defecto: 60)
  --output DIR       Directorio de resultados (defecto: ./test_results)
  --interface IF     Interfaz de red para comprobar PHC (defecto: autodetectada)
  --ctest-only       Solo suites de test (sin benchmarks)
  --no-bench         Igual que --ctest-only
  -h, --help         Esta ayuda
EOF
}

log()  { printf '%s\n' "$*"; }
warn() { printf 'AVISO: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --sbc)         PLATFORM="${2:-}"; shift 2 ;;
        --detect)      PLATFORM="$(sbc_detect)"; shift ;;
        --platform)    PLATFORM="${2:-}"; shift 2 ;;   # compatibilidad
        --list)        while IFS= read -r id; do sbc_print "$id"; done < <(sbc_ids); exit 0 ;;
        --mode)        MODE="${2:-}"; shift 2 ;;
        --duration)    DURATION="${2:-}"; shift 2 ;;
        --output)      OUTPUT_DIR="${2:-}"; shift 2 ;;
        --interface)   IFACE="${2:-}"; shift 2 ;;
        --ctest-only|--no-bench) RUN_BENCH=0; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "opción desconocida: $1 (use --help)" ;;
    esac
done

case "$MODE" in
    quick|full|stress) ;;
    *) die "modo no válido: $MODE (quick|full|stress)" ;;
esac

# --- Resolver perfil ---------------------------------------------------------
if [ -z "$PLATFORM" ]; then
    PLATFORM="$(sbc_detect)"
    [ -n "$PLATFORM" ] && log "SBC autodetectada: $PLATFORM"
fi

if [ -z "$PLATFORM" ]; then
    # Sin perfil: se usan los valores del host para no bloquear la ejecución.
    SBC_ID="host"; SBC_NAME="Host genérico"; SBC_SOC="desconocido"
    SBC_CPU="$(uname -m)"; SBC_ARCH="$(uname -m)"; SBC_CORES="$(nproc)"
    SBC_CRC32="yes"; SBC_CRYPTO="desconocido"; SBC_PHC="no"; SBC_BENCH=""
    SBC_NOTES="perfil no identificado; auditoría en modo genérico"
else
    sbc_load "$PLATFORM" || die "perfil desconocido: $PLATFORM (ver --list)"
fi

# --- Localizar el build ------------------------------------------------------
BUILD_DIR=""
if [ -f "$PROJECT_ROOT/build/$SBC_ID/CMakeCache.txt" ]; then
    BUILD_DIR="$PROJECT_ROOT/build/$SBC_ID"
else
    # Fallback al build genérico SOLO si su arquitectura es la del host: evita
    # ejecutar binarios x86 creyendo que son de una SBC ARM (o al revés).
    host="$(uname -m)"
    same_arch=0
    case "$SBC_ARCH" in
        aarch64) if [ "$host" = "aarch64" ]; then same_arch=1; fi ;;
        armv7)   if [ "$host" = "armv7l" ];  then same_arch=1; fi ;;
        x86_64)  if [ "$host" = "x86_64" ];  then same_arch=1; fi ;;
        *)       same_arch=0 ;;
    esac
    if [ "$same_arch" -eq 1 ] && [ -f "$PROJECT_ROOT/build/CMakeCache.txt" ]; then
        BUILD_DIR="$PROJECT_ROOT/build"
    fi
fi
[ -n "$BUILD_DIR" ] || die "no hay build nativo para '$SBC_ID' ($SBC_ARCH). Ejecute: ./scripts/build.sh --sbc $SBC_ID"

mkdir -p "$OUTPUT_DIR"
AUDIT_LOG="$OUTPUT_DIR/env_audit.log"
SUMMARY="$OUTPUT_DIR/summary.txt"
: > "$AUDIT_LOG"

log ""
log "=============================================================="
log " ChronosRT Test Runner — $SBC_NAME ($SBC_ID)"
log "=============================================================="
log " Build      : $BUILD_DIR"
log " Modo       : $MODE    Duración: ${DURATION}s"
log " Resultados : $OUTPUT_DIR"
log ""

# --- Auditoría del entorno ---------------------------------------------------
PASS_N=0; WARN_N=0; FAIL_N=0; SKIP_N=0

record() {
    local status="$1" name="$2" detail="${3:-}"
    printf '[%-4s] %-24s %s\n' "$status" "$name" "$detail" | tee -a "$AUDIT_LOG"
    case "$status" in
        PASS) PASS_N=$((PASS_N + 1)) ;;
        WARN) WARN_N=$((WARN_N + 1)) ;;
        FAIL) FAIL_N=$((FAIL_N + 1)) ;;
        SKIP) SKIP_N=$((SKIP_N + 1)) ;;
    esac
}

detect_iface() {
    [ -n "$IFACE" ] && { printf '%s' "$IFACE"; return; }
    if command -v ip >/dev/null 2>&1; then
        ip -o link show up 2>/dev/null | awk -F': ' '$2 != "lo" {print $2; exit}'
    fi
}

audit_environment() {
    log "--- Auditoría del entorno ---"

    # 1. Arquitectura
    local arch
    arch="$(uname -m)"
    case "$SBC_ARCH" in
        aarch64) [ "$arch" = "aarch64" ] && record PASS arch "$arch" || record WARN arch "$arch (perfil $SBC_ARCH)" ;;
        armv7)   [ "$arch" = "armv7l" ] && record PASS arch "$arch" || record WARN arch "$arch (perfil $SBC_ARCH)" ;;
        x86_64)  [ "$arch" = "x86_64" ] && record PASS arch "$arch" || record WARN arch "$arch (perfil $SBC_ARCH)" ;;
        *)       record SKIP arch "$arch" ;;
    esac

    # 2. CRC32 por instrucción (HWCAP_CRC32 / SSE4.2)
    local crc="no" cpuinfo=/proc/cpuinfo
    if [ -r "$cpuinfo" ]; then
        if [ "$SBC_ARCH" = "x86_64" ]; then
            grep -qw 'sse4_2' "$cpuinfo" && crc="yes"
        else
            grep -qw 'crc32' "$cpuinfo" && crc="yes"
        fi
    fi
    if sbc_expects_crc32; then
        [ "$crc" = "yes" ] && record PASS crc32-hw "instrucciones presentes" \
                           || record FAIL crc32-hw "el perfil espera CRC32 HW pero no aparece"
    else
        [ "$crc" = "no" ] && record PASS crc32-fallback "sin CRC32 HW (se usa slice-by-8)" \
                          || record WARN crc32-fallback "perfil sin CRC32 pero la CPU lo expone"
    fi

    # 3. Kernel PREEMPT_RT
    if [ -e /sys/kernel/realtime ] && [ "$(cat /sys/kernel/realtime 2>/dev/null)" = "1" ]; then
        record PASS preempt-rt "activo"
    elif grep -qa 'PREEMPT_RT' /proc/version 2>/dev/null; then
        record PASS preempt-rt "compilado en el kernel"
    else
        record WARN preempt-rt "no detectado ($(uname -r))"
    fi

    # 4. Aislamiento de CPUs (según §10.1)
    local cmdline=""
    [ -r /proc/cmdline ] && cmdline="$(cat /proc/cmdline)"
    if printf '%s' "$cmdline" | grep -q 'isolcpus=' && \
       printf '%s' "$cmdline" | grep -q 'nohz_full=' && \
       printf '%s' "$cmdline" | grep -q 'rcu_nocbs='; then
        record PASS cpu-isolation "isolcpus + nohz_full + rcu_nocbs"
    else
        record WARN cpu-isolation "añada: $(sbc_cmdline)"
    fi

    # 5. Governor
    local gov=""
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -r "$f" ] || continue
        gov="$(cat "$f" 2>/dev/null)"
        break
    done
    if [ -z "$gov" ]; then
        record SKIP governor "cpufreq no disponible"
    elif [ "$gov" = "performance" ]; then
        record PASS governor "performance"
    else
        record WARN governor "$gov (use performance)"
    fi

    # 6. Hugepages
    local hp
    hp="$(awk '/^HugePages_Total:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    if [ "${hp:-0}" -gt 0 ] 2>/dev/null; then
        record PASS hugepages "$hp páginas"
    else
        record WARN hugepages "0 páginas (vm.nr_hugepages)"
    fi

    # 7. CAP_NET_RAW (necesaria para AF_PACKET)
    local cap
    cap="$(awk '/^CapEff:/ {print $2}' /proc/self/status 2>/dev/null || true)"
    if [ -n "$cap" ] && (( (16#$cap >> 13) & 1 )); then
        record PASS cap-net-raw "presente"
    else
        record WARN cap-net-raw "ausente (sudo setcap cap_net_raw+ep ...)"
    fi

    # 8. PHC / Launch Time en la interfaz
    local ifc
    ifc="$(detect_iface)"
    if [ -z "$ifc" ]; then
        record SKIP phc "sin interfaz de red"
    elif ! command -v ethtool >/dev/null 2>&1; then
        record SKIP phc "ethtool no instalado"
    elif ethtool -T "$ifc" 2>/dev/null | grep -q 'hardware-transmit'; then
        record PASS phc "$ifc con timestamping HW"
    elif [ "$SBC_PHC" = "yes" ]; then
        record WARN phc "$ifc sin PHC (requiere NIC i210/i225/i226)"
    else
        record SKIP phc "$ifc sin PHC (normal en este perfil)"
    fi

    # 9. Protocolo seleccionado
    local proto="${CHRONOSRT_PROTOCOL:-<sin definir>}"
    case "$proto" in
        ethertux|ethercat) record PASS protocol "$proto" ;;
        "<sin definir>")   record WARN protocol "CHRONOSRT_PROTOCOL no definido (defecto: ethercat)" ;;
        *)                 record WARN protocol "valor no válido: $proto" ;;
    esac

    # 10. Kernel moderno (§11)
    local kver
    kver="$(uname -r)"
    if printf '%s' "$kver" | grep -Eq '^6\.12\.'; then
        record WARN kernel "$kver — la serie 6.12.y tiene regresiones RT documentadas"
    else
        record PASS kernel "$kver"
    fi

    log ""
}

# --- Suites de test ----------------------------------------------------------
run_tests() {
    log "--- Suites de test ---"
    local logf="$OUTPUT_DIR/unit_tests.log"
    if [ -f "$BUILD_DIR/CTestTestfile.cmake" ] && command -v ctest >/dev/null 2>&1; then
        if ctest --test-dir "$BUILD_DIR" --output-on-failure >"$logf" 2>&1; then
            local n
            n="$(grep -cE '^ *[0-9]+/[0-9]+ Test' "$logf" 2>/dev/null || echo 0)"
            log "  PASSED ($n suites) — $logf"
            TESTS_OK=1
        else
            log "  FAILED — ver $logf"
            TESTS_OK=0
        fi
    else
        # Sin ctest: se ejecutan todas las suites presentes.
        local ok=1 found=0 bin
        : > "$logf"
        for bin in chronosrt_test chronosrt_test_lockfree chronosrt_test_rt \
                   chronosrt_test_resilience chronosrt_test_integration \
                   chronosrt_test_network chronosrt_test_hwaccel chronosrt_test_ethertux; do
            [ -x "$BUILD_DIR/$bin" ] || continue
            found=$((found + 1))
            if "$BUILD_DIR/$bin" >>"$logf" 2>&1; then
                log "  PASS $bin"
            else
                log "  FAIL $bin"
                ok=0
            fi
        done
        [ "$found" -gt 0 ] || { log "  no se encontraron binarios de test"; ok=0; }
        TESTS_OK=$ok
    fi
    log ""
}

# --- Benchmarks --------------------------------------------------------------
run_benchmarks() {
    [ "$RUN_BENCH" -eq 1 ] || return 0
    local bench="$BUILD_DIR/chronosrt_bench"
    if [ ! -x "$bench" ]; then
        log "--- Benchmarks: omitidos (chronosrt_bench no compilado) ---"
        log ""
        return 0
    fi

    log "--- Benchmarks ---"
    local common=(--duration="$DURATION")
    local plat
    plat="$(sbc_bench_platform)"
    [ -n "$plat" ] && common+=("--platform=$plat")

    local threads
    threads="$(nproc 2>/dev/null || echo "$SBC_CORES")"

    "$bench" --mode=latency "${common[@]}" >"$OUTPUT_DIR/latency.log" 2>&1 \
        && log "  latency    OK — $OUTPUT_DIR/latency.log" \
        || log "  latency    FALLÓ — $OUTPUT_DIR/latency.log"

    "$bench" --mode=jitter "${common[@]}" >"$OUTPUT_DIR/jitter.log" 2>&1 \
        && log "  jitter     OK — $OUTPUT_DIR/jitter.log" \
        || log "  jitter     FALLÓ — $OUTPUT_DIR/jitter.log"

    if [ "$MODE" = "full" ] || [ "$MODE" = "stress" ]; then
        "$bench" --mode=throughput "${common[@]}" --threads="$threads" \
            >"$OUTPUT_DIR/throughput.log" 2>&1 \
            && log "  throughput OK — $OUTPUT_DIR/throughput.log" \
            || log "  throughput FALLÓ — $OUTPUT_DIR/throughput.log"
    fi

    if [ "$MODE" = "stress" ]; then
        "$bench" --mode=stress "${common[@]}" --threads="$threads" \
            >"$OUTPUT_DIR/stress.log" 2>&1 \
            && log "  stress     OK — $OUTPUT_DIR/stress.log" \
            || log "  stress     FALLÓ — $OUTPUT_DIR/stress.log"
    fi
    log ""
}

# --- Ejecución ---------------------------------------------------------------
audit_environment

if [ "$CTEST_ONLY" -eq 0 ]; then
    run_tests
    run_benchmarks
else
    run_tests
fi

# --- Resumen -----------------------------------------------------------------
{
    echo "ChronosRT — resumen de ejecución"
    echo "Fecha      : $(date '+%Y-%m-%d %H:%M:%S')"
    echo "SBC        : $SBC_ID ($SBC_NAME, $SBC_SOC/$SBC_CPU)"
    echo "Build      : $BUILD_DIR"
    echo "Modo       : $MODE (${DURATION}s)"
    echo "Auditoría  : PASS=$PASS_N WARN=$WARN_N FAIL=$FAIL_N SKIP=$SKIP_N"
    echo "Suites     : $([ "${TESTS_OK:-0}" -eq 1 ] && echo PASSED || echo FAILED)"
    echo "Logs       : $OUTPUT_DIR"
} | tee "$SUMMARY"

log ""
if [ "$FAIL_N" -gt 0 ] || [ "${TESTS_OK:-0}" -ne 1 ]; then
    log "Resultado: FALLO (revise $AUDIT_LOG y $OUTPUT_DIR/unit_tests.log)"
    exit 1
fi
log "Resultado: OK"
