/*
** LuaJIT VM tags, values and objects.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#ifndef _LJ_OBJ_H
#define _LJ_OBJ_H

#include "lua.h"
#include "lj_def.h"
#include "lj_arch.h"

/* -- Memory references (32 bit address space) ---------------------------- */

/* Memory size. */
typedef uint32_t MSize;

/* Memory reference */
typedef struct MRef {
  uint32_t ptr32;	/* Pseudo 32 bit pointer. */
} MRef;

#define mref(r, t)	((t *)(void *)(uintptr_t)(r).ptr32)

#define setmref(r, p)	((r).ptr32 = (uint32_t)(uintptr_t)(void *)(p))
#define setmrefr(r, v)	((r).ptr32 = (v).ptr32)

/* -- GC object references (32 bit address space) ------------------------- */

/* GCobj reference */
typedef struct GCRef {
  uint32_t gcptr32;	/* Pseudo 32 bit pointer. */
} GCRef;

/* Common GC header for all collectable objects. */
#define GCHeader	GCRef nextgc; uint8_t marked; uint8_t gct
/* This occupies 6 bytes, so use the next 2 bytes for non-32 bit fields. */

#define gcref(r)	((GCobj *)(uintptr_t)(r).gcptr32)
#define gcrefp(r, t)	((t *)(void *)(uintptr_t)(r).gcptr32)
#define gcrefu(r)	((r).gcptr32)
#define gcrefi(r)	((int32_t)(r).gcptr32)
#define gcrefeq(r1, r2)	((r1).gcptr32 == (r2).gcptr32)
#define gcnext(gc)	(gcref((gc)->gch.nextgc))

#define setgcref(r, gc)	((r).gcptr32 = (uint32_t)(uintptr_t)&(gc)->gch)
#define setgcrefi(r, i)	((r).gcptr32 = (uint32_t)(i))
#define setgcrefp(r, p)	((r).gcptr32 = (uint32_t)(uintptr_t)(p))
#define setgcrefnull(r)	((r).gcptr32 = 0)
#define setgcrefr(r, v)	((r).gcptr32 = (v).gcptr32)

/* IMPORTANT NOTE:
**
** All uses of the setgcref* macros MUST be accompanied with a write barrier.
**
** This is to ensure the integrity of the incremental GC. The invariant
** to preserve is that a black object never points to a white object.
** I.e. never store a white object into a field of a black object.
**
** It's ok to LEAVE OUT the write barrier ONLY in the following cases:
** - The source is not a GC object (NULL).
** - The target is a GC root. I.e. everything in global_State.
** - The target is a lua_State field (threads are never black).
** - The target is a stack slot, see setgcV et al.
** - The target is an open upvalue, i.e. pointing to a stack slot.
** - The target is a newly created object (i.e. marked white). But make
**   sure nothing invokes the GC inbetween.
** - The target and the source are the same object (self-reference).
** - The target already contains the object (e.g. moving elements around).
**
** The most common case is a store to a stack slot. All other cases where
** a barrier has been omitted are annotated with a NOBARRIER comment.
**
** The same logic applies for stores to table slots (array part or hash
** part). ALL uses of lj_tab_set* require a barrier for the stored *value*
** (if it's a GC object). The barrier for the *key* is already handled
** internally by lj_tab_newkey.
*/

/* -- Common type definitions --------------------------------------------- */

/* Types for handling bytecodes. Need this here, details in lj_bc.h. */
typedef uint32_t BCIns;  /* Bytecode instruction. */
typedef uint32_t BCPos;  /* Bytecode position. */
typedef uint32_t BCReg;  /* Bytecode register. */
typedef int32_t BCLine;  /* Bytecode line number. */

/* Internal assembler functions. Never call these directly from C. */
typedef void (*ASMFunction)(void);

