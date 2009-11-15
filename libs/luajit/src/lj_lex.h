/*
** Lexical analyzer.
** Major parts taken verbatim from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#ifndef _LJ_LEX_H
#define _LJ_LEX_H

#include <stdarg.h>

#include "lj_obj.h"
#include "lj_err.h"

/* Lua lexer tokens. */
#define TKDEF(_, __) \
  _(and) _(break) _(do) _(else) _(elseif) _(end) _(false) \
  _(for) _(function) _(if) _(in) _(local) _(nil) _(not) _(or) \
  _(repeat) _(return) _(then) _(true) _(until) _(while) \
  __(concat, ..) __(dots, ...) __(eq, ==) __(ge, >=) __(le, <=) __(ne, ~=) \
  __(number, <number>) __(name, <name>) __(string, <string>) __(eof, <eof>)

enum {
  TK_OFS = 256,
#define TKENUM1(name)		TK_##name,
#define TKENUM2(name, sym)	TK_##name,
TKDEF(TKENUM1, TKENUM2)
#undef TKENUM1
#undef TKENUM2
  TK_RESERVED = TK_while - TK_OFS
};

typedef int LexToken;

/* Lua lexer state. */
typedef struct LexState {
  struct FuncState *fs;	/* Current FuncState. Defined in lj_parse.c. */
  struct lua_State *L;	/* Lua state. */
  TValue tokenval;	/* Current token value. */
  TValue lookaheadval;	/* Lookahead token value. */
  int current;		/* Current character (charint). */
  LexToken token;	/* Current token. */
  LexToken lookahead;	/* Lookahead token. */
  SBuf sb;		/* String buffer for tokens. */
  const char *p;	/* Current position in input buffer. */
  MSize n;		/* Bytes left in input buffer. */
  lua_Reader rfunc;	/* Reader callback. */
  void *rdata;		/* Reader callback data. */
  BCLine linenumber;	/* Input line counter. */
  BCLine lastline;	/* Line of last token. */
  GCstr *chunkname;	/* Current chunk name (interned string). */
  const char *chunkarg;	/* Chunk name argument. */
  uint32_t level;	/* Syntactical nesting level. */
} LexState;

LJ_FUNC void lj_lex_start(lua_State *L, LexState *ls);
LJ_FUNC void lj_lex_next(LexState *ls);
LJ_FUNC LexToken lj_lex_lookahead(LexState *ls);
LJ_FUNC const char *lj_lex_token2str(LexState *ls, LexToken token);
LJ_FUNC_NORET void lj_lex_error(LexState *ls, LexToken token, ErrMsg em, ...);
LJ_FUNC void lj_lex_init(lua_State *L);

#endif
