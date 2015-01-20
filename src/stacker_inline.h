#pragma once

#include <cstdint>
#include <climits>

#include "stacker.h"
#include "stacker_paragraph.h"

namespace stkr {

struct Document;
struct Node;
struct Box;
struct VisualLayer;


/* Data associated with LCTX_INLINE nodes. */
struct InlineContext {
	char *text;
	unsigned text_length;
	ParagraphElement *elements;
	unsigned num_elements;
	Box *text_boxes;
	unsigned selection_start;
	unsigned selection_end;
};

/* How to decide which end of a node to return when an address being rewritten
 * with respect to that node proves to be inside it. */
enum AddressRewriteMode {
	ARW_TIES_TO_START,
	ARW_TIES_TO_END,
	ARW_TIES_TO_CLOSER
};

/* Iterator for walking the nodes in the tree between two caret positions. */
struct CaretWalker {
	CaretAddress start;
	CaretAddress end;
	Node *node;
	Node *back;
	Node *end_node;
	unsigned mask;
};

/* An interval of characters within a token that come from a particular child
 * node. */
struct TextSegment {
	unsigned start;
	unsigned end;
	const Node *child;
	unsigned child_offset;
};

/* An interval of characters within a token that have the same text style. */
struct TextStyleSegment {
	TextSegment segment;
	TextStyle style;
};

const unsigned TAB_WIDTH = 4;

/* Position of the tokenizer's text iterator. */
struct InlineTokenizerPosition {
	const char *text;
	const Node *child;
	unsigned child_offset;
	int next_char;
};

/* A micro tokenizer that produces a token stream describing the text and
 * non-text contents of an inline node. */
struct InlineTokenizer {
	unsigned mask;
	const Document *document;
	const Node *root;
	InlineTokenizerPosition pos;
	WhiteSpaceMode mode;
	unsigned repeat_count;
	char *text;
	unsigned text_length;
	ParagraphElement *elements;
	unsigned num_elements;
};


CaretAddress start_address(const Document *document, const Node *node);
CaretAddress end_address(const Document *document, const Node *node);
CaretAddress canonical_address(const Document *document, CaretAddress address);
InternalAddress closest_internal_address(const Document *document, 
	const Node *node, CaretAddress address, AddressRewriteMode mode);
CaretAddress caret_position(Document *document, const Box *box, float x);
bool caret_before(CaretAddress a, CaretAddress b);
bool caret_equal(CaretAddress a, CaretAddress b);
const Node *node_at_caret(CaretAddress address);
Node *cwalk_first(Document *document, CaretWalker *w, CaretAddress start, 
	CaretAddress end, unsigned mask);
Node *cwalk_next(Document *document, CaretWalker *w);

void rebuild_inline_context(Document *document, Node *node);
void destroy_inline_context(Document *document, Node *node);
unsigned inline_element_index(const InlineContext *icb, unsigned token);
const InlineToken *inline_element(const InlineContext *icb, unsigned token);
InternalAddress expand_internal_address(const Node *node, InternalAddress ia);
bool same_internal_address(const Node *node, const InternalAddress a, 
	const InternalAddress b);
InternalAddress inline_before(const InlineContext *icb, const Node *child);
InternalAddress inline_after(const InlineContext *icb, const Node *child);
TextSegment token_first_segment(const Node *container, const InlineToken *token);
TextSegment token_next_segment(const Node *container, const InlineToken *token, 
	const TextSegment *segment);
float token_character_position(const Document *document, 
	const Node *node, const InlineToken *token, unsigned offset);
TextStyleSegment token_first_style_segment(const Document *document,
	const Node *node, const InlineToken *token);
TextStyleSegment token_next_style_segment(const Document *document,
	const Node *node, const InlineToken *token, const TextStyleSegment *ss);
unsigned read_inline_text(const Document *document, const Node *node,
	InternalAddress start, InternalAddress end, char *buffer = 0);
void rebuild_inline_context(Document *document, Node *node);
void destroy_inline_context(Document *document, Node *node);
void measure_inline_tokens(Document *document, Node *node, 
	bool use_positioning = false);
VisualLayer *build_text_layer_stack(Document *document, Node *node);
void update_inline_selection_layers(Document *document, Node *node);
void build_paragraph(Document *document, Node *node, Paragraph *p, 
	int hanging_indent);
void update_inline_boxes(Document *document, Box *box, float width);

extern const InternalAddress INLINE_START;
extern const InternalAddress INLINE_END;

} // namespace stkr

