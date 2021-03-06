/*
** Common definitions for the JIT compiler.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_JIT_H
#define _LJ_JIT_H

#include "lj_obj.h"
#include "lj_ir.h"

/* JIT engine flags. */
#define JIT_F_ON		0x00000001

/* CPU-specific JIT engine flags. */
#if LJ_TARGET_X86ORX64
#define JIT_F_CMOV		0x00000100
#define JIT_F_SSE2		0x00000200
#define JIT_F_SSE4_1		0x00000400
#define JIT_F_P4		0x00000800
#define JIT_F_PREFER_IMUL	0x00001000
#define JIT_F_SPLIT_XMM		0x00002000
#define JIT_F_LEA_AGU		0x00004000

/* Names for the CPU-specific flags. Must match the order above. */
#define JIT_F_CPU_FIRST		JIT_F_CMOV
#define JIT_F_CPUSTRING		"\4CMOV\4SSE2\6SSE4.1\2P4\3AMD\2K8\4ATOM"
#else
#error "Missing CPU-specific JIT engine flags"
#endif

/* Optimization flags. */
#define JIT_F_OPT_MASK		0x00ff0000

#define JIT_F_OPT_FOLD		0x00010000
#define JIT_F_OPT_CSE		0x00020000
#define JIT_F_OPT_DCE		0x00040000
#define JIT_F_OPT_FWD		0x00080000
#define JIT_F_OPT_DSE		0x00100000
#define JIT_F_OPT_NARROW	0x00200000
#define JIT_F_OPT_LOOP		0x00400000
#define JIT_F_OPT_FUSE		0x00800000

/* Optimizations names for -O. Must match the order above. */
#define JIT_F_OPT_FIRST		JIT_F_OPT_FOLD
#define JIT_F_OPTSTRING	\
  "\4fold\3cse\3dce\3fwd\3dse\6narrow\4loop\4fuse"

/* Optimization levels set a fixed combination of flags. */
#define JIT_F_OPT_0	0
#define JIT_F_OPT_1	(JIT_F_OPT_FOLD|JIT_F_OPT_CSE|JIT_F_OPT_DCE)
#define JIT_F_OPT_2	(JIT_F_OPT_1|JIT_F_OPT_NARROW|JIT_F_OPT_LOOP)
#define JIT_F_OPT_3	(JIT_F_OPT_2|JIT_F_OPT_FWD|JIT_F_OPT_DSE|JIT_F_OPT_FUSE)
#define JIT_F_OPT_DEFAULT	JIT_F_OPT_3

#ifdef LUA_USE_WIN
/* See: http://blogs.msdn.com/oldnewthing/archive/2003/10/08/55239.aspx */
#define JIT_P_sizemcode_DEFAULT		64
#else
/* Could go as low as 4K, but the mmap() overhead would be rather high. */
#define JIT_P_sizemcode_DEFAULT		32
#endif

/* Optimization parameters and their defaults. Length is a char in octal! */
#define JIT_PARAMDEF(_) \
  _(\010, maxtrace,	1000)	/* Max. # of traces in cache. */ \
  _(\011, maxrecord,	2000)	/* Max. # of recorded IR instructions. */ \
  _(\012, maxirconst,	500)	/* Max. # of IR constants of a trace. */ \
  _(\007, maxside,	100)	/* Max. # of side traces of a root trace. */ \
  _(\007, maxsnap,	100)	/* Max. # of snapshots for a trace. */ \
  \
  _(\007, hotloop,	57)	/* # of iterations to detect a hot loop. */ \
  _(\007, hotexit,	10)	/* # of taken exits to start a side trace. */ \
  _(\007, tryside,	4)	/* # of attempts to compile a side trace. */ \
  \
  _(\012, instunroll,	4)	/* Max. unroll for instable loops. */ \
  _(\012, loopunroll,	7)	/* Max. unroll for loop ops in side traces. */ \
  _(\012, callunroll,	3)	/* Max. unroll for recursive calls. */ \
  _(\011, recunroll,	0)	/* Max. unroll for true recursion. */ \
  \
  /* Size of each machine code area (in KBytes). */ \
  _(\011, sizemcode,	JIT_P_sizemcode_DEFAULT) \
  /* Max. total size of all machine code areas (in KBytes). */ \
  _(\010, maxmcode,	512) \
  /* End of list. */

enum {
#define JIT_PARAMENUM(len, name, value)	JIT_P_##name,
JIT_PARAMDEF(JIT_PARAMENUM)
#undef JIT_PARAMENUM
  JIT_P__MAX
};

