cmake_minimum_required(VERSION 3.16)
project(chronosrt VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(CHRONOSRT_BUILD_TESTS     "Build test suite" ON)
option(CHRONOSRT_BUILD_BENCHMARK "Build benchmark executable" ON)
option(CHRONOSRT_BUILD_TOOLS     "Install helper tools" ON)
option(CHRONOSRT_BUILD_PROTOCOLS "Build the network protocols (EtherCAT + EtherTux)" ON)
option(CHRONOSRT_USE_SIMD        "Enable SIMD optimizations" ON)
option(CHRONOSRT_USE_HUGEPAGES   "Enable hugepages support" ON)
option(CHRONOSRT_ENABLE_LTO      "Enable link-time optimization (PF-6)" ON)
option(CHRONOSRT_ENABLE_NATIVE   "Build with -march=native (PF-6, no portátil)" OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(CHRONOSRT_DEBUG)
endif()

# glibc extensions (CPU_ZERO/CPU_SET, sched_setcpu, thread affinity, memfd)
add_compile_definitions(_GNU_SOURCE)

find_package(Threads REQUIRED)

# --- SIMD detection ---------------------------------------------------------
# Las pruebas AVX-512/AVX2 se compilan *y ejecutan* para detectar correctamente
# la capacidad del host (p. ej. Zen2 tiene AVX2 sin AVX-512) y evitar SIGILL.
include(CheckCSourceCompiles)
include(CheckCSourceRuns)

if(NOT CMAKE_CROSSCOMPILING)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")

        set(CMAKE_REQUIRED_FLAGS "-mavx512f")
        check_c_source_runs("
            #include <immintrin.h>
            int main(void) {
                __m512i a = _mm512_set1_epi32(7);
                __m512i b = _mm512_add_epi32(a, a);
                int buf[16];
                _mm512_storeu_si512((void*)buf, b);
                return buf[0] == 14 ? 0 : 1;
            }
        " HAVE_AVX512)
        set(CMAKE_REQUIRED_FLAGS)

        set(CMAKE_REQUIRED_FLAGS "-mavx2")
        check_c_source_runs("
            #include <immintrin.h>
            int main(void) {
                __m256i a = _mm256_set1_epi32(7);
                __m256i b = _mm256_add_epi32(a, a);
                int buf[8];
                _mm256_storeu_si256((__m256i*)buf, b);
                return buf[0] == 14 ? 0 : 1;
            }
        " HAVE_AVX2)
        set(CMAKE_REQUIRED_FLAGS)
    endif()
endif()

check_c_source_compiles("
    #if defined(__aarch64__)
    #include <arm_neon.h>
    int main(void) { uint64x2_t x = vdupq_n_u64(0); (void)x; return 0; }
    #else
    #error not aarch64
    #endif
" HAVE_NEON)

# SVE (PF-4): vectorización escalable en ARM64 (Jetson Orin y posteriores).
check_c_source_compiles("
    #if defined(__aarch64__) && defined(__ARM_FEATURE_SVE)
    #include <arm_sve.h>
    int main(void) { return (int)svcntb(); }
    #else
    #error no sve
    #endif
" HAVE_SVE)

# --- Fuentes y cabeceras ----------------------------------------------------
set(CHRONOSRT_PUBLIC_HEADERS
    include/chronosrt/chronosrt.h
    include/chronosrt/chronosrt_abi.h
    include/chronosrt/chronosrt_ring.h
    include/chronosrt/chronosrt_mpmc.h
    include/chronosrt/chronosrt_bounded.h
    include/chronosrt/chronosrt_simd.h
    include/chronosrt/chronosrt_mem.h
    include/chronosrt/chronosrt_kernel.h
    include/chronosrt/chronosrt_sched.h
    include/chronosrt/chronosrt_rt.h
    include/chronosrt/chronosrt_resilience.h
    include/chronosrt/chronosrt_obs.h
    include/chronosrt/chronosrt_tuxrt.h
    include/chronosrt/chronosrt_ethercat.h
    include/chronosrt/chronosrt_ethertux.h
    include/chronosrt/chronosrt_hwaccel.h
    include/chronosrt/chronosrt_protocol.h
)

set(CHRONOSRT_SOURCES
    # Runtime core: eventos, memoria, planificación RT, resiliencia, observabilidad
    src/core/chronosrt_core.c
    src/core/chronosrt_mem.c
    src/core/chronosrt_sched.c
    src/core/chronosrt_rt.c
    src/core/chronosrt_resilience.c
    src/core/chronosrt_obs.c
    # Plataforma: detección y validación del kernel
    src/platform/chronosrt_kernel.c
    # HAL de aceleración por hardware (CRC-32C, GF(2^8), Reed-Solomon)
    src/hwaccel/chronosrt_hwaccel.c
    # Selección de protocolo de red (EtherCAT / EtherTux)
    src/protocols/chronosrt_protocol.c
    # SIMD genérico, puente con libtuxrt_core y benchmark
    src/simd/chronosrt_simd.c
    src/integration/chronosrt_bridge.c
    src/benchmark/chronosrt_benchmark.c
)

if(CHRONOSRT_BUILD_PROTOCOLS)
    list(APPEND CHRONOSRT_SOURCES
        # Maestro EtherCAT (hardware dedicado)
        src/protocols/ethercat/ec_frame.c
        src/protocols/ethercat/ec_socket.c
        src/protocols/ethercat/ec_master.c
        src/protocols/ethercat/ec_slave.c
        src/protocols/ethercat/ec_dc.c
        src/protocols/ethercat/ec_mailbox.c
        # EtherTux (Ethernet comercial determinista)
        src/protocols/ethertux/et_frame.c
        src/protocols/ethertux/et_fec.c
        src/protocols/ethertux/et_qos.c
        src/protocols/ethertux/et_sched.c
        src/protocols/ethertux/et_esm.c
        src/protocols/ethertux/et_sync.c
        src/protocols/ethertux/et_topology.c
        src/protocols/ethertux/et_mbx.c
        src/protocols/ethertux/et_flowagg.c
        src/protocols/ethertux/et_frm.c
        src/protocols/ethertux/et_aalid.c
        src/protocols/ethertux/et_socket.c
        src/protocols/ethertux/et_master.c
    )
endif()

# --- Biblioteca compartida (ABI C estable, coexistencia con libtuxrt_core) ---
add_library(chronosrt SHARED ${CHRONOSRT_SOURCES} ${CHRONOSRT_PUBLIC_HEADERS})

set_target_properties(chronosrt PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_compile_definitions(chronosrt PRIVATE CHRONOSRT_BUILDING)

target_include_directories(chronosrt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/chronosrt>
    $<INSTALL_INTERFACE:include/chronosrt>
)

# --- Enlace universal de proveedores (TuxRT Provider ABI) -------------------
# ChronosRT se publica como PROVEEDOR del ecosistema exportando el símbolo
# descriptor `tuxrt_provider_descriptor`. La dependencia es UNIDIRECCIONAL
# (consumidor -> proveedor): libtuxrt_core lo descubre con dlopen. ChronosRT NO
# enlaza ni conoce a libtuxrt_core.
#
# `TuxRTProvider.cmake` es un módulo reutilizable por cualquier proyecto; se
# localiza por `TUXRT_PROVIDER_MODULE_DIR` o, por defecto, en el proyecto
# hermano TuxedoRT-Mojo.
set(TUXRT_PROVIDER_MODULE_DIR "" CACHE PATH
    "Directorio con TuxRTProvider.cmake (contrato de proveedor TuxRT)")

# El módulo vive en el proyecto anfitrión. Se resuelve partiendo del directorio
# REAL del proyecto (sin '..' ni enlaces) para que include() funcione también en
# montajes anidados, donde '..' no se resuelve de forma consistente.
set(_tuxrt_module "")
if(TUXRT_PROVIDER_MODULE_DIR)
    if(EXISTS "${TUXRT_PROVIDER_MODULE_DIR}/TuxRTProvider.cmake")
        set(_tuxrt_module "${TUXRT_PROVIDER_MODULE_DIR}/TuxRTProvider.cmake")
    endif()
endif()

if(NOT _tuxrt_module)
    get_filename_component(_src_real "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
    get_filename_component(_src_parent "${_src_real}" DIRECTORY)
    file(GLOB _tuxrt_candidates
         "${_src_parent}/TuxedoRT-Mojo/cmake/TuxRTProvider.cmake"
         "${_src_parent}/cmake/TuxRTProvider.cmake"
         "${_src_parent}/*/cmake/TuxRTProvider.cmake")
    if(_tuxrt_candidates)
        list(GET _tuxrt_candidates 0 _tuxrt_module)
    endif()
endif()

if(_tuxrt_module)
    get_filename_component(_tuxrt_dir "${_tuxrt_module}" DIRECTORY)
    set(TUXRT_PROVIDER_MODULE_DIR "${_tuxrt_dir}" CACHE PATH
        "Directorio con TuxRTProvider.cmake (contrato de proveedor TuxRT)" FORCE)
    include("${_tuxrt_module}")
endif()

if(COMMAND tuxrt_provide)
    tuxrt_provide(chronosrt
        NAME         "ChronosRT"
        VERSION      "${PROJECT_VERSION}"
        CAPABILITIES "ethercat,ethertux,rt-scheduler,recovery,observability,simd,lockfree")
else()
    message(STATUS "ChronosRT: TuxRTProvider.cmake no encontrado; descriptor de "
                   "proveedor omitido (defina -DTUXRT_PROVIDER_MODULE_DIR=<dir>)")
endif()

if(NOT CMAKE_CROSSCOMPILING)
    if(HAVE_SVE)
        target_compile_options(chronosrt PRIVATE -march=armv8-a+sve)
        target_compile_definitions(chronosrt PRIVATE CHRONOSRT_SIMD_SVE)
    endif()
endif()

if(CHRONOSRT_USE_SIMD)
    target_compile_definitions(chronosrt PRIVATE CHRONOSRT_SIMD_ENABLED)

    if(HAVE_AVX512)
        target_compile_options(chronosrt PRIVATE -mavx512f -mavx512dq -mavx512bw)
        target_compile_definitions(chronosrt PRIVATE CHRONOSRT_SIMD_AVX512)
    elseif(HAVE_AVX2)
        target_compile_options(chronosrt PRIVATE -mavx2)
        target_compile_definitions(chronosrt PRIVATE CHRONOSRT_SIMD_AVX2)
    elseif(HAVE_NEON)
        target_compile_definitions(chronosrt PRIVATE CHRONOSRT_SIMD_NEON)
    endif()
endif()

if(CHRONOSRT_USE_HUGEPAGES)
    target_compile_definitions(chronosrt PRIVATE CHRONOSRT_HUGEPAGES_ENABLED)
endif()

if(CHRONOSRT_ENABLE_NATIVE)
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-march=native" HAVE_MARCH_NATIVE)
    if(HAVE_MARCH_NATIVE)
        target_compile_options(chronosrt PRIVATE -march=native)
    endif()
endif()

if(CHRONOSRT_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT CHRONOSRT_IPO_OK OUTPUT CHRONOSRT_IPO_MSG)
    if(CHRONOSRT_IPO_OK)
        set_property(TARGET chronosrt PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "ChronosRT: LTO no disponible (${CHRONOSRT_IPO_MSG})")
    endif()
endif()

target_link_libraries(chronosrt PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})

# --- Benchmark executable ---------------------------------------------------
if(CHRONOSRT_BUILD_BENCHMARK)
    add_executable(chronosrt_bench tools/benchmark.c)
    target_link_libraries(chronosrt_bench PRIVATE chronosrt)
    if(CHRONOSRT_ENABLE_LTO AND CHRONOSRT_IPO_OK)
        set_property(TARGET chronosrt_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# --- Test suite -------------------------------------------------------------
if(CHRONOSRT_BUILD_TESTS)
    enable_testing()

    set(CHRONOSRT_TESTS
        chronosrt_test           tests/test_core.c
        chronosrt_test_lockfree  tests/test_lockfree.c
        chronosrt_test_rt        tests/test_rt.c
        chronosrt_test_resilience tests/test_resilience.c
        chronosrt_test_integration tests/test_integration.c
        chronosrt_test_network     tests/test_network.c
        chronosrt_test_hwaccel     tests/test_hwaccel.c
        chronosrt_test_ethertux    tests/test_ethertux.c
        chronosrt_test_ethertux_v010 tests/test_ethertux_v010.c
        chronosrt_test_aalid        tests/test_aalid.c
    )

    list(LENGTH CHRONOSRT_TESTS _n)
    math(EXPR _pairs "${_n} / 2 - 1")
    foreach(_i RANGE ${_pairs})
        math(EXPR _ti "${_i} * 2")
        math(EXPR _si "${_ti} + 1")
        list(GET CHRONOSRT_TESTS ${_ti} _tname)
        list(GET CHRONOSRT_TESTS ${_si} _tsrc)
        add_executable(${_tname} ${_tsrc})
        target_link_libraries(${_tname} PRIVATE chronosrt)
        add_test(NAME ${_tname} COMMAND ${_tname})
    endforeach()
endif()

# --- Instalación ------------------------------------------------------------
install(TARGETS chronosrt
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${CHRONOSRT_PUBLIC_HEADERS}
    DESTINATION include/chronosrt
)

install(DIRECTORY cmake/
    DESTINATION share/chronosrt/cmake
)

if(CHRONOSRT_BUILD_TOOLS)
    install(DIRECTORY scripts/
        DESTINATION share/chronosrt/scripts
        USE_SOURCE_PERMISSIONS
    )
endif()
