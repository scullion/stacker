#include "stacker_inline2.h"

#include <cstdint>

#include "stacker_system.h"
#include "stacker_encoding.h"
#include "stacker_node.h"
#include "stacker_document.h"
#include "stacker_paragraph.h"
#include "stacker_layer.h"
#include "stacker_box.h"

namespace stkr {

const unsigned TAB_WIDTH = 4;

/* Returned by the text iterator when a non-text node is encountered. */
const uint32_t TI_INLINE_OBJECT = END_OF_STREAM - 1;

/* Iterates code points in an inline container. */
struct TextIterator {
	const Document *document;
	const Node *root;
	const Node *child;
	const char *text;
	const char *text_end;
	uint32_t ch;
	uint32_t highest;
};

/* Advances a text iterator to the next character or inline object. */
static uint32_t text_iterator_next(TextIterator *ti)
{
	do {
		while (ti->text == ti->text_end) {
			ti->child = inline_next(ti->root, ti->child);
			if (ti->child == NULL) {
				ti->ch = END_OF_STREAM;
				return ti->ch;
			}
			if (ti->child->layout == LAYOUT_INLINE) {
				ti->text = ti->child->text;
				ti->text_end = ti->text + ti->child->text_length;
			} else {
				ti->ch = TI_INLINE_OBJECT;
				return ti->ch;
			}
		}
		ti->text += utf8_decode(ti->text, ti->text_end, &ti->ch);
	} while (ti->ch == UNICODE_REPLACEMENT || ti->ch > ti->highest);
	return ti->ch;
}

/* Prepares a text iterator to visit the text content of an inline container. */
static void text_iterator_init(TextIterator *ti, const Document *document,
	const Node *root)
{
	ti->document = document;
	ti->root = root;
	ti->child = root;
	ti->text_end = root->text + root->text_length;
	ti->text = root->text + utf8_decode(root->text, ti->text_end, &ti->ch);
	ti->highest = highest_encodable_code_point(document->system->encoding);
}

/* Determines the number of paragraph elements required to represent the 
 * contents of an inline container. */
static unsigned determine_paragraph_buffer_size(const Document *document, 
	const Node *root, WhiteSpaceMode mode)
{
	TextIterator ti;
	text_iterator_init(&ti, document, root);
	unsigned num_elements = 0;
	unsigned num_spaces = 0;
	unsigned num_stripped_spaces = 0;
	for (;;) {
		uint32_t ch = text_iterator_next(&ti);
		if (ch == END_OF_STREAM)
			break;
		num_stripped_spaces += (ch == (unsigned char)'\r');
		num_spaces += unicode_isspace(ch);
		num_elements++;
	}

	/* In preserve white space mode, non-ignored white space characters
	 * generate paragraph elements. */
	if (mode != WSM_PRESERVE)
		num_elements -= num_spaces;
	else
		num_elements -= num_stripped_spaces;

	return num_elements;
}

/* Clears the bits that say whether child of an inline container generated one
 * or more paragraph elements. */
static void clear_empty_bits(Node *container)
{
	Node *node = container;
	do {
		node->t.flags &= ~NFLAG_HAS_PARAGRAPH_ELEMENTS;
		node = (Node *)inline_next_no_objects(container, node);
	} while (node != NULL);
}

/* Builds an array of paragraph elements from the text content of an inline 
 * container. */
static unsigned build_paragraph_elements(Document *document, 
	Node *root, WhiteSpaceMode mode, ParagraphElement *elements)
{
	clear_empty_bits(root);

	TextIterator ti;
	text_iterator_init(&ti, document, root);
	
	/* Skip leading spaces unless we're preserving white space. */
	uint32_t ch;
	do {
		ch = text_iterator_next(&ti);
	} while (ch != END_OF_STREAM && 
		(unicode_isspace(ch) || mode == WSM_PRESERVE));

	unsigned num_elements = 0;
	Node *child = NULL;
	while (ch != END_OF_STREAM) {
		ParagraphElement e;
		e.code_point = ch;
		e.penalty_type = unicode_is_multipart_delimiter(ch) ? 
			PENALTY_MULTIPART : PENALTY_INTERCHARACTER;
		e.is_inline_object = (ch == TI_INLINE_OBJECT);
		e.is_word_end = false;
		e.is_selected = false;

		/* Have we changed node? */
		e.is_node_first = (ti.child != child);
		child = (Node *)ti.child;
		child->t.flags |= NFLAG_HAS_PARAGRAPH_ELEMENTS;

		if (mode == WSM_NORMAL) {
			ch = text_iterator_next(&ti);
			if (unicode_isspace(ch) || ch == END_OF_STREAM) {
				e.is_word_end = true;
				e.penalty_type = PENALTY_NONE;
				while (unicode_isspace(ch))
					ch = text_iterator_next(&ti);
			}
		} else {
			e.penalty_type = (ch == '\n') ? PENALTY_FORCE_BREAK : PENALTY_NONE;
			ch = text_iterator_next(&ti);
			if (ch == '\r')
				continue; /* Normalize \r\n to \n. */
		}

		elements[num_elements++] = e;
	}

	assertb(num_elements == determine_paragraph_buffer_size(document, root, mode));
	return num_elements;
}

/* Metrics of the string encoding of a run of paragraph elements. */
struct EncodingSizes {
	unsigned num_code_units;
	unsigned num_characters;
	unsigned num_bytes;
};

/* Counts the characters in a run of paragraph elements and determines the
 * number of bytes required to encode those characters in the specified 
 * encoding. The code unit and byte counts include space for a terminator but
 * the character count does not. */
static EncodingSizes encoding_buffer_size(TextEncoding encoding, 
	const ParagraphElement *elements, unsigned num_elements, 
	bool synthetic_spaces)
{
	EncodingSizes sizes = { 0, 0, 0 };
	unsigned length_mask = ENCODING_LENGTH_MASKS[encoding];
	unsigned num_words = 0;
	for (unsigned i = 0; i < num_elements; ++i) {
		const ParagraphElement *e = elements + i;
		if (e->is_inline_object)
			continue;
		num_words += e->is_word_end;
		sizes.num_code_units += encoded_length(e->code_point, length_mask);
		sizes.num_characters += 1;
	}
	if (synthetic_spaces && num_words != 0) {
		unsigned num_spaces = num_words;
		if (elements[num_elements - 1].is_word_end)
			num_spaces--; /* Word end in the last element generates no space. */
		sizes.num_characters += num_spaces;
		sizes.num_code_units += num_spaces * encoded_length(' ', length_mask);
	}
	sizes.num_code_units += 1; /* Null terminator. */
	sizes.num_bytes = sizes.num_code_units * BYTES_PER_CODE_UNIT[encoding];
	return sizes;
}

/* Produces a single byte encoding of the code points in a run of paragraph
 * elements. The code points are simply truncated, on the basis that any 
 * characters not representable in the encoding have already been filtered
 * out. */
static unsigned encode_paragraph_elements_as_bytes(
	const ParagraphElement *elements, unsigned count, char *out_text,
	bool synthetic_spaces)
{
	unsigned j = 0;
	for (unsigned i = 0; i < count; ++i) {
		const ParagraphElement *e = elements + i;
		if (e->is_inline_object)
			continue;
		out_text[j++] = (char)e->code_point;
		if (e->is_word_end && synthetic_spaces && i + 1 != count)
			out_text[j++] = ' ';
	}
	out_text[j++] = '\0';
	return j;
}

/* Produces a UTF-8 encoding of the code points in a run of paragraph 
 * elements. */
static unsigned encode_paragraph_elements_as_utf8(
	const ParagraphElement *elements, unsigned count, char *out_text,
	bool synthetic_spaces)
{
	unsigned j = 0;
	for (unsigned i = 0; i < count; ++i) {
		const ParagraphElement *e = elements + i;
		if (e->is_inline_object)
			continue;
		j += utf8_encode(out_text + j, e->code_point);
		if (e->is_word_end && synthetic_spaces && i + 1 != count)
			j += utf8_encode(out_text + j, ' ');
	}
	out_text[j++] = '\0';
	return j;
}

/* Produces a UTF-16 encoding of the code points in a run of paragraph 
 * elements. */
static unsigned encode_paragraph_elements_as_utf16(
	const ParagraphElement *elements, unsigned count, uint16_t *out_text,
	bool synthetic_spaces)
{
	unsigned j = 0;
	for (unsigned i = 0; i < count; ++i) {
		const ParagraphElement *e = elements + i;
		if (e->is_inline_object)
			continue;
		j += utf16_encode(out_text + j, elements[i].code_point);
		if (elements[i].is_word_end && synthetic_spaces && i + 1 != count)
			j += utf16_encode(out_text + j, ' ');
	}
	out_text[j++] = 0;
	return j;
}

/* Produces a UTF-32 encoding of the code points in a run of paragraph 
 * elements. */
static unsigned encode_paragraph_elements_as_utf32(
	const ParagraphElement *elements, unsigned count, uint32_t *out_text,
	bool synthetic_spaces)
{
	unsigned j = 0;
	for (unsigned i = 0; i < count; ++i) {
		const ParagraphElement *e = elements + i;
		if (e->is_inline_object)
			continue;
		out_text[j++] = e->code_point;
		if (e->is_word_end && synthetic_spaces && i + 1 != count)
			out_text[j++] = (unsigned char)' ';
	}
	out_text[j++] = 0;
	return j;
}

/* Converts the code points in a run of paragraph elements to text. */
static unsigned encode_paragraph_elements(const ParagraphElement *elements,
	unsigned count, void *out_text, TextEncoding encoding, 
	bool synthetic_spaces)
{
	switch (encoding) {
		case ENCODING_ASCII:
		case ENCODING_LATIN1:
			return encode_paragraph_elements_as_bytes(elements, count,
				(char *)out_text, synthetic_spaces);
		case ENCODING_UTF8:
			return encode_paragraph_elements_as_utf8(elements, count,
				(char *)out_text, synthetic_spaces);
		case ENCODING_UTF16:
			return encode_paragraph_elements_as_utf16(elements, count,
				(uint16_t *)out_text, synthetic_spaces);
		case ENCODING_UTF32:
			return encode_paragraph_elements_as_utf32(elements, count,
				(uint32_t *)out_text, synthetic_spaces);
	}
	assertb(false);
	return 0;
}

/* Reallocates the temporary text-and-advances buffer to accommodate the current
 * run. */
static void grow_measurement_buffer(TextMeasurementState *ms, 
	const EncodingSizes *sizes)
{
	unsigned bytes_required = sizes->num_bytes + 
		sizes->num_characters * sizeof(unsigned);
	if (bytes_required > (unsigned)abs(ms->capacity)) {
		if (ms->capacity > 0)
			delete [] ms->buffer;
		unsigned new_capacity = (bytes_required + 4095) & -4096;
		ms->buffer = new uint8_t[new_capacity];
		ms->capacity = (int)new_capacity;
	}
	ms->advances = (unsigned *)(ms->buffer + sizes->num_bytes);
}

/* Obtains advances from the back end for a text run and copies the advances
 * into the corresponding paragraph elements. */
static void measure_element_group(TextMeasurementState *ms, unsigned text_length)
{
	unsigned num_characters = measure_text(ms->iterator.document->system, 
		ms->iterator.style->font_id, ms->buffer, text_length, ms->advances);
	for (unsigned i = 0, j = 0; i < ms->iterator.count; ++i) {
		ParagraphElement *e = ms->iterator.elements + ms->iterator.offset + i;
		if (e->is_inline_object)
			continue;
		e->advance = ms->advances[j++];
		j += unsigned(e->is_word_end && j != num_characters);
	}
}

/* Expands the iterator to enclose the next measurement group, stopping along
 * the way to update the advances of any inline objects it contains. */
static void measurement_advance(TextMeasurementState *ms, ParagraphElement *e)
{
	while (e != NULL) {
		const Box *box = ms->iterator.next_child->t.counterpart.box;
		float dim = get_size(box, SSLOT_INTRINSIC, AXIS_H);
		e->advance = (unsigned)round_float_to_fixed(dim, TEXT_METRIC_PRECISION);
		e = expand_measurement_group(&ms->iterator);
	}
}

/* Initializes text measurement. */
void measurement_init(TextMeasurementState *ms, Document *document, 
	Node *container, uint8_t *buffer, unsigned buffer_size)
{
	ms->buffer = buffer;
	ms->capacity = -(int)buffer_size;
	ms->advances = NULL;
	ParagraphElement *e = iterate_measurement_groups(&ms->iterator, document, 
		container);
	measurement_advance(ms, e);
}

/* Deinitializes text measurement. */
void measurement_deinit(TextMeasurementState *ms)
{
	if (ms->capacity > 0)
		delete [] ms->buffer;
}

/* Incrementally updates the advance widths of all text paragraph elements. 
 * Returns true when the process is complete. */
bool measurement_continue(TextMeasurementState *ms, Document *document, Node *)
{
	TextEncoding encoding = document->system->encoding;
	while (ms->iterator.count != 0) {
		if (check_interrupt(document))
			return false;
		const ParagraphElement *elements = ms->iterator.elements + ms->iterator.offset;
		EncodingSizes sizes = encoding_buffer_size(encoding, elements, ms->iterator.count, true);
		grow_measurement_buffer(ms, &sizes);
		encode_paragraph_elements(elements, ms->iterator.count, ms->buffer, encoding, true);
		measure_element_group(ms, sizes.num_code_units - 1);
		measurement_advance(ms, next_measurement_group(&ms->iterator));
	}
	return true;
}

/* Calculates the width of spaces to use when positioning characters in a text
 * box. */
static int calculate_box_glue_width(System *system, const ParagraphLine *line, 
	const Box *box)
{
	const Node *child = box->t.counterpart.node;
	int16_t font_id = child->style.text.font_id;
	const FontMetrics *metrics = get_font_metrics(system, font_id);
	return adjust_glue(line->adjustment_ratio, metrics->space_width, 
		metrics->space_stretch, metrics->space_shrink);
}

/* Returns the total width of a group of paragraph elements placed with the
 * specified glue width. The group must not contain inline objects. */
static int compute_placement_group_width(const ParagraphElement *elements, 
	unsigned num_elements, int glue_width)
{
	int width = 0;
	for (unsigned i = 0; i < num_elements; ++i) {
		const ParagraphElement e = elements[i];
		assertb(!e.is_inline_object);
		width += e.advance;
		if (e.is_word_end)
			width += glue_width;
	}
	if (num_elements != 0 && elements[num_elements - 1].is_word_end)
		width -= glue_width;
	return width;
}

/* Builds the array of horizontal character offsets for a text layer. Note that 
 * the range of elements positioned by a text box never includes inline objects, 
 * so we can assume a 1:1 relationship between elements and characters.  */
static void position_characters(VisualLayer *layer, 
	const ParagraphElement *elements, unsigned num_elements, int glue_width)
{
	int *positions = (int *)get_text_layer_positions(layer);
	int char_x0 = 0;
	for (unsigned i = 0; i < num_elements; ++i) {
		const ParagraphElement *e = elements + i;
		assertb(!e->is_inline_object);
		positions[i] = round_fixed_to_int(char_x0, TEXT_METRIC_PRECISION);
		char_x0 += e->advance;
		if (e->is_word_end)
			char_x0 += glue_width;
	}
}

/* Returns the first child of the first non-empty line box in a sibling chain
 * of line boxes. */
static Box *first_line_child(Box *line_box)
{
	while (line_box != NULL) {
		if (line_box->t.first.box != NULL)
			return line_box->t.first.box;
		line_box = line_box->t.next.box;
	}
	return NULL;
}

/* Sets fixed sizes on a line box using information in the line structures. */
static bool set_line_box_sizes(Box *line_box, const ParagraphLine *line, 
	unsigned line_number, int leading)
{
	bool changed = false;
	if (set_size(line_box, SSLOT_EXTRINSIC, AXIS_H, (float)line->width))
		changed = true;
	if (set_size(line_box, SSLOT_EXTRINSIC, AXIS_V, (float)line->height))
		changed = true;
	if (line_number != 0 && leading > 0) {
		BoxAxis *axis = &line_box->axes[AXIS_V];
		axis->mode_margin_lower = DMODE_ABSOLUTE;
		float margin = (float)leading;
		if (axis->margin_lower != margin) {
			axis->margin_lower = margin;
			changed = true;
		}
	}
	if (changed) {
		line_box->layout_flags &= ~(BLFLAG_TREE_BOUNDS_VALID | 
			BLFLAG_CHILD_BOUNDS_VALID | BLFLAG_TREE_CLIP_VALID);
	}
	return changed;
}

/* Destroys a line box's text layer if it is no longer required. */
static void finish_line(InlineBoxUpdateState *s, Box *line_box)
{
	/* If we rebuilt the line using boxes from the queue (i.e. the line's boxes 
	 * didn't change), those boxes remained chained into the queue. Detach them 
	 * now. */
	Box *first = line_box->t.first.box;
	Box *last = line_box->t.last.box;
	if (first != NULL) {
		assertb(first->t.prev.box == NULL);
		assertb(last->t.next.box == NULL);
		first->t.prev.box = NULL;
		last->t.next.box = NULL;
	}

	/* If the line has a text layer but we're not using it as a text box any 
	 * more, destroy the layer. */
	if (line_box != NULL && (line_box->t.flags & BOXFLAG_IS_TEXT_BOX) == 0)
		destroy_box_text_layer((Document *)s->ei.document, line_box);

	s->eol = true;
}

static void compute_group_box_size(InlineBoxUpdateState *s, 
	const ParagraphLine *pl, int *out_width, int *out_height)
{
	Document *document = (Document *)s->ei.document;
	const FontMetrics *m = get_font_metrics(document->system, 
		s->ei.style->font_id);
	int glue_width = adjust_glue(pl->adjustment_ratio, 
		m->space_width, m->space_stretch, m->space_shrink);
	*out_width = compute_placement_group_width(s->ei.elements + 
		s->ei.offset, s->ei.count, glue_width);
	*out_height = m->height;
}

/* Sets the absolute size of a box representing a placement group based on the
 * measured sizes of the elements it contains. */
static void set_group_box_size(Box *group_box, int width, int height)
{
	/* This code is run in the middle of the sizing pass. To avoid the need to 
	 * run the main sizing algorithm over the boxes in an inline container after 
	 * they have been generated, box generation takes responsibility for all 
	 * sizing of text and line boxes. Accordingly, we set all the box's size
	 * slots, and do so using a low level function that does not alter flags. */
	 float w = (float)width;
	 float h = (float)height;
	 for (SizeSlot slot = SSLOT_PREFERRED; slot <= SSLOT_EXTRINSIC; 
		slot = SizeSlot(slot + 1)) {
		set_slot(group_box, slot, AXIS_H, w);
		set_slot(group_box, slot, AXIS_V, h);
	}
}

static void do_group_box_sizing(InlineBoxUpdateState *s, 
	const ParagraphLine *pl, Box *group_box)
{
	/* Calculate high precision sizes for the box and update the high precision
	 * accumulator. */
	int width_ideal, height_ideal;
	compute_group_box_size(s, pl, &width_ideal, &height_ideal);
	s->xpos += width_ideal;

	/* Round the sizes up to the nearest pixel and update the low precision 
	 * accumulator. */
	int pixel_width = fixed_ceil_as_int(width_ideal, TEXT_METRIC_PRECISION);
	int pixel_height = fixed_ceil_as_int(height_ideal, TEXT_METRIC_PRECISION);
	set_group_box_size(group_box, pixel_width, pixel_height);
	s->xpos_rounded += pixel_width;
}

static void set_group_box_debug_string(InlineBoxUpdateState *s, Box *group_box)
{
	const ParagraphElement *elements = s->ei.elements + group_box->first_element;
	unsigned count = group_box->last_element - group_box->first_element;
	EncodingSizes sizes = encoding_buffer_size(ENCODING_LATIN1, elements, count, true);
	char *buf = new char[sizes.num_bytes];
	encode_paragraph_elements_as_bytes(elements, count, buf, true);
	const char *prefix = (group_box->t.flags & BOXFLAG_IS_LINE_BOX) != 0 ? "whole line group" : "text group";
	set_box_debug_string(group_box, "%s: \"%s\"", prefix, buf);
	delete [] buf;
}

static int compute_intergroup_space(InlineBoxUpdateState *s, 
	const ParagraphLine *pl, unsigned position)
{
	/* No space at EOL. */
	if (position == pl->b)
		return 0;
	
	/* Use the larger of the space widths from the font metric objects of the
	 * adjacent groups. */
	System *system = (System *)s->ei.document->system;
	const FontMetrics *m1 = get_font_metrics(system, s->ei.style->font_id);
	const FontMetrics *m2 = get_font_metrics(system, s->ei.next_style->font_id);
	const FontMetrics *m = m1->space_width > m2->space_width ? m1 : m2;
	return adjust_glue(
		pl->adjustment_ratio, 
		m->space_width, 
		m->space_stretch, 
		m->space_shrink);
}

static void set_intergroup_space(InlineBoxUpdateState *s, 
	const ParagraphLine *pl, Box *b)
{
	s->xpos += compute_intergroup_space(s, pl, b->last_element);
	int nearest_pixel = round_fixed_to_int(s->xpos, TEXT_METRIC_PRECISION);
	b->axes[AXIS_H].margin_upper = (float)(nearest_pixel - s->xpos_rounded);
	b->axes[AXIS_H].mode_margin_upper = DMODE_ABSOLUTE;
	s->xpos_rounded = nearest_pixel;
}

/* Marks a text box's text layer for check or rebuild. */
static void maybe_invalidate_text_layer(InlineBoxUpdateState *s, Box *b)
{
	/* Any existing text layer must be rechecked before use to make sure the
	 * element range, font and spacing are the same. */
	b->t.flags &= ~BOXFLAG_TEXT_LAYER_KNOWN_VALID;
	/* The text layer is invalid if elements have changed since the last box
	 * update. */
	if ((s->ei.container->t.flags & BOXFLAG_SAME_PARAGRAPH) == 0)
		b->t.flags &= ~BOXFLAG_TEXT_LAYER_MAY_BE_VALID;
}

/* Dequeues a text box for reuse. */
static Box *dequeue_text_box(InlineBoxUpdateState *s)
{
	Box *box = s->free_list_head;
	while (box != NULL) {
		Box *next = box->t.next.box;
		s->free_list_head = next;
		if (next == NULL)
			s->free_list_tail = NULL;
		box->t.parent.box = NULL;
		box->t.next.box = NULL;
		box->t.prev.box = NULL;
		if ((box->t.flags & BOXFLAG_IS_TEXT_BOX) != 0)
			break;
		grid_remove((Document *)s->ei.document, box);
		box = next;
	}
	return box;
}

/* Destroys any text boxes remaining in the free list. */
static void destroy_free_list(InlineBoxUpdateState *s)
{
	for (Box *b; (b = dequeue_text_box(s)) != NULL; )
		destroy_box_internal((Document *)s->ei.document, b);
}

/* Returns true if a box was part of a line that has been reclaimed. */
static bool box_in_free_list(const InlineBoxUpdateState *s, const Box *b)
{
	const Box *p = b->t.parent.box;
	const Box *c = s->ei.container->t.counterpart.box;
	return (p != NULL && p->t.parent.box == c && 
		p->line_number <= s->line_number);
}

static void remove_from_free_list(InlineBoxUpdateState *s, Box *b)
{
	/* The free list is not always well formed because we don't bother to break 
	 * sibling links when dequeing boxes, resulting a bogus 'prev' pointer for 
	 * the head. Correct that before calling the standard list remove helper. */
	assertb(s->free_list_head != NULL);
	assertb(s->free_list_tail != NULL);
	assertb(s->free_list_tail->t.next.box == NULL);
	s->free_list_head->t.prev.box = NULL;
	list_remove(
		(void **)&s->free_list_head,
		(void **)&s->free_list_tail,
		b, offsetof(Tree, prev));
	b->t.parent.box = NULL;
}

/* Chooses or creates a box to represent the current placement group. */
static Box *get_or_create_group_box(InlineBoxUpdateState *s, 
	const ParagraphLine *pl, Box *lb)
{
	Node *node = (Node *)s->ei.child;
	ParagraphElement e = s->ei.elements[s->ei.offset];
	Box *b = NULL;

	if (e.is_inline_object) {
		/* Use the inline object's box, which must be removed from either the
		 * free list or its current parent. */
		b = node->t.counterpart.box;
		if (box_in_free_list(s, b)) {
			remove_from_free_list(s, b);
		} else {
			tree_remove(&b->t);	
		}
	} else {
		if (pl->a == s->ei.offset && iterator_at_eol(&s->ei)) {
			/* The group comprises the full text of the line. Use the line box 
			 * to display the text. */
			b = lb;
		} else {
			b = dequeue_text_box(s);
			if (b == NULL)
				b = create_box((Document *)s->ei.document, NULL);
		}
		b->t.counterpart.node = node;
		b->t.flags |= BOXFLAG_IS_TEXT_BOX;
		do_group_box_sizing(s, pl, b);
		maybe_invalidate_text_layer(s, b);
	}
	b->first_element = s->ei.offset;
	b->last_element = s->ei.offset + s->ei.count;
	if (!e.is_inline_object)
		set_group_box_debug_string(s, b);
	return b;
}

/* Inserts a group box into the current line, either after the tail or, if we've 
 * moved to a new line, at the start. */
static void insert_box_into_line(InlineBoxUpdateState *s, Box *b, Box *lb)
{
	if (b == lb)
		return;
	tree_insert_child_before(&lb->t, &b->t, NULL);
	lb->layout_flags &= ~BLFLAG_BOUNDS_VALID_MASK;
	s->must_update_bounds = true;
	b->line_number = lb->line_number;
	/* Layout relies on an inline container to manage the dependency flags
	 * of its children. We assume that word and inline object boxes don't
	 * depend on their containing line. */
	b->layout_flags |= BLFLAG_LAYOUT_INFO_VALID;
}

static bool build_queue_empty(const InlineBoxUpdateState *s)
{
	return s->bq_head == s->bq_tail;
}

static bool build_queue_full(const InlineBoxUpdateState *s)
{
	return (s->bq_tail + 1) % BQ_CAPACITY == s->bq_head;
}

static void build_queue_push(InlineBoxUpdateState *s, const ParagraphLine *pl, Box *box)
{
	assertb(!build_queue_full(s));
	s->bq[s->bq_tail].box = box;
	s->bq[s->bq_tail].line = pl;
	s->bq_tail = (s->bq_tail + 1) % BQ_CAPACITY;
}

static void build_queue_pop(InlineBoxUpdateState *s)
{
	assertb(!build_queue_empty(s));
	s->bq_head = (s->bq_head + 1) % BQ_CAPACITY;
}

/* Creates a new line box and adds it to the container. */
static Box *add_line_box(InlineBoxUpdateState *s, Document *document, 
	Node *container, Box *container_box)
{
	Box *lb = build_line_box(document, container, (Justification)
		container->style.justification, s->line_number);
	tree_insert_child_before(&container_box->t, &lb->t, NULL);
	/* Layout assumes that an inline container manages the dependency
	 * flags of its children, so set them here. */
	lb->layout_flags |= BLFLAG_LAYOUT_INFO_VALID;
	return lb;
}

/* True if the boxes inside a line must be reconstructed from paragraph 
 * elements. */
static bool must_rebuild_line(const Box *container, const ParagraphLine *line, 
	const Box *lb)
{
	/* If paragraph elements have changed since the last box rebuild, all lines
	 * must be rebuilt. */
	if ((container->t.flags & BOXFLAG_SAME_PARAGRAPH) == 0)
		return true;

	/* Otherwise, the line can be skipped if it has the same element range it
	 * did last time. */
	return lb->first_element != line->a || lb->last_element != line->b;
}

/* Updates a line box's element range and dimensions. */
static void update_line_box(InlineBoxUpdateState *s, const Node *container, 
	const ParagraphLine *line, unsigned line_number, Box *lb)
{
	lb->first_element = line->a;
	lb->last_element = line->b;
	lb->t.flags &= ~BOXFLAG_IS_TEXT_BOX;
	if (set_line_box_sizes(lb, line, line_number, container->style.leading))
		s->must_update_bounds = true;
	set_box_debug_string(lb, "line box %u", line_number);
}

/* Clears a line and adds its boxes to the free list. */
static void bulldoze_line(InlineBoxUpdateState *s, Box *b)
{
	/* Clear the line. */
	Box *first = b->t.first.box;
	Box *last = b->t.last.box;
	b->t.first.box = NULL;
	b->t.last.box = NULL;
	if (first == NULL)
		return;

	/* Append the line's boxes to the free list. */
	first->t.prev.box = s->free_list_tail;
	if (s->free_list_head == NULL) {
		s->free_list_head = first;
	} else {
		s->free_list_tail->t.next.box = first;
	}
	s->free_list_tail = last;
}

/* Moves to the next line, reclaiming the lines' boxes and adding it to the 
 * build queue if necessary. */
static bool next_line(InlineBoxUpdateState *s)
{
	/* Create or reuse line boxes until we reach the requested line number. */
	Document *document = (Document *)s->ei.document;
	Node *container = (Node *)s->ei.container;
	Box *cb = container->t.counterpart.box;

	/* Are there any more lines to visit? */
	if (s->line_number + 1 == s->lines->num_lines)
		return false;

	/* Move to the next line, creating a line box if required. */
	s->line_number++;
	Box *lb = (s->line_number != 0) ? s->line_box->t.next.box : cb->t.first.box;
	const ParagraphLine *pl = s->lines->lines + s->line_number;
	bool rebuild = true;
	if (lb == NULL) {
		lb = add_line_box(s, document, container, cb);
		s->must_update_bounds = true;
	} else {
		rebuild = must_rebuild_line(cb, pl, lb);
		if (rebuild)
			bulldoze_line(s, lb);
	}
	s->line_box = lb;

	/* If the line must be visited for rebuild, add it at the tail of the build 
	 * queue. */
	if (rebuild) {
		update_line_box(s, container, pl, s->line_number, lb);
		build_queue_push(s, pl, lb);
	}

	return true;
}

static void move_iterator_to_line_start(InlineBoxUpdateState *s, 
	const ParagraphLine *pl, const Box *lb)
{
	/* Use the start position of the previous line, for which we know the
	 * corresponding child node, as a synchronization point. */
	if (s->ei.offset + s->ei.count != pl->a && s->line_number != 0) {
		const Node *child = lb->t.prev.box->t.counterpart.node;
		placement_iterator_jump(&s->ei, pl[-1].a, child);
	}

	/* Move move the iterator forwards to the start of the current line. */
	iterate_placement_groups(&s->ei, pl);
}

/* Processes the current placement group, adding one box to the line at the
 * head of the build queue. */
static void build_step(InlineBoxUpdateState *s)
{
	if (build_queue_empty(s))
		return;

	Box *lb = s->bq[s->bq_head].box;
	const ParagraphLine *pl = s->bq[s->bq_head].line;

	/* Initialize the build state for this line if required. */
	if (s->eol) {
		move_iterator_to_line_start(s, pl, lb);
		s->xpos = 0;
		s->xpos_rounded = 0;
		s->eol = false;
	}

	/* Make a box for the placement group and append it to the line. */
	Box *b = get_or_create_group_box(s, pl, lb);
	set_intergroup_space(s, pl, b);
	insert_box_into_line(s, b, lb);

	/* If this is the last placement group on the line, finish the line. */
	if (iterator_at_eol(&s->ei)) {
		finish_line(s, lb);
		build_queue_pop(s);
	} else {
		next_placement_group(&s->ei);
	}
}

/* True if the box update should step to the next line, false if it should
 * process a placement group for the line at the head of the build queue. */
static bool should_advance(InlineBoxUpdateState *s)
{
	/* If we have free boxes, prefer to consume them before advancing. */
	if (s->free_list_head != NULL && !build_queue_empty(s))
		return false;
	
	/* The next line might need to be rebuilt, so we can't advance if the build
	 * queue is full. */
	if (build_queue_full(s))
		return false;

	/* We shouldn't advance if we already have the required number of lines. */
	return s->line_number + 1 != s->lines->num_lines;
}

/* True if all lines have been processed. */
static bool is_complete(const InlineBoxUpdateState *s)
{
	return s->eol && build_queue_empty(s) && 
		s->line_number + 1 == s->lines->num_lines;
}

/* Executes one step in an incremental box update. Returns false if the update 
 * is complete. */
static bool box_update_step(InlineBoxUpdateState *s)
{
	if (should_advance(s)) {
		next_line(s);
		return true;
	}
	build_step(s);
	return !is_complete(s);
}

/* Initializes an incremental box update. */
void box_update_init(InlineBoxUpdateState *s, Document *document, Node *container)
{
	init_placement_group_iterator(&s->ei, document, container);
	
	s->bq_head = 0;
	s->bq_tail = 0;
	
	s->free_list_head = NULL;
	s->free_list_tail = NULL;

	s->lines = container->icb->lines;
	s->line_number = unsigned(-1);
	s->line_box = NULL;
	s->must_update_bounds = false;
	s->eol = true;

	s->debug_stamp = rand() * rand() * rand();
}

/* Adds or removes line boxes as required so that the number of line boxes
 * matches the number of paragraph lines. */
static void finalize_lines(InlineBoxUpdateState *s)
{
	unsigned required = s->lines->num_lines;
	assertb(s->line_number + 1 == required);
	if (s->line_box != NULL) {
		finish_line(s, s->line_box);
		Box *surplus = s->line_box->t.next.box;
		if (surplus != NULL)
			remove_and_destroy_siblings((Document *)s->ei.document, surplus);
	}
	tree_check(s->ei.document->root->t.counterpart.tree);
}

static void box_update_finish(InlineBoxUpdateState *s)
{
	destroy_free_list(s);
	finalize_lines(s);
	if (s->must_update_bounds) {
		clear_flags((Document *)s->ei.document, 
			s->ei.container->t.counterpart.box, 
			BLFLAG_BOUNDS_VALID_MASK);
	}

	/* This flag is used to detect whether paragraph elements have changed
	 * in between box updates. */
	Node *container = (Node *)s->ei.container;
	container->t.flags |= BOXFLAG_SAME_PARAGRAPH;
}

/* Does work towards an inline box update until interrupted. Returns true if
 * the update completes. */
bool box_update_continue(InlineBoxUpdateState *s, Document *document)
{
	for (;;) {
		if (check_interrupt(document))
			return false;
		if (!box_update_step(s))
			break;
	}
	box_update_finish(s);
	return true;
}

const Node *find_box_inline_container(const Document *document, const Box *box)
{
	return find_inline_container_not_self(document, box->t.counterpart.node);
}

/* Destroys a box's text layer if it has one. */
void destroy_box_text_layer(Document *d, Box *b)
{
	VisualLayer *layer = layer_chain_replace(VLCHAIN_BOX, &b->layers, LKEY_TEXT, NULL);
	if (layer != NULL)
		destroy_layer(d, layer);
}

/* Rebuilds the text layer representing the paragraph elements positioned by
 * a box. */
VisualLayer *update_box_text_layer(Document *document, Box *box)
{
	assertb((box->t.flags & BOXFLAG_IS_TEXT_BOX) != 0);

	/* If the existing layer is known to be valid, return it immediately. */
	VisualLayer *old = layer_chain_find(VLCHAIN_BOX, box->layers, LKEY_TEXT);
	if ((box->t.flags & BOXFLAG_TEXT_LAYER_VALID_MASK) == BOXFLAG_TEXT_LAYER_VALID_MASK)
		return old;

	/* Get the range of paragraph elements positioned by the box. */
	System *system = document->system;
	Node *container = (Node *)find_box_inline_container(document, box);
	const Node *node = box->t.counterpart.node;
	InlineContext *icb = container->icb;
	unsigned start = box->first_element;
	unsigned end = box->last_element;
	unsigned num_elements = box->last_element - box->first_element;
	const ParagraphLine *line = &icb->lines->lines[box->line_number];

	/* If the existing layer may still be valid, determine whether it needs to
	 * be regenerated. */
	if ((box->t.flags & BOXFLAG_TEXT_LAYER_MAY_BE_VALID) != 0 && 
		old != NULL &&
		old->text.start == start && 
		old->text.end == end &&
		old->text.font_id == node->style.text.font_id &&
		old->text.adjustment_ratio == line->adjustment_ratio) 
	{
		box->t.flags |= BOXFLAG_TEXT_LAYER_KNOWN_VALID;
		return old;
	}
	
	/* Allocate a new text layer. */
	const ParagraphElement *elements = icb->elements + start;
	EncodingSizes sizes = encoding_buffer_size(system->encoding, elements, num_elements, false);
	unsigned bytes_required = sizes.num_bytes + sizes.num_characters * sizeof(int);
	VisualLayer *layer = create_layer(document, container, VLT_TEXT, bytes_required);
	layer->text.container = container;
	layer->text.start = start;
	layer->text.end = end;
	layer->text.num_characters = sizes.num_characters;
	layer->text.num_code_units = sizes.num_code_units;
	layer->text.adjustment_ratio = line->adjustment_ratio;
	layer->text.font_id = node->style.text.font_id;

	/* Encode the text into the format used by the back end. */
	void *text = (void *)get_text_layer_text(layer);
	encode_paragraph_elements(elements, num_elements, text, system->encoding, false);

	/* Calculate the adjusted glue width for the line. */
	int glue_width = calculate_box_glue_width(system, line, box);
	position_characters(layer, elements, num_elements, glue_width);

	/* Add the new layer to the box's chain. */
	layer_chain_replace(VLCHAIN_BOX, &box->layers, LKEY_TEXT, layer);
	if (old != NULL)
		destroy_layer(document, old);
	box->t.flags |= BOXFLAG_TEXT_LAYER_VALID_MASK;
	return layer;
}

/* Creates the selection layer for a line box if necessary. */
VisualLayer *require_selection_layer(Document *, Box *)
{
	/* FIXME(TJM): NYI. */
	// assertb(false);
	return NULL;
}

/* Destroys all text boxes in a line, taking care not to destroy inline object
 * boxes, which are owned by their respective nodes, not the inline 
 * container. */
static void destroy_text_boxes(Document *document, Box *line_box)
{
	for (Box *child = line_box->t.first.box; child != NULL; ) {
		Box *next = child->t.next.box;
		if ((child->t.flags & BOXFLAG_IS_TEXT_BOX) != 0)
			remove_and_destroy_box(document, child);
		child = next;
	}
}

/* Destroys all line and text boxes in an inline container. */
static void destroy_inline_boxes(Document *document, Node *container)
{
	Box *root = container->t.counterpart.box;
	for (Box *line_box = root->t.first.box; line_box != NULL; ) {
		Box *next = line_box->t.next.box;
		destroy_text_boxes(document, line_box);
		tree_remove_children(&line_box->t); // Orphan non-text boxes.
		destroy_box_internal(document, line_box);
		line_box = next;
	}
	root->t.first.box = NULL;
	root->t.last.box = NULL;
}

/* Destroys a node's inline context and all inline boxes. */
void destroy_inline_context(Document *document, Node *node)
{
	InlineContext *context = node->icb;
	if (context == NULL)
		return;
	destroy_line_list(context->lines);
	destroy_inline_boxes(document, node);
	delete [] (char *)context;
	node->icb = NULL;
}

/* Rebuilds the inline context of a text container node. */
void rebuild_inline_context(Document *document, Node *node)
{
	assertb(_heapchk() == _HEAPOK); /* FIXME: DEBUG. */
	if (node->icb != NULL) {
		destroy_line_list(node->icb->lines);
		assertb(_heapchk() == _HEAPOK); /* FIXME: DEBUG. */
		delete [] (char *)node->icb;
		assertb(_heapchk() == _HEAPOK); /* FIXME: DEBUG. */
		node->icb = NULL;
	}

	/* Read paragraph styles. */
	WhiteSpaceMode space_mode = (WhiteSpaceMode)node->style.white_space_mode;
	WrapMode wrap_mode = (WrapMode)node->style.wrap_mode;
	assertb((int)space_mode != ADEF_UNDEFINED);
	assertb((int)wrap_mode != ADEF_UNDEFINED);
		 
	unsigned num_elements = determine_paragraph_buffer_size(document, 
		node, space_mode);
	assertb(_heapchk() == _HEAPOK); /* FIXME: DEBUG. */

	unsigned bytes_required = sizeof(InlineContext);
	bytes_required += num_elements * sizeof(ParagraphElement);
	char *block = new char[bytes_required];
	InlineContext *icb = (InlineContext *)block;
	block += sizeof(InlineContext);
	icb->elements = (ParagraphElement *)block;
	icb->num_elements = num_elements;
	block += num_elements * sizeof(ParagraphElement);
	icb->lines = NULL;

	assertb(_heapchk() == _HEAPOK); /* FIXME: DEBUG. */
	build_paragraph_elements(document, node, space_mode, icb->elements);
	assertb(_heapchk() == _HEAPOK); /* FIXME: DEBUG. */

	node->icb = icb;
	node->t.flags &= ~NFLAG_RECONSTRUCT_PARAGRAPH;
	node->t.flags |= NFLAG_REMEASURE_PARAGRAPH_ELEMENTS;

	Box *box = node->t.counterpart.box;
	if (box != NULL) {
		box->layout_flags &= ~(BLFLAG_TEXT_VALID | BLFLAG_INLINE_BOXES_VALID);
		box->t.flags &= ~BOXFLAG_SAME_PARAGRAPH;
	}
	assertb(_heapchk() == _HEAPOK); /* FIXME: DEBUG. */
}

/* Resolves a document space horizontal position into a caret position within
 * the range of caret positions spanned by box. */
CaretAddress caret_position(Document *document, const Box *box, float x)
{
	CaretAddress address = { NULL, 0 };
	address.node = find_layout_node(document, box->t.counterpart.node);
	if (address.node == NULL)
		return address;
	float dx = x - box->axes[AXIS_H].pos;
	if ((box->t.flags & BOXFLAG_IS_TEXT_BOX) != 0) {
		VisualLayer *text_layer = update_box_text_layer(document, (Box *)box);
		address.offset = intercharacter_position(text_layer, dx);
	} else {
		float mid = 0.5f * outer_dim(box, AXIS_H);
		address.offset = dx < mid ? 0 : IA_END;
	}
	return address;
}

/* True if position A is before position B. */
bool caret_before(CaretAddress a, CaretAddress b)
{
	return a.node != b.node ? tree_before(&a.node->t, &b.node->t) : 
		a.offset < b.offset;
}

/* True if position A is equal to position B. */
bool caret_equal(CaretAddress a, CaretAddress b)
{
	return a.node == b.node && a.offset == b.offset;
}

/* Converts IA_END to a real paragraph element index. */
unsigned expand_internal_address(const Node *node, unsigned ia)
{
	if (ia == IA_END && node->layout == LAYOUT_INLINE_CONTAINER)
		ia = node->icb->num_elements;
	return ia;
}

/* True if two internal addreses currently refer to the same position. */
bool same_internal_address(const Node *node, unsigned a, unsigned b)
{
	unsigned ea = expand_internal_address(node, a);
	unsigned eb = expand_internal_address(node, b);
	return ea == eb;
}

/* Finds the node that generated a paragraph element. */
const Node *inline_node_at(const Node *container, unsigned ia)
{
	const InlineContext *icb = container->icb;
	ia = expand_internal_address(container, ia);
	assertb(ia <= icb->num_elements);
	const Node *child = container;
	for (unsigned i = 1; i < ia; ++i)
		if (icb->elements[i].is_node_first)
			child = inline_next_nonempty(container, child);
	return child;
}

/* Returns the offset of the first paragraph element generated by a child of
 * an inline container. */
static unsigned internal_address_of(const Node *container, const Node *child)
{
	const InlineContext *icb = container->icb;
	const Node *node = inline_first_nonempty(container);
	unsigned ia = 0;
	while (node != child) {
		if (++ia >= icb->num_elements)
			return IA_END;
		if (icb->elements[ia].is_node_first)
			node = inline_next_nonempty(container, node);
	}
	return ia;
}

/* Returns the node containing a caret address. This is different from the 
 * 'node' field in the CaretAddress structure, which contains the first node
 * in the parent chain that establishes a layout (i.e. for adresses inside an
 * inline container, it will point to the container). */
const Node *node_at_caret(CaretAddress address)
{
	const Node *node = address.node;
	if (node != NULL && node->layout == LAYOUT_INLINE_CONTAINER)
		node = inline_node_at(node, address.offset);
	return node;
}

/* Clamps an internal address to the start or end of the containing node. */
static unsigned closer_end(const Node *node, unsigned ia, AddressRewriteMode mode)
{
	if (node->layout == LAYOUT_INLINE_CONTAINER) {
		const InlineContext *icb = node->icb;
		bool after;
		if (mode == ARW_TIES_TO_CLOSER) {
			after = ia >= icb->num_elements / 2;
		} else {
			if (same_internal_address(node, ia, 0))
				after = false;
			else if (same_internal_address(node, ia, IA_END))
				after = true;
			else 
				after = (mode == ARW_TIES_TO_END);
		}
		ia = after ? IA_END : 0;
	}
	return ia;
}

/* Modifies a caret address so that if its node is the child of an inline 
 * container, the modified address refers to the container, and gives the
 * position of the child as an internal address. */
CaretAddress canonical_address(const Document *document, CaretAddress address)
{
	const Node *node = address.node;
	if (node != NULL) {
		const Node *container = find_inline_container_not_self(document, node);
		if (container != NULL) {
			address.node = container;
			address.offset = internal_address_of(container, node);
		}
	}
	return address;
}

/* Returns the caret address before a node. */
CaretAddress start_address(const Document *document, const Node *node)
{
	CaretAddress address = { node, 0 };
	return canonical_address(document, address);
}

/* Returns the caret address after a node. */
CaretAddress end_address(const Document *document, const Node *node)
{
	CaretAddress address = { node, IA_END };
	return canonical_address(document, address);
}

/* Attempts to rewrite a caret address in terms of a node in its parent chain. */
static CaretAddress rewrite_address(const Document *document, 
	const Node *parent, CaretAddress address, 
	AddressRewriteMode mode = ARW_TIES_TO_CLOSER)
{
	while (address.node != NULL && address.node != parent) {
		address.offset = closer_end(address.node, address.offset, mode);
		const Node *container = find_inline_container_not_self(document, address.node);
		if (container != NULL) {
			unsigned ia = internal_address_of(container, address.node);
			address.offset = ia + (address.offset == IA_END ? 1 : 0);
			address.node = container;
		} else {
			address.node = address.node->t.parent.node;
		}
	}
	return canonical_address(document, address);
}

/* Returns the closest address to 'address' inside the specified node. */
unsigned closest_internal_address(const Document *document, 
	const Node *node, CaretAddress address, AddressRewriteMode mode)
{
	/* Try to rewrite the address in terms of the target node. If we succeed, 
	 * the address is inside the target's subtree and we have the closest 
	 * position. */
	CaretAddress b_wrt_a = rewrite_address(document, node, address, mode);
	if (b_wrt_a.node != NULL)
		return b_wrt_a.offset;

	/* Construct positions at the ends of the target node and try to rewrite 
	 * them in terms of address.node. If we succeed, the target node is within 
	 * the subtree of address.node, and the result is whichever end of the 
	 * target node is closer to the address. */
	CaretAddress a0_wrt_b = rewrite_address(document, address.node, 
		start_address(document, node), mode);
	CaretAddress a1_wrt_b = rewrite_address(document, address.node, 
		end_address(document, node), mode);
	if (a0_wrt_b.node != NULL) {
		unsigned ia_a0 = expand_internal_address(node, a0_wrt_b.offset);
		unsigned ia_a1 = expand_internal_address(node, a1_wrt_b.offset);
		unsigned ia_b = expand_internal_address(address.node, address.offset);
		if (ia_a0 >= ia_b) 
			return 0;
		if (ia_b >= ia_a1) 
			return IA_END;
		/* The address is inside the interval occupied by 'node' within 
		 * 'address.node'. */
		return closer_end(node, ia_b, mode);
	}

	/* The target node and the address are in different subtrees. Return a 
	 * position at the beginning or end of the target node depending on their 
	 * tree order. */
	return tree_before(&address.node->t, &node->t) ? 0 : IA_END;
}

/* Stack frame used by the caret walk iterator. */
struct CaretWalkerFrame { const Node *jump_to; };

/* Initializes a caret-walk iterator, returning the first node. The mask is a
 * node flag the walker needs to keep track of nodes it has visited. It must
 * be clear in all nodes before iteration starts. */
const Node *cwalk_first(const Document *document, TreeIterator *ti, 
	CaretAddress start, CaretAddress end)
{
	start = canonical_address(document, start);
	end = canonical_address(document, end);
	if (caret_before(end, start))
		std::swap(start, end);
	const Node *start_node = node_at_caret(start);
	const Node *end_node = node_at_caret(end);

	/* If the start node is an inline child, arrange to visit its parent before
	 * jumping back to the child. */
	tree_iterator_init(ti);
	CaretWalkerFrame *frame = (CaretWalkerFrame *)tree_iterator_push(ti);
	if (start_node != start.node) {
		frame->jump_to = start_node;
		start_node = start.node;
	} else {
		frame->jump_to = NULL; 
	}

	tree_iterator_begin(ti, document, 
		&start_node->t, 
		&end_node->t,
		sizeof(CaretWalkerFrame));
	return start_node;
}

/* Returns the next node between two caret positions. */
const Node *cwalk_next(const Document *document, TreeIterator *ti)
{
	/* If the current node's stack entry is not a NULL pointer, it's a node 
	 * that should be jumped to. Perform the jump now, reusing the stack entry
	 * for the jump target. */
	CaretWalkerFrame *frame = (CaretWalkerFrame *)ti->frame;
	const Node *jump_target = frame->jump_to;
	if (jump_target != NULL) {
		tree_iterator_revisit(ti, &jump_target->t);
		frame->jump_to = NULL;
		jump_target = NULL;
	} else {
		/* Step until we find a node we haven't visited yet. */
		bool visit;
		do {
			unsigned flags = tree_iterator_step(ti);
			if ((flags & TIF_VISIT_POSTORDER) != 0) {
				/* If the stack is empty, we didn't visit this node on the way 
				 * down, so we must visit it now. */
				visit = !tree_iterator_pop(ti);
				/* If we just stepped upwards into a container we haven't 
				 * visited before, arrange to visit the container before coming
				 * back to the child. */
				if (visit) {
					const Node *node = (Node *)ti->node;
					if (node->layout == LAYOUT_INLINE) {
						const Node *container = find_inline_container_not_self(document, node);
						tree_iterator_revisit(ti, &container->t);
						jump_target = node;
					}
				}
			} else {
				visit = true;
			}
		} while (!visit);
	}

	/* Push a frame for the node we're visiting. */
	frame = (CaretWalkerFrame *)tree_iterator_push(ti);
	frame->jump_to = jump_target;
	return (Node *)ti->node;
}

/* Sets paragraph element selection bits in the interval [start, end) and clears
 * the rest. */
static void rewrite_selection_bits(ParagraphElement *elements, 
	unsigned num_elements, unsigned start, unsigned end)
{
	assertb(start <= num_elements);
	assertb(end <= num_elements);
	assertb(end >= start);
	for (unsigned i = 0; i < start; ++i)
		elements[i].is_selected = false;
	for (unsigned i = start; i != end; ++i)
		elements[i].is_selected = true;
	for (unsigned i = end; i != num_elements; ++i)
		elements[i].is_selected = false;
}

/* Sets the range of selected elements in an inline container. */
void set_selected_element_range(Document *document, Node *node, 
	CaretAddress start, CaretAddress end)
{
	InlineContext *icb = node->icb;
	unsigned start_offset = closest_internal_address(document, node, start, ARW_TIES_TO_END);
	unsigned end_offset = closest_internal_address(document, node, end, ARW_TIES_TO_START);
	rewrite_selection_bits(icb->elements, icb->num_elements, start_offset, end_offset);
}

/* Reads the first run of contiguous selected paragraph elements in an inline
 * container as a string. */
unsigned read_selected_text(const Document *, const Node *container, 
	void *buffer, TextEncoding encoding)
{
	const InlineContext *icb = container->icb;
	unsigned i, j;
	for (i = 0; i != icb->num_elements; ++i)
		if (icb->elements[i].is_selected)
			break;
	for (j = i; j != icb->num_elements; ++j)
		if (!icb->elements[j].is_selected)
			break;
	return encode_paragraph_elements(icb->elements + i, j - i, buffer, 
		encoding, true);
}

/* Returns the first line box in the parent chain of a box. */
static const Box *containing_line_box(const Document *, const Box *box)
{
	while (box != NULL && (box->t.flags & BOXFLAG_IS_LINE_BOX) == 0)
		box = box->t.parent.box;
	return box;
}

/* Returns the start of the range of paragraph elements displayed by the line 
 * containing 'box'. */
unsigned start_of_containing_line(const Document *document, const Box *box)
{
	const Box *line_box = containing_line_box(document, box);
	assertb(line_box != NULL);
	return line_box != NULL ? line_box->first_element : 0;
}

/* Returns the end of the range of paragraph elements displayed by the line 
 * containing 'box'. */
unsigned end_of_containing_line(const Document *document, const Box *box)
{
	const Box *line_box = containing_line_box(document, box);
	assertb(line_box != NULL);
	return line_box != NULL ? line_box->last_element : 0;
}

} // namespace stkr
