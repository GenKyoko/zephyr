# SPDX-License-Identifier: Apache-2.0

set(SUPPORTED_EMU_PLATFORMS qemu)

# The emulator binary is qemu-system-loongarch64 while ARCH is "loongarch"
set(QEMU_binary_suffix loongarch64)
set(QEMU_CPU_TYPE_${ARCH} la464)

set(QEMU_FLAGS_${ARCH}
  -machine virt
  -cpu ${QEMU_CPU_TYPE_${ARCH}}
  -m 256
  )

board_set_debugger_ifnset(qemu)
