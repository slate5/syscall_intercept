/*
 * Copyright 2016-2024, Intel Corporation
 * Contributor: Petar Andrić
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * patcher.c -- patching a library
 *
 * Jumping from the subject library:
 * TODO: the RISC-V version differs greatly, create a new table
 *
 *     /--------------------------\
 *     |               subject.so |
 *     |                          |
 *     |  jmp to_trampoline_table |  patched by activate_patches()
 *  /->|   |                      |
 *  |  \___|______________________/
 *  |      |
 *  |  /---|--------------------------\
 *  |  | movabs %r11, wrapper_address | jmp generated by activate_patches()
 *  |  | jmp *%r11                    | This allows subject.so and
 *  |  |   |                          | libsyscall_intercept.so to be farther
 *  |  \___|__________________________/ than 2 gigabytes from each other
 *  |      |
 *  |  /---|-----------------------------\
 *  |  |   |  libsyscall_intercept.so    |
 *  |  |   |                             |
 *  |  | /-|--------------------------\  |
 *  |  | | |  static unsigned char    |  |
 *  |  | | |  asm_wrapper_space[]     |  |
 *  |  | | |    in BSS                |  | wrapper routine
 *  |  | | |                          |  | generated into asm_wrapper_space
 *  |  | | |                          |  | by create_wrapper()
 *  |  | |wrapper routine             |  |
 *  |  | |calls C hook function  ----------> intercept_routine in intercept.c
 *  |  | |movabs %r11, return_address |  |
 *  |  | |jmp *%r11                   |  |
 *  |  | \_|__________________________/  |
 *  |  \___|_____________________________/
 *  |      |
 *  \______/
 *
 */

#include "intercept.h"
#include "intercept_util.h"
#include "intercept_log.h"
#include "rv_encode.h"

#include <assert.h>
#include <stdint.h>
#include <syscall.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>

#include <stdio.h>

/*
 * While executing patched instructions, these global variables are used in the
 * relocation space (intercept_irq_entry) in case the patched instructions use
 * ra. The ra register is the only one that lacks the original value from glibc
 * during patch execution, as it’s needed for jumps within intercept_irq_entry.
 * These globals serve to temporarily load and store the original ra when it’s
 * required by the patched instructions.
 */
extern __thread uint64_t asm_ra_orig;
extern __thread uint64_t asm_ra_temp;

struct tls_offset_table {
	ptrdiff_t asm_ra_orig;
	ptrdiff_t asm_ra_temp;
} tls_offset_table;

void
init_tls_offset_table(void)
{
	uintptr_t tp_addr = (uintptr_t)__builtin_thread_pointer();

	tls_offset_table.asm_ra_orig =
		(uintptr_t)&asm_ra_orig - tp_addr;
	tls_offset_table.asm_ra_temp =
		(uintptr_t)&asm_ra_temp - tp_addr;
}

/*
 * is_copiable_before_syscall
 * checks if an instruction found before a syscall instruction
 * can be copied (and thus overwritten).
 */
static bool
is_copiable_before_syscall(struct intercept_disasm_result ins)
{
	if (!ins.is_set)
		return false;

	return !(ins.has_ip_relative_opr || ins.is_abs_jump || ins.is_syscall);
}

/*
 * is_copiable_after_syscall
 * checks if an instruction found after a syscall instruction
 * can be copied (and thus overwritten).
 *
 * Notice: we allow the copy of ret instructions.
 */
static bool
is_copiable_after_syscall(struct intercept_disasm_result ins)
{
	if (!ins.is_set)
		return false;

	return !(ins.has_ip_relative_opr || ins.is_syscall);
}

static bool
is_SML_patchable(struct patch_desc *patch, uint8_t patchable_size)
{
	if (patch->syscall_num < 0)
		return false;
	else if (patchable_size <= JAL_INS_SIZE)
		return false;
	else if (!patch->return_register &&
			(patchable_size == JAL_INS_SIZE + C_LI_INS_SIZE &&
			patch->syscall_num > 31))
		return false;

	return true;
}

