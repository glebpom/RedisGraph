/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "op_create.h"
#include "../../util/arr.h"
#include "../../schema/schema.h"
#include <assert.h>

// Resolve the properties specified in the query into constant values.
PendingProperties *_ConvertPropertyMap(GraphContext *gc, Record r, const PropertyMap *map) {
	PendingProperties *converted = rm_malloc(sizeof(PendingProperties));
	converted->property_count = map->property_count;
	converted->attr_keys = map->keys; // This pointer can be copied directly.
	converted->values = rm_malloc(sizeof(SIValue) * map->property_count);
	for(int i = 0; i < map->property_count; i++) {
		converted->values[i] = AR_EXP_Evaluate(map->values[i], r);
	}

	return converted;
}

// Commit properties to the GraphEntity.
static void _AddProperties(OpCreate *op, GraphEntity *ge, PendingProperties *props) {
	for(int i = 0; i < props->property_count; i++) {
		GraphEntity_AddProperty(ge, props->attr_keys[i], props->values[i]);
	}

	if(op->stats) op->stats->properties_set += props->property_count;
}

// Free the properties that have been committed to the graph
static void _PendingPropertiesFree(PendingProperties *props) {
	if(props == NULL) return;
	// The 'keys' array belongs to the original PropertyMap, so so shouldn't be freed here.
	for(uint j = 0; j < props->property_count; j ++) {
		SIValue_Free(&props->values[j]);
	}
	rm_free(props->values);
	rm_free(props);
}

OpBase *NewCreateOp(ResultSetStatistics *stats, NodeCreateCtx *nodes, EdgeCreateCtx *edges) {
	OpCreate *op_create = calloc(1, sizeof(OpCreate));
	op_create->gc = GraphContext_GetFromTLS();
	op_create->records = NULL;
	op_create->nodes_to_create = nodes;
	op_create->edges_to_create = edges;
	op_create->created_nodes = array_new(Node *, 0);
	op_create->created_edges = array_new(Edge *, 0);
	op_create->node_properties = array_new(PendingProperties *, 0);
	op_create->edge_properties = array_new(PendingProperties *, 0);
	op_create->stats = stats;

	// Set our Op operations
	OpBase_Init(&op_create->op);
	op_create->op.name = "Create";
	op_create->op.type = OPType_CREATE;
	op_create->op.consume = OpCreateConsume;
	op_create->op.init = OpCreateInit;
	op_create->op.reset = OpCreateReset;
	op_create->op.free = OpCreateFree;

	uint node_blueprint_count = array_len(nodes);
	uint edge_blueprint_count = array_len(edges);
	// Construct the array of IDs this operation modifies
	op_create->op.modifies = array_new(uint, node_blueprint_count + edge_blueprint_count);
	for(uint i = 0; i < node_blueprint_count; i ++) {
		op_create->op.modifies = array_append(op_create->op.modifies, nodes[i].node_idx);
	}
	for(uint i = 0; i < edge_blueprint_count; i ++) {
		// TODO should this also add the src and dest IDs?
		op_create->op.modifies = array_append(op_create->op.modifies, edges[i].edge_idx);
	}

	return (OpBase *)op_create;
}

void _CreateNodes(OpCreate *op, Record r) {
	uint nodes_to_create_count = array_len(op->nodes_to_create);
	for(uint i = 0; i < nodes_to_create_count; i++) {
		/* Get specified node to create. */
		QGNode *n = op->nodes_to_create[i].node;

		/* Create a new node. */
		Node *newNode = Record_GetNode(r, op->nodes_to_create[i].node_idx);
		newNode->entity = NULL;
		newNode->label = n->label;
		newNode->labelID = n->labelID;

		/* Convert query-level properties. */
		PropertyMap *map = op->nodes_to_create[i].properties;
		PendingProperties *converted_properties = NULL;
		if(map) converted_properties = _ConvertPropertyMap(op->gc, r, map);

		/* Save node for later insertion. */
		op->created_nodes = array_append(op->created_nodes, newNode);

		/* Save properties to insert with node. */
		op->node_properties = array_append(op->node_properties, converted_properties);

	}
}

