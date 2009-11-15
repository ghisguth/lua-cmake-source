/*
** IR assembler (SSA IR -> machine code).
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_asm_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_gc.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_mcode.h"
#include "lj_iropt.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_asm.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_target.h"

/* -- Assembler state and common macros ----------------------------------- */

/* Assembler state. */
typedef struct ASMState {
  RegCost cost[RID_MAX];  /* Reference and blended allocation cost for regs. */

  MCode *mcp;		/* Current MCode pointer (grows down). */
  MCode *mclim;		/* Lower limit for MCode memory + red zone. */

  IRIns *ir;		/* Copy of pointer to IR instructions/constants. */
  jit_State *J;		/* JIT compiler state. */

  x86ModRM mrm;		/* Fused x86 address operand. */

  RegSet freeset;	/* Set of free registers. */
  RegSet modset;	/* Set of registers modified inside the loop. */
  RegSet phiset;	/* Set of PHI registers. */

  uint32_t flags;	/* Copy of JIT compiler flags. */
  int loopinv;		/* Loop branch inversion (0:no, 1:yes, 2:yes+CC_P). */

  int32_t evenspill;	/* Next even spill slot. */
  int32_t oddspill;	/* Next odd spill slot (or 0). */

  IRRef curins;		/* Reference of current instruction. */
  IRRef stopins;	/* Stop assembly before hitting this instruction. */
  IRRef orignins;	/* Original T->nins. */

  IRRef snapref;	/* Current snapshot is active after this reference. */
  IRRef snaprename;	/* Rename highwater mark for snapshot check. */
  SnapNo snapno;	/* Current snapshot number. */
  SnapNo loopsnapno;	/* Loop snapshot number. */

  Trace *T;		/* Trace to assemble. */
  Trace *parent;	/* Parent trace (or NULL). */

  IRRef fuseref;	/* Fusion limit (loopref, 0 or FUSE_DISABLED). */
  IRRef sectref;	/* Section base reference (loopref or 0). */
  IRRef loopref;	/* Reference of LOOP instruction (or 0). */

  BCReg topslot;	/* Number of slots for stack check (unless 0). */
  MSize gcsteps;	/* Accumulated number of GC steps (per section). */

  MCode *mcbot;		/* Bottom of reserved MCode. */
  MCode *mctop;		/* Top of generated MCode. */
  MCode *mcloop;	/* Pointer to loop MCode (or NULL). */
  MCode *invmcp;	/* Points to invertible loop branch (or NULL). */
  MCode *testmcp;	/* Pending opportunity to remove test r,r. */
  MCode *realign;	/* Realign loop if not NULL. */

  IRRef1 phireg[RID_MAX];  /* PHI register references. */
  uint16_t parentmap[LJ_MAX_JSLOTS];  /* Parent slot to RegSP map. */
} ASMState;

#define IR(ref)			(&as->ir[(ref)])

/* Check for variant to invariant references. */
#define iscrossref(as, ref)	((ref) < as->sectref)

/* Inhibit memory op fusion from variant to invariant references. */
#define FUSE_DISABLED		(~(IRRef)0)
#define mayfuse(as, ref)	((ref) > as->fuseref)
#define neverfuse(as)		(as->fuseref == FUSE_DISABLED)
#define opisfusableload(o) \
  ((o) == IR_ALOAD || (o) == IR_HLOAD || (o) == IR_ULOAD || \
   (o) == IR_FLOAD || (o) == IR_SLOAD || (o) == IR_XLOAD)

/* Instruction selection for XMM moves. */
#define XMM_MOVRR(as)	((as->flags & JIT_F_SPLIT_XMM) ? XO_MOVSD : XO_MOVAPS)
#define XMM_MOVRM(as)	((as->flags & JIT_F_SPLIT_XMM) ? XO_MOVLPD : XO_MOVSD)

/* Sparse limit checks using a red zone before the actual limit. */
#define MCLIM_REDZONE	64
#define checkmclim(as) \
  if (LJ_UNLIKELY(as->mcp < as->mclim)) asm_mclimit(as)

static LJ_NORET LJ_NOINLINE void asm_mclimit(ASMState *as)
{
  lj_mcode_limiterr(as->J, (size_t)(as->mctop - as->mcp + 4*MCLIM_REDZONE));
}

/* -- Emit x86 instructions ----------------------------------------------- */

#define MODRM(mode, r1, r2)	((MCode)((mode)+(((r1)&7)<<3)+((r2)&7)))

#if LJ_64
#define REXRB(p, rr, rb) \
    { MCode rex = 0x40 + (((rr)>>1)&4) + (((rb)>>3)&1); \
      if (rex != 0x40) *--(p) = rex; }
#define FORCE_REX		0x200
#else
#define REXRB(p, rr, rb)	((void)0)
#define FORCE_REX		0
#endif

#define emit_i8(as, i)		(*--as->mcp = (MCode)(i))
#define emit_i32(as, i)		(*(int32_t *)(as->mcp-4) = (i), as->mcp -= 4)

#define emit_x87op(as, xo) \
  (*(uint16_t *)(as->mcp-2) = (uint16_t)(xo), as->mcp -= 2)

/* op */
static LJ_AINLINE MCode *emit_op(x86Op xo, Reg rr, Reg rb, Reg rx,
				 MCode *p, int delta)
{
  int n = (int8_t)xo;
#if defined(__GNUC__)
  if (__builtin_constant_p(xo) && n == -2)
    p[delta-2] = (MCode)(xo >> 24);
  else if (__builtin_constant_p(xo) && n == -3)
    *(uint16_t *)(p+delta-3) = (uint16_t)(xo >> 16);
  else
#endif
    *(uint32_t *)(p+delta-5) = (uint32_t)xo;
  p += n + delta;
#if LJ_64
  {
    uint32_t rex = 0x40 + ((rr>>1)&(4+(FORCE_REX>>1)))+((rx>>2)&2)+((rb>>3)&1);
    if (rex != 0x40) {
      if (n == -4) { *p = (MCode)rex; rex = (MCode)(xo >> 8); }
      *--p = (MCode)rex;
    }
  }
#else
  UNUSED(rr); UNUSED(rb); UNUSED(rx);
#endif
  return p;
}

/* op + modrm */
#define emit_opm(xo, mode, rr, rb, p, delta) \
  (p[(delta)-1] = MODRM((mode), (rr), (rb)), \
   emit_op((xo), (rr), (rb), 0, (p), (delta)))

/* op + modrm + sib */
#define emit_opmx(xo, mode, scale, rr, rb, rx, p) \
  (p[-1] = MODRM((scale), (rx), (rb)), \
   p[-2] = MODRM((mode), (rr), RID_ESP), \
   emit_op((xo), (rr), (rb), (rx), (p), -1))

/* op r1, r2 */
static void emit_rr(ASMState *as, x86Op xo, Reg r1, Reg r2)
{
  MCode *p = as->mcp;
  as->mcp = emit_opm(xo, XM_REG, r1, r2, p, 0);
}

#if LJ_64 && defined(LUA_USE_ASSERT)
/* [addr] is sign-extended in x64 and must be in lower 2G (not 4G). */
static int32_t ptr2addr(void *p)
{
  lua_assert((uintptr_t)p < (uintptr_t)0x80000000);
  return i32ptr(p);
}
#else
#define ptr2addr(p)	(i32ptr((p)))
#endif

/* op r, [addr] */
static void emit_rma(ASMState *as, x86Op xo, Reg rr, const void *addr)
{
  MCode *p = as->mcp;
  *(int32_t *)(p-4) = ptr2addr(addr);
#if LJ_64
  p[-5] = MODRM(XM_SCALE1, RID_ESP, RID_EBP);
  as->mcp = emit_opm(xo, XM_OFS0, rr, RID_ESP, p, -5);
#else
  as->mcp = emit_opm(xo, XM_OFS0, rr, RID_EBP, p, -4);
#endif
}

/* op r, [base+ofs] */
static void emit_rmro(ASMState *as, x86Op xo, Reg rr, Reg rb, int32_t ofs)
{
  MCode *p = as->mcp;
  x86Mode mode;
  if (ra_hasreg(rb)) {
    if (ofs == 0 && (rb&7) != RID_EBP) {
      mode = XM_OFS0;
    } else if (checki8(ofs)) {
      *--p = (MCode)ofs;
      mode = XM_OFS8;
    } else {
      p -= 4;
      *(int32_t *)p = ofs;
      mode = XM_OFS32;
    }
    if ((rb&7) == RID_ESP)
      *--p = MODRM(XM_SCALE1, RID_ESP, RID_ESP);
  } else {
    *(int32_t *)(p-4) = ofs;
#if LJ_64
    p[-5] = MODRM(XM_SCALE1, RID_ESP, RID_EBP);
    p -= 5;
    rb = RID_ESP;
#else
    p -= 4;
    rb = RID_EBP;
#endif
    mode = XM_OFS0;
  }
  as->mcp = emit_opm(xo, mode, rr, rb, p, 0);
}

/* op r, [base+idx*scale+ofs] */
static void emit_rmrxo(ASMState *as, x86Op xo, Reg rr, Reg rb, Reg rx,
		       x86Mode scale, int32_t ofs)
{
  MCode *p = as->mcp;
  x86Mode mode;
  if (ofs == 0 && (rb&7) != RID_EBP) {
    mode = XM_OFS0;
  } else if (checki8(ofs)) {
    mode = XM_OFS8;
    *--p = (MCode)ofs;
  } else {
    mode = XM_OFS32;
    p -= 4;
    *(int32_t *)p = ofs;
  }
  as->mcp = emit_opmx(xo, mode, scale, rr, rb, rx, p);
}

/* op r, i */
static void emit_gri(ASMState *as, x86Group xg, Reg rb, int32_t i)
{
  MCode *p = as->mcp;
  if (checki8(i)) {
    p -= 3;
    p[2] = (MCode)i;
    p[0] = (MCode)(xg >> 16);
  } else {
    p -= 6;
    *(int32_t *)(p+2) = i;
    p[0] = (MCode)(xg >> 8);
  }
  p[1] = MODRM(XM_REG, xg, rb);
  REXRB(p, 0, rb);
  as->mcp = p;
}

/* op [base+ofs], i */
static void emit_gmroi(ASMState *as, x86Group xg, Reg rb, int32_t ofs,
		       int32_t i)
{
  x86Op xo;
  if (checki8(i)) {
    emit_i8(as, i);
    xo = (x86Op)(((xg >> 16) << 24)+0xfe);
  } else {
    emit_i32(as, i);
    xo = (x86Op)(((xg >> 8) << 24)+0xfe);
  }
  emit_rmro(as, xo, (Reg)xg, rb, ofs);
}

#define emit_shifti(as, xg, r, i) \
  (emit_i8(as, (i)), emit_rr(as, XO_SHIFTi, (Reg)(xg), (r)))

/* op r, rm/mrm */
static void emit_mrm(ASMState *as, x86Op xo, Reg rr, Reg rb)
{
  MCode *p = as->mcp;
  x86Mode mode = XM_REG;
  if (rb == RID_MRM) {
    rb = as->mrm.base;
    if (rb == RID_NONE) {
      rb = RID_EBP;
      mode = XM_OFS0;
      p -= 4;
      *(int32_t *)p = as->mrm.ofs;
      if (as->mrm.idx != RID_NONE)
	goto mrmidx;
#if LJ_64
      *--p = MODRM(XM_SCALE1, RID_ESP, RID_EBP);
      rb = RID_ESP;
#endif
    } else {
      if (as->mrm.ofs == 0 && (rb&7) != RID_EBP) {
	mode = XM_OFS0;
      } else if (checki8(as->mrm.ofs)) {
	*--p = (MCode)as->mrm.ofs;
	mode = XM_OFS8;
      } else {
	p -= 4;
	*(int32_t *)p = as->mrm.ofs;
	mode = XM_OFS32;
      }
      if (as->mrm.idx != RID_NONE) {
      mrmidx:
	as->mcp = emit_opmx(xo, mode, as->mrm.scale, rr, rb, as->mrm.idx, p);
	return;
      }
      if ((rb&7) == RID_ESP)
	*--p = MODRM(XM_SCALE1, RID_ESP, RID_ESP);
    }
  }
  as->mcp = emit_opm(xo, mode, rr, rb, p, 0);
}

static void emit_addptr(ASMState *as, Reg r, int32_t ofs)
{
  if (ofs) {
    if ((as->flags & JIT_F_LEA_AGU))
      emit_rmro(as, XO_LEA, r, r, ofs);
    else
      emit_gri(as, XG_ARITHi(XOg_ADD), r, ofs);
  }
}

/* -- Emit moves ---------------------------------------------------------- */

/* Generic move between two regs. */
static void emit_movrr(ASMState *as, Reg r1, Reg r2)
{
  emit_rr(as, r1 < RID_MAX_GPR ? XO_MOV : XMM_MOVRR(as), r1, r2);
}

/* Generic move from [base+ofs]. */
static void emit_movrmro(ASMState *as, Reg rr, Reg rb, int32_t ofs)
{
  emit_rmro(as, rr < RID_MAX_GPR ? XO_MOV : XMM_MOVRM(as), rr, rb, ofs);
}

/* mov [base+ofs], i */
static void emit_movmroi(ASMState *as, Reg base, int32_t ofs, int32_t i)
{
  emit_i32(as, i);
  emit_rmro(as, XO_MOVmi, 0, base, ofs);
}

/* mov [base+ofs], r */
#define emit_movtomro(as, r, base, ofs) \
  emit_rmro(as, XO_MOVto, (r), (base), (ofs))

/* Get/set global_State fields. */
#define emit_opgl(as, xo, r, field) \
  emit_rma(as, (xo), (r), (void *)&J2G(as->J)->field)
#define emit_getgl(as, r, field)	emit_opgl(as, XO_MOV, (r), field)
#define emit_setgl(as, r, field)	emit_opgl(as, XO_MOVto, (r), field)
#define emit_setgli(as, field, i) \
  (emit_i32(as, i), emit_opgl(as, XO_MOVmi, 0, field))

/* mov r, i / xor r, r */
static void emit_loadi(ASMState *as, Reg r, int32_t i)
{
  if (i == 0) {
    emit_rr(as, XO_ARITH(XOg_XOR), r, r);
  } else {
    MCode *p = as->mcp;
    *(int32_t *)(p-4) = i;
    p[-5] = (MCode)(XI_MOVri+(r&7));
    p -= 5;
    REXRB(p, 0, r);
    as->mcp = p;
  }
}

/* mov r, addr */
#define emit_loada(as, r, addr) \
  emit_loadi(as, (r), ptr2addr((addr)))

/* movsd r, [&tv->n] / xorps r, r */
static void emit_loadn(ASMState *as, Reg r, cTValue *tv)
{
  if (tvispzero(tv))  /* Use xor only for +0. */
    emit_rr(as, XO_XORPS, r, r);
  else
    emit_rma(as, XMM_MOVRM(as), r, &tv->n);
}

/* -- Emit branches ------------------------------------------------------- */

/* Label for short jumps. */
typedef MCode *MCLabel;

/* jcc short target */
static void emit_sjcc(ASMState *as, int cc, MCLabel target)
{
  MCode *p = as->mcp;
  p[-1] = (MCode)(int8_t)(target-p);
  p[-2] = (MCode)(XI_JCCs+(cc&15));
  as->mcp = p - 2;
}

/* jcc short (pending target) */
static MCLabel emit_sjcc_label(ASMState *as, int cc)
{
  MCode *p = as->mcp;
  p[-1] = 0;
  p[-2] = (MCode)(XI_JCCs+(cc&15));
  as->mcp = p - 2;
  return p;
}

/* Fixup jcc short target. */
static void emit_sfixup(ASMState *as, MCLabel source)
{
  source[-1] = (MCode)(as->mcp-source);
}

/* Return label pointing to current PC. */
#define emit_label(as)		((as)->mcp)

/* jcc target */
static void emit_jcc(ASMState *as, int cc, MCode *target)
{
  MCode *p = as->mcp;
  int32_t addr = (int32_t)(target - p);
  *(int32_t *)(p-4) = addr;
  p[-5] = (MCode)(XI_JCCn+(cc&15));
  p[-6] = 0x0f;
  as->mcp = p - 6;
}

/* call target */
static void emit_call_(ASMState *as, MCode *target)
{
  MCode *p = as->mcp;
  *(int32_t *)(p-4) = (int32_t)(target - p);
  p[-5] = XI_CALL;
  as->mcp = p - 5;
}

#define emit_call(as, f)	emit_call_(as, (MCode *)(void *)(f))

/* Argument setup for C calls. Up to 3 args need no stack adjustment. */
#define emit_setargr(as, narg, r) \
  emit_movtomro(as, (r), RID_ESP, ((narg)-1)*4);
#define emit_setargi(as, narg, imm) \
  emit_movmroi(as, RID_ESP, ((narg)-1)*4, (imm))
#define emit_setargp(as, narg, ptr) \
  emit_setargi(as, (narg), ptr2addr((ptr)))

/* -- Register allocator debugging ---------------------------------------- */

/* #define LUAJIT_DEBUG_RA */

#ifdef LUAJIT_DEBUG_RA

#include <stdio.h>
#include <stdarg.h>

#define RIDNAME(name)	#name,
static const char *const ra_regname[] = {
  GPRDEF(RIDNAME)
  FPRDEF(RIDNAME)
  "mrm",
  NULL
};
#undef RIDNAME

static char ra_dbg_buf[65536];
static char *ra_dbg_p;
static char *ra_dbg_merge;
static MCode *ra_dbg_mcp;

