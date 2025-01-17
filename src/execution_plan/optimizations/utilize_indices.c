#include "utilize_indices.h"
#include "../../value.h"
#include "../../util/arr.h"
#include "../ops/op_index_scan.h"
#include "../../ast/ast_shared.h"
#include "../../util/range/string_range.h"
#include "../../util/range/numeric_range.h"

/* Reverse an inequality symbol so that indices can support
 * inequalities with right-hand variables. */
int _reverseOp(int op) {
	switch(op) {
	case OP_LT:
		return OP_GT;
	case OP_LE:
		return OP_GE;
	case OP_GT:
		return OP_LT;
	case OP_GE:
		return OP_LE;
	default:
		return op;
	}
}

/* Modifies filter tree such that the left-hand side
 * is of type variadic and the right-hand side is constant. */
void _normalize_filter(FT_FilterNode *filter) {
	// Normalize, left hand side should be variadic, right hand side const.
	if(filter->pred.rhs->operand.type == AR_EXP_VARIADIC) {
		// Swap.
		AR_ExpNode *tmp = filter->pred.rhs;
		filter->pred.rhs = filter->pred.lhs;
		filter->pred.lhs = tmp;
		filter->pred.op = _reverseOp(filter->pred.op);
	}
}

//------------------------------------------------------------------------------
// To RediSearch query node
//------------------------------------------------------------------------------
/* Create a RediSearch query node out of a numeric range object. */
RSQNode *_NumericRangeToQueryNode(RSIndex *idx, const char *field, const NumericRange *range) {
	double max = (range->max == INFINITY) ? RSRANGE_INF : range->max;
	double min = (range->min == -INFINITY) ? RSRANGE_NEG_INF : range->min;
	return RediSearch_CreateNumericNode(idx, field, max, min, range->include_max, range->include_min);
}

/* Create a RediSearch query node out of a string range object. */
RSQNode *_StringRangeToQueryNode(RSIndex *idx, const char *field, const StringRange *range) {
	const char *max = (range->max == NULL) ? RSLECRANGE_INF : range->max;
	const char *min = (range->min == NULL) ? RSLEXRANGE_NEG_INF : range->min;
	RSQNode *root = RediSearch_CreateTagNode(idx, field);
	RSQNode *child = RediSearch_CreateLexRangeNode(idx, field, min, max,
												   range->include_min,
												   range->include_max);
	RediSearch_QueryNodeAddChild(root, child);
	return root;
}

/* Creates a RediSearch query node out of given filter tree. */
RSQNode *_filterTreeToQueryNode(FT_FilterNode *filter, RSIndex *sp) {
	RSQNode *node = NULL;
	RSQNode *parent = NULL;

	if(filter->t == FT_N_COND) {
		RSQNode *left = NULL;
		RSQNode *right = NULL;
		switch(filter->cond.op) {
		case OP_OR:
			node = RediSearch_CreateUnionNode(sp);
			left = _filterTreeToQueryNode(filter->cond.left, sp);
			right = _filterTreeToQueryNode(filter->cond.right, sp);
			RediSearch_QueryNodeAddChild(node, left);
			RediSearch_QueryNodeAddChild(node, right);
			break;
		case OP_AND:
			node = RediSearch_CreateIntersectNode(sp, false);
			left = _filterTreeToQueryNode(filter->cond.left, sp);
			right = _filterTreeToQueryNode(filter->cond.right, sp);
			RediSearch_QueryNodeAddChild(node, left);
			RediSearch_QueryNodeAddChild(node, right);
			break;
		default:
			assert("unexpected conditional operation");
		}
	} else if(filter->t == FT_N_PRED) {
		// Make sure left hand side is variadic and right hand side is constant.
		_normalize_filter(filter);
		double d;
		const char *field = filter->pred.lhs->operand.variadic.entity_prop;
		SIValue v = filter->pred.rhs->operand.constant;
		switch(SI_TYPE(v)) {
		case T_STRING:
			parent = RediSearch_CreateTagNode(sp, field);
			switch(filter->pred.op) {
			case OP_LT:    // <
				node = RediSearch_CreateLexRangeNode(sp, field, RSLEXRANGE_NEG_INF, v.stringval, 0, 0);
				break;
			case OP_LE:    // <=
				node = RediSearch_CreateLexRangeNode(sp, field, RSLEXRANGE_NEG_INF, v.stringval, 0, 1);
				break;
			case OP_GT:    // >
				node = RediSearch_CreateLexRangeNode(sp, field, v.stringval, RSLECRANGE_INF, 0, 0);
				break;
			case OP_GE:    // >=
				node = RediSearch_CreateLexRangeNode(sp, field, v.stringval, RSLECRANGE_INF, 1, 0);
				break;
			case OP_EQUAL:  // ==
				node = RediSearch_CreateTokenNode(sp, field, v.stringval);
				break;
			case OP_NEQUAL: // !=
				assert("Index can't utilize the 'not equals' operation.");
				break;
			default:
				assert("unexpected operation");
			}

			RediSearch_QueryNodeAddChild(parent, node);
			node = parent;
			break;

		case T_DOUBLE:
		case T_INT64:
		case T_BOOL:
			d = SI_GET_NUMERIC(v);
			switch(filter->pred.op) {
			case OP_LT:    // <
				node = RediSearch_CreateNumericNode(sp, field, d, RSRANGE_NEG_INF, false, false);
				break;
			case OP_LE:    // <=
				node = RediSearch_CreateNumericNode(sp, field, d, RSRANGE_NEG_INF, true, false);
				break;
			case OP_GT:    // >
				node = RediSearch_CreateNumericNode(sp, field, RSRANGE_INF, d, false, false);
				break;
			case OP_GE:    // >=
				node = RediSearch_CreateNumericNode(sp, field, RSRANGE_INF, d, false, true);
				break;
			case OP_EQUAL:  // ==
				node = RediSearch_CreateNumericNode(sp, field, d, d, true, true);
				break;
			case OP_NEQUAL: // !=
				assert("Index can't utilize the 'not equals' operation.");
				break;
			default:
				assert("unexpected operation");
			}
			break;
		default:
			assert("unexpected value type");
		}
	} else {
		assert("unknow filter tree node type");
	}
	return node;
}