void _CreateEdges(OpCreate *op, Record r) {
	uint edges_to_create_count = array_len(op->edges_to_create);
	for(uint i = 0; i < edges_to_create_count; i++) {
		/* Get specified edge to create. */
		QGEdge *e = op->edges_to_create[i].edge;

		/* Retrieve source and dest nodes. */
		Node *src_node = Record_GetNode(r, op->edges_to_create[i].src_idx);
		Node *dest_node = Record_GetNode(r, op->edges_to_create[i].dest_idx);

		/* Create the actual edge. */
		Edge *newEdge = Record_GetEdge(r, op->edges_to_create[i].edge_idx);
		if(array_len(e->reltypes) > 0) newEdge->relationship = e->reltypes[0];
		Edge_SetSrcNode(newEdge, src_node);
		Edge_SetDestNode(newEdge, dest_node);

		/* Convert query-level properties. */
		PropertyMap *map = op->edges_to_create[i].properties;
		PendingProperties *converted_properties = NULL;
		if(map) converted_properties = _ConvertPropertyMap(op->gc, r, map);

		/* Save edge for later insertion. */
		op->created_edges = array_append(op->created_edges, newEdge);

		/* Save properties to insert with node. */
		op->edge_properties = array_append(op->edge_properties, converted_properties);
	}
}

/* Commit insertions. */
static void _CommitNodes(OpCreate *op) {
	Node *n;
	GraphContext *gc = op->gc;
	Graph *g = gc->g;

	/* Create missing schemas.
	 * this loop iterates over the CREATE pattern, e.g.
	 * CREATE (p:Person)
	 * As such we're not expecting a large number of iterations. */
	uint blueprint_node_count = array_len(op->nodes_to_create);
	for(uint i = 0; i < blueprint_node_count; i++) {
		NodeCreateCtx *node_ctx = op->nodes_to_create + i;

		const char *label = node_ctx->node->label;
		if(label) {
			if(GraphContext_GetSchema(gc, label, SCHEMA_NODE) == NULL) {
				Schema *s = GraphContext_AddSchema(gc, label, SCHEMA_NODE);
				op->stats->labels_added++;
			}
		}
	}

	uint node_count = array_len(op->created_nodes);
	Graph_AllocateNodes(g, node_count);

	for(uint i = 0; i < node_count; i++) {
		n = op->created_nodes[i];
		Schema *s = NULL;

		// Get label ID.
		int labelID = GRAPH_NO_LABEL;
		if(n->label != NULL) {
			s = GraphContext_GetSchema(op->gc, n->label, SCHEMA_NODE);
			assert(s);
			labelID = s->id;
		}

		// Introduce node into graph.
		Graph_CreateNode(g, labelID, n);

		if(op->node_properties[i]) _AddProperties(op, (GraphEntity *)n, op->node_properties[i]);

		if(s && Schema_HasIndices(s)) Schema_AddNodeToIndices(s, n, false);
	}
}

static void _CommitEdges(OpCreate *op) {
	Edge *e;
	GraphContext *gc = op->gc;
	Graph *g = op->gc->g;

	/* Create missing schemas.
	 * this loop iterates over the CREATE pattern, e.g.
	 * CREATE (p:Person)-[e:VISITED]->(q)
	 * As such we're not expecting a large number of iterations. */
	uint blueprint_edge_count = array_len(op->edges_to_create);
	for(uint i = 0; i < blueprint_edge_count; i++) {
		EdgeCreateCtx *edge_ctx = op->edges_to_create + i;

		const char **reltypes = edge_ctx->edge->reltypes;
		if(reltypes) {
			uint reltype_count = array_len(reltypes);
			for(uint j = 0; j < reltype_count; j ++) {
				const char *reltype = reltypes[j];
				if(GraphContext_GetSchema(gc, reltype, SCHEMA_EDGE) == NULL) {
					Schema *s = GraphContext_AddSchema(gc, reltype, SCHEMA_EDGE);
				}
			}
		}
	}

	int relationships_created = 0;

	uint edge_count = array_len(op->created_edges);
	for(uint i = 0; i < edge_count; i++) {
		e = op->created_edges[i];
		NodeID srcNodeID;
		NodeID destNodeID;

		// Nodes which already existed prior to this query would
		// have their ID set under e->srcNodeID and e->destNodeID
		// Nodes which are created as part of this query would be
		// saved under edge src/dest pointer.
		if(e->srcNodeID != INVALID_ENTITY_ID) srcNodeID = e->srcNodeID;
		else srcNodeID = ENTITY_GET_ID(Edge_GetSrcNode(e));
		if(e->destNodeID != INVALID_ENTITY_ID) destNodeID = e->destNodeID;
		else destNodeID = ENTITY_GET_ID(Edge_GetDestNode(e));

		Schema *schema = GraphContext_GetSchema(op->gc, e->relationship, SCHEMA_EDGE);
		if(!schema) schema = GraphContext_AddSchema(op->gc, e->relationship, SCHEMA_EDGE);
		int relation_id = schema->id;

		if(!Graph_ConnectNodes(g, srcNodeID, destNodeID, relation_id, e)) continue;
		relationships_created++;

		if(op->edge_properties[i]) _AddProperties(op, (GraphEntity *)e, op->edge_properties[i]);
	}

	op->stats->relationships_created += relationships_created;
}

