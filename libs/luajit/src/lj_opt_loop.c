/*
** LOOP: Loop Optimizations.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_opt_loop_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_vm.h"

/* Loop optimization:
**
** Traditional Loop-Invariant Code Motion (LICM) splits the instructions
** of a loop into invariant and variant instructions. The invariant
** instructions are hoisted out of the loop and only the variant
** instructions remain inside the loop body.
**
** Unfortunately LICM is mostly useless for compiling dynamic languages.
** The IR has many guards and most of the subsequent instructions are
** control-dependent on them. The first non-hoistable guard would
** effectively prevent hoisting of all subsequent instructions.
**
** That's why we use a special form of unrolling using copy-substitution,
** combined with redundancy elimination:
**
** The recorded instruction stream is re-emitted to the compiler pipeline
** with substituted operands. The substitution table is filled with the
** refs returned by re-emitting each instruction. This can be done
** on-the-fly, because the IR is in strict SSA form, where every ref is
** defined before its use.
**
** This aproach generates two code sections, separated by the LOOP
** instruction:
**
** 1. The recorded instructions form a kind of pre-roll for the loop. It
** contains a mix of invariant and variant instructions and performs
** exactly one loop iteration (but not necessarily the 1st iteration).
**
** 2. The loop body contains only the variant instructions and performs
** all remaining loop iterations.
**
** On first sight that looks like a waste of space, because the variant
** instructions are present twice. But the key insight is that the
** pre-roll honors the control-dependencies for *both* the pre-roll itself
** *and* the loop body!
**
** It also means one doesn't have to explicitly model control-dependencies
** (which, BTW, wouldn't help LICM much). And it's much easier to
** integrate sparse snapshotting with this approach.
**
** One of the nicest aspects of this approach is that all of the
** optimizations of the compiler pipeline (FOLD, CSE, FWD, etc.) can be
** reused with only minor restrictions (e.g. one should not fold
** instructions across loop-carried dependencies).
**
** But in general all optimizations can be applied which only need to look
** backwards into the generated instruction stream. At any point in time
** during the copy-substitution process this contains both a static loop
** iteration (the pre-roll) and a dynamic one (from the to-be-copied
** instruction up to the end of the partial loop body).
**
** Since control-dependencies are implicitly kept, CSE also applies to all
** kinds of guards. The major advantage is that all invariant guards can
** be hoisted, too.
**
** Load/store forwarding works across loop iterations, too. This is
** important if loop-carried dependencies are kept in upvalues or tables.
** E.g. 'self.idx = self.idx + 1' deep down in some OO-style method may
** become a forwarded loop-recurrence after inlining.
**
** Since the IR is in SSA form, loop-carried dependencies have to be
** modeled with PHI instructions. The potential candidates for PHIs are
** collected on-the-fly during copy-substitution. After eliminating the
** redundant ones, PHI instructions are emitted *below* the loop body.
**
** Note that this departure from traditional SSA form doesn't change the
** semantics of the PHI instructions themselves. But it greatly simplifies
** on-the-fly generation of the IR and the machine code.
*/

/* Some local macros to save typing. Undef'd at the end. */
#define IR(ref)		(&J->cur.ir[(ref)])

/* Pass IR on to next optimization in chain (FOLD). */
#define emitir(ot, a, b)	(lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

/* Emit raw IR without passing through optimizations. */
#define emitir_raw(ot, a, b)	(lj_ir_set(J, (ot), (a), (b)), lj_ir_emit(J))

/* -- PHI elimination ----------------------------------------------------- */

/* Emit or eliminate collected PHIs. */
static void loop_emit_phi(jit_State *J, IRRef1 *subst, IRRef1 *phi, IRRef nphi)
{
  int pass2 = 0;
  IRRef i, nslots;
  IRRef invar = J->chain[IR_LOOP];
  /* Pass #1: mark redundant and potentially redundant PHIs. */
  for (i = 0; i < nphi; i++) {
    IRRef lref = phi[i];
    IRRef rref = subst[lref];
    if (lref == rref || rref == REF_DROP) {  /* Invariants are redundant. */
      irt_setmark(IR(lref)->t);
    } else if (!(IR(rref)->op1 == lref || IR(rref)->op2 == lref)) {
      /* Quick check for simple recurrences failed, need pass2. */
      irt_setmark(IR(lref)->t);
      pass2 = 1;
    }
  }
  /* Pass #2: traverse variant part and clear marks of non-redundant PHIs. */
  if (pass2) {
    for (i = J->cur.nins-1; i > invar; i--) {
      IRIns *ir = IR(i);
      if (!irref_isk(ir->op1)) irt_clearmark(IR(ir->op1)->t);
      if (!irref_isk(ir->op2)) irt_clearmark(IR(ir->op2)->t);
    }
  }
  /* Pass #3: add PHIs for variant slots without a corresponding SLOAD. */
  nslots = J->baseslot+J->maxslot;
  for (i = 1; i < nslots; i++) {
    IRRef ref = tref_ref(J->slot[i]);
    if (!irref_isk(ref) && ref != subst[ref]) {
      IRIns *ir = IR(ref);
      irt_clearmark(ir->t);  /* Unmark potential uses, too. */
      if (!irt_isphi(ir->t) && !irt_ispri(ir->t)) {
	irt_setphi(ir->t);
	if (nphi >= LJ_MAX_PHI)
	  lj_trace_err(J, LJ_TRERR_PHIOV);
	phi[nphi++] = (IRRef1)ref;
      }
    }
  }
  /* Pass #4: emit PHI instructions or eliminate PHIs. */
  for (i = 0; i < nphi; i++) {
    IRRef lref = phi[i];
    IRIns *ir = IR(lref);
    if (!irt_ismarked(ir->t)) {  /* Emit PHI if not marked. */
      IRRef rref = subst[lref];
      if (rref > invar)
	irt_setphi(IR(rref)->t);
      emitir_raw(IRT(IR_PHI, irt_type(ir->t)), lref, rref);
    } else {  /* Otherwise eliminate PHI. */
      irt_clearmark(ir->t);
      irt_clearphi(ir->t);
    }
  }
}

