/*

   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.
 
   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 
  You should have received a copy of the GNU General Public License  
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
  
*/

#include "env_context.h"

#include "files_names.h"
#include "logic_expressions.h"
#include "syntax.h"
#include "item_lib.h"
#include "ornaments.h"
#include "expand.h"
#include "matching.h"
#include "string_lib.h"
#include "misc_lib.h"
#include "assoc.h"
#include "scope.h"
#include "vars.h"
#include "syslog_client.h"
#include "audit.h"
#include "logging.h"
#include "logging_old.h"
#include "promise_logging.h"
#include "rlist.h"

#ifdef HAVE_NOVA
#include "cf.nova.h"
#endif

#include <assert.h>

/*****************************************************************************/

static bool EvalContextStackFrameContainsNegated(const EvalContext *ctx, const char *context);

static bool ABORTBUNDLE = false;

/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

static const char *StackFrameOwnerName(const StackFrame *frame)
{
    switch (frame->type)
    {
    case STACK_FRAME_TYPE_BUNDLE:
        return frame->data.bundle.owner->name;

    case STACK_FRAME_TYPE_BODY:
        return frame->data.body.owner->name;

    case STACK_FRAME_TYPE_PROMISE:
    case STACK_FRAME_TYPE_PROMISE_ITERATION:
        return "this";

    default:
        ProgrammingError("Unhandled stack frame type");
    }
}

static StackFrame *LastStackFrame(const EvalContext *ctx, size_t offset)
{
    if (SeqLength(ctx->stack) <= offset)
    {
        return NULL;
    }
    return SeqAt(ctx->stack, SeqLength(ctx->stack) - 1 - offset);
}

static StackFrame *LastStackFrameBundle(const EvalContext *ctx)
{
    StackFrame *last_frame = LastStackFrame(ctx, 0);

    switch (last_frame->type)
    {
    case STACK_FRAME_TYPE_BUNDLE:
        return last_frame;

    case STACK_FRAME_TYPE_BODY:
        {
            assert(LastStackFrame(ctx, 1));
            assert(LastStackFrame(ctx, 1)->type == STACK_FRAME_TYPE_PROMISE);
            StackFrame *previous_frame = LastStackFrame(ctx, 2);
            if (previous_frame)
            {
                assert(previous_frame->type == STACK_FRAME_TYPE_BUNDLE);
                return previous_frame;
            }
            else
            {
                return NULL;
            }
        }

    case STACK_FRAME_TYPE_PROMISE:
        {
            StackFrame *previous_frame = LastStackFrame(ctx, 1);
            assert(previous_frame);
            assert("Promise stack frame does not follow bundle stack frame" && previous_frame->type == STACK_FRAME_TYPE_BUNDLE);
            return previous_frame;
        }

    case STACK_FRAME_TYPE_PROMISE_ITERATION:
        {
            StackFrame *previous_frame = LastStackFrame(ctx, 2);
            assert(previous_frame);
            assert("Promise stack frame does not follow bundle stack frame" && previous_frame->type == STACK_FRAME_TYPE_BUNDLE);
            return previous_frame;
        }

    default:
        ProgrammingError("Unhandled stack frame type");
    }
}

void EvalContextHeapAddSoft(EvalContext *ctx, const char *context, const char *ns)
{
    char context_copy[CF_MAXVARSIZE];
    char canonified_context[CF_MAXVARSIZE];

    strcpy(canonified_context, context);
    if (Chop(canonified_context, CF_EXPANDSIZE) == -1)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Chop was called on a string that seemed to have no terminator");
    }
    CanonifyNameInPlace(canonified_context);
    
    if (ns && strcmp(ns, "default") != 0)
    {
        snprintf(context_copy, CF_MAXVARSIZE, "%s:%s", ns, canonified_context);
    }
    else
    {
        strncpy(context_copy, canonified_context, CF_MAXVARSIZE);
    }
    
    CfDebug("EvalContextHeapAddSoft(%s)\n", context_copy);

    if (strlen(context_copy) == 0)
    {
        return;
    }

    if (IsRegexItemIn(ctx, ctx->heap_abort_current_bundle, context_copy))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Bundle aborted on defined class \"%s\"\n", context_copy);
        ABORTBUNDLE = true;
    }

    if (IsRegexItemIn(ctx, ctx->heap_abort, context_copy))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "cf-agent aborted on defined class \"%s\"\n", context_copy);
        exit(1);
    }

    if (EvalContextHeapContainsSoft(ctx, context_copy))
    {
        return;
    }

    StringSetAdd(ctx->heap_soft, xstrdup(context_copy));

    for (const Item *ip = ctx->heap_abort; ip != NULL; ip = ip->next)
    {
        if (IsDefinedClass(ctx, ip->name, ns))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", "cf-agent aborted on defined class \"%s\" defined in bundle %s\n", ip->name, StackFrameOwnerName(LastStackFrame(ctx, 0)));
            exit(1);
        }
    }

    if (!ABORTBUNDLE)
    {
        for (const Item *ip = ctx->heap_abort_current_bundle; ip != NULL; ip = ip->next)
        {
            if (IsDefinedClass(ctx, ip->name, ns))
            {
                CfOut(OUTPUT_LEVEL_ERROR, "", " -> Setting abort for \"%s\" when setting \"%s\"", ip->name, context_copy);
                ABORTBUNDLE = true;
                break;
            }
        }
    }
}

/*******************************************************************/

void EvalContextHeapAddHard(EvalContext *ctx, const char *context)
{
    char context_copy[CF_MAXVARSIZE];

    strcpy(context_copy, context);
    if (Chop(context_copy, CF_EXPANDSIZE) == -1)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Chop was called on a string that seemed to have no terminator");
    }
    CanonifyNameInPlace(context_copy);

    CfDebug("EvalContextHeapAddHard(%s)\n", context_copy);

    if (strlen(context_copy) == 0)
    {
        return;
    }

    if (IsRegexItemIn(ctx, ctx->heap_abort_current_bundle, context_copy))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Bundle aborted on defined class \"%s\"\n", context_copy);
        ABORTBUNDLE = true;
    }

    if (IsRegexItemIn(ctx, ctx->heap_abort, context_copy))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "cf-agent aborted on defined class \"%s\"\n", context_copy);
        exit(1);
    }

    if (EvalContextHeapContainsHard(ctx, context_copy))
    {
        return;
    }

    StringSetAdd(ctx->heap_hard, xstrdup(context_copy));

    for (const Item *ip = ctx->heap_abort; ip != NULL; ip = ip->next)
    {
        if (IsDefinedClass(ctx, ip->name, NULL))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", "cf-agent aborted on defined class \"%s\" defined in bundle %s\n", ip->name, StackFrameOwnerName(LastStackFrame(ctx, 0)));
            exit(1);
        }
    }

    if (!ABORTBUNDLE)
    {
        for (const Item *ip = ctx->heap_abort_current_bundle; ip != NULL; ip = ip->next)
        {
            if (IsDefinedClass(ctx, ip->name, NULL))
            {
                CfOut(OUTPUT_LEVEL_ERROR, "", " -> Setting abort for \"%s\" when setting \"%s\"", ip->name, context_copy);
                ABORTBUNDLE = true;
                break;
            }
        }
    }
}

