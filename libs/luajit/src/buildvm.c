/*
** LuaJIT VM builder.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
**
** This is a tool to build the hand-tuned assembler code required for
** LuaJIT's bytecode interpreter. It supports a variety of output formats
** to feed different toolchains (see usage() below).
**
** This tool is not particularly optimized because it's only used while
** _building_ LuaJIT. There's no point in distributing or installing it.
** Only the object code generated by this tool is linked into LuaJIT.
**
** Caveat: some memory is not free'd, error handling is lazy.
** It's a one-shot tool -- any effort fixing this would be wasted.
*/

#include "lua.h"
#include "luajit.h"

#ifdef LUA_USE_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_frame.h"
#include "lj_dispatch.h"
#include "lj_target.h"

#include "buildvm.h"

/* ------------------------------------------------------------------------ */

/* DynASM glue definitions. */
#define Dst		ctx
#define Dst_DECL	BuildCtx *ctx
#define Dst_REF		(ctx->D)

#include "../dynasm/dasm_proto.h"

/* Glue macros for DynASM. */
#define DASM_M_GROW(ctx, t, p, sz, need) \
  do { \
    size_t _sz = (sz), _need = (need); \
    if (_sz < _need) { \
      if (_sz < 16) _sz = 16; \
      while (_sz < _need) _sz += _sz; \
      (p) = (t *)realloc((p), _sz); \
      if ((p) == NULL) exit(1); \
      (sz) = _sz; \
    } \
  } while(0)

#define DASM_M_FREE(ctx, p, sz)	free(p)

static int collect_reloc(BuildCtx *ctx, uint8_t *addr, int idx, int type);

#define DASM_EXTERN(ctx, addr, idx, type) \
  collect_reloc(ctx, addr, idx, type)

/* ------------------------------------------------------------------------ */

/* Avoid trouble if cross-compiling for an x86 target. Speed doesn't matter. */
#define DASM_ALIGNED_WRITES	1

/* Embed architecture-specific DynASM encoder and backend. */
#if LJ_TARGET_X86
#include "../dynasm/dasm_x86.h"
#include "buildvm_x86.h"
#else
#error "No support for this architecture (yet)"
#endif

/* ------------------------------------------------------------------------ */

void owrite(BuildCtx *ctx, const void *ptr, size_t sz)
{
  if (fwrite(ptr, 1, sz, ctx->fp) != sz) {
    fprintf(stderr, "Error: cannot write to output file: %s\n",
	    strerror(errno));
    exit(1);
  }
}

/* ------------------------------------------------------------------------ */

/* Emit code as raw bytes. Only used for DynASM debugging. */
static void emit_raw(BuildCtx *ctx)
{
  owrite(ctx, ctx->code, ctx->codesz);
}

/* -- Build machine code -------------------------------------------------- */

/* Collect external relocations. */
static int collect_reloc(BuildCtx *ctx, uint8_t *addr, int idx, int type)
{
  if (ctx->nreloc >= BUILD_MAX_RELOC) {
    fprintf(stderr, "Error: too many relocations, increase BUILD_MAX_RELOC.\n");
    exit(1);
  }
  ctx->reloc[ctx->nreloc].ofs = (int32_t)(addr - ctx->code);
  ctx->reloc[ctx->nreloc].sym = idx;
  ctx->reloc[ctx->nreloc].type = type;
  ctx->nreloc++;
  return 0;  /* Encode symbol offset of 0. */
}

/* Naive insertion sort. Performance doesn't matter here. */
static void perm_insert(int *perm, int32_t *ofs, int i)
{
  perm[i] = i;
  while (i > 0) {
    int a = perm[i-1];
    int b = perm[i];
    if (ofs[a] <= ofs[b]) break;
    perm[i] = a;
    perm[i-1] = b;
    i--;
  }
}

