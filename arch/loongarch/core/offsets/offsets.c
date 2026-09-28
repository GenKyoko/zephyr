/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch kernel structure member offset definition file.
 */

#include <kernel_arch_data.h>
#include <gen_offset.h>
#include <kernel_offsets.h>

/* struct _callee_saved member offsets */
GEN_OFFSET_SYM(_callee_saved_t, sp);
GEN_OFFSET_SYM(_callee_saved_t, ra);
GEN_OFFSET_SYM(_callee_saved_t, fp);
GEN_OFFSET_SYM(_callee_saved_t, s0);
GEN_OFFSET_SYM(_callee_saved_t, s1);
GEN_OFFSET_SYM(_callee_saved_t, s2);
GEN_OFFSET_SYM(_callee_saved_t, s3);
GEN_OFFSET_SYM(_callee_saved_t, s4);
GEN_OFFSET_SYM(_callee_saved_t, s5);
GEN_OFFSET_SYM(_callee_saved_t, s6);
GEN_OFFSET_SYM(_callee_saved_t, s7);
GEN_OFFSET_SYM(_callee_saved_t, s8);

/* struct arch_esf member offsets */
GEN_OFFSET_STRUCT(arch_esf, regs);
GEN_OFFSET_STRUCT(arch_esf, orig_a0);
GEN_OFFSET_STRUCT(arch_esf, csr_era);
GEN_OFFSET_STRUCT(arch_esf, csr_badv);
GEN_OFFSET_STRUCT(arch_esf, csr_prmd);
GEN_OFFSET_STRUCT(arch_esf, csr_estat);

GEN_ABSOLUTE_SYM(__struct_arch_esf_SIZEOF,
		 STACK_ROUND_UP(sizeof(struct arch_esf)));

GEN_ABS_SYM_END