#define JIT_PARAMSTR(len, name, value)	#len #name
#define JIT_P_STRING	JIT_PARAMDEF(JIT_PARAMSTR)

/* Trace compiler state. */
typedef enum {
  LJ_TRACE_IDLE,	/* Trace compiler idle. */
  LJ_TRACE_ACTIVE = 0x10,
  LJ_TRACE_RECORD,	/* Bytecode recording active. */
  LJ_TRACE_START,	/* New trace started. */
  LJ_TRACE_END,		/* End of trace. */
  LJ_TRACE_ASM,		/* Assemble trace. */
  LJ_TRACE_ERR,		/* Trace aborted with error. */
} TraceState;

/* Machine code type. */
typedef uint8_t MCode;

/* Stack snapshot header. */
typedef struct SnapShot {
  uint16_t mapofs;	/* Offset into snapshot map. */
  IRRef1 ref;		/* First IR ref for this snapshot. */
  uint8_t nslots;	/* Number of stack slots. */
  uint8_t nframelinks;	/* Number of frame links. */
  uint8_t count;	/* Count of taken exits for this snapshot. */
  uint8_t unused1;
} SnapShot;

#define SNAPCOUNT_DONE	255	/* Already compiled and linked a side trace. */
#define snap_ref(sn)	((IRRef)(IRRef1)(sn))
#define snap_ridsp(sn)	((sn) >> 16)

/* Snapshot and exit numbers. */
typedef uint32_t SnapNo;
typedef uint32_t ExitNo;

/* Trace number. */
typedef uint32_t TraceNo;	/* Used to pass around trace numbers. */
typedef uint16_t TraceNo1;	/* Stored trace number. */

#define TRACE_INTERP	0	/* Fallback to interpreter. */

/* Trace anchor. */
typedef struct Trace {
  IRIns *ir;		/* IR instructions/constants. Biased with REF_BIAS. */
  IRRef nins;		/* Next IR instruction. Biased with REF_BIAS. */
  IRRef nk;		/* Lowest IR constant. Biased with REF_BIAS. */
  SnapShot *snap;	/* Snapshot array. */
  IRRef2 *snapmap;	/* Snapshot map. */
  uint16_t nsnap;	/* Number of snapshots. */
  uint16_t nsnapmap;	/* Number of snapshot map elements. */
  GCRef startpt;	/* Starting prototype. */
  BCIns startins;	/* Original bytecode of starting instruction. */
  MCode *mcode;		/* Start of machine code. */
  MSize szmcode;	/* Size of machine code. */
  MSize mcloop;		/* Offset of loop start in machine code. */
  TraceNo1 link;	/* Linked trace (or self for loops). */
  TraceNo1 root;	/* Root trace of side trace (or 0 for root traces). */
  TraceNo1 nextroot;	/* Next root trace for same prototype. */
  TraceNo1 nextside;	/* Next side trace of same root trace. */
  uint16_t nchild;	/* Number of child traces (root trace only). */
  uint16_t spadjust;	/* Stack pointer adjustment (offset in bytes). */
#ifdef LUAJIT_USE_GDBJIT
  void *gdbjit_entry;	/* GDB JIT entry. */
#endif
} Trace;

/* Round-robin penalty cache for bytecodes leading to aborted traces. */
typedef struct HotPenalty {
  const BCIns *pc;	/* Starting bytecode PC. */
  uint16_t val;		/* Penalty value, i.e. hotcount start. */
  uint16_t reason;	/* Abort reason (really TraceErr). */
} HotPenalty;

/* Number of slots for the penalty cache. Must be a power of 2. */
#define PENALTY_SLOTS	16

/* Round-robin backpropagation cache for narrowing conversions. */
typedef struct BPropEntry {
  IRRef1 key;		/* Key: original reference. */
  IRRef1 val;		/* Value: reference after conversion. */
  IRRef mode;		/* Mode for this entry (currently IRTOINT_*). */
} BPropEntry;

/* Number of slots for the backpropagation cache. Must be a power of 2. */
#define BPROP_SLOTS	16

/* Fold state is used to fold instructions on-the-fly. */
typedef struct FoldState {
  IRIns ins;		/* Currently emitted instruction. */
  IRIns left;		/* Instruction referenced by left operand. */
  IRIns right;		/* Instruction referenced by right operand. */
} FoldState;