/* Build the machine code. */
static int build_code(BuildCtx *ctx)
{
  int status;
  int i, j;

  /* Initialize DynASM structures. */
  ctx->nglob = GLOB__MAX;
  ctx->glob = (void **)malloc(ctx->nglob*sizeof(void *));
  memset(ctx->glob, 0, ctx->nglob*sizeof(void *));
  ctx->nreloc = 0;

  ctx->extnames = extnames;
  ctx->globnames = globnames;

  ctx->dasm_ident = DASM_IDENT;
  ctx->dasm_arch = DASM_ARCH;

  dasm_init(Dst, DASM_MAXSECTION);
  dasm_setupglobal(Dst, ctx->glob, ctx->nglob);
  dasm_setup(Dst, build_actionlist);

  /* Call arch-specific backend to emit the code. */
  ctx->npc = build_backend(ctx);

  /* Finalize the code. */
  (void)dasm_checkstep(Dst, DASM_SECTION_CODE);
  if ((status = dasm_link(Dst, &ctx->codesz))) return status;
  ctx->code = (uint8_t *)malloc(ctx->codesz);
  if ((status = dasm_encode(Dst, (void *)ctx->code))) return status;

  /* Allocate the symbol offset and permutation tables. */
  ctx->nsym = ctx->npc + ctx->nglob;
  ctx->perm = (int *)malloc((ctx->nsym+1)*sizeof(int *));
  ctx->sym_ofs = (int32_t *)malloc((ctx->nsym+1)*sizeof(int32_t));

  /* Collect the opcodes (PC labels). */
  for (i = 0; i < ctx->npc; i++) {
    int32_t n = dasm_getpclabel(Dst, i);
    if (n < 0) return 0x22000000|i;
    ctx->sym_ofs[i] = n;
    perm_insert(ctx->perm, ctx->sym_ofs, i);
  }

  /* Collect the globals (named labels). */
  for (j = 0; j < ctx->nglob; j++, i++) {
    const char *gl = globnames[j];
    int len = (int)strlen(gl);
    if (!ctx->glob[j]) {
      fprintf(stderr, "Error: undefined global %s\n", gl);
      exit(2);
    }
    if (len >= 2 && gl[len-2] == '_' && gl[len-1] == 'Z')
      ctx->sym_ofs[i] = -1;  /* Skip the _Z symbols. */
    else
      ctx->sym_ofs[i] = (int32_t)((uint8_t *)(ctx->glob[j]) - ctx->code);
    perm_insert(ctx->perm, ctx->sym_ofs, i);
  }

  /* Close the address range. */
  ctx->sym_ofs[i] = (int32_t)ctx->codesz;
  perm_insert(ctx->perm, ctx->sym_ofs, i);

  dasm_free(Dst);

  return 0;
}

/* -- Generate VM enums --------------------------------------------------- */

const char *const bc_names[] = {
#define BCNAME(name, ma, mb, mc, mt)       #name,
BCDEF(BCNAME)
#undef BCNAME
  NULL
};

const char *const ir_names[] = {
#define IRNAME(name, m, m1, m2)	#name,
IRDEF(IRNAME)
#undef IRNAME
  NULL
};

const char *const irfpm_names[] = {
#define FPMNAME(name)		#name,
IRFPMDEF(FPMNAME)
#undef FPMNAME
  NULL
};

const char *const irfield_names[] = {
#define FLNAME(name, type, field)	#name,
IRFLDEF(FLNAME)
#undef FLNAME
  NULL
};

static const char *const trace_errors[] = {
#define TREDEF(name, msg)	msg,
#include "lj_traceerr.h"
  NULL
};

static const char *lower(char *buf, const char *s)
{
  char *p = buf;
  while (*s) {
    *p++ = (*s >= 'A' && *s <= 'Z') ? *s+0x20 : *s;
    s++;
  }
  *p = '\0';
  return buf;
}

/* Emit VM definitions as Lua code for debug modules. */
static void emit_vmdef(BuildCtx *ctx)
{
  char buf[80];
  int i;
  fprintf(ctx->fp, "-- This is a generated file. DO NOT EDIT!\n\n");
  fprintf(ctx->fp, "module(...)\n\n");

  fprintf(ctx->fp, "bcnames = \"");
  for (i = 0; bc_names[i]; i++) fprintf(ctx->fp, "%-6s", bc_names[i]);
  fprintf(ctx->fp, "\"\n\n");

  fprintf(ctx->fp, "irnames = \"");
  for (i = 0; ir_names[i]; i++) fprintf(ctx->fp, "%-6s", ir_names[i]);
  fprintf(ctx->fp, "\"\n\n");

  fprintf(ctx->fp, "irfpm = { [0]=");
  for (i = 0; irfpm_names[i]; i++)
    fprintf(ctx->fp, "\"%s\", ", lower(buf, irfpm_names[i]));
  fprintf(ctx->fp, "}\n\n");

  fprintf(ctx->fp, "irfield = { [0]=");
  for (i = 0; irfield_names[i]; i++) {
    char *p;
    lower(buf, irfield_names[i]);
    p = strchr(buf, '_');
    if (p) *p = '.';
    fprintf(ctx->fp, "\"%s\", ", buf);
  }
  fprintf(ctx->fp, "}\n\n");

  fprintf(ctx->fp, "traceerr = {\n[0]=");
  for (i = 0; trace_errors[i]; i++)
    fprintf(ctx->fp, "\"%s\",\n", trace_errors[i]);
  fprintf(ctx->fp, "}\n\n");
}