//------------------------------------------------------------------------------

/* Tests to see if given filter tree is a simple predicate
 * e.g. n.v = 2
 * one side is variadic while the other side is constant. */
bool _simple_predicates(const FT_FilterNode *filter) {
	if(filter->t == FT_N_PRED) {
		if(filter->pred.lhs->type == AR_EXP_OP || filter->pred.rhs->type == AR_EXP_OP) {
			return false;
		}

		// Both left and right side are variadic.
		if(filter->pred.lhs->operand.type == AR_EXP_VARIADIC &&
		   filter->pred.rhs->operand.type == AR_EXP_VARIADIC) {
			return false;
		}

		// Both left and right are constants.
		if(filter->pred.lhs->operand.type == AR_EXP_CONSTANT &&
		   filter->pred.rhs->operand.type == AR_EXP_CONSTANT) {
			return false;
		}

		// Validate constant type.
		SIValue c = SI_NullVal();
		if(filter->pred.lhs->operand.type == AR_EXP_CONSTANT) c = filter->pred.lhs->operand.constant;
		if(filter->pred.rhs->operand.type == AR_EXP_CONSTANT) c = filter->pred.rhs->operand.constant;
		SIType t = SI_TYPE(c);

		return(t & (SI_NUMERIC | T_STRING | T_BOOL));
	}

	// FT_N_COND.
	if(!_simple_predicates(filter->cond.left)) return false;
	if(!_simple_predicates(filter->cond.right)) return false;
	return true;
}

/* Checks to see if given filter can be resolved by index. */
bool _applicableFilter(Index *idx, Filter *filter) {
	bool res = true;
	rax *attr = NULL;
	rax *entities = NULL;

	uint idx_fields_count = Index_FieldsCount(idx);
	const char **idx_fields = Index_GetFields(idx);
	FT_FilterNode *filter_tree = filter->filterTree;

	/* filterTree will either be a predicate or a tree with an OR root.
	 * make sure filter doesn't contains predicates of type: a.v = b.y */
	entities = FilterTree_CollectModified(filter_tree);
	uint entity_count = raxSize(entities);

	// a.v op b.k
	if(entity_count != 1) {
		res = false;
		goto cleanup;
	}

	// Make sure the "not equal, <>" operator isn't used.
	if(FilterTree_containsOp(filter_tree, OP_NEQUAL)) {
		res = false;
		goto cleanup;
	}

	// Make sure all filtered attributes are indexed.
	attr = FilterTree_CollectAttributes(filter_tree);
	uint filter_attribute_count = raxSize(attr);

	// Filter refers to a greater number of attributes.
	if(filter_attribute_count > idx_fields_count) {
		res = false;
		goto cleanup;
	}

	for(uint i = 0; i < idx_fields_count; i++) {
		const char *field = idx_fields[i];
		if(raxFind(attr, (unsigned char *)field, strlen(field)) != raxNotFound) {
			filter_attribute_count--;
			// All filtered attributes are indexed.
			if(filter_attribute_count == 0) break;
		}
	}
	res = (filter_attribute_count == 0);

cleanup:
	if(attr) raxFree(attr);
	if(entities) raxFree(entities);
	return res;
}