/* Resizable string buffer. Need this here, details in lj_str.h. */
typedef struct SBuf {
  char *buf;		/* String buffer base. */
  MSize n;		/* String buffer length. */
  MSize sz;		/* String buffer size. */
} SBuf;

/* -- Tags and values ----------------------------------------------------- */

/* Frame link. */
typedef union {
  int32_t ftsz;		/* Frame type and size of previous frame. */
  MRef pcr;		/* Overlaps PC for Lua frames. */
} FrameLink;

/* Tagged value. */
typedef LJ_ALIGN(8) union TValue {
  uint64_t u64;		/* 64 bit pattern overlaps number. */
  lua_Number n;		/* Number object overlaps split tag/value object. */
  struct {
    LJ_ENDIAN_LOHI(
      GCRef gcr;	/* GCobj reference (if any). */
    , int32_t it;	/* Internal object tag. Must overlap MSW of number. */
    )
  };
  struct {
    LJ_ENDIAN_LOHI(
      GCRef func;	/* Function for next frame (or dummy L). */
    , FrameLink tp;	/* Link to previous frame. */
    )
  } fr;
  struct {
    LJ_ENDIAN_LOHI(
      uint32_t lo;	/* Lower 32 bits of number. */
    , uint32_t hi;	/* Upper 32 bits of number. */
    )
  } u32;
} TValue;

typedef const TValue cTValue;

#define tvref(r)	(mref(r, TValue))

/* More external and GCobj tags for internal objects. */
#define LAST_TT		LUA_TTHREAD

#define LUA_TPROTO	(LAST_TT+1)
#define LUA_TUPVAL	(LAST_TT+2)
#define LUA_TDEADKEY	(LAST_TT+3)

/* Internal object tags.
**
** Internal tags overlap the MSW of a number object (must be a double).
** Interpreted as a double these are special NaNs. The FPU only generates
** one type of NaN (0xfff8_0000_0000_0000). So MSWs > 0xfff80000 are available
** for use as internal tags. Small negative numbers are used to shorten the
** encoding of type comparisons (reg/mem against sign-ext. 8 bit immediate).
**
**                  ---MSW---.---LSW---
** primitive types |  itype  |         |
** lightuserdata   |  itype  |  void * |  (32 bit platforms)
** lightuserdata   |fffc|    void *    |  (64 bit platforms, 48 bit pointers)
** GC objects      |  itype  |  GCRef  |
** number           -------double------
**
** ORDER LJ_T
** Primitive types nil/false/true must be first, lightuserdata next.
** GC objects are at the end, table/userdata must be lowest.
** Also check lj_ir.h for similar ordering constraints.
*/
#define LJ_TNIL			(-1)
#define LJ_TFALSE		(-2)
#define LJ_TTRUE		(-3)
#define LJ_TLIGHTUD		(-4)
#define LJ_TSTR			(-5)
#define LJ_TUPVAL		(-6)
#define LJ_TTHREAD		(-7)
#define LJ_TPROTO		(-8)
#define LJ_TFUNC		(-9)
#define LJ_TDEADKEY		(-10)
#define LJ_TTAB			(-11)
#define LJ_TUDATA		(-12)
/* This is just the canonical number type used in some places. */
#define LJ_TNUMX		(-13)

#if LJ_64
#define LJ_TISNUM		((uint32_t)0xfff80000)
#else
#define LJ_TISNUM		((uint32_t)LJ_TNUMX)
#endif
#define LJ_TISTRUECOND		((uint32_t)LJ_TFALSE)
#define LJ_TISPRI		((uint32_t)LJ_TTRUE)
#define LJ_TISGCV		((uint32_t)(LJ_TSTR+1))
#define LJ_TISTABUD		((uint32_t)LJ_TTAB)

/* -- TValue getters/setters ---------------------------------------------- */