void EvalContextStackFrameAddSoft(EvalContext *ctx, const char *context)
{
    assert(SeqLength(ctx->stack) > 0);

    StackFrameBundle frame;
    {
        StackFrame *last_frame = LastStackFrameBundle(ctx);
        if (!last_frame)
        {
            ProgrammingError("Attempted to add a soft class on the stack, but stack had no bundle frame");
        }
        frame = last_frame->data.bundle;
    }

    char copy[CF_BUFSIZE];
    if (strcmp(frame.owner->ns, "default") != 0)
    {
         snprintf(copy, CF_MAXVARSIZE, "%s:%s", frame.owner->ns, context);
    }
    else
    {
         strncpy(copy, context, CF_MAXVARSIZE);
    }

    if (Chop(copy, CF_EXPANDSIZE) == -1)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Chop was called on a string that seemed to have no terminator");
    }

    if (strlen(copy) == 0)
    {
        return;
    }

    CfDebug("NewBundleClass(%s)\n", copy);
    
    if (IsRegexItemIn(ctx, ctx->heap_abort_current_bundle, copy))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Bundle %s aborted on defined class \"%s\"\n", frame.owner->name, copy);
        ABORTBUNDLE = true;
    }

    if (IsRegexItemIn(ctx, ctx->heap_abort, copy))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "cf-agent aborted on defined class \"%s\" defined in bundle %s\n", copy, frame.owner->name);
        exit(1);
    }

    if (EvalContextHeapContainsSoft(ctx, copy))
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "WARNING - private class \"%s\" in bundle \"%s\" shadows a global class - you should choose a different name to avoid conflicts",
              copy, frame.owner->name);
    }

    if (EvalContextStackFrameContainsSoft(ctx, copy))
    {
        return;
    }

    StringSetAdd(frame.contexts, xstrdup(copy));

    for (const Item *ip = ctx->heap_abort; ip != NULL; ip = ip->next)
    {
        if (IsDefinedClass(ctx, ip->name, frame.owner->ns))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", "cf-agent aborted on defined class \"%s\" defined in bundle %s\n", copy, frame.owner->name);
            exit(1);
        }
    }

    if (!ABORTBUNDLE)
    {
        for (const Item *ip = ctx->heap_abort_current_bundle; ip != NULL; ip = ip->next)
        {
            if (IsDefinedClass(ctx, ip->name, frame.owner->ns))
            {
                CfOut(OUTPUT_LEVEL_ERROR, "", " -> Setting abort for \"%s\" when setting \"%s\"", ip->name, context);
                ABORTBUNDLE = true;
                break;
            }
        }
    }
}

typedef struct
{
    const EvalContext *ctx;
    const char *ns;
} EvalTokenAsClassContext;

static ExpressionValue EvalTokenAsClass(const char *classname, void *param)
{
    const EvalContext *ctx = ((EvalTokenAsClassContext *)param)->ctx;
    const char *ns = ((EvalTokenAsClassContext *)param)->ns;

    char qualified_class[CF_MAXVARSIZE];

    if (strcmp(classname, "any") == 0)
       {
       return true;
       }
    
    if (strchr(classname, ':'))
    {
        if (strncmp(classname, "default:", strlen("default:")) == 0)
        {
            snprintf(qualified_class, CF_MAXVARSIZE, "%s", classname + strlen("default:"));
        }
        else
        {
            snprintf(qualified_class, CF_MAXVARSIZE, "%s", classname);
        }
    }
    else if (ns != NULL && strcmp(ns, "default") != 0)
    {
        snprintf(qualified_class, CF_MAXVARSIZE, "%s:%s", ns, (char *)classname);
    }
    else
    {
        snprintf(qualified_class, CF_MAXVARSIZE, "%s", classname);
    }

    if (EvalContextHeapContainsNegated(ctx, qualified_class))
    {
        return false;
    }
    if (EvalContextStackFrameContainsNegated(ctx, qualified_class))
    {
        return false;
    }
    if (EvalContextHeapContainsHard(ctx, classname))  // Hard classes are always unqualified
    {
        return true;
    }
    if (EvalContextHeapContainsSoft(ctx, qualified_class))
    {
        return true;
    }
    if (EvalContextStackFrameContainsSoft(ctx, qualified_class))
    {
        return true;
    }
    return false;
}

/**********************************************************************/

static char *EvalVarRef(ARG_UNUSED const char *varname, ARG_UNUSED VarRefType type, ARG_UNUSED void *param)
{
/*
 * There should be no unexpanded variables when we evaluate any kind of
 * logic expressions, until parsing of logic expression changes and they are
 * not pre-expanded before evaluation.
 */
    return NULL;
}

/**********************************************************************/

bool IsDefinedClass(const EvalContext *ctx, const char *context, const char *ns)
{
    ParseResult res;

    if (!context)
    {
        return true;
    }

    res = ParseExpression(context, 0, strlen(context));

    if (!res.result)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Unable to parse class expression: %s", context);
        return false;
    }
    else
    {
        EvalTokenAsClassContext etacc = {
            .ctx = ctx,
            .ns = ns
        };

        ExpressionValue r = EvalExpression(res.result,
                                           &EvalTokenAsClass, &EvalVarRef,
                                           &etacc);

        FreeExpression(res.result);

        CfDebug("Evaluate(%s) -> %d\n", context, r);

        /* r is EvalResult which could be ERROR */
        return r == true;
    }
}

/**********************************************************************/

static ExpressionValue EvalTokenFromList(const char *token, void *param)
{
    StringSet *set = param;
    return StringSetContains(set, token);
}

/**********************************************************************/

static bool EvalWithTokenFromList(const char *expr, StringSet *token_set)
{
    ParseResult res = ParseExpression(expr, 0, strlen(expr));

    if (!res.result)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "Syntax error in expression: %s", expr);
        return false;           /* FIXME: return error */
    }
    else
    {
        ExpressionValue r = EvalExpression(res.result,
                                           &EvalTokenFromList,
                                           &EvalVarRef,
                                           token_set);

        FreeExpression(res.result);

        /* r is EvalResult which could be ERROR */
        return r == true;
    }
}

/**********************************************************************/

/* Process result expression */

bool EvalProcessResult(const char *process_result, StringSet *proc_attr)
{
    return EvalWithTokenFromList(process_result, proc_attr);
}

/**********************************************************************/

/* File result expressions */

bool EvalFileResult(const char *file_result, StringSet *leaf_attr)
{
    return EvalWithTokenFromList(file_result, leaf_attr);
}

/*****************************************************************************/

