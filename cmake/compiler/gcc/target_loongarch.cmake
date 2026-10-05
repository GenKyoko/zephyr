# SPDX-License-Identifier: Apache-2.0

#
# Map the LoongArch Kconfig options described in the GCC "LoongArch Options"
# section onto -m* compiler and linker switches.
#
# Every option is emitted explicitly (both the positive and the negated form),
# so that the generated code depends only on the Zephyr configuration and not
# on the default of the toolchain in use.
#

# -march: instruction set level or processor model
if(CONFIG_LOONGARCH_MARCH_LA464)
  set(loongarch_march la464)
elseif(CONFIG_LOONGARCH_MARCH_LA664)
  set(loongarch_march la664)
elseif(CONFIG_LOONGARCH_MARCH_LOONGARCH64)
  set(loongarch_march loongarch64)
elseif(CONFIG_LOONGARCH_MARCH_LA64V1_1)
  set(loongarch_march la64v1.1)
else()
  set(loongarch_march la64v1.0)
endif()

# -mtune: instruction scheduling / cost model
if(CONFIG_LOONGARCH_MTUNE_GENERIC)
  set(loongarch_mtune generic)
elseif(CONFIG_LOONGARCH_MTUNE_LA464)
  set(loongarch_mtune la464)
elseif(CONFIG_LOONGARCH_MTUNE_LA664)
  set(loongarch_mtune la664)
else()
  set(loongarch_mtune loongarch64)
endif()

# -mcmodel
if(CONFIG_LOONGARCH_CMODEL_MEDIUM)
  set(loongarch_mcmodel medium)
elseif(CONFIG_LOONGARCH_CMODEL_LARGE)
  set(loongarch_mcmodel large)
elseif(CONFIG_LOONGARCH_CMODEL_EXTREME)
  set(loongarch_mcmodel extreme)
elseif(CONFIG_LOONGARCH_CMODEL_TINY)
  set(loongarch_mcmodel tiny)
elseif(CONFIG_LOONGARCH_CMODEL_TINY_STATIC)
  set(loongarch_mcmodel tiny-static)
else()
  set(loongarch_mcmodel normal)
endif()

# -mabi: how floating point values are passed between functions
if(CONFIG_LOONGARCH_FLOAT_ABI_LP64F)
  set(loongarch_mabi lp64f)
elseif(CONFIG_LOONGARCH_FLOAT_ABI_LP64S)
  set(loongarch_mabi lp64s)
else()
  set(loongarch_mabi lp64d)
endif()

# -mfpu: whether the compiler may emit floating point instructions at all.
# This follows CONFIG_FPU (and not the ABI), so that the assembler accepts
# floating point code whenever the hardware unit is enabled, e.g. in
# arch/loongarch/core/switch.S and fpu.c.
if(CONFIG_FPU)
  if(CONFIG_LOONGARCH_FLOAT_ABI_LP64F)
    set(loongarch_mfpu 32)
  else()
    set(loongarch_mfpu 64)
  endif()
else()
  set(loongarch_mfpu none)
endif()

# -msimd: vector extension (LASX implies LSX)
if(CONFIG_LOONGARCH_LASX)
  set(loongarch_msimd lasx)
elseif(CONFIG_LOONGARCH_LSX)
  set(loongarch_msimd lsx)
else()
  set(loongarch_msimd none)
endif()

list(APPEND LOONGARCH_C_FLAGS
  -march=${loongarch_march}
  -mtune=${loongarch_mtune}
  -mabi=${loongarch_mabi}
  -mfpu=${loongarch_mfpu}
  -msimd=${loongarch_msimd}
  -mcmodel=${loongarch_mcmodel}
  )

# Relocation handling
if(CONFIG_LOONGARCH_EXPLICIT_RELOCS_ALWAYS)
  list(APPEND LOONGARCH_C_FLAGS -mexplicit-relocs=always)
elseif(CONFIG_LOONGARCH_EXPLICIT_RELOCS_NONE)
  list(APPEND LOONGARCH_C_FLAGS -mexplicit-relocs=none)
else()
  list(APPEND LOONGARCH_C_FLAGS -mexplicit-relocs=auto)
endif()

