set(PCSX_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/vendor/pcsx_rearmed")
set(PCSX_LIBCHDR_DIR "${PCSX_VENDOR_DIR}/deps/libchdr")
set(PCSX_ZLIB_DIR "${PCSX_LIBCHDR_DIR}/deps/zlib-1.3.1")
set(PCSX_LZMA_DIR "${PCSX_LIBCHDR_DIR}/deps/lzma-24.05")
set(PCSX_ZSTD_DIR "${PCSX_LIBCHDR_DIR}/deps/zstd-1.5.6/lib")
set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS
    "."
    "${PCSX_VENDOR_DIR}"
    "${PCSX_VENDOR_DIR}/include"
    "${PCSX_VENDOR_DIR}/frontend"
    "${PCSX_VENDOR_DIR}/libpcsxcore"
    "${PCSX_LIBCHDR_DIR}/include"
    "${PCSX_LZMA_DIR}/include"
    "${PCSX_ZSTD_DIR}"
    "${PCSX_VENDOR_DIR}/plugins/dfsound"
    "${PCSX_VENDOR_DIR}/plugins/gpulib"
    "${PCSX_VENDOR_DIR}/plugins/cdrcimg"
    "${PCSX_ZLIB_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/jit"
)
set(COMPONENT_REQUIRES "retro-go")
register_component()

if(EXISTS "${PCSX_VENDOR_DIR}/README.md" AND EXISTS "${PCSX_VENDOR_DIR}/frontend/main.c")
    message(STATUS "PCSX ReARMed vendor tree detected at ${PCSX_VENDOR_DIR}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RG_PSX_VENDOR_PRESENT=1)
    include(${CMAKE_CURRENT_LIST_DIR}/pcsx_vendor_sources.cmake)
    list(LENGTH PCSX_VENDOR_CORE_SOURCES PCSX_VENDOR_CORE_COUNT)
    list(LENGTH PCSX_VENDOR_PLUGIN_SOURCES PCSX_VENDOR_PLUGIN_COUNT)
    message(STATUS "PCSX staged source lists prepared: core=${PCSX_VENDOR_CORE_COUNT}, plugin=${PCSX_VENDOR_PLUGIN_COUNT}")
    target_sources(${COMPONENT_LIB} PRIVATE
        ${PCSX_VENDOR_CORE_SOURCES}
        ${PCSX_VENDOR_PLUGIN_SOURCES}
        ${PCSX_VENDOR_ZLIB_SOURCES}
        ${PCSX_VENDOR_CHD_SOURCES}
        ${PCSX_VENDOR_LZMA_SOURCES}
        ${PCSX_VENDOR_ZSTD_SOURCES}
        "${CMAKE_CURRENT_LIST_DIR}/jit/jit_xtensa.c"
        "${CMAKE_CURRENT_LIST_DIR}/jit/jit_compile.c"
        "${CMAKE_CURRENT_LIST_DIR}/jit/jit_psx.c"
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        NO_FRONTEND=1
        NO_DYLIB=1
        DRC_DISABLE=1
        HAVE_RETROGO=1
        JIT_XTENSA=1
        JIT_XTENSA_INCLUDE_PCSX_HEADERS=1
        JIT_XTENSA_USE_WRAPPER=1
        P_HAVE_MMAP=0
        P_HAVE_POSIX_MEMALIGN=1
        DISABLE_MEM_LUTS=0
          HAVE_UNISTD_H=1
          USE_GPULIB=1
          GPU_UNAI_NO_OLD=1
          HAVE_CHD=1
          _FILE_OFFSET_BITS=64
          Z7_ST=1
          ZSTD_DISABLE_ASM=1
          RG_PSX_DEFAULT_FRAMESKIP=0
          RG_PSX_CYCLE_MULTIPLIER=500
          RG_PSX_SCALE_HIRES=0
          RG_PSX_FAST_MDEC=0
          RG_PSX_MAX_RENDER_WIDTH=320
          RG_PSX_MAX_RENDER_HEIGHT=240
          RG_PSX_TARGET_WIDTH=320
          RG_PSX_TARGET_HEIGHT=240
          RG_PSX_SPU_REVERB=0
          RG_PSX_SPU_INTERPOLATION=0
          RG_PSX_CDREAD_CACHE_SECTORS=64
          RG_PSX_DISABLE_AUDIO=0
          NDEBUG=1
      )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/jit/jit_xtensa_config_esp32s3.h"
        -O3
        -fomit-frame-pointer
        -fno-strict-aliasing
        -fno-strict-overflow
        -fno-math-errno
        -fno-trapping-math
        -Wno-error=maybe-uninitialized
        -Wno-error=unused-function
        -Wno-error=unused-variable
        -Wno-error=unused-but-set-variable
        -Wno-error=stringop-overflow
        -Wno-error=stringop-truncation
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
else()
    message(STATUS "PCSX ReARMed vendor tree not present, building PSX stub")
endif()

rg_setup_compile_options(-O3 -Wno-error=format)
