#!/usr/bin/env bash
# =============================================================================
# scripts/lib/sbc.sh — Perfiles de SBC objetivo de ChronosRT
#
# Fuente ÚNICA de verdad para build.sh y test_runner.sh.
# Referencia: docs/Protocolo EtherTux.md
#   §7.5 Matriz de capacidades por SBC (CRC32, crypto engine, PHC/Launch Time)
#   §10   Configuración por plataforma (kernel, cmdline, afinidad, PTP)
#
# Uso:
#   source "$(dirname "$0")/lib/sbc.sh"
#   sbc_load rpi5        # define las variables SBC_*
#   sbc_detect           # imprime el id detectado (o vacío)
#
# Variables definidas por sbc_load:
#   SBC_ID SBC_NAME SBC_SOC SBC_CPU SBC_ARCH SBC_CORES
#   SBC_CRC32 (yes|no)  SBC_CRYPTO (texto)  SBC_PHC (yes|no)
#   SBC_BENCH (token de --platform, vacío si no hay equivalente)
#   SBC_MARCH SBC_MCPU SBC_MFLOAT  (flags de compilación; vacíos si no aplican)
#   SBC_NOTES (aviso operativo)
# =============================================================================

# Identificadores canónicos (orden de presentación en --list).
sbc_ids() {
    printf '%s\n' \
        rpi-zero-2w rpi3 rpi4 rpi5 cm4 cm5 \
        opi-zero2 opi-zero3 opi-3-lts opi-4-lts opi-5 opi-5-plus \
        btt-cb1 btt-cb2 mks-pi mks-skipr rock-pi-4 radxa-rock-3 radxa-rock-5 \
        bpi-m2-zero bpi-m4 bpi-m5 le-potat renegade sweet-potato \
        jetson-nano jetson-orin x86-64 x86-64-rt
}

# Normaliza alias habituales (rpi4, raspberry-pi-4, raspberrypi4, ...).
_sbc_alias() {
    local id="${1,,}"
    id="${id// /-}"
    id="${id//_/-}"
    case "$id" in
        rpi-zero-2w|rpi-zero-2-w|zero2w|rpi02w|raspberry-pi-zero-2-w) echo rpi-zero-2w ;;
        rpi3|rpi-3|rpi3b|rpi3b+|raspberry-pi-3)                       echo rpi3 ;;
        rpi4|rpi-4|rpi4b|raspberry-pi-4)                              echo rpi4 ;;
        rpi5|rpi-5|rpi5b|raspberry-pi-5)                              echo rpi5 ;;
        cm4|compute-module-4)                                         echo cm4 ;;
        cm5|compute-module-5)                                         echo cm5 ;;
        opi-zero2|orange-pi-zero-2|orangepi-zero2)                    echo opi-zero2 ;;
        opi-zero3|orange-pi-zero-3|orangepi-zero3)                    echo opi-zero3 ;;
        opi-3-lts|orange-pi-3-lts|orangepi-3)                         echo opi-3-lts ;;
        opi-4-lts|orange-pi-4-lts|orangepi-4)                         echo opi-4-lts ;;
        opi-5|orange-pi-5|orangepi5|orange-pi5)                       echo opi-5 ;;
        opi-5-plus|orange-pi-5-plus|orangepi5plus)                    echo opi-5-plus ;;
        btt-cb1|cb1|bigtreetech-cb1)                                  echo btt-cb1 ;;
        btt-cb2|cb2|bigtreetech-cb2)                                  echo btt-cb2 ;;
        mks-pi|mkspi)                                                 echo mks-pi ;;
        mks-skipr|skipr)                                              echo mks-skipr ;;
        rock-pi-4|rockpi4|rock-pi4)                                   echo rock-pi-4 ;;
        radxa-rock-3|rock-3|rock3|radxa3)                             echo radxa-rock-3 ;;
        radxa-rock-5|rock-5|rock5|rock5b|rock-5b|radxa5)             echo radxa-rock-5 ;;
        bpi-m2-zero|banana-pi-m2-zero|m2zero|bpi-m2zero)              echo bpi-m2-zero ;;
        bpi-m4|banana-pi-m4)                                          echo bpi-m4 ;;
        bpi-m5|banana-pi-m5)                                          echo bpi-m5 ;;
        le-potat|lepotato|le-potato)                                  echo le-potat ;;
        renegade|libre-computer-renegade)                             echo renegade ;;
        sweet-potato|sweetpotato)                                     echo sweet-potato ;;
        jetson|jetson-nano|jetson_nano)                               echo jetson-nano ;;
        jetson-orin|jetson_orin|orin)                                 echo jetson-orin ;;
        intel-nuc|intel_nuc|x86-64|x86_64|x86_64_rt|amd64|generic)    echo x86-64 ;;
        x86-64-rt|x86_64-rt|rt-x86-64)                                echo x86-64-rt ;;
        *) echo "$id" ;;
    esac
}

