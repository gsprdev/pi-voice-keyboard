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

# Microphone timing: "standard" I2S (ICS-43434, INMP441, ...) samples data on
# the rising clock edge; "sph0645" samples on the falling edge.
set(MIC_TIMING sph0645 CACHE STRING "Microphone I2S timing: standard or sph0645")
set_property(CACHE MIC_TIMING PROPERTY STRINGS standard sph0645)

add_executable(voice-keyboard
    src/feedback.c
    src/main.c
    src/mic.c
    src/ptt.c
    src/service.c
    src/typer.c
    src/usb_descriptors.c
)

pico_generate_pio_header(voice-keyboard ${CMAKE_CURRENT_LIST_DIR}/src/mic_i2s.pio)

# tusb_config.h is looked up on the include path
target_include_directories(voice-keyboard PRIVATE src)

target_link_libraries(voice-keyboard PRIVATE
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_pwm
    tinyusb_device
)

# Wi-Fi boards: stream to the transcription service. Other boards build
# without networking (useful for testing the keyboard and microphone).
if (PICO_CYW43_SUPPORTED)
    if (NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/secrets.h)
        message(FATAL_ERROR "Wi-Fi settings missing: copy src/secrets.h.example to src/secrets.h and fill it in")
    endif()
    target_sources(voice-keyboard PRIVATE src/http.c src/net.c)
    target_link_libraries(voice-keyboard PRIVATE pico_cyw43_arch_lwip_poll)
    target_compile_definitions(voice-keyboard PRIVATE
        VOICEKB_NETWORK=1
        CYW43_HOST_NAME="voice-keyboard"
    )
else()
    target_compile_definitions(voice-keyboard PRIVATE VOICEKB_NETWORK=0)
endif()

# Logs (printf) and stage 1 text input go over the first CDC serial interface.
# Opening the port at 1200 baud reboots into BOOTSEL for flashing.
pico_enable_stdio_usb(voice-keyboard 1)
pico_enable_stdio_uart(voice-keyboard 0)
target_compile_definitions(voice-keyboard PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
    MIC_SAMPLE_ON_FALLING_EDGE=$<STREQUAL:${MIC_TIMING},sph0645>
)

pico_add_extra_outputs(voice-keyboard)
