#pragma once

#include <unordered_map>

namespace stkr {

struct Document;
struct Box;

const unsigned INVALID_CELL_CODE = 0;

struct GridCell {
	unsigned code;
	struct Box *boxes;
	unsigned num_boxes;
	unsigned query_stamp;
};

struct Grid {
	GridCell *cells;
	unsigned num_cells;
	unsigned capacity;
};

void grid_init(Grid *grid);
void grid_deinit(Grid *grid);
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