static uint8_t
check_two_ecalls(struct patch_desc *patch, uint8_t syscall_idx,
			uint8_t start_idx, uint8_t second_ecall_idx)
{
	struct intercept_disasm_result *instrs = patch->surrounding_instrs;

	// when a7 is not obtained, force the TYPE_MID
	uint8_t before_2nd_ecall_size = 0;
	if (patch->syscall_num < 0) {
		for (uint8_t i = start_idx; i < second_ecall_idx; ++i) {
			before_2nd_ecall_size += instrs[i].length;

			if (before_2nd_ecall_size >= TYPE_MID_SIZE)
				return i + 1;
		}
	}

	// when the TYPE_MID/TYPE_SML fits before the 1st ecall (best option)
	uint8_t up_to_ecall_size = 0;
	for (uint8_t i = start_idx; i <= syscall_idx; ++i) {
		up_to_ecall_size += instrs[i].length;

		if (up_to_ecall_size >= TYPE_MID_SIZE ||
				is_SML_patchable(patch, up_to_ecall_size))
			return syscall_idx + 1;
	}

	// as a last resort, fit TYPE_SML anywhere up to the 2nd ecall
	before_2nd_ecall_size = 0;
	for (uint8_t i = start_idx; i < second_ecall_idx; ++i) {
		before_2nd_ecall_size += instrs[i].length;

		if (is_SML_patchable(patch, before_2nd_ecall_size))
			return i + 1;
	}

	// failed: end_idx == start_idx
	return start_idx;
}

/*
 * check_surrounding_instructions
 * Sets up the following members in a patch_desc, based on
 * instruction being relocateable or not:
 * uses_prev_ins ; uses_prev_ins_2 ; uses_next_ins
 */
static uint8_t
check_surrounding_instructions(struct intercept_desc *desc,
				struct patch_desc *patch)
{
	struct intercept_disasm_result *instrs = patch->surrounding_instrs;
	uint8_t instrs_num = SURROUNDING_INSTRS_NUM;
	uint8_t syscall_idx = SYSCALL_IDX;
	uint8_t patch_start_idx = 0;
	uint8_t patch_end_idx = instrs_num;
	uint8_t patchable_size = 0;

	// check if the instruction after the ecall sets a register
	if (instrs[syscall_idx + 1].reg_set)
		patch->return_register = instrs[syscall_idx + 1].reg_set;

	for (uint8_t i = 0; i < instrs_num; ++i) {
		if (i < syscall_idx) {
			if (instrs[i].a7_set > -1)
				patch->syscall_num = instrs[i].a7_set;
			else if (instrs[i].is_a7_modified)
				patch->syscall_num = -1;

			if (has_jump(desc, instrs[i + 1].address)) {
				patch_start_idx = i + 1;
				patch->syscall_num = -1;
			} else if (!is_copiable_before_syscall(instrs[i])) {
				patch_start_idx = i + 1;
			}
		} else if (i > syscall_idx) {
			if (instrs[i].is_syscall) {
				patch_end_idx = check_two_ecalls(patch,
						syscall_idx, patch_start_idx, i);
				break;
			} else if (!is_copiable_after_syscall(instrs[i]) ||
					has_jump(desc, instrs[i].address)) {
				patch_end_idx = i;
				break;
			}
		}
	}

	// fix indexes according to patchable instrs
	syscall_idx = syscall_idx - patch_start_idx;
	patch->syscall_idx = syscall_idx;
	instrs_num = patch_end_idx - patch_start_idx;
	if (instrs_num < 1)
		return 0;

	// shift usable instrs to the left
	memmove(patch->surrounding_instrs, instrs + patch_start_idx,
		instrs_num * sizeof(struct intercept_disasm_result));

	// get final patchable size, and check if ra is used before ecall
	for (uint8_t i = 0; i < instrs_num; ++i) {
		patchable_size += instrs[i].length;

		if (instrs[i].is_ra_used) {
			if (i < syscall_idx)
				patch->is_ra_used_before = true;
			else
				patch->is_ra_used_after = true;
		}
	}

