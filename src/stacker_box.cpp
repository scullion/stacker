#include "stacker_box.h"

#include <cmath>
#include <cstdarg>

#include <algorithm>

#include "stacker_util.h"
#include "stacker_token.h"
#include "stacker_attribute.h"
#include "stacker_document.h"
#include "stacker_system.h"
#include "stacker_node.h"
#include "stacker_paragraph.h"
#include "stacker_layer.h"
#include "stacker_quadtree.h"
#include "stacker_diagnostics.h"
#include "stacker_layout.h"
#include "stacker_inline2.h"
#include "stacker_view.h"

namespace stkr {

Axis box_axis(const Box *box) 
{
	return (Axis)bitfield_read(box->t.flags, 
		BOXFLAG_AXIS_SHIFT, BOXFLAG_AXIS_MASK);
}

Alignment box_arrangement(const Box *box)
{
	return (Alignment)bitfield_read(box->t.flags, 
		BOXFLAG_ARRANGEMENT_SHIFT, BOXFLAG_ARRANGEMENT_MASK);
}

Alignment box_alignment(const Box *box)
{
	return (Alignment)bitfield_read(box->t.flags, 
		BOXFLAG_ALIGNMENT_SHIFT, BOXFLAG_ALIGNMENT_MASK);
}

BoundingBox box_clip_box(const Box *box) 
{
	return (BoundingBox)bitfield_read(box->t.flags, 
		BOXFLAG_CLIP_BOX_SHIFT, BOXFLAG_CLIP_BOX_MASK);
}

/* Sets all the fields packed into a box's flags in one go. */
void box_set_all_flag_fields(Box *box, Axis axis, Alignment arrangement, 
	Alignment alignment, BoundingBox clip_box)
{
	box->t.flags = (box->t.flags & ~BOXFLAG_FIELD_MASK) |
		unsigned(axis)        << BOXFLAG_AXIS_SHIFT        |
		unsigned(arrangement) << BOXFLAG_ARRANGEMENT_SHIFT |
		unsigned(alignment)   << BOXFLAG_ALIGNMENT_SHIFT   |
		unsigned(clip_box)    << BOXFLAG_CLIP_BOX_SHIFT;
}

/* Returns the tree itself if it's a box, or else, if it's a node, returns the
 * node's box. */
Box *tree_box(Tree *tree)
{
	return (tree->flags & TREEFLAG_IS_BOX) != 0 ? 
		(Box *)tree : (Box *)tree->counterpart.box;
}

/* True if two dimensions should be considered equal for the purposes of change
 * detection. */
bool sizes_equal(float a, float b)
{
	return fabsf(a - b) < 0.5f;
}

/* Converts an ASEM_EDGES value into the corresponding mask of BOXFLAG clipping 
 * bits. */
unsigned edge_set_to_box_clip_flags(unsigned edges)
{
	return edges / EDGE_FLAG_LEFT * BOXFLAG_CLIP_LEFT;
}

bool is_mouse_over(const Document *document, const Box *box)
{
	return box->mouse_hit_stamp == document->hit_clock;
}

bool size_valid(const Box *box, SizeSlot slot, Axis axis)
{
	return (box->layout_flags & slotflag(slot, axis)) != 0;
}

void validate_size(Box *box, SizeSlot slot, Axis axis)
{
	box->layout_flags |= slotflag(slot, axis);
}

float get_slot(const Box *box, SizeSlot slot, Axis axis)
{
	return box->axes[axis].sizes[slot];
}

float get_size(const Box *box, SizeSlot slot, Axis axis)
{
	assertb(size_valid(box, slot, axis));
	return get_slot(box, slot, axis);
}

float get_size(const Box *box, Axis axis)
{
	return get_size(box, SSLOT_EXTRINSIC, axis);
}

float get_slot(const Box *box, Axis axis)
{
	return get_slot(box, SSLOT_EXTRINSIC, axis);
}

void set_slot(Box *box, SizeSlot slot, Axis axis, float new_size)
{
	box->axes[axis].sizes[slot] = new_size;
	box->layout_flags |= slotflag(slot, axis);
}

bool set_size(Box *box, SizeSlot slot, Axis axis, float new_size)
{
	bool changed = !sizes_equal(new_size, box->axes[axis].sizes[slot]);
	set_slot(box, slot, axis, new_size);
	return changed;
}

float padded_dim(const Box *box, Axis axis)
{
	return get_size(box, axis) + padding(box, axis);
}

float outer_dim(const Box *box, Axis axis)
{
	return get_size(box, axis) + padding_and_margins(box, axis);
}

float content_edge_lower(const Box *box, Axis axis)
{
	return box->axes[axis].pos + box->axes[axis].margin_lower + box->axes[axis].pad_lower;
}

float content_edge_upper(const Box *box, Axis axis)
{
	return content_edge_lower(box, axis) + get_size(box, axis);
}

float padding_edge_lower(const Box *box, Axis axis)
{
	return box->axes[axis].pos + box->axes[axis].margin_lower;
}

float padding_edge_upper(const Box *box, Axis axis)
{
	return padding_edge_lower(box, axis) + padded_dim(box, axis);
}

float outer_edge_lower(const Box *box, Axis axis)
{
	return box->axes[axis].pos;
}

float outer_edge_upper(const Box *box, Axis axis)
{
	return outer_edge_lower(box, axis) + outer_dim(box, axis);
}

float padding(const Box *box, Axis axis)
{
	return box->axes[axis].pad_lower + box->axes[axis].pad_upper;
}

float margins(const Box *box, Axis axis)
{
	return box->axes[axis].margin_lower + box->axes[axis].margin_upper;
}

float padding_and_margins(const Box *box, Axis axis)
{
	return box->axes[axis].pad_lower + box->axes[axis].pad_upper + 
		box->axes[axis].margin_lower + box->axes[axis].margin_upper;
}

void content_rectangle(const Box *box, float *r)
{
	r[0] = box->axes[AXIS_H].pos + box->axes[AXIS_H].margin_lower + box->axes[AXIS_H].pad_lower;
	r[2] = box->axes[AXIS_V].pos + box->axes[AXIS_V].margin_lower + box->axes[AXIS_V].pad_lower;
	r[1] = r[0] + get_size(box, AXIS_H);
	r[3] = r[2] + get_size(box, AXIS_V);
}

void padding_rectangle(const Box *box, float *r)
{
	r[0] = box->axes[AXIS_H].pos + box->axes[AXIS_H].margin_lower;
	r[2] = box->axes[AXIS_V].pos + box->axes[AXIS_V].margin_lower;
	r[1] = r[0] + padded_dim(box, AXIS_H);
	r[3] = r[2] + padded_dim(box, AXIS_V);
}

void outer_rectangle(const Box *box, float *r)
{
	r[0] = box->axes[AXIS_H].pos;
	r[2] = box->axes[AXIS_V].pos;
	r[1] = r[0] + outer_dim(box, AXIS_H);
	r[3] = r[2] + outer_dim(box, AXIS_V);
}

void content_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	*x0 = box->axes[AXIS_H].pos + box->axes[AXIS_H].margin_lower + box->axes[AXIS_H].pad_lower;
	*y0 = box->axes[AXIS_V].pos + box->axes[AXIS_V].margin_lower + box->axes[AXIS_V].pad_lower;
	*x1 = *x0 + get_size(box, AXIS_H);
	*y1 = *y0 + get_size(box, AXIS_V);
}

