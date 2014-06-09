#pragma once

#include "stacker.h"
#include "stacker_attribute_buffer.h"

namespace stkr {

struct Document;
struct Node;
struct Box;
struct InlineContext;
struct VisualLayer;
struct Rule;
enum SizingPass;

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
	struct Document *document;

	Node *parent;
	Node *first_child;
	Node *last_child;
	Node *prev_sibling;
	Node *next_sibling;

	Node *hit_prev;
	Node *hit_next;

	Node *selection_prev;
	Node *selection_next;

	Box *box;
	VisualLayer *layers;

	uint8_t type;
	uint8_t layout;
	uint8_t current_layout;
	uint8_t target_layout;
	uint8_t token;
	uint8_t num_matched_rules;
	uint8_t num_rule_keys;
	uint8_t rule_key_capacity;
	uint32_t flags;
	uint32_t text_length;
	uint32_t mouse_hit_stamp;

	char *text;
	
	AttributeBuffer attributes;

	RuleSlot rule_slots[NUM_RULE_SLOTS];
	uint64_t *rule_keys;

	NodeStyle style;

	InlineContext *inline_context;

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

const Node *tree_next_up(const Document *document, const Node *root, 
	const Node *node);
const Node *tree_next(const Document *document, const Node *root, 
	const Node *node);
const Node *inline_next(const Document *document, const Node *root,
	const Node *node);
const Node *text_next(const Document *document, const Node *root,
	const Node *node);
const Node *find_context_node(const Document *document, const Node *node);
const Node *find_inline_container(const Document *document, const Node *node);
const Node *find_chain_inline_container(const Document *document, 
	const Node *node);
void propagate_expansion_flags(Node *child, unsigned axes);
bool is_inline_child(const Document *document, const Node *node);
const Node *lowest_common_ancestor(const Node *a, const Node *b,
	const Node **below_a = 0, const Node **below_b = 0);
bool node_before(const Node *a, const Node *b);

void update_matched_rules(Document *document, Node *node);
bool must_update_rule_keys(const Node *node);

unsigned update_nodes_pre_layout(Document *document, Node *node, 
	unsigned propagate_down = 0, bool rule_tables_changed = false);
unsigned update_nodes_post_layout(Document *document, Node *node, 
	unsigned propagate_down = 0);
void do_text_layout(Document *document, Node *node);
void compute_sizes_iteratively(Document *document, SizingPass pass, Node *root);

void set_interaction_state(Document *document, Node *node, 
	unsigned mask, bool value);

unsigned make_node_debug_string(const Document *document, 
	const Node *node, char *buffer, unsigned buffer_size);

extern const char * const NODE_TYPE_STRINGS[NUM_NODE_TYPES];
extern const NodeStyle DEFAULT_NODE_STYLE;


} // namespace stkr