void EvalContextHeapPersistentSave(const char *context, const char *ns, unsigned int ttl_minutes, ContextStatePolicy policy)
{
    CF_DB *dbp;
    CfState state;
    time_t now = time(NULL);
    char name[CF_BUFSIZE];

    if (!OpenDB(&dbp, dbid_state))
    {
        return;
    }

    snprintf(name, CF_BUFSIZE, "%s%c%s", ns, CF_NS, context);
    
    if (ReadDB(dbp, name, &state, sizeof(state)))
    {
        if (state.policy == CONTEXT_STATE_POLICY_PRESERVE)
        {
            if (now < state.expires)
            {
                CfOut(OUTPUT_LEVEL_VERBOSE, "", " -> Persisent state %s is already in a preserved state --  %jd minutes to go\n",
                      name, (intmax_t)((state.expires - now) / 60));
                CloseDB(dbp);
                return;
            }
        }
    }
    else
    {
        CfOut(OUTPUT_LEVEL_VERBOSE, "", " -> New persistent state %s\n", name);
    }

    state.expires = now + ttl_minutes * 60;
    state.policy = policy;

    WriteDB(dbp, name, &state, sizeof(state));
    CloseDB(dbp);
}

/*****************************************************************************/

void EvalContextHeapPersistentRemove(const char *context)
{
    CF_DB *dbp;

    if (!OpenDB(&dbp, dbid_state))
    {
        return;
    }

    DeleteDB(dbp, context);
    CfDebug("Deleted any persistent state %s\n", context);
    CloseDB(dbp);
}

/*****************************************************************************/

void EvalContextHeapPersistentLoadAll(EvalContext *ctx)
{
    CF_DB *dbp;
    CF_DBC *dbcp;
    int ksize, vsize;
    char *key;
    void *value;
    time_t now = time(NULL);
    CfState q;

    if (LOOKUP)
    {
        return;
    }

    Banner("Loading persistent classes");

    if (!OpenDB(&dbp, dbid_state))
    {
        return;
    }

/* Acquire a cursor for the database. */

    if (!NewDBCursor(dbp, &dbcp))
    {
        CfOut(OUTPUT_LEVEL_INFORM, "", " !! Unable to scan persistence cache");
        return;
    }

    while (NextDB(dbcp, &key, &ksize, &value, &vsize))
    {
        memcpy((void *) &q, value, sizeof(CfState));

        CfDebug(" - Found key %s...\n", key);

        if (now > q.expires)
        {
            CfOut(OUTPUT_LEVEL_VERBOSE, "", " Persistent class %s expired\n", key);
            DBCursorDeleteEntry(dbcp);
        }
        else
        {
            CfOut(OUTPUT_LEVEL_VERBOSE, "", " Persistent class %s for %jd more minutes\n", key, (intmax_t)((q.expires - now) / 60));
            CfOut(OUTPUT_LEVEL_VERBOSE, "", " Adding persistent class %s to heap\n", key);
            if (strchr(key, CF_NS))
               {
               char ns[CF_MAXVARSIZE], name[CF_MAXVARSIZE];
               ns[0] = '\0';
               name[0] = '\0';
               sscanf(key, "%[^:]:%[^\n]", ns, name);
               EvalContextHeapAddSoft(ctx, name, ns);
               }
            else
               {
               EvalContextHeapAddSoft(ctx, key, NULL);
               }
        }
    }

    DeleteDBCursor(dbcp);
    CloseDB(dbp);

    Banner("Loaded persistent memory");
}

/***************************************************************************/

int Abort()
{
    if (ABORTBUNDLE)
    {
        ABORTBUNDLE = false;
        return true;
    }

    return false;
}

/*****************************************************************************/

int VarClassExcluded(EvalContext *ctx, Promise *pp, char **classes)
{
    Constraint *cp = PromiseGetConstraint(ctx, pp, "ifvarclass");

    if (cp == NULL)
    {
        return false;
    }

    *classes = (char *) ConstraintGetRvalValue(ctx, "ifvarclass", pp, RVAL_TYPE_SCALAR);

    if (*classes == NULL)
    {
        return true;
    }

    if (strchr(*classes, '$') || strchr(*classes, '@'))
    {
        CfDebug("Class expression did not evaluate");
        return true;
    }

    if (*classes && IsDefinedClass(ctx, *classes, PromiseGetNamespace(pp)))
    {
        return false;
    }
    else
    {
        return true;
    }
}

void EvalContextHeapAddAbort(EvalContext *ctx, const char *context, const char *activated_on_context)
{
    if (!IsItemIn(ctx->heap_abort, context))
    {
        AppendItem(&ctx->heap_abort, context, activated_on_context);
    }
}

void EvalContextHeapAddAbortCurrentBundle(EvalContext *ctx, const char *context, const char *activated_on_context)
{
    if (!IsItemIn(ctx->heap_abort_current_bundle, context))
    {
        AppendItem(&ctx->heap_abort_current_bundle, context, activated_on_context);
    }
}

/*****************************************************************************/

void MarkPromiseHandleDone(EvalContext *ctx, const Promise *pp)
{
    char name[CF_BUFSIZE];
    const char *handle = PromiseGetHandle(pp);

    if (handle == NULL)
    {
       return;
    }

    snprintf(name, CF_BUFSIZE, "%s:%s", PromiseGetNamespace(pp), handle);
    StringSetAdd(ctx->dependency_handles, xstrdup(name));
}

/*****************************************************************************/

int MissingDependencies(EvalContext *ctx, const Promise *pp)
{
    if (pp == NULL)
    {
        return false;
    }

    char name[CF_BUFSIZE], *d;
    Rlist *rp, *deps = PromiseGetConstraintAsList(ctx, "depends_on", pp);
    
    for (rp = deps; rp != NULL; rp = rp->next)
    {
        if (strchr(rp->item, ':'))
        {
            d = (char *)rp->item;
        }
        else
        {
            snprintf(name, CF_BUFSIZE, "%s:%s", PromiseGetNamespace(pp), (char *)rp->item);
            d = name;
        }

        if (!StringSetContains(ctx->dependency_handles, d))
        {
            CfOut(OUTPUT_LEVEL_VERBOSE, "", "\n");
            CfOut(OUTPUT_LEVEL_VERBOSE, "", ". . . . . . . . . . . . . . . . . . . . . . . . . . . . \n");
            CfOut(OUTPUT_LEVEL_VERBOSE, "", "Skipping whole next promise (%s), as promise dependency %s has not yet been kept\n", pp->promiser, d);
            CfOut(OUTPUT_LEVEL_VERBOSE, "", ". . . . . . . . . . . . . . . . . . . . . . . . . . . . \n");

            return true;
        }
    }

    return false;
}

static void StackFrameBundleDestroy(StackFrameBundle frame)
{
    StringSetDestroy(frame.contexts);
    StringSetDestroy(frame.contexts_negated);
}

static void StackFrameBodyDestroy(ARG_UNUSED StackFrameBody frame)
{
    return;
}

static void StackFramePromiseDestroy(StackFramePromise frame)
{
    HashFree(frame.variables);
}

static void StackFramePromiseIterationDestroy(ARG_UNUSED StackFramePromiseIteration frame)
{
    return;
}