/* Returns an array of filter operation which can be
 * reduced into a single index scan operation. */
Filter **_applicableFilters(NodeByLabelScan *scanOp, Index *idx) {
	Filter **filters = array_new(Filter *, 0);

	/* We begin with a LabelScan, and want to find predicate filters that modify
	 * the active entity. */
	OpBase *current = scanOp->op.parent;
	while(current->type == OPType_FILTER) {
		Filter *filter = (Filter *)current;

		if(_applicableFilter(idx, filter)) {
			// Make sure all predicates are of type n.v = CONST.
			FT_FilterNode *filter_tree = filter->filterTree;
			if(_simple_predicates(filter_tree)) {
				_normalize_filter(filter_tree);
				filters = array_append(filters, filter);
			}
		}

		// Advance to the next operation.
		current = current->parent;
	}

	return filters;
}

/* Reduce filter into a range object
 * Return true if filter was reduce, false otherwise. */
void _predicateTreeToRange(const FT_FilterNode *tree, rax *string_ranges, rax *numeric_ranges) {
	// Simple predicate trees are used to build up a range object.
	assert(tree->pred.lhs->operand.type == AR_EXP_VARIADIC &&
		   tree->pred.rhs->operand.type == AR_EXP_CONSTANT);

	int op = tree->pred.op;
	SIValue c = tree->pred.rhs->operand.constant;
	const char *prop = tree->pred.lhs->operand.variadic.entity_prop;
	StringRange *sr = raxFind(string_ranges, (unsigned char *)prop, strlen(prop));
	NumericRange *nr = raxFind(numeric_ranges, (unsigned char *)prop, strlen(prop));

	// Get or create range object for alias.prop.
	if(SI_TYPE(c) & SI_NUMERIC || SI_TYPE(c) == T_BOOL) {
		// Create if doesn't exists.
		if(nr == raxNotFound) {
			nr = NumericRange_New();
			raxTryInsert(numeric_ranges, (unsigned char *)prop, strlen(prop), nr, NULL);
		}
		NumericRange_TightenRange(nr, op, SI_GET_NUMERIC(c));
	} else if(SI_TYPE(c) == T_STRING) {
		// Create if doesn't exists.
		if(sr == raxNotFound) {
			sr = StringRange_New();
			raxTryInsert(string_ranges, (unsigned char *)prop, strlen(prop), sr, NULL);
		}
		StringRange_TightenRange(sr, op, c.stringval);
	} else {
		assert(false);
	}
}

/* Try to replace given Label Scan operation and a set of Filter operations with
 * a single Index Scan operation. */