static void _CommitNewEntities(OpCreate *op) {
	Graph *g = op->gc->g;

	// Lock everything.
	Graph_AcquireWriteLock(g);
	Graph_SetMatrixPolicy(g, RESIZE_TO_CAPACITY);
	uint node_count = array_len(op->created_nodes);
	if(node_count > 0) _CommitNodes(op);
	if(array_len(op->created_edges) > 0) _CommitEdges(op);
	Graph_SetMatrixPolicy(g, SYNC_AND_MINIMIZE_SPACE);
	// Release lock.
	Graph_ReleaseLock(g);

	op->stats->nodes_created += node_count;
}

static Record _handoff(OpCreate *op) {
	Record r = NULL;
	if(array_len(op->records)) r = array_pop(op->records);
	return r;
}

OpResult OpCreateInit(OpBase *opBase) {
	return OP_OK;
}

Record OpCreateConsume(OpBase *opBase) {
	OpCreate *op = (OpCreate *)opBase;
	Record r;

	// Return mode, all data was consumed.
	if(op->records) return _handoff(op);

	// Consume mode.
	op->records = array_new(Record, 32);

	// No child operation to call.
	OpBase *child = NULL;
	if(!op->op.childCount) {
		r = Record_New(opBase->record_map->record_len);
		/* Create entities. */
		_CreateNodes(op, r);
		_CreateEdges(op, r);

		// Save record for later use.
		op->records = array_append(op->records, r);
	} else {
		// Pull data until child is depleted.
		child = op->op.children[0];
		while((r = OpBase_Consume(child))) {
			if(Record_length(r) < opBase->record_map->record_len) {
				// If the child record was created in a different segment, it may not be
				// large enough to accommodate the new entities.
				Record_Extend(&r, opBase->record_map->record_len);
			}
			/* Create entities. */
			_CreateNodes(op, r);
			_CreateEdges(op, r);

			// Save record for later use.
			op->records = array_append(op->records, r);
		}
	}

	/* Done reading, we're not going to call consume any longer
	 * there might be operations e.g. index scan that need to free
	 * index R/W lock, as such free all execution plan operation up the chain. */
	if(child) OpBase_PropagateFree(child);

	// Create entities.
	_CommitNewEntities(op);

	// Return record.
	return _handoff(op);
}

OpResult OpCreateReset(OpBase *ctx) {
	OpCreate *op = (OpCreate *)ctx;
	return OP_OK;
}

void OpCreateFree(OpBase *ctx) {
	OpCreate *op = (OpCreate *)ctx;

	if(op->records) {
		uint rec_count = array_len(op->records);
		for(uint i = 0; i < rec_count; i++) Record_Free(op->records[i]);
		array_free(op->records);
		op->records = NULL;
	}

	if(op->nodes_to_create) {
		uint nodes_to_create_count = array_len(op->nodes_to_create);
		for(uint i = 0; i < nodes_to_create_count; i ++) {
			PropertyMap_Free(op->nodes_to_create[i].properties);
		}
		array_free(op->nodes_to_create);
		op->nodes_to_create = NULL;
	}

	if(op->edges_to_create) {
		uint edges_to_create_count = array_len(op->edges_to_create);
		for(uint i = 0; i < edges_to_create_count; i ++) {
			PropertyMap_Free(op->edges_to_create[i].properties);
		}
		array_free(op->edges_to_create);
		op->edges_to_create = NULL;
	}

	if(op->created_nodes) {
		array_free(op->created_nodes);
		op->created_nodes = NULL;
	}

	if(op->created_edges) {
		array_free(op->created_edges);
		op->created_edges = NULL;
	}

	// Free all graph-committed properties associated with nodes
	uint prop_count = array_len(op->node_properties);
	for(uint i = 0; i < prop_count; i ++) {
		_PendingPropertiesFree(op->node_properties[i]);
	}
	array_free(op->node_properties);
	op->node_properties = NULL;

	// Free all graph-committed properties associated withedges
	prop_count = array_len(op->edge_properties);
	for(uint i = 0; i < prop_count; i ++) {
		_PendingPropertiesFree(op->edge_properties[i]);
	}
	array_free(op->edge_properties);
	op->edge_properties = NULL;
}
