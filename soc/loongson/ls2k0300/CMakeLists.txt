# SPDX-License-Identifier: Apache-2.0

zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/ls2k0300/soc.c)

# pinctrl_configure_pins() for the GPIO_CFG pin mux block. The pinctrl framework
# only needs this function and the controller's reg address, so it lives here
# rather than in a separate driver.
zephyr_library_sources_ifdef(CONFIG_PINCTRL ${CMAKE_CURRENT_LIST_DIR}/pinctrl.c)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/loongarch/linker.ld CACHE INTERNAL "")

# u-boot is loaded with the legacy image format, so emit zephyr.uImage next to
# zephyr.elf/zephyr.bin. The load address and entry point are taken from the
# ELF file, see gen_uimage.py.
if(CONFIG_LOONGARCH_UBOOT_UIMAGE)
  set(uboot_uimage ${CMAKE_BINARY_DIR}/zephyr/${KERNEL_NAME}.uImage)

  set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_uimage.py
    --elf ${CMAKE_BINARY_DIR}/zephyr/${KERNEL_ELF_NAME}
    --bin ${CMAKE_BINARY_DIR}/zephyr/${KERNEL_BIN_NAME}
    --output ${uboot_uimage}
  )

  set_property(GLOBAL APPEND PROPERTY extra_post_build_byproducts ${uboot_uimage})
endif()
