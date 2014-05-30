#pragma once

#include <cstdarg>
#include <climits>

#include "stacker_token.h"
#include "stacker_attribute.h"
#include "stacker_style.h"
#include "stacker_diagnostics.h"
#include "stacker_message.h"

namespace urlcache { class UrlCache; struct ParsedUrl; enum UrlFetchPriority; }

namespace stkr {

struct System;
struct BackEnd;
struct Document;
struct Message;
struct Node;
struct Box;
struct View;
struct Rule;

enum Axis { AXIS_H, AXIS_V };

enum AxisBit { 
	AXIS_BIT_H = 1 << AXIS_H,
	AXIS_BIT_V = 1 << AXIS_V
};

enum NodeType {
	LNODE_INVALID = -1,

	LNODE_BASIC,
	LNODE_TEXT,
	LNODE_HBOX,
	LNODE_VBOX,
	LNODE_PARAGRAPH,
	LNODE_HEADING,
	LNODE_HYPERLINK,
	LNODE_IMAGE,
	LNODE_USER,

	NUM_NODE_TYPES
};

enum NodeFlag {
	/* Bits that say things need to be recalculated. */
	NFLAG_PARENT_CHANGED            = 1 << 0,  // The node has been moved in the graph.
	NFLAG_UPDATE_BACKGROUND_LAYERS  = 1 << 1,  // The node's visual layer stack must be updated to reflect its current attributes.
	NFLAG_UPDATE_TEXT_LAYERS        = 1 << 2,  // The node's text layer stack must be updated. This is done post-layout.
	NFLAG_UPDATE_BOX_LAYERS         = 1 << 3,  // The box's layer stack must by synchronized with the node's.
	NFLAG_COMPUTE_ATTRIBUTES        = 1 << 4,  // Final attribute values must be recalculated.
	NFLAG_UPDATE_STYLE              = 1 << 5,  // Styles in this subtree should be recalculated. 
	NFLAG_REBUILD_BOXES             = 1 << 6,  // This node's box must be recreated.
	NFLAG_REBUILD_INLINE_CONTEXT    = 1 << 7,  // The node's inline context buffer must be rebuilt from its children.
	NFLAG_REMEASURE_INLINE_TOKENS   = 1 << 8,  // Tokens of an inline container may have changed size.
	NFLAG_RECOMPOSE_CHILD_BOXES     = 1 << 9,  // Node child boxes have changed, and should be rearranged within the parent.
	NFLAG_UPDATE_RULE_KEYS          = 1 << 10, // The set of keys used to match rules for this node must be recalculated.
	NFLAG_UPDATE_MATCHED_RULES      = 1 << 11, // The node's match rule list must be recalculated.

	/* Memory management flags. */		   
	NFLAG_HAS_STATIC_TEXT           = 1 << 12, // The node's text buffer is allocated as part of the node block. 
	NFLAG_HAS_STATIC_RULE_KEYS      = 1 << 13, // The node's rule key buffer is allocated as part of the node block.
												 
	/* Hit testing bits. */				      
	NFLAG_IN_HIT_CHAIN              = 1 << 14, // The node is a member of the most recently calculated hit set.
	NFLAG_MOUSE_OVER                = 1 << 15, // One of the node's boxes is the top of the mouse hit stack.
	NFLAG_MOUSE_OVER_CHILD          = 1 << 16, // A box of one of the node's children is the top of the mouse hit stack.
	NFLAG_MOUSE_INSIDE              = 1 << 17, // The node's box or the box of one of its children is in the hit stack.

	/* Selection bits. */				   
	NFLAG_IN_SELECTION_CHAIN        = 1 << 18, // The node is part of the selection chain.
	NFLAG_UPDATE_SELECTION_LAYERS   = 1 << 19, // The node's selection state has changed.

	/* Interaction states. */
	NFLAG_INTERACTION_HIGHLIGHTED   = 1 << 20, // Mouse over.
	NFLAG_INTERACTION_ACTIVE        = 1 << 21, // Mouse down.

	/* Set for nodes that have a box when that box changes size. */
	NFLAG_WIDTH_CHANGED             = 1 << 22,
	NFLAG_HEIGHT_CHANGED            = 1 << 23,

