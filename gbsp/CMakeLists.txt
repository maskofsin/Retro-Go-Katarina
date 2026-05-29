cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED RG_PROJECT_APP)
    set(RG_PROJECT_APP gbsp)
endif()
if(NOT DEFINED RG_PROJECT_VER)
    set(RG_PROJECT_VER 1.1)
endif()

set(COMPONENTS
    main
    retro-go
    gbsp-libretro
    app_trace
    bootloader
    esptool_py
)
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(gbsp)
