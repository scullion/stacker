#pragma once

#include <cstdint>

#include "stacker_style.h"

namespace stkr {

struct Document;
struct Node;
struct Box;
struct FontMetrics;

const unsigned MAX_ACTIVE_BREAKPOINTS = 16;
const unsigned PARAGRAPH_INDEX_BITS   = 31;
const unsigned MAX_PARAGRAPH_ELEMENTS = (1u << PARAGRAPH_INDEX_BITS) - 1;
const int      INFINITE_LINE_WIDTH    = -1;
const int      INFINITE_BADNESS       = 10000;
const int      INFINITE_DEMERITS      = 10000 * 10000;
const unsigned TEXT_METRIC_PRECISION  = 16;

/* Indicates which of the line breaker's set of penalty values should be applied
 * to the position following a paragraph element. */
enum PenaltyType {
	PENALTY_NONE,           // Zero penalty. Used at word ends.
	PENALTY_PROHIBIT_BREAK, // May not break here (large positive penalty).
	PENALTY_MULTIPART,      // Used after hyphens.
	PENALTY_INTERCHARACTER, // Used between ordinary characters in a word.
	PENALTY_FORCE_BREAK,    // Must break here (large negative penalty).
	NUM_PENALTY_TYPES       // Must fit in 3 bits.
};

/* Represents a single character or inline object for the purposes of paragraph 
 * layout. */
struct ParagraphElement {
	unsigned advance;
	unsigned code_point   : 21;
	unsigned penalty_type : 3;
	bool is_word_end      : 1;
	bool is_inline_object : 1;
	bool is_node_first    : 1;
	bool is_selected      : 1;
	unsigned spare        : 3;
};

/* An interval of paragraph elements to be displayed as a line. */
struct ParagraphLine {
	unsigned a, b;
	int demerits;
	int line_demerits;
	int adjustment_ratio;
	unsigned width;
	unsigned height;
};

/* A list of breakpoints for a paragraph. */
struct LineList {
	int max_width;
	unsigned num_lines;
	int capacity;
	ParagraphLine lines[1];
};

/* Co-iterator for paragraph elements and the nodes that generated them. */
struct ParagraphIterator {
	const Document *document;
	const Node *container;
	ParagraphElement *elements;
	
	const Node *child;
	const Node *next_child;
	const TextStyle *style;
	const TextStyle *next_style;

	unsigned offset;
	unsigned count;
	unsigned eol;
	unsigned end;
	
	unsigned text_start;
	unsigned text_end;
	unsigned encoding_mask;
};

/* Places the paragraph could be broken. */
struct Breakpoint {
	int b : 31;
	bool unscaled : 1;
	int predecessor;
	int total_demerits;
	int stretch_or_shrink;
	unsigned width;
	unsigned height;
};

/* Candidate line starts. */
struct ActiveBreakpoint {
	int offset : 31;
	bool unscaled : 1;
	int width;
	int stretch;
	int shrink;
	unsigned height;
};

/* Incremental paragraph layout state. */
struct IncrementalBreakState {
	const Document *document;
	const Node *container;

	const ParagraphElement *elements;
	const unsigned *advances;
	unsigned num_elements;
	unsigned num_groups;
	int max_width;

	Breakpoint *breakpoints;
	unsigned num_breakpoints;
	unsigned max_breakpoints;

	ActiveBreakpoint active[MAX_ACTIVE_BREAKPOINTS];
	unsigned num_active;

	unsigned position;
	const Node *node;
	const Node *next_node;
	ParagraphElement element;
	ParagraphElement next_element;
	const FontMetrics *metrics;
	const FontMetrics *next_metrics;
	unsigned height;
	unsigned next_height;
	int trailing_space;
	int trailing_stretch;
	int trailing_shrink;
};

extern const char * const PENALTY_TYPE_STRINGS[];

void placement_iterator_jump(ParagraphIterator *ei, unsigned start, const Node *child);
bool iterator_at_eol(const ParagraphIterator *ei);
unsigned next_placement_group(ParagraphIterator *ei);
unsigned iterate_placement_groups(ParagraphIterator *ei, const ParagraphLine *pl);
void init_placement_group_iterator(ParagraphIterator *ei, 
	const Document *document, const Node *container);

ParagraphElement *iterate_measurement_groups(ParagraphIterator *ei, 
	const Document *document, const Node *container);
ParagraphElement *next_measurement_group(ParagraphIterator *ei);
ParagraphElement *expand_measurement_group(ParagraphIterator *ei);

unsigned next_fragment(ParagraphIterator *ei);
unsigned iterate_fragments(ParagraphIterator *ei, 
	const Document *document, const Node *container, const Box *box);
bool fragment_in_selection(const ParagraphIterator *ei);

void incremental_break_init(IncrementalBreakState *s);
void incremental_break_deinit(IncrementalBreakState *s);
void incremental_break_begin(IncrementalBreakState *s, 
	const Document *document, const Node *container, 
	int line_width);
bool incremental_break_update(IncrementalBreakState *s, Document *document);
LineList *incremental_break_build_lines(IncrementalBreakState *s, LineList *lines = 0, 
	unsigned *out_width = 0, unsigned *out_height = 0);
unsigned incremental_break_compute_size(IncrementalBreakState *s, 
	unsigned *out_width, unsigned *out_height);
int adjust_glue(int ratio, int width, int stretch, int shrink);

LineList *allocate_line_list(unsigned capacity);
LineList *allocate_static_line_list(char *buffer, unsigned buffer_size);
void destroy_line_list(LineList *list);

} // namespace stkr