static void ra_dstart(void)
{
  ra_dbg_p = ra_dbg_buf;
  ra_dbg_merge = NULL;
  ra_dbg_mcp = NULL;
}

static void ra_dflush(void)
{
  fwrite(ra_dbg_buf, 1, (size_t)(ra_dbg_p-ra_dbg_buf), stdout);
  ra_dstart();
}

static void ra_dprintf(ASMState *as, const char *fmt, ...)
{
  char *p;
  va_list argp;
  va_start(argp, fmt);
  p = ra_dbg_mcp == as->mcp ? ra_dbg_merge : ra_dbg_p;
  ra_dbg_mcp = NULL;
  p += sprintf(p, "%08x  \e[36m%04d ", (uintptr_t)as->mcp, as->curins-REF_BIAS);
  for (;;) {
    const char *e = strchr(fmt, '$');
    if (e == NULL) break;
    memcpy(p, fmt, (size_t)(e-fmt));
    p += e-fmt;
    if (e[1] == 'r') {
      Reg r = va_arg(argp, Reg) & RID_MASK;
      if (r <= RID_MAX) {
	const char *q;
	for (q = ra_regname[r]; *q; q++)
	  *p++ = *q >= 'A' && *q <= 'Z' ? *q + 0x20 : *q;
      } else {
	*p++ = '?';
	lua_assert(0);
      }
    } else if (e[1] == 'f' || e[1] == 'i') {
      IRRef ref;
      if (e[1] == 'f')
	ref = va_arg(argp, IRRef);
      else
	ref = va_arg(argp, IRIns *) - as->ir;
      if (ref >= REF_BIAS)
	p += sprintf(p, "%04d", ref - REF_BIAS);
      else
	p += sprintf(p, "K%03d", REF_BIAS - ref);
    } else if (e[1] == 's') {
      uint32_t slot = va_arg(argp, uint32_t);
      p += sprintf(p, "[esp+0x%x]", sps_scale(slot));
    } else {
      lua_assert(0);
    }
    fmt = e+2;
  }
  va_end(argp);
  while (*fmt)
    *p++ = *fmt++;
  *p++ = '\e'; *p++ = '['; *p++ = 'm'; *p++ = '\n';
  if (p > ra_dbg_buf+sizeof(ra_dbg_buf)-256) {
    fwrite(ra_dbg_buf, 1, (size_t)(p-ra_dbg_buf), stdout);
    p = ra_dbg_buf;
  }
  ra_dbg_p = p;
}

#define RA_DBG_START()	ra_dstart()
#define RA_DBG_FLUSH()	ra_dflush()
#define RA_DBG_REF() \
  do { char *_p = ra_dbg_p; ra_dprintf(as, ""); \
       ra_dbg_merge = _p; ra_dbg_mcp = as->mcp; } while (0)
#define RA_DBGX(x)	ra_dprintf x

#else
#define RA_DBG_START()	((void)0)
#define RA_DBG_FLUSH()	((void)0)
#define RA_DBG_REF()	((void)0)
#define RA_DBGX(x)	((void)0)
#endif

/* -- Register allocator -------------------------------------------------- */

#define ra_free(as, r)		rset_set(as->freeset, (r))
#define ra_modified(as, r)	rset_set(as->modset, (r))

#define ra_used(ir)		(ra_hasreg((ir)->r) || ra_hasspill((ir)->s))

/* Setup register allocator. */
static void ra_setup(ASMState *as)
{
  /* Initially all regs (except the stack pointer) are free for use. */
  as->freeset = RSET_ALL;
  as->modset = RSET_EMPTY;
  as->phiset = RSET_EMPTY;
  memset(as->phireg, 0, sizeof(as->phireg));
  memset(as->cost, 0, sizeof(as->cost));
  as->cost[RID_ESP] = REGCOST(~0u, 0u);

  /* Start slots for spill slot allocation. */
  as->evenspill = (SPS_FIRST+1)&~1;
  as->oddspill = (SPS_FIRST&1) ? SPS_FIRST : 0;
}

/* Rematerialize constants. */
static Reg ra_rematk(ASMState *as, IRIns *ir)
{
  Reg r = ir->r;
  lua_assert(ra_hasreg(r) && !ra_hasspill(ir->s));
  ra_free(as, r);
  ra_modified(as, r);
  ir->r = RID_INIT;  /* Do not keep any hint. */
  RA_DBGX((as, "remat     $i $r", ir, r));
  if (ir->o == IR_KNUM) {
    emit_loadn(as, r, ir_knum(ir));
  } else if (ir->o == IR_BASE) {
    ra_sethint(ir->r, RID_BASE);  /* Restore BASE register hint. */
    emit_getgl(as, r, jit_base);
  } else {
    lua_assert(ir->o == IR_KINT || ir->o == IR_KGC ||
	       ir->o == IR_KPTR || ir->o == IR_KNULL);
    emit_loadi(as, r, ir->i);
  }
  return r;
}

/* Force a spill. Allocate a new spill slot if needed. */
static int32_t ra_spill(ASMState *as, IRIns *ir)
{
  int32_t slot = ir->s;
  if (!ra_hasspill(slot)) {
    if (irt_isnum(ir->t)) {
      slot = as->evenspill;
      as->evenspill += 2;
    } else if (as->oddspill) {
      slot = as->oddspill;
      as->oddspill = 0;
    } else {
      slot = as->evenspill;
      as->oddspill = slot+1;
      as->evenspill += 2;
    }
    if (as->evenspill > 256)
      lj_trace_err(as->J, LJ_TRERR_SPILLOV);
    ir->s = (uint8_t)slot;
  }
  return sps_scale(slot);
}

/* Restore a register (marked as free). Rematerialize or force a spill. */
static Reg ra_restore(ASMState *as, IRRef ref)
{
  IRIns *ir = IR(ref);
  if (irref_isk(ref) || ref == REF_BASE) {
    return ra_rematk(as, ir);
  } else {
    Reg r = ir->r;
    lua_assert(ra_hasreg(r));
    ra_free(as, r);
    ra_modified(as, r);
    ra_sethint(ir->r, r);  /* Keep hint. */
    RA_DBGX((as, "restore   $i $r", ir, r));
    emit_movrmro(as, r, RID_ESP, ra_spill(as, ir));  /* Force a spill. */
    return r;
  }
}

/* Save a register to a spill slot. */
static LJ_AINLINE void ra_save(ASMState *as, IRIns *ir, Reg r)
{
  RA_DBGX((as, "save      $i $r", ir, r));
  emit_rmro(as, r < RID_MAX_GPR ? XO_MOVto : XO_MOVSDto,
	    r, RID_ESP, sps_scale(ir->s));
}

#define MINCOST(r) \
  if (LJ_LIKELY(allow&RID2RSET(r)) && as->cost[r] < cost) \
    cost = as->cost[r]

/* Evict the register with the lowest cost, forcing a restore. */
static Reg ra_evict(ASMState *as, RegSet allow)
{
  RegCost cost = ~(RegCost)0;
  if (allow < RID2RSET(RID_MAX_GPR)) {
    MINCOST(RID_EAX);MINCOST(RID_ECX);MINCOST(RID_EDX);MINCOST(RID_EBX);
    MINCOST(RID_EBP);MINCOST(RID_ESI);MINCOST(RID_EDI);
#if LJ_64
    MINCOST(RID_R8D);MINCOST(RID_R9D);MINCOST(RID_R10D);MINCOST(RID_R11D);
    MINCOST(RID_R12D);MINCOST(RID_R13D);MINCOST(RID_R14D);MINCOST(RID_R15D);
#endif
  } else {
    MINCOST(RID_XMM0);MINCOST(RID_XMM1);MINCOST(RID_XMM2);MINCOST(RID_XMM3);
    MINCOST(RID_XMM4);MINCOST(RID_XMM5);MINCOST(RID_XMM6);MINCOST(RID_XMM7);
#if LJ_64
    MINCOST(RID_XMM8);MINCOST(RID_XMM9);MINCOST(RID_XMM10);MINCOST(RID_XMM11);
    MINCOST(RID_XMM12);MINCOST(RID_XMM13);MINCOST(RID_XMM14);MINCOST(RID_XMM15);
#endif
  }
  lua_assert(allow != RSET_EMPTY);
  lua_assert(regcost_ref(cost) >= as->T->nk && regcost_ref(cost) < as->T->nins);
  return ra_restore(as, regcost_ref(cost));
}

/* Pick any register (marked as free). Evict on-demand. */
static LJ_AINLINE Reg ra_pick(ASMState *as, RegSet allow)
{
  RegSet pick = as->freeset & allow;
  if (!pick)
    return ra_evict(as, allow);
  else
    return rset_picktop(pick);
}

/* Get a scratch register (marked as free). */
static LJ_AINLINE Reg ra_scratch(ASMState *as, RegSet allow)
{
  Reg r = ra_pick(as, allow);
  ra_modified(as, r);
  RA_DBGX((as, "scratch        $r", r));
  return r;
}

/* Evict all registers from a set (if not free). */
static void ra_evictset(ASMState *as, RegSet drop)
{
  as->modset |= drop;
  drop &= ~as->freeset;
  while (drop) {
    Reg r = rset_picktop(drop);
    ra_restore(as, regcost_ref(as->cost[r]));
    rset_clear(drop, r);
    checkmclim(as);
  }
}

/* Allocate a register for ref from the allowed set of registers.
** Note: this function assumes the ref does NOT have a register yet!
** Picks an optimal register, sets the cost and marks the register as non-free.
*/
static Reg ra_allocref(ASMState *as, IRRef ref, RegSet allow)
{
  IRIns *ir = IR(ref);
  RegSet pick = as->freeset & allow;
  Reg r;
  lua_assert(ra_noreg(ir->r));
  if (pick) {
    /* First check register hint from propagation or PHI. */
    if (ra_hashint(ir->r)) {
      r = ra_gethint(ir->r);
      if (rset_test(pick, r))  /* Use hint register if possible. */
	goto found;
      /* Rematerialization is cheaper than missing a hint. */
      if (rset_test(allow, r) && irref_isk(regcost_ref(as->cost[r]))) {
	ra_rematk(as, IR(regcost_ref(as->cost[r])));
	goto found;
      }
      RA_DBGX((as, "hintmiss  $f $r", ref, r));
    }
    /* Invariants should preferably get unmodified registers. */
    if (ref < as->loopref && !irt_isphi(ir->t)) {
      if ((pick & ~as->modset))
	pick &= ~as->modset;
      r = rset_pickbot(pick);  /* Reduce conflicts with inverse allocation. */
    } else {
      r = rset_picktop(pick);
    }
  } else {
    r = ra_evict(as, allow);
  }
found:
  RA_DBGX((as, "alloc     $f $r", ref, r));
  ir->r = (uint8_t)r;
  rset_clear(as->freeset, r);
  as->cost[r] = REGCOST_REF_T(ref, irt_t(ir->t));
  return r;
}

/* Allocate a register on-demand. */
static LJ_INLINE Reg ra_alloc1(ASMState *as, IRRef ref, RegSet allow)
{
  Reg r = IR(ref)->r;
  /* Note: allow is ignored if the register is already allocated. */
  if (ra_noreg(r)) r = ra_allocref(as, ref, allow);
  return r;
}

/* Rename register allocation and emit move. */
static void ra_rename(ASMState *as, Reg down, Reg up)
{
  IRRef ren, ref = regcost_ref(as->cost[up] = as->cost[down]);
  IR(ref)->r = (uint8_t)up;
  as->cost[down] = 0;
  lua_assert((down < RID_MAX_GPR) == (up < RID_MAX_GPR));
  lua_assert(!rset_test(as->freeset, down) && rset_test(as->freeset, up));
  rset_set(as->freeset, down);  /* 'down' is free ... */
  rset_clear(as->freeset, up);  /* ... and 'up' is now allocated. */
  RA_DBGX((as, "rename    $f $r $r", regcost_ref(as->cost[up]), down, up));
  emit_movrr(as, down, up);  /* Backwards code generation needs inverse move. */
  if (!ra_hasspill(IR(ref)->s)) {  /* Add the rename to the IR. */
    lj_ir_set(as->J, IRT(IR_RENAME, IRT_NIL), ref, as->snapno);
    ren = tref_ref(lj_ir_emit(as->J));
    as->ir = as->T->ir;  /* The IR may have been reallocated. */
    IR(ren)->r = (uint8_t)down;
    IR(ren)->s = SPS_NONE;
  }
}

/* Pick a destination register (marked as free).
** Caveat: allow is ignored if there's already a destination register.
** Use ra_destreg() to get a specific register.
*/
static Reg ra_dest(ASMState *as, IRIns *ir, RegSet allow)
{
  Reg dest = ir->r;
  if (ra_hasreg(dest)) {
    ra_free(as, dest);
    ra_modified(as, dest);
  } else {
    dest = ra_scratch(as, allow);
  }
  if (LJ_UNLIKELY(ra_hasspill(ir->s))) ra_save(as, ir, dest);
  return dest;
}

/* Force a specific destination register (marked as free). */
static void ra_destreg(ASMState *as, IRIns *ir, Reg r)
{
  Reg dest = ra_dest(as, ir, RID2RSET(r));
  if (dest != r) {
    ra_scratch(as, RID2RSET(r));
    emit_movrr(as, dest, r);
  }
}

/* Propagate dest register to left reference. Emit moves as needed.
** This is a required fixup step for all 2-operand machine instructions.
*/
static void ra_left(ASMState *as, Reg dest, IRRef lref)
{
  IRIns *ir = IR(lref);
  Reg left = ir->r;
  if (ra_noreg(left)) {
    if (irref_isk(lref)) {
      if (ir->o == IR_KNUM) {
	cTValue *tv = ir_knum(ir);
	/* FP remat needs a load except for +0. Still better than eviction. */
	if (tvispzero(tv) || !(as->freeset & RSET_FPR)) {
	  emit_loadn(as, dest, tv);
	  return;
	}
      } else {
	lua_assert(ir->o == IR_KINT || ir->o == IR_KGC ||
		   ir->o == IR_KPTR || ir->o == IR_KNULL);
	emit_loadi(as, dest, ir->i);
	return;
      }
    }
    if (!ra_hashint(left) && !iscrossref(as, lref))
      ra_sethint(ir->r, dest);  /* Propagate register hint. */
    left = ra_allocref(as, lref, dest < RID_MAX_GPR ? RSET_GPR : RSET_FPR);
  }
  /* Move needed for true 3-operand instruction: y=a+b ==> y=a; y+=b. */
  if (dest != left) {
    /* Use register renaming if dest is the PHI reg. */
    if (irt_isphi(ir->t) && as->phireg[dest] == lref) {
      ra_modified(as, left);
      ra_rename(as, left, dest);
    } else {
      emit_movrr(as, dest, left);
    }
  }
}

/* -- Exit stubs ---------------------------------------------------------- */

/* Generate an exit stub group at the bottom of the reserved MCode memory. */
static MCode *asm_exitstub_gen(ASMState *as, ExitNo group)
{
  ExitNo i, groupofs = (group*EXITSTUBS_PER_GROUP) & 0xff;
  MCode *mxp = as->mcbot;
  MCode *mxpstart = mxp;
  if (mxp + (2+2)*EXITSTUBS_PER_GROUP+8+5 >= as->mctop)
    asm_mclimit(as);
  /* Push low byte of exitno for each exit stub. */
  *mxp++ = XI_PUSHi8; *mxp++ = (MCode)groupofs;
  for (i = 1; i < EXITSTUBS_PER_GROUP; i++) {
    *mxp++ = XI_JMPs; *mxp++ = (MCode)((2+2)*(EXITSTUBS_PER_GROUP - i) - 2);
    *mxp++ = XI_PUSHi8; *mxp++ = (MCode)(groupofs + i);
  }
  /* Push the high byte of the exitno for each exit stub group. */
  *mxp++ = XI_PUSHi8; *mxp++ = (MCode)((group*EXITSTUBS_PER_GROUP)>>8);
  /* Store DISPATCH in ExitInfo->dispatch. Account for the two push ops. */
  *mxp++ = XI_MOVmi;
  *mxp++ = MODRM(XM_OFS8, 0, RID_ESP);
  *mxp++ = MODRM(XM_SCALE1, RID_ESP, RID_ESP);
  *mxp++ = 2*sizeof(void *);
  *(int32_t *)mxp = ptr2addr(J2GG(as->J)->dispatch); mxp += 4;
  /* Jump to exit handler which fills in the ExitState. */
  *mxp++ = XI_JMP; mxp += 4;
  *((int32_t *)(mxp-4)) = (int32_t)((MCode *)lj_vm_exit_handler - mxp);
  /* Commit the code for this group (even if assembly fails later on). */
  lj_mcode_commitbot(as->J, mxp);
  as->mcbot = mxp;
  as->mclim = as->mcbot + MCLIM_REDZONE;
  return mxpstart;
}

/* Setup all needed exit stubs. */
static void asm_exitstub_setup(ASMState *as, ExitNo nexits)
{
  ExitNo i;
  if (nexits >= EXITSTUBS_PER_GROUP*LJ_MAX_EXITSTUBGR)
    lj_trace_err(as->J, LJ_TRERR_SNAPOV);
  for (i = 0; i < (nexits+EXITSTUBS_PER_GROUP-1)/EXITSTUBS_PER_GROUP; i++)
    if (as->J->exitstubgroup[i] == NULL)
      as->J->exitstubgroup[i] = asm_exitstub_gen(as, i);
}

/* -- Snapshot and guard handling ----------------------------------------- */