void padding_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	*x0 = box->axes[AXIS_H].pos + box->axes[AXIS_H].margin_lower;
	*y0 = box->axes[AXIS_V].pos + box->axes[AXIS_V].margin_lower;
	*x1 = *x0 + padded_dim(box, AXIS_H);
	*y1 = *y0 + padded_dim(box, AXIS_V);
}

void outer_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	*x0 = box->axes[AXIS_H].pos;
	*y0 = box->axes[AXIS_V].pos;
	*x1 = *x0 + outer_dim(box, AXIS_H);
	*y1 = *y0 + outer_dim(box, AXIS_V);
}

void hit_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	if ((box->t.flags & BOXFLAG_HIT_OUTER) != 0)
		outer_rectangle(box, x0, x1, y0, y1);
	else
		padding_rectangle(box, x0, x1, y0, y1);
}

/* Retrieves the content, padding or margin rectangle of a box. */
void bounding_box_rectangle(const Box *box, BoundingBox bbox, float *bounds)
{
	if (bbox == BBOX_CONTENT)
		content_rectangle(box, bounds);
	else if (bbox == BBOX_OUTER)
		outer_rectangle(box, bounds);
	else if (bbox == BBOX_PADDING)
		padding_rectangle(box, bounds);
	else
		ensure(false);
}