/* -- Loop unrolling using copy-substitution ------------------------------ */

/* Unroll loop. */
static void loop_unroll(jit_State *J)
{
  IRRef1 phi[LJ_MAX_PHI];
  uint32_t nphi = 0;
  IRRef1 *subst;
  SnapShot *osnap, *snap;
  IRRef2 *loopmap;
  BCReg loopslots;
  MSize nsnap, nsnapmap;
  IRRef ins, invar, osnapref;

  /* Use temp buffer for substitution table.
  ** Only non-constant refs in [REF_BIAS,invar) are valid indexes.
  ** Note: don't call into the VM or run the GC or the buffer may be gone.
  */
  invar = J->cur.nins;
  subst = (IRRef1 *)lj_str_needbuf(J->L, &G(J->L)->tmpbuf,
				   (invar-REF_BIAS)*sizeof(IRRef1)) - REF_BIAS;
  subst[REF_BASE] = REF_BASE;

  /* LOOP separates the pre-roll from the loop body. */
  emitir_raw(IRTG(IR_LOOP, IRT_NIL), 0, 0);

  /* Ensure size for copy-substituted snapshots (minus #0 and loop snapshot). */
  nsnap = J->cur.nsnap;
  if (LJ_UNLIKELY(2*nsnap-2 > J->sizesnap)) {
    MSize maxsnap = (MSize)J->param[JIT_P_maxsnap];
    if (2*nsnap-2 > maxsnap)
      lj_trace_err(J, LJ_TRERR_SNAPOV);
    lj_mem_growvec(J->L, J->snapbuf, J->sizesnap, maxsnap, SnapShot);
    J->cur.snap = J->snapbuf;
  }
  nsnapmap = J->cur.nsnapmap;  /* Use temp. copy to avoid undo. */
  if (LJ_UNLIKELY(nsnapmap*2 > J->sizesnapmap)) {
    J->snapmapbuf = (IRRef2 *)lj_mem_realloc(J->L, J->snapmapbuf,
					     J->sizesnapmap*sizeof(IRRef2),
					     2*J->sizesnapmap*sizeof(IRRef2));
    J->cur.snapmap = J->snapmapbuf;
    J->sizesnapmap *= 2;
  }

  /* The loop snapshot is used for fallback substitutions. */
  snap = &J->cur.snap[nsnap-1];
  loopmap = &J->cur.snapmap[snap->mapofs];
  loopslots = snap->nslots;
  /* The PC of snapshot #0 and the loop snapshot must match. */
  lua_assert(loopmap[loopslots] == J->cur.snapmap[J->cur.snap[0].nslots]);

  /* Start substitution with snapshot #1 (#0 is empty for root traces). */
  osnap = &J->cur.snap[1];
  osnapref = osnap->ref;

  /* Copy and substitute all recorded instructions and snapshots. */
  for (ins = REF_FIRST; ins < invar; ins++) {
    IRIns *ir;
    IRRef op1, op2;

    /* Copy-substitute snapshot. */
    if (ins >= osnapref) {
      IRRef2 *nmap, *omap = &J->cur.snapmap[osnap->mapofs];
      BCReg s, nslots;
      uint32_t nmapofs, nframelinks;
      if (irt_isguard(J->guardemit)) {  /* Guard inbetween? */
	nmapofs = nsnapmap;
	snap++;  /* Add new snapshot. */
      } else {
	nmapofs = snap->mapofs;  /* Overwrite previous snapshot. */
      }
      J->guardemit.irt = 0;
      nslots = osnap->nslots;
      nframelinks = osnap->nframelinks;
      snap->mapofs = (uint16_t)nmapofs;
      snap->ref = (IRRef1)J->cur.nins;
      snap->nslots = (uint8_t)nslots;
      snap->nframelinks = (uint8_t)nframelinks;
      snap->count = 0;
      osnap++;
      osnapref = osnap->ref;
      nsnapmap = nmapofs + nslots + nframelinks;
      nmap = &J->cur.snapmap[nmapofs];
      /* Substitute snapshot slots. */
      for (s = 0; s < nslots; s++) {
	IRRef ref = snap_ref(omap[s]);
	if (ref) {
	  if (!irref_isk(ref))
	    ref = subst[ref];
	} else if (s < loopslots) {
	  ref = loopmap[s];
	}
	nmap[s] = ref;
      }
      /* Copy frame links. */
      nmap += nslots;
      omap += nslots;
      for (s = 0; s < nframelinks; s++)
	nmap[s] = omap[s];
    }

    /* Substitute instruction operands. */
    ir = IR(ins);
    op1 = ir->op1;
    if (!irref_isk(op1)) op1 = subst[op1];
    op2 = ir->op2;
    if (!irref_isk(op2)) op2 = subst[op2];
    if (irm_kind(lj_ir_mode[ir->o]) == IRM_N &&
	op1 == ir->op1 && op2 == ir->op2) {  /* Regular invariant ins? */
      subst[ins] = (IRRef1)ins;  /* Shortcut. */
    } else {
      /* Re-emit substituted instruction to the FOLD/CSE/etc. pipeline. */
      IRType1 t = ir->t;  /* Get this first, since emitir may invalidate ir. */
      IRRef ref = tref_ref(emitir(ir->ot & ~IRT_ISPHI, op1, op2));
      subst[ins] = (IRRef1)ref;
      if (ref != ins && ref < invar) {  /* Loop-carried dependency? */
	IRIns *irr = IR(ref);
	/* Potential PHI? */
	if (!irref_isk(ref) && !irt_isphi(irr->t) && !irt_ispri(irr->t)) {
	  irt_setphi(irr->t);
	  if (nphi >= LJ_MAX_PHI)
	    lj_trace_err(J, LJ_TRERR_PHIOV);
	  phi[nphi++] = (IRRef1)ref;
	}
	/* Check all loop-carried dependencies for type instability. */
	if (!irt_sametype(t, irr->t)) {
	  if (irt_isnum(t) && irt_isinteger(irr->t))  /* Fix int->num case. */
	    subst[ins] = tref_ref(emitir(IRTN(IR_TONUM), ref, 0));
	  else if (!(irt_isinteger(t) && irt_isinteger(irr->t)))
	    lj_trace_err(J, LJ_TRERR_TYPEINS);
	}
      }
    }
  }
  if (irt_isguard(J->guardemit)) {  /* Guard inbetween? */
    J->cur.nsnapmap = (uint16_t)nsnapmap;
    snap++;
  } else {
    J->cur.nsnapmap = (uint16_t)snap->mapofs;  /* Last snapshot is redundant. */
  }
  J->cur.nsnap = (uint16_t)(snap - J->cur.snap);
  lua_assert(J->cur.nsnapmap <= J->sizesnapmap);

  loop_emit_phi(J, subst, phi, nphi);
}