static void StackFrameDestroy(StackFrame *frame)
{
    if (frame)
    {
        switch (frame->type)
        {
        case STACK_FRAME_TYPE_BUNDLE:
            StackFrameBundleDestroy(frame->data.bundle);
            break;

        case STACK_FRAME_TYPE_BODY:
            StackFrameBodyDestroy(frame->data.body);
            break;

        case STACK_FRAME_TYPE_PROMISE:
            StackFramePromiseDestroy(frame->data.promise);
            break;

        case STACK_FRAME_TYPE_PROMISE_ITERATION:
            StackFramePromiseIterationDestroy(frame->data.promise_iteration);
            break;

        default:
            ProgrammingError("Unhandled stack frame type");
        }


    }
}

static unsigned PointerHashFn(const void *p, unsigned int max)
{
    return ((unsigned)(uintptr_t)p) % max;
}

static bool PointerEqualFn(const void *key1, const void *key2)
{
    return key1 == key2;
}

TYPED_SET_DEFINE(Promise, const Promise *, &PointerHashFn, &PointerEqualFn, NULL)

EvalContext *EvalContextNew(void)
{
    EvalContext *ctx = xmalloc(sizeof(EvalContext));

    ctx->heap_soft = StringSetNew();
    ctx->heap_hard = StringSetNew();
    ctx->heap_negated = StringSetNew();
    ctx->heap_abort = NULL;
    ctx->heap_abort_current_bundle = NULL;

    ctx->stack = SeqNew(10, StackFrameDestroy);

    ctx->dependency_handles = StringSetNew();

    ctx->promises_done = PromiseSetNew();

    return ctx;
}

void EvalContextDestroy(EvalContext *ctx)
{
    if (ctx)
    {
        StringSetDestroy(ctx->heap_soft);
        StringSetDestroy(ctx->heap_hard);
        StringSetDestroy(ctx->heap_negated);
        DeleteItemList(ctx->heap_abort);
        DeleteItemList(ctx->heap_abort_current_bundle);

        SeqDestroy(ctx->stack);
        ScopeDeleteAll();

        StringSetDestroy(ctx->dependency_handles);

        PromiseSetDestroy(ctx->promises_done);
    }
}

void EvalContextHeapAddNegated(EvalContext *ctx, const char *context)
{
    StringSetAdd(ctx->heap_negated, xstrdup(context));
}

void EvalContextStackFrameAddNegated(EvalContext *ctx, const char *context)
{
    StackFrame *frame = LastStackFrameBundle(ctx);
    assert(frame);

    StringSetAdd(frame->data.bundle.contexts_negated, xstrdup(context));
}

bool EvalContextHeapContainsSoft(const EvalContext *ctx, const char *context)
{
    return StringSetContains(ctx->heap_soft, context);
}

bool EvalContextHeapContainsHard(const EvalContext *ctx, const char *context)
{
    return StringSetContains(ctx->heap_hard, context);
}

bool EvalContextHeapContainsNegated(const EvalContext *ctx, const char *context)
{
    return StringSetContains(ctx->heap_negated, context);
}

bool StackFrameContainsSoftRecursive(const EvalContext *ctx, const char *context, size_t stack_index)
{
    StackFrame *frame = SeqAt(ctx->stack, stack_index);
    if (frame->type == STACK_FRAME_TYPE_BUNDLE && StringSetContains(frame->data.bundle.contexts, context))
    {
        return true;
    }
    else if (stack_index > 0 && frame->inherits_previous)
    {
        return StackFrameContainsSoftRecursive(ctx, context, stack_index - 1);
    }
    else
    {
        return false;
    }
}

bool EvalContextStackFrameContainsSoft(const EvalContext *ctx, const char *context)
{
    if (SeqLength(ctx->stack) == 0)
    {
        return false;
    }

    size_t stack_index = SeqLength(ctx->stack) - 1;
    return StackFrameContainsSoftRecursive(ctx, context, stack_index);
}

bool StackFrameContainsNegatedRecursive(const EvalContext *ctx, const char *context, size_t stack_index)
{
    StackFrame *frame = SeqAt(ctx->stack, stack_index);
    if (frame->type == STACK_FRAME_TYPE_BUNDLE && StringSetContains(frame->data.bundle.contexts_negated, context))
    {
        return true;
    }
    else if (stack_index > 0 && frame->inherits_previous)
    {
        return StackFrameContainsNegatedRecursive(ctx, context, stack_index - 1);
    }
    else
    {
        return false;
    }
}

static bool EvalContextStackFrameContainsNegated(const EvalContext *ctx, const char *context)
{
    if (SeqLength(ctx->stack) == 0)
    {
        return false;
    }

    size_t stack_index = SeqLength(ctx->stack) - 1;
    return StackFrameContainsNegatedRecursive(ctx, context, stack_index);
}

bool EvalContextHeapRemoveSoft(EvalContext *ctx, const char *context)
{
    return StringSetRemove(ctx->heap_soft, context);
}

bool EvalContextHeapRemoveHard(EvalContext *ctx, const char *context)
{
    return StringSetRemove(ctx->heap_hard, context);
}

void EvalContextHeapClear(EvalContext *ctx)
{
    StringSetClear(ctx->heap_soft);
    StringSetClear(ctx->heap_hard);
    StringSetClear(ctx->heap_negated);
}

static size_t StringSetMatchCount(StringSet *set, const char *regex)
{
    size_t count = 0;
    StringSetIterator it = StringSetIteratorInit(set);
    const char *context = NULL;
    while ((context = SetIteratorNext(&it)))
    {
        // TODO: used FullTextMatch to avoid regressions, investigate whether StringMatch can be used
        if (FullTextMatch(regex, context))
        {
            count++;
        }
    }
    return count;
}

size_t EvalContextHeapMatchCountSoft(const EvalContext *ctx, const char *context_regex)
{
    return StringSetMatchCount(ctx->heap_soft, context_regex);
}

size_t EvalContextHeapMatchCountHard(const EvalContext *ctx, const char *context_regex)
{
    return StringSetMatchCount(ctx->heap_hard, context_regex);
}

size_t EvalContextStackFrameMatchCountSoft(const EvalContext *ctx, const char *context_regex)
{
    if (SeqLength(ctx->stack) == 0)
    {
        return 0;
    }

    const StackFrame *frame = LastStackFrameBundle(ctx);
    assert(frame);

    return StringSetMatchCount(frame->data.bundle.contexts, context_regex);
}

StringSet *StringSetAddAllMatchingIterator(StringSet* base, StringSetIterator it, const char *filter_regex)
{
    const char *element = NULL;
    while ((element = SetIteratorNext(&it)))
    {
        if (StringMatch(filter_regex, element))
        {
            StringSetAdd(base, xstrdup(element));
        }
    }
    return base;
}

StringSet *StringSetAddAllMatching(StringSet* base, const StringSet* filtered, const char *filter_regex)
{
    return StringSetAddAllMatchingIterator(base, StringSetIteratorInit((StringSet*)filtered), filter_regex);
}