/* Identifies views that have been updated more recently than the time of the
 * most recent validation and clears the corresponding bits. */
static unsigned box_validate_visibility(const Document *document, const Box *box)
{
	unsigned flags = box->t.flags;
	if (box->visibility_stamp == document->update_clock)
		return flags;
	for (const View *view = document->views; view != NULL; 
		view = view->next_view) {
		if (stamp_less(box->visibility_stamp, view->visibility_stamp))
			flags &= ~(1 << BOXFLAG_VISIBLE_SHIFT << view->id);
	}
	return flags;
}

/* True if a box is visible in the specified view. */
bool box_is_visible(const Document *, const Box *box, const View *view)
{
	return (box->t.flags & (1 << BOXFLAG_VISIBLE_SHIFT << view->id)) != 0 &&
		!stamp_less(box->visibility_stamp, view->visibility_stamp);
}

/* True if a box is visible in any view. */
bool box_is_visible(const Document *document, const Box *box)
{
	unsigned flags = box_validate_visibility(document, box);
	return (flags & BOXFLAG_VISIBILITY_MASK) != 0;
}

/* Called to tell a box that it is visible in a particular view. */
void box_advise_visible(Document *document, Box *box, const View *view)
{
	/* Stale visibility bits must be cleared before advancing the stamp. If we
	 * didn't do this, it would be possible to miss the fact that a view had
	 * been updated. */
	box->t.flags = box_validate_visibility(document, box); 
	box->t.flags |= 1 << BOXFLAG_VISIBLE_SHIFT << view->id;
	box->visibility_stamp = document->update_clock;

	/* Lazily create the text layer required by visible text boxes. */
	if ((box->t.flags & BOXFLAG_IS_TEXT_BOX) != 0)
		update_box_text_layer(document, box);

	/* Lazily create line box selection layers. */
	if ((box->t.flags & BOXFLAG_IS_LINE_BOX) != 0)
		require_selection_layer(document, box);
}

/* Constructs the non-hierarchical clipping rectangle for a box. */
void build_clip_rectangle(const Box *box, float *r)
{
	if ((box->t.flags & BOXFLAG_CLIP_ALL) != 0) {
		bounding_box_rectangle(box, box_clip_box(box), r);
		for (unsigned i = 0; i < 4; ++i)
			if ((box->t.flags & (BOXFLAG_CLIP_LEFT << i)) == 0)
				r[i] = INFINITE_RECTANGLE[i];
	} else {
		memcpy(r, INFINITE_RECTANGLE, 4 * sizeof(float));
	}
}

unsigned box_tree_depth(const Box *box)
{
	unsigned depth = 0;
	while (box->t.parent.box != NULL) {
		box = box->t.parent.box;
		depth++;
	}
	return depth;
}

