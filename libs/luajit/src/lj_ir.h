/*
** SSA IR (Intermediate Representation) format.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_IR_H
#define _LJ_IR_H

#include "lj_obj.h"

/* IR instruction definition. Order matters, see below. */
#define IRDEF(_) \
  /* Miscellaneous ops. */ \
  _(NOP,	N , ___, ___) \
  _(BASE,	N , lit, lit) \
  _(LOOP,	G , ___, ___) \
  _(PHI,	S , ref, ref) \
  _(RENAME,	S , ref, lit) \
  \
  /* Constants. */ \
  _(KPRI,	N , ___, ___) \
  _(KINT,	N , cst, ___) \
  _(KGC,	N , cst, ___) \
  _(KPTR,	N , cst, ___) \
  _(KNULL,	N , cst, ___) \
  _(KNUM,	N , cst, ___) \
  _(KSLOT,	N , ref, lit) \
  \
  /* Guarded assertions. */ \
  /* Must be properly aligned to flip opposites (^1) and (un)ordered (^4). */ \
  _(EQ,		GC, ref, ref) \
  _(NE,		GC, ref, ref) \
  \
  _(ABC,	G , ref, ref) \
  _(FRAME,	G , ref, ref) \
  \
  _(LT,		G , ref, ref) \
  _(GE,		G , ref, ref) \
  _(LE,		G , ref, ref) \
  _(GT,		G , ref, ref) \
  \
  _(ULT,	G , ref, ref) \
  _(UGE,	G , ref, ref) \
  _(ULE,	G , ref, ref) \
  _(UGT,	G , ref, ref) \
  \
  /* Bit ops. */ \
  _(BNOT,	N , ref, ___) \
  _(BSWAP,	N , ref, ___) \
  _(BAND,	C , ref, ref) \
  _(BOR,	C , ref, ref) \
  _(BXOR,	C , ref, ref) \
  _(BSHL,	N , ref, ref) \
  _(BSHR,	N , ref, ref) \
  _(BSAR,	N , ref, ref) \
  _(BROL,	N , ref, ref) \
  _(BROR,	N , ref, ref) \
  \
  /* Arithmetic ops. ORDER ARITH (FPMATH/POWI take the space for MOD/POW). */ \
  _(ADD,	C , ref, ref) \
  _(SUB,	N , ref, ref) \
  _(MUL,	C , ref, ref) \
  _(DIV,	N , ref, ref) \
  \
  _(FPMATH,	N , ref, lit) \
  _(POWI,	N , ref, ref) \
  \
  _(NEG,	N , ref, ref) \
  _(ABS,	N , ref, ref) \
  _(ATAN2,	N , ref, ref) \
  _(LDEXP,	N , ref, ref) \
  _(MIN,	C , ref, ref) \
  _(MAX,	C , ref, ref) \
  \
  /* Overflow-checking arithmetic ops. */ \
  _(ADDOV,	GC, ref, ref) \
  _(SUBOV,	G , ref, ref) \
  \
  /* Memory ops. A = array, H = hash, U = upvalue, F = field, S = stack. */ \
  \
  /* Memory references. */ \
  _(AREF,	R , ref, ref) \
  _(HREFK,	RG, ref, ref) \
  _(HREF,	L , ref, ref) \
  _(NEWREF,	S , ref, ref) \
  _(UREFO,	LG, ref, lit) \
  _(UREFC,	LG, ref, lit) \
  _(FREF,	R , ref, lit) \
  _(STRREF,	N , ref, ref) \
  \
  /* Loads and Stores. These must be in the same order. */ \
  _(ALOAD,	LG, ref, ___) \
  _(HLOAD,	LG, ref, ___) \
  _(ULOAD,	LG, ref, ___) \
  _(FLOAD,	L , ref, lit) \
  _(SLOAD,	LG, lit, lit) \
  _(XLOAD,	L , ref, lit) \
  \
  _(ASTORE,	S , ref, ref) \
  _(HSTORE,	S , ref, ref) \
  _(USTORE,	S , ref, ref) \
  _(FSTORE,	S , ref, ref) \
  \
  /* String ops. */ \
  _(SNEW,	N , ref, ref) \
  \
  /* Table ops. */ \
  _(TNEW,	A , lit, lit) \
  _(TDUP,	A , ref, ___) \
  _(TLEN,	L , ref, ___) \
  _(TBAR,	S , ref, ___) \
  _(OBAR,	S , ref, ref) \
  \
  /* Type conversions. */ \
  _(TONUM,	N , ref, ___) \
  _(TOINT,	N , ref, lit) \
  _(TOBIT,	N , ref, ref) \
  _(TOSTR,	N , ref, ___) \
  _(STRTO,	G , ref, ___) \
  \
  /* End of list. */