# Carga el perfil. Devuelve 1 si el identificador no existe.
sbc_load() {
    local id
    id="$(_sbc_alias "${1:-}")"

    SBC_ID="$id"
    SBC_NAME=""; SBC_SOC=""; SBC_CPU=""; SBC_ARCH=""; SBC_CORES=""
    SBC_CRC32=""; SBC_CRYPTO=""; SBC_PHC=""; SBC_BENCH=""; SBC_NOTES=""

    case "$id" in
        # --- Raspberry Pi -----------------------------------------------------
        rpi-zero-2w)
            SBC_NAME="Raspberry Pi Zero 2 W"; SBC_SOC="BCM2710A1"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="no"
            SBC_PHC=no; SBC_BENCH="";;
        rpi3)
            SBC_NAME="Raspberry Pi 3 B/B+"; SBC_SOC="BCM2837/B0"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="no"
            SBC_PHC=no; SBC_BENCH="";;
        rpi4)
            SBC_NAME="Raspberry Pi 4 B"; SBC_SOC="BCM2711"; SBC_CPU="Cortex-A72"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="no"
            SBC_PHC=no; SBC_BENCH="rpi4";;
        rpi5)
            SBC_NAME="Raspberry Pi 5"; SBC_SOC="BCM2712"; SBC_CPU="Cortex-A76"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="ARMv8 Crypto (AES/SHA)"
            SBC_PHC=yes; SBC_BENCH="rpi5"
            SBC_NOTES="PHC/Launch Time solo con NIC PCIe (i226)";;
        cm4)
            SBC_NAME="Raspberry Pi CM4"; SBC_SOC="BCM2711"; SBC_CPU="Cortex-A72"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="no"
            SBC_PHC=no; SBC_BENCH="rpi4";;
        cm5)
            SBC_NAME="Raspberry Pi CM5"; SBC_SOC="BCM2712"; SBC_CPU="Cortex-A76"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="ARMv8 Crypto (AES/SHA)"
            SBC_PHC=yes; SBC_BENCH="rpi5"
            SBC_NOTES="PHC/Launch Time solo con NIC PCIe (i226)";;
        # --- Allwinner --------------------------------------------------------
        opi-zero2)
            SBC_NAME="Orange Pi Zero 2"; SBC_SOC="Allwinner H616"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="sun8i-ce"
            SBC_PHC=no; SBC_BENCH="";;
        opi-zero3)
            SBC_NAME="Orange Pi Zero 3"; SBC_SOC="Allwinner H618"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="sun8i-ce"
            SBC_PHC=no; SBC_BENCH="";;
        opi-3-lts)
            SBC_NAME="Orange Pi 3 LTS"; SBC_SOC="Allwinner H6"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="sun8i-ce"
            SBC_PHC=no; SBC_BENCH="";;
        btt-cb1)
            SBC_NAME="BTT CB1"; SBC_SOC="Allwinner H616"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="sun8i-ce"
            SBC_PHC=no; SBC_BENCH="";;
        # --- Rockchip ---------------------------------------------------------
        opi-4-lts)
            SBC_NAME="Orange Pi 4 LTS"; SBC_SOC="Rockchip RK3399"; SBC_CPU="Cortex-A72"
            SBC_ARCH=aarch64; SBC_CORES=6; SBC_CRC32=yes; SBC_CRYPTO="rk_crypto V1"
            SBC_PHC=no; SBC_BENCH=""
            SBC_NOTES="big.LITTLE A72+A53; se compila para los núcleos grandes";;
        opi-5)
            SBC_NAME="Orange Pi 5"; SBC_SOC="Rockchip RK3588S"; SBC_CPU="Cortex-A76"
            SBC_ARCH=aarch64; SBC_CORES=8; SBC_CRC32=yes; SBC_CRYPTO="rk2_crypto (NO usable)"
            SBC_PHC=no; SBC_BENCH="orange_pi5"
            SBC_NOTES="A76+A55; rk2_crypto aún en parcheo => solo software";;
        opi-5-plus)
            SBC_NAME="Orange Pi 5 Plus"; SBC_SOC="Rockchip RK3588"; SBC_CPU="Cortex-A76"
            SBC_ARCH=aarch64; SBC_CORES=8; SBC_CRC32=yes; SBC_CRYPTO="rk2_crypto (NO usable)"
            SBC_PHC=no; SBC_BENCH="orange_pi5"
            SBC_NOTES="A76+A55; rk2_crypto aún en parcheo => solo software";;
        btt-cb2)
            SBC_NAME="BTT CB2"; SBC_SOC="Rockchip RK3566"; SBC_CPU="Cortex-A55"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="rk2_crypto (NO usable)"
            SBC_PHC=no; SBC_BENCH="";;
        mks-pi)
            SBC_NAME="MKS PI"; SBC_SOC="Rockchip RK3328"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="rk_crypto V1"
            SBC_PHC=no; SBC_BENCH="";;
        mks-skipr)
            SBC_NAME="MKS Skipr"; SBC_SOC="Rockchip RK3328 + STM32F407"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="rk_crypto V1"
            SBC_PHC=no; SBC_BENCH=""
            SBC_NOTES="el STM32F407 es un MCU auxiliar, no ejecuta ChronosRT";;
        rock-pi-4)
            SBC_NAME="Rock Pi 4"; SBC_SOC="Rockchip RK3399"; SBC_CPU="Cortex-A72"
            SBC_ARCH=aarch64; SBC_CORES=6; SBC_CRC32=yes; SBC_CRYPTO="rk_crypto V1"
            SBC_PHC=no; SBC_BENCH=""
            SBC_NOTES="big.LITTLE A72+A53; se compila para los núcleos grandes";;
        radxa-rock-3)
            SBC_NAME="Radxa Rock 3"; SBC_SOC="Rockchip RK3568"; SBC_CPU="Cortex-A55"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="rk2_crypto (NO usable)"
            SBC_PHC=no; SBC_BENCH="";;
        radxa-rock-5)
            SBC_NAME="Radxa Rock 5"; SBC_SOC="Rockchip RK3588"; SBC_CPU="Cortex-A76"
            SBC_ARCH=aarch64; SBC_CORES=8; SBC_CRC32=yes; SBC_CRYPTO="rk2_crypto (NO usable)"
            SBC_PHC=no; SBC_BENCH="rock5b"
            SBC_NOTES="A76+A55; rk2_crypto aún en parcheo => solo software";;
        # --- Banana Pi / Amlogic / Libre Computer -----------------------------
        bpi-m2-zero)
            SBC_NAME="Banana Pi M2 Zero"; SBC_SOC="Allwinner H3/H2+"; SBC_CPU="Cortex-A7"
            SBC_ARCH=armv7; SBC_CORES=4; SBC_CRC32=no; SBC_CRYPTO="sun8i-ce"
            SBC_PHC=no; SBC_BENCH=""
            SBC_NOTES="ÚNICA SBC sin CRC32 por instrucción: la HAL usa el fallback slice-by-8";;
        bpi-m4)
            SBC_NAME="Banana Pi M4"; SBC_SOC="Realtek RTD1395"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="desconocido"
            SBC_PHC=no; SBC_BENCH="";;
        bpi-m5)
            SBC_NAME="Banana Pi M5"; SBC_SOC="Amlogic S905X3"; SBC_CPU="Cortex-A55"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="Crypto Engine"
            SBC_PHC=no; SBC_BENCH="";;
        le-potat)
            SBC_NAME="Libre Computer Le Potato"; SBC_SOC="Amlogic S905X"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="sin Crypto ARMv8"
            SBC_PHC=no; SBC_BENCH="";;
        renegade)
            SBC_NAME="Libre Computer Renegade"; SBC_SOC="Rockchip RK3328"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="rk_crypto V1"
            SBC_PHC=no; SBC_BENCH="";;
        sweet-potato)
            SBC_NAME="Libre Computer Sweet Potato"; SBC_SOC="Amlogic S905X"; SBC_CPU="Cortex-A53"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="sin Crypto ARMv8"
            SBC_PHC=no; SBC_BENCH="";;
        # --- NVIDIA -----------------------------------------------------------
        jetson-nano)
            SBC_NAME="NVIDIA Jetson Nano"; SBC_SOC="Tegra X1"; SBC_CPU="Cortex-A57"
            SBC_ARCH=aarch64; SBC_CORES=4; SBC_CRC32=yes; SBC_CRYPTO="no"
            SBC_PHC=no; SBC_BENCH="jetson_nano";;
        jetson-orin)
            SBC_NAME="NVIDIA Jetson Orin"; SBC_SOC="Tegra 234"; SBC_CPU="Cortex-A78AE"
            SBC_ARCH=aarch64; SBC_CORES=12; SBC_CRC32=yes; SBC_CRYPTO="ARMv8 Crypto"
            SBC_PHC=no; SBC_BENCH="jetson_orin"
            SBC_NOTES="soporta SVE (PF-4)";;
        # --- x86-64 -----------------------------------------------------------
        x86-64)
            SBC_NAME="x86-64 genérico"; SBC_SOC="x86-64"; SBC_CPU="x86-64"
            SBC_ARCH=x86_64; SBC_CORES=8; SBC_CRC32=yes; SBC_CRYPTO="AES-NI"
            SBC_PHC=yes; SBC_BENCH=""
            SBC_NOTES="PHC/Launch Time solo con NIC Intel i210/i225/i226 o Mellanox";;
        x86-64-rt)
            SBC_NAME="x86-64 con PREEMPT_RT"; SBC_SOC="x86-64"; SBC_CPU="x86-64"
            SBC_ARCH=x86_64; SBC_CORES=8; SBC_CRC32=yes; SBC_CRYPTO="AES-NI"
            SBC_PHC=yes; SBC_BENCH="x86_64_rt"
            SBC_NOTES="máximo determinismo; requiere kernel PREEMPT_RT";;
        *)
            return 1;;
    esac

    # Flags de compilación derivados de la microarquitectura.
    SBC_MARCH=""; SBC_MCPU=""; SBC_MFLOAT=""
    case "$SBC_CPU" in
        Cortex-A53)   SBC_MARCH="-march=armv8-a+crc";   SBC_MCPU="-mcpu=cortex-a53" ;;
        Cortex-A55)   SBC_MARCH="-march=armv8.2-a+crc"; SBC_MCPU="-mcpu=cortex-a55" ;;
        Cortex-A57)   SBC_MARCH="-march=armv8-a+crc";   SBC_MCPU="-mcpu=cortex-a57" ;;
        Cortex-A72)   SBC_MARCH="-march=armv8-a+crc";   SBC_MCPU="-mcpu=cortex-a72" ;;
        Cortex-A76)   SBC_MARCH="-march=armv8.2-a+crc"; SBC_MCPU="-mcpu=cortex-a76" ;;
        Cortex-A78AE) SBC_MARCH="-march=armv8.2-a+crc+sve"; SBC_MCPU="-mcpu=cortex-a78" ;;
        Cortex-A7)
            SBC_MARCH="-march=armv7-a+neon-vfpv4"
            SBC_MCPU="-mcpu=cortex-a7"
            SBC_MFLOAT="-mfpu=neon-vfpv4"
            ;;
        x86-64)       SBC_MARCH=""; SBC_MCPU="" ;;
    esac
    return 0
}