/* Macros to test types. */
#define itype(o)	((o)->it)
#define uitype(o)	((uint32_t)itype(o))
#define tvisnil(o)	(itype(o) == LJ_TNIL)
#define tvisfalse(o)	(itype(o) == LJ_TFALSE)
#define tvistrue(o)	(itype(o) == LJ_TTRUE)
#define tvisbool(o)	(tvisfalse(o) || tvistrue(o))
#if LJ_64
#define tvislightud(o)	((itype(o) >> 16) == LJ_TLIGHTUD)
#else
#define tvislightud(o)	(itype(o) == LJ_TLIGHTUD)
#endif
#define tvisstr(o)	(itype(o) == LJ_TSTR)
#define tvisfunc(o)	(itype(o) == LJ_TFUNC)
#define tvisthread(o)	(itype(o) == LJ_TTHREAD)
#define tvisproto(o)	(itype(o) == LJ_TPROTO)
#define tvistab(o)	(itype(o) == LJ_TTAB)
#define tvisudata(o)	(itype(o) == LJ_TUDATA)
#define tvisnum(o)	(uitype(o) <= LJ_TISNUM)

#define tvistruecond(o)	(uitype(o) < LJ_TISTRUECOND)
#define tvispri(o)	(uitype(o) >= LJ_TISPRI)
#define tvistabud(o)	(uitype(o) <= LJ_TISTABUD)  /* && !tvisnum() */
#define tvisgcv(o) \
  ((uitype(o) - LJ_TISGCV) > ((uint32_t)LJ_TNUMX - LJ_TISGCV))

/* Special macros to test numbers for NaN, +0, -0, +1 and raw equality. */
#define tvisnan(o)	((o)->n != (o)->n)
#define tvispzero(o)	((o)->u64 == 0)
#define tvismzero(o)	((o)->u64 == U64x(80000000,00000000))
#define tvispone(o)	((o)->u64 == U64x(3ff00000,00000000))
#define rawnumequal(o1, o2)	((o1)->u64 == (o2)->u64)

/* Macros to convert type ids. */
#if LJ_64
#define itypemap(o) \
  (tvisnum(o) ? ~LJ_TNUMX : tvislightud(o) ? ~LJ_TLIGHTUD : ~itype(o))
#else
#define itypemap(o)	(tvisnum(o) ? ~LJ_TNUMX : ~itype(o))
#endif

/* Macros to get tagged values. */
#define gcval(o)	(gcref((o)->gcr))
#define boolV(o)	check_exp(tvisbool(o), (LJ_TFALSE - (o)->it))
#if LJ_64
#define lightudV(o)	check_exp(tvislightud(o), \
			  (void *)((o)->u64 & U64x(0000ffff,ffffffff)))
#else
#define lightudV(o)	check_exp(tvislightud(o), gcrefp((o)->gcr, void))
#endif
#define gcV(o)		check_exp(tvisgcv(o), gcval(o))
#define strV(o)		check_exp(tvisstr(o), &gcval(o)->str)
#define funcV(o)	check_exp(tvisfunc(o), &gcval(o)->fn)
#define threadV(o)	check_exp(tvisthread(o), &gcval(o)->th)
#define protoV(o)	check_exp(tvisproto(o), &gcval(o)->pt)
#define tabV(o)		check_exp(tvistab(o), &gcval(o)->tab)
#define udataV(o)	check_exp(tvisudata(o), &gcval(o)->ud)
#define numV(o)		check_exp(tvisnum(o), (o)->n)

/* Macros to set tagged values. */
#define setitype(o, i)		((o)->it = (i))
#define setnilV(o)		((o)->it = LJ_TNIL)
#define setboolV(o, x)		((o)->it = LJ_TFALSE-(x))

#if LJ_64
#define checklightudptr(L, p) \
  (((uint64_t)(p) >> 48) ? (lj_err_msg(L, LJ_ERR_BADLU), NULL) : (p))
#define setlightudV(o, x) \
  ((o)->u64 = (uint64_t)(x) | (((uint64_t)LJ_TLIGHTUD) << 48))