/* Can we rematerialize a KNUM instead of forcing a spill? */
static int asm_snap_canremat(ASMState *as)
{
  Reg r;
  for (r = RID_MIN_FPR; r < RID_MAX_FPR; r++)
    if (irref_isk(regcost_ref(as->cost[r])))
      return 1;
  return 0;
}

/* Allocate registers or spill slots for refs escaping to a snapshot. */
static void asm_snap_alloc(ASMState *as)
{
  SnapShot *snap = &as->T->snap[as->snapno];
  IRRef2 *map = &as->T->snapmap[snap->mapofs];
  BCReg s, nslots = snap->nslots;
  for (s = 0; s < nslots; s++) {
    IRRef ref = snap_ref(map[s]);
    if (!irref_isk(ref)) {
      IRIns *ir = IR(ref);
      if (!ra_used(ir) && ir->o != IR_FRAME) {
	RegSet allow = irt_isnum(ir->t) ? RSET_FPR : RSET_GPR;
	/* Not a var-to-invar ref and got a free register (or a remat)? */
	if ((!iscrossref(as, ref) || irt_isphi(ir->t)) &&
	    ((as->freeset & allow) ||
	     (allow == RSET_FPR && asm_snap_canremat(as)))) {
	  ra_allocref(as, ref, allow);  /* Allocate a register. */
	  checkmclim(as);
	  RA_DBGX((as, "snapreg   $f $r", ref, ir->r));
	} else {
	  ra_spill(as, ir);  /* Otherwise force a spill slot. */
	  RA_DBGX((as, "snapspill $f $s", ref, ir->s));
	}
      }
    }
  }
}

/* All guards for a snapshot use the same exitno. This is currently the
** same as the snapshot number. Since the exact origin of the exit cannot
** be determined, all guards for the same snapshot must exit with the same
** RegSP mapping.
** A renamed ref which has been used in a prior guard for the same snapshot
** would cause an inconsistency. The easy way out is to force a spill slot.
*/
static int asm_snap_checkrename(ASMState *as, IRRef ren)
{
  SnapShot *snap = &as->T->snap[as->snapno];
  IRRef2 *map = &as->T->snapmap[snap->mapofs];
  BCReg s, nslots = snap->nslots;
  for (s = 0; s < nslots; s++) {
    IRRef ref = snap_ref(map[s]);
    if (ref == ren) {
      IRIns *ir = IR(ref);
      ra_spill(as, ir);  /* Register renamed, so force a spill slot. */
      RA_DBGX((as, "snaprensp $f $s", ref, ir->s));
      return 1;  /* Found. */
    }
  }
  return 0;  /* Not found. */
}

/* Prepare snapshot for next guard instruction. */
static void asm_snap_prep(ASMState *as)
{
  if (as->curins < as->snapref) {
    do {
      lua_assert(as->snapno != 0);
      as->snapno--;
      as->snapref = as->T->snap[as->snapno].ref;
    } while (as->curins < as->snapref);
    asm_snap_alloc(as);
    as->snaprename = as->T->nins;
  } else {
    /* Process any renames above the highwater mark. */
    for (; as->snaprename < as->T->nins; as->snaprename++) {
      IRIns *ir = IR(as->snaprename);
      if (asm_snap_checkrename(as, ir->op1))
	ir->op2 = REF_BIAS-1;  /* Kill rename. */
    }
  }
}

/* Emit conditional branch to exit for guard.
** It's important to emit this *after* all registers have been allocated,
** because rematerializations may invalidate the flags.
*/
static void asm_guardcc(ASMState *as, int cc)
{
  MCode *target = exitstub_addr(as->J, as->snapno);
  MCode *p = as->mcp;
  if (LJ_UNLIKELY(p == as->invmcp)) {
    as->loopinv = 1;
    *(int32_t *)(p+1) = target - (p+5);
    target = p;
    cc ^= 1;
    if (as->realign) {
      emit_sjcc(as, cc, target);
      return;
    }
  }
  emit_jcc(as, cc, target);
}

/* -- Memory operand fusion ----------------------------------------------- */

/* Arch-specific field offsets. */
static const uint8_t field_ofs[IRFL__MAX+1] = {
#define FLOFS(name, type, field)	(uint8_t)offsetof(type, field),
IRFLDEF(FLOFS)
#undef FLOFS
  0
};

/* Limit linear search to this distance. Avoids O(n^2) behavior. */
#define CONFLICT_SEARCH_LIM	15

/* Check if there's no conflicting instruction between curins and ref. */
static int noconflict(ASMState *as, IRRef ref, IROp conflict)
{
  IRIns *ir = as->ir;
  IRRef i = as->curins;
  if (i > ref + CONFLICT_SEARCH_LIM)
    return 0;  /* Give up, ref is too far away. */
  while (--i > ref)
    if (ir[i].o == conflict)
      return 0;  /* Conflict found. */
  return 1;  /* Ok, no conflict. */
}

/* Fuse array reference into memory operand. */
static void asm_fusearef(ASMState *as, IRIns *ir, RegSet allow)
{
  IRIns *irb = IR(ir->op1);
  IRIns *ira, *irx;
  lua_assert(ir->o == IR_AREF);
  lua_assert(irb->o == IR_FLOAD && irb->op2 == IRFL_TAB_ARRAY);
  ira = IR(irb->op1);
  if (ira->o == IR_TNEW && ira->op1 <= LJ_MAX_COLOSIZE &&
      noconflict(as, irb->op1, IR_NEWREF)) {
    /* We can avoid the FLOAD of t->array for colocated arrays. */
    as->mrm.base = (uint8_t)ra_alloc1(as, irb->op1, allow);  /* Table obj. */
    as->mrm.ofs = -(int32_t)(ira->op1*sizeof(TValue));  /* Ofs to colo array. */
  } else {
    as->mrm.base = (uint8_t)ra_alloc1(as, ir->op1, allow);  /* Array base. */
    as->mrm.ofs = 0;
  }
  irx = IR(ir->op2);
  if (irref_isk(ir->op2)) {
    as->mrm.ofs += 8*irx->i;
    as->mrm.idx = RID_NONE;
  } else {
    rset_clear(allow, as->mrm.base);
    as->mrm.scale = XM_SCALE8;
    /* Fuse a constant ADD (e.g. t[i+1]) into the offset.
    ** Doesn't help much without ABCelim, but reduces register pressure.
    */
    if (mayfuse(as, ir->op2) && ra_noreg(irx->r) &&
	irx->o == IR_ADD && irref_isk(irx->op2)) {
      as->mrm.ofs += 8*IR(irx->op2)->i;
      as->mrm.idx = (uint8_t)ra_alloc1(as, irx->op1, allow);
    } else {
      as->mrm.idx = (uint8_t)ra_alloc1(as, ir->op2, allow);
    }
  }
}

/* Fuse array/hash/upvalue reference into memory operand.
** Caveat: this may allocate GPRs for the base/idx registers. Be sure to
** pass the final allow mask, excluding any GPRs used for other inputs.
** In particular: 2-operand GPR instructions need to call ra_dest() first!
*/
static void asm_fuseahuref(ASMState *as, IRRef ref, RegSet allow)
{
  IRIns *ir = IR(ref);
  if (ra_noreg(ir->r)) {
    switch ((IROp)ir->o) {
    case IR_AREF:
      if (mayfuse(as, ref)) {
	asm_fusearef(as, ir, allow);
	return;
      }
      break;
    case IR_HREFK:
      if (mayfuse(as, ref)) {
	as->mrm.base = (uint8_t)ra_alloc1(as, ir->op1, allow);
	as->mrm.ofs = (int32_t)(IR(ir->op2)->op2 * sizeof(Node));
	as->mrm.idx = RID_NONE;
	return;
      }
      break;
    case IR_UREFC:
      if (irref_isk(ir->op1)) {
	GCfunc *fn = ir_kfunc(IR(ir->op1));
	GCupval *uv = &gcref(fn->l.uvptr[ir->op2])->uv;
	as->mrm.ofs = ptr2addr(&uv->tv);
	as->mrm.base = as->mrm.idx = RID_NONE;
	return;
      }
      break;
    default:
      lua_assert(ir->o == IR_HREF || ir->o == IR_NEWREF || ir->o == IR_UREFO);
      break;
    }
  }
  as->mrm.base = (uint8_t)ra_alloc1(as, ref, allow);
  as->mrm.ofs = 0;
  as->mrm.idx = RID_NONE;
}

/* Fuse FLOAD/FREF reference into memory operand. */
static void asm_fusefref(ASMState *as, IRIns *ir, RegSet allow)
{
  lua_assert(ir->o == IR_FLOAD || ir->o == IR_FREF);
  as->mrm.ofs = field_ofs[ir->op2];
  as->mrm.idx = RID_NONE;
  if (irref_isk(ir->op1)) {
    as->mrm.ofs += IR(ir->op1)->i;
    as->mrm.base = RID_NONE;
  } else {
    as->mrm.base = (uint8_t)ra_alloc1(as, ir->op1, allow);
  }
}

/* Fuse string reference into memory operand. */
static void asm_fusestrref(ASMState *as, IRIns *ir, RegSet allow)
{
  IRIns *irr;
  lua_assert(ir->o == IR_STRREF);
  as->mrm.idx = as->mrm.base = RID_NONE;
  as->mrm.scale = XM_SCALE1;
  as->mrm.ofs = sizeof(GCstr);
  if (irref_isk(ir->op1)) {
    as->mrm.ofs += IR(ir->op1)->i;
  } else {
    Reg r = ra_alloc1(as, ir->op1, allow);
    rset_clear(allow, r);
    as->mrm.base = (uint8_t)r;
  }
  irr = IR(ir->op2);
  if (irref_isk(ir->op2)) {
    as->mrm.ofs += irr->i;
  } else {
    Reg r;
    /* Fuse a constant add into the offset, e.g. string.sub(s, i+10). */
    if (mayfuse(as, ir->op2) && irr->o == IR_ADD && irref_isk(irr->op2)) {
      as->mrm.ofs += IR(irr->op2)->i;
      r = ra_alloc1(as, irr->op1, allow);
    } else {
      r = ra_alloc1(as, ir->op2, allow);
    }
    if (as->mrm.base == RID_NONE)
      as->mrm.base = (uint8_t)r;
    else
      as->mrm.idx = (uint8_t)r;
  }
}

/* Fuse load into memory operand. */
static Reg asm_fuseload(ASMState *as, IRRef ref, RegSet allow)
{
  IRIns *ir = IR(ref);
  if (ra_hasreg(ir->r)) {
    if (allow != RSET_EMPTY) return ir->r;  /* Fast path. */
  fusespill:
    /* Force a spill if only memory operands are allowed (asm_x87load). */
    as->mrm.base = RID_ESP;
    as->mrm.ofs = ra_spill(as, ir);
    as->mrm.idx = RID_NONE;
    return RID_MRM;
  }
  if (ir->o == IR_KNUM) {
    lua_assert(allow != RSET_EMPTY);
    if (!(as->freeset & ~as->modset & RSET_FPR)) {
      as->mrm.ofs = ptr2addr(ir_knum(ir));
      as->mrm.base = as->mrm.idx = RID_NONE;
      return RID_MRM;
    }
  } else if (mayfuse(as, ref)) {
    RegSet xallow = (allow & RSET_GPR) ? allow : RSET_GPR;
    if (ir->o == IR_SLOAD) {
      if (!irt_isint(ir->t) && !(ir->op2 & IRSLOAD_PARENT)) {
	as->mrm.base = (uint8_t)ra_alloc1(as, REF_BASE, xallow);
	as->mrm.ofs = 8*((int32_t)ir->op1-1);
	as->mrm.idx = RID_NONE;
	return RID_MRM;
      }
    } else if (ir->o == IR_FLOAD) {
      /* Generic fusion is only ok for IRT_INT operand (but see asm_comp). */
      if (irt_isint(ir->t) && noconflict(as, ref, IR_FSTORE)) {
	asm_fusefref(as, ir, xallow);
	return RID_MRM;
      }
    } else if (ir->o == IR_ALOAD || ir->o == IR_HLOAD || ir->o == IR_ULOAD) {
      if (noconflict(as, ref, ir->o + IRDELTA_L2S)) {
	asm_fuseahuref(as, ir->op1, xallow);
	return RID_MRM;
      }
    } else if (ir->o == IR_XLOAD) {
      /* Generic fusion is only ok for IRT_INT operand (but see asm_comp).
      ** Fusing unaligned memory operands is ok on x86 (except for SIMD types).
      */
      if (irt_isint(ir->t)) {
	asm_fusestrref(as, IR(ir->op1), xallow);
	return RID_MRM;
      }
    }
  }
  if (!(as->freeset & allow) &&
      (allow == RSET_EMPTY || ra_hasspill(ir->s) || ref < as->loopref))
    goto fusespill;
  return ra_allocref(as, ref, allow);
}

/* -- Type conversions ---------------------------------------------------- */

static void asm_tonum(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_FPR);
  Reg left = asm_fuseload(as, ir->op1, RSET_GPR);
  emit_mrm(as, XO_CVTSI2SD, dest, left);
  if (!(as->flags & JIT_F_SPLIT_XMM))
    emit_rr(as, XO_XORPS, dest, dest);  /* Avoid partial register stall. */
}

static void asm_tointg(ASMState *as, IRIns *ir, Reg left)
{
  Reg tmp = ra_scratch(as, rset_exclude(RSET_FPR, left));
  Reg dest = ra_dest(as, ir, RSET_GPR);
  asm_guardcc(as, CC_P);
  asm_guardcc(as, CC_NE);
  emit_rr(as, XO_UCOMISD, left, tmp);
  emit_rr(as, XO_CVTSI2SD, tmp, dest);
  if (!(as->flags & JIT_F_SPLIT_XMM))
    emit_rr(as, XO_XORPS, tmp, tmp);  /* Avoid partial register stall. */
  emit_rr(as, XO_CVTTSD2SI, dest, left);
  /* Can't fuse since left is needed twice. */
}

static void asm_toint(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  Reg left = asm_fuseload(as, ir->op1, RSET_FPR);
  emit_mrm(as, XO_CVTSD2SI, dest, left);
}

static void asm_tobit(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  Reg tmp = ra_noreg(IR(ir->op1)->r) ?
	      ra_alloc1(as, ir->op1, RSET_FPR) :
	      ra_scratch(as, RSET_FPR);
  Reg right = asm_fuseload(as, ir->op2, rset_exclude(RSET_FPR, tmp));
  emit_rr(as, XO_MOVDto, tmp, dest);
  emit_mrm(as, XO_ADDSD, tmp, right);
  ra_left(as, tmp, ir->op1);
}

static void asm_strto(ASMState *as, IRIns *ir)
{
  Reg str;
  int32_t ofs;
  RegSet drop = RSET_SCRATCH;
  /* Force a spill slot for the destination register (if any). */
  if ((drop & RSET_FPR) != RSET_FPR && ra_hasreg(ir->r))
    rset_set(drop, ir->r);  /* WIN64 doesn't spill all FPRs. */
  ra_evictset(as, drop);
  asm_guardcc(as, CC_E);
  emit_rr(as, XO_TEST, RID_RET, RID_RET);
  /* int lj_str_numconv(const char *s, TValue *n) */
  emit_call(as, lj_str_numconv);
  ofs = sps_scale(ir->s);  /* Use spill slot or slots SPS_TEMP1/2. */
  if (ofs == 0) {
    emit_setargr(as, 2, RID_ESP);
  } else {
    emit_setargr(as, 2, RID_RET);
    emit_rmro(as, XO_LEA, RID_RET, RID_ESP, ofs);
  }
  emit_setargr(as, 1, RID_RET);
  str = ra_alloc1(as, ir->op1, RSET_GPR);
  emit_rmro(as, XO_LEA, RID_RET, str, sizeof(GCstr));
}

static void asm_tostr(ASMState *as, IRIns *ir)
{
  IRIns *irl = IR(ir->op1);
  ra_destreg(as, ir, RID_RET);
  ra_evictset(as, rset_exclude(RSET_SCRATCH, RID_RET));
  as->gcsteps++;
  if (irt_isnum(irl->t)) {
    /* GCstr *lj_str_fromnum(lua_State *L, const lua_Number *np) */
    emit_call(as, lj_str_fromnum);
    emit_setargr(as, 1, RID_RET);
    emit_getgl(as, RID_RET, jit_L);
    emit_setargr(as, 2, RID_RET);
    emit_rmro(as, XO_LEA, RID_RET, RID_ESP, ra_spill(as, irl));
  } else {
    /* GCstr *lj_str_fromint(lua_State *L, int32_t k) */
    emit_call(as, lj_str_fromint);
    emit_setargr(as, 1, RID_RET);
    emit_getgl(as, RID_RET, jit_L);
    emit_setargr(as, 2, ra_alloc1(as, ir->op1, RSET_GPR));
  }
}

/* -- Memory references --------------------------------------------------- */

static void asm_aref(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  asm_fusearef(as, ir, RSET_GPR);
  if (!(as->mrm.idx == RID_NONE && as->mrm.ofs == 0))
    emit_mrm(as, XO_LEA, dest, RID_MRM);
  else if (as->mrm.base != dest)
    emit_rr(as, XO_MOV, dest, as->mrm.base);
}