/* Comparison operator for selection anchor candidate boxes. */
bool better_anchor(float x, float y, const Box *a, const Box *b)
{
	/* Compare siblings based on the distance from the query point to the
	 * nearest edge of the box along the axis of their shared parent, and
	 * non siblings by their vertical distances, unless they are vertically
	 * level. */
	float ax0, ax1, ay0, ay1;
	float bx0, bx1, by0, by1;
	hit_rectangle(a, &ax0, &ax1, &ay0, &ay1);
	hit_rectangle(b, &bx0, &bx1, &by0, &by1);
	float dxa = band_distance(x, ax0, ax1);
	float dya = band_distance(y, ay0, ay1);
	float dxb = band_distance(x, bx0, bx1);
	float dyb = band_distance(y, by0, by1);
	if (tree_is_child(&a->t, &b->t) && 
		dxb == 0.0f && dyb == 0.0f)
		return true;
	Axis axis = AXIS_H;
	if (a->t.parent.box == b->t.parent.box && a->t.parent.box != NULL)
		axis = box_axis(a->t.parent.box);
	else if (fabsf(dya - dyb) >= 1.0f)
		axis = AXIS_V;
	return (axis == AXIS_H) ? (dxa < dxb) : (dya < dyb);
}

void depth_sort_boxes(const Box **boxes, unsigned count)
{
	struct {
		bool operator () (const Box *a, const Box *b) const 
			{ return tree_before(&a->t, &b->t); }
	} less_depth;
	std::sort(boxes, boxes + count, less_depth);
}

/* Changes a parent box's layout flags in response to a child box being added to
 * or removed from its child list. */
static void box_notify_child_added_or_removed(Document *document, 
	Box *parent, Box *child, bool removed)
{
	/* Depending on the parent's arragement mode, removing a box may shift
	 * its siblings. When a box is added, we currently conservatively assume
	 * that we have to reposition all siblings, because even if the new box
	 * is the first or last, we rely on the parent to set its position. */
	bool reposition_siblings = true;
	if (removed && child != NULL) {
		switch (box_arrangement(parent)) {
			case ALIGN_END:
				reposition_siblings = child->t.prev.tree != NULL;
				break;
			case ALIGN_MIDDLE:
				reposition_siblings = true;
				break;
			case ALIGN_START:
			default:
				reposition_siblings = child->t.next.tree != NULL;
				break;
		}
	}
	unsigned to_clear = BLFLAG_LAYOUT_INFO_VALID;
	if (reposition_siblings)
		to_clear |= BLFLAG_CHILD_BOUNDS_VALID;
	clear_flags(document, parent, to_clear, axismask(AXISFLAG_PREFERRED_VALID | AXISFLAG_INTRINSIC_VALID));
}

/* Recursively removes boxes from the grid. */
static void remove_children_from_grid(Document *document, Box *box)
{
	grid_remove(document, box);
	for (Box *child = box->t.first.box; child != NULL; 
		child = child->t.next.box) {
		remove_children_from_grid(document, child);
	} 
}

/* Updates a child box's layout flags in response to the child's parent having
 * changed. Does not change parent flags. */
static void box_notify_new_parent(Document *document, Box *child, Box *parent)
{
	/* The bounds of the child and its children are invalid. If the child's
	 * size depends on its parent, it's invalid. */
	unsigned to_clear = BLFLAG_LAYOUT_INFO_VALID | BLFLAG_CHILD_BOUNDS_VALID;
	if (size_depends_on_parent(child))
		to_clear |= axismask(AXISFLAG_EXTRINSIC_VALID);
	clear_flags(document, child, to_clear);
	/* Boxes not in the tree should not be in the grid because we don't want
	 * them to be found in queries for mouse selection and view visibility. */
	if (parent == NULL && child != document->root->t.counterpart.box) 
		remove_children_from_grid(document, child);
}

void remove_from_parent(Document *document, Box *box)
{
	Box *parent = box->t.parent.box;
	if (parent != NULL) {
		box_notify_child_added_or_removed(document, parent, box, true);
		tree_remove_from_parent(&parent->t, &box->t);
	}
	box_notify_new_parent(document, box, NULL);
}

