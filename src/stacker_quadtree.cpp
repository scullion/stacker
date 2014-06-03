#include "stacker_quadtree.h"

#include <algorithm>

#include "stacker_shared.h"
#include "stacker_util.h"
#include "stacker_box.h"
#include "stacker_node.h"
#include "stacker_document.h"

namespace stkr {

const unsigned GRID_DEPTH = 4;
const unsigned GRID_LOG_PITCH[GRID_DEPTH] = { 15, 11, 8, 6 };
const unsigned GRID_COORD_MASK    = 0x3FFF;
const unsigned GRID_LEVEL_MASK    = 7;
const unsigned GRID_COORD_SHIFT   = 14;
const unsigned GRID_LEVEL_SHIFT   = 28;
const unsigned GRID_CODE_BIT      = 1u << 31;
const unsigned GRID_CODE_EMPTY    = 0;
const unsigned GRID_CODE_SENTINEL = 1;

static int grid_i(unsigned code)
{
	return int(code & GRID_COORD_MASK);
}

static int grid_j(unsigned code)
{
	return int((code >> GRID_COORD_SHIFT) & GRID_COORD_MASK);
}

static unsigned grid_level_from_code(unsigned code)
{
	return (code >> GRID_LEVEL_SHIFT) & GRID_LEVEL_MASK;
}

static unsigned grid_log_pitch_from_code(unsigned code)
{
	return GRID_LOG_PITCH[grid_level_from_code(code)];
}

static unsigned grid_cell_code(int x, int y, unsigned level)
{
	unsigned shift = GRID_LOG_PITCH[level];
	unsigned ci = (x >> shift) & GRID_COORD_MASK;
	unsigned cj = (y >> shift) & GRID_COORD_MASK;
	unsigned code = (cj << GRID_COORD_SHIFT) + ci;
	code |= level << GRID_LEVEL_SHIFT;
	code |= GRID_CODE_BIT;
	assertb(grid_log_pitch_from_code(code) == GRID_LOG_PITCH[level]);
	assertb(grid_level_from_code(code) == level);
	return code;
}

static unsigned grid_level(unsigned diameter)
{
	unsigned level;
	for (level = GRID_DEPTH - 1; level != 0; --level)
		if (diameter <= (1u << GRID_LOG_PITCH[level]))
			break;
	return level;
}

static unsigned box_cell_code(const Box *box)
{
	float x0, x1, y0, y1;
	outer_rectangle(box, &x0, &x1, &y0, &y1);
	unsigned diameter = unsigned(std::max(x1 - x0, y1 - y0));
	int cx = int(0.5f * (x0 + x1));
	int cy = int(0.5f * (y0 + y1));
	unsigned level = grid_level(diameter);
	return grid_cell_code(cx, cy, level); 
}


void grid_init(Grid *grid)
{
	grid->cells = NULL;
	grid->num_cells = 0;
	grid->capacity = 0;
}

void grid_deinit(Grid *grid)
{
	delete [] grid->cells;
	grid->num_cells = 0;
	grid->capacity = 0;
}

static void grid_set_capacity(Grid *grid, unsigned new_capacity);

inline unsigned hash_cell_code(unsigned cell_code)
{
	unsigned key = cell_code * 0xcc9e2d51;
	key = (key << 15) | (key >> 17);
	return key * 5 + 0xe6546b64;
}

static GridCell *grid_find_cell(Grid *grid, unsigned cell_code)
{
	if (grid->num_cells == 0)
		return NULL;
	unsigned mask = grid->capacity - 1;
	unsigned index = hash_cell_code(cell_code) & mask;
	for (unsigned probe = 0; ; ++probe) {
		GridCell *cell = grid->cells + index;
		if (cell->code == cell_code)
			return cell;
		if (cell->code == GRID_CODE_EMPTY)
			break;
		unsigned distance = (index - hash_cell_code(cell->code)) & mask;
		if (probe > distance)
			break;
		index = (index + 1) & mask;
	}
	return NULL;
}

inline unsigned grid_new_capacity(unsigned capacity)
{
	return capacity < 64 ? 64 : next_power_of_two(capacity + 1);
}

static GridCell *grid_insert_cell(Grid *grid, unsigned cell_code, 
	Box *boxes = NULL, unsigned num_boxes = 0, unsigned query_stamp = 0)
{
	if (grid->num_cells * 3 / 2 >= grid->capacity)
		grid_set_capacity(grid, grid_new_capacity(grid->capacity));
	unsigned mask = grid->capacity - 1;
	unsigned index = hash_cell_code(cell_code) & mask;
	GridCell *inserted_cell = NULL;
	for (unsigned probe = 0; ; ++probe) {
		GridCell *cell = grid->cells + index;
		if ((cell->code & GRID_CODE_BIT) == 0) {
			grid->num_cells += (cell->code == GRID_CODE_EMPTY);
			cell->code = cell_code;
			cell->boxes = boxes;
			cell->num_boxes = num_boxes;
			cell->query_stamp = query_stamp;
			if (inserted_cell == NULL)
				inserted_cell = cell;
			break;
		} else if (cell->code == cell_code) {
			inserted_cell = cell;
			break;
		}
		unsigned distance = (index - hash_cell_code(cell->code)) & mask;
		if (probe > distance) {
			std::swap(cell_code, cell->code);
			std::swap(boxes, cell->boxes);
			std::swap(num_boxes, cell->num_boxes);
			std::swap(query_stamp, cell->query_stamp);
			if (inserted_cell == NULL)
				inserted_cell = cell;
			probe = distance;
		}
		index = (index + 1) & mask;
	}
	return inserted_cell;
}

static void grid_set_capacity(Grid *grid, unsigned new_capacity)
{
	unsigned old_capacity = grid->capacity;
	GridCell *old_cells = grid->cells;
	grid->cells = new GridCell[new_capacity];
	memset(grid->cells, 0, new_capacity * sizeof(GridCell));
	grid->capacity = new_capacity;
	grid->num_cells = 0;
 	for (unsigned i = 0; i < old_capacity; ++i) {
		const GridCell *old_cell = old_cells + i;
		if ((old_cell->code & GRID_CODE_BIT) != 0) {
			grid_insert_cell(grid, old_cell->code, old_cell->boxes, 
				old_cell->num_boxes, old_cell->query_stamp);
		}
	}
	delete [] old_cells;
}

void grid_remove(Document *document, Box *box)
{
	if (box->cell_code == INVALID_CELL_CODE)
		return;
	GridCell *cell = grid_find_cell(&document->grid, box->cell_code);
	assertb(cell != NULL && cell->num_boxes != 0);
	if (box->cell_prev != NULL) {
		box->cell_prev->cell_next = box->cell_next;
	} else {
		cell->boxes = box->cell_next;
	}
	if (box->cell_next != NULL) {
		box->cell_next->cell_prev = box->cell_prev;
	}
	box->cell_prev = NULL;
	box->cell_next = NULL;
	box->cell_code = INVALID_CELL_CODE;
	assertb(cell->num_boxes != 0);
	cell->num_boxes--;
}

void grid_insert(Document *document, Box *box)
{
	unsigned cell_code = box_cell_code(box); 
	GridCell *cell = grid_insert_cell(&document->grid, cell_code);
	if (cell_code != box->cell_code) {
		grid_remove(document, box);
		if (cell->boxes != NULL)
			cell->boxes->cell_prev = box;
		box->cell_next = cell->boxes;
		box->cell_code = cell_code;
		cell->boxes = box;
		cell->num_boxes++;
	}
}

/* Finds all boxes partially intersecting a query rectangle. */
unsigned grid_query_rect(Document *document, Box **result, 
	unsigned max_count, float qx0, float qx1, float qy0, float qy1, 
	bool clip)
{
	if (qx1 < qx0) std::swap(qx0, qx1);
	if (qy1 < qy0) std::swap(qy0, qy1);
	int x0i = round_signed(qx0);
	int x1i = round_signed(qx1);
	int y0i = round_signed(qy0);
	int y1i = round_signed(qy1);
	unsigned count = 0;
	for (unsigned level = 0; level < GRID_DEPTH; ++level) {
		unsigned shift = GRID_LOG_PITCH[level];
		int pitch = 1 << shift;
		int half_pitch = pitch / 2;
		int first_i = (x0i - half_pitch) >> shift;
		int first_j = (y0i - half_pitch) >> shift;
		int last_i = (x1i + half_pitch) >> shift;
		int last_j = (y1i + half_pitch) >> shift;
		for (int i = first_i; i <= last_i; ++i) {
			for (int j = first_j; j <= last_j; ++j) {
				unsigned cell_code = grid_cell_code(i * pitch, j * pitch, level);
				GridCell *cell = grid_find_cell(&document->grid, cell_code);
				if (cell == NULL || cell->query_stamp == document->box_query_stamp)
					continue;
				cell->query_stamp = document->box_query_stamp;
				for (Box *box = cell->boxes; box != NULL; box = box->cell_next) {
					float bx0, bx1, by0, by1;
					hit_rectangle(box, &bx0, &bx1, &by0, &by1);
					if (!clip || rectangles_overlap(qx0, qx1, qy0, qy1, 
						bx0, bx1, by0, by1)) {
						if (count < max_count)
							result[count] = box;
						count++;
					}
				}
			}
		}
	}
	document->box_query_stamp++;
	return count;
}

/* Finds a single box to serve as the start or end of a mouse selection. */
Box *grid_query_anchor(Document *document, float qx, 
	float qx0, float qx1, float qy0, float qy1, float step)
{
	static const unsigned MAX_BOXES = 1024;

	/* Clip the (qy0, qy1) interval against the document to form the query 
	 * band. */
	float doc_x0, doc_x1, doc_y0, doc_y1;
	if (document->root->box == NULL)
		return NULL;
	outer_rectangle(document->root->box, &doc_x0, &doc_x1, &doc_y0, &doc_y1);
	qy0 = clip(qy0, doc_y0, doc_y1);
	qy1 = clip(qy1, doc_y0, doc_y1);
	if (qy1 < qy0)
		step = -step;

	/* Step through the interval (qx0, qx1) in vertical slices until an 
	 * acceptable anchor is found. */
	float band_y0 = qy0, band_y1;
	Box *boxes[MAX_BOXES], *anchor = NULL;
	do {
		/* Find all boxes in a small slice of the band, querying in no-clip
		 * mode so that the full contents of each visited cell are returned.
		 * This obviates the need to revisit cells. */
		band_y1 = band_y0 + step;
		unsigned num_boxes = grid_query_rect(document, boxes, MAX_BOXES, 
			qx0, qx1, band_y0, band_y1, false);
		// dmsg("Box query count: %u.\n", num_boxes);
		document->box_query_stamp--; /* Don't visit cells twice. */
		
		/* Find the closest selection anchor to (qx, qy0) in the query result. */
		if (num_boxes > MAX_BOXES)
			num_boxes = MAX_BOXES;
		for (unsigned i = 0; i < num_boxes; ++i) {
			Box *box = boxes[i];
			if ((box->flags & BOXFLAG_SELECTION_ANCHOR) != 0 && 
				(anchor == NULL || better_anchor(qx, qy0, box, anchor)))
				anchor = box;
		}

		/* Move up or down to the next slice. */
		band_y0 += step;
	} while (anchor == NULL && (qy1 - band_y0) * step >= 0.0f);
	document->box_query_stamp++;
	return anchor;
}

unsigned grid_query_point(Document *document, Box **result, 
	unsigned max_count, float x, float y)
{
	return grid_query_rect(document, result, max_count, x, x, y, y);
}

/* FIXME (TJM): avoid narrowphase test for cells fully inside the query rect. */

static unsigned query_rect_linear(Document *document, Box **result, 
	unsigned max_count, float qx0, float qx1, float qy0, float qy1)
{
	unsigned count = 0;
	Box *box = document->root->box;
	while (box != NULL) {
		float bx0, bx1, by0, by1;
		hit_rectangle(box, &bx0, &bx1, &by0, &by1);
		if (rectangles_overlap(qx0, qx1, qy0, qy1, bx0, bx1, by0, by1)) {
			if (count < max_count)
				result[count] = box;
			count++;
		}
		if (box->first_child != NULL) {
			box = box->first_child;
		} else {
			while (box->next_sibling == NULL) {
				box = box->parent;
				if (box == NULL)
					break;
			}
			if (box != NULL)
				box = box->next_sibling;
		}
	}
	return count;
}

static void debug_analyze_box_query_result(const Document *document, 
	Box **boxes, unsigned count, const char *name)
{
	std::sort(boxes, boxes + count);
	unsigned dupcount = 0;
	const Box *last = NULL;
	for (unsigned i = 0; i < count; ++i) {
		const Box *box = boxes[i];
		if (box == NULL) {
			dmsg("Query [%s] contains NULL box at position %u.\n", name, i);
			continue;
		}
		if (box == last) {
			dupcount++;
			if (i + 1 == count || box != boxes[i + 1]) {
				dmsg("Query [%s] duplicates box \"%s\" %u times.\n", 
					name, get_box_debug_string(box), dupcount);
				dupcount = 0;
			}
		}
		last = box;
	}
}

void unit_test_box_grid(Document *document)
{
	static const unsigned NUM_TRIALS = 100;
	static const unsigned MAX_BOXES = 1000;
	static const float QUERY_RANGE = 300.0f;
	static const float QUERY_MAX_DIM = 500.0f;

	srand(0);

	/* Query with random rectangles. */
	Box *result_linear[MAX_BOXES];
	Box *result_quadtree[MAX_BOXES];
	for (unsigned trial = 0; trial < NUM_TRIALS; ++trial) {
		float qx0 = QUERY_RANGE * float(rand()) / float(RAND_MAX);
		float qy0 = QUERY_RANGE * float(rand()) / float(RAND_MAX);
		float qx1 = qx0 + QUERY_MAX_DIM * float(rand()) / float(RAND_MAX);
		float qy1 = qy0 + QUERY_MAX_DIM * float(rand()) / float(RAND_MAX);

		unsigned count_linear = query_rect_linear(document, result_linear, MAX_BOXES, qx0, qx1, qy0, qy1);
		unsigned count_quadtree = grid_query_rect(document, result_quadtree, MAX_BOXES, qx0, qx1, qy0, qy1);

		dmsg("Query %.3u: stamp=%u, query_rect=(%.2f, %.2f, %.2f, %.2f) "
			"count_linear=%u count_quadtree=%u.\n", trial, document->box_query_stamp,
			qx0, qx1, qy0, qy1, count_linear, count_quadtree);

		debug_analyze_box_query_result(document, result_linear, count_linear, "LINEAR");
		debug_analyze_box_query_result(document, result_quadtree, count_quadtree, "QUADTREE");

		Box *missing[MAX_BOXES], **missing_end = std::set_difference(
			result_linear, result_linear + count_linear,
			result_quadtree, result_quadtree + count_quadtree,
			missing);
		if (missing_end != missing) {
			unsigned num_missing = missing_end - missing;
			dmsg("QUADTREE result missing %u boxes:\n", num_missing);
			for (unsigned i = 0; i < num_missing; ++i) {
				float bx0, bx1, by0, by1;
				const Box *missing_box = missing[i];
				outer_rectangle(missing_box, &bx0, &bx1, &by0, &by1);
				dmsg("\tBox \"%s\" bounds=(%.2f, %.2f, %.2f, %.2f)",
					get_box_debug_string(missing_box), bx0, bx1, by0, by1);
				if (missing_box->cell_code != INVALID_CELL_CODE) {
					const GridCell *cell = grid_find_cell(&document->grid, 
						missing_box->cell_code);
					dmsg(" in cell [code=%xh, stamp=%u]\n", 
						cell->code, cell->query_stamp);
				} else {
					dmsg(", which is not in the grid.\n");
				}
			}
		}

		//ensure(count_linear == count_quadtree);
		//ensure(std::equal(result_quadtree, result_quadtree + count_quadtree, result_linear))
	}
}

void dump_grid(Document *document)
{
	const Grid *grid = &document->grid;

	struct LevelStatistics {
		unsigned cell_count;
		unsigned box_count;
		unsigned max_box_count;
		unsigned mean_box_count;
		float mean_diameter;
	} stats[GRID_DEPTH];
	memset(stats, 0, sizeof(stats));
	unsigned num_cells = grid->num_cells;
	for (unsigned i = 0; i < grid->capacity; ++i) {
		const GridCell *cell = grid->cells + i;
		if ((cell->code & GRID_CODE_BIT) == 0)
			continue;
		unsigned level = grid_level_from_code(cell->code);
		LevelStatistics *s = stats + level;
		s->box_count += cell->num_boxes;
		s->cell_count++;
		s->max_box_count = std::max(s->max_box_count, cell->num_boxes);
		for (const Box *box = cell->boxes; box != NULL; box = box->cell_next) {
			float x0, x1, y0, y1;
			outer_rectangle(box, &x0, &x1, &y0, &y1);
			s->mean_diameter += std::max(x1 - x0, y1 - y0);
		}
	}
	unsigned total_boxes = 0;
	for (unsigned i = 0; i < GRID_DEPTH; ++i) {
		stats[i].mean_box_count = stats[i].cell_count != 0 ? 
			stats[i].box_count / stats[i].cell_count : 0;
		if (stats[i].box_count != 0)
			stats[i].mean_diameter /= float(stats[i].box_count);
		total_boxes += stats[i].box_count;
	}
	dmsg("Grid cells: %u, %u levels:, total_boxes: %u\n", 
		num_cells, GRID_DEPTH, total_boxes);
	for (unsigned level = 0; level < GRID_DEPTH; ++level) {
		dmsg("L%u [pitch: %5u]: cells:%5u, boxes:%5u, "
			"max_occupancy:%3u, mean_occupancy:%3u, mean_diameter:%7.2f\n", 
			level, 1u << GRID_LOG_PITCH[level],
			stats[level].cell_count,
			stats[level].box_count,
			stats[level].max_box_count,
			stats[level].mean_box_count,
			stats[level].mean_diameter);
	}
	dmsg("\n");
	for (unsigned i = 0; i < grid->capacity; ++i) {
		const GridCell *cell = grid->cells + i;
		if ((cell->code & GRID_CODE_BIT) == 0)
			continue;
		dmsg("Cell log_pitch=%u, level=%u, pos=(%d,%d) code=%0.8x, num_boxes=%u\n", 
			grid_log_pitch_from_code(cell->code),
			grid_level_from_code(cell->code), 
			grid_i(cell->code), grid_j(cell->code),
			cell->code, cell->num_boxes);
		for (const Box *box = cell->boxes; box != NULL; box = box->cell_next) {
			float x0, x1, y0, y1;
			outer_rectangle(box, &x0, &x1, &y0, &y1);
			dmsg("\t[%s] bounds=(%.2f, %.2f, %.2f, %.2f).\n",
				get_box_debug_string(box), x0, x1, y0, y1);
		}
	}
}

} // namespace stkr