#define setcont(o, x) \
  ((o)->u64 = (uint64_t)(x) - (uint64_t)lj_vm_asm_begin)
#else
#define checklightudptr(L, p)	(p)
#define setlightudV(o, x) \
  { TValue *i_o = (o); \
    setgcrefp(i_o->gcr, (x)); i_o->it = LJ_TLIGHTUD; }
#define setcont(o, x) \
  { TValue *i_o = (o); \
    setgcrefp(i_o->gcr, (x)); i_o->it = LJ_TLIGHTUD; }
#endif

#define tvchecklive(g, o) \
  lua_assert(!tvisgcv(o) || \
  ((~itype(o) == gcval(o)->gch.gct) && !isdead(g, gcval(o))))

#define setgcV(L, o, x, itype) \
  { TValue *i_o = (o); \
    setgcrefp(i_o->gcr, &(x)->nextgc); i_o->it = itype; \
    tvchecklive(G(L), i_o); }
#define setstrV(L, o, x)	setgcV(L, o, x, LJ_TSTR)
#define setthreadV(L, o, x)	setgcV(L, o, x, LJ_TTHREAD)
#define setprotoV(L, o, x)	setgcV(L, o, x, LJ_TPROTO)
#define setfuncV(L, o, x)	setgcV(L, o, &(x)->l, LJ_TFUNC)
#define settabV(L, o, x)	setgcV(L, o, x, LJ_TTAB)
#define setudataV(L, o, x)	setgcV(L, o, x, LJ_TUDATA)

#define setnumV(o, x)		((o)->n = (x))
#define setnanV(o)		((o)->u64 = U64x(fff80000,00000000))
#define setintV(o, i)		((o)->n = cast_num((int32_t)(i)))

/* Copy tagged values. */
#define copyTV(L, o1, o2) \
  { cTValue *i_o2 = (o2); TValue *i_o1 = (o1); \
    *i_o1 = *i_o2; tvchecklive(G(L), i_o1); }

/* -- String object ------------------------------------------------------- */

/* String object header. String payload follows. */
typedef struct GCstr {
  GCHeader;
  uint8_t reserved;	/* Used by lexer for fast lookup of reserved words. */
  uint8_t unused;
  MSize hash;		/* Hash of string. */
  MSize len;		/* Size of string. */
} GCstr;

#define strref(r)	(&gcref((r))->str)
#define strdata(s)	((const char *)((s)+1))
#define strdatawr(s)	((char *)((s)+1))
#define strVdata(o)	strdata(strV(o))
#define sizestring(s)	(sizeof(struct GCstr)+(s)->len+1)

/* -- Userdata object ----------------------------------------------------- */

/* Userdata object. Payload follows. */
typedef struct GCudata {
  GCHeader;
  uint8_t unused1;
  uint8_t unused2;
  GCRef env;		/* Should be at same offset in GCfunc. */
  MSize len;		/* Size of payload. */
  GCRef metatable;	/* Must be at same offset in GCtab. */
  uint32_t align1;	/* To force 8 byte alignment of the payload. */
} GCudata;

#define uddata(u)	((void *)((u)+1))
#define sizeudata(u)	(sizeof(struct GCudata)+(u)->len)

/* -- Prototype object ---------------------------------------------------- */

/* Split constant array. Collectables are below, numbers above pointer. */
typedef union ProtoK {
  lua_Number *n;	/* Numbers. */
  GCRef *gc;		/* Collectable objects (strings/table/proto). */
} ProtoK;

#define SCALE_NUM_GCO	((int32_t)sizeof(lua_Number)/sizeof(GCRef))
#define round_nkgc(n)	(((n) + SCALE_NUM_GCO-1) & ~(SCALE_NUM_GCO-1))

typedef struct VarInfo {
  GCstr *name;		/* Local variable name. */
  BCPos startpc;	/* First point where the local variable is active. */
  BCPos endpc;		/* First point where the local variable is dead. */
} VarInfo;