void append_child(Document *document, Box *parent, Box *child)
{
	remove_from_parent(document, child);
	tree_insert_child_before(&parent->t, &child->t, NULL);
	box_notify_child_added_or_removed(document, parent, child, false);
	box_notify_new_parent(document, child, parent);
}

void insert_child_before(Document *document, Box *parent, Box *child, 
	Box *before)
{
	remove_from_parent(document, child);
	tree_insert_child_before(&parent->t, &child->t, 
		before != NULL ? &before->t : NULL);
	parent->t.flags |= NFLAG_RECOMPOSE_CHILD_BOXES;
	box_notify_child_added_or_removed(document, parent, child, false);
	box_notify_new_parent(document, child, parent);
}

void remove_all_children(Document *document, Box *parent)
{
	for (Box *child = parent->t.first.box, *next; 
		child != NULL; child = next) {
		next = child->t.next.box;
		child->t.parent.box = NULL;
		child->t.prev.box = NULL;
		child->t.next.box = NULL;
		box_notify_new_parent(document, child, NULL);
	}
	parent->t.first.box = NULL;
	parent->t.last.box = NULL;
	box_notify_child_added_or_removed(document, parent, NULL, true);
}

Box *create_box(Document *document, Node *owner)
{
	document->system->total_boxes++;
	
	Box *box = document->free_boxes;
	if (box != NULL) {
		document->free_boxes = box->t.next.box;
		box->t.next.box = NULL;
		memset(box, 0, sizeof(Box));
	} else {
		box = new Box();
	}

	tree_init(&box->t, TREEFLAG_IS_BOX);
	box->t.counterpart.node = owner;
	box->layers = NULL;
	box->layout_flags = 0;
	box->mouse_hit_stamp = uint32_t(-1);
	box->first_element = unsigned(-1);
	box->last_element = unsigned(-1);
	box->line_number = unsigned(-1);
	box->depth_interval = 0;
	box->depth = 0;
	box->cell_code = INVALID_CELL_CODE;
	box->cell_prev = NULL;
	box->cell_next = NULL;
	return box;
}

const char *get_box_debug_string(const Box *box, const char *value_if_null)
{
	if (box == NULL)
		return value_if_null;
#if defined(STACKER_DIAGNOSTICS)
	return box->debug_info;
#else
	return "box";
#endif
}

void set_box_debug_string(Box *box, const char *fmt, ...)
{
#if defined(STACKER_DIAGNOSTICS)
	va_list args;
	va_start(args, fmt);
	vsnprintf(box->debug_info, sizeof(box->debug_info), fmt, args);
	va_end(args);
	box->debug_info[sizeof(box->debug_info) - 1] = '\0';
#else
	box; fmt;
#endif
}

static void initialize_dimensions(Document *document, Box *box)
{
	box->axes[AXIS_H].mode_min = DMODE_ABSOLUTE;
	box->axes[AXIS_V].mode_min = DMODE_ABSOLUTE;
	box->axes[AXIS_H].mode_max = ADEF_UNDEFINED;
	box->axes[AXIS_V].mode_max = ADEF_UNDEFINED;
	box->axes[AXIS_H].mode_pad_lower = ADEF_UNDEFINED;
	box->axes[AXIS_V].mode_pad_lower = ADEF_UNDEFINED;
	box->axes[AXIS_H].mode_pad_upper = ADEF_UNDEFINED;
	box->axes[AXIS_V].mode_pad_upper = ADEF_UNDEFINED;
	box->axes[AXIS_H].min = 0.0f;
	box->axes[AXIS_V].min = 0.0f;
	box->axes[AXIS_H].max = FLT_MAX;
	box->axes[AXIS_V].max = FLT_MAX;
	box->axes[AXIS_H].pad_lower = 0.0f;
	box->axes[AXIS_V].pad_lower = 0.0f;
	box->axes[AXIS_H].pad_upper = 0.0f;
	box->axes[AXIS_V].pad_upper = 0.0f;
	box->axes[AXIS_H].margin_lower = 0.0f;
	box->axes[AXIS_V].margin_lower = 0.0f;
	box->axes[AXIS_H].margin_upper = 0.0f;
	box->axes[AXIS_V].margin_upper = 0.0f;
	set_ideal_size(document, box, AXIS_H, (DimensionMode)ADEF_UNDEFINED, 0.0f);
	set_ideal_size(document, box, AXIS_V, (DimensionMode)ADEF_UNDEFINED, 0.0f);
}