/* Must match with hashkey() and hashrot() in lj_tab.c. */
static uint32_t ir_khash(IRIns *ir)
{
  uint32_t lo, hi;
  if (irt_isstr(ir->t)) {
    return ir_kstr(ir)->hash;
  } else if (irt_isnum(ir->t)) {
    lo = ir_knum(ir)->u32.lo;
    hi = ir_knum(ir)->u32.hi & 0x7fffffff;
  } else if (irt_ispri(ir->t)) {
    lua_assert(!irt_isnil(ir->t));
    return irt_type(ir->t)-IRT_FALSE;
  } else {
    lua_assert(irt_isaddr(ir->t));
    lo = u32ptr(ir_kgc(ir));
    hi = lo - 0x04c11db7;
  }
  lo ^= hi; hi = lj_rol(hi, 14);
  lo -= hi; hi = lj_rol(hi, 5);
  hi ^= lo; hi -= lj_rol(lo, 27);
  return hi;
}

/* Merge NE(HREF, niltv) check. */
static MCode *merge_href_niltv(ASMState *as, IRIns *ir)
{
  /* Assumes nothing else generates NE of HREF. */
  if (ir[1].o == IR_NE && ir[1].op1 == as->curins) {
    if (LJ_64 && *as->mcp != XI_ARITHi)
      as->mcp += 7+6;
    else
      as->mcp += 6+6;  /* Kill cmp reg, imm32 + jz exit. */
    return as->mcp + *(int32_t *)(as->mcp-4);  /* Return exit address. */
  }
  return NULL;
}

/* Inlined hash lookup. Specialized for key type and for const keys.
** The equivalent C code is:
**   Node *n = hashkey(t, key);
**   do {
**     if (lj_obj_equal(&n->key, key)) return &n->val;
**   } while ((n = nextnode(n)));
**   return niltv(L);
*/
static void asm_href(ASMState *as, IRIns *ir)
{
  MCode *nilexit = merge_href_niltv(as, ir);  /* Do this before any restores. */
  RegSet allow = RSET_GPR;
  Reg dest = ra_dest(as, ir, allow);
  Reg tab = ra_alloc1(as, ir->op1, rset_clear(allow, dest));
  Reg key = RID_NONE, tmp = RID_NONE;
  IRIns *irkey = IR(ir->op2);
  int isk = irref_isk(ir->op2);
  IRType1 kt = irkey->t;
  uint32_t khash;
  MCLabel l_end, l_loop, l_next;

  if (!isk) {
    rset_clear(allow, tab);
    key = ra_alloc1(as, ir->op2, irt_isnum(kt) ? RSET_FPR : allow);
    if (!irt_isstr(kt))
      tmp = ra_scratch(as, rset_exclude(allow, key));
  }

  /* Key not found in chain: jump to exit (if merged with NE) or load niltv. */
  l_end = emit_label(as);
  if (nilexit)
    emit_jcc(as, CC_E, nilexit);  /* XI_JMP is not found by lj_asm_patchexit. */
  else
    emit_loada(as, dest, niltvg(J2G(as->J)));

  /* Follow hash chain until the end. */
  l_loop = emit_sjcc_label(as, CC_NZ);
  emit_rr(as, XO_TEST, dest, dest);
  emit_rmro(as, XO_MOV, dest, dest, offsetof(Node, next));
  l_next = emit_label(as);

  /* Type and value comparison. */
  emit_sjcc(as, CC_E, l_end);
  if (irt_isnum(kt)) {
    if (isk) {
      /* Assumes -0.0 is already canonicalized to +0.0. */
      emit_gmroi(as, XG_ARITHi(XOg_CMP), dest, offsetof(Node, key.u32.lo),
		 (int32_t)ir_knum(irkey)->u32.lo);
      emit_sjcc(as, CC_NE, l_next);
      emit_gmroi(as, XG_ARITHi(XOg_CMP), dest, offsetof(Node, key.u32.hi),
		 (int32_t)ir_knum(irkey)->u32.hi);
    } else {
      emit_sjcc(as, CC_P, l_next);
      emit_rmro(as, XO_UCOMISD, key, dest, offsetof(Node, key.n));
      emit_sjcc(as, CC_A, l_next);
      /* The type check avoids NaN penalties and complaints from Valgrind. */
      emit_i8(as, ~IRT_NUM);
      emit_rmro(as, XO_ARITHi8, XOg_CMP, dest, offsetof(Node, key.it));
    }
  } else {
    if (!irt_ispri(kt)) {
      lua_assert(irt_isaddr(kt));
      if (isk)
	emit_gmroi(as, XG_ARITHi(XOg_CMP), dest, offsetof(Node, key.gcr),
		   ptr2addr(ir_kgc(irkey)));
      else
	emit_rmro(as, XO_CMP, key, dest, offsetof(Node, key.gcr));
      emit_sjcc(as, CC_NE, l_next);
    }
    lua_assert(!irt_isnil(kt));
    emit_i8(as, ~irt_type(kt));
    emit_rmro(as, XO_ARITHi8, XOg_CMP, dest, offsetof(Node, key.it));
  }
  emit_sfixup(as, l_loop);
  checkmclim(as);

  /* Load main position relative to tab->node into dest. */
  khash = isk ? ir_khash(irkey) : 1;
  if (khash == 0) {
    emit_rmro(as, XO_MOV, dest, tab, offsetof(GCtab, node));
  } else {
    emit_rmro(as, XO_ARITH(XOg_ADD), dest, tab, offsetof(GCtab, node));
    if ((as->flags & JIT_F_PREFER_IMUL)) {
      emit_i8(as, sizeof(Node));
      emit_rr(as, XO_IMULi8, dest, dest);
    } else {
      emit_shifti(as, XOg_SHL, dest, 3);
      emit_rmrxo(as, XO_LEA, dest, dest, dest, XM_SCALE2, 0);
    }
    if (isk) {
      emit_gri(as, XG_ARITHi(XOg_AND), dest, (int32_t)khash);
      emit_rmro(as, XO_MOV, dest, tab, offsetof(GCtab, hmask));
    } else if (irt_isstr(kt)) {
      emit_rmro(as, XO_ARITH(XOg_AND), dest, key, offsetof(GCstr, hash));
      emit_rmro(as, XO_MOV, dest, tab, offsetof(GCtab, hmask));
    } else {  /* Must match with hashrot() in lj_tab.c. */
      emit_rmro(as, XO_ARITH(XOg_AND), dest, tab, offsetof(GCtab, hmask));
      emit_rr(as, XO_ARITH(XOg_SUB), dest, tmp);
      emit_shifti(as, XOg_ROL, tmp, 27);
      emit_rr(as, XO_ARITH(XOg_XOR), dest, tmp);
      emit_shifti(as, XOg_ROL, dest, 5);
      emit_rr(as, XO_ARITH(XOg_SUB), tmp, dest);
      emit_shifti(as, XOg_ROL, dest, 14);
      emit_rr(as, XO_ARITH(XOg_XOR), tmp, dest);
      if (irt_isnum(kt)) {
	emit_rmro(as, XO_ARITH(XOg_AND), dest, RID_ESP, ra_spill(as, irkey)+4);
	emit_loadi(as, dest, 0x7fffffff);
	emit_rr(as, XO_MOVDto, key, tmp);
      } else {
	emit_rr(as, XO_MOV, tmp, key);
	emit_rmro(as, XO_LEA, dest, key, -0x04c11db7);
      }
    }
  }
}

static void asm_hrefk(ASMState *as, IRIns *ir)
{
  IRIns *kslot = IR(ir->op2);
  IRIns *irkey = IR(kslot->op1);
  int32_t ofs = (int32_t)(kslot->op2 * sizeof(Node));
  Reg dest = ra_used(ir) ? ra_dest(as, ir, RSET_GPR) : RID_NONE;
  Reg node = ra_alloc1(as, ir->op1, RSET_GPR);
  MCLabel l_exit;
  lua_assert(ofs % sizeof(Node) == 0);
  if (ra_hasreg(dest)) {
    if (ofs != 0) {
      if (dest == node && !(as->flags & JIT_F_LEA_AGU))
	emit_gri(as, XG_ARITHi(XOg_ADD), dest, ofs);
      else
	emit_rmro(as, XO_LEA, dest, node, ofs);
    } else if (dest != node) {
      emit_rr(as, XO_MOV, dest, node);
    }
  }
  asm_guardcc(as, CC_NE);
  l_exit = emit_label(as);
  if (irt_isnum(irkey->t)) {
    /* Assumes -0.0 is already canonicalized to +0.0. */
    emit_gmroi(as, XG_ARITHi(XOg_CMP), node,
	       ofs + (int32_t)offsetof(Node, key.u32.lo),
	       (int32_t)ir_knum(irkey)->u32.lo);
    emit_sjcc(as, CC_NE, l_exit);
    emit_gmroi(as, XG_ARITHi(XOg_CMP), node,
	       ofs + (int32_t)offsetof(Node, key.u32.hi),
	       (int32_t)ir_knum(irkey)->u32.hi);
  } else {
    if (!irt_ispri(irkey->t)) {
      lua_assert(irt_isgcv(irkey->t));
      emit_gmroi(as, XG_ARITHi(XOg_CMP), node,
		 ofs + (int32_t)offsetof(Node, key.gcr),
		 ptr2addr(ir_kgc(irkey)));
      emit_sjcc(as, CC_NE, l_exit);
    }
    lua_assert(!irt_isnil(irkey->t));
    emit_i8(as, ~irt_type(irkey->t));
    emit_rmro(as, XO_ARITHi8, XOg_CMP, node,
	      ofs + (int32_t)offsetof(Node, key.it));
  }
}

static void asm_newref(ASMState *as, IRIns *ir)
{
  IRRef keyref = ir->op2;
  IRIns *irkey = IR(keyref);
  RegSet allow = RSET_GPR;
  Reg tab, tmp;
  ra_destreg(as, ir, RID_RET);
  ra_evictset(as, rset_exclude(RSET_SCRATCH, RID_RET));
  tab = ra_alloc1(as, ir->op1, allow);
  tmp = ra_scratch(as, rset_clear(allow, tab));
  /* TValue *lj_tab_newkey(lua_State *L, GCtab *t, cTValue *key) */
  emit_call(as, lj_tab_newkey);
  emit_setargr(as, 1, tmp);
  emit_setargr(as, 2, tab);
  emit_getgl(as, tmp, jit_L);
  if (irt_isnum(irkey->t)) {
    /* For numbers use the constant itself or a spill slot as a TValue. */
    if (irref_isk(keyref)) {
      emit_setargp(as, 3, ir_knum(irkey));
    } else {
      emit_setargr(as, 3, tmp);
      emit_rmro(as, XO_LEA, tmp, RID_ESP, ra_spill(as, irkey));
    }
  } else {
    /* Otherwise use g->tmptv to hold the TValue. */
    lua_assert(irt_ispri(irkey->t) || irt_isaddr(irkey->t));
    emit_setargr(as, 3, tmp);
    if (!irref_isk(keyref)) {
      Reg src = ra_alloc1(as, keyref, rset_exclude(allow, tmp));
      emit_movtomro(as, src, tmp, 0);
    } else if (!irt_ispri(irkey->t)) {
      emit_movmroi(as, tmp, 0, irkey->i);
    }
    emit_movmroi(as, tmp, 4, irt_toitype(irkey->t));
    emit_loada(as, tmp, &J2G(as->J)->tmptv);
  }
}

static void asm_uref(ASMState *as, IRIns *ir)
{
  /* NYI: Check that UREFO is still open and not aliasing a slot. */
  if (ra_used(ir)) {
    Reg dest = ra_dest(as, ir, RSET_GPR);
    if (irref_isk(ir->op1)) {
      GCfunc *fn = ir_kfunc(IR(ir->op1));
      TValue **v = &gcref(fn->l.uvptr[ir->op2])->uv.v;
      emit_rma(as, XO_MOV, dest, v);
    } else {
      Reg uv = ra_scratch(as, RSET_GPR);
      Reg func = ra_alloc1(as, ir->op1, RSET_GPR);
      if (ir->o == IR_UREFC) {
	emit_rmro(as, XO_LEA, dest, uv, offsetof(GCupval, tv));
	asm_guardcc(as, CC_NE);
	emit_i8(as, 1);
	emit_rmro(as, XO_ARITHib, XOg_CMP, uv, offsetof(GCupval, closed));
      } else {
	emit_rmro(as, XO_MOV, dest, uv, offsetof(GCupval, v));
      }
      emit_rmro(as, XO_MOV, uv, func,
		(int32_t)offsetof(GCfuncL, uvptr) + 4*(int32_t)ir->op2);
    }
  }
}

static void asm_fref(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  asm_fusefref(as, ir, RSET_GPR);
  emit_mrm(as, XO_LEA, dest, RID_MRM);
}

static void asm_strref(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  asm_fusestrref(as, ir, RSET_GPR);
  if (as->mrm.base == RID_NONE)
    emit_loadi(as, dest, as->mrm.ofs);
  else if (as->mrm.base == dest && as->mrm.idx == RID_NONE)
    emit_gri(as, XG_ARITHi(XOg_ADD), dest, as->mrm.ofs);
  else
    emit_mrm(as, XO_LEA, dest, RID_MRM);
}

/* -- Loads and stores ---------------------------------------------------- */

static void asm_fload(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  x86Op xo;
  asm_fusefref(as, ir, RSET_GPR);
  switch (irt_type(ir->t)) {
  case IRT_I8: xo = XO_MOVSXb; break;
  case IRT_U8: xo = XO_MOVZXb; break;
  case IRT_I16: xo = XO_MOVSXw; break;
  case IRT_U16: xo = XO_MOVZXw; break;
  default:
    lua_assert(irt_isint(ir->t) || irt_isaddr(ir->t));
    xo = XO_MOV;
    break;
  }
  emit_mrm(as, xo, dest, RID_MRM);
}

static void asm_fstore(ASMState *as, IRIns *ir)
{
  RegSet allow = RSET_GPR;
  Reg src = RID_NONE;
  /* The IRT_I16/IRT_U16 stores should never be simplified for constant
  ** values since mov word [mem], imm16 has a length-changing prefix.
  */
  if (!irref_isk(ir->op2) || irt_isi16(ir->t) || irt_isu16(ir->t)) {
    RegSet allow8 = (irt_isi8(ir->t) || irt_isu8(ir->t)) ? RSET_GPR8 : RSET_GPR;
    src = ra_alloc1(as, ir->op2, allow8);
    rset_clear(allow, src);
  }
  asm_fusefref(as, IR(ir->op1), allow);
  if (ra_hasreg(src)) {
    x86Op xo;
    switch (irt_type(ir->t)) {
    case IRT_I8: case IRT_U8: xo = XO_MOVtob; src |= FORCE_REX; break;
    case IRT_I16: case IRT_U16: xo = XO_MOVtow; break;
    default:
      lua_assert(irt_isint(ir->t) || irt_isaddr(ir->t));
      xo = XO_MOVto;
      break;
    }
    emit_mrm(as, xo, src, RID_MRM);
  } else {
    if (irt_isi8(ir->t) || irt_isu8(ir->t)) {
      emit_i8(as, IR(ir->op2)->i);
      emit_mrm(as, XO_MOVmib, 0, RID_MRM);
    } else {
      lua_assert(irt_isint(ir->t) || irt_isaddr(ir->t));
      emit_i32(as, IR(ir->op2)->i);
      emit_mrm(as, XO_MOVmi, 0, RID_MRM);
    }
  }
}

static void asm_ahuload(ASMState *as, IRIns *ir)
{
  RegSet allow = irt_isnum(ir->t) ? RSET_FPR : RSET_GPR;
  lua_assert(irt_isnum(ir->t) || irt_ispri(ir->t) || irt_isaddr(ir->t));
  if (ra_used(ir)) {
    Reg dest = ra_dest(as, ir, allow);
    asm_fuseahuref(as, ir->op1, RSET_GPR);
    emit_mrm(as, dest < RID_MAX_GPR ? XO_MOV : XMM_MOVRM(as), dest, RID_MRM);
  } else {
    asm_fuseahuref(as, ir->op1, RSET_GPR);
  }
  /* Always do the type check, even if the load result is unused. */
  asm_guardcc(as, irt_isnum(ir->t) ? CC_A : CC_NE);
  emit_i8(as, ~irt_type(ir->t));
  as->mrm.ofs += 4;
  emit_mrm(as, XO_ARITHi8, XOg_CMP, RID_MRM);
}

static void asm_ahustore(ASMState *as, IRIns *ir)
{
  if (irt_isnum(ir->t)) {
    Reg src = ra_alloc1(as, ir->op2, RSET_FPR);
    asm_fuseahuref(as, ir->op1, RSET_GPR);
    emit_mrm(as, XO_MOVSDto, src, RID_MRM);
  } else {
    IRIns *irr = IR(ir->op2);
    RegSet allow = RSET_GPR;
    Reg src = RID_NONE;
    if (!irref_isk(ir->op2)) {
      src = ra_alloc1(as, ir->op2, allow);
      rset_clear(allow, src);
    }
    asm_fuseahuref(as, ir->op1, allow);
    if (ra_hasreg(src)) {
      emit_mrm(as, XO_MOVto, src, RID_MRM);
    } else if (!irt_ispri(irr->t)) {
      lua_assert(irt_isaddr(ir->t));
      emit_i32(as, irr->i);
      emit_mrm(as, XO_MOVmi, 0, RID_MRM);
    }
    as->mrm.ofs += 4;
    emit_i32(as, (int32_t)~irt_type(ir->t));
    emit_mrm(as, XO_MOVmi, 0, RID_MRM);
  }
}