	/* Bits that say which direction a node expanded or contracted in. It's not
	 * always possible to say. */
	NFLAG_NOTIFY_EXPANSION          = 1 << 24, // Send messages when the node expands or contracts.
	NFLAG_EXPANDED_LEFT             = 1 << 25, // The left edge of the node has moved.
	NFLAG_EXPANDED_RIGHT            = 1 << 26, // The right edge of the node has moved.
	NFLAG_EXPANDED_UP               = 1 << 27, // The top edge of the node has moved.
	NFLAG_EXPANDED_DOWN             = 1 << 28  // The bottom edge of the node has moved.
};

const unsigned NFLAG_EXPANSION_MASK = NFLAG_EXPANDED_LEFT | 
	NFLAG_EXPANDED_RIGHT | NFLAG_EXPANDED_UP | NFLAG_EXPANDED_DOWN;

/* A position between two characters in an inline context. */
struct InternalAddress {
	unsigned token;
	unsigned offset;
};

/* A special value for InlineAddress::offset signifying the position after
 * the last character. This exists to allow us to distinguish "before" and
 * "after" positions for zero-width tokens, which would otherwise share 
 * offset 0. */
static const unsigned IA_END = UINT_MAX;

/* A tree position between any two characters. */
struct CaretAddress {
	const Node *node;
	InternalAddress ia;
};

enum DocumentFlag {
	DOCFLAG_CONSTRAIN_WIDTH         = 1 <<  0, // root_width contains a dimension to maintain on the root box.
	DOCFLAG_CONSTRAIN_HEIGHT        = 1 <<  1, // root_height contains a dimension to maintain on the root box.
	DOCFLAG_ENABLE_SELECTION        = 1 <<  2, // Allow text selection.
	DOCFLAG_SELECTING               = 1 <<  3, // Mouse selection in progress.
	DOCFLAG_HAS_SELECTION           = 1 <<  4, // Document has a final, non-empty selection.
	DOCFLAG_UPDATE_SELECTION_CHAIN  = 1 <<  5, // Selection chain must be rebuilt.
	DOCFLAG_EXTERNAL_MESSAGES       = 1 <<  6, // Unhandled document-level messages are added to a queue to be processed by the client.
	DOCFLAG_RULE_TABLE_CHANGED      = 1 <<  7, // The document rule table has changed since the last layout.
	DOCFLAG_KEEP_SOURCE             = 1 <<  8, // Keep a copy of the markup last parsed into the document.

	DOCFLAG_DEBUG_LAYOUT            = 1 <<  9, // Send layout diagnostics to the dump function.
	DOCFLAG_DEBUG_FULL_LAYOUT       = 1 << 10, // Send layout diagnostics to the dump function.
	DOCFLAG_DEBUG_PARAGRAPHS        = 1 << 11, // Dump paragraph breakpoint info.
	DOCFLAG_DEBUG_SELECTION         = 1 << 12  // Print selection hit testing messages.
};

/* The status of a document's attempt to navigate to a URL. */
enum NavigationState {
	DOCNAV_IDLE,
	DOCNAV_IN_PROGRESS,
	DOCNAV_FAILED,
	DOCNAV_PARSE_ERROR,
	DOCNAV_SUCCESS
};

enum ViewFlag {
	VFLAG_CONSTRAIN_DOCUMENT_WIDTH   = 1 <<  0, // Constrain root node width to view bounds.
	VFLAG_CONSTRAIN_DOCUMENT_HEIGHT  = 1 <<  1, // Constrain root node height to view bounds.
	VFLAG_DEBUG_OUTER_BOXES          = 1 <<  2, // Debug box visualization.
	VFLAG_DEBUG_PADDING_BOXES        = 1 <<  3, // Debug box visualization.
	VFLAG_DEBUG_CONTENT_BOXES        = 1 <<  4, // Debug box visualization.
	VFLAG_DEBUG_DIMENSIONS           = 1 <<  5, // Show box dimensions.
	VFLAG_DEBUG_PARAGRAPH            = 1 <<  6, // Show paragraph line demerits.
	VFLAG_DEBUG_MOUSE_HIT            = 1 <<  7, // Show mouse hit set.