StringSet *EvalContextHeapAddMatchingSoft(const EvalContext *ctx, StringSet* base, const char *context_regex)
{
    return StringSetAddAllMatching(base, ctx->heap_soft, context_regex);
}

StringSet *EvalContextHeapAddMatchingHard(const EvalContext *ctx, StringSet* base, const char *context_regex)
{
    return StringSetAddAllMatching(base, ctx->heap_hard, context_regex);
}

StringSet *EvalContextStackFrameAddMatchingSoft(const EvalContext *ctx, StringSet* base, const char *context_regex)
{
    if (SeqLength(ctx->stack) == 0)
    {
        return base;
    }

    return StringSetAddAllMatchingIterator(base, EvalContextStackFrameIteratorSoft(ctx), context_regex);
}

StringSetIterator EvalContextHeapIteratorSoft(const EvalContext *ctx)
{
    return StringSetIteratorInit(ctx->heap_soft);
}

StringSetIterator EvalContextHeapIteratorHard(const EvalContext *ctx)
{
    return StringSetIteratorInit(ctx->heap_hard);
}

StringSetIterator EvalContextHeapIteratorNegated(const EvalContext *ctx)
{
    return StringSetIteratorInit(ctx->heap_negated);
}

static StackFrame *StackFrameNew(StackFrameType type, bool inherit_previous)
{
    StackFrame *frame = xmalloc(sizeof(StackFrame));

    frame->type = type;
    frame->inherits_previous = inherit_previous;

    return frame;
}

static StackFrame *StackFrameNewBundle(const Bundle *owner, bool inherit_previous)
{
    StackFrame *frame = StackFrameNew(STACK_FRAME_TYPE_BUNDLE, inherit_previous);

    frame->data.bundle.owner = owner;
    frame->data.bundle.contexts = StringSetNew();
    frame->data.bundle.contexts_negated = StringSetNew();

    return frame;
}

static StackFrame *StackFrameNewBody(const Body *owner)
{
    StackFrame *frame = StackFrameNew(STACK_FRAME_TYPE_BODY, false);

    frame->data.body.owner = owner;

    return frame;
}

static StackFrame *StackFrameNewPromise(const Promise *owner)
{
    StackFrame *frame = StackFrameNew(STACK_FRAME_TYPE_PROMISE, true);

    frame->data.promise.owner = owner;
    frame->data.promise.variables = HashInit();

    return frame;
}

static StackFrame *StackFrameNewPromiseIteration(const Promise *owner)
{
    StackFrame *frame = StackFrameNew(STACK_FRAME_TYPE_PROMISE_ITERATION, true);

    frame->data.promise_iteration.owner = owner;

    return frame;
}

void EvalContextStackFrameRemoveSoft(EvalContext *ctx, const char *context)
{
    StackFrame *frame = LastStackFrameBundle(ctx);
    assert(frame);

    StringSetRemove(frame->data.bundle.contexts, context);
}

static void EvalContextStackPushFrame(EvalContext *ctx, StackFrame *frame)
{
    SeqAppend(ctx->stack, frame);
}

void EvalContextStackPushBundleFrame(EvalContext *ctx, const Bundle *owner, bool inherits_previous)
{
    assert(!LastStackFrame(ctx, 0) || LastStackFrame(ctx, 0)->type == STACK_FRAME_TYPE_PROMISE_ITERATION);

    EvalContextStackPushFrame(ctx, StackFrameNewBundle(owner, inherits_previous));
    ScopeSetCurrent(owner->name);
}

void EvalContextStackPushBodyFrame(EvalContext *ctx, const Body *owner)
{
    assert((!LastStackFrame(ctx, 0) && strcmp("control", owner->name) == 0) || LastStackFrame(ctx, 0)->type == STACK_FRAME_TYPE_PROMISE);

    EvalContextStackPushFrame(ctx, StackFrameNewBody(owner));
}

void EvalContextStackPushPromiseFrame(EvalContext *ctx, const Promise *owner)
{
    assert(LastStackFrame(ctx, 0) && LastStackFrame(ctx, 0)->type == STACK_FRAME_TYPE_BUNDLE);

    EvalContextStackPushFrame(ctx, StackFrameNewPromise(owner));
    ScopeSetCurrent("this");
}

void EvalContextStackPushPromiseIterationFrame(EvalContext *ctx, const Promise *owner)
{
    assert(LastStackFrame(ctx, 0) && LastStackFrame(ctx, 0)->type == STACK_FRAME_TYPE_PROMISE);

    EvalContextStackPushFrame(ctx, StackFrameNewPromiseIteration(owner));
    ScopeSetCurrent("this");
}

void EvalContextStackPopFrame(EvalContext *ctx)
{
    assert(SeqLength(ctx->stack) > 0);
    SeqRemove(ctx->stack, SeqLength(ctx->stack) - 1);

    StackFrame *last_frame = LastStackFrame(ctx, 0);
    if (last_frame)
    {
        ScopeSetCurrent(StackFrameOwnerName(last_frame));
    }
}

StringSetIterator EvalContextStackFrameIteratorSoft(const EvalContext *ctx)
{
    StackFrame *frame = LastStackFrameBundle(ctx);
    assert(frame);

    return StringSetIteratorInit(frame->data.bundle.contexts);
}

const Promise *EvalContextStackGetTopPromise(const EvalContext *ctx)
{
    for (int i = SeqLength(ctx->stack) - 1; i >= 0; --i)
    {
        StackFrame *st = SeqAt(ctx->stack, i);
        if (st->type == STACK_FRAME_TYPE_PROMISE)
        {
            return st->data.promise.owner;
        }

        if (st->type == STACK_FRAME_TYPE_PROMISE_ITERATION)
        {
            return st->data.promise_iteration.owner;
        }
    }

    return NULL;
}

