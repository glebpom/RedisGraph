/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "proc_fulltext_query.h"
#include "../value.h"
#include "../util/arr.h"
#include "../index/index.h"
#include "../util/rmalloc.h"
#include "../graph/graphcontext.h"

//------------------------------------------------------------------------------
// fulltext createNodeIndex
//------------------------------------------------------------------------------

// CALL db.idx.fulltext.queryNodes(label, query)

typedef struct {
	Node n;
	Graph *g;
	SIValue *output;
	Index *idx;
	RSResultsIterator *iter;
} QueryNodeContext;

ProcedureResult Proc_FulltextQueryNodeInvoke(ProcedureCtx *ctx, const char **args) {
	if(array_len(args) < 2) return PROCEDURE_ERR;

	ctx->privateData = NULL;
	GraphContext *gc = GraphContext_GetFromTLS();

	// See if there's a full-text index for given label.
	char *err = NULL;
	const char *label = args[0];
	const char *query = args[1];

	// Get full-text index from schema.
	Schema *s = GraphContext_GetSchema(gc, label, SCHEMA_NODE);
	if(s == NULL) return PROCEDURE_OK;
	Index *idx = Schema_GetIndex(s, NULL, IDX_FULLTEXT);
	if(!idx) return PROCEDURE_ERR; // TODO this should cause an error to be emitted.

	QueryNodeContext *pdata = rm_malloc(sizeof(QueryNodeContext));
	pdata->idx = idx;
	pdata->g = gc->g;
	pdata->output = array_new(SIValue, 2);
	pdata->output = array_append(pdata->output, SI_ConstStringVal("node"));
	pdata->output = array_append(pdata->output, SI_Node(&pdata->n));
	// pdata->output = array_append(pdata->output, SI_ConstStringVal("score"));
	// pdata->output = array_append(pdata->output, SI_DoubleVal(0.0));

	// Execute query
	pdata->iter = Index_Query(pdata->idx, query, &err);
	assert(pdata->iter);

	ctx->privateData = pdata;
	return PROCEDURE_OK;
}

SIValue *Proc_FulltextQueryNodeStep(ProcedureCtx *ctx) {
	if(!ctx->privateData) return NULL; // No index was attached to this procedure.

	QueryNodeContext *pdata = (QueryNodeContext *)ctx->privateData;
	if(!pdata || !pdata->iter) return NULL;

	/* Try to get a result out of the iterator.
	 * NULL is returned if iterator id depleted. */
	size_t len = 0;
	NodeID *id = (NodeID *)RediSearch_ResultsIteratorNext(pdata->iter, pdata->idx->idx, &len);

	// Depleted.
	if(!id) return NULL;

	// Get Node.
	Node *n = &pdata->n;
	Graph_GetNode(pdata->g, *id, n);

	pdata->output[1] = SI_Node(n);
	return pdata->output;
}

ProcedureResult Proc_FulltextQueryNodeFree(ProcedureCtx *ctx) {
	// Clean up.
	if(!ctx->privateData) return PROCEDURE_OK;

	QueryNodeContext *pdata = ctx->privateData;
	array_free(pdata->output);
	if(pdata->iter) RediSearch_ResultsIteratorFree(pdata->iter);
	rm_free(pdata);

	return PROCEDURE_OK;
}

ProcedureCtx *Proc_FulltextQueryNodeGen() {
	void *privateData = NULL;
	ProcedureOutput **output = array_new(ProcedureOutput *, 1);
	ProcedureOutput *out_node = rm_malloc(sizeof(ProcedureOutput));
	out_node->name = "node";
	out_node->type = T_NODE;

	output = array_append(output, out_node);
	ProcedureCtx *ctx = ProcCtxNew("db.idx.fulltext.queryNodes",
								   2,
								   output,
								   Proc_FulltextQueryNodeStep,
								   Proc_FulltextQueryNodeInvoke,
								   Proc_FulltextQueryNodeFree,
								   privateData);
	return ctx;
}
