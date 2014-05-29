#include "stacker_view.h"

#include <algorithm>

#include "stacker.h"
#include "stacker_shared.h"
#include "stacker_attribute.h"
#include "stacker_util.h"
#include "stacker_document.h"
#include "stacker_box.h"
#include "stacker_layer.h"
#include "stacker_system.h"
#include "stacker_platform.h"

namespace stkr {

const uint16_t NOT_CLIPPED = 0xFFFF;

const unsigned CLIP_MEMORY_SIZE = 4;

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
	View *view = new View();
	view->document = document;
	view->flags = flags;
	std::fill(view->bounds, view->bounds + 4, 0.0f);
	view->layout_clock = document->layout_clock - 1;
	view->paint_clock = unsigned(-1);
	view->headers = NULL;
	view->num_headers = 0;
	view->header_capacity = 0;
	view->header_start = 0;
	view->command_data = NULL;
	view->command_data_size = 0;
	view->command_data_capacity = 0;
	return view;
}

void destroy_view(View *view)
{
	if (view == view->document->selection_view)
		clear_selection(view->document);
	delete [] view->headers;
	delete [] view->command_data;
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
	paint_clock |= (unsigned)(view->document->layout_clock != 
		view->layout_clock);
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

/* Updates the list of boxes that need to be drawn for a view. */
static void view_build_box_list(View *view)
{
	unsigned box_capacity = std::max(view->boxes.capacity(), 
		DEFAULT_VIEW_BOX_CAPACITY);
	view->boxes.resize(box_capacity);
	unsigned num_boxes = grid_query_rect(
		view->document, &view->boxes[0], 
		box_capacity, 
		rleft(view->bounds), 
		rright(view->bounds), 
		rtop(view->bounds), 
		rbottom(view->bounds));
	view->boxes.resize(num_boxes);
	if (num_boxes > box_capacity) {
		grid_query_rect(
			view->document, &view->boxes[0], num_boxes, 
			rleft(view->bounds), 
			rright(view->bounds), 
			rtop(view->bounds), 
			rbottom(view->bounds));
	}
	unsigned j = 0;
	for (unsigned i = 0; i < num_boxes; ++i) {
		const Box *box = view->boxes[i];
		if ((box->parent != NULL || box->owner == view->document->root) && 
			(box->layers != NULL || (view->flags & VFLAG_DEBUG_MASK) != 0))
			view->boxes[j++] = view->boxes[i];
	}
	view->boxes.resize(j);
}

/* Appends a command header to the command list. */
static void view_add_command_header(View *view, DrawCommand command, 
	const void *data, unsigned clip_index, int depth)
{
	if (view->num_headers < view->header_capacity) {
		uint32_t data_offset = data != NULL ? 
			(const uint8_t *)data - view->command_data : 0;
		DrawCommandHeader *header = view->headers + view->header_start + 
			view->num_headers;
		header->command = command;
		header->data_offset =  data_offset;
		header->key = (int16_t)depth;
		header->clip_index = (uint16_t)clip_index;
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
	uint32_t data_size, unsigned clip_index, int depth)
{
	unsigned required = view->command_data_size + data_size;
	void *data = view->command_data + view->command_data_size;
	view_add_command_header(view, command, data, clip_index, depth);
	view->command_data_size = required;
	return required <= view->command_data_capacity ? data : NULL;
}

/* Orders view command headers by depth, deepest first. */
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
			DCMD_SET_CLIP, sizeof(ClipCommandData), NOT_CLIPPED, 0);
		if (cd != NULL)
			intersect(view->bounds, r, cd->clip);
		unsigned next = (memory->tail + 1) % CLIP_MEMORY_SIZE;
		if (next == memory->head)
			memory->head = (memory->head + 1) % CLIP_MEMORY_SIZE;
		memory->data[memory->tail] = cd;
		memcpy(memory->rectangles[memory->tail], r, 4 * sizeof(float));
		memory->tail = next;
	} else {
		view_add_command_header(view, DCMD_SET_CLIP, 
			(const ClipCommandData *)memory->data[i], 
			NOT_CLIPPED, 0);
	}
}