bool EvalContextVariablePut(EvalContext *ctx, VarRef lval, Rval rval, DataType type)
{
    assert(type != DATA_TYPE_NONE);

    Scope *ptr;
    const Rlist *rp;
    CfAssoc *assoc;

    if (rval.type == RVAL_TYPE_SCALAR)
    {
        CfDebug("AddVariableHash(%s.%s=%s (%s) rtype=%c)\n", lval.scope, lval.lval, (const char *) rval.item, CF_DATATYPES[type],
                rval.type);
    }
    else
    {
        CfDebug("AddVariableHash(%s.%s=(list) (%s) rtype=%c)\n", lval.scope, lval.lval, CF_DATATYPES[type], rval.type);
    }

    if (lval.lval == NULL || lval.scope == NULL)
    {
        CfOut(OUTPUT_LEVEL_ERROR, "", "scope.value = %s.%s", lval.scope, lval.lval);
        ProgrammingError("Bad variable or scope in a variable assignment, should not happen - forgotten to register a function call in fncall.c?");
    }

    if (rval.item == NULL)
    {
        CfDebug("No value to assignment - probably a parameter in an unused bundle/body\n");
        return false;
    }

    if (strlen(lval.lval) > CF_MAXVARSIZE)
    {
        char *lval_str = VarRefToString(lval, true);
        CfOut(OUTPUT_LEVEL_ERROR, "", "Variable %s cannot be added because its length exceeds the maximum length allowed: %d", lval_str, CF_MAXVARSIZE);
        free(lval_str);
        return false;
    }

/* If we are not expanding a body template, check for recursive singularities */

    if (strcmp(lval.scope, "body") != 0)
    {
        switch (rval.type)
        {
        case RVAL_TYPE_SCALAR:

            if (StringContainsVar((char *) rval.item, lval.lval))
            {
                CfOut(OUTPUT_LEVEL_ERROR, "", "Scalar variable %s.%s contains itself (non-convergent): %s", lval.scope, lval.lval,
                      (char *) rval.item);
                return false;
            }
            break;

        case RVAL_TYPE_LIST:

            for (rp = rval.item; rp != NULL; rp = rp->next)
            {
                if (StringContainsVar((char *) rp->item, lval.lval))
                {
                    CfOut(OUTPUT_LEVEL_ERROR, "", "List variable %s contains itself (non-convergent)", lval.lval);
                    return false;
                }
            }
            break;

        default:
            break;
        }
    }

    ptr = ScopeGet(lval.scope);
    if (!ptr)
    {
        ptr = ScopeNew(lval.scope);
        if (!ptr)
        {
            return false;
        }
    }

// Look for outstanding lists in variable rvals

    if (THIS_AGENT_TYPE == AGENT_TYPE_COMMON)
    {
        Rlist *listvars = NULL;
        Rlist *scalars = NULL; // TODO what do we do with scalars?

        if (ScopeGetCurrent() && strcmp(ScopeGetCurrent()->scope, "this") != 0)
        {
            MapIteratorsFromRval(ctx, ScopeGetCurrent()->scope, &listvars, &scalars, rval);

            if (listvars != NULL)
            {
                CfOut(OUTPUT_LEVEL_ERROR, "", " !! Redefinition of variable \"%s\" (embedded list in RHS) in context \"%s\"",
                      lval.lval, ScopeGetCurrent()->scope);
            }

            RlistDestroy(listvars);
            RlistDestroy(scalars);
        }
    }

    // FIX: lval is stored with array params as part of the lval for legacy reasons.
    char *final_lval = VarRefToString(lval, false);

    assoc = HashLookupElement(ptr->hashtable, final_lval);

    if (assoc)
    {
        if (CompareVariableValue(rval, assoc) == 0)
        {
            /* Identical value, keep as is */
        }
        else
        {
            /* Different value, bark and replace */
            if (!UnresolvedVariables(assoc, rval.type))
            {
                CfOut(OUTPUT_LEVEL_INFORM, "", " !! Duplicate selection of value for variable \"%s\" in scope %s", lval.lval, ptr->scope);
            }
            RvalDestroy(assoc->rval);
            assoc->rval = RvalCopy(rval);
            assoc->dtype = type;
            CfDebug("Stored \"%s\" in context %s\n", lval.lval, lval.scope);
        }
    }
    else
    {
        if (!HashInsertElement(ptr->hashtable, final_lval, rval, type))
        {
            ProgrammingError("Hash table is full");
        }
    }

    free(final_lval);

    CfDebug("Added Variable %s in scope %s with value (omitted)\n", lval.lval, lval.scope);
    return true;
}

bool EvalContextVariableGet(const EvalContext *ctx, VarRef lval, Rval *rval_out, DataType *type_out)
{
    Scope *ptr = NULL;
    char scopeid[CF_MAXVARSIZE], vlval[CF_MAXVARSIZE], sval[CF_MAXVARSIZE];
    char expbuf[CF_EXPANDSIZE];
    CfAssoc *assoc;

    CfDebug("GetVariable(%s,%s) type=(to be determined)\n", lval.scope, lval.lval);

    if (lval.lval == NULL)
    {
        if (rval_out)
        {
            *rval_out = (Rval) {NULL, RVAL_TYPE_SCALAR };
        }
        if (type_out)
        {
            *type_out = DATA_TYPE_NONE;
        }
        return false;
    }

    if (!IsExpandable(lval.lval))
    {
        strncpy(sval, lval.lval, CF_MAXVARSIZE - 1);
    }
    else
    {
        if (ExpandScalar(ctx, lval.scope, lval.lval, expbuf))
        {
            strncpy(sval, expbuf, CF_MAXVARSIZE - 1);
        }
        else
        {
            /* C type system does not allow us to express the fact that returned
               value may contain immutable string. */
            if (rval_out)
            {
                *rval_out = (Rval) {(char *) lval.lval, RVAL_TYPE_SCALAR };
            }
            if (type_out)
            {
                *type_out = DATA_TYPE_NONE;
            }
            CfDebug("Couldn't expand array-like variable (%s) due to undefined dependencies\n", lval.lval);
            return false;
        }
    }

    if (IsQualifiedVariable(sval))
    {
        scopeid[0] = '\0';
        sscanf(sval, "%[^.].", scopeid);
        strlcpy(vlval, sval + strlen(scopeid) + 1, sizeof(vlval));
        CfDebug("Variable identifier \"%s\" is prefixed with scope id \"%s\"\n", vlval, scopeid);
        ptr = ScopeGet(scopeid);
    }
    else
    {
        strlcpy(vlval, sval, sizeof(vlval));
        strlcpy(scopeid, lval.scope, sizeof(scopeid));
    }

    if (ptr == NULL)
    {
        /* Assume current scope */
        strcpy(vlval, lval.lval);
        ptr = ScopeGet(scopeid);
    }

    if (ptr == NULL)
    {
        CfDebug("Scope \"%s\" for variable \"%s\" does not seem to exist\n", scopeid, vlval);
        /* C type system does not allow us to express the fact that returned
           value may contain immutable string. */
        // TODO: returning the same lval as was past in?
        if (rval_out)
        {
            *rval_out = (Rval) {(char *) lval.lval, RVAL_TYPE_SCALAR };
        }
        if (type_out)
        {
            *type_out = DATA_TYPE_NONE;
        }
        return false;
    }

    CfDebug("GetVariable(%s,%s): using scope '%s' for variable '%s'\n", scopeid, vlval, ptr->scope, vlval);

    assoc = HashLookupElement(ptr->hashtable, vlval);

    if (assoc == NULL)
    {
        CfDebug("No such variable found %s.%s\n\n", scopeid, vlval);
        /* C type system does not allow us to express the fact that returned
           value may contain immutable string. */

        if (rval_out)
        {
            *rval_out = (Rval) {(char *) lval.lval, RVAL_TYPE_SCALAR };
        }
        if (type_out)
        {
            *type_out = DATA_TYPE_NONE;
        }
        return false;
    }

    CfDebug("return final variable type=%s, value={\n", CF_DATATYPES[assoc->dtype]);

    if (DEBUG)
    {
        RvalShow(stdout, assoc->rval);
    }
    CfDebug("}\n");

    if (rval_out)
    {
        *rval_out = assoc->rval;
    }
    if (type_out)
    {
        *type_out = assoc->dtype;
        assert(*type_out != DATA_TYPE_NONE);
    }

    return true;
}