/* IR opcodes (max. 256). */
typedef enum {
#define IRENUM(name, m, m1, m2)	IR_##name,
IRDEF(IRENUM)
#undef IRENUM
  IR__MAX
} IROp;

/* Stored opcode. */
typedef uint8_t IROp1;

LJ_STATIC_ASSERT(((int)IR_EQ^1) == (int)IR_NE);
LJ_STATIC_ASSERT(((int)IR_LT^1) == (int)IR_GE);
LJ_STATIC_ASSERT(((int)IR_LE^1) == (int)IR_GT);
LJ_STATIC_ASSERT(((int)IR_LT^3) == (int)IR_GT);
LJ_STATIC_ASSERT(((int)IR_LT^4) == (int)IR_ULT);

/* Delta between xLOAD and xSTORE. */
#define IRDELTA_L2S		((int)IR_ASTORE - (int)IR_ALOAD)

LJ_STATIC_ASSERT((int)IR_HLOAD + IRDELTA_L2S == (int)IR_HSTORE);
LJ_STATIC_ASSERT((int)IR_ULOAD + IRDELTA_L2S == (int)IR_USTORE);
LJ_STATIC_ASSERT((int)IR_FLOAD + IRDELTA_L2S == (int)IR_FSTORE);

/* FPMATH sub-functions. ORDER FPM. */
#define IRFPMDEF(_) \
  _(FLOOR) _(CEIL) _(TRUNC)  /* Must be first and in this order. */ \
  _(SQRT) _(EXP) _(EXP2) _(LOG) _(LOG2) _(LOG10) \
  _(SIN) _(COS) _(TAN) \
  _(OTHER)

typedef enum {
#define FPMENUM(name)		IRFPM_##name,
IRFPMDEF(FPMENUM)
#undef FPMENUM
  IRFPM__MAX
} IRFPMathOp;

/* FLOAD field IDs. */
#define IRFLDEF(_) \
  _(STR_LEN,	GCstr, len) \
  _(FUNC_ENV,	GCfunc, l.env) \
  _(TAB_META,	GCtab, metatable) \
  _(TAB_ARRAY,	GCtab, array) \
  _(TAB_NODE,	GCtab, node) \
  _(TAB_ASIZE,	GCtab, asize) \
  _(TAB_HMASK,	GCtab, hmask) \
  _(TAB_NOMM,	GCtab, nomm) \
  _(UDATA_META,	GCudata, metatable)

typedef enum {
#define FLENUM(name, type, field)	IRFL_##name,
IRFLDEF(FLENUM)
#undef FLENUM
  IRFL__MAX
} IRFieldID;

/* SLOAD mode bits, stored in op2. */
#define IRSLOAD_INHERIT		1	/* Inherited by exits/side traces. */
#define IRSLOAD_READONLY	2	/* Read-only, omit slot store. */
#define IRSLOAD_PARENT		4	/* Coalesce with parent trace. */

/* XLOAD mode, stored in op2. */
#define IRXLOAD_UNALIGNED	1