static void asm_sload(ASMState *as, IRIns *ir)
{
  int32_t ofs = 8*((int32_t)ir->op1-1);
  IRType1 t = ir->t;
  Reg base;
  lua_assert(!(ir->op2 & IRSLOAD_PARENT));  /* Handled by asm_head_side(). */
  if (irt_isint(t)) {
    Reg left = ra_scratch(as, RSET_FPR);
    asm_tointg(as, ir, left);  /* Frees dest reg. Do this before base alloc. */
    base = ra_alloc1(as, REF_BASE, RSET_GPR);
    emit_rmro(as, XMM_MOVRM(as), left, base, ofs);
    t.irt = IRT_NUM;  /* Continue with a regular number type check. */
  } else if (ra_used(ir)) {
    RegSet allow = irt_isnum(ir->t) ? RSET_FPR : RSET_GPR;
    Reg dest = ra_dest(as, ir, allow);
    lua_assert(irt_isnum(ir->t) || irt_isaddr(ir->t));
    base = ra_alloc1(as, REF_BASE, RSET_GPR);
    emit_movrmro(as, dest, base, ofs);
  } else {
    if (!irt_isguard(ir->t))
      return;  /* No type check: avoid base alloc. */
    base = ra_alloc1(as, REF_BASE, RSET_GPR);
  }
  if (irt_isguard(ir->t)) {
    /* Need type check, even if the load result is unused. */
    asm_guardcc(as, irt_isnum(t) ? CC_A : CC_NE);
    emit_i8(as, ~irt_type(t));
    emit_rmro(as, XO_ARITHi8, XOg_CMP, base, ofs+4);
  }
}

static void asm_xload(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  x86Op xo;
  asm_fusestrref(as, IR(ir->op1), RSET_GPR);  /* For now only support STRREF. */
  /* ir->op2 is ignored -- unaligned loads are ok on x86. */
  switch (irt_type(ir->t)) {
  case IRT_I8: xo = XO_MOVSXb; break;
  case IRT_U8: xo = XO_MOVZXb; break;
  case IRT_I16: xo = XO_MOVSXw; break;
  case IRT_U16: xo = XO_MOVZXw; break;
  default: lua_assert(irt_isint(ir->t)); xo = XO_MOV; break;
  }
  emit_mrm(as, xo, dest, RID_MRM);
}

/* -- String ops ---------------------------------------------------------- */

static void asm_snew(ASMState *as, IRIns *ir)
{
  RegSet allow = RSET_GPR;
  Reg left, right;
  IRIns *irl;
  ra_destreg(as, ir, RID_RET);
  ra_evictset(as, rset_exclude(RSET_SCRATCH, RID_RET));
  irl = IR(ir->op1);
  left = irl->r;
  right = IR(ir->op2)->r;
  if (ra_noreg(left)) {
    lua_assert(irl->o == IR_STRREF);
    /* Get register only for non-const STRREF. */
    if (!(irref_isk(irl->op1) && irref_isk(irl->op2))) {
      if (ra_hasreg(right)) rset_clear(allow, right);
      left = ra_allocref(as, ir->op1, allow);
    }
  }
  if (ra_noreg(right) && !irref_isk(ir->op2)) {
    if (ra_hasreg(left)) rset_clear(allow, left);
    right = ra_allocref(as, ir->op2, allow);
  }
  /* GCstr *lj_str_new(lua_State *L, const char *str, size_t len) */
  emit_call(as, lj_str_new);
  emit_setargr(as, 1, RID_RET);
  emit_getgl(as, RID_RET, jit_L);
  if (ra_noreg(left))  /* Use immediate for const STRREF. */
    emit_setargi(as, 2, IR(irl->op1)->i + IR(irl->op2)->i +
			(int32_t)sizeof(GCstr));
  else
    emit_setargr(as, 2, left);
  if (ra_noreg(right))
    emit_setargi(as, 3, IR(ir->op2)->i);
  else
    emit_setargr(as, 3, right);
  as->gcsteps++;
}

/* -- Table ops ----------------------------------------------------------- */

static void asm_tnew(ASMState *as, IRIns *ir)
{
  ra_destreg(as, ir, RID_RET);
  ra_evictset(as, rset_exclude(RSET_SCRATCH, RID_RET));
  /* GCtab *lj_tab_new(lua_State *L, int32_t asize, uint32_t hbits) */
  emit_call(as, lj_tab_new);
  emit_setargr(as, 1, RID_RET);
  emit_setargi(as, 2, ir->op1);
  emit_setargi(as, 3, ir->op2);
  emit_getgl(as, RID_RET, jit_L);
  as->gcsteps++;
}

static void asm_tdup(ASMState *as, IRIns *ir)
{
  ra_destreg(as, ir, RID_RET);
  ra_evictset(as, rset_exclude(RSET_SCRATCH, RID_RET));
  /* GCtab *lj_tab_dup(lua_State *L, const GCtab *kt) */
  emit_call(as, lj_tab_dup);
  emit_setargr(as, 1, RID_RET);
  emit_setargp(as, 2, ir_kgc(IR(ir->op1)));
  emit_getgl(as, RID_RET, jit_L);
  as->gcsteps++;
}

static void asm_tlen(ASMState *as, IRIns *ir)
{
  ra_destreg(as, ir, RID_RET);
  ra_evictset(as, rset_exclude(RSET_SCRATCH, RID_RET));
  emit_call(as, lj_tab_len);  /* MSize lj_tab_len(GCtab *t) */
  emit_setargr(as, 1, ra_alloc1(as, ir->op1, RSET_GPR));
}

static void asm_tbar(ASMState *as, IRIns *ir)
{
  Reg tab = ra_alloc1(as, ir->op1, RSET_GPR);
  Reg tmp = ra_scratch(as, rset_exclude(RSET_GPR, tab));
  MCLabel l_end = emit_label(as);
  emit_movtomro(as, tmp, tab, offsetof(GCtab, gclist));
  emit_setgl(as, tab, gc.grayagain);
  emit_getgl(as, tmp, gc.grayagain);
  emit_i8(as, ~LJ_GC_BLACK);
  emit_rmro(as, XO_ARITHib, XOg_AND, tab, offsetof(GCtab, marked));
  emit_sjcc(as, CC_Z, l_end);
  emit_i8(as, LJ_GC_BLACK);
  emit_rmro(as, XO_GROUP3b, XOg_TEST, tab, offsetof(GCtab, marked));
}

static void asm_obar(ASMState *as, IRIns *ir)
{
  RegSet allow = RSET_GPR;
  Reg obj, val;
  GCobj *valp;
  MCLabel l_end;
  int32_t ofs;
  ra_evictset(as, RSET_SCRATCH);
  if (irref_isk(ir->op2)) {
    valp = ir_kgc(IR(ir->op2));
    val = RID_NONE;
  } else {
    valp = NULL;
    val = ra_alloc1(as, ir->op2, allow);
    rset_clear(allow, val);
  }
  obj = ra_alloc1(as, ir->op1, allow);
  l_end = emit_label(as);
  /* No need for other object barriers (yet). */
  lua_assert(IR(ir->op1)->o == IR_UREFC);
  ofs = -(int32_t)offsetof(GCupval, tv);
  /* void lj_gc_barrieruv(global_State *g, GCobj *o, GCobj *v) */
  emit_call(as, lj_gc_barrieruv);
  if (ofs == 0) {
    emit_setargr(as, 2, obj);
  } else if (rset_test(RSET_SCRATCH, obj) && !(as->flags & JIT_F_LEA_AGU)) {
    emit_setargr(as, 2, obj);
    emit_gri(as, XG_ARITHi(XOg_ADD), obj, ofs);
  } else {
    emit_setargr(as, 2, RID_RET);
    emit_rmro(as, XO_LEA, RID_RET, obj, ofs);
  }
  emit_setargp(as, 1, J2G(as->J));
  if (valp)
    emit_setargp(as, 3, valp);
  else
    emit_setargr(as, 3, val);
  emit_sjcc(as, CC_Z, l_end);
  emit_i8(as, LJ_GC_WHITES);
  if (valp)
    emit_rma(as, XO_GROUP3b, XOg_TEST, &valp->gch.marked);
  else
    emit_rmro(as, XO_GROUP3b, XOg_TEST, val, (int32_t)offsetof(GChead, marked));
  emit_sjcc(as, CC_Z, l_end);
  emit_i8(as, LJ_GC_BLACK);
  emit_rmro(as, XO_GROUP3b, XOg_TEST, obj,
	    ofs + (int32_t)offsetof(GChead, marked));
}

/* -- FP/int arithmetic and logic operations ------------------------------ */

/* Load reference onto x87 stack. Force a spill to memory if needed. */
static void asm_x87load(ASMState *as, IRRef ref)
{
  IRIns *ir = IR(ref);
  if (ir->o == IR_KNUM) {
    cTValue *tv = ir_knum(ir);
    if (tvispzero(tv))  /* Use fldz only for +0. */
      emit_x87op(as, XI_FLDZ);
    else if (tvispone(tv))
      emit_x87op(as, XI_FLD1);
    else
      emit_rma(as, XO_FLDq, XOg_FLDq, tv);
  } else if (ir->o == IR_TONUM && !ra_used(ir) &&
	     !irref_isk(ir->op1) && mayfuse(as, ir->op1)) {
    IRIns *iri = IR(ir->op1);
    emit_rmro(as, XO_FILDd, XOg_FILDd, RID_ESP, ra_spill(as, iri));
  } else {
    emit_mrm(as, XO_FLDq, XOg_FLDq, asm_fuseload(as, ref, RSET_EMPTY));
  }
}

/* Try to rejoin pow from EXP2, MUL and LOG2 (if still unsplit). */
static int fpmjoin_pow(ASMState *as, IRIns *ir)
{
  IRIns *irp = IR(ir->op1);
  if (irp == ir-1 && irp->o == IR_MUL && !ra_used(irp)) {
    IRIns *irpp = IR(irp->op1);
    if (irpp == ir-2 && irpp->o == IR_FPMATH &&
	irpp->op2 == IRFPM_LOG2 && !ra_used(irpp)) {
      emit_call(as, lj_vm_pow);  /* st0 = lj_vm_pow(st1, st0) */
      asm_x87load(as, irp->op2);
      asm_x87load(as, irpp->op1);
      return 1;
    }
  }
  return 0;
}

static void asm_fpmath(ASMState *as, IRIns *ir)
{
  IRFPMathOp fpm = ir->o == IR_FPMATH ? (IRFPMathOp)ir->op2 : IRFPM_OTHER;
  if (fpm == IRFPM_SQRT) {
    Reg dest = ra_dest(as, ir, RSET_FPR);
    Reg left = asm_fuseload(as, ir->op1, RSET_FPR);
    emit_mrm(as, XO_SQRTSD, dest, left);
  } else if ((as->flags & JIT_F_SSE4_1) && fpm <= IRFPM_TRUNC) {
    Reg dest = ra_dest(as, ir, RSET_FPR);
    Reg left = asm_fuseload(as, ir->op1, RSET_FPR);
    /* Round down/up/trunc == 1001/1010/1011. */
    emit_i8(as, 0x09 + fpm);
    /* ROUNDSD has a 4-byte opcode which doesn't fit in x86Op. */
    emit_mrm(as, XO_ROUNDSD, dest, left);
    /* Let's pretend it's a 3-byte opcode, and compensate afterwards. */
    /* This is atrocious, but the alternatives are much worse. */
    if (LJ_64 && as->mcp[1] != (MCode)(XO_ROUNDSD >> 16)) {
      as->mcp[0] = as->mcp[1]; as->mcp[1] = 0x0f;  /* Swap 0F and REX. */
    }
    *--as->mcp = 0x66;  /* 1st byte of ROUNDSD opcode. */
  } else {
    int32_t ofs = sps_scale(ir->s);  /* Use spill slot or slots SPS_TEMP1/2. */
    Reg dest = ir->r;
    if (ra_hasreg(dest)) {
      ra_free(as, dest);
      ra_modified(as, dest);
      emit_rmro(as, XMM_MOVRM(as), dest, RID_ESP, ofs);
    }
    emit_rmro(as, XO_FSTPq, XOg_FSTPq, RID_ESP, ofs);
    switch (fpm) {  /* st0 = lj_vm_*(st0) */
    case IRFPM_FLOOR: emit_call(as, lj_vm_floor); break;
    case IRFPM_CEIL: emit_call(as, lj_vm_ceil); break;
    case IRFPM_TRUNC: emit_call(as, lj_vm_trunc); break;
    case IRFPM_EXP: emit_call(as, lj_vm_exp); break;
    case IRFPM_EXP2:
      if (fpmjoin_pow(as, ir)) return;
      emit_call(as, lj_vm_exp2);  /* st0 = lj_vm_exp2(st0) */
      break;
    case IRFPM_SIN: emit_x87op(as, XI_FSIN); break;
    case IRFPM_COS: emit_x87op(as, XI_FCOS); break;
    case IRFPM_TAN: emit_x87op(as, XI_FPOP); emit_x87op(as, XI_FPTAN); break;
    case IRFPM_LOG: case IRFPM_LOG2: case IRFPM_LOG10:
      /* Note: the use of fyl2xp1 would be pointless here. When computing
      ** log(1.0+eps) the precision is already lost after 1.0 is added.
      ** Subtracting 1.0 won't recover it. OTOH math.log1p would make sense.
      */
      emit_x87op(as, XI_FYL2X); break;
    case IRFPM_OTHER:
      switch (ir->o) {
      case IR_ATAN2:
	emit_x87op(as, XI_FPATAN); asm_x87load(as, ir->op2); break;
      case IR_LDEXP:
	emit_x87op(as, XI_FPOP1); emit_x87op(as, XI_FSCALE); break;
      case IR_POWI:
	emit_call(as, lj_vm_powi);  /* st0 = lj_vm_powi(st0, [esp]) */
	emit_rmro(as, XO_MOVto, ra_alloc1(as, ir->op2, RSET_GPR), RID_ESP, 0);
	break;
      default: lua_assert(0); break;
      }
      break;
    default: lua_assert(0); break;
    }
    asm_x87load(as, ir->op1);
    switch (fpm) {
    case IRFPM_LOG: emit_x87op(as, XI_FLDLN2); break;
    case IRFPM_LOG2: emit_x87op(as, XI_FLD1); break;
    case IRFPM_LOG10: emit_x87op(as, XI_FLDLG2); break;
    case IRFPM_OTHER:
      if (ir->o == IR_LDEXP) asm_x87load(as, ir->op2);
      break;
    default: break;
    }
  }
}

/* Find out whether swapping operands might be beneficial. */
static int swapops(ASMState *as, IRIns *ir)
{
  IRIns *irl = IR(ir->op1);
  IRIns *irr = IR(ir->op2);
  lua_assert(ra_noreg(irr->r));
  if (!irm_iscomm(lj_ir_mode[ir->o]))
    return 0;  /* Can't swap non-commutative operations. */
  if (irref_isk(ir->op2))
    return 0;  /* Don't swap constants to the left. */
  if (ra_hasreg(irl->r))
    return 1;  /* Swap if left already has a register. */
  if (ra_samehint(ir->r, irr->r))
    return 1;  /* Swap if dest and right have matching hints. */
  if (ir->op1 < as->loopref && !irt_isphi(irl->t) &&
      !(ir->op2 < as->loopref && !irt_isphi(irr->t)))
    return 1;  /* Swap invariants to the right. */
  if (opisfusableload(irl->o))
    return 1;  /* Swap fusable loads to the right. */
  return 0;  /* Otherwise don't swap. */
}

static void asm_fparith(ASMState *as, IRIns *ir, x86Op xo)
{
  IRRef lref = ir->op1;
  IRRef rref = ir->op2;
  RegSet allow = RSET_FPR;
  Reg dest;
  Reg right = IR(rref)->r;
  if (ra_hasreg(right))
    rset_clear(allow, right);
  dest = ra_dest(as, ir, allow);
  if (lref == rref) {
    right = dest;
  } else if (ra_noreg(right)) {
    if (swapops(as, ir)) {
      IRRef tmp = lref; lref = rref; rref = tmp;
    }
    right = asm_fuseload(as, rref, rset_clear(allow, dest));
  }
  emit_mrm(as, xo, dest, right);
  ra_left(as, dest, lref);
}

static void asm_intarith(ASMState *as, IRIns *ir, x86Arith xa)
{
  IRRef lref = ir->op1;
  IRRef rref = ir->op2;
  RegSet allow = RSET_GPR;
  Reg dest, right;
  if (as->testmcp == as->mcp) {  /* Drop test r,r instruction. */
    as->testmcp = NULL;
    as->mcp += (LJ_64 && *as->mcp != XI_TEST) ? 3 : 2;
  }
  right = IR(rref)->r;
  if (ra_hasreg(right))
    rset_clear(allow, right);
  dest = ra_dest(as, ir, allow);
  if (lref == rref) {
    right = dest;
  } else if (ra_noreg(right) && !irref_isk(rref)) {
    if (swapops(as, ir)) {
      IRRef tmp = lref; lref = rref; rref = tmp;
    }
    right = asm_fuseload(as, rref, rset_clear(allow, dest));
    /* Note: fuses only with IR_FLOAD for now. */
  }
  if (irt_isguard(ir->t))  /* For IR_ADDOV etc. */
    asm_guardcc(as, CC_O);
  if (ra_hasreg(right))
    emit_mrm(as, XO_ARITH(xa), dest, right);
  else
    emit_gri(as, XG_ARITHi(xa), dest, IR(ir->op2)->i);
  ra_left(as, dest, lref);
}