typedef struct GCproto {
  GCHeader;
  uint8_t numparams;	/* Number of parameters. */
  uint8_t framesize;	/* Fixed frame size. */
  MSize sizebc;		/* Number of bytecode instructions. */
  GCRef gclist;
  ProtoK k;		/* Split constant array (points to the middle). */
  BCIns *bc;		/* Array of bytecode instructions. */
  int16_t *uv;		/* Upvalue list. local >= 0. parent uv < 0. */
  MSize sizekgc;	/* Number of collectable constants. */
  MSize sizekn;		/* Number of lua_Number constants. */
  uint8_t sizeuv;	/* Number of upvalues. */
  uint8_t flags;	/* Miscellaneous flags (see below). */
  uint16_t trace;	/* Anchor for chain of root traces. */
  /* ------ The following fields are for debugging/tracebacks only ------ */
  MSize sizelineinfo;	/* Size of lineinfo array (may be 0). */
  MSize sizevarinfo;	/* Size of local var info array (may be 0). */
  MSize sizeuvname;	/* Size of upvalue names array (may be 0). */
  BCLine linedefined;	/* First line of the function definition. */
  BCLine lastlinedefined;  /* Last line of the function definition. */
  BCLine *lineinfo;	/* Map from bytecode instructions to source lines. */
  struct VarInfo *varinfo;  /* Names and extents of local variables. */
  GCstr **uvname;	/* Upvalue names. */
  GCstr *chunkname;	/* Name of the chunk this function was defined in. */
} GCproto;

#define PROTO_IS_VARARG		0x01
#define PROTO_HAS_FNEW		0x02
#define PROTO_HAS_RETURN	0x04
#define PROTO_FIXUP_RETURN	0x08
#define PROTO_NO_JIT		0x10
#define PROTO_HAS_ILOOP		0x20

/* -- Upvalue object ------------------------------------------------------ */

typedef struct GCupval {
  GCHeader;
  uint8_t closed;	/* Set if closed (i.e. uv->v == &uv->u.value). */
  uint8_t unused;
  union {
    TValue tv;		/* If closed: the value itself. */
    struct {		/* If open: double linked list, anchored at thread. */
      GCRef prev;
      GCRef next;
    };
  };
  TValue *v;		/* Points to stack slot (open) or above (closed). */
#if LJ_32
  int32_t unusedv;	/* For consistent alignment (32 bit only). */
#endif
} GCupval;

#define uvprev(uv_)	(&gcref((uv_)->prev)->uv)
#define uvnext(uv_)	(&gcref((uv_)->next)->uv)

/* -- Function object (closures) ------------------------------------------ */

/* Common header for functions. env should be at same offset in GCudata. */
#define GCfuncHeader \
  GCHeader; uint8_t ffid; uint8_t nupvalues; \
  GCRef env; GCRef gclist; ASMFunction gate

typedef struct GCfuncC {
  GCfuncHeader;
  lua_CFunction f;	/* C function to be called. */
  TValue upvalue[1];	/* Array of upvalues (TValue). */
} GCfuncC;

typedef struct GCfuncL {
  GCfuncHeader;
  GCRef pt;		/* Link to prototype this function is based on. */
  GCRef uvptr[1];	/* Array of _pointers_ to upvalue objects (GCupval). */
} GCfuncL;

typedef union GCfunc {
  GCfuncC c;
  GCfuncL l;
} GCfunc;

#define FF_LUA		0
#define FF_C		1
#define isluafunc(fn)	((fn)->c.ffid == FF_LUA)
#define iscfunc(fn)	((fn)->c.ffid == FF_C)
#define isffunc(fn)	((fn)->c.ffid > FF_C)
#define funcproto(fn)	check_exp(isluafunc(fn), &gcref((fn)->l.pt)->pt)
#define sizeCfunc(n)	(sizeof(GCfuncC) + sizeof(TValue)*((n)-1))
#define sizeLfunc(n)	(sizeof(GCfuncL) + sizeof(TValue *)*((n)-1))

