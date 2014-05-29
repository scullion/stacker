#pragma once

#include <vector>

#include "stacker_style.h"

namespace stkr {

struct Document;
struct Box;

enum DrawCommand {
	DCMD_SET_CLIP,
	DCMD_RECTANGLE,
	DCMD_TEXT,
	DCMD_IMAGE,
	DCMD_EXTERNAL,
	DCMD_END
};

const unsigned DEFAULT_VIEW_BOX_CAPACITY = 256;

struct DrawCommandHeader {
	uint32_t command     : 8;
	uint32_t data_offset : 24;
	int16_t key;
	uint16_t clip_index;
};

struct ClipCommandData {
	float clip[4];
};

struct RectangleCommandData {
	float bounds[4];
	uint32_t border_color;
	uint32_t fill_color;
	float border_width;
};

struct TextCommandData {
	uint32_t key;
	int16_t font_id;
	unsigned length;
	unsigned num_colors;
	const char *text;
	const uint16_t *flags;
	const int *positions;
	const uint32_t *palette;
};

struct ImageCommandData {
	float bounds[4];
	void *system_image;
	uint32_t tint;
};

struct View {
	struct Document *document;
	unsigned flags;
	float bounds[4];
	std::vector<Box *> boxes;
	DrawCommandHeader *headers;
	unsigned num_headers;
	unsigned header_capacity;
	unsigned header_start;
	uint8_t *command_data;
	unsigned command_data_size;
	unsigned command_data_capacity;
	unsigned layout_clock;
	unsigned paint_clock;
};

struct ViewCommandIterator {
	const View *view;
	unsigned position;
};

DrawCommand view_next_command(ViewCommandIterator *iter, const void **data);
DrawCommand view_first_command(const View *view, ViewCommandIterator *iter,
	const void **data);

} // namespace stkr