# Capacidad CRC32 por instrucción esperada según el perfil (para auditoría).
sbc_expects_crc32() {
    [ "${SBC_CRC32:-no}" = "yes" ]
}

# Token válido para `chronosrt_bench --platform` (vacío => dejar autodetección).
sbc_bench_platform() {
    printf '%s' "${SBC_BENCH:-}"
}

# Línea de comandos de kernel recomendada para el perfil cargado.
# `sbc_cmdline [cores]` permite forzar el número de núcleos del destino; por
# defecto usa los del perfil (no los del host, que puede ser otro equipo).
sbc_cmdline() {
    local cores isolate
    cores="${1:-${SBC_CORES:-$(nproc 2>/dev/null || echo 4)}}"
    if   [ "$cores" -ge 8 ]; then isolate="4,5,6,7"
    elif [ "$cores" -ge 6 ]; then isolate="4,5"
    elif [ "$cores" -ge 4 ]; then isolate="2,3"
    else                          isolate=""
    fi
    if [ -z "$isolate" ]; then
        printf 'cpuidle.off=1 cpufreq.off=1'
    else
        printf 'isolcpus=%s nohz_full=%s rcu_nocbs=%s cpuidle.off=1 cpufreq.off=1' \
               "$isolate" "$isolate" "$isolate"
    fi
}

