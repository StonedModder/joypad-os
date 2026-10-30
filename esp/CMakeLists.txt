cmake_minimum_required(VERSION 3.16)

# Dynamic project name based on CONFIG_APP (default: bt2usb)
if(NOT DEFINED CONFIG_APP)
    set(CONFIG_APP "bt2usb")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(joypad_${CONFIG_APP})

# Allow the USB output mode to be fixed at build time, e.g.
#   idf.py -DJP_USBD_DEFAULT_MODE=USB_OUTPUT_MODE_XINPUT build
# Needed for an unattended rig: the stock default is HID and the only other way
# to change it is double-clicking the BOOT button.
if(DEFINED JP_USBD_DEFAULT_MODE)
    idf_build_set_property(COMPILE_DEFINITIONS
        "USBD_DEFAULT_MODE=${JP_USBD_DEFAULT_MODE}" APPEND)
    message(STATUS "USBD_DEFAULT_MODE=${JP_USBD_DEFAULT_MODE}")
endif()
