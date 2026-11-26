/*
 * Keep `x IN (SELECT ... GROUP BY ...)` as a SubPlan when everything involved
 * lives in ClickHouse, so it can be pushed down.
 *
 * Postgres pulls an uncorrelated IN sublink up into a semi-join. When the
 * subquery aggregates, the inner side of that semi-join is a subquery rel that
 * is never offered to the FDW's join pushdown, so the join runs in Postgres and
 * every row of the foreign side crosses the wire (TPC-H Q18: all of lineitem).
 * A sublink under an OR is not pulled up; it stays a SubPlan, which pg_clickhouse
 * already ships as part of the remote WHERE clause. `sublink OR false` is
 * folded back to `sublink` during expression preprocessing, which runs after
 * the pull-up, so the wrapper costs nothing and changes no semantics in a
 * WHERE clause (NULL and false both reject the row).
 *
 * Only top-level conjuncts of a WHERE clause are rewritten, and only when the
 * subquery aggregates, is uncorrelated, reads nothing but pg_clickhouse
 * foreign tables, and the tested expression uses only pg_clickhouse columns
 * from the same server. Everything else is left to the planner untouched.
 *
 * The hook runs at parse analysis, so the library must already be loaded in
 * the session (session_preload_libraries = 'pg_clickhouse').
 */
#include "postgres.h"

#include "fdw.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "utils/guc.h"

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static bool ch_pushdown_subquery_in                              = true;

static bool
is_ch_foreign_rte(RangeTblEntry* rte, Oid* serverid) {
    ForeignTable* ft;
    ForeignServer* srv;
    ForeignDataWrapper* fdw;

    if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_FOREIGN_TABLE) {
        return false;
    }
    ft  = GetForeignTable(rte->relid);
    srv = GetForeignServer(ft->serverid);
    fdw = GetForeignDataWrapper(srv->fdwid);
    if (strcmp(fdw->fdwname, "clickhouse_fdw") != 0) {
        return false;
    }
    if (OidIsValid(*serverid) && *serverid != ft->serverid) {
        return false;
    }
    *serverid = ft->serverid;
    return true;
}

static bool
sublink_is_pushable(Query* outer, SubLink* sl) {
    Query* sub;
    ListCell* lc;
    List* vars;
    Oid server = InvalidOid;

    if (sl->subLinkType != ANY_SUBLINK || sl->subselect == NULL || !IsA(sl->subselect, Query)) {
        return false;
    }
    sub = (Query*)sl->subselect;
    if (!(sub->hasAggs || sub->groupClause != NIL || sub->havingQual != NULL)) {
        return false; /* plain subqueries already push down as a semi-join */
    }
    if (sub->hasSubLinks || sub->hasWindowFuncs || sub->hasTargetSRFs || sub->limitCount ||
        sub->limitOffset || sub->setOperations || sub->cteList) {
        return false;
    }
    if (contain_vars_of_level((Node*)sub, 1)) {
        return false; /* correlated */
    }
    foreach (lc, sub->rtable) {
        RangeTblEntry* rte = lfirst_node(RangeTblEntry, lc);

#if PG_VERSION_NUM >= 180000
        /* PG18 adds an RTE_GROUP entry to every query that has a GROUP BY. */
        if (rte->rtekind == RTE_GROUP) {
            continue;
        }
#endif
        if (!is_ch_foreign_rte(rte, &server)) {
            return false;
        }
    }
    vars = pull_var_clause(sl->testexpr, 0);
    if (vars == NIL) {
        return false;
    }
    foreach (lc, vars) {
        Var* v = (Var*)lfirst(lc);

        if (!IsA(v, Var) || v->varlevelsup != 0 ||
            !is_ch_foreign_rte(rt_fetch(v->varno, outer->rtable), &server)) {
            return false;
        }
    }
    return true;
}

static Node*
wrap_quals(Query* q, Node* quals) {
    if (quals == NULL) {
        return NULL;
    }
    if (IsA(quals, SubLink)) {
        if (sublink_is_pushable(q, (SubLink*)quals)) {
            return (Node*)makeBoolExpr(
                OR_EXPR, list_make2(quals, makeBoolConst(false, false)), -1
            );
        }
        return quals;
    }
    if (IsA(quals, BoolExpr) && ((BoolExpr*)quals)->boolop == AND_EXPR) {
        ListCell* lc;

        foreach (lc, ((BoolExpr*)quals)->args) {
            lfirst(lc) = wrap_quals(q, lfirst(lc));
        }
    }
    return quals;
}

static void
walk_query(Query* q) {
    ListCell* lc;

    if (q->jointree != NULL && q->hasSubLinks) {
        q->jointree->quals = wrap_quals(q, q->jointree->quals);
    }
    foreach (lc, q->rtable) {
        RangeTblEntry* rte = lfirst_node(RangeTblEntry, lc);

        if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL) {
            walk_query(rte->subquery);
        }
    }
}

static void
chfdw_post_parse_analyze(ParseState* pstate, Query* query, JumbleState* jstate) {
    if (prev_post_parse_analyze_hook) {
        prev_post_parse_analyze_hook(pstate, query, jstate);
    }
    if (ch_pushdown_subquery_in && query->commandType == CMD_SELECT) {
        walk_query(query);
    }
}

void
chfdw_install_subquery_in_hook(void) {
    DefineCustomBoolVariable(
        "pg_clickhouse.pushdown_subquery_in",
        "Keep IN (aggregate subquery) over ClickHouse tables as a pushable SubPlan.",
        "Postgres turns such a sublink into a semi-join that cannot be pushed "
        "down, so the foreign tables are read in full. When this is on, the "
        "sublink is kept as a SubPlan and shipped to ClickHouse. Takes effect "
        "only if pg_clickhouse is loaded at parse time, for example through "
        "session_preload_libraries.",
        &ch_pushdown_subquery_in,
        true,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    prev_post_parse_analyze_hook = post_parse_analyze_hook;
    post_parse_analyze_hook      = chfdw_post_parse_analyze;
}