void destroy_box_internal(Document *document, Box *box)
{
	document->system->total_boxes--;
	document_notify_box_destroy(document, box);
	release_layer_chain(document, VLCHAIN_BOX, box->layers);
	grid_remove(document, box); 
	box->t.next.box = document->free_boxes;
	document->free_boxes = box;
}

void destroy_box_tree(Document *document, Box *box)
{
	for (Box *child = box->t.first.box; child != NULL; ) {
		Box *next = child->t.next.box;
		destroy_box_tree(document, child);
		child = next;
	}
	destroy_box_internal(document, box);
}

void remove_and_destroy_box(Document *document, Box *box)
{
	tree_remove(&box->t);
	destroy_box_internal(document, box);
}

void remove_and_destroy_siblings(Document *document, Box *first)
{
	tree_detach_siblings(&first->t);
	do {
		Box *next = first->t.next.box;
		destroy_box_tree(document, first);
		first = next;
	} while (first != NULL);
}

void destroy_box(Document *document, Box *box, bool destroy_children)
{
	remove_from_parent(document, box);
	if (destroy_children) {
		destroy_box_tree(document, box);
	} else {
		remove_all_children(document, box);
		destroy_box_internal(document, box);
	}
}

/* Synchronizes the properties of a block or inline container box with the 
 * attributes of the node that owns it. */