	/* Internal. Do not use. */
	VFLAG_REBUILD_COMMANDS           = 1 << 12  // Must rebuild draw commands.
};

const unsigned VFLAG_DEBUG_MASK = 
	VFLAG_DEBUG_OUTER_BOXES | 
	VFLAG_DEBUG_PADDING_BOXES | 
	VFLAG_DEBUG_CONTENT_BOXES | 
	VFLAG_DEBUG_DIMENSIONS | 
	VFLAG_DEBUG_PARAGRAPH | 
	VFLAG_DEBUG_MOUSE_HIT;

enum SystemFlag {
	SYSFLAG_TEXT_LAYER_PALETTES = 1 << 0 // Group text clusters for the same font into a single layer containing a style palette.
};

enum Code {
	STKR_CANNOT_FOLD                    = -26,
	STKR_INVALID_SET_LITERAL            = -25,
	STKR_INVALID_OPERATION              = -24,
	STKR_NO_SUCH_ATTRIBUTE              = -23,
	STKR_TYPE_MISMATCH                  = -22,
	STKR_OUT_OF_BOUNDS                  = -21,
	STKR_INCORRECT_CONTEXT              = -20,
	STKR_MISSING_SELECTOR               = -19,
	STKR_SELECTOR_ILL_FORMED            = -18,
	STKR_SELECTOR_EMPTY                 = -17,
	STKR_SELECTOR_INVALID_CHAR          = -16,
	STKR_SELECTOR_MISSING_CLASS         = -15,
	STKR_SELECTOR_TOO_LONG              = -14,
	STKR_COLOR_COMPONENT_OUT_OF_RANGE   = -13,
	STKR_INVALID_INPUT                  = -12,
	STKR_INVALID_TAG                    = -11,
	STKR_INVALID_KEYWORD                = -10,
	STKR_MISMATCHED_TAGS                = -9,
	STKR_UNTERMINATED_STRING            = -8,
	STKR_ATTRIBUTE_VALUE_OUT_OF_BOUNDS  = -7,
	STKR_ATTRIBUTE_VALUE_TYPE_MISMATCH  = -6,
	STKR_TOO_MANY_ATTRIBUTES            = -5,
	STKR_UNEXPECTED_TOKEN               = -4,
	STKR_INVALID_NUMERIC_LITERAL        = -3,
	STKR_INVALID_TOKEN                  = -2,
	STKR_ERROR                          = -1,
	STKR_OK                             = 0
};

const unsigned MAX_SELECTOR_DEPTH         = 16;
const unsigned MAX_SELECTOR_CLAUSES       = 16;
const unsigned MAX_SELECTOR_KEYS          = MAX_SELECTOR_CLAUSES * MAX_SELECTOR_DEPTH;

/* A special priority threshold which causes attributes of a rule to override 
 * even those of the node against which it is matched. */
const int RULE_PRIORITY_OVERRIDE =  -64;
const int RULE_PRIORITY_LOWEST   =  127;
const int RULE_PRIORITY_HIGHEST  = -128;

/* Rule priority keys contain the user-supplied priority in the upper 8 bits
 * and the document order in the lower bits. */
const unsigned RULE_PRIORITY_SHIFT = 24;

enum RuleFlag {
	RFLAG_ENABLED           = 1 << 0, // Attributes in this rule should be 
	                                  // applied to matching nodes.
	RFLAG_GLOBAL            = 1 << 1, // Create a global rule.
	RFLAG_IN_DOCUMENT_TABLE = 1 << 2, // Rule is in a document rule table.
	RFLAG_IN_SYSTEM_TABLE   = 1 << 3, // Rule is in the system rule table.
	RFLAG_MODIFIES_CLASS    = 1 << 4  // Rule alters the "class" attribute.
};

/* A set of rule keys representing a multi-clause selector expression. */
struct ParsedSelector {
	uint64_t keys[MAX_SELECTOR_KEYS];
	unsigned total_keys;
	unsigned num_clauses;
	unsigned keys_per_clause[MAX_SELECTOR_CLAUSES];
};

typedef void (*DumpCallback)(void *data, const char *fmt, va_list args);

/*
 * Node
 */
int create_node(Node **result, Document *document, NodeType type, int tag_name,
	const AttributeAssignment *attributes = 0, unsigned num_attributes = 0, 
	const char *text = 0, uint32_t text_length = 0);
void destroy_node(Document *document, Node *node, bool recursive = true);
void destroy_children(Document *document, Node *node);
void insert_child_before(Document *document, Node *parent, Node *child, 
	Node *before);
void append_child(Document *document, Node *parent, Node *child);
void prepend_child(Document *document, Node *parent, Node *child);
void remove_from_parent(Document *document, Node *child);
bool is_child(const Node *child, const Node *parent);
NodeType get_type(const Node *node);
unsigned get_text_length(const Node *node);
Box *get_box(Node *node);
const Box *get_box(const Node *node);
const char *get_text(const Node *node);
LayoutContext get_layout_context(const Node *node);
unsigned get_flags(const Node *node);
void set_node_flags(Document *document, Node *node, unsigned mask, bool value);
void set_node_flags_internal(Document *document, Node *node, 
	unsigned mask, bool value);
const NodeStyle *get_style(const Node *node);
NodeStyle *get_style(Node *node);
bool is_enabled(const Node *node);
Token get_token(const Node *node);
bool send_message(Document *document, Node *node, Message *message);
const char *get_node_debug_string(const Node *node, const char *value_if_null = "NULL");
void set_node_debug_string(Node *node, const char *fmt, ...);

/*
 * Traversal
 */
const Node *parent(const Node *node);
const Node *next_sibling(const Node *node);
const Node *previous_sibling(const Node *node);
const Node *first_child(const Node *node);
const Node *last_child(const Node *node);

Node *parent(Node *node);
Node *next_sibling(Node *node);
Node *previous_sibling(Node *node);
Node *first_child(Node *node);
Node *last_child(Node *node);

/*
 * Attribute Access
 */
int read_mode(const Node *node, int name_token, int defmode = ADEF_UNDEFINED);
int read_as_integer(const Node *node, int name_token, int32_t *result, 
	int32_t defval = 0);
int read_as_float(const Node *node, int name, float *result, 
	float defval = 0.0f);
int read_as_string(const Node *node, int name, const char **out_data,
	unsigned *out_length = 0, const char *defval = 0);
int read_as_string(const Node *node, int name, char *buffer, 
	unsigned bufer_size, unsigned *out_length = 0, const char *defval = 0,
	StringSetRepresentation ssr = SSR_INTERNAL);
int read_as_url(const Node *node, int name_token, 
	urlcache::ParsedUrl **out_url, char *buffer = 0, unsigned buffer_size = 0);

int set_integer_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, int value, AttributeOperator op = AOP_SET);
int set_float_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, float value, AttributeOperator op = AOP_SET);
int set_string_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, const char *value, int length = -1, 
	AttributeOperator op = AOP_SET);