/* Adds drawing commands for a pane layer. */
static void view_add_pane_commands(View *view, unsigned clip_index, 
	const VisualLayer *layer, int depth)
{
	const Box *box = view->boxes[clip_index];
	if (layer->pane.pane_type == PANE_FLAT) {
		RectangleCommandData *d = (RectangleCommandData *)view_add_command(
			view, DCMD_RECTANGLE, sizeof(RectangleCommandData), 
			clip_index, depth + layer->depth_offset);
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
static void view_add_image_commands(View *view, unsigned clip_index,
	const VisualLayer *layer, int depth)
{
	/* Are we still waiting for the image? */
	if ((layer->flags & VLFLAG_IMAGE_AVAILABLE) == 0)
		return;

	/* Add an image command. */
	ImageCommandData *d = (ImageCommandData *)view_add_command(view, DCMD_IMAGE,
		sizeof(ImageCommandData), clip_index, depth + layer->depth_offset);
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

	const Box *box = view->boxes[clip_index];
	compute_layer_position(box, &layer->image.position, d->bounds, 
		(float)natural_width, (float)natural_height, has_natural_size);
}

static void view_add_text_commands(View *view, unsigned clip_index, 
	const VisualLayer *layer, int depth)
{
	TextCommandData *d = (TextCommandData *)view_add_command(
		view, DCMD_TEXT, sizeof(TextCommandData), 
		clip_index, depth + layer->depth_offset);
	if (d == NULL)
		return;
	d->text = get_text_layer_text(layer);
	d->flags = get_text_layer_flags(layer);
	d->positions = get_text_layer_positions(layer);
	d->palette = get_text_layer_palette(layer);
	d->length = layer->text.length;
	d->num_colors = layer->text.num_colors;
	d->key = layer->text.key;
	d->font_id = layer->text.font_id;
}

static void view_add_box_layer_commands(View *view, unsigned clip_index)
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
				view_add_text_commands(view, clip_index, layer, depth);
				break;
		}
	}
}

/* Draws a debug label box. */
static void view_draw_box_label(View *view, const char *label, unsigned length,
	const float *bounds, Alignment align_h, Alignment align_v, 
	float pad_h, float pad_v, uint32_t background_color, uint32_t text_color,
	int depth)
{
	if (length == 0)
		return;

	System *system = view->document->system;

	/* Measure the text. */
	int16_t label_font_id = get_debug_label_font_id(system);
	if (label_font_id == INVALID_FONT_ID)
		return;
	void *label_font = get_font_handle(system, label_font_id);
	unsigned text_width, text_height;
	unsigned *label_advances = new unsigned[length];
	platform_measure_text(system->back_end, label_font, label, length,
		&text_width, &text_height, label_advances);

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
		DCMD_RECTANGLE, sizeof(RectangleCommandData), NOT_CLIPPED, depth++);
	if (d != NULL) {
		std::copy(bg_rect, bg_rect + 4, d->bounds);
		d->border_color = 0;
		d->border_width = 0;
		d->fill_color = background_color;
	}

	/* Allocate a text command with extra space to store the text, flags, 
	 * positions and palette. */
	unsigned required = sizeof(TextCommandData);
	required += length * TEXT_LAYER_BYTES_PER_CHAR; /* Text, flags, positions. */
	required += 1 * sizeof(uint32_t); /* Palette. */
	TextCommandData *td = (TextCommandData *)view_add_command(view, DCMD_TEXT,
		required, NOT_CLIPPED, depth++);
	if (td == NULL)
		return;

	char *block = (char *)(td + 1);
	td->text = block;
	block += length;
	td->flags = (uint16_t *)block;
	block += length * sizeof(uint16_t);
	td->positions = (int *)block;
	block += length * 2 * sizeof(int);
	td->palette = (uint32_t *)block;
	block += 1 * sizeof(uint32_t);

	td->font_id = label_font_id;
	td->key = label_font_id;
	td->length = length;
	td->num_colors = 1;

	/* Copy in the data. */
	memcpy((char *)td->text, label, length * sizeof(char));
	((uint32_t *)td->palette)[0] = text_color;
	unsigned x = round_signed(rleft(text_rect));
	unsigned y = round_signed(rtop(text_rect));
	for (unsigned i = 0; i < length; ++i) {
		((int *)td->positions)[2 * i + 0] = x;
		((int *)td->positions)[2 * i + 1] = y;
		x += label_advances[i];
		((uint16_t *)td->flags)[i] = 0;
	}
	((uint16_t *)td->flags)[0] = TLF_LINE_HEAD | TLF_SEGMENT_HEAD | 
		TLF_STYLE_HEAD;
	delete [] label_advances;
}

