cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Target board")

# Use the pico-sdk submodule unless the caller points elsewhere
if (NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
endif()
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(voice_keyboard C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(voice-keyboard
    src/main.c
    src/typer.c
    src/usb_descriptors.c
)

# tusb_config.h is looked up on the include path
target_include_directories(voice-keyboard PRIVATE src)

target_link_libraries(voice-keyboard PRIVATE
    pico_stdlib
    tinyusb_device
)

# Logs (printf) and stage 1 text input go over the CDC serial interface.
# Opening the port at 1200 baud reboots into BOOTSEL for flashing.
pico_enable_stdio_usb(voice-keyboard 1)
pico_enable_stdio_uart(voice-keyboard 0)
target_compile_definitions(voice-keyboard PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
)

pico_add_extra_outputs(voice-keyboard)
