# P4-as-C6-flasher: a tiny ESP-IDF app that turns the ESP32-P4 on the
# CrowPanel into a transparent USB-CDC <-> UART1 bridge so the host PC
# can flash/monitor the on-board ESP32-C6 over the P4's USB-C port.
#
# This is meant to be flashed onto the P4 only when you need to flash
# the C6. Flash the factory firmware back when you're done.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(p4_c6_flasher)