/* -- Argument parsing ---------------------------------------------------- */

/* Build mode names. */
static const char *const modenames[] = {
#define BUILDNAME(name)		#name,
BUILDDEF(BUILDNAME)
#undef BUILDNAME
  NULL
};

/* Print usage information and exit. */
static void usage(void)
{
  int i;
  fprintf(stderr, LUAJIT_VERSION " VM builder.\n");
  fprintf(stderr, LUAJIT_COPYRIGHT ", " LUAJIT_URL "\n");
  fprintf(stderr, "Target architecture: " LJ_ARCH_NAME "\n\n");
  fprintf(stderr, "Usage: buildvm -m mode [-o outfile] [infiles...]\n\n");
  fprintf(stderr, "Available modes:\n");
  for (i = 0; i < BUILD__MAX; i++)
    fprintf(stderr, "  %s\n", modenames[i]);
  exit(1);
}

/* Parse the output mode name. */
static BuildMode parsemode(const char *mode)
{
  int i;
  for (i = 0; modenames[i]; i++)
    if (!strcmp(mode, modenames[i]))
      return (BuildMode)i;
  usage();
  return (BuildMode)-1;
}

/* Parse arguments. */
static void parseargs(BuildCtx *ctx, char **argv)
{
  const char *a;
  int i;
  ctx->mode = (BuildMode)-1;
  ctx->outname = "-";
  for (i = 1; (a = argv[i]) != NULL; i++) {
    if (a[0] != '-')
      break;
    switch (a[1]) {
    case '-':
      if (a[2]) goto err;
      i++;
      goto ok;
    case '\0':
      goto ok;
    case 'm':
      i++;
      if (a[2] || argv[i] == NULL) goto err;
      ctx->mode = parsemode(argv[i]);
      break;
    case 'o':
      i++;
      if (a[2] || argv[i] == NULL) goto err;
      ctx->outname = argv[i];
      break;
    default: err:
      usage();
      break;
    }
  }
ok:
  ctx->args = argv+i;
  if (ctx->mode == (BuildMode)-1) goto err;
}

int main(int argc, char **argv)
{
  BuildCtx ctx_;
  BuildCtx *ctx = &ctx_;
  int status, binmode;

  UNUSED(argc);
  parseargs(ctx, argv);

  if ((status = build_code(ctx))) {
    fprintf(stderr,"Error: DASM error %08x\n", status);
    return 1;
  }

  switch (ctx->mode) {
#if LJ_TARGET_X86ORX64
  case BUILD_peobj:
#endif
  case BUILD_raw:
    binmode = 1;
    break;
  default:
    binmode = 0;
    break;
  }

  if (ctx->outname[0] == '-' && ctx->outname[1] == '\0') {
    ctx->fp = stdout;
#ifdef LUA_USE_WIN
    if (binmode)
      _setmode(_fileno(stdout), _O_BINARY);  /* Yuck. */
#endif
  } else if (!(ctx->fp = fopen(ctx->outname, binmode ? "wb" : "w"))) {
    fprintf(stderr, "Error: cannot open output file '%s': %s\n",
	    ctx->outname, strerror(errno));
    exit(1);
  }

  switch (ctx->mode) {
  case BUILD_asm:
#if defined(__ELF__)
    ctx->mode = BUILD_elfasm;
#elif defined(__MACH__)
    ctx->mode = BUILD_machasm;
#else
    fprintf(stderr,"Error: auto-guessing the system assembler failed\n");
    return 1;
#endif
    /* fallthrough */
  case BUILD_elfasm:
  case BUILD_coffasm:
  case BUILD_machasm:
    emit_asm(ctx);
    emit_asm_debug(ctx);
    break;
#if LJ_TARGET_X86ORX64
  case BUILD_peobj:
    emit_peobj(ctx);
    break;
#endif
  case BUILD_raw:
    emit_raw(ctx);
    break;
  case BUILD_vmdef:
    emit_vmdef(ctx);
    /* fallthrough */
  case BUILD_ffdef:
  case BUILD_libdef:
  case BUILD_recdef:
    emit_lib(ctx);
    break;
  case BUILD_folddef:
    emit_fold(ctx);
    break;
  default:
    break;
  }

  fflush(ctx->fp);
  if (ferror(ctx->fp)) {
    fprintf(stderr, "Error: cannot write to output file: %s\n",
	    strerror(errno));
    exit(1);
  }
  fclose(ctx->fp);

  return 0;
}