int fold_integer_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, int value, AttributeOperator op);
int fold_float_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, float value, AttributeOperator op);
int fold_string_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, const char *value, int length, AttributeOperator op);

void set_node_text(Document *document, Node *node, const char *text, 
	int length = -1);
void set_outer_dimension(Document *document, Node *node, 
	Axis axis, int dim);

/*
 * Rules
 */
int add_rule(
	Rule **result, 
	System *system, 
	Document *document, 
	const ParsedSelector *ps,
	const AttributeAssignment *attributes = 0, 
	unsigned num_attributes = 0,
	unsigned flags = 0, 
	int priority = 0);
int add_rule(
	Rule **result, 
	System *system, 
	Document *document, 
	const char *selector, 
	int selector_length,
	const AttributeAssignment *attributes = 0, 
	unsigned num_attributes = 0,
	unsigned flags = RFLAG_ENABLED, 
	int priority = 0);
void destroy_rule(Rule *rule);
unsigned get_rule_flags(const Rule *rule);
void set_rule_flags(Rule *rule, unsigned mask, bool value);
int set_integer_attribute(Rule *rule, int name, 
	ValueSemantic vs, int value);
int set_float_attribute(Rule *rule, int name, 
	ValueSemantic vs, float value);
int set_string_attribute(Rule *rule, int name, 
	ValueSemantic vs, const char *value, int length);