	return patchable_size;
}

static void
find_GW(struct intercept_desc *desc, struct patch_desc *patch)
{
	// TYPE_MID and TYPE_SML jump address and offset (TYPE_MID)
	const uint8_t *jump_from;
	if (patch->syscall_num == TYPE_MID)
		jump_from = patch->return_address - JAL_INS_SIZE +
				MODIFY_SP_INS_SIZE;
	else // TYPE_SML
		jump_from = patch->return_address - JAL_INS_SIZE;


	for (uint32_t patch_i = 0; patch_i < desc->count; ++patch_i) {
		struct patch_desc *patch_GW = desc->items + patch_i;

		// not a TYPE_GW, skip
		if (patch_GW->syscall_num != TYPE_GW)
			continue;

		if (labs(patch_GW->dst_jmp_patch - jump_from) < JAL_MID_REACH) {
			patch->dst_jmp_patch = patch_GW->dst_jmp_patch;
			break;
		}
	}

	// offsetting TYPE_MID to skip `addi sp, sp, -48`
	if (patch->syscall_num == TYPE_MID)
		patch->dst_jmp_patch += MODIFY_SP_INS_SIZE;
}

#ifdef __riscv_c
static void
check_patch_alignment(struct patch_desc *patch, const uint8_t *start_addr,
			uint8_t required_size)
{
	struct intercept_disasm_result *instrs = patch->surrounding_instrs;
	const uint8_t *end_addr = start_addr + required_size;
	patch->start_with_c_nop = true;
	patch->end_with_c_nop = true;

	for (uint8_t i = 0; i < SURROUNDING_INSTRS_NUM; ++i) {
		if (start_addr == instrs[i].address)
			patch->start_with_c_nop = false;
		else if (end_addr == instrs[i].address)
			patch->end_with_c_nop = false;
		else if (end_addr < instrs[i].address)
			break;
	}
}
#endif

static void
position_patch(struct patch_desc *patch)
{
	struct intercept_disasm_result *instrs = patch->surrounding_instrs;
	uint8_t up_to_ecall_size = 0;
	const uint8_t *start_addr;
	uint8_t required_size;

	for (uint8_t i = 0; i <= patch->syscall_idx; ++i)
		up_to_ecall_size += instrs[i].length;

	switch (patch->syscall_num) {
	case TYPE_GW:
		required_size = TYPE_GW_SIZE;

		if (up_to_ecall_size >= required_size)
			patch->return_address =
				patch->syscall_addr + ECALL_INS_SIZE -
				MODIFY_SP_INS_SIZE - STORE_LOAD_INS_SIZE;
		else
			patch->return_address =
				instrs[0].address + MODIFY_SP_INS_SIZE +
				STORE_LOAD_INS_SIZE + JUMP_2GB_INS_SIZE;

		start_addr = patch->return_address - JUMP_2GB_INS_SIZE -
				STORE_LOAD_INS_SIZE - MODIFY_SP_INS_SIZE;
		break;
	case TYPE_MID:
		required_size = TYPE_MID_SIZE;

		if (up_to_ecall_size >= required_size)
			patch->return_address =
				patch->syscall_addr + ECALL_INS_SIZE -
				MODIFY_SP_INS_SIZE - STORE_LOAD_INS_SIZE;
		else
			patch->return_address =
				instrs[0].address + MODIFY_SP_INS_SIZE +
				STORE_LOAD_INS_SIZE + JAL_INS_SIZE;

		start_addr = patch->return_address - JAL_INS_SIZE -
				STORE_LOAD_INS_SIZE - MODIFY_SP_INS_SIZE;
		break;
	default: // TYPE_SML
		if (patch->return_register)
			required_size = JAL_INS_SIZE;
#ifdef __riscv_c
		else if (patch->syscall_num < 32)
			required_size = JAL_INS_SIZE + C_LI_INS_SIZE;
#endif
		else
			required_size = JAL_INS_SIZE + ADDI_INS_SIZE;

		if (patch->return_register)
			patch->return_address =
				patch->syscall_addr + JAL_INS_SIZE;
		else if (up_to_ecall_size >= required_size)
			patch->return_address =
				patch->syscall_addr + ECALL_INS_SIZE -
				required_size + JAL_INS_SIZE;
		else
			patch->return_address =
				instrs[0].address + JAL_INS_SIZE;

		start_addr = patch->return_address - JAL_INS_SIZE;
		break;
	}

	patch->dst_jmp_patch = (uint8_t *)start_addr;
	patch->patch_size_bytes = required_size;

#ifdef __riscv_c
	check_patch_alignment(patch, start_addr, required_size);
#endif
}