/* TOINT mode, stored in op2. Ordered by strength of the checks. */
#define IRTOINT_CHECK		0	/* Number checked for integerness. */
#define IRTOINT_INDEX		1	/* Checked + special backprop rules. */
#define IRTOINT_ANY		2	/* Any FP number is ok. */
#define IRTOINT_TOBIT		3	/* Cache only: TOBIT conversion. */

/* IR operand mode (2 bit). */
typedef enum {
  IRMref,		/* IR reference. */
  IRMlit,		/* 16 bit unsigned literal. */
  IRMcst,		/* Constant literal: i, gcr or ptr. */
  IRMnone		/* Unused operand. */
} IRMode;
#define IRM___		IRMnone

/* Mode bits: Commutative, {Normal/Ref, Alloc, Load, Store}, Guard. */
#define IRM_C			0x10

#define IRM_N			0x00
#define IRM_R			IRM_N
#define IRM_A			0x20
#define IRM_L			0x40
#define IRM_S			0x60

#define IRM_G			0x80

#define IRM_GC			(IRM_G|IRM_C)
#define IRM_RG			(IRM_R|IRM_G)
#define IRM_LG			(IRM_L|IRM_G)

#define irm_op1(m)		(cast(IRMode, (m)&3))
#define irm_op2(m)		(cast(IRMode, ((m)>>2)&3))
#define irm_iscomm(m)		((m) & IRM_C)
#define irm_kind(m)		((m) & IRM_S)
#define irm_isguard(m)		((m) & IRM_G)
/* Stores or any other op with a guard has a side-effect. */
#define irm_sideeff(m)		((m) >= IRM_S)

#define IRMODE(name, m, m1, m2)	((IRM##m1)|((IRM##m2)<<2)|(IRM_##m)),

LJ_DATA const uint8_t lj_ir_mode[IR__MAX+1];

/* IR result type and flags (8 bit). */
typedef enum {
  /* Map of itypes to non-negative numbers. ORDER LJ_T */
  IRT_NIL,
  IRT_FALSE,
  IRT_TRUE,
  IRT_LIGHTUD,
  /* GCobj types are from here ... */
  IRT_STR,
  IRT_PTR,		/* IRT_PTR never escapes the IR (map of LJ_TUPVAL). */
  IRT_THREAD,
  IRT_PROTO,
  IRT_FUNC,
  IRT_9,		/* LJ_TDEADKEY is never used in the IR. */
  IRT_TAB,
  IRT_UDATA,
  /* ... until here. */
  IRT_NUM,
  /* The various integers are only used in the IR and can only escape to
  ** a TValue after implicit or explicit conversion (TONUM). Their types
  ** must be contiguous and next to IRT_NUM (see the typerange macros below).
  */
  IRT_INT,
  IRT_I8,
  IRT_U8,
  IRT_I16,
  IRT_U16,
  /* There is room for 14 more types. */

  /* Additional flags. */
  IRT_MARK = 0x20,	/* Marker for misc. purposes. */
  IRT_GUARD = 0x40,	/* Instruction is a guard. */
  IRT_ISPHI = 0x80,	/* Instruction is left or right PHI operand. */

  /* Masks. */
  IRT_TYPE = 0x1f,
  IRT_T = 0xff
} IRType;

#define irtype_ispri(irt)	((uint32_t)(irt) <= IRT_TRUE)

/* Stored IRType. */
typedef struct IRType1 { uint8_t irt; } IRType1;

#define IRT(o, t)		((uint32_t)(((o)<<8) | (t)))
#define IRTI(o)			(IRT((o), IRT_INT))
#define IRTN(o)			(IRT((o), IRT_NUM))
#define IRTG(o, t)		(IRT((o), IRT_GUARD|(t)))
#define IRTGI(o)		(IRT((o), IRT_GUARD|IRT_INT))

#define irt_t(t)		(cast(IRType, (t).irt))
#define irt_type(t)		(cast(IRType, (t).irt & IRT_TYPE))
#define irt_sametype(t1, t2)	((((t1).irt ^ (t2).irt) & IRT_TYPE) == 0)
#define irt_typerange(t, first, last) \
  ((uint32_t)((t).irt & IRT_TYPE) - (uint32_t)(first) <= (uint32_t)(last-first))

