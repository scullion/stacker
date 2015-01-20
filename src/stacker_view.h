#pragma once

#include "stacker_style.h"

namespace stkr {

struct Document;
struct Box;

/* Says that you ought to draw something. */
enum DrawCommand {
	DCMD_SET_CLIP,   // Clip to given rectangle until the next SET_CLIP command.
	DCMD_RECTANGLE,  // Fill and outline a rectangle. Used only for debugging.
	DCMD_TEXT,       // Draw a text string.
	DCMD_IMAGE,      // Draw an image.
	DCMD_EXTERNAL,   // Unused.
	DCMD_END         // Marks the end of a command buffer.
};

/* A command buffer entry. */
struct DrawCommandHeader {
	uint16_t  command      :  3;
	int16_t   key          : 13;
	uint16_t  box_index;
	uintptr_t data_offset;  
};

/* Data for DCMD_CLIP. */
struct ClipCommandData {
	float clip[4];
};

/* Data for DCMD_RECTANGLE. */
struct RectangleCommandData {
	float bounds[4];
	uint32_t border_color;
	uint32_t fill_color;
	float border_width;
};

/* Data for DCMD_TEXT. */
struct TextCommandData {
	int16_t font_id;
	unsigned length;
	unsigned num_colors;
	union {
		const char     *bytes;
		const char     *utf8;
		const uint16_t *utf16;
		const uint32_t *utf32;
	} text;
	const int *x_positions;
	union {
		const int *y_positions; /* If multi-line commands are enabled. */
		int line_y_position; /* If single line commands are enabled. */
	};
	const uint32_t *colors;
	const uint32_t *color_code_unit_counts;
	const uint32_t *color_character_counts;
};

/* Data for DCMD_IMAGE. */
struct ImageCommandData {
	float bounds[4];
	void *system_image;
	uint32_t tint;
};

/* Information required to display a rectangular region of a document. */
struct View {
	Document *document;        // Associated document. May be NULL.
	View *next_view;           // Document active view chain.
	unsigned id;               // ID within attached document.
	unsigned flags;	           // Control flags.
	float bounds[4];           // Document space rectangle.
	uint32_t visibility_stamp; // Monotonic counter used to track box visibility.
	unsigned layout_clock;     // Copy of document's layout clock at last update.
	unsigned paint_clock;      // Changes when the view needs to be redrawn.
	
	/* List of boxes used to build this view. */
	Box **boxes;
	unsigned box_capacity;
	unsigned num_boxes;

	/* Command header buffer. */
	DrawCommandHeader *headers;
	unsigned num_headers;
	unsigned header_capacity;
	unsigned header_start;
	
	/* Command data heap. */
	uint8_t *command_data;
	unsigned command_data_size;
	unsigned command_data_capacity;
};

/* Iterates over draw commands and their associated data. */
struct ViewCommandIterator {
	const View *view;
	unsigned position;
};

DrawCommand view_next_command(ViewCommandIterator *iter, const void **data);
DrawCommand view_first_command(const View *view, ViewCommandIterator *iter, 
	const void **data);

} // namespace stkr