bool EvalContextVariableControlCommonGet(const EvalContext *ctx, CommonControl lval, Rval *rval_out)
{
    return EvalContextVariableGet(ctx, (VarRef) { NULL, "control_common", CFG_CONTROLBODY[lval].lval }, rval_out, NULL);
}

bool EvalContextPromiseIsDone(const EvalContext *ctx, const Promise *pp)
{
    return PromiseSetContains(ctx->promises_done, pp);
}

void EvalContextMarkPromiseDone(EvalContext *ctx, const Promise *pp)
{
    PromiseSetAdd(ctx->promises_done, pp->org_pp);
}

void EvalContextMarkPromiseNotDone(EvalContext *ctx, const Promise *pp)
{
    PromiseSetRemove(ctx->promises_done, pp->org_pp);
}



/* cfPS and associated machinery */



/*
 * Internal functions temporarily used from logging implementation
 */

static const char *NO_STATUS_TYPES[] =
    { "vars", "classes", "insert_lines", "delete_lines", "replace_patterns", "field_edits", NULL };
static const char *NO_LOG_TYPES[] =
    { "vars", "classes", "insert_lines", "delete_lines", "replace_patterns", "field_edits", NULL };

/*
 * Vars, classes and similar promises which do not affect the system itself (but
 * just support evalution) do not need to be counted as repaired/failed, as they
 * may change every iteration and introduce lot of churn in reports without
 * giving any value.
 */
static bool IsPromiseValuableForStatus(const Promise *pp)
{
    return pp && (pp->parent_promise_type->name != NULL) && (!IsStrIn(pp->parent_promise_type->name, NO_STATUS_TYPES));
}

/*
 * Vars, classes and subordinate promises (like edit_line) do not need to be
 * logged, as they exist to support other promises.
 */

static bool IsPromiseValuableForLogging(const Promise *pp)
{
    return pp && (pp->parent_promise_type->name != NULL) && (!IsStrIn(pp->parent_promise_type->name, NO_LOG_TYPES));
}

static void AddAllClasses(EvalContext *ctx, const char *ns, const Rlist *list, bool persist, ContextStatePolicy policy, ContextScope context_scope)
{
    for (const Rlist *rp = list; rp != NULL; rp = rp->next)
    {
        char *classname = xstrdup(rp->item);

        CanonifyNameInPlace(classname);

        if (EvalContextHeapContainsHard(ctx, classname))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", " !! You cannot use reserved hard class \"%s\" as post-condition class", classname);
            // TODO: ok.. but should we take any action? continue; maybe?
        }

        if (persist > 0)
        {
            if (context_scope != CONTEXT_SCOPE_NAMESPACE)
            {
                CfOut(OUTPUT_LEVEL_INFORM, "", "Automatically promoting context scope for '%s' to namespace visibility, due to persistence", classname);
            }

            CfOut(OUTPUT_LEVEL_VERBOSE, "", " ?> defining persistent promise result class %s\n", classname);
            EvalContextHeapPersistentSave(CanonifyName(rp->item), ns, persist, policy);
            EvalContextHeapAddSoft(ctx, classname, ns);
        }
        else
        {
            CfOut(OUTPUT_LEVEL_VERBOSE, "", " ?> defining promise result class %s\n", classname);

            switch (context_scope)
            {
            case CONTEXT_SCOPE_BUNDLE:
                EvalContextStackFrameAddSoft(ctx, classname);
                break;

            default:
            case CONTEXT_SCOPE_NAMESPACE:
                EvalContextHeapAddSoft(ctx, classname, ns);
                break;
            }
        }
    }
}

static void DeleteAllClasses(EvalContext *ctx, const Rlist *list)
{
    for (const Rlist *rp = list; rp != NULL; rp = rp->next)
    {
        if (CheckParseContext((char *) rp->item, CF_IDRANGE) != SYNTAX_TYPE_MATCH_OK)
        {
            return; // TODO: interesting course of action, but why is the check there in the first place?
        }

        if (EvalContextHeapContainsHard(ctx, (char *) rp->item))
        {
            CfOut(OUTPUT_LEVEL_ERROR, "", " !! You cannot cancel a reserved hard class \"%s\" in post-condition classes",
                  RlistScalarValue(rp));
        }

        const char *string = (char *) (rp->item);

        CfOut(OUTPUT_LEVEL_VERBOSE, "", " -> Cancelling class %s\n", string);

        EvalContextHeapPersistentRemove(string);

        EvalContextHeapRemoveSoft(ctx, CanonifyName(string));

        EvalContextStackFrameAddNegated(ctx, CanonifyName(string));
    }
}

#ifdef HAVE_NOVA
static void TrackTotalCompliance(PromiseResult status, const Promise *pp)
{
    char nova_status;

    switch (status)
    {
    case PROMISE_RESULT_CHANGE:
        nova_status = 'r';
        break;

    case PROMISE_RESULT_WARN:
    case PROMISE_RESULT_TIMEOUT:
    case PROMISE_RESULT_FAIL:
    case PROMISE_RESULT_DENIED:
    case PROMISE_RESULT_INTERRUPTED:
        nova_status = 'n';
        break;

    case PROMISE_RESULT_NOOP:
        nova_status = 'c';
        break;

    default:
        ProgrammingError("Unexpected status '%c' has been passed to TrackTotalCompliance", status);
    }

    EnterpriseTrackTotalCompliance(pp, nova_status);
}
#endif


static void SetPromiseOutcomeClasses(PromiseResult status, EvalContext *ctx, const Promise *pp, DefineClasses dc)
{
    Rlist *add_classes = NULL;
    Rlist *del_classes = NULL;

    switch (status)
    {
    case PROMISE_RESULT_CHANGE:
        add_classes = dc.change;
        del_classes = dc.del_change;
        break;

    case PROMISE_RESULT_TIMEOUT:
        add_classes = dc.timeout;
        del_classes = dc.del_notkept;
        break;

    case PROMISE_RESULT_WARN:
    case PROMISE_RESULT_FAIL:
        add_classes = dc.failure;
        del_classes = dc.del_notkept;
        break;

    case PROMISE_RESULT_DENIED:
        add_classes = dc.denied;
        del_classes = dc.del_notkept;
        break;

    case PROMISE_RESULT_INTERRUPTED:
        add_classes = dc.interrupt;
        del_classes = dc.del_notkept;
        break;

    case PROMISE_RESULT_NOOP:
        add_classes = dc.kept;
        del_classes = dc.del_kept;
        break;

    default:
        ProgrammingError("Unexpected status '%c' has been passed to SetPromiseOutcomeClasses", status);
    }

    AddAllClasses(ctx, PromiseGetNamespace(pp), add_classes, dc.persist, dc.timer, dc.scope);
    DeleteAllClasses(ctx, del_classes);
}