#define irt_isnil(t)		(irt_type(t) == IRT_NIL)
#define irt_ispri(t)		((uint32_t)irt_type(t) <= IRT_TRUE)
#define irt_isstr(t)		(irt_type(t) == IRT_STR)
#define irt_isfunc(t)		(irt_type(t) == IRT_FUNC)
#define irt_istab(t)		(irt_type(t) == IRT_TAB)
#define irt_isnum(t)		(irt_type(t) == IRT_NUM)
#define irt_isint(t)		(irt_type(t) == IRT_INT)
#define irt_isi8(t)		(irt_type(t) == IRT_I8)
#define irt_isu8(t)		(irt_type(t) == IRT_U8)
#define irt_isi16(t)		(irt_type(t) == IRT_I16)
#define irt_isu16(t)		(irt_type(t) == IRT_U16)

#define irt_isinteger(t)	(irt_typerange((t), IRT_INT, IRT_U16))
#define irt_isgcv(t)		(irt_typerange((t), IRT_STR, IRT_UDATA))
#define irt_isaddr(t)		(irt_typerange((t), IRT_LIGHTUD, IRT_UDATA))

#define itype2irt(tv) \
  (~uitype(tv) < IRT_NUM ? cast(IRType, ~uitype(tv)) : IRT_NUM)
#define irt_toitype(t)		((int32_t)~(uint32_t)irt_type(t))

#define irt_isguard(t)		((t).irt & IRT_GUARD)
#define irt_ismarked(t)		((t).irt & IRT_MARK)
#define irt_setmark(t)		((t).irt |= IRT_MARK)
#define irt_clearmark(t)	((t).irt &= ~IRT_MARK)
#define irt_isphi(t)		((t).irt & IRT_ISPHI)
#define irt_setphi(t)		((t).irt |= IRT_ISPHI)
#define irt_clearphi(t)		((t).irt &= ~IRT_ISPHI)

/* Stored combined IR opcode and type. */
typedef uint16_t IROpT;

/* IR references. */
typedef uint16_t IRRef1;	/* One stored reference. */
typedef uint32_t IRRef2;	/* Two stored references. */
typedef uint32_t IRRef;		/* Used to pass around references. */

/* Fixed references. */
enum {
  REF_BIAS =	0x8000,
  REF_TRUE =	REF_BIAS-3,
  REF_FALSE =	REF_BIAS-2,
  REF_NIL =	REF_BIAS-1,	/* \--- Constants grow downwards. */
  REF_BASE =	REF_BIAS,	/* /--- IR grows upwards. */
  REF_FIRST =	REF_BIAS+1,
  REF_DROP =	0xffff
};

/* Note: IRMlit operands must be < REF_BIAS, too!
** This allows for fast and uniform manipulation of all operands
** without looking up the operand mode in lj_ir_mode:
** - CSE calculates the maximum reference of two operands.
**   This must work with mixed reference/literal operands, too.
** - DCE marking only checks for operand >= REF_BIAS.
** - LOOP needs to substitute reference operands.
**   Constant references and literals must not be modified.
*/

#define IRREF2(lo, hi)		((IRRef2)(lo) | ((IRRef2)(hi) << 16))

#define irref_isk(ref)		((ref) < REF_BIAS)

/* Tagged IR references. */
typedef uint32_t TRef;

#define TREF(ref, t)		(cast(TRef, (ref) + ((t)<<16)))

#define tref_ref(tr)		(cast(IRRef1, (tr)))
#define tref_t(tr)		(cast(IRType, (tr)>>16))
#define tref_type(tr)		(cast(IRType, ((tr)>>16) & IRT_TYPE))
#define tref_typerange(tr, first, last) \
  ((((tr)>>16) & IRT_TYPE) - (TRef)(first) <= (TRef)(last-first))