int parse_selector(ParsedSelector *ps, const char *s, int length = -1);
int node_matches_selector(const Document *document, const Node *node, 
	const ParsedSelector *ps);
int node_matches_selector(const Document *document, const Node *node, 
	const char *selector, int length = -1);
int match_nodes(const Document *document, const Node *root, 
	const ParsedSelector *ps, const Node **matched_nodes = 0, 
	unsigned max_matched = 0, int max_depth = -1);
int match_nodes(const Document *document, const Node *root, 
	const char *selector, int selector_length = -1, 
	const Node **matched_nodes = 0, unsigned max_matched = 0,
	int max_depth = -1);

/*
 * Box
 */
const char *get_box_debug_string(const Box *box, const char *value_if_null = "NULL");
void set_box_debug_string(Box *box, const char *fmt, ...);

/*
 * System
 */
System *create_system(unsigned flags = 0, BackEnd *back_end = 0, 
	urlcache::UrlCache *url_cache = 0);
void destroy_system(System *system);
BackEnd *get_back_end(System *system);
unsigned get_total_nodes(const System *system);
unsigned get_total_boxes(const System *system);

/*
 * Document
 */
Document *create_document(System *system, unsigned flags = 0);
void destroy_document(Document *document);
void reset_document(Document *document);
void set_document_flags(Document *document, unsigned mask, bool value);
float get_root_dimension(const Document *document, Axis axis);
void set_root_dimension(Document *document, Axis axis, unsigned dimension);
void set_layout_dump_callback(Document *document, DumpCallback layout_dump, 
	void *layout_dump_data = 0);
void update_document(Document *document);
Node *get_root(Document *document);
const Node *get_root(const Document  *document);
unsigned get_hit_clock(const Document *document);
unsigned get_layout_clock(const Document *document);
unsigned get_flags(const Document *document);
const Box *get_selection_start_anchor(const Document *document);
const Box *get_selection_end_anchor(const Document *document);
CaretAddress get_selection_start(const Document *document);
CaretAddress get_selection_end(const Document *document);
const Message *dequeue_message(Document *document);
CursorType get_cursor(const Document *document);
urlcache::ParsedUrl *get_url(const Document *document, void *buffer = 0, 
	unsigned buffer_size = 0);
int set_url(Document *document, const char *url);
int navigate(Document *document, const char *url, 
	urlcache::UrlFetchPriority priority = urlcache::UrlFetchPriority(1));
NavigationState get_navigation_state(const Document *document);
const char *get_source(const Document *document, unsigned *out_size);

/*
 * View
 */
View *create_view(Document *document, unsigned flags);
void destroy_view(View *view);
void update_view(View *view);
unsigned get_view_flags(const View *view);
void set_view_flags(View *view, unsigned flags, bool value);
unsigned get_paint_clock(const View *view);
void set_view_bounds(View *view, float x0, float x1, float y0, float y1);
void set_view_bounds(View *view, float *bounds);
void view_handle_mouse_event(View *view, MessageType type, 
	int x, int y, unsigned flags);
void view_handle_keyboard_event(View *view, MessageType type, 
	unsigned key_code, unsigned flags);

/*
 * Parser
 */
int parse(System *system, Document *document, Node *root, const char *input, 
	unsigned length, char *error_buffer = 0, unsigned max_error_size = 0);
int create_node_from_markup(
	Node **out_node, 
	Document *document, 
	const char *input, 
	unsigned length, 
	char *error_buffer = 0, 
	unsigned max_error_size = 0);

/*
 * Utilities
 */
unsigned murmur3_32(const void *key, int len, unsigned seed = 0);
uint64_t murmur3_64(const void *key, const int len, unsigned seed = 0);
uint64_t murmur3_64_cstr(const char *key, unsigned seed = 0);

} // namespace stkr
