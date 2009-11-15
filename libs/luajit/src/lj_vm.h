/*
** Assembler VM interface definitions.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_VM_H
#define _LJ_VM_H

#include "lj_obj.h"

/* Entry points for ASM parts of VM. */
LJ_ASMF void lj_vm_call(lua_State *L, TValue *base, int nres1);
LJ_ASMF int lj_vm_pcall(lua_State *L, TValue *base, int nres1, ptrdiff_t ef);
typedef TValue *(*lua_CPFunction)(lua_State *L, lua_CFunction func, void *ud);
LJ_ASMF int lj_vm_cpcall(lua_State *L, lua_CPFunction cp, lua_CFunction func,
			 void *ud);
LJ_ASMF int lj_vm_resume(lua_State *L, TValue *base, int nres1, ptrdiff_t ef);
LJ_ASMF_NORET void lj_vm_unwind_c(void *cframe, int errcode);
LJ_ASMF_NORET void lj_vm_unwind_ff(void *cframe);

/* Miscellaneous functions. */
#if LJ_TARGET_X86ORX64
LJ_ASMF int lj_vm_cpuid(uint32_t f, uint32_t res[4]);
#endif
LJ_ASMF double lj_vm_foldarith(double x, double y, int op);
LJ_ASMF double lj_vm_foldfpm(double x, int op);

/* Dispatch targets for recording and hooks. */
LJ_ASMF void lj_vm_record(void);
LJ_ASMF void lj_vm_hook(void);

/* Trace exit handling. */
LJ_ASMF void lj_vm_exit_handler(void);
LJ_ASMF void lj_vm_exit_interp(void);

/* Handlers callable from compiled code. */
LJ_ASMF void lj_vm_floor(void);
LJ_ASMF void lj_vm_ceil(void);
LJ_ASMF void lj_vm_trunc(void);
LJ_ASMF void lj_vm_exp(void);
LJ_ASMF void lj_vm_exp2(void);
LJ_ASMF void lj_vm_pow(void);
LJ_ASMF void lj_vm_powi(void);

/* Call gates for functions. */
LJ_ASMF void lj_gate_lf(void);
LJ_ASMF void lj_gate_lv(void);
LJ_ASMF void lj_gate_c(void);
LJ_ASMF void lj_gate_cwrap(void);

/* Continuations for metamethods. */
LJ_ASMF void lj_cont_cat(void);  /* Continue with concatenation. */
LJ_ASMF void lj_cont_ra(void);  /* Store result in RA from instruction. */
LJ_ASMF void lj_cont_nop(void);  /* Do nothing, just continue execution. */
LJ_ASMF void lj_cont_condt(void);  /* Branch if result is true. */
LJ_ASMF void lj_cont_condf(void);  /* Branch if result is false. */

/* Start of the ASM code. */
LJ_ASMF char lj_vm_asm_begin[];

/* Opcode handler offsets, relative to lj_vm_asm_begin. */
LJ_ASMF const uint16_t lj_vm_op_ofs[];

#define makeasmfunc(ofs)	((ASMFunction)(lj_vm_asm_begin + (ofs)))

#endif
