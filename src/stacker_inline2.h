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
struct TreeIterator;

/* Data associated with inline container nodes. */
struct InlineContext {
	ParagraphElement *elements;
	unsigned num_elements;
	LineList *lines;
};

/* How to decide which end of a node to return when an address being rewritten
 * with respect to that node proves to be inside it. */
enum AddressRewriteMode {
	ARW_TIES_TO_START,
	ARW_TIES_TO_END,
	ARW_TIES_TO_CLOSER
};

/* Incremental text measurement update state. */
struct TextMeasurementState {
	ParagraphIterator iterator;
	uint8_t *buffer;
	int capacity;
	unsigned *advances;
};

const unsigned BQ_CAPACITY = 8;

struct BuildQueueItem {
	const ParagraphLine *line;
	Box *box;
};

/* State used while synchronizing boxes in an inline container. */
struct InlineBoxUpdateState {
	ParagraphIterator ei;

	/* Build queue. */
	BuildQueueItem bq[BQ_CAPACITY];
	unsigned bq_head;
	unsigned bq_tail;

	/* Box free list. */
	Box *free_list_head;
	Box *free_list_tail;

	/* Line iteration state. */
	const LineList *lines;
	unsigned line_number;
	Box *line_box;
	bool must_update_bounds;
	bool eol;

	/* Line rebuild state. */
	int xpos;
	int xpos_rounded;
	unsigned debug_stamp;
};

CaretAddress start_address(const Document *document, const Node *node);
CaretAddress end_address(const Document *document, const Node *node);
bool caret_equal(CaretAddress a, CaretAddress b);
bool caret_before(CaretAddress a, CaretAddress b);
const Node *node_at_caret(CaretAddress address);

void destroy_box_text_layer(Document *d, Box *b);
VisualLayer *update_box_text_layer(Document *document, Box *box);
VisualLayer *require_selection_layer(Document *document, Box *box);
void destroy_inline_context(Document *document, Node *node);
void rebuild_inline_context(Document *document, Node *node);
CaretAddress caret_position(Document *document, const Box *box, float x);
void set_selected_element_range(Document *document, Node *node, 
	CaretAddress start, CaretAddress end);
unsigned read_selected_text(const Document *document, const Node *container, 
	void *buffer, TextEncoding encoding);

void measurement_init(TextMeasurementState *ms, Document *document, 
	Node *container, uint8_t *buffer = 0, unsigned buffer_size = 0);
void measurement_deinit(TextMeasurementState *ms);
bool measurement_continue(TextMeasurementState *ms, Document *document, 
	Node *container);

void box_update_init(InlineBoxUpdateState *s, Document *document, 
	Node *container);
bool box_update_continue(InlineBoxUpdateState *s, Document *document);

const Node *cwalk_first(const Document *document, TreeIterator *ti, 
	CaretAddress start, CaretAddress end);
const Node *cwalk_next(const Document *document, TreeIterator *ti);

unsigned start_of_containing_line(const Document *document, const Box *box);
unsigned end_of_containing_line(const Document *document, const Box *box);

} // namespace stkr