/* LEA is really a 4-operand ADD with an independent destination register,
** up to two source registers and an immediate. One register can be scaled
** by 1, 2, 4 or 8. This can be used to avoid moves or to fuse several
** instructions.
**
** Currently only a few common cases are supported:
** - 3-operand ADD:    y = a+b; y = a+k   with a and b already allocated
** - Left ADD fusion:  y = (a+b)+k; y = (a+k)+b
** - Right ADD fusion: y = a+(b+k)
** The ommited variants have already been reduced by FOLD.
**
** There are more fusion opportunities, like gathering shifts or joining
** common references. But these are probably not worth the trouble, since
** array indexing is not decomposed and already makes use of all fields
** of the ModRM operand.
*/
static int asm_lea(ASMState *as, IRIns *ir)
{
  IRIns *irl = IR(ir->op1);
  IRIns *irr = IR(ir->op2);
  RegSet allow = RSET_GPR;
  Reg dest;
  as->mrm.base = as->mrm.idx = RID_NONE;
  as->mrm.scale = XM_SCALE1;
  as->mrm.ofs = 0;
  if (ra_hasreg(irl->r)) {
    rset_clear(allow, irl->r);
    as->mrm.base = irl->r;
    if (irref_isk(ir->op2) || ra_hasreg(irr->r)) {
      /* The PHI renaming logic does a better job in some cases. */
      if (ra_hasreg(ir->r) &&
	  ((irt_isphi(irl->t) && as->phireg[ir->r] == ir->op1) ||
	   (irt_isphi(irr->t) && as->phireg[ir->r] == ir->op2)))
	return 0;
      if (irref_isk(ir->op2)) {
	as->mrm.ofs = irr->i;
      } else {
	rset_clear(allow, irr->r);
	as->mrm.idx = irr->r;
      }
    } else if (irr->o == IR_ADD && mayfuse(as, ir->op2) &&
	       irref_isk(irr->op2)) {
      Reg idx = ra_alloc1(as, irr->op1, allow);
      rset_clear(allow, idx);
      as->mrm.idx = (uint8_t)idx;
      as->mrm.ofs = IR(irr->op2)->i;
    } else {
      return 0;
    }
  } else if (ir->op1 != ir->op2 && irl->o == IR_ADD && mayfuse(as, ir->op1) &&
	     (irref_isk(ir->op2) || irref_isk(irl->op2))) {
    Reg idx, base = ra_alloc1(as, irl->op1, allow);
    rset_clear(allow, base);
    as->mrm.base = (uint8_t)base;
    if (irref_isk(ir->op2)) {
      as->mrm.ofs = irr->i;
      idx = ra_alloc1(as, irl->op2, allow);
    } else {
      as->mrm.ofs = IR(irl->op2)->i;
      idx = ra_alloc1(as, ir->op2, allow);
    }
    rset_clear(allow, idx);
    as->mrm.idx = (uint8_t)idx;
  } else {
    return 0;
  }
  dest = ra_dest(as, ir, allow);
  emit_mrm(as, XO_LEA, dest, RID_MRM);
  return 1;  /* Success. */
}

static void asm_add(ASMState *as, IRIns *ir)
{
  if (irt_isnum(ir->t))
    asm_fparith(as, ir, XO_ADDSD);
  else if ((as->flags & JIT_F_LEA_AGU) || as->testmcp == as->mcp ||
	   !asm_lea(as, ir))
    asm_intarith(as, ir, XOg_ADD);
}

static void asm_bitnot(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  emit_rr(as, XO_GROUP3, XOg_NOT, dest);
  ra_left(as, dest, ir->op1);
}

static void asm_bitswap(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_GPR);
  MCode *p = as->mcp;
  p[-1] = (MCode)(XI_BSWAP+(dest&7));
  p[-2] = 0x0f;
  p -= 2;
  REXRB(p, 0, dest);
  as->mcp = p;
  ra_left(as, dest, ir->op1);
}

static void asm_bitshift(ASMState *as, IRIns *ir, x86Shift xs)
{
  IRRef rref = ir->op2;
  IRIns *irr = IR(rref);
  Reg dest;
  if (irref_isk(rref)) {  /* Constant shifts. */
    int shift;
    dest = ra_dest(as, ir, RSET_GPR);
    shift = irr->i & 31;  /* Handle shifts of 0..31 bits. */
    switch (shift) {
    case 0: return;
    case 1: emit_rr(as, XO_SHIFT1, (Reg)xs, dest); break;
    default: emit_shifti(as, xs, dest, shift); break;
    }
  } else {  /* Variable shifts implicitly use register cl (i.e. ecx). */
    RegSet allow = rset_exclude(RSET_GPR, RID_ECX);
    Reg right = irr->r;
    if (ra_noreg(right)) {
      right = ra_allocref(as, rref, RID2RSET(RID_ECX));
    } else if (right != RID_ECX) {
      rset_clear(allow, right);
      ra_scratch(as, RID2RSET(RID_ECX));
    }
    dest = ra_dest(as, ir, allow);
    emit_rr(as, XO_SHIFTcl, (Reg)xs, dest);
    if (right != RID_ECX)
      emit_rr(as, XO_MOV, RID_ECX, right);
  }
  ra_left(as, dest, ir->op1);
  /*
  ** Note: avoid using the flags resulting from a shift or rotate!
  ** All of them cause a partial flag stall, except for r,1 shifts
  ** (but not rotates). And a shift count of 0 leaves the flags unmodified.
  */
}

/* -- Comparisons --------------------------------------------------------- */

/* Virtual flags for unordered FP comparisons. */
#define VCC_U	0x100		/* Unordered. */
#define VCC_P	0x200		/* Needs extra CC_P branch. */
#define VCC_S	0x400		/* Swap avoids CC_P branch. */
#define VCC_PS	(VCC_P|VCC_S)

static void asm_comp_(ASMState *as, IRIns *ir, int cc)
{
  if (irt_isnum(ir->t)) {
    IRRef lref = ir->op1;
    IRRef rref = ir->op2;
    Reg left, right;
    MCLabel l_around;
    /*
    ** An extra CC_P branch is required to preserve ordered/unordered
    ** semantics for FP comparisons. This can be avoided by swapping
    ** the operands and inverting the condition (except for EQ and UNE).
    ** So always try to swap if possible.
    **
    ** Another option would be to swap operands to achieve better memory
    ** operand fusion. But it's unlikely that this outweighs the cost
    ** of the extra branches.
    */
    if (cc & VCC_S) {  /* Swap? */
      IRRef tmp = lref; lref = rref; rref = tmp;
      cc ^= (VCC_PS|(5<<4));  /* A <-> B, AE <-> BE, PS <-> none */
    }
    left = ra_alloc1(as, lref, RSET_FPR);
    right = asm_fuseload(as, rref, rset_exclude(RSET_FPR, left));
    l_around = emit_label(as);
    asm_guardcc(as, cc >> 4);
    if (cc & VCC_P) {  /* Extra CC_P branch required? */
      if (!(cc & VCC_U)) {
	asm_guardcc(as, CC_P);  /* Branch to exit for ordered comparisons. */
      } else if (l_around != as->invmcp) {
	emit_sjcc(as, CC_P, l_around);  /* Branch around for unordered. */
      } else {
	/* Patched to mcloop by asm_loop_fixup. */
	as->loopinv = 2;
	if (as->realign)
	  emit_sjcc(as, CC_P, as->mcp);
	else
	  emit_jcc(as, CC_P, as->mcp);
      }
    }
    emit_mrm(as, XO_UCOMISD, left, right);
  } else if (!(irt_isstr(ir->t) && (cc & 0xe) != CC_E)) {
    IRRef lref = ir->op1, rref = ir->op2;
    IROp leftop = (IROp)(IR(lref)->o);
    lua_assert(irt_isint(ir->t) || irt_isaddr(ir->t));
    /* Swap constants (only for ABC) and fusable loads to the right. */
    if (irref_isk(lref) || (!irref_isk(rref) && opisfusableload(leftop))) {
      if ((cc & 0xc) == 0xc) cc ^= 3;  /* L <-> G, LE <-> GE */
      else if ((cc & 0xa) == 0x2) cc ^= 5;  /* A <-> B, AE <-> BE */
      lref = ir->op2; rref = ir->op1;
    }
    if (irref_isk(rref)) {
      IRIns *irl = IR(lref);
      int32_t imm = IR(rref)->i;
      /* Check wether we can use test ins. Not for unsigned, since CF=0. */
      int usetest = (imm == 0 && (cc & 0xa) != 0x2);
      if (usetest && irl->o == IR_BAND && irl+1 == ir && !ra_used(irl)) {
	/* Combine comp(BAND(ref, r/imm), 0) into test mrm, r/imm. */
	Reg right, left = RID_NONE;
	RegSet allow = RSET_GPR;
	if (!irref_isk(irl->op2)) {
	  left = ra_alloc1(as, irl->op2, allow);
	  rset_clear(allow, left);
	}
	right = asm_fuseload(as, irl->op1, allow);
	asm_guardcc(as, cc);
	if (irref_isk(irl->op2)) {
	  emit_i32(as, IR(irl->op2)->i);
	  emit_mrm(as, XO_GROUP3, XOg_TEST, right);
	} else {
	  emit_mrm(as, XO_TEST, left, right);
	}
      } else {
	Reg left;
	if (opisfusableload((IROp)irl->o) &&
	    ((irt_isi8(irl->t) && checki8(imm)) ||
	     (irt_isu8(irl->t) && checku8(imm)))) {
	  /* Only the IRT_INT case is fused by asm_fuseload. The IRT_I8/IRT_U8
	  ** loads are handled here. The IRT_I16/IRT_U16 loads should never be
	  ** fused, since cmp word [mem], imm16 has a length-changing prefix.
	  */
	  IRType1 origt = irl->t;  /* Temporarily flip types. */
	  irl->t.irt = (irl->t.irt & ~IRT_TYPE) | IRT_INT;
	  left = asm_fuseload(as, lref, RSET_GPR);
	  irl->t = origt;
	  if (left == RID_MRM) {  /* Fusion succeeded? */
	    asm_guardcc(as, cc);
	    emit_i8(as, imm);
	    emit_mrm(as, XO_ARITHib, XOg_CMP, RID_MRM);
	    return;
	  }  /* Otherwise handle register case as usual. */
	} else {
	  left = asm_fuseload(as, lref, RSET_GPR);
	}
	asm_guardcc(as, cc);
	if (usetest && left != RID_MRM) {
	  /* Use test r,r instead of cmp r,0. */
	  if (irl+1 == ir)  /* Referencing previous ins? */
	    as->testmcp = as->mcp;  /* Set flag to drop test r,r if possible. */
	  emit_rr(as, XO_TEST, left, left);
	} else {
	  x86Op xo;
	  if (checki8(imm)) {
	    emit_i8(as, imm);
	    xo = XO_ARITHi8;
	  } else {
	    emit_i32(as, imm);
	    xo = XO_ARITHi;
	  }
	  emit_mrm(as, xo, XOg_CMP, left);
	}
      }
    } else {
      Reg left = ra_alloc1(as, lref, RSET_GPR);
      Reg right = asm_fuseload(as, rref, rset_exclude(RSET_GPR, left));
      asm_guardcc(as, cc);
      emit_mrm(as, XO_CMP, left, right);
    }
  } else {  /* Handle ordered string compares. */
    RegSet allow = RSET_GPR;
    /* This assumes lj_str_cmp never uses any SSE registers. */
    ra_evictset(as, (RSET_SCRATCH & RSET_GPR));
    asm_guardcc(as, cc);
    emit_rr(as, XO_TEST, RID_RET, RID_RET);
    emit_call(as, lj_str_cmp);  /* int32_t lj_str_cmp(GCstr *a, GCstr *b) */
    if (irref_isk(ir->op1)) {
      emit_setargi(as, 1, IR(ir->op1)->i);
    } else {
      Reg left = ra_alloc1(as, ir->op1, allow);
      rset_clear(allow, left);
      emit_setargr(as, 1, left);
    }
    if (irref_isk(ir->op2)) {
      emit_setargi(as, 2, IR(ir->op2)->i);
    } else {
      Reg right = ra_alloc1(as, ir->op2, allow);
      emit_setargr(as, 2, right);
    }
  }
}

#define asm_comp(as, ir, ci, cf, cu) \
  asm_comp_(as, ir, (ci)+((cf)<<4)+(cu))

/* -- GC handling --------------------------------------------------------- */

/* Sync all live GC values to Lua stack slots. */
static void asm_gc_sync(ASMState *as, SnapShot *snap, Reg base, RegSet allow)
{
  IRRef2 *map = &as->T->snapmap[snap->mapofs];
  BCReg s, nslots = snap->nslots;
  for (s = 0; s < nslots; s++) {
    IRRef ref = snap_ref(map[s]);
    if (!irref_isk(ref)) {
      IRIns *ir = IR(ref);
      if (ir->o == IR_FRAME) {
	/* NYI: sync the frame, bump base, set topslot, clear new slots. */
	lj_trace_err(as->J, LJ_TRERR_NYIGCF);
      } else if (irt_isgcv(ir->t) &&
	       !(ir->o == IR_SLOAD && ir->op1 < nslots && map[ir->op1] == 0)) {
	Reg src = ra_alloc1(as, ref, allow);
	int32_t ofs = 8*(int32_t)(s-1);
	emit_movtomro(as, src, base, ofs);
	emit_movmroi(as, base, ofs+4, irt_toitype(ir->t));
	checkmclim(as);
      }
    }
  }
}

/* Check GC threshold and do one or more GC steps. */
static void asm_gc_check(ASMState *as, SnapShot *snap)
{
  MCLabel l_end;
  const BCIns *pc;
  Reg tmp, base;
  RegSet drop = RSET_SCRATCH;
  /* Must evict BASE because the stack may be reallocated by the GC. */
  if (ra_hasreg(IR(REF_BASE)->r))
    drop |= RID2RSET(IR(REF_BASE)->r);
  ra_evictset(as, drop);
  base = ra_alloc1(as, REF_BASE, rset_exclude(RSET_GPR, RID_RET));
  l_end = emit_label(as);
  /* void lj_gc_step_jit(lua_State *L, const BCIns *pc, MSize steps) */
  emit_call(as, lj_gc_step_jit);
  emit_movtomro(as, base, RID_RET, offsetof(lua_State, base));
  emit_setargr(as, 1, RID_RET);
  emit_setargi(as, 3, (int32_t)as->gcsteps);
  emit_getgl(as, RID_RET, jit_L);
  pc = (const BCIns *)(uintptr_t)as->T->snapmap[snap->mapofs+snap->nslots];
  emit_setargp(as, 2, pc);
  asm_gc_sync(as, snap, base, rset_exclude(RSET_SCRATCH & RSET_GPR, base));
  if (as->curins == as->loopref)  /* BASE gets restored by LOOP anyway. */
    ra_restore(as, REF_BASE);  /* Better do it inside the slow path. */
  /* Jump around GC step if GC total < GC threshold. */
  tmp = ra_scratch(as, RSET_SCRATCH & RSET_GPR);
  emit_sjcc(as, CC_B, l_end);
  emit_opgl(as, XO_ARITH(XOg_CMP), tmp, gc.threshold);
  emit_getgl(as, tmp, gc.total);
  as->gcsteps = 0;
  checkmclim(as);
}

/* -- PHI and loop handling ----------------------------------------------- */

/* Break a PHI cycle by renaming to a free register (evict if needed). */
static void asm_phi_break(ASMState *as, RegSet blocked, RegSet blockedby,
			  RegSet allow)
{
  RegSet candidates = blocked & allow;
  if (candidates) {  /* If this register file has candidates. */
    /* Note: the set for ra_pick cannot be empty, since each register file
    ** has some registers never allocated to PHIs.
    */
    Reg down, up = ra_pick(as, ~blocked & allow);  /* Get a free register. */
    if (candidates & ~blockedby)  /* Optimize shifts, else it's a cycle. */
      candidates = candidates & ~blockedby;
    down = rset_picktop(candidates);  /* Pick candidate PHI register. */
    ra_rename(as, down, up);  /* And rename it to the free register. */
  }
}

/* PHI register shuffling.
**
** The allocator tries hard to preserve PHI register assignments across
** the loop body. Most of the time this loop does nothing, since there
** are no register mismatches.
**
** If a register mismatch is detected and ...
** - the register is currently free: rename it.
** - the register is blocked by an invariant: restore/remat and rename it.
** - Otherwise the register is used by another PHI, so mark it as blocked.
**
** The renames are order-sensitive, so just retry the loop if a register
** is marked as blocked, but has been freed in the meantime. A cycle is
** detected if all of the blocked registers are allocated. To break the
** cycle rename one of them to a free register and retry.
**
** Note that PHI spill slots are kept in sync and don't need to be shuffled.
*/
static void asm_phi_shuffle(ASMState *as)
{
  RegSet work;

  /* Find and resolve PHI register mismatches. */
  for (;;) {
    RegSet blocked = RSET_EMPTY;
    RegSet blockedby = RSET_EMPTY;
    RegSet phiset = as->phiset;
    while (phiset) {  /* Check all left PHI operand registers. */
      Reg r = rset_picktop(phiset);
      IRIns *irl = IR(as->phireg[r]);
      Reg left = irl->r;
      if (r != left) {  /* Mismatch? */
	if (!rset_test(as->freeset, r)) {  /* PHI register blocked? */
	  IRRef ref = regcost_ref(as->cost[r]);
	  if (irt_ismarked(IR(ref)->t)) {  /* Blocked by other PHI (w/reg)? */
	    rset_set(blocked, r);
	    if (ra_hasreg(left))
	      rset_set(blockedby, left);
	    left = RID_NONE;
	  } else {  /* Otherwise grab register from invariant. */
	    ra_restore(as, ref);
	    checkmclim(as);
	  }
	}
	if (ra_hasreg(left)) {
	  ra_rename(as, left, r);
	  checkmclim(as);
	}
      }
      rset_clear(phiset, r);
    }
    if (!blocked) break;  /* Finished. */
    if (!(as->freeset & blocked)) {  /* Break cycles if none are free. */
      asm_phi_break(as, blocked, blockedby, RSET_GPR);
      asm_phi_break(as, blocked, blockedby, RSET_FPR);
      checkmclim(as);
    }  /* Else retry some more renames. */
  }

  /* Restore/remat invariants whose registers are modified inside the loop. */
  work = as->modset & ~(as->freeset | as->phiset);
  while (work) {
    Reg r = rset_picktop(work);
    ra_restore(as, regcost_ref(as->cost[r]));
    rset_clear(work, r);
    checkmclim(as);
  }

  /* Allocate and save all unsaved PHI regs and clear marks. */
  work = as->phiset;
  while (work) {
    Reg r = rset_picktop(work);
    IRRef lref = as->phireg[r];
    IRIns *ir = IR(lref);
    if (ra_hasspill(ir->s)) {  /* Left PHI gained a spill slot? */
      irt_clearmark(ir->t);  /* Handled here, so clear marker now. */
      ra_alloc1(as, lref, RID2RSET(r));
      ra_save(as, ir, r);  /* Save to spill slot inside the loop. */
      checkmclim(as);
    }
    rset_clear(work, r);
  }
}

