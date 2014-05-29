#pragma once

#include <unordered_map>

namespace stkr {

struct Document;
struct Box;

struct GridCell {
	struct Box *boxes;
	unsigned code;
	unsigned num_boxes;
	unsigned query_stamp;
};

typedef std::unordered_map<unsigned, GridCell> GridHash;

void grid_remove(Document *document, Box *box);
void grid_insert(Document *document, Box *box);
unsigned grid_query_rect(Document *document, Box **result, 
	unsigned max_count, float qx0, float qx1, float qy0, float qy1, 
	bool clip = true);
Box *grid_query_anchor(Document *document, float qx, 
	float qx0, float qx1, float qy0, float qy1, float step = 16.0f);
unsigned grid_query_point(Document *document, Box **result, 
	unsigned max_count, float x, float y);

} // namespace stkr