#ifdef __riscv_c
static void
align_start_addr_and_size(struct patch_desc *patch,
				uint8_t **start_addr, size_t *patch_size)
{
	if (patch->start_with_c_nop) {
		*start_addr -= C_NOP_INS_SIZE;
		*patch_size += C_NOP_INS_SIZE;
	}
	if (patch->end_with_c_nop)
		*patch_size += C_NOP_INS_SIZE;
}
#endif

static void
load_orig_ra_temp(uint8_t **dst)
{
	uint8_t instrs_buff[MAX_PC_INS_SIZE * 2];
	uint8_t instrs_size = 0;

	instrs_size += rvpc_sd(instrs_buff + instrs_size, REG_RA, REG_TP,
				(int32_t)tls_offset_table.asm_ra_temp);
	instrs_size += rvpc_ld(instrs_buff + instrs_size, REG_RA, REG_TP,
				(int32_t)tls_offset_table.asm_ra_orig);

	memcpy(*dst, instrs_buff, instrs_size);
	*dst += instrs_size;
}

static void
store_new_ra_temp(uint8_t **dst)
{
	uint8_t instrs_buff[MAX_PC_INS_SIZE * 2];
	uint8_t instrs_size = 0;

	instrs_size += rvpc_sd(instrs_buff + instrs_size, REG_RA, REG_TP,
				(int32_t)tls_offset_table.asm_ra_orig);
	instrs_size += rvpc_ld(instrs_buff + instrs_size, REG_RA, REG_TP,
				(int32_t)tls_offset_table.asm_ra_temp);

	memcpy(*dst, instrs_buff, instrs_size);
	*dst += instrs_size;
}

static void
copy_jump(uint8_t **dst, uint8_t rd, uint8_t rs, int16_t offset)
{
	uint8_t instr_buff[MAX_PC_INS_SIZE];
	uint8_t instr_size;

	instr_size = rvpc_jalr(instr_buff, rd, rs, offset);

	memcpy(*dst, instr_buff, instr_size);
	*dst += instr_size;
}

static void
finalize_and_jump_back(uint8_t **dst, struct patch_desc *patch)
{
	uint8_t instrs_buff[MAX_PC_INS_SIZE * 5];
	uint8_t instrs_size = 0;
	uint8_t ret_reg = patch->return_register;

	// load original ra value if it's not used for jumping back
	if (ret_reg != REG_RA)
		instrs_size += rvpc_ld(instrs_buff + instrs_size,
					REG_RA, REG_SP, 0);

	switch (patch->syscall_num) {
	case TYPE_GW:
		// load the return address into the register used for jumping back
		instrs_size += rvpc_ld(instrs_buff + instrs_size, ret_reg, REG_SP, 16);
		break;
	case TYPE_MID:
		/*
		 * TYPE_MID expects the original ra value at different offset
		 * than TYPE_GW, so reorganize the stack by moving the value at
		 * offset 0 to offset 8.
		 */
		instrs_size += rvpc_ld(instrs_buff + instrs_size, ret_reg, REG_SP, 0);
		instrs_size += rvpc_sd(instrs_buff + instrs_size, ret_reg, REG_SP, 8);

		// load the return address into the register used for jumping back
		instrs_size += rvpc_ld(instrs_buff + instrs_size, ret_reg, REG_SP, 16);
		break;
	default: // TYPE_SML
		// if not specified, TYPE_SML uses REG_A7 to jump back to glibc
		if (!ret_reg)
			ret_reg = REG_A7;

		// load the return address into the register used for jumping back
		instrs_size += rvpc_ld(instrs_buff + instrs_size, ret_reg, REG_SP, 16);

		/*
		 * The TYPE_SML patch doesn't allocate any stack space in glibc,
		 * but sp gets reduced by 48 in GW, so deallocate the stack here
		 * before jumping back to TYPE_SML.
		 */
		instrs_size += rvpc_addisp(instrs_buff + instrs_size, 48);
		break;
	}

	// copy the jump instruction to return to glibc
	instrs_size += rvpc_jalr(instrs_buff + instrs_size, REG_ZERO, ret_reg, 0);

	memcpy(*dst, instrs_buff, instrs_size);
	*dst += instrs_size;
}

