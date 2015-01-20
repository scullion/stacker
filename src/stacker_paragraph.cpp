#include "stacker_paragraph.h"

#include <cmath>
#include <cfloat>
#include <climits>

#include <algorithm>

#include "stacker.h"
#include "stacker_util.h"
#include "stacker_encoding.h"
#include "stacker_system.h"
#include "stacker_document.h"
#include "stacker_node.h"
#include "stacker_box.h"

namespace stkr {

extern const char * const PENALTY_TYPE_STRINGS[] =
	{ "none", "prohibit-break", "multipart", "intercharacter", "force-break" };

const int PENALTIES[NUM_PENALTY_TYPES] = { 0, 10000, 50, 5000, -10000 };

const unsigned BREAKPOINT_ALLOCATION_CHUNK = 128;

void incremental_break_init(IncrementalBreakState *s)
{
	s->breakpoints = NULL;
	s->num_breakpoints = 0;
	s->max_breakpoints = 0;
	s->elements = NULL;
}

void incremental_break_deinit(IncrementalBreakState *s)
{
	if (s->breakpoints != NULL)
		delete[] s->breakpoints;
	s->breakpoints = NULL;
	s->max_breakpoints = 0;
}

static void allocate_breakpoints(IncrementalBreakState *s, unsigned count)
{
	if (count <= s->max_breakpoints)
		return;
	s->max_breakpoints = (count + BREAKPOINT_ALLOCATION_CHUNK - 1) & 
		-int(BREAKPOINT_ALLOCATION_CHUNK);
	delete[] s->breakpoints;
	s->breakpoints = new Breakpoint[s->max_breakpoints];
}

static void update_metrics(IncrementalBreakState *s)
{
	System *system = s->document->system;
	const NodeStyle *style = &s->next_node->style;
	s->next_metrics = get_font_metrics(system, style->text.font_id);
	if (s->next_element.is_inline_object) {
		const Box *box = s->next_node->t.counterpart.box;
		if (box != NULL) {
			float height = get_size(box, SSLOT_INTRINSIC, AXIS_V);
			s->next_height = round_float_to_fixed(height, TEXT_METRIC_PRECISION);
		} else {
			s->next_height = 0;
		}
	} else {
		s->next_height = s->next_metrics->height;
	}
}

static void maybe_update_metrics(IncrementalBreakState *s)
{
	/* No need to do anything if the next element comes from the same node as
	 * the current element. */
	if (!s->next_element.is_node_first)
		return;
	/* No need to do anything if the new node has the same font as the old. */
	s->next_node = inline_next_nonempty(s->container, s->node);
	const TextStyle *s1 = &s->node->style.text;
	const TextStyle *s2 = &s->next_node->style.text;
	if (measurement_compatible(s1, s2) && !s->element.is_inline_object && 
		!s->next_element.is_inline_object)
		return;
	update_metrics(s);
}

void incremental_break_begin(IncrementalBreakState *s, const Document *document,
	const Node *container, int line_width)
{
	const InlineContext *icb = container->icb;

	s->document = document;
	s->container = container;
	s->elements = icb->elements;
	s->num_elements = icb->num_elements;

	/* Conservatively allocate the breakpoint buffer. */
	allocate_breakpoints(s, icb->num_elements + 1);

	/* Negative line widths count as infinity. Just because we have infinite
	 * width, doesn't mean the result is necessarily a single line, because the 
	 * paragraph may contain forced breaks. */
	if (line_width < 0)
		line_width = 10000;
	s->max_width = int_to_fixed(line_width, TEXT_METRIC_PRECISION);

	s->num_breakpoints = 0;
	s->num_active = 0;

	/* Start with one active breakpoint before the first element. */
	s->breakpoints[0].b = 0;
	s->breakpoints[0].unscaled = false;
	s->breakpoints[0].predecessor = -1;
	s->breakpoints[0].total_demerits = 0;
	s->breakpoints[0].width = 0;
	s->breakpoints[0].height = 0;
	s->num_breakpoints = 1;
	s->active[0].offset = 0;
	s->active[0].unscaled = false;
	s->active[0].shrink = 0;
	s->active[0].stretch = 0;
	s->active[0].width = 0;
	s->active[0].height = 0;
	s->num_active = 1;

	s->trailing_space = 0;
	s->trailing_stretch = 0;
	s->trailing_shrink = 0;

	/* Initialize the element iterator. */
	s->position = 0;
	if (s->num_elements != 0) {
		s->next_element = s->elements[0];
		s->next_node = inline_first_nonempty(container);
		update_metrics(s);
	}
}

/* Computes the adjustment ratio R according to whether the ideal width of the 
 * line from A to B is less than or greater than the desired line width, and
 * from that, an appoximation to the badness 100r^3. */
static int calculate_badness(const IncrementalBreakState *s, 
	const ActiveBreakpoint *ab)
{
	int slack = s->max_width - ab->width;
	if (slack == 0 || ab->unscaled)
		return 0; /* A perfect fit. */

	/* If the line is too long, use the total shrink. If it's too short, use
	 * the total stretch. */
	int stretch_or_shrink = (slack < 0) ? ab->shrink : ab->stretch;

	/* Calculate the adjustment ratio r = slack / stretch_or_shrink, scaled such
	 * that r_scaled ^ 3 does not overflow a 31-bit integer when r is the
	 * maximum value of interest, max_r ~= (10,000 / 100) ^ (1/3) ~= 4.64. */
	int r_scaled;
	int denom = round_fixed_to_int(stretch_or_shrink, TEXT_METRIC_PRECISION);
	if (denom != 0) {
		r_scaled = 277 * round_fixed_to_int(slack, TEXT_METRIC_PRECISION) / denom;
	} else {
		denom = round_fixed_to_int(ab->width, TEXT_METRIC_PRECISION);
		if (slack >= 0 && denom != 0) {
			/* Lines with no stretch are very bad, but if they are the only 
			 * option, we should order among them to favour those with less 
			 * slack. */
			int limit_rounded = round_fixed_to_int(s->max_width, 
				TEXT_METRIC_PRECISION);
			r_scaled = 800 + 105 * limit_rounded / denom;
		} else {
			return INFINITE_BADNESS;
		}
	}
	if (r_scaled > 1290)
		return INFINITE_BADNESS;

	/* Scale such that badness(max_r) ~= 10,000. */
	return r_scaled * r_scaled * r_scaled / 214668;
}

/* Computes the demerits for a line. */
static int calculate_demerits(ParagraphElement e, int badness)
{
	int demerits = 1 + badness;
	demerits = abs(demerits) >= INFINITE_BADNESS ? 
		INFINITE_DEMERITS : demerits * demerits;
	int penalty = PENALTIES[e.penalty_type];
	return demerits + abs(penalty) * penalty;
}

static int calculate_adjustment_ratio(int max_width, int unscaled_width, 
	int stretch_or_shrink)
{
	int slack = max_width - unscaled_width;
	if (slack == 0 || stretch_or_shrink == 0)
		return 0; /* A perfect fit, or R undefined. */
	/* This is approximate, but adequete in practice and allows us to avoid
	 * multiword division. */
	return slack / round_fixed_to_int(stretch_or_shrink, TEXT_METRIC_PRECISION);
}

int adjust_glue(int ratio, int width, int stretch, int shrink)
{
	int m = ratio < 0 ? shrink : stretch;
	return width + fixed_multiply(m, ratio, TEXT_METRIC_PRECISION);
}

static bool build_breakpoint(IncrementalBreakState *s, ParagraphElement e,
	unsigned position)
{
	Breakpoint *b = s->breakpoints + s->num_breakpoints;
	b->unscaled = false;
	b->b = (int)position;
	b->total_demerits = INT_MAX;
	for (unsigned j = 0; j < s->num_active; ++j) {
		/* Score the line. */
		const ActiveBreakpoint *ab = s->active + j;
		int badness = calculate_badness(s, ab);
		int demerits = calculate_demerits(e, badness);

		/* Is 'a' the best line start candidate we have discovered so far? */
		const Breakpoint *a = s->breakpoints + ab->offset;
		if (a->total_demerits + demerits < b->total_demerits ||
 			(e.penalty_type == PENALTY_FORCE_BREAK && j == 0)) {
			assertb((unsigned)ab->offset < s->num_breakpoints);
			b->predecessor = ab->offset;
			b->unscaled = ab->unscaled;
			b->stretch_or_shrink = (s->max_width > (int)ab->width) ? 
				ab->stretch : ab->shrink;
			b->width = ab->width;
			b->height = ab->height;
			b->total_demerits = a->total_demerits + demerits;
		}
	}
	
	bool have_breakpoint = b->total_demerits != INT_MAX;
	if (e.penalty_type == PENALTY_FORCE_BREAK) {
		/* If we have no breakpoint, it's because the active set is empty.
		 * Honour the forced break by adding an empty line. */
		if (!have_breakpoint) {
			*b = s->breakpoints[0];
			b->total_demerits = 0;
			b->stretch_or_shrink = 0;
			b->width = 0;
			b->height = s->metrics->height;
			have_breakpoint = true;
		}
		/* To ensure that all forced breakpoints are included in the solution, 
		 * we empty the active set before adding one. This prevents subsequent 
		 * breakpoints from "reaching behind" the forced break, causing it 
		 * not to be included. */
		s->num_active = 0;
	}

	s->num_breakpoints += have_breakpoint;
	return have_breakpoint;
}

/* Adds the last breakpoint to the active set, displacing the worst-scoring
 * line if the set is full. */
static void activate_breakpoint(IncrementalBreakState *s)
{
	ActiveBreakpoint *ab = s->active;
	if (s->num_active == MAX_ACTIVE_BREAKPOINTS) {
		for (unsigned j = 1; j < s->num_active; ++j) {
			if (s->breakpoints[s->active[j].offset].total_demerits >
				s->breakpoints[ab->offset].total_demerits)
				ab = s->active + j;
		}
	} else {
		ab = s->active + s->num_active;
		s->num_active++;
	}
	ab->unscaled = false;
	ab->offset = s->num_breakpoints - 1;
	ab->width = -s->trailing_space;
	ab->stretch = -s->trailing_stretch;
	ab->shrink = -s->trailing_shrink;
	ab->height = 0;
}

/* Updates the width bounds of each candidate line, deactivating lines whose 
 * new minimal width exceeds the maximum line width. */
static void update_active_breakpoints(IncrementalBreakState *s, 
	ParagraphElement e)
{
	/* Handle glue immediately following the element. */
	int width = s->trailing_space + e.advance;
	int stretch = s->trailing_stretch;
	int shrink = s->trailing_shrink;
	if (e.is_word_end) {
		const FontMetrics *m = (s->metrics->space_width > 
			s->next_metrics->space_width) ? s->metrics : s->next_metrics;
		s->trailing_stretch = m->space_stretch;
		s->trailing_shrink = m->space_shrink;
		s->trailing_space = m->space_width;
	} else {
		s->trailing_space = 0;
		s->trailing_stretch = 0;
		s->trailing_shrink = 0;
	}

	/* Special case: the last line has infinite stretch. */
	bool unscaled = (s->position == s->num_elements);

	/* Update the active breakpoints, cutting out lines that become too long. */
	unsigned j = 0;
	for (unsigned i = 0; i < s->num_active; ++i) {
		s->active[j].unscaled = unscaled;
		s->active[j].offset = s->active[i].offset;
		s->active[j].width = s->active[i].width + width;
		s->active[j].height = (s->height > s->active[i].height) ? 
			s->height : s->active[i].height;
		s->active[j].stretch = s->active[i].stretch + stretch;
		s->active[j].shrink = s->active[i].shrink + shrink;
		j += s->active[j].width - s->active[j].shrink <= (int)s->max_width;
	}
	s->num_active = j > 1u ? j : 1u;
}

const unsigned LINE_LIST_HEADER_SIZE = sizeof(LineList) - sizeof(ParagraphLine);

LineList *allocate_line_list(unsigned capacity)
{
	unsigned bytes_required = LINE_LIST_HEADER_SIZE + capacity * sizeof(ParagraphLine);
	LineList *list = (LineList *)new char[bytes_required];
	list->capacity = (int)capacity;
	list->num_lines = 0;
	list->max_width = 0;
	return list;
}

LineList *allocate_static_line_list(char *buffer, unsigned buffer_size)
{
	assertb(buffer_size >= LINE_LIST_HEADER_SIZE + sizeof(ParagraphLine));
	LineList *lines = (LineList *)buffer;
	lines->capacity = -int((buffer_size - LINE_LIST_HEADER_SIZE) / 
		sizeof(ParagraphLine));
	lines->max_width = 0;
	lines->num_lines = 0;
	return lines;
}

void destroy_line_list(LineList *list)
{
	if (list != NULL && list->capacity >= 0)
		delete [] (char *)list;
}

/* Moves a break state to the next element. */
static bool next_element(IncrementalBreakState *s)
{
	if (s->position == s->num_elements)
		return false;
	s->element = s->next_element;
	s->metrics = s->next_metrics;
	s->height = s->next_height;
	s->node = s->next_node;
	if (++s->position != s->num_elements) {
		s->next_element = s->elements[s->position];
		maybe_update_metrics(s);
	}
	return true;
}

/* Computes a list of places to break a paragraph into lines. This is a simple 
 * implementation of the Knuth-Plass optimal fit algorithm [1].
 * 
 * [1] Knuth, D.E. & Plass, M.F. (1981). Breaking Paragraphs into Lines. 
 *     Software - Practice and Experience, Vol. 11, 1119-1184.
 */
bool incremental_break_update(IncrementalBreakState *s, Document *document)
{
	while (next_element(s)) {
		/* Add the element to each candidate line. */
		ParagraphElement e = s->element;
		update_active_breakpoints(s, e);
		/* Maybe break after this element. */
		if (e.penalty_type != PENALTY_PROHIBIT_BREAK) 
			if (build_breakpoint(s, e, s->position))
				activate_breakpoint(s);
		/* Have we run out of time? */
		if (check_interrupt(document))
			return false;
	}
	return true;
}

/* At the end of paragraph layout, breakpoints contain unadjusted line widths
 * and adjustment ratios that would extend the lines to flush. This function
 * computes the final adjusted width of a line and the effective adjustment
 * ratio according to the kind of justification being performed. */
int justified_width(const IncrementalBreakState *s, 
	const Breakpoint *b, Justification justification, int *out_r)
{
	int r = 0;
	bool squashing = (int)b->width > s->max_width;
	if (squashing || (justification == JUSTIFY_FLUSH && !b->unscaled)) {
		r = calculate_adjustment_ratio(s->max_width, b->width, 
			b->stretch_or_shrink);
	}
	if (out_r != NULL)
		*out_r = r;
	return (r != 0) ? s->max_width : b->width;
}

/* Finalizes the calculation of container width and height. */
static void compute_container_size(IncrementalBreakState *s, unsigned max_width, 
	unsigned total_height, unsigned num_lines, 
	unsigned *out_width, unsigned *out_height)
{
	if (out_width != NULL) {
		*out_width = max_width;
	}
	if (out_height != NULL) {
		*out_height = total_height;
		if (num_lines > 1) 
			*out_height += (num_lines - 1) * s->container->style.leading;
	}
}

/* Converts the shortest path through the breakpoint tree into an array of 
 * line objects. */
LineList *incremental_break_build_lines(IncrementalBreakState *s, 
	LineList *lines, unsigned *out_width, unsigned *out_height)
{
	assertb(s->position == s->num_elements);

	/* Count the lines. */
	const Breakpoint *a, *b;
	unsigned num_lines = 0;
	for (b = s->breakpoints + s->num_breakpoints - 1; b->predecessor >= 0; 
		b = s->breakpoints + b->predecessor)
		num_lines++;

	/* Reallocate the line list. */
	if (lines == NULL || (int)num_lines > lines->capacity) {
		if (lines != NULL)
			destroy_line_list(lines);
		lines = allocate_line_list(num_lines);
	}

	/* Build the line objects from the breakpoint tree. The lowest-cost path 
	 * terminates at the final breakpoint, and we omit the breakpoint at 
	 * position 0. */
	unsigned max_width = 0;
	unsigned total_height = 0;
	Justification justification = (Justification)s->container->style.justification;
	if (justification == ADEF_UNDEFINED)
		justification = JUSTIFY_FLUSH; /* FIXME (TJM): should always have defined justification here. */
	ParagraphLine *line = lines->lines + num_lines - 1;
	for (b = s->breakpoints + s->num_breakpoints - 1; 
		b->predecessor >= 0; b = a) {
		a = s->breakpoints + b->predecessor;
		line->a = a->b;
		line->b = b->b;
		line->demerits = b->total_demerits;
		line->line_demerits = b->total_demerits - a->total_demerits;
		line->width = justified_width(s, b, justification, &line->adjustment_ratio);
		line->width = fixed_ceil_as_int(line->width, TEXT_METRIC_PRECISION);
		line->height = fixed_ceil_as_int(b->height, TEXT_METRIC_PRECISION);
		total_height += line->height;
		if (line->width > max_width)
			max_width = line->width;
		line--;
	}
	lines->max_width = round_fixed_to_int(s->max_width, TEXT_METRIC_PRECISION);
	lines->num_lines = num_lines;
	compute_container_size(s, max_width, total_height, num_lines, 
		out_width, out_height);
	return lines;
}

/* Calculates the dimensions of a broken paragraph without building a 
 * line list. */
unsigned incremental_break_compute_size(IncrementalBreakState *s, 
	unsigned *out_width, unsigned *out_height)
{
	assertb(s->position == s->num_elements);

	Justification justification = (Justification)s->container->style.justification;
	if (justification == ADEF_UNDEFINED)
		justification = JUSTIFY_FLUSH; /* FIXME (TJM): should always have defined justification here. */

	unsigned max_width = 0;
	unsigned total_height = 0;
	unsigned num_lines = 0;
	for (const Breakpoint *b = s->breakpoints + s->num_breakpoints - 1; 
		b->predecessor >= 0; b = s->breakpoints + b->predecessor) {
		total_height += fixed_ceil_as_int(b->height, TEXT_METRIC_PRECISION);
		unsigned w = justified_width(s, b, justification, NULL);
		w = fixed_ceil_as_int(w, TEXT_METRIC_PRECISION);
		if (w > max_width)
			max_width = w;
		num_lines++;
	}
	compute_container_size(s, max_width, total_height, num_lines, 
		out_width, out_height);

	return num_lines;
}

/* Helper to advance a paragraph element iterator to the first element of the
 * next group. */
inline bool ei_begin_group(ParagraphIterator *ei)
{
	ei->offset += ei->count;
	ei->child = ei->next_child;
	ei->style = ei->next_style;
	ei->text_start = ei->text_end;
	ei->text_end = ei->text_end;
	ei->count = 0;
	if (ei->offset == ei->end)
		return false;
	ei->count = 1;
	ei->text_end += encoded_length(ei->elements[ei->offset].code_point, ei->encoding_mask);
	return true;
}

/* Advances a paragraph element iterator to the next inline child. */
inline void ei_next_child(ParagraphIterator *ei)
{
	ei->next_child = inline_next_nonempty(ei->container, ei->next_child);
	ensure(ei->next_child != NULL);
	ei->next_style = &ei->next_child->style.text;
}

/* Expands the range of a paragraph element iterator to enclose elements up to
 * but not including the first element of the next inline child. Returns false
 * if the modified range ends at the iteration limit. */
inline bool ei_expand_to_style_boundary(ParagraphIterator *ei)
{
	while (ei->offset + ei->count != ei->end) {
		if (ei->elements[ei->offset + ei->count].is_node_first) {
			ei_next_child(ei);
			return true;
		}
		ei->count++;
	}
	return false;
}

/* Expands the current group in a paragraph element iterator to enclose elements
 * up to but not including the first element of the next inline child, or the
 * first element of the next line, whichever comes first. Returns true if the
 * updated group terminates at a style boundary and could be expanded 
 * further. */
inline bool ei_expand_to_placement_boundary(ParagraphIterator *ei)
{
	while (ei->offset + ei->count != ei->end) {
		const ParagraphElement *e = ei->elements + ei->offset + ei->count;
		if (e->is_node_first) 
			ei_next_child(ei);
		/* Note that when we stop at EOL, we must still advance to the next
		 * child if the current element is node-first. */
		if (ei->offset + ei->count == ei->eol)
			break;
		if (e->is_node_first)
			return !(e->is_inline_object || e[-1].is_inline_object);
		ei->count++;
	}
	return false;
}

inline bool ei_skip_inline_objects(ParagraphIterator *ei)
{
	while (ei->elements[ei->offset].is_inline_object)
		if (++ei->offset == ei->end)
			return false;
	return true;
}

/* Shared paragraph element initializer. */
static void ei_init(ParagraphIterator *ei, const Document *document, 
	const Node *container, const Node *child, unsigned offset, unsigned end)
{
	const InlineContext *icb = container->icb;
	ei->elements = icb->elements;
	ei->document = document;
	ei->container = container;
	ei->offset = offset;
	ei->count = 0;
	ei->end = end;
	ei->eol = end;
	ei->text_start = 0;
	ei->text_end = 0;
	ei->encoding_mask = ENCODING_LENGTH_MASKS[document->system->encoding];
	ei->next_child = child;
	ei->next_style = child != NULL ? &child->style.text : NULL;
}

/* Advances a paragraph element iterator to cover the next group of elements 
 * that can be measured together, pausing to visit inline objects that are
 * part of the group (see expand_measurement_group()). */
ParagraphElement *next_measurement_group(ParagraphIterator *ei)
{
	if (!ei_begin_group(ei))
		return 0;
	return expand_measurement_group(ei);
}

/* Expands the iterator to the end of the current measurement group. If an 
 * inline object is encountered inside the group, it is returned. The caller 
 * should then repeat the call until the function returns NULL, indicating
 * that the group is complete. */
ParagraphElement *expand_measurement_group(ParagraphIterator *ei)
{
	while (ei_expand_to_style_boundary(ei)) {
		if (ei->next_child->text_length != 0 && !measurement_compatible(
			ei->style, &ei->next_child->style.text))
			break;
		ParagraphElement *e = ei->elements + ei->offset + ei->count;
		ei->count++;
		if (e->is_inline_object)
			return e;
	}
	return NULL;
}

/* Advances a paragraph element iterator to cover the next group of elements
 * that can be placed together by a box. Placement groups are guaranteed to
 * contain only text elements. */
unsigned next_placement_group(ParagraphIterator *ei)
{
	if (!ei_begin_group(ei))
		return 0;
	while (ei_expand_to_placement_boundary(ei)) {
		if (ei->next_child->text_length != 0 && !measurement_compatible(
			ei->style, &ei->next_child->style.text))
			break;
		ei->count++;
	}
	return ei->count;
}

/* Fast-forwards a placement group iterator to a position for which the caller
 * knows the child node. This is purely a performance optimization. */
void placement_iterator_jump(ParagraphIterator *ei, 
	unsigned start, const Node *child)
{
	ei->offset = start;
	ei->count = start != ei->end ? 1 : 0;
	ei->next_child = child;
	ei->next_style = (child != NULL) ? &child->style.text : NULL;
}

/* True if the current placement group is the last on the line. */
bool iterator_at_eol(const ParagraphIterator *ei)
{
	return ei->offset + ei->count == ei->eol;
}

/* Moves the iterator to the first placement group on the specified line. */
unsigned iterate_placement_groups(ParagraphIterator *ei, 
	const ParagraphLine *pl)
{
	ei->offset += ei->count;
	ei->count = 0;
	assertb(ei->offset <= pl->a);
	while (ei->offset != pl->a) {
		if (ei->elements[ei->offset].is_node_first)
			ei_next_child(ei);
		ei->offset++;
	}
	ei->eol = pl->b;
	return next_placement_group(ei);
}

/* Initializes a paragraph element iterator to visit all placement groups
 * in an inline container. */
void init_placement_group_iterator(ParagraphIterator *ei, 
	const Document *document, const Node *container)
{
	ei_init(ei, document, container, inline_first_nonempty(container), 
		0, container->icb->num_elements);
}

/* Advances a paragraph element iterator to cover the next fragment.  */
unsigned next_fragment(ParagraphIterator *ei)
{
	if (!ei_begin_group(ei))
		return 0;
	bool in_selection = ei->elements[ei->offset].is_selected;
	for (; ei->offset + ei->count != ei->end; ei->count++) {
		const ParagraphElement *e = ei->elements + ei->offset + ei->count;
		if (e->is_node_first) {
			ei_next_child(ei);
			if (!fragment_compatible(ei->style, &ei->next_child->style.text))
				break;
		}
		if (e->is_selected != in_selection)
			break;
		ei->text_end += encoded_length(e->code_point, ei->encoding_mask);
	}
	return ei->count;
}

/* Initializes a paragraph element iterator to visit all measurement groups
 * in an inline container. */
ParagraphElement *iterate_measurement_groups(ParagraphIterator *ei, 
	const Document *document, const Node *container)
{
	unsigned end = container->icb->num_elements;
	ei_init(ei, document, container, inline_first_nonempty(container), 0, end);
	return next_measurement_group(ei);
}

/* Initializes a paragraph element iterator to visit the text fragments among
 * the elements in a placement group (i.e. a box). */
unsigned iterate_fragments(ParagraphIterator *ei, 
	const Document *document, const Node *container, const Box *box)
{
	ei_init(ei, document, container, box->t.counterpart.node, 
		box->first_element, box->last_element);
	return next_fragment(ei);
}

/* True if the current fragment is part of the text selection. */
bool fragment_in_selection(const ParagraphIterator *ei)
{
	return ei->offset + ei->count != ei->end && 
		ei->elements[ei->offset + ei->count].is_selected;
}

} // namespace stkr
