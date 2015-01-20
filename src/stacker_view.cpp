#include "stacker_view.h"

#include <algorithm>

#include "stacker.h"
#include "stacker_shared.h"
#include "stacker_encoding.h"
#include "stacker_attribute.h"
#include "stacker_util.h"
#include "stacker_document.h"
#include "stacker_box.h"
#include "stacker_inline2.h"
#include "stacker_layer.h"
#include "stacker_system.h"
#include "stacker_platform.h"

namespace stkr {

const unsigned DEFAULT_VIEW_BOX_CAPACITY = 256;

const uint16_t NO_BOX = 0xFFFF;

const unsigned KEY_LAYER_BITS = 3;
const unsigned KEY_LAYER_MASK = (1 << KEY_LAYER_BITS) - 1;

/* An internal command representing a text layer. Will never appear in a final
 * command list. */
const DrawCommand DCMD_TEXT_LAYER = DrawCommand(DCMD_END + 1);

const unsigned CLIP_MEMORY_SIZE = 4;

/* A circular queue storing a smaller number of the most recently used clip
 * rectangles. These rectangles can be referenced in subsequent clip commands,
 * reducing the size of the command list. */
struct ClipMemory {
	float rectangles[CLIP_MEMORY_SIZE][4];
	const ClipCommandData *data[CLIP_MEMORY_SIZE];
	unsigned head;
	unsigned tail;
};

DrawCommand view_next_command(ViewCommandIterator *iter, const void **data)
{
	const View *view = iter->view;
	if (iter->position == view->num_headers) {
		*data = NULL;
		return DCMD_END;
	}
	const DrawCommandHeader *header = view->headers + view->header_start + 
		iter->position;
	iter->position++;
	*data = view->command_data + header->data_offset;
	return (DrawCommand)header->command;
}

DrawCommand view_first_command(const View *view, ViewCommandIterator *iter,
	const void **data)
{
	iter->view = view;
	iter->position = 0;
	return view_next_command(iter, data);
}

View *create_view(Document *document, unsigned flags)
{
	int id = allocate_view_id(document);
	if (id == INVALID_VIEW_ID)
		return NULL;

	View *view = new View();
	view->document = document;
	view->id = id;
	view->visibility_stamp = 0;
	view->flags = flags;
	std::fill(view->bounds, view->bounds + 4, 0.0f);
	view->layout_clock = document->update_clock - 1;
	view->paint_clock = unsigned(-1);
	view->headers = NULL;
	view->num_headers = 0;
	view->header_capacity = 0;
	view->header_start = 0;
	view->command_data = NULL;
	view->command_data_size = 0;
	view->command_data_capacity = 0;
	view->boxes = NULL;
	view->box_capacity = 0;
	view->num_boxes = 0;

	add_to_view_list(document, view);

	return view;
}

void destroy_view(View *view)
{
	if (view == view->document->selection_view)
		clear_selection(view->document);
	deallocate_view_id(view->document, view->id);
	remove_from_view_list(view->document, view);
	delete [] view->headers;
	delete [] view->command_data;
	delete [] view->boxes;
	delete view;
}

/* If a view's constrain-the-document flags have changed, propagates the 
 * constraints to the document and changes the corresponding flag. Does nothing
 * if the flags haven't changed, to avoid interfering with other views. */
static void view_update_document_constraints(View *view, unsigned old_flags = 0)
{
	Document *document = view->document;

	unsigned changed = old_flags ^ view->flags;
	if ((changed & VFLAG_CONSTRAIN_DOCUMENT_WIDTH) != 0) {
		bool constrain = (view->flags & VFLAG_CONSTRAIN_DOCUMENT_WIDTH) != 0;
		set_document_flags(document, DOCFLAG_CONSTRAIN_WIDTH, constrain);
		set_root_dimension(document, AXIS_H, round_signed(rwidth(view->bounds)));
	}
	if ((changed & VFLAG_CONSTRAIN_DOCUMENT_HEIGHT) != 0) {
		bool constrain = (view->flags & VFLAG_CONSTRAIN_DOCUMENT_HEIGHT) != 0;
		set_document_flags(document, DOCFLAG_CONSTRAIN_HEIGHT, constrain);
		set_root_dimension(document, AXIS_V, round_signed(rheight(view->bounds)));
	}
}

unsigned get_view_flags(const View *view)
{
	return view->flags;
}

/* Changes the value of a mask of view flags to 'value'. */
void set_view_flags(View *view, unsigned flags, bool value)
{
	unsigned old_flags = view->flags;
	view->flags = set_or_clear(view->flags, flags, value);
	if (view->flags == old_flags)
		return;
	view_update_document_constraints(view, old_flags);
	view->flags |= VFLAG_REBUILD_COMMANDS;
	view->paint_clock++;
}

/* The number returned by this function changes every time the view changes
 * in a way that requires a repaint. The owner(s) of the view can keep their
 * own copy and repaint when that copy no longer matches the view's clock. */
unsigned get_paint_clock(const View *view)
{
	unsigned paint_clock = view->paint_clock << 1;
	paint_clock |= (unsigned)needs_update(view->document);
	paint_clock |= (unsigned)(view->document->update_clock != view->layout_clock);
	return paint_clock;
}

void set_view_bounds(View *view, float x0, float x1, float y0, float y1)
{
	float new_bounds[4];
	rset(new_bounds, x0, x1, y0, y1);
	set_view_bounds(view, new_bounds);
}

/* Sets the document space rectangle a view displays. */
void set_view_bounds(View *view, float *bounds)
{
	if (requal(view->bounds, bounds))
		return;
	std::copy(bounds, bounds + 4, view->bounds);
	view_update_document_constraints(view);
	view->flags |= VFLAG_REBUILD_COMMANDS;
	view->paint_clock++;
}

static void allocate_box_list(View *view, unsigned new_capacity)
{
	delete [] view->boxes;
	view->boxes = new Box *[new_capacity];
	view->box_capacity = new_capacity;
}

static void query_boxes(View *view)
{
	view->num_boxes = grid_query_rect(
		view->document, 
		view->boxes, 
		view->box_capacity, 
		rleft(view->bounds), 
		rright(view->bounds), 
		rtop(view->bounds), 
		rbottom(view->bounds));
}

/* Finds all the boxes visible in this view. */
static void find_visible_boxes(View *view)
{
	if (view->box_capacity == 0)
		allocate_box_list(view, DEFAULT_VIEW_BOX_CAPACITY);
	query_boxes(view);
	if (view->num_boxes > view->box_capacity) {
		allocate_box_list(view, view->num_boxes);
		query_boxes(view);
	}
}

/* Marks each box in the box list as visible in this view. */
static void set_visibility_bits(View *view)
{
	unsigned visible_flag = 1 << BOXFLAG_VISIBLE_SHIFT << view->id;
	for (unsigned i = 0; i < view->num_boxes; ++i) {
		Box *box = view->boxes[i];
		box_advise_visible(view->document, box, view);
		box->t.flags |= visible_flag;
	}
}

/* Updates the list of boxes that need to be drawn for a view. */
static void view_update_box_list(View *view)
{
	find_visible_boxes(view);
	set_visibility_bits(view);
}

inline unsigned make_command_key(int depth, int layer_key = 0)
{
	return (depth << KEY_LAYER_BITS) + layer_key;
}

inline unsigned make_command_key(int depth, const VisualLayer *layer)
{
	return make_command_key(depth + layer->depth_offset, layer->key);
}

/* Appends a command header to the command list. */
static void view_add_command_header(View *view, DrawCommand command, 
	uintptr_t data_offset, int key, unsigned box_index)
{
	if (view->num_headers < view->header_capacity) {
		DrawCommandHeader *header = view->headers + view->header_start + 
			view->num_headers;
		header->command = command;
		header->key = key;
		header->box_index = (uint16_t)box_index;
		header->data_offset =  data_offset;
	}
	view->num_headers++;
}

/* Appends a copy of an existing header to the command list. */
static void view_add_command_header(View *view, const DrawCommandHeader *header)
{
	if (view->num_headers < view->header_capacity)
		view->headers[view->header_start + view->num_headers] = *header;
	view->num_headers++;
}

/* Appends a command with the specified amount of associated data to the command
 * list. */
static void *view_add_command(View *view, DrawCommand command,
	uint32_t data_size, int key, unsigned box_index)
{
	unsigned required = view->command_data_size + data_size;
	void *data = view->command_data + view->command_data_size;
	view_add_command_header(view, command, view->command_data_size, key, 
		box_index);
	view->command_data_size = required;
	return required <= view->command_data_capacity ? data : NULL;
}

/* Helper to add a draw-text command. */
static TextCommandData *view_add_text_command(View *view, 
	unsigned num_code_units, unsigned num_characters, unsigned num_colors,
	int16_t font_id, int key)
{
	const System *system = view->document->system;
	bool multi_line = (system->flags & SYSFLAG_SINGLE_LINE_TEXT_LAYERS) == 0;

	unsigned bytes_required = sizeof(TextCommandData);
	unsigned text_bytes = (num_code_units + 1) *
		BYTES_PER_CODE_UNIT[system->encoding];
	bytes_required += text_bytes;
	bytes_required += num_characters * sizeof(int); // X positions.
	if (multi_line)
		bytes_required += num_characters * sizeof(int); // Y positions.
	bytes_required += num_colors * sizeof(uint32_t); // Colors.
	bytes_required += num_colors * sizeof(uint32_t); // Color code unit counts.
	bytes_required += num_colors * sizeof(uint32_t); // Color character counts.
	char *block = (char *)view_add_command(view, DCMD_TEXT, bytes_required, key, NO_BOX);
	if (block == NULL)
		return NULL;

	TextCommandData *d = (TextCommandData *)block;
	block += sizeof(TextCommandData);
	d->font_id = font_id;
	d->length = num_characters;
	d->num_colors = num_colors;
	d->text.bytes = block;
	block += text_bytes;
	d->x_positions = (int *)block;
	block += num_characters * sizeof(int);
	if (multi_line) {
		d->y_positions = (int *)block;
		block += num_characters * sizeof(int);
	} else {
		d->line_y_position = 0;
	}
	d->colors = (uint32_t *)block;
	block += num_colors * sizeof(uint32_t);
	d->color_code_unit_counts = (uint32_t *)block;
	block += num_colors * sizeof(uint32_t);
	d->color_character_counts = (uint32_t *)block;
	block += num_colors * sizeof(uint32_t);
	return d;
}

/* Orders view command headers by they depth|type key, deepest first. */
static void view_sort_commands(View *view)
{
	unsigned count = view->num_headers;
	DrawCommandHeader *a = view->headers + view->header_start;
	DrawCommandHeader *b = view->headers + 
		(view->header_start ^ view->header_capacity);
	unsigned freq_low[256] = { 0 };
	unsigned freq_high[256] = { 0 };
	for (unsigned i = 0; i < count; ++i) {
		uint16_t key = (uint16_t)a[i].key;
		freq_low[key & 0xFF]++;
		freq_high[key >> 8]++;
	}
	unsigned sum_low = 0, sum_high = 0;
	for (unsigned i = 0; i < 256; ++i) {
		unsigned low = freq_low[i];
		unsigned high = freq_high[i];
		freq_low[i] = sum_low;
		freq_high[i] = sum_high;
		sum_low += low;
		sum_high += high;
	}
	for (unsigned i = 0; i < count; ++i)
		b[freq_low[a[i].key & 0xFF]++] = a[i];
	if (freq_high[1] != count) {
		for (unsigned i = 0; i < count; ++i)
			a[freq_high[uint16_t(b[i].key) >> 8]++] = b[i];
	} else {
		view->header_start ^= view->header_capacity;
	}
}

/* Generates a SET_CLIP command, omitting the command or reusing an already-
 * stored rectangle if possible. */
static void view_set_clip(View *view, ClipMemory *memory, const float *r)
{
	unsigned i;
	for (i = memory->head; i != memory->tail; i = (i + 1)  % CLIP_MEMORY_SIZE) {
		if (0 == memcmp(memory->rectangles[i], r, 4 * sizeof(float)))
			break;
	}
	if (i == memory->tail) {
		ClipCommandData *cd = (ClipCommandData *)view_add_command(view, 
			DCMD_SET_CLIP, sizeof(ClipCommandData), 0, NO_BOX);
		if (cd != NULL)
			rect_intersect(view->bounds, r, cd->clip);
		unsigned next = (memory->tail + 1) % CLIP_MEMORY_SIZE;
		if (next == memory->head)
			memory->head = (memory->head + 1) % CLIP_MEMORY_SIZE;
		memory->data[memory->tail] = cd;
		memcpy(memory->rectangles[memory->tail], r, 4 * sizeof(float));
		memory->tail = next;
	} else {
		uintptr_t offset = (uint8_t *)memory->data[i] - view->command_data;
		view_add_command_header(view, DCMD_SET_CLIP, offset, 0, NO_BOX);
	}
}

/* Adds drawing commands for a pane layer. */
static void view_add_pane_commands(View *view, unsigned box_index, 
	const VisualLayer *layer, int depth)
{
	const Box *box = view->boxes[box_index];
	if (layer->pane.pane_type == PANE_FLAT) {
		unsigned key = make_command_key(depth, layer);
		RectangleCommandData *d = (RectangleCommandData *)view_add_command(
			view, DCMD_RECTANGLE, sizeof(RectangleCommandData), key, box_index);
		if (d == NULL)
			return;
		compute_layer_position(box, &layer->pane.position, d->bounds);
		d->fill_color = layer->pane.fill_color;
		d->border_color = layer->pane.border_color;
		d->border_width = layer->pane.border_width;
	} else if (layer->pane.pane_type == PANE_RAISED) {
		/* FIXME (TJM): NYI. Built-in image? */
	} else if (layer->pane.pane_type == PANE_SUNKEN) {
		/* FIXME (TJM): NYI. Built-in image? */
	}
}

/* Adds drawing commands for an image layer. */
static void view_add_image_commands(View *view, unsigned box_index,
	const VisualLayer *layer, int depth)
{
	/* Are we still waiting for the image? */
	if ((layer->flags & VLFLAG_IMAGE_AVAILABLE) == 0)
		return;

	/* Add an image command. */
	unsigned key = make_command_key(depth, layer);
	ImageCommandData *d = (ImageCommandData *)view_add_command(view, DCMD_IMAGE,
		sizeof(ImageCommandData), key, box_index);
	if (d == NULL)
		return;

	/* Get the bitmap handle. */
	const ImageLayer *il = &layer->image;
	BackEnd *back_end = view->document->system->back_end;
	UrlCache *cache = view->document->system->url_cache;
	void *system_image = platform_get_network_image_data(back_end, cache, 
		il->image_handle);
	ensure(system_image != NULL);

	/* Configure the command. */
	d->system_image = system_image;
	d->tint = layer->image.tint;
	unsigned natural_width, natural_height;
	bool has_natural_size = platform_get_network_image_info(back_end, 
		cache, il->image_handle, &natural_width, &natural_height);

	const Box *box = view->boxes[box_index];
	compute_layer_position(box, &layer->image.position, d->bounds, 
		(float)natural_width, (float)natural_height, has_natural_size);
}

static void view_add_text_layer_commands(View *view, unsigned box_index, 
	const VisualLayer *layer, int depth)
{
	unsigned key = make_command_key(depth, layer);
	uintptr_t data_offset = (uintptr_t)layer;
	view_add_command_header(view, DCMD_TEXT_LAYER, data_offset, key, box_index);
}

static void view_add_box_layer_commands(View *view, int clip_index)
{
	const Box *box = view->boxes[clip_index];
	if (box->layers == NULL)
		return;
	LayerKey last_key = (LayerKey)box->layers->key;
	int depth = box->depth;
	for (const VisualLayer *layer = box->layers; layer != NULL; 
		layer = layer->next[VLCHAIN_BOX]) {
		depth += ((LayerKey)layer->key != last_key);
		last_key = (LayerKey)layer->key;
		switch (layer->type) {
			case VLT_PANE:
				view_add_pane_commands(view, clip_index, layer, depth);
				break;
			case VLT_IMAGE:
				view_add_image_commands(view, clip_index, layer, depth);
				break;
			case VLT_TEXT:
				view_add_text_layer_commands(view, clip_index, layer, depth);
				break;
		}
	}
}

/* Helper to build a text command to draw things like debug labels. */
static void add_simple_text_command(View *view, int x0, int y0, 
	const char *text, unsigned length, unsigned num_characters, 
	const unsigned *advances, int16_t font_id, uint32_t color, int key)
{
	System *system = view->document->system;

	/* Measure the text if advances were not supplied. */
	const unsigned *adv = advances;
	if (adv == NULL) {
		adv = new unsigned[length];
		num_characters = measure_text(system, font_id, text, length, 
			(unsigned *)adv);
	}

	/* Build the draw-text command. */
	unsigned encoded_code_units = utf8_transcode(text, length, NULL, 
		system->encoding);
	TextCommandData *td = view_add_text_command(view, encoded_code_units, 
		num_characters, 1, font_id, key);
	if (td == NULL)
		goto cleanup;
	utf8_transcode(text, length, (void *)td->text.bytes, system->encoding);
	*(uint32_t *)td->colors = color;
	*(uint32_t *)td->color_code_unit_counts = length;
	*(uint32_t *)td->color_character_counts = num_characters;
	int dx = 0;
	for (unsigned i = 0; i < num_characters; ++i) {
		((int *)td->x_positions)[i] = x0 + 
			round_fixed_to_int(dx, TEXT_METRIC_PRECISION);
		dx += adv[i];
	}
	if ((system->flags & SYSFLAG_SINGLE_LINE_TEXT_LAYERS) == 0) {
		for (unsigned i = 0; i < num_characters; ++i)
			((int *)td->y_positions)[i] = y0;
	} else {
		td->line_y_position = y0;
	}

cleanup:
	
	if (advances == NULL)
		delete [] adv;
}

/* Draws a debug label box. */
static void view_draw_box_label(View *view, const char *label, 
	unsigned length, const float *bounds, Alignment align_h, Alignment align_v, 
	float pad_h, float pad_v, uint32_t background_color, uint32_t text_color,
	int depth)
{
	if (length == 0)
		return;

	/* Measure the label. */
	System *system = view->document->system;
	int16_t label_font_id = get_debug_label_font_id(system);
	unsigned text_width, text_height, *advances = NULL;
	unsigned num_characters = measure_text_rectangle(system, label_font_id, 
		label, length, &text_width, &text_height, &advances);

	/* Calculate the label rectangle and place it within the bounding 
	 * rectangle. */
	float bg_width = (float)text_width + 2 * pad_h;
	float bg_height = (float)text_height + 2 * pad_v;
	float bg_rect[4], text_rect[4];
	align_rectangle(align_h, align_v, bg_width, bg_height, 0.0f, 0.0f, 
		bounds, bg_rect);
	align_rectangle(ALIGN_MIDDLE, ALIGN_MIDDLE, (float)text_width, 
		(float)text_height, 0.0f, 0.0f, bg_rect, text_rect); 

	/* Add a command to draw the background. */
	RectangleCommandData *d = (RectangleCommandData *)view_add_command(view, 
		DCMD_RECTANGLE, sizeof(RectangleCommandData), 
		make_command_key(depth, 0), NO_BOX);
	if (d != NULL) {
		std::copy(bg_rect, bg_rect + 4, d->bounds);
		d->border_color = 0;
		d->border_width = 0;
		d->fill_color = background_color;
	}

	/* Add a command to draw the label. */
	int x = round_signed(rleft(text_rect));
	int y = round_signed(rtop(text_rect));
	add_simple_text_command(view, x, y, label, length, num_characters, 
		advances, label_font_id, text_color, make_command_key(depth, 1));
	delete [] advances;
}

static void view_add_debug_rectangle_commands(View *view, const Box *box, 
	const float *r, uint32_t bg_color, uint32_t text_color,	
	int depth, bool draw_label)
{
	RectangleCommandData *d = NULL;

	/* Outline the rectangle. */
	d = (RectangleCommandData *)view_add_command(view, DCMD_RECTANGLE, 
		sizeof(RectangleCommandData), make_command_key(depth, 0), NO_BOX);
	if (d != NULL) {
		memcpy(d->bounds, r, sizeof(d->bounds));
		d->border_color = bg_color;
		d->border_width = 1;
		d->fill_color = 0;
	}

	/* Add a label. */
	if (draw_label) {
		/* Build the label text. */
		static const unsigned MAX_LABEL_LENGTH = 256;
		char label[MAX_LABEL_LENGTH];
		int length = snprintf(label, sizeof(label), "%s: %.0fx%.0f", 
			get_box_debug_string(box), rwidth(r), rheight(r));
		if (length < 0)
			length = sizeof(label) - 1;
		label[length] = '\0';

		/* Add commands to draw the label. */
		view_draw_box_label(view, label, length, r, 
			ALIGN_END, ALIGN_START, 2.0f, 1.0f, bg_color, text_color, 
			make_command_key(depth, 1));
	}
}

static void view_add_box_debug_commands(View *view, const Box *box)
{
	if ((view->flags & VFLAG_DEBUG_MASK) == 0)
		return;

	const Document *document = view->document;

	int depth = 100 + box_tree_depth(box) * 6;
	RectangleCommandData *d;
	
	/* Avoid drawing the same rectangle multiple times to avoid clutter. */
	float outer[4], padding[4], content[4];
	outer_rectangle(box, outer);
	padding_rectangle(box, padding);
	content_rectangle(box, content);
	bool draw_outer = (view->flags & VFLAG_DEBUG_OUTER_BOXES) != 0;
	bool draw_padding = (view->flags & VFLAG_DEBUG_PADDING_BOXES) != 0;
	bool draw_content = (view->flags & VFLAG_DEBUG_CONTENT_BOXES) != 0;
	if (draw_padding && draw_outer && requal(padding, outer))
		draw_outer = false;
	if (draw_content && draw_padding && requal(content, padding))
		draw_padding = false;

	bool mouse_over = is_mouse_over(document, box);
	bool draw_labels = mouse_over && (box->t.flags & BOXFLAG_NO_LABEL) == 0;
	uint32_t tint = mouse_over ? 0xFFFFFFFF : 0xFF808080;

	/* Outer box. */
	if (draw_outer) {
		view_add_debug_rectangle_commands(view, box, outer, 
			blend32(0xFF66BA66, tint), 0xFFFFFFFF, depth, draw_labels);
		depth += 2;
	}
	/* Padding box. */
	if (draw_padding) {
		view_add_debug_rectangle_commands(view, box, padding, 
			blend32(0xFFEDC84C, tint), 0xFF000000, depth, draw_labels);
		depth += 2;
	}
	/* Content box. */
	if (draw_content) {
		view_add_debug_rectangle_commands(view, box, content, 
			blend32(0xFF4CCAED, tint), 0xFF000000, depth, draw_labels);
		depth += 2;
	}

	/* Show moused-over elements. */
	if ((view->flags & VFLAG_DEBUG_MOUSE_HIT) != 0 && is_mouse_over(document, box)) {
		d = (RectangleCommandData *)view_add_command(view, 	DCMD_RECTANGLE, 
			sizeof(RectangleCommandData), make_command_key(depth), NO_BOX);
		if (d == NULL)
			return;
		outer_rectangle(box, d->bounds);
		d->border_color = 0xFF00FF00;
		d->border_width = 2;
		d->fill_color = 0;
		depth += 1;
	}
}

static unsigned grow_capacity(unsigned n)
{
	return ((n * 3 / 2) + 15) & unsigned(-16);
}

static bool view_grow_buffers(View *view)
{
	bool rebuild = false;
	if (view->num_headers > view->header_capacity) {
		unsigned new_capacity = grow_capacity(view->num_headers);
		delete [] view->headers;
		view->headers = new DrawCommandHeader[new_capacity * 2];
		view->header_capacity = new_capacity;
		rebuild = true;
	}
	if (view->command_data_size > view->command_data_capacity) {
		delete [] view->command_data;
		unsigned new_capacity = grow_capacity(view->command_data_size);
		view->command_data = new uint8_t[new_capacity];
		view->command_data_capacity = new_capacity;
		rebuild = true;
	}
	return rebuild;
}

static void view_build_box_commands(View *view)
{
	view->header_start = 0;
	do {
		view->num_headers = 0;
		view->command_data_size = 0;
		for (unsigned i = 0; i < view->num_boxes; ++i) {
			view_add_box_layer_commands(view, i);
			view_add_box_debug_commands(view, view->boxes[i]);
		}
	} while (view_grow_buffers(view));
}

/* Helper to return the key from the text layer associated with a text fragment
 * command. */
inline const VisualLayer *get_text_layer(const DrawCommandHeader *header)
{
	return ((const VisualLayer *)header->data_offset);
}

/* A slice of a text layer. Fragments are the atomic units of text drawing, and
 * contain characters that are styled identically. */
struct TextFragment {
	const Box *box;
	const VisualLayer *layer;
	const TextStyle *style;
	uint32_t text_start;
	uint32_t text_end;
	uint32_t start     : 31; 
	uint32_t end       : 31;
	bool run_start     : 1;
	bool selected      : 1;
};

/* True if A and B can be part of the same draw-text command. */
inline bool fragments_draw_compatible(const TextFragment *a, 
	const TextFragment *b, bool single_line)
{
	if (!measurement_compatible(a->style, b->style))
		return false;
	if (a->box->clip_ancestor != b->box->clip_ancestor)
		return false;
	if (single_line && a->box->axes[AXIS_V].pos != b->box->axes[AXIS_V].pos)
		return false;
	return true;
}

/* Operator used to order drawing groups into compatible clusters. */
inline bool fragment_less(const TextFragment *a, 
	const TextFragment *b, bool single_line) 
{
	if (!fragments_draw_compatible(a, b, single_line))
		return a < b;
	if (a->layer->text.container != b->layer->text.container)
		return a->layer->text.container < b->layer->text.container;
	return a->style->color < b->style->color;
}

static void safe_swap_fragments(TextFragment &a, TextFragment &b)
{
	assertb(!a.run_start);
	assertb(!b.run_start);
	std::swap(a, b);
}

/* A three way quicksort used to order text fragments into "clusters" of
 * compatible fragments, and those clusters into colour runs. The first fragment
 * in each colour run is marked. Efficient when there are only a few equivalence 
 * classes, as is typically the case. */
void quicksort_fragments(TextFragment *a, unsigned count, bool single)
{
	while (count > 1) {
		TextFragment pivot = a[1];
		unsigned i = unsigned(-1), j = count;
		unsigned p = i, q = j;
		for (;;) {
			do { ++i; } while (fragment_less(a + i, &pivot, single));
			do { --j; } while (j >= i && fragment_less(&pivot, a + j, single));
			if (j <= i) break;
			safe_swap_fragments(a[i], a[j]);
			if (!fragment_less(a + i, &pivot, single)) 
				safe_swap_fragments(a[i], a[++p]);
			if (!fragment_less(&pivot, a + j, single))
				safe_swap_fragments(a[j], a[--q]);
		}
		if (i == j) 
			safe_swap_fragments(a[i], a[--q]);
		j = i;
		while (p + 1 != 0) 
			safe_swap_fragments(a[p--], a[--i]);
		while (q != count) 
			safe_swap_fragments(a[q++], a[j++]);
		a[i].run_start = true;
	
		unsigned count_greater = count - j;
		if (count_greater > i) {
			quicksort_fragments(a, i, single);
			a += j;
			count = count_greater;
		} else {
			quicksort_fragments(a + j, count_greater, single);
			count = i;
		}
	}
	if (count != 0)
		a[0].run_start = true;
}

const unsigned COMBINER_STATIC_FRAGMENTS = 256;

struct TextCombiner {
	TextFragment fragment_buffer[COMBINER_STATIC_FRAGMENTS];
	TextFragment *fragments;
	unsigned capacity;
	unsigned count;
};

static void combiner_reset(TextCombiner *combiner)
{
	combiner->count = 0;
}

static void combiner_init(TextCombiner *combiner)
{
	combiner->fragments = combiner->fragment_buffer;
	combiner->capacity = COMBINER_STATIC_FRAGMENTS;
	combiner_reset(combiner);
}

static void combiner_deinit(TextCombiner *combiner)
{
	if (combiner->fragments != combiner->fragment_buffer)
		delete [] combiner->fragments;
}

static void combiner_add_fragment(TextCombiner *c, 
	const Box *box, const VisualLayer *layer, 
	unsigned element_start, unsigned element_end, 
	unsigned text_start, unsigned text_end, 
	const TextStyle *style, bool selected)
{
	assertb(element_start <= layer->text.num_characters);
	assertb(element_end <= layer->text.num_characters);
	assertb(text_start <= text_end);

	if (c->count == c->capacity) {
		c->capacity *= 2;
		TextFragment *nf = new TextFragment[c->capacity];
		memcpy(nf, c->fragments, c->count * sizeof(TextFragment));
		if (c->fragments != c->fragment_buffer)
			delete [] c->fragments;
		c->fragments = nf;
	}

	TextFragment *fragment = c->fragments + c->count;
	fragment->box = box;
	fragment->layer = layer;
	fragment->style = style;
	fragment->text_start = text_start;
	fragment->text_end = text_end;
	fragment->start = element_start;
	fragment->end = element_end;
	fragment->run_start = false;
	fragment->selected = selected;
	c->count++;
}

static void combiner_build_fragments(View *view, TextCombiner *combiner, 
	unsigned start, unsigned end)
{
	combiner_reset(combiner);
	ParagraphIterator ei;
	const DrawCommandHeader *headers = view->headers;
	for (unsigned i = start; i != end; ++i) {
		const DrawCommandHeader *header = headers + i;
		const VisualLayer *layer = get_text_layer(header);
		const Box *box = view->boxes[header->box_index];
		assertb((box->t.flags & BOXFLAG_IS_TEXT_BOX) != 0);
		iterate_fragments(&ei, view->document, layer->text.container, box);
		while (ei.count != 0) {
			combiner_add_fragment(
				combiner, box, layer, 
				ei.offset - layer->text.start, 
				ei.offset + ei.count - layer->text.start, 
				ei.text_start, 
				ei.text_end, 
				ei.style, 
				fragment_in_selection(&ei));
			next_fragment(&ei);
		}
	}
}

/* Length totals for an interval of draw-compatible fragments.  */
struct ClusterSizes {
	unsigned num_characters;
	unsigned num_code_units;
	unsigned num_palette_entries;
};

/* Orders fragments within the combiner, bringing together fragments that can
 * be part of the same draw command into "clusters". The fragments within a
 * cluster are further grouped by colour for palette generation. */
static void combiner_identify_clusters(View *view, TextCombiner *combiner)
{
	const System *system = view->document->system;
	quicksort_fragments(combiner->fragments, combiner->count, 
		(system->flags & SYSFLAG_SINGLE_LINE_TEXT_LAYERS) != 0);
}

/* Assembles a run of compatible text fragments into a final draw-text command. */
static void process_text_cluster(
	View *view, ClipMemory *clip_memory, 
	TextCombiner *combiner,
	unsigned start, unsigned end, 
	const ClusterSizes *sizes)
{
	/* Add a clip command for the cluster. Fragments within a cluster have the 
	 * same clip box. */
	const System *system = view->document->system;
	const TextFragment *first_fragment = combiner->fragments + start;
	view_set_clip(view, clip_memory, first_fragment->box->clip);

	/* Add the text command. */
	int16_t font_id = first_fragment->style->font_id;
	TextCommandData *d = view_add_text_command(view, sizes->num_code_units,
		sizes->num_characters, sizes->num_palette_entries, font_id, 0);
	if (d == NULL)
		return;

	unsigned byte_shift = ENCODING_BYTE_SHIFTS[system->encoding];
	char *text_pos = (char *)d->text.bytes;
	int *x_pos = (int *)d->x_positions;
	int *y_pos = NULL;
	uint32_t *colors = (uint32_t *)d->colors;
	uint32_t *code_unit_counts = (uint32_t *)d->color_code_unit_counts;
	uint32_t *character_counts = (uint32_t *)d->color_character_counts;

	/* Multi-line commands store a Y position for each character. Single line
	 * commands store the common Y position of all characters in the 
	 * cluster. */
	bool multi_line = (system->flags & SYSFLAG_SINGLE_LINE_TEXT_LAYERS) == 0;
	if (multi_line) {
		y_pos = (int *)d->y_positions;
	} else {
		float top = content_edge_lower(first_fragment->box, AXIS_V);
		d->line_y_position = round_signed(top);
	}

	int run_index = -1;
	for (unsigned i = start; i != end; ++i) {
		const TextFragment *fragment = combiner->fragments + i;
		const VisualLayer *layer = fragment->layer;
		const Box *box = fragment->box;

		/* Append the fragment's text. */
		const char *text = (const char *)get_text_layer_text(layer);
		unsigned text_start = fragment->text_start << byte_shift;
		unsigned text_end = fragment->text_end << byte_shift;
		unsigned text_bytes = text_end - text_start;
		memcpy(text_pos, text + text_start, text_bytes);
		text_pos += text_bytes;

		/* Copy in the fragment's positions, adding in the top-left position
		 * of the box. */
		int offset_x = round_signed(box->axes[AXIS_H].pos);
		int offset_y = round_signed(box->axes[AXIS_V].pos);
		const int *layer_x_positions = get_text_layer_positions(layer);
		for (unsigned j = fragment->start; j < fragment->end; ++j) {
			assertb(j < layer->text.num_characters);
			*x_pos++ = offset_x + layer_x_positions[j];
		}
		if (multi_line) {
			for (unsigned j = fragment->start; j < fragment->end; ++j)
				*y_pos++ = offset_y;
		}

		/* Add a palette entry if this is the first fragment of a colour run. */
		if (fragment->run_start) {
			run_index++;
			colors[run_index] = fragment->selected ? 
				view->document->selected_text_color :
				blend32(fragment->style->color, fragment->style->tint);
			code_unit_counts[run_index] = 0;
			character_counts[run_index] = 0;
		}
		code_unit_counts[run_index] += fragment->text_end - fragment->text_start;
		character_counts[run_index] += fragment->end - fragment->start;
	}
}

/* Converts each cluster of compatible fragments into a draw-text command. */
static void combiner_visit_clusters(View *view, ClipMemory *clip_memory, 
	TextCombiner *combiner)
{
	bool single_line = (view->document->system->flags & 
		SYSFLAG_SINGLE_LINE_TEXT_LAYERS) != 0;
	ClusterSizes sizes = { 0, 0, 0 };
	unsigned start = 0;
	const TextFragment *last = NULL;
	for (unsigned i = 0; i < combiner->count; ++i) {
		const TextFragment *fragment = combiner->fragments + i;
		const TextLayer *layer = &fragment->layer->text;
		if (fragment->run_start && last != NULL && 
			!fragments_draw_compatible(fragment, last, single_line)) {
			process_text_cluster(view, clip_memory, combiner, start, i, &sizes);
			sizes.num_code_units = 0;
			sizes.num_characters = 0;
			sizes.num_palette_entries = 0;
			start = i;
		}
		sizes.num_code_units += layer->num_code_units;
		sizes.num_characters += layer->num_characters;
		sizes.num_palette_entries += fragment->run_start;
		last = fragment;
	}
	if (start != combiner->count)
		process_text_cluster(view, clip_memory, combiner, start, 
			combiner->count, &sizes);
}

/* Processes a run of text fragment commands, generating one or more draw-text
 * commands. */
static void combine_text_layers(View *view, ClipMemory *clip_memory, 
	TextCombiner *combiner, unsigned start, unsigned end)
{
	combiner_reset(combiner);
	combiner_build_fragments(view, combiner, start, end);
	combiner_identify_clusters(view, combiner);
	combiner_visit_clusters(view, clip_memory, combiner);
}

/* A second command building pass that rewrites the sorted command list, 
 * inserting clipping commands and converting runs of text fragments in each
 * depth interval to final draw-text commands. */
static void view_insert_dependent_commands(View *view)
{
	ClipMemory clip_memory;
	clip_memory.head = 0;
	clip_memory.tail = 0;
	unsigned start = view->header_start;
	unsigned count = view->num_headers;
	view->header_start ^= view->header_capacity;
	view->num_headers = 0;

	TextCombiner combiner;
	combiner_init(&combiner);
	unsigned text_layer_count = 0;
	for (unsigned i = start, end = start + count; i != end; ++i) {
		const DrawCommandHeader *header = view->headers + i;
		if (header->command == DCMD_TEXT_LAYER) {
			text_layer_count += 1;
		} else if (text_layer_count != 0) {
			combine_text_layers(view, &clip_memory, &combiner, 
				i - text_layer_count, i);
			text_layer_count = 0;
		} else {
			if (header->box_index != NO_BOX) {
				const Box *box = view->boxes[header->box_index];
				view_set_clip(view, &clip_memory, box->clip);
			} else {
				view_set_clip(view, &clip_memory, view->bounds);
			}
			view_add_command_header(view, header);
		}
	}
	if (text_layer_count != 0) {
		combine_text_layers(view, &clip_memory, &combiner, 
			start + count - text_layer_count, start + count);
	}
	combiner_deinit(&combiner);
}

/* Rebuilds the view's command list. */
static void view_build_commands(View *view)
{
	view->flags &= ~VFLAG_REBUILD_COMMANDS;
	do {
		view_build_box_commands(view);
		view_sort_commands(view);
		view_insert_dependent_commands(view);
	} while (view_grow_buffers(view));
}

void update_view(View *view)
{
	Document *document = view->document;
	if (view->layout_clock == document->update_clock && 
		(view->flags & VFLAG_REBUILD_COMMANDS) == 0)
		return;
	view_update_box_list(view);
	view_build_commands(view);
	view->layout_clock = document->update_clock;
	view->paint_clock++;
}

void view_handle_mouse_event(View *view, MessageType type, 
	int x, int y, unsigned flags)
{
	float doc_x = rleft(view->bounds) + (float)x;
	float doc_y = rtop(view->bounds) + (float)y;
	document_handle_mouse_event(view->document, view, type, 
		doc_x, doc_y, flags);
}

void view_handle_keyboard_event(View *view, MessageType type, 
	unsigned key_code, unsigned flags)
{
	document_handle_keyboard_event(view->document, view, type, key_code, flags);
}

} // namespace stkr