void reduce_scan_op(ExecutionPlan *plan, NodeByLabelScan *scan) {
	RSQNode *root = NULL;
	uint rsqnode_count = 0;

	// Make sure there's an index for scanned label.
	const char *label = scan->node->label;
	GraphContext *gc = GraphContext_GetFromTLS();
	Index *idx = GraphContext_GetIndex(gc, label, NULL, IDX_EXACT_MATCH);
	if(idx == NULL) return;

	/* Reduce filters into ranges.
	 * we differentiate between between numeric filters
	 * and string filters. */
	rax *string_ranges = NULL;
	rax *numeric_ranges = NULL;
	RSQNode **rsqnodes = array_new(RSQNode *, 1);

	// Get all applicable filter for index.
	RSIndex *rs_idx = idx->idx;
	Filter **filters = _applicableFilters(scan, idx);

	// No filters, return.
	uint filters_count = array_len(filters);
	if(filters_count == 0) goto cleanup;

	string_ranges = raxNew();
	numeric_ranges = raxNew();

	for(uint i = 0; i < filters_count; i++) {
		Filter *filter = filters[i];
		FT_FilterNode *filter_tree = filter->filterTree;

		if(filter_tree->t == FT_N_PRED) {
			_predicateTreeToRange(filter_tree, string_ranges, numeric_ranges);
		} else {
			// OR trees are directly converted into RSQnodes.
			RSQNode *rsqnode = _filterTreeToQueryNode(filter_tree, rs_idx);
			rsqnodes = array_append(rsqnodes, rsqnode);
		}
	}

	/* Build RediSearch query tree
	 * Convert each range object to RediSearch query node. */
	raxIterator it;
	raxStart(&it, string_ranges);
	raxSeek(&it, "^", NULL, 0);
	char query_field_name[1024];
	while(raxNext(&it)) {
		char *field = (char *)it.key;

		/* Make sure each property is bound to either numeric or string type
		 * but not to both, e.g. a.v = 1 AND a.v = 'a'
		 * in which case use an empty RSQueryNode. */
		if(raxFind(numeric_ranges, (unsigned char *)field, (int)it.key_len) != raxNotFound) {
			root = RediSearch_CreateEmptyNode(rs_idx);
			goto cleanup;
		}

		StringRange *sr = raxFind(string_ranges, (unsigned char *)field, (int)it.key_len);
		if(!StringRange_IsValid(sr)) {
			root = RediSearch_CreateEmptyNode(rs_idx);
			goto cleanup;
		}

		sprintf(query_field_name, "%.*s", (int)it.key_len, field);
		RSQNode *rsqn = _StringRangeToQueryNode(rs_idx, query_field_name, sr);
		rsqnodes = array_append(rsqnodes, rsqn);
	}
	raxStop(&it);

	raxStart(&it, numeric_ranges);
	raxSeek(&it, "^", NULL, 0);
	while(raxNext(&it)) {
		char *field = (char *)it.key;
		NumericRange *nr = raxFind(numeric_ranges, (unsigned char *)field, (int)it.key_len);

		// return empty RSQueryNode.
		if(!NumericRange_IsValid(nr)) {
			root = RediSearch_CreateEmptyNode(rs_idx);
			goto cleanup;
		}

		sprintf(query_field_name, "%.*s", (int)it.key_len, field);
		RSQNode *rsqn = _NumericRangeToQueryNode(rs_idx, query_field_name, nr);
		rsqnodes = array_append(rsqnodes, rsqn);
	}
	raxStop(&it);

	// Connect all RediSearch query nodes.
	rsqnode_count = array_len(rsqnodes);

	// No way to utilize the index.
	if(rsqnode_count == 0) goto cleanup;

	// Just a single filter.
	if(rsqnode_count == 1) {
		root = array_pop(rsqnodes);
	} else {
		// Multiple filters, combine using AND.
		root = RediSearch_CreateIntersectNode(rs_idx, false);
		for(uint i = 0; i < rsqnode_count; i++) {
			RSQNode *qnode = array_pop(rsqnodes);
			RediSearch_QueryNodeAddChild(root, qnode);
		}
	}

cleanup:
	if(string_ranges) raxFreeWithCallback(string_ranges, (void(*)(void *))StringRange_Free);
	if(numeric_ranges) raxFreeWithCallback(numeric_ranges, (void(*)(void *))NumericRange_Free);
	if(rsqnodes) array_free(rsqnodes);

	if(filters) {
		for(uint i = 0; i < filters_count; i++) {
			Filter *filter = filters[i];
			ExecutionPlan_RemoveOp(plan, (OpBase *)filter);
			OpBase_Free((OpBase *)filter);
		}
		array_free(filters);
	}

	if(root) {
		// Pass ownership of root to iterator.
		RSResultsIterator *iter = RediSearch_GetResultsIterator(root, rs_idx);
		OpBase *indexOp = NewIndexScanOp(scan->g, scan->node, scan->nodeRecIdx, rs_idx, iter);
		ExecutionPlan_ReplaceOp(plan, (OpBase *)scan, indexOp);
		OpBase_Free((OpBase *)scan);
	}
}

void utilizeIndices(GraphContext *gc, ExecutionPlan *plan) {
	// Return immediately if the graph has no indices
	if(!GraphContext_HasIndices(gc)) return;

	// Collect all label scans.
	OpBase **scanOps = ExecutionPlan_LocateOps(plan->root, OPType_NODE_BY_LABEL_SCAN);

	int scanOpCount = array_len(scanOps);
	for(int i = 0; i < scanOpCount; i++) {
		NodeByLabelScan *scanOp = (NodeByLabelScan *)scanOps[i];
		reduce_scan_op(plan, scanOp);
	}

	// Cleanup
	array_free(scanOps);
}