#define tref_istype(tr, t)	(((tr) & (IRT_TYPE<<16)) == ((t)<<16))
#define tref_isnil(tr)		(tref_istype((tr), IRT_NIL))
#define tref_isfalse(tr)	(tref_istype((tr), IRT_FALSE))
#define tref_istrue(tr)		(tref_istype((tr), IRT_TRUE))
#define tref_isstr(tr)		(tref_istype((tr), IRT_STR))
#define tref_isfunc(tr)		(tref_istype((tr), IRT_FUNC))
#define tref_istab(tr)		(tref_istype((tr), IRT_TAB))
#define tref_isudata(tr)	(tref_istype((tr), IRT_UDATA))
#define tref_isnum(tr)		(tref_istype((tr), IRT_NUM))
#define tref_isint(tr)		(tref_istype((tr), IRT_INT))

#define tref_isbool(tr)		(tref_typerange((tr), IRT_FALSE, IRT_TRUE))
#define tref_ispri(tr)		(tref_typerange((tr), IRT_NIL, IRT_TRUE))
#define tref_istruecond(tr)	(!tref_typerange((tr), IRT_NIL, IRT_FALSE))
#define tref_isinteger(tr)	(tref_typerange((tr), IRT_INT, IRT_U16))
#define tref_isnumber(tr)	(tref_typerange((tr), IRT_NUM, IRT_U16))
#define tref_isnumber_str(tr)	(tref_isnumber((tr)) || tref_isstr((tr)))
#define tref_isgcv(tr)		(tref_typerange((tr), IRT_STR, IRT_UDATA))

#define tref_isk(tr)		(irref_isk(tref_ref((tr))))
#define tref_isk2(tr1, tr2)	(irref_isk(tref_ref((tr1) | (tr2))))

#define TREF_PRI(t)		(TREF(REF_NIL-(t), (t)))
#define TREF_NIL		(TREF_PRI(IRT_NIL))
#define TREF_FALSE		(TREF_PRI(IRT_FALSE))
#define TREF_TRUE		(TREF_PRI(IRT_TRUE))

/* IR instruction format (64 bit).
**
**    16      16     8   8   8   8
** +-------+-------+---+---+---+---+
** |  op1  |  op2  | t | o | r | s |
** +-------+-------+---+---+---+---+
** |  op12/i/gco   |   ot  | prev  | (alternative fields in union)
** +---------------+-------+-------+
**        32           16      16
**
** prev is only valid prior to register allocation and then reused for r + s.
*/

typedef union IRIns {
  struct {
    LJ_ENDIAN_LOHI(
      IRRef1 op1;	/* IR operand 1. */
    , IRRef1 op2;	/* IR operand 2. */
    )
    IROpT ot;		/* IR opcode and type (overlaps t and o). */
    IRRef1 prev;	/* Previous ins in same chain (overlaps r and s). */
  };
  struct {
    IRRef2 op12;	/* IR operand 1 and 2 (overlaps op1 and op2). */
    LJ_ENDIAN_LOHI(
      IRType1 t;	/* IR type. */
    , IROp1 o;		/* IR opcode. */
    )
    LJ_ENDIAN_LOHI(
      uint8_t r;	/* Register allocation (overlaps prev). */
    , uint8_t s;	/* Spill slot allocation (overlaps prev). */
    )
  };
  int32_t i;		/* 32 bit signed integer literal (overlaps op12). */
  GCRef gcr;		/* GCobj constant (overlaps op12). */
  MRef ptr;		/* Pointer constant (overlaps op12). */
} IRIns;

#define ir_kgc(ir)	(gcref((ir)->gcr))
#define ir_kstr(ir)	(gco2str(ir_kgc((ir))))
#define ir_ktab(ir)	(gco2tab(ir_kgc((ir))))
#define ir_kfunc(ir)	(gco2func(ir_kgc((ir))))
#define ir_knum(ir)	(mref((ir)->ptr, cTValue))

#endif
