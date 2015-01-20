#pragma once

#include "stacker.h"
#include "stacker_attribute_buffer.h"
#include "stacker_tree.h"

namespace stkr {

struct Document;
struct Node;
struct Box;
struct InlineContext;
struct VisualLayer;
struct Rule;

const unsigned NUM_RULE_SLOTS = 4;

/* A reference to a rule that has matched against a node, along with a copy
 * of the rule's update clock. When the clock in the reference does not match
 * the clock in the rule, the node must update itself. */
struct RuleSlot {
	const Rule *rule;
	unsigned revision;
};

/* A part of a document. */
struct Node {
	Tree t;

	struct Document *document;

	Node *hit_prev;
	Node *hit_next;

	Node *selection_prev;
	Node *selection_next;

	VisualLayer *layers;

	uint8_t type;
	uint8_t layout;
	uint8_t current_layout;
	uint8_t target_layout;
	uint8_t token;
	uint8_t num_matched_rules;
	uint8_t num_rule_keys;
	uint8_t rule_key_capacity;
	uint32_t text_length;
	uint32_t mouse_hit_stamp;
	uint32_t first_element;

	char *text;
	
	AttributeBuffer attributes;

	RuleSlot rule_slots[NUM_RULE_SLOTS];
	uint64_t *rule_keys;

	NodeStyle style;

	InlineContext *icb;

#if defined(STACKER_DIAGNOSTICS)
	char debug_info[64];
#endif
};

struct AttributeIterator {
	const struct Node *node;
	const Attribute *attribute;
	uint32_t visited[ATTRIBUTE_MASK_WORDS];
	const AttributeBuffer *buffers[1 + NUM_RULE_SLOTS];
	unsigned index;
	unsigned num_buffers;
};

NodeType node_type_for_tag(int tag_name);
Layout natural_layout(NodeType type);
Layout token_natural_layout(int token);

const Node *inline_next(const Node *container, const Node *node);
const Node *inline_first_nonempty(const Node *container);
const Node *inline_next_nonempty(const Node *container, const Node *node);
const Node *inline_next_no_objects(const Node *container, const Node *node);

const Attribute *node_first_attribute(const Node *node, 
	AttributeIterator *iterator);
const Attribute *node_next_attribute(AttributeIterator *ai);
void attribute_changed(Document *document, Node *node, int name);

const Attribute *find_attribute(const Node *node, int name);
const Attribute *find_inherited_attribute(const Node *node, int name, 
	const Node **owner = 0);
void fold_node_attributes(const Node *base, AttributeBuffer *dest, 
	const uint32_t *mask, bool base_only = false, 
	bool add_base_attributes = false);

const Node *find_layout_node(const Document *document, const Node *node);
const Node *find_inline_container(const Document *document, const Node *node);
const Node *find_inline_container_not_self(const Document *document, const Node *node);
const Node *find_chain_inline_container(const Document *document, 
	const Node *node);
void propagate_expansion_flags(Node *child, unsigned axes);
bool is_inline_child(const Document *document, const Node *node);
bool node_before(const Node *a, const Node *b);

void update_matched_rules(Document *document, Node *node);
bool must_update_rule_keys(const Node *node);

unsigned update_node_pre_layout_preorder(Document *document, Node *node, unsigned propagate_down);
unsigned update_node_pre_layout_postorder(Document *document, Node *node, unsigned propagate_up);
unsigned update_node_post_layout_postorder(Document *document, Node *node, unsigned propagate_up);

void set_interaction_state(Document *document, Node *node, 
	unsigned mask, bool value);

unsigned make_node_debug_string(const Document *document, 
	const Node *node, char *buffer, unsigned buffer_size);

extern const char * const NODE_TYPE_STRINGS[NUM_NODE_TYPES];
extern const NodeStyle DEFAULT_NODE_STYLE;


} // namespace stkr


