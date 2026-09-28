# SPDX-License-Identifier: Apache-2.0

# 64-bit LoongArch, ISA v1.0, hard-float double ABI.
list(APPEND TOOLCHAIN_C_FLAGS  -march=la64v1.0 -mtune=loongarch64)
list(APPEND TOOLCHAIN_LD_FLAGS -march=la64v1.0 -mtune=loongarch64)

list(APPEND TOOLCHAIN_C_FLAGS  -mabi=lp64d)
list(APPEND TOOLCHAIN_LD_FLAGS -mabi=lp64d)

list(APPEND TOOLCHAIN_C_FLAGS  -mcmodel=normal)
list(APPEND TOOLCHAIN_LD_FLAGS -mcmodel=normal)

if(CONFIG_LOONGARCH_LSX)
  list(APPEND TOOLCHAIN_C_FLAGS -mlsx)
else()
  list(APPEND TOOLCHAIN_C_FLAGS -mno-lsx)
endif()

if(CONFIG_LOONGARCH_LASX)
  list(APPEND TOOLCHAIN_C_FLAGS -mlasx)
else()
  list(APPEND TOOLCHAIN_C_FLAGS -mno-lasx)
endif()

list(APPEND TOOLCHAIN_C_FLAGS -fno-pic)