/* -- Table object -------------------------------------------------------- */

/* Hash node. */
typedef struct Node {
  TValue val;		/* Value object. Must be first field. */
  TValue key;		/* Key object. */
  MRef next;		/* Hash chain. */
  int32_t unused;	/* For consistent alignment. */
} Node;

LJ_STATIC_ASSERT(offsetof(Node, val) == 0);

typedef struct GCtab {
  GCHeader;
  uint8_t nomm;		/* Negative cache for fast metamethods. */
  int8_t colo;		/* Array colocation. */
  MRef array;		/* Array part. */
  GCRef gclist;
  GCRef metatable;	/* Must be at same offset in GCudata. */
  MRef node;		/* Hash part. */
  uint32_t asize;	/* Size of array part (keys [0, asize-1]). */
  uint32_t hmask;	/* Hash part mask (size of hash part - 1). */
  MRef lastfree;	/* Any free position is before this position. */
} GCtab;

#define sizetabcolo(n)	((n)*sizeof(TValue) + sizeof(GCtab))
#define tabref(r)	(&gcref((r))->tab)
#define noderef(r)	(mref((r), Node))
#define nextnode(n)	(mref((n)->next, Node))

/* -- State objects ------------------------------------------------------- */

/* VM states. */
enum {
  LJ_VMST_INTERP,	/* Interpreter. */
  LJ_VMST_C,		/* C function. */
  LJ_VMST_GC,		/* Garbage collector. */
  LJ_VMST_EXIT,		/* Trace exit handler. */
  LJ_VMST_RECORD,	/* Trace recorder. */
  LJ_VMST_OPT,		/* Optimizer. */
  LJ_VMST_ASM,		/* Assembler. */
  LJ_VMST__MAX
};

#define setvmstate(g, st)	((g)->vmstate = ~LJ_VMST_##st)

/* Metamethods. */
#define MMDEF(_) \
  _(index) _(newindex) _(gc) _(mode) _(eq) \
  /* Only the above (fast) metamethods are negative cached (max. 8). */ \
  _(len) _(lt) _(le) _(concat) _(call) \
  /* The following must be in ORDER ARITH. */ \
  _(add) _(sub) _(mul) _(div) _(mod) _(pow) _(unm) \
  /* The following are used in the standard libraries. */ \
  _(metatable) _(tostring)

typedef enum {
#define MMENUM(name)	MM_##name,
MMDEF(MMENUM)
#undef MMENUM
  MM_MAX,
  MM____ = MM_MAX,
  MM_FAST = MM_eq
} MMS;

#define BASEMT_MAX	((~LJ_TNUMX)+1)

typedef struct GCState {
  MSize total;		/* Memory currently allocated. */
  MSize threshold;	/* Memory threshold. */
  uint8_t currentwhite;	/* Current white color. */
  uint8_t state;	/* GC state. */
  uint8_t unused1;
  uint8_t unused2;
  MSize sweepstr;	/* Sweep position in string table. */
  GCRef root;		/* List of all collectable objects. */
  GCRef *sweep;		/* Sweep position in root list. */
  GCRef gray;		/* List of gray objects. */
  GCRef grayagain;	/* List of objects for atomic traversal. */
  GCRef weak;		/* List of weak tables (to be cleared). */
  GCRef mmudata;	/* List of userdata (to be finalized). */
  MSize stepmul;	/* Incremental GC step granularity. */
  MSize debt;		/* Debt (how much GC is behind schedule). */
  MSize estimate;	/* Estimate of memory actually in use. */
  MSize pause;		/* Pause between successive GC cycles. */
} GCState;