/* Undo any partial changes made by the loop optimization. */
static void loop_undo(jit_State *J, IRRef ins)
{
  lj_ir_rollback(J, ins);
  for (ins--; ins >= REF_FIRST; ins--) {  /* Remove flags. */
    IRIns *ir = IR(ins);
    irt_clearphi(ir->t);
    irt_clearmark(ir->t);
  }
}

/* Protected callback for loop optimization. */
static TValue *cploop_opt(lua_State *L, lua_CFunction dummy, void *ud)
{
  UNUSED(L); UNUSED(dummy);
  loop_unroll((jit_State *)ud);
  return NULL;
}

/* Loop optimization. */
int lj_opt_loop(jit_State *J)
{
  IRRef nins = J->cur.nins;
  int errcode = lj_vm_cpcall(J->L, cploop_opt, NULL, J);
  if (LJ_UNLIKELY(errcode)) {
    lua_State *L = J->L;
    if (errcode == LUA_ERRRUN && tvisnum(L->top-1)) {  /* Trace error? */
      int32_t e = lj_num2int(numV(L->top-1));
      switch ((TraceError)e) {
      case LJ_TRERR_TYPEINS:  /* Type instability. */
      case LJ_TRERR_GFAIL:  /* Guard would always fail. */
	/* Unrolling via recording fixes many cases, e.g. a flipped boolean. */
	if (--J->instunroll < 0)  /* But do not unroll forever. */
	  break;
	L->top--;  /* Remove error object. */
	J->guardemit.irt = 0;
	loop_undo(J, nins);
	return 1;  /* Loop optimization failed, continue recording. */
      default:
	break;
      }
    }
    lj_err_throw(L, errcode);  /* Propagate all other errors. */
  }
  return 0;  /* Loop optimization is ok. */
}

#undef IR
#undef emitir
#undef emitir_raw

#endif
