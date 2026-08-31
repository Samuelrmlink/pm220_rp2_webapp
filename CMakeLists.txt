# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# SDK 2.3.0 needs picotool 2.3.0; ~/.pico-sdk only has 2.2.0-a4, so let CMake fetch 2.3.0.
unset(picotool_DIR CACHE)
set(PICOTOOL_FORCE_FETCH_FROM_GIT ON CACHE BOOL "" FORCE)
# ====================================================================================

cmake_minimum_required(VERSION 3.13)
# Dev board: Pimoroni Pico Plus 2 W (16 MB). Production size-check: -DPICO_BOARD=pico2_w
set(PICO_BOARD pimoroni_pico_plus2_w_rp2350 CACHE STRING "Board type")

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

project(pm220 C CXX ASM)

pico_sdk_init()

add_executable(pm220
    src/main.c
    src/net/dhcpserver.c
    src/net/http.c
    src/net/mdns.c
    src/bt/bt_core.c
    src/printer/tspl.c
    src/printer/job.c
    src/usb/usb_descriptors.c
    src/net/usb_ncm.c
)

target_include_directories(pm220 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(pm220 PRIVATE
    pico_stdlib
    pico_unique_id
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mdns
    pico_btstack_classic
    pico_btstack_cyw43
)

target_compile_definitions(pm220 PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF=4
)

pico_enable_stdio_usb(pm220 1)
pico_enable_stdio_uart(pm220 0)
pico_add_extra_outputs(pm220)
