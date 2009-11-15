/*
** Trace management.
** Copyright (C) 2005-2009 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TRACE_H
#define _LJ_TRACE_H

#if LJ_HASJIT

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"

/* Trace errors. */
typedef enum {
#define TREDEF(name, msg)	LJ_TRERR_##name,
#include "lj_traceerr.h"
  LJ_TRERR__MAX
} TraceError;

LJ_FUNC_NORET void lj_trace_err(jit_State *J, TraceError e);
LJ_FUNC_NORET void lj_trace_err_info(jit_State *J, TraceError e);

/* Trace management. */
LJ_FUNC void lj_trace_freeproto(global_State *g, GCproto *pt);
LJ_FUNC void lj_trace_reenableproto(GCproto *pt);
LJ_FUNC void lj_trace_flushproto(global_State *g, GCproto *pt);
LJ_FUNC int lj_trace_flush(jit_State *J, TraceNo traceno);
LJ_FUNC int lj_trace_flushall(lua_State *L);
LJ_FUNC void lj_trace_freestate(global_State *g);

/* Event handling. */
LJ_FUNC void lj_trace_ins(jit_State *J);
LJ_FUNCA void lj_trace_hot(jit_State *J, const BCIns *pc);
LJ_FUNCA void *lj_trace_exit(jit_State *J, void *exptr);

/* Signal asynchronous abort of trace or end of trace. */
#define lj_trace_abort(g)	(G2J(g)->state &= ~LJ_TRACE_ACTIVE)
#define lj_trace_end(J)		(J->state = LJ_TRACE_END)

#else

#define lj_trace_flushall(L)	(UNUSED(L), 0)
#define lj_trace_freestate(g)	UNUSED(g)
#define lj_trace_freeproto(g, pt)  (UNUSED(g), UNUSED(pt), (void)0)
#define lj_trace_abort(g)	UNUSED(g)
#define lj_trace_end(J)		UNUSED(J)

#endif

#endif
