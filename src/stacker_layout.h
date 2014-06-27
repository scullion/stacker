#pragma once

namespace stkr {

struct Document;
struct Box;
enum Axis;
enum DimensionMode;

void layout(Document *document, Box *root);
void clear_flags(Document *document, Box *box, unsigned to_clear,
	unsigned cleared_in_children = 0);
void clear_flags(Document *document, Box *box, Axis axis, 
	unsigned to_clear, unsigned cleared_in_children = 0);
bool set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim = 0.0f);
void update_layout_info(Document *document, Box *box);
bool size_depends_on_parent(const Box *box, Axis axis);
bool size_depends_on_parent(const Box *box);

};