/* Global state, shared by all threads of a Lua universe. */
typedef struct global_State {
  GCRef *strhash;	/* String hash table (hash chain anchors). */
  MSize strmask;	/* String hash mask (size of hash table - 1). */
  MSize strnum;		/* Number of strings in hash table. */
  lua_Alloc allocf;	/* Memory allocator. */
  void *allocd;		/* Memory allocator data. */
  GCState gc;		/* Garbage collector. */
  SBuf tmpbuf;		/* Temporary buffer for string concatenation. */
  Node nilnode;		/* Fallback 1-element hash part (nil key and value). */
  uint8_t hookmask;	/* Hook mask. */
  uint8_t dispatchmode;	/* Dispatch mode. */
  uint8_t vmevmask;	/* VM event mask. */
  uint8_t wrapmode;	/* Wrap mode. */
  GCRef mainthref;	/* Link to main thread. */
  TValue registrytv;	/* Anchor for registry. */
  TValue tmptv;		/* Temporary TValue. */
  GCupval uvhead;	/* Head of double-linked list of all open upvalues. */
  int32_t hookcount;	/* Instruction hook countdown. */
  int32_t hookcstart;	/* Start count for instruction hook counter. */
  lua_Hook hookf;	/* Hook function. */
  lua_CFunction wrapf;	/* Wrapper for C function calls. */
  lua_CFunction panic;	/* Called as a last resort for errors. */
  volatile int32_t vmstate;  /* VM state or current JIT code trace number. */
  GCRef jit_L;		/* Current JIT code lua_State or NULL. */
  MRef jit_base;	/* Current JIT code L->base. */
  GCRef basemt[BASEMT_MAX];  /* Metatables for base types. */
  GCRef mmname[MM_MAX];	/* Array holding metamethod names. */
} global_State;

#define mainthread(g)	(&gcref(g->mainthref)->th)
#define niltv(L) \
  check_exp(tvisnil(&G(L)->nilnode.val), &G(L)->nilnode.val)
#define niltvg(g) \
  check_exp(tvisnil(&(g)->nilnode.val), &(g)->nilnode.val)

/* Hook management. Hook event masks are defined in lua.h. */
#define HOOK_EVENTMASK		0x0f
#define HOOK_ACTIVE		0x10
#define HOOK_VMEVENT		0x20
#define HOOK_GC			0x40
#define hook_active(g)		((g)->hookmask & HOOK_ACTIVE)
#define hook_enter(g)		((g)->hookmask |= HOOK_ACTIVE)
#define hook_entergc(g)		((g)->hookmask |= (HOOK_ACTIVE|HOOK_GC))
#define hook_vmevent(g)		((g)->hookmask |= (HOOK_ACTIVE|HOOK_VMEVENT))
#define hook_leave(g)		((g)->hookmask &= ~HOOK_ACTIVE)
#define hook_save(g)		((g)->hookmask & ~HOOK_EVENTMASK)
#define hook_restore(g, h) \
  ((g)->hookmask = ((g)->hookmask & HOOK_EVENTMASK) | (h))

/* Per-thread state object. */
struct lua_State {
  GCHeader;
  uint8_t dummy_ffid;	/* Fake FF_C for curr_funcisL() on dummy frames. */
  uint8_t status;	/* Thread status. */
  MRef glref;		/* Link to global state. */
  GCRef gclist;		/* GC chain. */
  TValue *base;		/* Base of currently executing function. */
  TValue *top;		/* First free slot in the stack. */
  TValue *maxstack;	/* Last free slot in the stack. */
  TValue *stack;	/* Stack base. */
  GCRef openupval;	/* List of open upvalues in the stack. */
  GCRef env;		/* Thread environment (table of globals). */
  void *cframe;		/* End of C stack frame chain. */
  MSize stacksize;	/* True stack size (incl. LJ_STACK_EXTRA). */
};

#define G(L)			(mref(L->glref, global_State))
#define registry(L)		(&G(L)->registrytv)