/* JIT compiler state. */
typedef struct jit_State {
  Trace cur;		/* Current trace. */

  lua_State *L;		/* Current Lua state. */
  const BCIns *pc;	/* Current PC. */
  BCReg maxslot;	/* Relative to baseslot. */

  uint32_t flags;	/* JIT engine flags. */
  TRef *base;		/* Current frame base, points into J->slots. */
  BCReg baseslot;	/* Current frame base, offset into J->slots. */
  GCfunc *fn;		/* Current function. */
  GCproto *pt;		/* Current prototype. */

  FoldState fold;	/* Fold state. */

  uint8_t mergesnap;	/* Allowed to merge with next snapshot. */
  uint8_t needsnap;	/* Need snapshot before recording next bytecode. */
  IRType1 guardemit;	/* Accumulated IRT_GUARD for emitted instructions. */
  uint8_t unused1;

  const BCIns *bc_min;	/* Start of allowed bytecode range for root trace. */
  MSize bc_extent;	/* Extent of the range. */

  TraceState state;	/* Trace compiler state. */

  int32_t instunroll;	/* Unroll counter for instable loops. */
  int32_t loopunroll;	/* Unroll counter for loop ops in side traces. */
  int32_t tailcalled;	/* Number of successive tailcalls. */
  int32_t framedepth;	/* Current frame depth. */

  MRef knum;		/* Pointer to chained array of KNUM constants. */

  IRIns *irbuf;		/* Temp. IR instruction buffer. Biased with REF_BIAS. */
  IRRef irtoplim;	/* Upper limit of instuction buffer (biased). */
  IRRef irbotlim;	/* Lower limit of instuction buffer (biased). */
  IRRef loopref;	/* Last loop reference or ref of final LOOP (or 0). */

  SnapShot *snapbuf;	/* Temp. snapshot buffer. */
  IRRef2 *snapmapbuf;	/* Temp. snapshot map buffer. */
  MSize sizesnap;	/* Size of temp. snapshot buffer. */
  MSize sizesnapmap;	/* Size of temp. snapshot map buffer. */

  Trace **trace;	/* Array of traces. */
  TraceNo curtrace;	/* Current trace number (if not 0). Kept in J->cur. */
  TraceNo freetrace;	/* Start of scan for next free trace. */
  MSize sizetrace;	/* Size of trace array. */

  IRRef1 chain[IR__MAX];  /* IR instruction skip-list chain anchors. */
  TRef slot[LJ_MAX_JSLOTS+LJ_STACK_EXTRA];  /* Stack slot map. */

  int32_t param[JIT_P__MAX];  /* JIT engine parameters. */

  MCode *exitstubgroup[LJ_MAX_EXITSTUBGR];  /* Exit stub group addresses. */

  HotPenalty penalty[PENALTY_SLOTS];  /* Penalty slots. */
  uint32_t penaltyslot;	/* Round-robin index into penalty slots. */

  BPropEntry bpropcache[BPROP_SLOTS];  /* Backpropagation cache slots. */
  uint32_t bpropslot;	/* Round-robin index into bpropcache slots. */

  const BCIns *startpc;	/* Bytecode PC of starting instruction. */
  TraceNo parent;	/* Parent of current side trace (0 for root traces). */
  ExitNo exitno;	/* Exit number in parent of current side trace. */

  TValue errinfo;	/* Additional info element for trace errors. */

  MCode *mcarea;	/* Base of current mcode area. */
  MCode *mctop;		/* Top of current mcode area. */
  MCode *mcbot;		/* Bottom of current mcode area. */
  size_t szmcarea;	/* Size of current mcode area. */
  size_t szallmcarea;	/* Total size of all allocated mcode areas. */
  int mcprot;		/* Protection of current mcode area. */
} jit_State;

/* Exit stubs. */
#if LJ_TARGET_X86ORX64
/* Limited by the range of a short fwd jump (127): (2+2)*(32-1)-2 = 122. */
#define EXITSTUB_SPACING	(2+2)
#define EXITSTUBS_PER_GROUP	32
#else
#error "Missing CPU-specific exit stub definitions"
#endif

/* Return the address of an exit stub. */
static LJ_AINLINE MCode *exitstub_addr(jit_State *J, ExitNo exitno)
{
  lua_assert(J->exitstubgroup[exitno / EXITSTUBS_PER_GROUP] != NULL);
  return J->exitstubgroup[exitno / EXITSTUBS_PER_GROUP] +
	 EXITSTUB_SPACING*(exitno % EXITSTUBS_PER_GROUP);
}

#endif
