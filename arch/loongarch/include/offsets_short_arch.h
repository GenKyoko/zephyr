/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Short offsets used by the LoongArch context switch / exception entry code.
 */

#ifndef ZEPHYR_ARCH_LOONGARCH_INCLUDE_OFFSETS_SHORT_ARCH_H_
#define ZEPHYR_ARCH_LOONGARCH_INCLUDE_OFFSETS_SHORT_ARCH_H_

#include <zephyr/offsets.h>

#define _thread_offset_to_sp \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_sp_OFFSET)
#define _thread_offset_to_ra \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_ra_OFFSET)
#define _thread_offset_to_fp \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_fp_OFFSET)
#define _thread_offset_to_s0 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s0_OFFSET)
#define _thread_offset_to_s1 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s1_OFFSET)
#define _thread_offset_to_s2 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s2_OFFSET)
#define _thread_offset_to_s3 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s3_OFFSET)
#define _thread_offset_to_s4 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s4_OFFSET)
#define _thread_offset_to_s5 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s5_OFFSET)
#define _thread_offset_to_s6 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s6_OFFSET)
#define _thread_offset_to_s7 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s7_OFFSET)
#define _thread_offset_to_s8 \
	(___thread_t_callee_saved_OFFSET + ___callee_saved_t_s8_OFFSET)

#define _thread_offset_to_switch_handle \
	(___thread_t_switch_handle_OFFSET)

/* Offset of the general purpose register $rN inside struct arch_esf */
#define _esf_reg_offset(n) \
	(__struct_arch_esf_regs_OFFSET + (n) * 8)

#endif /* ZEPHYR_ARCH_LOONGARCH_INCLUDE_OFFSETS_SHORT_ARCH_H_ */