/* Macros to access the currently executing (Lua) function. */
#define curr_func(L)		(&gcref((L->base-1)->fr.func)->fn)
#define curr_funcisL(L)		(isluafunc(curr_func(L)))
#define curr_proto(L)		(funcproto(curr_func(L)))
#define curr_topL(L)		(L->base + curr_proto(L)->framesize)
#define curr_top(L)		(curr_funcisL(L) ? curr_topL(L) : L->top)

/* -- GC object definition and conversions -------------------------------- */

/* GC header for generic access to common fields of GC objects. */
typedef struct GChead {
  GCHeader;
  uint8_t unused1;
  uint8_t unused2;
  GCRef env;
  GCRef gclist;
  GCRef metatable;
} GChead;

/* The env field SHOULD be at the same offset for all GC objects. */
LJ_STATIC_ASSERT(offsetof(GChead, env) == offsetof(GCfuncL, env));
LJ_STATIC_ASSERT(offsetof(GChead, env) == offsetof(GCudata, env));

/* The metatable field MUST be at the same offset for all GC objects. */
LJ_STATIC_ASSERT(offsetof(GChead, metatable) == offsetof(GCtab, metatable));
LJ_STATIC_ASSERT(offsetof(GChead, metatable) == offsetof(GCudata, metatable));

/* The gclist field MUST be at the same offset for all GC objects. */
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(lua_State, gclist));
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(GCproto, gclist));
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(GCfuncL, gclist));
LJ_STATIC_ASSERT(offsetof(GChead, gclist) == offsetof(GCtab, gclist));

typedef union GCobj {
  GChead gch;
  GCstr str;
  GCupval uv;
  lua_State th;
  GCproto pt;
  GCfunc fn;
  GCtab tab;
  GCudata ud;
} GCobj;

/* Macros to convert a GCobj pointer into a specific value. */
#define gco2str(o)	check_exp((o)->gch.gct == ~LJ_TSTR, &(o)->str)
#define gco2uv(o)	check_exp((o)->gch.gct == ~LJ_TUPVAL, &(o)->uv)
#define gco2th(o)	check_exp((o)->gch.gct == ~LJ_TTHREAD, &(o)->th)
#define gco2pt(o)	check_exp((o)->gch.gct == ~LJ_TPROTO, &(o)->pt)
#define gco2func(o)	check_exp((o)->gch.gct == ~LJ_TFUNC, &(o)->fn)
#define gco2tab(o)	check_exp((o)->gch.gct == ~LJ_TTAB, &(o)->tab)
#define gco2ud(o)	check_exp((o)->gch.gct == ~LJ_TUDATA, &(o)->ud)

/* Macro to convert any collectable object into a GCobj pointer. */
#define obj2gco(v)	(cast(GCobj *, (v)))

/* -- Number to integer conversion ---------------------------------------- */

static LJ_AINLINE int32_t lj_num2bit(lua_Number n)
{
  TValue o;
  o.n = n + 6755399441055744.0;  /* 2^52 + 2^51 */
  return (int32_t)o.u32.lo;
}

#if (defined(__i386__) || defined(_M_IX86)) && !defined(__SSE2__)
#define lj_num2int(n)   lj_num2bit((n))
#else
#define lj_num2int(n)   ((int32_t)(n))
#endif

/* -- Miscellaneous object handling --------------------------------------- */

/* Names and maps for internal and external object tags. */
LJ_DATA const char *const lj_obj_typename[1+LUA_TUPVAL+1];
LJ_DATA const char *const lj_obj_itypename[~LJ_TNUMX+1];

#define typename(o)	(lj_obj_itypename[itypemap(o)])

/* Compare two objects without calling metamethods. */
LJ_FUNC int lj_obj_equal(cTValue *o1, cTValue *o2);

#ifdef LUA_USE_ASSERT
#include "lj_gc.h"
#endif

#endif