static void
relocate_instrs(struct patch_desc *patch, uint8_t **dst)
{
	patch->relocation_address = *dst;

	uint8_t *start_addr = patch->dst_jmp_patch;
	size_t patch_size = patch->patch_size_bytes;
	size_t before_ecall_size;
	size_t after_ecall_size;

#ifdef __riscv_c
	align_start_addr_and_size(patch, &start_addr, &patch_size);
#endif
	if (patch->is_ra_used_before)
		load_orig_ra_temp(dst);

	/* copy patched instructions before ecall */
	before_ecall_size = patch->syscall_addr - start_addr;
	memcpy(*dst, start_addr, before_ecall_size);
	*dst += before_ecall_size;

	if (patch->is_ra_used_before)
		store_new_ra_temp(dst);

	/*
	 * the instructions before ecall are copied,
	 * copy jump instruction to go back to asm_entry_point
	 */
	copy_jump(dst, REG_RA, REG_RA, 0);

	/* copy patched instructions after ecall */
	after_ecall_size = patch_size - before_ecall_size - ECALL_INS_SIZE;
	if (after_ecall_size > 0) {
		if (patch->is_ra_used_after)
			load_orig_ra_temp(dst);

		memcpy(*dst, patch->syscall_addr + ECALL_INS_SIZE,
			after_ecall_size);
		*dst += after_ecall_size;

		if (patch->is_ra_used_after)
			store_new_ra_temp(dst);
	}

	/*
	 * the instructions after ecall are copied,
	 * copy jump instruction to return to asm_entry_point
	 */
	copy_jump(dst, REG_RA, REG_RA, 0);

	/* prepare for jump and go back to glibc */
	finalize_and_jump_back(dst, patch);

	return;
}

/*
 * create_patch - create the custom assembly wrappers
 * around each syscall to be intercepted. Well, actually, the
 * function create_wrapper does that, so perhaps this function
 * deserves a better name.
 * What this function actually does, is figure out how to create
 * a jump instruction in libc ( which bytes to overwrite ).
 * If it successfully finds suitable bytes for hotpatching,
 * then it determines the exact bytes to overwrite, and the exact
 * address for jumping back to libc.
 *
 * This is all based on the information collected by the routine
 * find_syscalls, which does the disassembling, finding jump destinations,
 * finding padding bytes, etc..
 */
