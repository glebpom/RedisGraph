/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "proc_fulltext_drop_index.h"
#include "../value.h"
#include "../util/arr.h"
#include "../util/rmalloc.h"
#include "../graph/graphcontext.h"

//------------------------------------------------------------------------------
// fulltext dropNodeIndex
//------------------------------------------------------------------------------

// CALL db.idx.fulltext.drop(label)
// CALL db.idx.fulltext.drop('books')

ProcedureResult Proc_FulltextDropIndexInvoke(ProcedureCtx *ctx, const char **args) {
	if(array_len(args) != 1) return PROCEDURE_ERR;

	const char *label = args[0];
	GraphContext *gc = GraphContext_GetFromTLS();
	Schema *s = GraphContext_GetSchema(gc, label, SCHEMA_NODE);
	// Schema doesn't exists, TODO: report error.
	if(!s) return PROCEDURE_ERR;

	if(Schema_RemoveIndex(s, NULL, IDX_FULLTEXT) == INDEX_FAIL) return PROCEDURE_OK;

	return PROCEDURE_ERR;
}

SIValue *Proc_FulltextDropIndexStep(ProcedureCtx *ctx) {
	return NULL;
}

ProcedureResult Proc_FulltextDropIndexFree(ProcedureCtx *ctx) {
	// Clean up.
	return PROCEDURE_OK;
}

ProcedureCtx *Proc_FulltextDropIdxGen() {
	void *privateData = NULL;
	ProcedureOutput **output = array_new(ProcedureOutput *, 0);
	ProcedureCtx *ctx = ProcCtxNew("db.idx.fulltext.drop",
								   1,
								   output,
								   Proc_FulltextDropIndexStep,
								   Proc_FulltextDropIndexInvoke,
								   Proc_FulltextDropIndexFree,
								   privateData);

	return ctx;
}