void configure_container_box(Document *document, Node *node, Axis axis, Box *box)
{
	box->axes[AXIS_H].mode_min          = (unsigned char)read_as_float(node, TOKEN_MIN_WIDTH, &box->axes[AXIS_H].min, 0.0f);
	box->axes[AXIS_V].mode_min          = (unsigned char)read_as_float(node, TOKEN_MIN_HEIGHT, &box->axes[AXIS_V].min, 0.0f);
	box->axes[AXIS_H].mode_max          = (unsigned char)read_as_float(node, TOKEN_MAX_WIDTH, &box->axes[AXIS_H].max, FLT_MAX);
	box->axes[AXIS_V].mode_max          = (unsigned char)read_as_float(node, TOKEN_MAX_HEIGHT, &box->axes[AXIS_V].max, FLT_MAX);
	box->axes[AXIS_H].mode_pad_lower    = (unsigned char)read_as_float(node, TOKEN_PADDING_LEFT, &box->axes[AXIS_H].pad_lower);
	box->axes[AXIS_H].mode_pad_upper    = (unsigned char)read_as_float(node, TOKEN_PADDING_RIGHT, &box->axes[AXIS_H].pad_upper);
	box->axes[AXIS_V].mode_pad_lower    = (unsigned char)read_as_float(node, TOKEN_PADDING_TOP, &box->axes[AXIS_V].pad_lower);
	box->axes[AXIS_V].mode_pad_upper    = (unsigned char)read_as_float(node, TOKEN_PADDING_BOTTOM, &box->axes[AXIS_V].pad_upper);
	box->axes[AXIS_H].mode_margin_lower = (unsigned char)read_as_float(node, TOKEN_MARGIN_LEFT, &box->axes[AXIS_H].margin_lower);
	box->axes[AXIS_H].mode_margin_upper = (unsigned char)read_as_float(node, TOKEN_MARGIN_RIGHT, &box->axes[AXIS_H].margin_upper);
	box->axes[AXIS_V].mode_margin_lower = (unsigned char)read_as_float(node, TOKEN_MARGIN_TOP, &box->axes[AXIS_V].margin_lower);
	box->axes[AXIS_V].mode_margin_upper = (unsigned char)read_as_float(node, TOKEN_MARGIN_BOTTOM, &box->axes[AXIS_V].margin_upper);
	box->axes[GDIR_GROW].mode_growth    = (unsigned char)read_as_float(node, TOKEN_GROW, &box->growth[GDIR_GROW], 0.0f);
	box->axes[GDIR_SHRINK].mode_growth  = (unsigned char)read_as_float(node, TOKEN_SHRINK, &box->growth[GDIR_SHRINK], 0.0f);

	box_set_all_flag_fields(box, axis, 
		(Alignment)read_mode(node, TOKEN_ARRANGE, ALIGN_START),
		(Alignment)read_mode(node, TOKEN_ALIGN, ALIGN_START),
		(BoundingBox)read_mode(node, TOKEN_CLIP_BOX, BBOX_OUTER));

	unsigned clip_edges = read_mode(node, TOKEN_CLIP, EDGE_FLAG_ALL);
	box->t.flags |= edge_set_to_box_clip_flags(clip_edges);

	float ideal_width, ideal_height;
	DimensionMode mode_width  = (DimensionMode)read_as_float(node, TOKEN_WIDTH, &ideal_width, 0.0f);
	DimensionMode mode_height = (DimensionMode)read_as_float(node, TOKEN_HEIGHT, &ideal_height, 0.0f);
	set_ideal_size(document, box, AXIS_H, mode_width, ideal_width);
	set_ideal_size(document, box, AXIS_V, mode_height, ideal_height);

	set_box_dimensions_from_image(document, node, box);
	node->t.flags |= NFLAG_UPDATE_SELECTION_LAYERS | NFLAG_UPDATE_BOX_LAYERS;

	box->layout_flags &= ~BLFLAG_LAYOUT_INFO_VALID;
}

Box *build_line_box(Document *document, Node *node, 
	Justification justification, unsigned line_number)
{
	Box *box = create_box(document, node);
	initialize_dimensions(document, box);
	box->t.flags |= BOXFLAG_IS_LINE_BOX;
	box->line_number = line_number;
	Alignment alignment;
	switch (justification) {
		case JUSTIFY_RIGHT:
			alignment = ALIGN_END;
			break;
		case JUSTIFY_CENTER:
			alignment = ALIGN_MIDDLE;
			break;
		case JUSTIFY_LEFT:
		case JUSTIFY_FLUSH:
		default:
			alignment = ALIGN_START;
			break;
	}
	box_set_all_flag_fields(box, AXIS_H,
		ALIGN_START, alignment, BBOX_OUTER);
	set_box_debug_string(box, "line box %u", line_number);
	return box;
}

/* Builds a box used to position a sequence of tokens in an inline context. */
Box *build_text_box(Document *document, Node *owner, 
	const char *text, unsigned text_length)
{
	Box *box = create_box(document, owner);
	initialize_dimensions(document, box);
	box_set_all_flag_fields(box, AXIS_H, ALIGN_START, ALIGN_MIDDLE, BBOX_OUTER);
	box->t.flags |= BOXFLAG_SELECTION_ANCHOR | BOXFLAG_HIT_OUTER | BOXFLAG_NO_LABEL;
	set_box_debug_string(box, "subword \"%.*s\"", text_length, text);
	return box;
}

void clear_box_tree_flags(Document *document, Box *box, unsigned mask)
{
	box->layout_flags &= ~mask;
	for (Box *child = box->t.first.box; child != NULL; 
		child = child->t.next.box)
		clear_box_tree_flags(document, child, mask);
}

} // namespace stkr

