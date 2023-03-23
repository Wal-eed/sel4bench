#
# Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)
#
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.7.2)

project(irqcold C)

set(configure_string "")
config_option(
    AppIrqColdBench
    APP_IRQCOLDBENCH
    "Application to benchmark seL4 IRQs without modification to the kernel with a cold cache."
    DEFAULT
    ON
    DEPENDS
    "DefaultBenchDeps"
)
add_config_library(sel4benchirqcold "${configure_string}")

file(GLOB deps src/*.c)
list(SORT deps)
add_executable(irqcold EXCLUDE_FROM_ALL ${deps})
target_link_libraries(
    irqcold
    sel4_autoconf
    sel4benchirqcold_Config
    sel4
    muslc
    sel4vka
    sel4allocman
    sel4utils
    sel4simple
    sel4muslcsys
    sel4platsupport
    platsupport
    sel4vspace
    sel4benchsupport
    sel4debug
)

if(AppIrqColdBench)
    set_property(GLOBAL APPEND PROPERTY sel4benchapps_property "$<TARGET_FILE:irqcold>")
endif()
