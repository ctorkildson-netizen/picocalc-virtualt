# VirtualT (TRS-80 Model 100 emulator) for the ClockworkPi PicoCalc, RP2350.
# VirtualT's CPU/IO core (core/, BSD licence, Stephen Hurd and Ken Pettit),
# the PicoCalc platform layer (pico_main.c), and Blair Leduc's drivers and
# shell header as submodules (modules/).
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(PICO_BOARD pico2_w CACHE STRING "Board type")   # RP2350W: WiFi for NTP
include(pico_sdk_import.cmake)
project(picocalc-virtualt C CXX ASM)
pico_sdk_init()

set(DRV modules/picocalc-text-starter/drivers)

# The SD card: the text starter (v0.17 and later) uses ChaN's FatFs, built
# as its own CMakeLists.txt does, from the Pico SDK's copy with the
# starter's ffconf.h
set(FATFS_SRC ${PICO_SDK_PATH}/lib/tinyusb/lib/fatfs/source)
set(FATFS_DIR ${CMAKE_BINARY_DIR}/fatfs)
foreach(f ff.c ff.h ffunicode.c diskio.h)
    configure_file(${FATFS_SRC}/${f} ${FATFS_DIR}/${f} COPYONLY)
endforeach()
configure_file(${DRV}/fatfs/ffconf.h ${FATFS_DIR}/ffconf.h COPYONLY)
add_executable(picocalc-virtualt
    pico_main.c vt_config.c
    core/vt_glue.c core/mem_vt.c core/lcdtext.c core/vt_export.c core/vt_basic.c core/clock_vt.c core/io.c core/doins.c
    core/m100rom.c core/m200rom.c core/n8201rom.c core/m10rom.c
    core/kc85rom.c core/n8300rom.c core/romstrings.c
    ${DRV}/audio.c ${DRV}/clib.c ${DRV}/font-5x10.c ${DRV}/font-8x10.c
    ${DRV}/lcd.c ${DRV}/sdcard.c ${DRV}/southbridge.c
    ${DRV}/diskio.c ${DRV}/filesystem.c ${FATFS_DIR}/ff.c ${FATFS_DIR}/ffunicode.c
)
# VirtualT's core is old C with lots of benign warnings; keep it quiet.
set_source_files_properties(core/vt_glue.c core/mem_vt.c core/lcdtext.c core/vt_export.c core/vt_basic.c core/clock_vt.c core/io.c core/doins.c
    core/m100rom.c core/m200rom.c core/n8201rom.c core/m10rom.c
    core/kc85rom.c core/n8300rom.c core/romstrings.c
    PROPERTIES COMPILE_OPTIONS "-w;-O2;-fsigned-char")
# VirtualT was written on x86, where plain char is signed; ARM defaults to
# unsigned, which corrupts BASIC memory pointers. Match x86.
target_compile_definitions(picocalc-virtualt PRIVATE __unix__ PICO_BUILD PICOCALC_APP_NAME=\"virtualt\")
target_include_directories(picocalc-virtualt PRIVATE
    ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/core
    ${CMAKE_CURRENT_LIST_DIR}/modules/picocalc-text-starter
    ${CMAKE_CURRENT_LIST_DIR}/modules/picocalc-shell/include
    ${FATFS_DIR})
pico_set_program_name(picocalc-virtualt "picocalc-virtualt")
pico_enable_stdio_uart(picocalc-virtualt 0)
pico_enable_stdio_usb(picocalc-virtualt 0)
target_link_libraries(picocalc-virtualt pico_stdlib pico_printf hardware_gpio
    hardware_i2c hardware_spi hardware_pio hardware_clocks hardware_watchdog hardware_powman
    pico_cyw43_arch_lwip_poll pico_lwip_sntp pico_aon_timer)
pico_generate_pio_header(picocalc-virtualt ${CMAKE_CURRENT_LIST_DIR}/${DRV}/audio.pio)
pico_add_extra_outputs(picocalc-virtualt)