void
create_patch(struct intercept_desc *desc, uint8_t **dst)
{
	for (uint32_t patch_i = 0; patch_i < desc->count; ++patch_i) {
		struct patch_desc *patch = desc->items + patch_i;
		debug_dump("patching %s:0x%lx\n", desc->path,
				patch->syscall_addr - desc->base_addr);

		uint8_t length = check_surrounding_instructions(desc, patch);

		if (length >= TYPE_GW_SIZE) {
			patch->syscall_num = TYPE_GW;
			patch->return_register = REG_RA;

		} else if (length >= TYPE_MID_SIZE) {
			patch->syscall_num = TYPE_MID;
			patch->return_register = REG_RA;

		} else if (!is_SML_patchable(patch, length)) {
			char buffer[0x1000];

			int l = snprintf(buffer, sizeof(buffer),
				"unintercepted syscall at: %s 0x%lx\n",
				desc->path,
				patch->syscall_offset);

			intercept_log(buffer, (size_t)l);
			xabort("not enough space for patching around syscall");
		}

		position_patch(patch);

		uint8_t *last_instr_addr =
			patch->dst_jmp_patch + patch->patch_size_bytes;
#ifdef __riscv_c
		if (patch->end_with_c_nop)
			last_instr_addr += C_NOP_INS_SIZE;
#endif
		mark_jump(desc, last_instr_addr);

		relocate_instrs(patch, dst);

		/*
		 * All valuable info from the surrounding instrs is gathered,
		 * free all intercept_disasm_result structs.
		 */
		free(patch->surrounding_instrs);
		patch->surrounding_instrs = NULL;
	}

	for (uint32_t patch_i = 0; patch_i < desc->count; ++patch_i) {
		struct patch_desc *patch = desc->items + patch_i;

		if (patch->syscall_num != TYPE_GW)
			find_GW(desc, patch);
	}
}

static void
copy_trampoline(uint8_t *trampoline_address)
{
	/* This function (destination) is part of intercept_irq_entry.S */
	extern void asm_entry_point(void);
	uintptr_t destination = (uintptr_t)asm_entry_point + TRAMPOLINE_JUMP_OFFSET;

	uint8_t instrs_buff[MAX_PC_INS_SIZE + MAX_P_INS_SIZE];
	uint8_t instrs_size = 0;

	instrs_size += rvpc_sd(instrs_buff + instrs_size, REG_RA, REG_SP, 32);

	instrs_size += rvp_jump_abs(instrs_buff + instrs_size, REG_ZERO,
					REG_RA, destination);

	for (uint8_t i = 0; i < instrs_size; ++i)
		trampoline_address[i] = instrs_buff[i];
}

static void
copy_GW(struct intercept_desc *desc, const struct patch_desc *patch)
{
	/* This function (destination) is part of intercept_irq_entry.S */
	extern void asm_entry_point(void);

	uint8_t instrs_buff[MAX_PC_INS_SIZE * 6 + MAX_P_INS_SIZE];
	uint8_t instrs_size = 0;

	uint8_t *patch_start_addr = patch->dst_jmp_patch;
	uint8_t ret_reg = patch->return_register;
	uintptr_t jalr_addr = (uintptr_t)patch->return_address -
				JUMP_2GB_INS_SIZE;

	uintptr_t destination;
	if (desc->uses_trampoline)
		destination = (uintptr_t)desc->trampoline_address;
	else
		destination = (uintptr_t)asm_entry_point;

#ifdef __riscv_c
	if (patch->start_with_c_nop) {
		instrs_size += rvc_nop(instrs_buff + instrs_size);
		patch_start_addr -= RVC_INS_SIZE;
	}
#endif

	instrs_size += rvpc_addisp(instrs_buff + instrs_size, -48);
	instrs_size += rvpc_sd(instrs_buff + instrs_size, ret_reg, REG_SP, 0);

	instrs_size += rvp_jump_2GB(instrs_buff + instrs_size, ret_reg, ret_reg,
					jalr_addr, destination);

	instrs_size += rvpc_ld(instrs_buff + instrs_size, ret_reg, REG_SP, 0);
	instrs_size += rvpc_addisp(instrs_buff + instrs_size, 48);

#ifdef __riscv_c
	if (patch->end_with_c_nop)
		instrs_size += rvc_nop(instrs_buff + instrs_size);
#endif

	// cannot use memcpy() anymore...
	for (uint8_t i = 0; i < instrs_size; ++i)
		patch_start_addr[i] = instrs_buff[i];
}