if(CONFIG_LOONGARCH_RELAX)
  list(APPEND LOONGARCH_C_FLAGS -mrelax)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-relax)
endif()

if(CONFIG_LOONGARCH_DIRECT_EXTERN_ACCESS)
  list(APPEND LOONGARCH_C_FLAGS -mdirect-extern-access)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-direct-extern-access)
endif()

# Memory access and barrier behavior
if(CONFIG_LOONGARCH_STRICT_ALIGN)
  list(APPEND LOONGARCH_C_FLAGS -mstrict-align)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-strict-align)
endif()

if(CONFIG_LOONGARCH_LD_SEQ_SA)
  list(APPEND LOONGARCH_C_FLAGS -mld-seq-sa)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-ld-seq-sa)
endif()

if(CONFIG_LOONGARCH_MEMCPY)
  list(APPEND LOONGARCH_C_FLAGS -mmemcpy)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-memcpy)
endif()

if(CONFIG_LOONGARCH_MAX_INLINE_MEMCPY_SIZE GREATER 0)
  list(APPEND LOONGARCH_C_FLAGS
    -mmax-inline-memcpy-size=${CONFIG_LOONGARCH_MAX_INLINE_MEMCPY_SIZE}
    )
endif()

# Atomic memory operations
if(CONFIG_LOONGARCH_LAM_BH)
  list(APPEND LOONGARCH_C_FLAGS -mlam-bh)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-lam-bh)
endif()

if(CONFIG_LOONGARCH_LAMCAS)
  list(APPEND LOONGARCH_C_FLAGS -mlamcas)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-lamcas)
endif()

# Integer division
if(CONFIG_LOONGARCH_CHECK_ZERO_DIVISION)
  list(APPEND LOONGARCH_C_FLAGS -mcheck-zero-division)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-check-zero-division)
endif()

# Floating point
if(CONFIG_LOONGARCH_RECIP)
  if(CONFIG_LOONGARCH_RECIP_OPTIONS STREQUAL "")
    list(APPEND LOONGARCH_C_FLAGS -mrecip)
  else()
    list(APPEND LOONGARCH_C_FLAGS -mrecip=${CONFIG_LOONGARCH_RECIP_OPTIONS})
  endif()
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-recip)
endif()

if(CONFIG_LOONGARCH_FRECIPE)
  list(APPEND LOONGARCH_C_FLAGS -mfrecipe)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-frecipe)
endif()

if(CONFIG_LOONGARCH_COND_MOVE_FLOAT)
  list(APPEND LOONGARCH_C_FLAGS -mcond-move-float)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-cond-move-float)
endif()

if(CONFIG_LOONGARCH_COND_MOVE_INT)
  list(APPEND LOONGARCH_C_FLAGS -mcond-move-int)
else()
  list(APPEND LOONGARCH_C_FLAGS -mno-cond-move-int)
endif()

# Branch cost model
if(CONFIG_LOONGARCH_BRANCH_COST GREATER 0)
  list(APPEND LOONGARCH_C_FLAGS -mbranch-cost=${CONFIG_LOONGARCH_BRANCH_COST})
endif()

# Thread local storage access model
if(CONFIG_LOONGARCH_TLS_DIALECT_DESC)
  list(APPEND LOONGARCH_C_FLAGS -mtls-dialect=desc)
else()
  list(APPEND LOONGARCH_C_FLAGS -mtls-dialect=trad)
endif()

# Flags that only affect the compilation of C code
list(APPEND LOONGARCH_EXTRA_C_FLAGS -fno-pic)

if(CONFIG_LOONGARCH_FP_CONTRACT_OFF)
  list(APPEND LOONGARCH_EXTRA_C_FLAGS -ffp-contract=off)
elseif(CONFIG_LOONGARCH_FP_CONTRACT_ON)
  list(APPEND LOONGARCH_EXTRA_C_FLAGS -ffp-contract=on)
else()
  list(APPEND LOONGARCH_EXTRA_C_FLAGS -ffp-contract=fast)
endif()

list(APPEND TOOLCHAIN_C_FLAGS ${LOONGARCH_C_FLAGS} ${LOONGARCH_EXTRA_C_FLAGS})
list(APPEND TOOLCHAIN_GROUPED_LD_FLAGS LOONGARCH_C_FLAGS)