/* Emit renames for left PHIs which are only spilled outside the loop. */
static void asm_phi_fixup(ASMState *as)
{
  RegSet work = as->phiset;
  while (work) {
    Reg r = rset_picktop(work);
    IRRef lref = as->phireg[r];
    IRIns *ir = IR(lref);
    /* Left PHI gained a spill slot before the loop? */
    if (irt_ismarked(ir->t) && ra_hasspill(ir->s)) {
      IRRef ren;
      lj_ir_set(as->J, IRT(IR_RENAME, IRT_NIL), lref, as->loopsnapno);
      ren = tref_ref(lj_ir_emit(as->J));
      as->ir = as->T->ir;  /* The IR may have been reallocated. */
      IR(ren)->r = (uint8_t)r;
      IR(ren)->s = SPS_NONE;
    }
    irt_clearmark(ir->t);  /* Always clear marker. */
    rset_clear(work, r);
  }
}

/* Setup right PHI reference. */
static void asm_phi(ASMState *as, IRIns *ir)
{
  RegSet allow = irt_isnum(ir->t) ? RSET_FPR : RSET_GPR;
  RegSet afree = (as->freeset & allow);
  IRIns *irl = IR(ir->op1);
  IRIns *irr = IR(ir->op2);
  /* Spill slot shuffling is not implemented yet (but rarely needed). */
  if (ra_hasspill(irl->s) || ra_hasspill(irr->s))
    lj_trace_err(as->J, LJ_TRERR_NYIPHI);
  /* Leave at least one register free for non-PHIs (and PHI cycle breaking). */
  if ((afree & (afree-1))) {  /* Two or more free registers? */
    Reg r;
    if (ra_noreg(irr->r)) {  /* Get a register for the right PHI. */
      r = ra_allocref(as, ir->op2, allow);
    } else {  /* Duplicate right PHI, need a copy (rare). */
      r = ra_scratch(as, allow);
      emit_movrr(as, r, irr->r);
    }
    ir->r = (uint8_t)r;
    rset_set(as->phiset, r);
    as->phireg[r] = (IRRef1)ir->op1;
    irt_setmark(irl->t);  /* Marks left PHIs _with_ register. */
    if (ra_noreg(irl->r))
      ra_sethint(irl->r, r); /* Set register hint for left PHI. */
  } else {  /* Otherwise allocate a spill slot. */
    /* This is overly restrictive, but it triggers only on synthetic code. */
    if (ra_hasreg(irl->r) || ra_hasreg(irr->r))
      lj_trace_err(as->J, LJ_TRERR_NYIPHI);
    ra_spill(as, ir);
    irl->s = irr->s = ir->s;  /* Sync left/right PHI spill slots. */
  }
}

/* Fixup the loop branch. */
static void asm_loop_fixup(ASMState *as)
{
  MCode *p = as->mctop;
  MCode *target = as->mcp;
  if (as->realign) {  /* Realigned loops use short jumps. */
    as->realign = NULL;  /* Stop another retry. */
    lua_assert(((intptr_t)target & 15) == 0);
    if (as->loopinv) {  /* Inverted loop branch? */
      p -= 5;
      p[0] = XI_JMP;
      lua_assert(target - p >= -128);
      p[-1] = (MCode)(target - p);  /* Patch sjcc. */
      if (as->loopinv == 2)
	p[-3] = (MCode)(target - p + 2);  /* Patch opt. short jp. */
    } else {
      lua_assert(target - p >= -128);
      p[-1] = (MCode)(int8_t)(target - p);  /* Patch short jmp. */
      p[-2] = XI_JMPs;
    }
  } else {
    MCode *newloop;
    p[-5] = XI_JMP;
    if (as->loopinv) {  /* Inverted loop branch? */
      /* asm_guardcc already inverted the jcc and patched the jmp. */
      p -= 5;
      newloop = target+4;
      *(int32_t *)(p-4) = (int32_t)(target - p);  /* Patch jcc. */
      if (as->loopinv == 2) {
	*(int32_t *)(p-10) = (int32_t)(target - p + 6);  /* Patch opt. jp. */
	newloop = target+8;
      }
    } else {  /* Otherwise just patch jmp. */
      *(int32_t *)(p-4) = (int32_t)(target - p);
      newloop = target+3;
    }
    /* Realign small loops and shorten the loop branch. */
    if (newloop >= p - 128) {
      as->realign = newloop;  /* Force a retry and remember alignment. */
      as->curins = as->stopins;  /* Abort asm_trace now. */
      as->T->nins = as->orignins;  /* Remove any added renames. */
    }
  }
}

/* Middle part of a loop. */
static void asm_loop(ASMState *as)
{
  /* LOOP is a guard, so the snapno is up to date. */
  as->loopsnapno = as->snapno;
  if (as->gcsteps)
    asm_gc_check(as, &as->T->snap[as->loopsnapno]);
  /* LOOP marks the transition from the variant to the invariant part. */
  as->testmcp = as->invmcp = NULL;
  as->sectref = 0;
  if (!neverfuse(as)) as->fuseref = 0;
  asm_phi_shuffle(as);
  asm_loop_fixup(as);
  as->mcloop = as->mcp;
  RA_DBGX((as, "===== LOOP ====="));
  if (!as->realign) RA_DBG_FLUSH();
}

/* -- Head of trace ------------------------------------------------------- */

/* Rematerialize all remaining constants in registers. */
static void asm_const_remat(ASMState *as)
{
  RegSet work = ~as->freeset & RSET_ALL;
  while (work) {
    Reg r = rset_pickbot(work);
    IRRef ref = regcost_ref(as->cost[r]);
    if (irref_isk(ref) || ref == REF_BASE) {
      ra_rematk(as, IR(ref));
      checkmclim(as);
    }
    rset_clear(work, r);
  }
}

/* Head of a root trace. */
static void asm_head_root(ASMState *as)
{
  int32_t spadj;
  emit_setgli(as, vmstate, (int32_t)as->J->curtrace);
  spadj = sps_adjust(as);
  as->T->spadjust = (uint16_t)spadj;
  emit_addptr(as, RID_ESP, -spadj);
}

/* Handle BASE coalescing for a root trace. */
static void asm_head_base(ASMState *as)
{
  IRIns *ir = IR(REF_BASE);
  Reg r = ir->r;
  lua_assert(ra_hasreg(r) && !ra_hasspill(ir->s));
  ra_free(as, r);
  if (r != RID_BASE) {
    ra_scratch(as, RID2RSET(RID_BASE));
    emit_rr(as, XO_MOV, r, RID_BASE);
  }
}

/* Check Lua stack size for overflow at the start of a side trace.
** Stack overflow is rare, so let the regular exit handling fix this up.
** This is done in the context of the *parent* trace and parent exitno!
*/
static void asm_checkstack(ASMState *as, RegSet allow)
{
  /* Try to get an unused temp. register, otherwise spill/restore eax. */
  Reg r = allow ? rset_pickbot(allow) : RID_EAX;
  emit_jcc(as, CC_B, exitstub_addr(as->J, as->J->exitno));
  if (allow == RSET_EMPTY)  /* Restore temp. register. */
    emit_rmro(as, XO_MOV, r, RID_ESP, sps_scale(SPS_TEMP1));
  emit_gri(as, XG_ARITHi(XOg_CMP), r, (int32_t)(8*as->topslot));
  emit_rmro(as, XO_ARITH(XOg_SUB), r, RID_NONE, ptr2addr(&J2G(as->J)->jit_base));
  emit_rmro(as, XO_MOV, r, r, offsetof(lua_State, maxstack));
  emit_getgl(as, r, jit_L);
  if (allow == RSET_EMPTY)  /* Spill temp. register. */
    emit_rmro(as, XO_MOVto, r, RID_ESP, sps_scale(SPS_TEMP1));
}

/* Head of a side trace.
**
** The current simplistic algorithm requires that all slots inherited
** from the parent are live in a register between pass 2 and pass 3. This
** avoids the complexity of stack slot shuffling. But of course this may
** overflow the register set in some cases and cause the dreaded error:
** "NYI: register coalescing too complex". A refined algorithm is needed.
*/
static void asm_head_side(ASMState *as)
{
  IRRef1 sloadins[RID_MAX];
  RegSet allow = RSET_ALL;  /* Inverse of all coalesced registers. */
  RegSet live = RSET_EMPTY;  /* Live parent registers. */
  int32_t spadj, spdelta;
  int pass2 = 0;
  int pass3 = 0;
  IRRef i;

  /* Scan all parent SLOADs and collect register dependencies. */
  for (i = as->curins; i > REF_BASE; i--) {
    IRIns *ir = IR(i);
    lua_assert((ir->o == IR_SLOAD && (ir->op2 & IRSLOAD_PARENT)) ||
	       ir->o == IR_FRAME);
    if (ir->o == IR_SLOAD) {
      RegSP rs = as->parentmap[ir->op1];
      if (ra_hasreg(ir->r)) {
	rset_clear(allow, ir->r);
	if (ra_hasspill(ir->s))
	  ra_save(as, ir, ir->r);
      } else if (ra_hasspill(ir->s)) {
	irt_setmark(ir->t);
	pass2 = 1;
      }
      if (ir->r == rs) {  /* Coalesce matching registers right now. */
	ra_free(as, ir->r);
      } else if (ra_hasspill(regsp_spill(rs))) {
	if (ra_hasreg(ir->r))
	  pass3 = 1;
      } else if (ra_used(ir)) {
	sloadins[rs] = (IRRef1)i;
	rset_set(live, rs);  /* Block live parent register. */
      }
    }
  }

  /* Calculate stack frame adjustment. */
  spadj = sps_adjust(as);
  spdelta = spadj - (int32_t)as->parent->spadjust;
  if (spdelta < 0) {  /* Don't shrink the stack frame. */
    spadj = (int32_t)as->parent->spadjust;
    spdelta = 0;
  }
  as->T->spadjust = (uint16_t)spadj;

  /* Reload spilled target registers. */
  if (pass2) {
    for (i = as->curins; i > REF_BASE; i--) {
      IRIns *ir = IR(i);
      if (irt_ismarked(ir->t)) {
	RegSet mask;
	Reg r;
	RegSP rs;
	irt_clearmark(ir->t);
	rs = as->parentmap[ir->op1];
	if (!ra_hasspill(regsp_spill(rs)))
	  ra_sethint(ir->r, rs);  /* Hint may be gone, set it again. */
	else if (sps_scale(regsp_spill(rs))+spdelta == sps_scale(ir->s))
	  continue;  /* Same spill slot, do nothing. */
	mask = (irt_isnum(ir->t) ? RSET_FPR : RSET_GPR) & allow;
	if (mask == RSET_EMPTY)
	  lj_trace_err(as->J, LJ_TRERR_NYICOAL);
	r = ra_allocref(as, i, mask);
	ra_save(as, ir, r);
	rset_clear(allow, r);
	if (r == rs) {  /* Coalesce matching registers right now. */
	  ra_free(as, r);
	  rset_clear(live, r);
	} else if (ra_hasspill(regsp_spill(rs))) {
	  pass3 = 1;
	}
	checkmclim(as);
      }
    }
  }

  /* Store trace number and adjust stack frame relative to the parent. */
  emit_setgli(as, vmstate, (int32_t)as->J->curtrace);
  emit_addptr(as, RID_ESP, -spdelta);

  /* Restore target registers from parent spill slots. */
  if (pass3) {
    RegSet work = ~as->freeset & RSET_ALL;
    while (work) {
      Reg r = rset_pickbot(work);
      IRIns *ir = IR(regcost_ref(as->cost[r]));
      RegSP rs = as->parentmap[ir->op1];
      rset_clear(work, r);
      if (ra_hasspill(regsp_spill(rs))) {
	int32_t ofs = sps_scale(regsp_spill(rs));
	ra_free(as, r);
	emit_movrmro(as, r, RID_ESP, ofs);
	checkmclim(as);
      }
    }
  }

  /* Shuffle registers to match up target regs with parent regs. */
  for (;;) {
    RegSet work;

    /* Repeatedly coalesce free live registers by moving to their target. */
    while ((work = as->freeset & live) != RSET_EMPTY) {
      Reg rp = rset_pickbot(work);
      IRIns *ir = IR(sloadins[rp]);
      rset_clear(live, rp);
      rset_clear(allow, rp);
      ra_free(as, ir->r);
      emit_movrr(as, ir->r, rp);
      checkmclim(as);
    }

    /* We're done if no live registers remain. */
    if (live == RSET_EMPTY)
      break;

    /* Break cycles by renaming one target to a temp. register. */
    if (live & RSET_GPR) {
      RegSet tmpset = as->freeset & ~live & allow & RSET_GPR;
      if (tmpset == RSET_EMPTY)
	lj_trace_err(as->J, LJ_TRERR_NYICOAL);
      ra_rename(as, rset_pickbot(live & RSET_GPR), rset_pickbot(tmpset));
    }
    if (live & RSET_FPR) {
      RegSet tmpset = as->freeset & ~live & allow & RSET_FPR;
      if (tmpset == RSET_EMPTY)
	lj_trace_err(as->J, LJ_TRERR_NYICOAL);
      ra_rename(as, rset_pickbot(live & RSET_FPR), rset_pickbot(tmpset));
    }
    checkmclim(as);
    /* Continue with coalescing to fix up the broken cycle(s). */
  }

  /* Check Lua stack size if frames have been added. */
  if (as->topslot)
    asm_checkstack(as, allow & RSET_GPR);
}

/* -- Tail of trace ------------------------------------------------------- */

/* Sync Lua stack slots to match the last snapshot.
** Note: code generation is backwards, so this is best read bottom-up.
*/
static void asm_tail_sync(ASMState *as)
{
  SnapShot *snap = &as->T->snap[as->T->nsnap-1];  /* Last snapshot. */
  BCReg s, nslots = snap->nslots;
  IRRef2 *map = &as->T->snapmap[snap->mapofs];
  IRRef2 *flinks = map + nslots + snap->nframelinks;
  BCReg newbase = 0;
  BCReg secondbase = ~(BCReg)0;
  BCReg topslot = 0;

  checkmclim(as);
  ra_allocref(as, REF_BASE, RID2RSET(RID_BASE));

  /* Must check all frames to find topslot (outer can be larger than inner). */
  for (s = 0; s < nslots; s++) {
    IRRef ref = snap_ref(map[s]);
    if (!irref_isk(ref)) {
      IRIns *ir = IR(ref);
      if (ir->o == IR_FRAME && irt_isfunc(ir->t)) {
	GCfunc *fn = ir_kfunc(IR(ir->op2));
	if (isluafunc(fn)) {
	  BCReg fs = s + funcproto(fn)->framesize;
	  newbase = s;
	  if (secondbase == ~(BCReg)0) secondbase = s;
	  if (fs > topslot) topslot = fs;
	}
      }
    }
  }
  as->topslot = topslot;  /* Used in asm_head_side(). */

  if (as->T->link == TRACE_INTERP) {
    /* Setup fixed registers for exit to interpreter. */
    emit_loada(as, RID_DISPATCH, J2GG(as->J)->dispatch);
    emit_loadi(as, RID_PC, (int32_t)map[nslots]);
  } else if (newbase) {
    /* Save modified BASE for linking to trace with higher start frame. */
    emit_setgl(as, RID_BASE, jit_base);
  }

  emit_addptr(as, RID_BASE, 8*(int32_t)newbase);

  /* Clear stack slots of newly added frames. */
  if (nslots <= topslot) {
    if (nslots < topslot) {
      for (s = nslots; s <= topslot; s++) {
	emit_movtomro(as, RID_EAX, RID_BASE, 8*(int32_t)s-4);
	checkmclim(as);
      }
      emit_loadi(as, RID_EAX, LJ_TNIL);
    } else {
      emit_movmroi(as, RID_BASE, 8*(int32_t)nslots-4, LJ_TNIL);
    }
  }

  /* Store the value of all modified slots to the Lua stack. */
  for (s = 0; s < nslots; s++) {
    int32_t ofs = 8*((int32_t)s-1);
    IRRef ref = snap_ref(map[s]);
    if (ref) {
      IRIns *ir = IR(ref);
      /* No need to restore readonly slots and unmodified non-parent slots. */
      if (ir->o == IR_SLOAD && ir->op1 == s &&
	  (ir->op2 & (IRSLOAD_READONLY|IRSLOAD_PARENT)) != IRSLOAD_PARENT)
	continue;
      if (irt_isnum(ir->t)) {
	Reg src = ra_alloc1(as, ref, RSET_FPR);
	emit_rmro(as, XO_MOVSDto, src, RID_BASE, ofs);
      } else if (ir->o == IR_FRAME) {
	emit_movmroi(as, RID_BASE, ofs, ptr2addr(ir_kgc(IR(ir->op2))));
	if (s != 0)  /* Do not overwrite link to previous frame. */
	  emit_movmroi(as, RID_BASE, ofs+4, (int32_t)(*--flinks));
      } else {
	lua_assert(irt_ispri(ir->t) || irt_isaddr(ir->t));
	if (!irref_isk(ref)) {
	  Reg src = ra_alloc1(as, ref, rset_exclude(RSET_GPR, RID_BASE));
	  emit_movtomro(as, src, RID_BASE, ofs);
	} else if (!irt_ispri(ir->t)) {
	  emit_movmroi(as, RID_BASE, ofs, ir->i);
	}
	emit_movmroi(as, RID_BASE, ofs+4, irt_toitype(ir->t));
      }
    } else if (s > secondbase) {
      emit_movmroi(as, RID_BASE, ofs+4, LJ_TNIL);
    }
    checkmclim(as);
  }
  lua_assert(map + nslots == flinks-1);
}