static void
copy_MID(const struct patch_desc *patch)
{
	uint8_t instrs_buff[MAX_PC_INS_SIZE * 6 + MAX_P_INS_SIZE];
	uint8_t instrs_size = 0;
	uint8_t *patch_start_addr = (uint8_t *)patch->return_address -
						JAL_INS_SIZE -
						STORE_LOAD_INS_SIZE -
						MODIFY_SP_INS_SIZE;
	uint8_t ret_reg = patch->return_register;
	uintptr_t GW_entry_addr = (uintptr_t)patch->dst_jmp_patch;
	uintptr_t jal_addr = (uintptr_t)patch->return_address - JAL_INS_SIZE;

#ifdef __riscv_c
	if (patch->start_with_c_nop) {
		instrs_size += rvc_nop(instrs_buff + instrs_size);
		patch_start_addr -= RVC_INS_SIZE;
	}
#endif

	instrs_size += rvpc_addisp(instrs_buff + instrs_size, -48);
	instrs_size += rvpc_sd(instrs_buff + instrs_size, ret_reg, REG_SP, 8);

	instrs_size += rvp_jal(instrs_buff + instrs_size, ret_reg,
				jal_addr, GW_entry_addr);

	instrs_size += rvpc_ld(instrs_buff + instrs_size, ret_reg, REG_SP, 8);
	instrs_size += rvpc_addisp(instrs_buff + instrs_size, 48);

#ifdef __riscv_c
	if (patch->end_with_c_nop)
		instrs_size += rvc_nop(instrs_buff + instrs_size);
#endif

	// cannot use memcpy() anymore...
	for (uint8_t i = 0; i < instrs_size; ++i)
		patch_start_addr[i] = instrs_buff[i];
}

static void
copy_SML(const struct patch_desc *patch)
{
	uint8_t instrs_buff[MAX_PC_INS_SIZE * 3 + MAX_P_INS_SIZE];
	uint8_t instrs_size = 0;
	uint8_t *patch_start_addr = (uint8_t *)patch->return_address -
							JAL_INS_SIZE;
	uintptr_t GW_entry_addr = (uintptr_t)patch->dst_jmp_patch;
	uintptr_t jal_addr = (uintptr_t)patch->return_address - JAL_INS_SIZE;

#ifdef __riscv_c
	if (patch->start_with_c_nop) {
		instrs_size += rvc_nop(instrs_buff + instrs_size);
		patch_start_addr -= RVC_INS_SIZE;
	}
#endif

	instrs_size += rvp_jal(instrs_buff + instrs_size, REG_A7,
				jal_addr, GW_entry_addr);

	if (!patch->return_register)
		instrs_size += rvpc_li(instrs_buff + instrs_size, REG_A7,
					patch->syscall_num);

#ifdef __riscv_c
	if (patch->end_with_c_nop)
		instrs_size += rvc_nop(instrs_buff + instrs_size);
#endif

	// cannot use memcpy() anymore...
	for (uint8_t i = 0; i < instrs_size; ++i)
		patch_start_addr[i] = instrs_buff[i];
}

/*
 * activate_patches()
 * Loop over all the patches, and and overwrite each syscall.
 */
void
activate_patches(struct intercept_desc *desc)
{
	unsigned char *first_page;
	size_t size;

	if (desc->count == 0)
		return;

	if (desc->uses_trampoline)
		copy_trampoline(desc->trampoline_address);

	first_page = round_down_address(desc->text_start);
	size = (size_t)(desc->text_end - first_page);

	mprotect_no_intercept(first_page, size,
	    PROT_READ | PROT_WRITE | PROT_EXEC,
	    "mprotect PROT_READ | PROT_WRITE | PROT_EXEC");

	for (unsigned i = 0; i < desc->count; ++i) {
		const struct patch_desc *patch = desc->items + i;

		if (patch->dst_jmp_patch < desc->text_start ||
		    patch->dst_jmp_patch > desc->text_end)
			xabort("dst_jmp_patch outside text");

		switch (patch->syscall_num) {
		case TYPE_GW:
			copy_GW(desc, patch);
			break;
		case TYPE_MID:
			copy_MID(patch);
			break;
		default:
			copy_SML(patch);
			break;
		}
	}

	__builtin___clear_cache(first_page, first_page + size);

	mprotect_no_intercept(first_page, size,
	    PROT_READ | PROT_EXEC,
	    "mprotect PROT_READ | PROT_EXEC");
}