# Paquete Debian/Ubuntu con el cross-compilador necesario (vacío = nativo).
sbc_cross_pkg() {
    case "${SBC_ARCH:-}" in
        aarch64) echo "gcc-aarch64-linux-gnu" ;;
        armv7)   echo "gcc-arm-linux-gnueabihf" ;;
        x86_64)  echo "gcc" ;;
        *)       echo "" ;;
    esac
}

# Detecta la SBC en la que se ejecuta el script. Imprime el id o nada.
sbc_detect() {
    local compat="" arch id=""

    if [ -r /proc/device-tree/compatible ]; then
        compat="$(tr -d '\0' < /proc/device-tree/compatible)"
    fi

    case "$compat" in
        *brcm,bcm2712*)            id=cm5 ;;
        *brcm,bcm2711*)            id=rpi4 ;;
        *brcm,bcm2837*)            id=rpi3 ;;
        *brcm,bcm2710*)            id=rpi-zero-2w ;;
        *allwinner,sun50i-h618*)   id=opi-zero3 ;;
        *allwinner,sun50i-h616*)   id=opi-zero2 ;;
        *allwinner,sun50i-h6*)     id=opi-3-lts ;;
        *allwinner,sun8i-h3*|*allwinner,sun8i-h2-plus*) id=bpi-m2-zero ;;
        *rockchip,rk3588*)         id=radxa-rock-5 ;;
        *rockchip,rk3568*)         id=radxa-rock-3 ;;
        *rockchip,rk3566*)         id=btt-cb2 ;;
        *rockchip,rk3399*)         id=rock-pi-4 ;;
        *rockchip,rk3328*)         id=mks-pi ;;
        *amlogic,s905x3*)          id=bpi-m5 ;;
        *amlogic,s905x*)           id=le-potat ;;
        *realtek,rtd1395*)         id=bpi-m4 ;;
        *nvidia,tegra234*)         id=jetson-orin ;;
        *nvidia,tegra210*)         id=jetson-nano ;;
        *) id="" ;;
    esac

    if [ -z "$id" ]; then
        arch="$(uname -m)"
        case "$arch" in
            x86_64|amd64)
                if [ -e /sys/kernel/realtime ] || grep -qa 'PREEMPT_RT' /proc/version 2>/dev/null; then
                    id=x86-64-rt
                else
                    id=x86-64
                fi
                ;;
            aarch64|arm64) id="" ;;   # ARM64 genérico: sin perfil específico
            armv7l|armv6l) id=bpi-m2-zero ;;
        esac
    fi

    printf '%s' "$id"
}

# Resumen legible de un perfil.
sbc_print() {
    local id="$1"
    if ! sbc_load "$id"; then
        echo "Perfil desconocido: $id" >&2
        return 1
    fi
    printf '%-16s %-30s %-12s %-10s %2s cores  CRC32=%-3s PHC=%-3s\n' \
        "$SBC_ID" "$SBC_NAME" "$SBC_SOC" "$SBC_CPU" "$SBC_CORES" \
        "$SBC_CRC32" "$SBC_PHC"
    printf '                 crypto: %s\n' "$SBC_CRYPTO"
    [ -n "$SBC_NOTES" ] && printf '                 nota  : %s\n' "$SBC_NOTES"
    printf '                 flags : %s %s %s\n' "$SBC_MARCH" "$SBC_MCPU" "$SBC_MFLOAT"
    printf '                 cmdline: %s\n' "$(sbc_cmdline)"
}
