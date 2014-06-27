#pragma once

#include <cstdint>
#include <climits>

#include "stacker.h"

namespace stkr {

struct Document;
struct Node;
struct Box;
struct VisualLayer;

enum InlineTokenType { 
	TTT_WORD,
	TTT_SPACE, 
	TTT_BREAK, 
	TTT_CHILD, 
	TTT_EOS,
	NUM_INLINE_TOKEN_TYPES
};

enum InlineTokenFlag {
	ITF_HAS_PARAGRAPH_BOX = 1 << 0, // Token has a corresponding PET_BOX paragraph element.
	ITF_POSITIONED        = 1 << 1, // Token is the first in the run of tokens posioned by a text box.
	ITF_MULTIPART_HEAD    = 1 << 2, // Token is the first part of a hyphenated word.
	ITF_MULTIPART_TAIL    = 1 << 3  // Token a part of a hyphenated word other than the first.
};

const char * const INLINE_TOKEN_STRINGS[NUM_INLINE_TOKEN_TYPES] = 
	{ "TTT_WORD", "TTT_SPACE", "TTT_BREAK", "TTT_CHILD", "TTT_EOS" };

/* A pseudo character returned when the tokenizer encounters a non-text child.
 * It behaves like a zero-width space, breaking any word token surrounding
 * the child into two. */
const int ITOK_CHILD = -1;

struct InlineToken {
	InlineTokenType type;
	uint16_t flags;
	unsigned start, end;
	float width, height;
	const Node *child;
	Box *text_box;
	unsigned child_offset;
};

/* Data associated with LCTX_INLINE nodes. */
struct InlineContext {
	char *text;
	unsigned *advances;
	unsigned text_length;
	InlineToken *tokens;
	unsigned num_tokens;
	Box *text_boxes;
	InternalAddress selection_start;
	InternalAddress selection_end;
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
	InlineToken token;
	unsigned repeat_count;
	char *text;
	unsigned text_length;
	InlineToken *tokens;
	unsigned num_tokens;
	unsigned chunk_length;
	unsigned max_chunk_length;
};


CaretAddress caret_start(const Document *document, const Node *node);
CaretAddress caret_end(const Document *document, const Node *node);
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
unsigned inline_token_index(const InlineContext *icb, unsigned token);
const InlineToken *inline_token(const InlineContext *icb, unsigned token);
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