/* Fixup the tail code. */
static void asm_tail_fixup(ASMState *as, TraceNo lnk)
{
  /* Note: don't use as->mcp swap + emit_*: emit_op overwrites more bytes. */
  MCode *p = as->mctop;
  MCode *target, *q;
  int32_t spadj = as->T->spadjust;
  if (spadj == 0) {
    p -= (as->flags & JIT_F_LEA_AGU) ? 7 : 6;
  } else {
    MCode *p1;
    /* Patch stack adjustment. */
    if (checki8(spadj)) {
      p -= 3;
      p1 = p-6;
      *p1 = (MCode)spadj;
    } else {
      p1 = p-9;
      *(int32_t *)p1 = spadj;
    }
    if ((as->flags & JIT_F_LEA_AGU)) {
      p1[-3] = (MCode)XI_LEA;
      p1[-2] = MODRM(checki8(spadj) ? XM_OFS8 : XM_OFS32, RID_ESP, RID_ESP);
      p1[-1] = MODRM(XM_SCALE1, RID_ESP, RID_ESP);
    } else {
      p1[-2] = (MCode)(checki8(spadj) ? XI_ARITHi8 : XI_ARITHi);
      p1[-1] = MODRM(XM_REG, XOg_ADD, RID_ESP);
    }
  }
  /* Patch exit branch. */
  target = lnk == TRACE_INTERP ? (MCode *)lj_vm_exit_interp :
				 as->J->trace[lnk]->mcode;
  *(int32_t *)(p-4) = (int32_t)(target - p);
  p[-5] = XI_JMP;
  /* Drop unused mcode tail. Fill with NOPs to make the prefetcher happy. */
  for (q = as->mctop-1; q >= p; q--)
    *q = XI_NOP;
  as->mctop = p;
}

/* -- Instruction dispatch ------------------------------------------------ */

/* Assemble a single instruction. */
static void asm_ir(ASMState *as, IRIns *ir)
{
  switch ((IROp)ir->o) {
  /* Miscellaneous ops. */
  case IR_LOOP: asm_loop(as); break;
  case IR_NOP: break;
  case IR_PHI: asm_phi(as, ir); break;

  /* Guarded assertions. */
  case IR_LT:  asm_comp(as, ir, CC_GE, CC_AE, VCC_PS); break;
  case IR_GE:  asm_comp(as, ir, CC_L,  CC_B,  0); break;
  case IR_LE:  asm_comp(as, ir, CC_G,  CC_A,  VCC_PS); break;
  case IR_GT:  asm_comp(as, ir, CC_LE, CC_BE, 0); break;
  case IR_ULT: asm_comp(as, ir, CC_AE, CC_AE, VCC_U); break;
  case IR_UGE: asm_comp(as, ir, CC_B,  CC_B,  VCC_U|VCC_PS); break;
  case IR_ULE: asm_comp(as, ir, CC_A,  CC_A,  VCC_U); break;
  case IR_ABC:
  case IR_UGT: asm_comp(as, ir, CC_BE, CC_BE, VCC_U|VCC_PS); break;

  case IR_FRAME:
    if (ir->op1 == ir->op2) break;  /* No check needed for placeholder. */
    /* fallthrough */
  case IR_EQ:  asm_comp(as, ir, CC_NE, CC_NE, VCC_P); break;
  case IR_NE:  asm_comp(as, ir, CC_E,  CC_E,  VCC_U|VCC_P); break;

  /* Bit ops. */
  case IR_BNOT: asm_bitnot(as, ir); break;
  case IR_BSWAP: asm_bitswap(as, ir); break;

  case IR_BAND: asm_intarith(as, ir, XOg_AND); break;
  case IR_BOR:  asm_intarith(as, ir, XOg_OR); break;
  case IR_BXOR: asm_intarith(as, ir, XOg_XOR); break;

  case IR_BSHL: asm_bitshift(as, ir, XOg_SHL); break;
  case IR_BSHR: asm_bitshift(as, ir, XOg_SHR); break;
  case IR_BSAR: asm_bitshift(as, ir, XOg_SAR); break;
  case IR_BROL: asm_bitshift(as, ir, XOg_ROL); break;
  case IR_BROR: asm_bitshift(as, ir, XOg_ROR); break;

  /* Arithmetic ops. */
  case IR_ADD: asm_add(as, ir); break;
  case IR_SUB:
    if (irt_isnum(ir->t))
      asm_fparith(as, ir, XO_SUBSD);
    else  /* Note: no need for LEA trick here. i-k is encoded as i+(-k). */
      asm_intarith(as, ir, XOg_SUB);
    break;
  case IR_MUL: asm_fparith(as, ir, XO_MULSD); break;
  case IR_DIV: asm_fparith(as, ir, XO_DIVSD); break;

  case IR_NEG: asm_fparith(as, ir, XO_XORPS); break;
  case IR_ABS: asm_fparith(as, ir, XO_ANDPS); break;

  case IR_MIN: asm_fparith(as, ir, XO_MINSD); break;
  case IR_MAX: asm_fparith(as, ir, XO_MAXSD); break;

  case IR_FPMATH: case IR_ATAN2: case IR_LDEXP: case IR_POWI:
    asm_fpmath(as, ir);
    break;

  /* Overflow-checking arithmetic ops. Note: don't use LEA here! */
  case IR_ADDOV: asm_intarith(as, ir, XOg_ADD); break;
  case IR_SUBOV: asm_intarith(as, ir, XOg_SUB); break;

  /* Memory references. */
  case IR_AREF: asm_aref(as, ir); break;
  case IR_HREF: asm_href(as, ir); break;
  case IR_HREFK: asm_hrefk(as, ir); break;
  case IR_NEWREF: asm_newref(as, ir); break;
  case IR_UREFO: case IR_UREFC: asm_uref(as, ir); break;
  case IR_FREF: asm_fref(as, ir); break;
  case IR_STRREF: asm_strref(as, ir); break;

  /* Loads and stores. */
  case IR_ALOAD: case IR_HLOAD: case IR_ULOAD: asm_ahuload(as, ir); break;
  case IR_FLOAD: asm_fload(as, ir); break;
  case IR_SLOAD: asm_sload(as, ir); break;
  case IR_XLOAD: asm_xload(as, ir); break;

  case IR_ASTORE: case IR_HSTORE: case IR_USTORE: asm_ahustore(as, ir); break;
  case IR_FSTORE: asm_fstore(as, ir); break;

  /* String ops. */
  case IR_SNEW: asm_snew(as, ir); break;

  /* Table ops. */
  case IR_TNEW: asm_tnew(as, ir); break;
  case IR_TDUP: asm_tdup(as, ir); break;
  case IR_TLEN: asm_tlen(as, ir); break;
  case IR_TBAR: asm_tbar(as, ir); break;
  case IR_OBAR: asm_obar(as, ir); break;

  /* Type conversions. */
  case IR_TONUM: asm_tonum(as, ir); break;
  case IR_TOINT:
    if (irt_isguard(ir->t))
      asm_tointg(as, ir, ra_alloc1(as, ir->op1, RSET_FPR));
    else
      asm_toint(as, ir); break;
    break;
  case IR_TOBIT: asm_tobit(as, ir); break;
  case IR_TOSTR: asm_tostr(as, ir); break;
  case IR_STRTO: asm_strto(as, ir); break;

  default:
    setintV(&as->J->errinfo, ir->o);
    lj_trace_err_info(as->J, LJ_TRERR_NYIIR);
    break;
  }
}

/* Assemble a trace in linear backwards order. */
static void asm_trace(ASMState *as)
{
  for (as->curins--; as->curins > as->stopins; as->curins--) {
    IRIns *ir = IR(as->curins);
    if (irt_isguard(ir->t))
      asm_snap_prep(as);
    else if (!ra_used(ir) && !irm_sideeff(lj_ir_mode[ir->o]) &&
	     (as->flags & JIT_F_OPT_DCE))
      continue;  /* Dead-code elimination can be soooo easy. */
    RA_DBG_REF();
    checkmclim(as);
    asm_ir(as, ir);
  }
}

/* -- Trace setup --------------------------------------------------------- */

/* Clear reg/sp for all instructions and add register hints. */
static void asm_setup_regsp(ASMState *as, Trace *T)
{
  IRRef i, nins;
  int inloop;

  /* Clear reg/sp for constants. */
  for (i = T->nk; i < REF_BIAS; i++)
    IR(i)->prev = REGSP_INIT;

  /* REF_BASE is used for implicit references to the BASE register. */
  IR(REF_BASE)->prev = REGSP_HINT(RID_BASE);

  nins = T->nins;
  if (IR(nins-1)->o == IR_RENAME) {
    do { nins--; } while (IR(nins-1)->o == IR_RENAME);
    T->nins = nins;  /* Remove any renames left over from ASM restart. */
  }
  as->snaprename = nins;
  as->snapref = nins;
  as->snapno = T->nsnap;

  as->stopins = REF_BASE;
  as->orignins = nins;
  as->curins = nins;

  inloop = 0;
  for (i = REF_FIRST; i < nins; i++) {
    IRIns *ir = IR(i);
    switch (ir->o) {
    case IR_LOOP:
      inloop = 1;
      break;
    /* Set hints for slot loads from a parent trace. */
    case IR_SLOAD:
      if ((ir->op2 & IRSLOAD_PARENT)) {
	RegSP rs = as->parentmap[ir->op1];
	lua_assert(regsp_used(rs));
	as->stopins = i;
	if (!ra_hasspill(regsp_spill(rs)) && ra_hasreg(regsp_reg(rs))) {
	  ir->prev = (uint16_t)REGSP_HINT(regsp_reg(rs));
	  continue;
	}
      }
      break;
    case IR_FRAME:
      if (i == as->stopins+1 && ir->op1 == ir->op2)
	as->stopins++;
      break;
    /* C calls evict all scratch regs and return results in RID_RET. */
    case IR_SNEW: case IR_TNEW: case IR_TDUP: case IR_TLEN: case IR_TOSTR:
    case IR_NEWREF:
      ir->prev = REGSP_HINT(RID_RET);
      if (inloop)
	as->modset = RSET_SCRATCH;
      continue;
    case IR_STRTO: case IR_OBAR:
      if (inloop)
	as->modset = RSET_SCRATCH;
      break;
    /* Ordered string compares evict all integer scratch registers. */
    case IR_LT: case IR_GE: case IR_LE: case IR_GT:
      if (irt_isstr(ir->t) && inloop)
	as->modset |= (RSET_SCRATCH & RSET_GPR);
      break;
    /* Non-constant shift counts need to be in RID_ECX. */
    case IR_BSHL: case IR_BSHR: case IR_BSAR: case IR_BROL: case IR_BROR:
      if (!irref_isk(ir->op2) && !ra_hashint(IR(ir->op2)->r))
	IR(ir->op2)->r = REGSP_HINT(RID_ECX);
      break;
    /* Do not propagate hints across type conversions. */
    case IR_TONUM: case IR_TOINT: case IR_TOBIT:
      break;
    default:
      /* Propagate hints across likely 'op reg, imm' or 'op reg'. */
      if (irref_isk(ir->op2) && !irref_isk(ir->op1)) {
	ir->prev = IR(ir->op1)->prev;
	continue;
      }
      break;
    }
    ir->prev = REGSP_INIT;
  }
}

/* -- Assembler core ------------------------------------------------------ */

/* Define this if you want to run LuaJIT with Valgrind. */
#ifdef LUAJIT_USE_VALGRIND
#include <valgrind/valgrind.h>
#define VG_INVALIDATE(p, sz)	VALGRIND_DISCARD_TRANSLATIONS(p, sz)
#else
#define VG_INVALIDATE(p, sz)	((void)0)
#endif

/* Assemble a trace. */
void lj_asm_trace(jit_State *J, Trace *T)
{
  ASMState as_;
  ASMState *as = &as_;

  /* Setup initial state. Copy some fields to reduce indirections. */
  as->J = J;
  as->T = T;
  as->ir = T->ir;
  as->flags = J->flags;
  as->loopref = J->loopref;
  as->realign = NULL;
  as->loopinv = 0;
  if (J->parent) {
    as->parent = J->trace[J->parent];
    lj_snap_regspmap(as->parentmap, as->parent, J->exitno);
  } else {
    as->parent = NULL;
  }
  as->mctop = lj_mcode_reserve(J, &as->mcbot);  /* Reserve MCode memory. */
  as->mcp = as->mctop;
  as->mclim = as->mcbot + MCLIM_REDZONE;
  asm_exitstub_setup(as, T->nsnap);

  do {
    as->mcp = as->mctop;
    as->curins = T->nins;
    RA_DBG_START();
    RA_DBGX((as, "===== STOP ====="));
    /* Realign and leave room for backwards loop branch or exit branch. */
    if (as->realign) {
      int i = ((int)(intptr_t)as->realign) & 15;
      MCode *p = as->mctop;
      /* Fill unused mcode tail with NOPs to make the prefetcher happy. */
      while (i-- > 0)
	*--p = XI_NOP;
      as->mctop = p;
      as->mcp = p - (as->loopinv ? 5 : 2);  /* Space for short/near jmp. */
    } else {
      as->mcp = as->mctop - 5;  /* Space for exit branch (near jmp). */
    }
    as->invmcp = as->mcp;
    as->mcloop = NULL;
    as->testmcp = NULL;
    as->topslot = 0;
    as->gcsteps = 0;
    as->sectref = as->loopref;
    as->fuseref = (as->flags & JIT_F_OPT_FUSE) ? as->loopref : FUSE_DISABLED;

    /* Setup register allocation. */
    ra_setup(as);
    asm_setup_regsp(as, T);

    if (!as->loopref) {
      /* Leave room for ESP adjustment: add esp, imm or lea esp, [esp+imm] */
      as->mcp -= (as->flags & JIT_F_LEA_AGU) ? 7 : 6;
      as->invmcp = NULL;
      asm_tail_sync(as);
    }
    asm_trace(as);
  } while (as->realign);  /* Retry in case the MCode needs to be realigned. */

  RA_DBG_REF();
  checkmclim(as);
  if (as->gcsteps)
    asm_gc_check(as, &as->T->snap[0]);
  if (!J->parent)
    asm_head_base(as);
  asm_const_remat(as);
  if (J->parent)
    asm_head_side(as);
  else
    asm_head_root(as);
  asm_phi_fixup(as);

  RA_DBGX((as, "===== START ===="));
  RA_DBG_FLUSH();
  if (as->freeset != RSET_ALL)
    lj_trace_err(as->J, LJ_TRERR_BADRA);  /* Ouch! Should never happen. */

  /* Set trace entry point before fixing up tail to allow link to self. */
  T->mcode = as->mcp;
  T->mcloop = as->mcloop ? (MSize)(as->mcloop - as->mcp) : 0;
  if (!as->loopref)
    asm_tail_fixup(as, T->link);  /* Note: this may change as->mctop! */
  T->szmcode = (MSize)(as->mctop - as->mcp);
  VG_INVALIDATE(T->mcode, T->szmcode);
}

/* Patch exit jumps of existing machine code to a new target. */
void lj_asm_patchexit(jit_State *J, Trace *T, ExitNo exitno, MCode *target)
{
  MCode *p = T->mcode;
  MCode *mcarea = lj_mcode_patch(J, p, 0);
  MSize len = T->szmcode;
  MCode *px = exitstub_addr(J, exitno) - 6;
  MCode *pe = p+len-6;
  if (len > 5 && p[len-5] == XI_JMP && p+len-6 + *(int32_t *)(p+len-4) == px)
    *(int32_t *)(p+len-4) = (int32_t)(target - (p+len));
  for (; p < pe; p++) {
    if ((*(uint16_t *)p & 0xf0ff) == 0x800f && p + *(int32_t *)(p+2) == px) {
      *(int32_t *)(p+2) = (int32_t)(target - (p+6));
      p += 5;
    }
  }
  lj_mcode_patch(J, mcarea, 1);
  VG_INVALIDATE(T->mcode, T->szmcode);
}

#undef IR

#endif