static void view_add_debug_rectangle_commands(View *view, const Box *box, 
	const float *r, uint32_t bg_color, uint32_t text_color,	
	int depth, bool draw_label)
{
	RectangleCommandData *d = NULL;

	/* Outline the rectangle. */
	d = (RectangleCommandData *)view_add_command(view, DCMD_RECTANGLE, 
		sizeof(RectangleCommandData), NOT_CLIPPED, depth++);
	if (d != NULL) {
		memcpy(d->bounds, r, sizeof(d->bounds));
		d->border_color = bg_color;
		d->border_width = 1;
		d->fill_color = 0;
	}

	/* Add a label. */
	if (draw_label) {
		static const unsigned MAX_LABEL_LENGTH = 256;

		/* Build the label text. */
		char label[MAX_LABEL_LENGTH];
		int length = snprintf(label, sizeof(label), "%s: %.0fx%.0f", 
			get_box_debug_string(box), rwidth(r), rheight(r));
		if (length < 0)
			length = sizeof(label) - 1;
		label[length] = '\0';

		/* Add commands to draw the label. */
		view_draw_box_label(view, label, length, r, ALIGN_END, ALIGN_START, 
			2.0f, 1.0f, bg_color, text_color, 100 + depth);
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
	bool draw_labels = mouse_over && (box->flags & BOXFLAG_NO_LABEL) == 0;
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
			sizeof(RectangleCommandData), NOT_CLIPPED, depth);
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
		unsigned num_boxes = view->boxes.size();
		for (unsigned i = 0; i < num_boxes; ++i) {
			view_add_box_layer_commands(view, i);
			view_add_box_debug_commands(view, view->boxes[i]);
		}
	} while (view_grow_buffers(view));
}

static void view_insert_clipping_commands(View *view)
{
	ClipMemory clip_memory;
	clip_memory.head = 0;
	clip_memory.tail = 0;
	unsigned start = view->header_start;
	unsigned count = view->num_headers;
	view->header_start ^= view->header_capacity;
	view->num_headers = 0;
	for (unsigned i = 0; i < count; ++i) {
		const DrawCommandHeader *header = view->headers + start + i;
		if (header->clip_index != NOT_CLIPPED) {
			const Box *box = view->boxes[header->clip_index];
			view_set_clip(view, &clip_memory, box->clip);
		} else {
			view_set_clip(view, &clip_memory, view->bounds);
		}
		view_add_command_header(view, header);
	}
}

/* Rebuilds the view's command list. */
static void view_build_commands(View *view)
{
	view->flags &= ~VFLAG_REBUILD_COMMANDS;
	do {
		view_build_box_commands(view);
		view_sort_commands(view);
		view_insert_clipping_commands(view);
	} while (view_grow_buffers(view));
}

void update_view(View *view)
{
	Document *document = view->document;
	if (view->layout_clock == document->layout_clock && 
		(view->flags & VFLAG_REBUILD_COMMANDS) == 0)
		return;
	view_build_box_list(view);
	view_build_commands(view);
	view->layout_clock = document->layout_clock;
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
