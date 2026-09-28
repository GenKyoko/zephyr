# SPDX-License-Identifier: Apache-2.0

add_subdirectory(core)
zephyr_include_directories(include)

if(CONFIG_64BIT)
set_property(GLOBAL PROPERTY PROPERTY_OUTPUT_FORMAT elf64-loongarch)
else()
set_property(GLOBAL PROPERTY PROPERTY_OUTPUT_FORMAT elf32-loongarch)
endif()