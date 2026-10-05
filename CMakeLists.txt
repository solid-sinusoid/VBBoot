cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

set(BOOTLOADER_FDCAN_ID_FORMAT "standard" CACHE STRING "Bootloader FDCAN ID format: standard or extended")
set_property(CACHE BOOTLOADER_FDCAN_ID_FORMAT PROPERTY STRINGS standard extended)
if(NOT BOOTLOADER_FDCAN_ID_FORMAT MATCHES "^(standard|extended)$")
    message(FATAL_ERROR "BOOTLOADER_FDCAN_ID_FORMAT must be 'standard' or 'extended'")
endif()
option(BOOTLOADER_USE_EEPROM_CONFIG "Load bootloader CAN node id and rates from VBDrive EEPROM config" ON)
set(BOOTLOADER_MCU "STM32G431xx" CACHE STRING "STM32 device compile definition")
set(BOOTLOADER_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/stm32g431vbtx_flash.ld" CACHE FILEPATH "Bootloader linker script")
set(BOOTLOADER_APP_END "0x08020000UL" CACHE STRING "Exclusive application flash end")
set(BOOTLOADER_RAM_END "0x20008000UL" CACHE STRING "Exclusive application RAM end")
set(BOOTLOADER_FDCAN_CLOCK_SOURCE "PCLK1" CACHE STRING "FDCAN clock source: PCLK1 or PLL")
set(BOOTLOADER_STARTUP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/startup_stm32g431xx.s" CACHE FILEPATH "MCU startup assembly")

set(CMAKE_PROJECT_NAME VBBoot)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(${CMAKE_PROJECT_NAME} LANGUAGES C ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

add_executable(${CMAKE_PROJECT_NAME})
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    OUTPUT_NAME ${CMAKE_PROJECT_NAME}
    SUFFIX ".elf"
)
add_subdirectory(cmake/stm32cubemx)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    stm32cubemx
)

target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
)

target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    "LINKER:-Map=${CMAKE_PROJECT_NAME}.map"
)

set(USER_SOURCES
    App/app.c
    App/communications.c
    App/fdcan_msp.c
    App/state_manager.c
    App/bootloader_runtime.c
    App/runtime_stubs.c
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${USER_SOURCES}
)

set(VB_CONFIG_ADDRESS "0" CACHE STRING "Configuration byte address in storage")
if(NOT VB_CONFIG_ADDRESS MATCHES "^(0[xX][0-9a-fA-F]+|[0-9]+)$")
    message(FATAL_ERROR "VB_CONFIG_ADDRESS must be a non-negative decimal or hexadecimal address")
endif()
math(EXPR VB_CONFIG_ADDRESS_VALUE "${VB_CONFIG_ADDRESS}")
if(VB_CONFIG_ADDRESS_VALUE LESS 0 OR VB_CONFIG_ADDRESS_VALUE GREATER 65508)
    message(FATAL_ERROR "VB_CONFIG_ADDRESS exceeds the 16-bit EEPROM address range")
endif()

set(LIBVOLTBRO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/libvoltbro" CACHE PATH "libvoltbro shared config headers")
if(NOT EXISTS "${LIBVOLTBRO_DIR}/voltbro/config/base_config.h")
    message(FATAL_ERROR "Set LIBVOLTBRO_DIR to libvoltbro with the shared BaseConfigData header")
endif()
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App
    ${LIBVOLTBRO_DIR}
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    -DUSE_HAL_DRIVER
    -DSTM32G4
    -D${BOOTLOADER_MCU}
    -DAPP_END_ADDR=${BOOTLOADER_APP_END}
    -DAPP_RAM_END_ADDR=${BOOTLOADER_RAM_END}
)
if(BOOTLOADER_FDCAN_CLOCK_SOURCE STREQUAL "PLL")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE -DBOOTLOADER_FDCAN_CLOCK_PLL=1 -DBOOTLOADER_BOARD_RUKA2=1)
endif()
if(BOOTLOADER_FDCAN_ID_FORMAT STREQUAL "extended")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE -DBOOTLOADER_FDCAN_ID_FORMAT_EXTENDED)
endif()
if(BOOTLOADER_USE_EEPROM_CONFIG)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE -DBOOTLOADER_USE_EEPROM_CONFIG=1)
endif()

add_compile_options(
    -mfloat-abi=hard   # Use hardware FPU
    -mfpu=fpv4-sp-d16  # For Cortex-M4
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>: -Os -flto -fomit-frame-pointer>
    $<$<CONFIG:RelWithDebInfo>: -Os -g0 -flto>
    $<$<CONFIG:Debug>: -Os -g3 -flto>
)

if (NOT CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
set_source_files_properties(App/bootloader_runtime.c PROPERTIES COMPILE_OPTIONS "-fno-lto")
# communications.c accesses FDCAN message RAM (hardware-written memory).
# LTO treats message RAM as always-zero (only CPU zeroes it; hardware writes are invisible),
# causing Identifier checks and boot_process_command to be eliminated as dead code.
set_source_files_properties(App/communications.c PROPERTIES COMPILE_OPTIONS "-fno-lto")
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:-flto>
    $<$<CONFIG:RelWithDebInfo>:-flto>
    $<$<CONFIG:Debug>:-flto>
)

set(
    CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -mfloat-abi=hard -mfpu=fpv4-sp-d16"
)

add_custom_command(
    POST_BUILD
    TARGET ${CMAKE_PROJECT_NAME}
    COMMAND arm-none-eabi-objcopy -O ihex ${CMAKE_PROJECT_NAME}.elf ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary ${CMAKE_PROJECT_NAME}.elf ${CMAKE_PROJECT_NAME}.bin
    COMMAND ${CMAKE_COMMAND}
            -DBIN_FILE=${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
            -DMAX_BOOT_SIZE=12000
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_boot_size.cmake
)


target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE VB_CONFIG_ADDRESS=${VB_CONFIG_ADDRESS_VALUE})