static void UpdatePromiseComplianceStatus(PromiseResult status, const Promise *pp, char *reason)
{
    if (!IsPromiseValuableForLogging(pp))
    {
        return;
    }

    char compliance_status;

    switch (status)
    {
    case PROMISE_RESULT_CHANGE:
        compliance_status = PROMISE_STATE_REPAIRED;
        break;

    case PROMISE_RESULT_WARN:
    case PROMISE_RESULT_TIMEOUT:
    case PROMISE_RESULT_FAIL:
    case PROMISE_RESULT_DENIED:
    case PROMISE_RESULT_INTERRUPTED:
        compliance_status = PROMISE_STATE_NOTKEPT;
        break;

    case PROMISE_RESULT_NOOP:
        compliance_status = PROMISE_STATE_ANY;
        break;

    default:
        ProgrammingError("Unknown status '%c' has been passed to UpdatePromiseComplianceStatus", status);
    }

    NotePromiseCompliance(pp, compliance_status, reason);
}

static void SummarizeTransaction(EvalContext *ctx, TransactionContext tc, const char *logname)
{
    if (logname && (tc.log_string))
    {
        char buffer[CF_EXPANDSIZE];

        ExpandScalar(ctx, ScopeGetCurrent()->scope, tc.log_string, buffer);

        if (strcmp(logname, "udp_syslog") == 0)
        {
            RemoteSysLog(tc.log_priority, buffer);
        }
        else if (strcmp(logname, "stdout") == 0)
        {
            CfOut(OUTPUT_LEVEL_INFORM, "", "L: %s\n", buffer);
        }
        else
        {
            FILE *fout = fopen(logname, "a");

            if (fout == NULL)
            {
                CfOut(OUTPUT_LEVEL_ERROR, "", "Unable to open private log %s", logname);
                return;
            }

            CfOut(OUTPUT_LEVEL_VERBOSE, "", " -> Logging string \"%s\" to %s\n", buffer, logname);
            fprintf(fout, "%s\n", buffer);

            fclose(fout);
        }

        tc.log_string = NULL;     /* To avoid repetition */
    }
}

static void DoSummarizeTransaction(EvalContext *ctx, PromiseResult status, const Promise *pp, TransactionContext tc)
{
    if (!IsPromiseValuableForLogging(pp))
    {
        return;
    }

    char *log_name;

    switch (status)
    {
    case PROMISE_RESULT_CHANGE:
        log_name = tc.log_repaired;
        break;

    case PROMISE_RESULT_WARN:
        /* FIXME: nothing? */
        return;

    case PROMISE_RESULT_TIMEOUT:
    case PROMISE_RESULT_FAIL:
    case PROMISE_RESULT_DENIED:
    case PROMISE_RESULT_INTERRUPTED:
        log_name = tc.log_failed;
        break;

    case PROMISE_RESULT_NOOP:
        log_name = tc.log_kept;
        break;
    }

    SummarizeTransaction(ctx, tc, log_name);
}

static void NotifyDependantPromises(PromiseResult status, EvalContext *ctx, const Promise *pp)
{
    switch (status)
    {
    case PROMISE_RESULT_CHANGE:
    case PROMISE_RESULT_NOOP:
        MarkPromiseHandleDone(ctx, pp);
        break;

    default:
        /* This promise is not yet done, don't mark it is as such */
        break;
    }
}

void ClassAuditLog(EvalContext *ctx, const Promise *pp, Attributes attr, PromiseResult status)
{
    if (!IsPromiseValuableForStatus(pp))
    {
#ifdef HAVE_NOVA
        TrackTotalCompliance(status, pp);
#endif
        UpdatePromiseCounters(status, attr.transaction);
    }

    SetPromiseOutcomeClasses(status, ctx, pp, attr.classes);
    NotifyDependantPromises(status, ctx, pp);
    DoSummarizeTransaction(ctx, status, pp, attr.transaction);
}

static void LogPromiseContext(const EvalContext *ctx, const Promise *pp)
{
    Rval retval;
    char *v;
    if (EvalContextVariableControlCommonGet(ctx, COMMON_CONTROL_VERSION, &retval))
    {
        v = (char *) retval.item;
    }
    else
    {
        v = "not specified";
    }

    const char *sp = PromiseGetHandle(pp);
    if (sp == NULL)
    {
        sp = PromiseID(pp);
    }
    if (sp == NULL)
    {
        sp = "(unknown)";
    }

    Log(LOG_LEVEL_INFO, "I: Report relates to a promise with handle \"%s\"", sp);

    if (PromiseGetBundle(pp)->source_path)
    {
        Log(LOG_LEVEL_INFO, "I: Made in version \'%s\' of \'%s\' near line %zu",
            v, PromiseGetBundle(pp)->source_path, pp->offset.line);
    }
    else
    {
        Log(LOG_LEVEL_INFO, "I: Promise is made internally by cfengine");
    }

    switch (pp->promisee.type)
    {
    case RVAL_TYPE_SCALAR:
        Log(LOG_LEVEL_INFO,"I: The promise was made to: \'%s\'", (char *) pp->promisee.item);
        break;

    case RVAL_TYPE_LIST:
    {
        Writer *w = StringWriter();
        RlistWrite(w, pp->promisee.item);
        Log(LOG_LEVEL_INFO, "I: The promise was made to (stakeholders): %s", StringWriterData(w));
        WriterClose(w);
        break;
    }
    default:
        break;
    }

    if (pp->comment)
    {
        Log(LOG_LEVEL_INFO, "I: Comment: %s\n", pp->comment);
    }
}

void cfPS(EvalContext *ctx, OutputLevel level, PromiseResult status, const char *errstr, const Promise *pp, Attributes attr, const char *fmt, ...)
{
    /*
     * This stub implementation of cfPS delegates to the new logging backend.
     *
     * Due to the fact very little of the code has been converted, this code
     * does a full initialization and shutdown of logging subsystem for each
     * cfPS.
     *
     * Instead, LoggingInit should be called at the moment EvalContext is
     * created, LoggingPromiseEnter/LoggingPromiseFinish should be called around
     * ExpandPromise and LoggingFinish should be called when EvalContext is
     * going to be destroyed.
     *
     * But it requires all calls to cfPS to be eliminated.
     */

    /* FIXME: Ensure that NULL pp is never passed into cfPS */

    if (pp)
    {
        PromiseLoggingInit(ctx);
        PromiseLoggingPromiseEnter(ctx, pp);

        if (level == OUTPUT_LEVEL_ERROR)
        {
            LogPromiseContext(ctx, pp);
        }
    }

    va_list ap;
    va_start(ap, fmt);
    CfVOut(level, errstr, fmt, ap);
    va_end(ap);

    if (pp)
    {
        char *last_msg = PromiseLoggingPromiseFinish(ctx, pp);
        PromiseLoggingFinish(ctx);

        /* Now complete the exits status classes and auditing */

        ClassAuditLog(ctx, pp, attr, status);
        UpdatePromiseComplianceStatus(status, pp, last_msg);

        free(last_msg);
    }
}
