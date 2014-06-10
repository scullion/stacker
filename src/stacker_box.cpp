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

namespace stkr {

static void update_layout_info(Document *document, Box *box);
static void initialize_provisional_size(Document *document, Box *box, 
	Axis axis, float dim);
static bool set_provisional_size(Document *document, SizingPass pass, Box *box, 
	Axis axis, float dim, bool from_parent);

/* Converts an ASEM_EDGES value into the corresponding mask of BOXFLAG clipping 
 * bits. */
unsigned edge_set_to_box_clip_flags(unsigned edges)
{
	return edges / EDGE_FLAG_LEFT * BOXFLAG_CLIP_LEFT;
}

bool is_mouse_over(const Document *document, const Box *box)
{
	return box->mouse_hit_stamp == get_hit_clock(document);
}

void clear_flag_in_parents(Document *document, Box *box, unsigned mask)
{
	document;
	while (box->parent != NULL) {
		box = box->parent;
		box->flags &= ~mask;
	}
}

/* Returns the current size of a box on the specified axis. */
float get_size(const Box *box, Axis axis)
{
	unsigned primary = (box->flags >> axis) / BOXFLAG_WIDTH_PRIMARY_ABOVE;
	return box->sizes[primary & 1][axis];
}

/* Returns a box's active provisional size on the specified axis. */
static float get_active_size(const Box *box, Axis axis)
{
	unsigned active = (box->flags >> axis) / BOXFLAG_WIDTH_ACTIVE_ABOVE;
	return box->sizes[active & 1][axis];
}

/* Returns most recent size set from above or below. */
float get_size_directional(const Box *box, Axis axis, bool from_parent)
{
	return box->sizes[from_parent][axis];
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
	return box->pos[axis] + box->margin_lower[axis] + box->pad_lower[axis];
}

float content_edge_upper(const Box *box, Axis axis)
{
	return content_edge_lower(box, axis) + get_size(box, axis);
}

float padding_edge_lower(const Box *box, Axis axis)
{
	return box->pos[axis] + box->margin_lower[axis];
}

float padding_edge_upper(const Box *box, Axis axis)
{
	return padding_edge_lower(box, axis) + padded_dim(box, axis);
}

float outer_edge_lower(const Box *box, Axis axis)
{
	return box->pos[axis];
}

float outer_edge_upper(const Box *box, Axis axis)
{
	return outer_edge_lower(box, axis) + outer_dim(box, axis);
}

float padding(const Box *box, Axis axis)
{
	return box->pad_lower[axis] + box->pad_upper[axis];
}

float margins(const Box *box, Axis axis)
{
	return box->margin_lower[axis] + box->margin_upper[axis];
}

float padding_and_margins(const Box *box, Axis axis)
{
	return box->pad_lower[axis] + box->pad_upper[axis] + 
		box->margin_lower[axis] + box->margin_upper[axis];
}

void content_rectangle(const Box *box, float *r)
{
	r[0] = box->pos[AXIS_H] + box->margin_lower[AXIS_H] + box->pad_lower[AXIS_H];
	r[2] = box->pos[AXIS_V] + box->margin_lower[AXIS_V] + box->pad_lower[AXIS_V];
	r[1] = r[0] + get_size(box, AXIS_H);
	r[3] = r[2] + get_size(box, AXIS_V);
}

void padding_rectangle(const Box *box, float *r)
{
	r[0] = box->pos[AXIS_H] + box->margin_lower[AXIS_H];
	r[2] = box->pos[AXIS_V] + box->margin_lower[AXIS_V];
	r[1] = r[0] + padded_dim(box, AXIS_H);
	r[3] = r[2] + padded_dim(box, AXIS_V);
}

void outer_rectangle(const Box *box, float *r)
{
	r[0] = box->pos[AXIS_H];
	r[2] = box->pos[AXIS_V];
	r[1] = r[0] + outer_dim(box, AXIS_H);
	r[3] = r[2] + outer_dim(box, AXIS_V);
}

void content_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	*x0 = box->pos[AXIS_H] + box->margin_lower[AXIS_H] + box->pad_lower[AXIS_H];
	*y0 = box->pos[AXIS_V] + box->margin_lower[AXIS_V] + box->pad_lower[AXIS_V];
	*x1 = *x0 + get_size(box, AXIS_H);
	*y1 = *y0 + get_size(box, AXIS_V);
}

void padding_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	*x0 = box->pos[AXIS_H] + box->margin_lower[AXIS_H];
	*y0 = box->pos[AXIS_V] + box->margin_lower[AXIS_V];
	*x1 = *x0 + padded_dim(box, AXIS_H);
	*y1 = *y0 + padded_dim(box, AXIS_V);
}

void outer_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	*x0 = box->pos[AXIS_H];
	*y0 = box->pos[AXIS_V];
	*x1 = *x0 + outer_dim(box, AXIS_H);
	*y1 = *y0 + outer_dim(box, AXIS_V);
}

void hit_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1)
{
	if ((box->flags & BOXFLAG_HIT_OUTER) != 0)
		outer_rectangle(box, x0, x1, y0, y1);
	else
		padding_rectangle(box, x0, x1, y0, y1);
}

/* True if the size of a box may change when the size of its parent changes. */
static bool size_depends_on_parent(const Box *box)
{
	box;
	return true;
}

/* True if the size of a box may change when the size of one of its children
 * changes. */
static bool size_depends_on_children(const Box *box)
{
	box;
	return true;
}

/* Retrieves the content, padding or margin rectangle of a box. */
void get_bounding_box_rectangle(const Box *box, BoundingBox bbox, float *bounds)
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

/* Constructs the non-hierarchical clipping rectangle for a box. */
void build_clip_rectangle(const Box *box, float *r)
{
	if ((box->flags & BOXFLAG_CLIP_ALL) != 0) {
		get_bounding_box_rectangle(box, (BoundingBox)box->clip_box, r);
		for (unsigned i = 0; i < 4; ++i)
			if ((box->flags & (BOXFLAG_CLIP_LEFT << i)) == 0)
				r[i] = INFINITE_RECTANGLE[i];
	} else {
		memcpy(r, INFINITE_RECTANGLE, 4 * sizeof(float));
	}
}

/* True if A is before B in the tree. */
static bool box_before(const Box *a, const Box *b)
{
	const Box *ba, *bb;
	const Box *ancestor = (const Box *)lowest_common_ancestor(
		(const void *)a, 
		(const void *)b, 
		(const void **)&ba, 
		(const void **)&bb, 
		offsetof(Box, parent));
	ensure(ancestor != NULL); /* Undefined if A and B are not in the same tree. */
	if (ancestor == b) return false; /* A is a child of B or A = B. */
	if (ancestor == a) return true; /* B is a child of A. */
	while (ba != NULL) {
		if (ba == bb)
			return true;
		ba = ba->next_sibling;
	}
	return false;
}

/* True if 'child' is in the subtree of 'parent'. */
static bool is_child(const Box *child, const Box *parent)
{
	for (const Box *p = child->parent; p != NULL; p = p->parent)
		if (p == parent)
			return true;
	return false;
}

unsigned box_tree_depth(const Box *box)
{
	unsigned depth = 0;
	while (box->parent != NULL) {
		box = box->parent;
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
	if (is_child(a, b) && dxb == 0.0f && dyb == 0.0f)
		return true;
	Axis axis = AXIS_H;
	if (a->parent == b->parent && a->parent != NULL)
		axis = (Axis)a->parent->axis;
	else if (fabsf(dya - dyb) >= 1.0f)
		axis = AXIS_V;
	return (axis == AXIS_H) ? (dxa < dxb) : (dya < dyb);
}

void depth_sort_boxes(const Box **boxes, unsigned count)
{
	struct {
		bool operator () (const Box *a, const Box *b) const 
			{ return box_before(a, b); }
	} less_depth;
	std::sort(boxes, boxes + count, less_depth);
}

/* Changes a parent box's layout flags in response to a child box being added to
 * or removed from its child list. */
static void box_notify_child_added_or_removed(Document *document, 
	Box *parent, Box *child)
{
	/* If the parent's size depends on the size of its children, it will
	 * need to be recalculated. */
	unsigned clear_in_parents = 0;
	if (size_depends_on_children(parent)) {
		parent->flags &= ~BOXFLAG_STABLE_MASK;
		clear_in_parents |= BOXFLAG_TREE_SIZE_STABLE;
	}
	/* Depending on the parent's arragement mode, removing a box may shift
	 * its siblings. */
	bool siblings_will_move = true;
	if (child != NULL) {
		switch (parent->arrangement) {
			case ALIGN_END:
				siblings_will_move = child->prev_sibling != NULL;
				break;
			case ALIGN_MIDDLE:
				siblings_will_move = true;
				break;
			case ALIGN_START:
			default:
				siblings_will_move = child->next_sibling != NULL;
				break;
		}
	}
	if (siblings_will_move) {
		parent->flags &= ~BOXFLAG_CHILD_BOUNDS_VALID;
		clear_in_parents |= BOXFLAG_TREE_BOUNDS_VALID;
	}
	/* Clear the required parent flags. */
	if (clear_in_parents != 0) {
		parent->flags &= ~clear_in_parents;
		clear_flag_in_parents(document, parent, clear_in_parents);
	}
}

/* True if 'child' should be in the main grid. */
static bool should_be_in_grid(const Document *document, const Box *child, 
	const Box *parent)
{
	const Box *root = document->root->box;
	return child == root || (parent != NULL && is_child(parent, root));
}

/* Recursively removes boxes from the grid. */
static void remove_children_from_grid(Document *document, Box *box)
{
	grid_remove(document, box);
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
		remove_children_from_grid(document, child);
	} 
}

/* Updates a child box's layout flags in response to the child's parent having
 * changed. Does not change parent flags. */
static void box_notify_new_parent(Document *document, Box *child, Box *parent)
{
	/* Dependency bits depend on the parent for some kinds of boxes. */
	update_layout_info(document, child);
	/* If the child's size is a function of its parent's, it will need to be
	 * recalculated. */
	unsigned clear_in_parents = 0;
	if (size_depends_on_parent(child)) {
		child->flags &= ~BOXFLAG_STABLE_MASK;
		clear_in_parents |= BOXFLAG_TREE_SIZE_STABLE;
	}
	/* The child's position will always need to be recalculated. */
	child->flags &= ~(BOXFLAG_CHILD_BOUNDS_VALID | BOXFLAG_TREE_BOUNDS_VALID);
	clear_in_parents |= BOXFLAG_TREE_BOUNDS_VALID;
	clear_flag_in_parents(document, child, clear_in_parents);
	/* Boxes not in the tree should not be in the grid because we don't want
	 * them to be found in queries for mouse selection and view visibility. */
	if (!should_be_in_grid(document, child, parent)) 
		remove_children_from_grid(document, child);
}

/* True if two dimensions should be considered equal for the purposes of change
 * detection. */
inline bool sizes_equal(float a, float b)
{
	return fabsf(a - b) < 0.5f;
}

/* Sets one half of a box's size slot, making the incoming side the active
 * side if 1) this is the first time the slot has been set this tick, or 2)
 * the slot is currently using the reserve side. */
static bool set_size(const Document *document, Box *box, Axis axis, 
	float dim, bool from_parent)
{
	box->sizes[from_parent][axis] = dim;
	if ((box->flags & (BOXFLAG_WIDTH_DEFINED << axis)) == 0) {
		/* The first time a size is defined, it is copied into both parent and 
		 * child slots to simplify change detection. */
		box->flags |= BOXFLAG_WIDTH_DEFINED << axis;
		box->sizes[from_parent ^ 1][axis] = dim;
	}
	unsigned axis_flags = box->flags >> axis;
	unsigned active = axis_flags / BOXFLAG_WIDTH_ACTIVE_ABOVE;
	unsigned primary = axis_flags / BOXFLAG_WIDTH_PRIMARY_ABOVE;
	unsigned first_set = (box->size_stamp[axis] != document->layout_clock);
	unsigned mask = (first_set | (primary ^ active)) & 1;
	unsigned diff = ((unsigned(from_parent) ^ active) & mask);
	box->flags ^= (diff * BOXFLAG_WIDTH_ACTIVE_ABOVE) << axis;
	box->size_stamp[axis] = document->layout_clock;
	return ((active ^ diff) & 1) != 0;
}

/* Applies a box's size limits to 'dim'. */
static float apply_min_max(const Box *box, Axis axis, float dim)
{
	DimensionMode mode_min = (DimensionMode)box->mode_min[axis];
	DimensionMode mode_max = (DimensionMode)box->mode_max[axis];
	if (mode_min != ADEF_UNDEFINED) {
		assertb(mode_min == DMODE_ABSOLUTE);
		dim = std::max(dim, box->min[axis]);
	}
	if (mode_max != ADEF_UNDEFINED) {
		assertb(mode_max == DMODE_ABSOLUTE);
		dim = std::min(dim, box->max[axis]);
	}
	return dim;
}

/* True if 'box' is the main box of an inline context node. */
static bool is_inline_container_box(const Box *box)
{
	return box->owner != NULL && box == get_box(box->owner) && 
		get_layout(box->owner) == LAYOUT_INLINE_CONTAINER;
}

/* True if a box will be subject to grow-shrink adjustment along its parent's
 * major axis. */
static bool may_grow_or_shrink(const Box *box)
{
	return box->growth[GDIR_GROW] != 0.0f || box->growth[GDIR_SHRINK] != 0.0f;
}

/* Marks a box to be visited in the next layout iteration. */
static void must_visit(Document *document, Box *box, Axis axis)
{
	document;
	unsigned mask = (BOXFLAG_WIDTH_STABLE << axis) | BOXFLAG_TREE_SIZE_STABLE;
	do {
		box->flags &= ~mask;
		box = box->parent;
	} while (box != NULL);
}

/* Determines whether a box has dimensions that cannot by changed by layout,
 * and initializes the permanent provisional size of any such dimensions. */
static void update_fixed_dimensions(Document *document, Box *box)
{
	box->flags &= ~(BOXFLAG_WIDTH_FIXED | BOXFLAG_HEIGHT_FIXED);
	bool flexible = box->parent != NULL && may_grow_or_shrink(box);
	for (unsigned axis = 0; axis < 2; ++axis) {
		if (box->mode_dim[axis] == DMODE_ABSOLUTE && 
			(!flexible || axis != box->parent->axis)) {
			box->flags |= BOXFLAG_WIDTH_FIXED << axis;
			initialize_provisional_size(document, box, (Axis)axis, 
				box->ideal[axis]);
		}
	}
}

/* Determines whether a box's primary dimension on each axis should come from
 * the "above" or "below" half of the axis' size slot. Also sets per-pass
 * constraint enable/disable bits. */
static void update_layout_flags(Document *document, Box *box)
{
	document;

	bool is_inline = is_inline_container_box(box);
	unsigned pass0 = PASSFLAG_ALL, pass1_diff = 0;
	for (unsigned axis = 0; axis < 2; ++axis) {
		unsigned primary_above_flag = BOXFLAG_WIDTH_PRIMARY_ABOVE << axis;
		unsigned children_flag = PASSFLAG_COMPUTE_WIDTH_FROM_CHILDREN << axis;
		unsigned fixed_flag = BOXFLAG_WIDTH_FIXED << axis;

		/* When a box's dimension is absolute or fractional, or, along the 
		 * parent's major axis only, the box has nonzero grow or shrink factors, 
		 * then the box's position is set by its parent, so the from-above side 
		 * of its size slot takes precedence. */
		DimensionMode dmode = (DimensionMode)box->mode_dim[axis];
		if (dmode == DMODE_ABSOLUTE || dmode == DMODE_FRACTIONAL || 
			(!is_inline && box->parent != NULL && axis == box->parent->axis && 
				may_grow_or_shrink(box))) {
			box->flags |= primary_above_flag;
			/* Fixed dimensions are pre-initialized and independent of the box's
			 * children. */
			if ((box->flags & fixed_flag) != 0)
				pass0 &= ~children_flag;
		} else {
			/* No explicit dimension, so the box shrinks to fit its children. */
			box->flags &= ~primary_above_flag;
			/* Text boxes are invalid in the pre-text-layout pass, so disable 
			 * sizing inline containers from their children before text layout
			 * has been performed. */
			if (is_inline) {
				pass0 &= ~children_flag;
				pass1_diff |= children_flag;
			}
		}
	}
	unsigned pass1 = pass0 ^ pass1_diff;

	/* A limitation of Stacker's two-pass system is that text layout widths
	 * must be computed entirely in the first pass. It doesn't make sense to
	 * change the width of an inline container in post-text-layout, because
	 * doing so would necessitate a repeat of text layout. Clearing this flag
	 * makes inline containers refuse to change size in the second pass. */
	if (is_inline)
		pass1 &= ~PASSFLAG_PARENT_MASK;

	/* The preorder and postorder bits can only be set if the corresponding
	 * depends-on bit is set. */
	box->pass_flags[PASS_PRE_TEXT_LAYOUT] = (uint8_t)pass0;
	box->pass_flags[PASS_POST_TEXT_LAYOUT] = (uint8_t)pass1;
}

/* Precalculates information used by layout after a box's properties have 
 * changed or it has been moved in the tree. */
static void update_layout_info(Document *document, Box *box)
{
	update_fixed_dimensions(document, box);
	update_layout_flags(document, box);
}

/* A box dimension has changed. */
static void active_size_changed(Document *document, SizingPass pass, Box *box, Axis axis)
{
	unsigned stable_flag = BOXFLAG_WIDTH_STABLE << axis;
	unsigned clear_in_parents = 0;

	/* This box must be visited to propagate the new size to its children. */
	box->flags &= ~(stable_flag | BOXFLAG_TREE_SIZE_STABLE);
	clear_in_parents |= BOXFLAG_TREE_SIZE_STABLE;

	/* The box's parent must be visited to recalculate its own size from those
	 * of its children, and to redo grow-shrink positioning, since the size of
	 * this box might have affected the computed size of its siblings. */
	if (box->parent != NULL && size_depends_on_children(box->parent))
		box->parent->flags &= ~stable_flag;

	/* Our size change may move our siblings. */
	if (box->parent != NULL) {
		box->parent->flags &= ~BOXFLAG_CHILD_BOUNDS_VALID;
		clear_in_parents |= BOXFLAG_TREE_BOUNDS_VALID;
	}

	/* If this is the main box of a node, set the appropriate size-changed 
	 * flag on the node, and expansion flags in the node's parent chain. */
	if (box->owner != NULL && box->owner->box == box) {
		box->owner->flags |= (NFLAG_WIDTH_CHANGED << axis);
		propagate_expansion_flags(box->owner, 1 << axis);
	}

	/* The box's clip rectangle and the clip rectangles of all its children
	 * must be recalculated. */
	box->flags &= ~BOXFLAG_TREE_CLIP_VALID;
	clear_in_parents |= BOXFLAG_TREE_CLIP_VALID;

	/* Changing the width of an inline container invalidates its paragraph
	 * layout. */
	if (pass == PASS_PRE_TEXT_LAYOUT && is_inline_container_box(box))
		box->flags &= ~BOXFLAG_PARAGRAPH_VALID;

	if (clear_in_parents != 0)
		clear_flag_in_parents(document, box, clear_in_parents);
}

/* Sets a box's initial provisional size without performing any change 
 * detection. */
static void initialize_provisional_size(Document *document, Box *box, 
	Axis axis, float dim)
{
	dim = apply_min_max(box, axis, dim);
	set_size(document, box, axis, dim, false);
	active_size_changed(document, PASS_PRE_TEXT_LAYOUT, box, axis);
	must_visit(document, box, axis);
	lmsg("size init: box: %s axis: %d new: %.2f\n",
		get_box_debug_string(box), axis, dim);
}

/* Sets the provisional width or height of a box, updating flags accordingly. 
 * Returns true if the new constrained dimension is different from the old 
 * one. */
bool set_provisional_size(Document *document, SizingPass pass, Box *box, 
	Axis axis, float dim, bool from_parent)
{
	/* Enforce the guard bit that prevents the primary size of inline containers
	 * being modified in the post text layout pass. */
	if (from_parent && (box->pass_flags[pass] & 
		(PASSFLAG_COMPUTE_WIDTH_FROM_PARENT << axis)) == 0 && 
		(((box->flags >> axis) / BOXFLAG_WIDTH_PRIMARY_ABOVE) & 1) != 0)
		return false;

	/* Is the constrained dimension different from the one stored? */
	dim = apply_min_max(box, axis, dim);
	float old = box->sizes[from_parent][axis];
	bool changed = (box->flags & (BOXFLAG_WIDTH_DEFINED << axis)) == 0 || 
		!sizes_equal(dim, old);

	/* Store the new size. */
	bool active = set_size(document, box, axis, dim, from_parent);

	/* If we have changed the active size, we have new information to 
	 * propagate. */
	if (changed && active) {
		active_size_changed(document, pass, box, axis);
		lmsg("size change: pass: %d box: %s axis: %d, from_parent: %d, "
			"old: %.2f new: %.2f active: %.2f\n", pass, 
			get_box_debug_string(box), axis, from_parent, old, dim, 
			get_active_size(box, axis));
	}
	return changed;
}

/* Sets the ideal or initial dimension of a box. For absolute modes this also
 * immediately defines the provisional size. Note that the ideal size must be
 * separate from the provisional size to handle fractional sizes. */
void set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim)
{
	if (mode != box->mode_dim[axis] || !sizes_equal(box->ideal[axis], dim)) {
		box->mode_dim[axis] = (unsigned char)mode;
		box->ideal[axis] = dim;
		box->flags &= ~(BOXFLAG_WIDTH_DEFINED << axis);
		lmsg("ideal changed: box: %s axis: %d new: %.2f\n", 
			get_box_debug_string(box), axis, dim);
	} else {
		lmsg("ideal unchanged: box: %s dim: %.2f\n", 
			get_box_debug_string(box), dim);
	}
}

void remove_from_parent(Document *document, Box *box)
{
	Box *parent = box->parent;
	if (parent != NULL) {
		/* Update layout flags. */
		box_notify_child_added_or_removed(document, parent, box);
		/* Remove the box from the sibling chain. */
		list_remove((void **)&parent->first_child, (void **)&parent->last_child, 
			box, offsetof(Box, prev_sibling));
		box->parent = NULL;
	}
	box_notify_new_parent(document, box, NULL);
}

void append_child(Document *document, Box *parent, Box *child)
{
	remove_from_parent(document, child);
	list_insert_before(
		(void **)&parent->first_child, 
		(void **)&parent->last_child, 
		child, NULL, 
		offsetof(Box, prev_sibling));
	child->parent = parent;
	box_notify_child_added_or_removed(document, parent, child);
	box_notify_new_parent(document, child, parent);
}

void remove_all_children(Document *document, Box *parent)
{
	document;
	for (Box *child = parent->first_child, *next; child != NULL; child = next) {
		next = child->next_sibling;
		child->parent = NULL;
		child->prev_sibling = NULL;
		child->next_sibling = NULL;
		box_notify_new_parent(document, child, NULL);
	}
	parent->first_child = NULL;
	parent->last_child = NULL;
	box_notify_child_added_or_removed(document, parent, NULL);
}

Box *create_box(Document *document, Node *owner)
{
	document->system->total_boxes++;
	
	Box *box = document->free_boxes;
	if (box != NULL) {
		document->free_boxes = box->next_sibling;
		box->next_sibling = NULL;
		memset(box, 0, sizeof(Box));
	} else {
		box = new Box();
	}

	box->owner = owner;
	box->owner_next = NULL;
	box->parent = NULL;
	box->first_child = NULL;
	box->last_child = NULL;
	box->next_sibling = NULL;
	box->prev_sibling = NULL;
	box->layers = NULL;
	box->flags = 0;
	box->mouse_hit_stamp = uint32_t(-1);
	box->token_start = uint32_t(-1);
	box->token_end = uint32_t(-1);
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
	box->mode_min[AXIS_H] = ADEF_UNDEFINED;
	box->mode_min[AXIS_V] = ADEF_UNDEFINED;
	box->mode_max[AXIS_H] = ADEF_UNDEFINED;
	box->mode_max[AXIS_V] = ADEF_UNDEFINED;
	box->mode_pad_lower[AXIS_H] = ADEF_UNDEFINED;
	box->mode_pad_lower[AXIS_V] = ADEF_UNDEFINED;
	box->mode_pad_upper[AXIS_H] = ADEF_UNDEFINED;
	box->mode_pad_upper[AXIS_V] = ADEF_UNDEFINED;
	box->min[AXIS_H] = 0.0f;
	box->min[AXIS_V] = 0.0f;
	box->max[AXIS_H] = 0.0f;
	box->max[AXIS_H] = 0.0f;
	box->pad_lower[AXIS_H] = 0.0f;
	box->pad_lower[AXIS_V] = 0.0f;
	box->pad_upper[AXIS_H] = 0.0f;
	box->pad_upper[AXIS_V] = 0.0f;
	box->margin_lower[AXIS_H] = 0.0f;
	box->margin_lower[AXIS_V] = 0.0f;
	box->margin_upper[AXIS_H] = 0.0f;
	box->margin_upper[AXIS_V] = 0.0f;
	set_ideal_size(document, box, AXIS_H, (DimensionMode)ADEF_UNDEFINED, 0.0f);
	set_ideal_size(document, box, AXIS_V, (DimensionMode)ADEF_UNDEFINED, 0.0f);
}

void destroy_box(Document *document, Box *box, bool destroy_children)
{
	document->system->total_boxes--;
	document_notify_box_destroy(document, box);
	release_layer_chain(document, VLCHAIN_BOX, box->layers);
	remove_from_parent(document, box);
	grid_remove(document, box); 
	if (destroy_children) {
		destroy_sibling_chain(document, box->first_child, true);
	} else {
		remove_all_children(document, box);
	}
	box->next_sibling = document->free_boxes;
	document->free_boxes = box;
}

void destroy_sibling_chain(Document *document, Box *first, 
	bool destroy_children)
{
	while (first != NULL) {
		Box *next = first->next_sibling;
		destroy_box(document, first, destroy_children);
		first = next;
	}
}

void destroy_owner_chain(Document *document, Box *first, 
	bool destroy_children)
{
	while (first != NULL) {
		Box *next = first->owner_next;
		destroy_box(document, first, destroy_children);
		first = next;
	}
}

/* Synchronizes the properties of a block or inline container box with the 
 * attributes of the node that owns it. */
void configure_container_box(Document *document, Node *node, Axis axis, Box *box)
{
	box->axis = axis;
	
	box->mode_min[AXIS_H]          = (unsigned char)read_as_float(node, TOKEN_MIN_WIDTH, &box->min[AXIS_H], 0.0f);
	box->mode_min[AXIS_V]          = (unsigned char)read_as_float(node, TOKEN_MIN_HEIGHT, &box->min[AXIS_V], 0.0f);
	box->mode_max[AXIS_H]          = (unsigned char)read_as_float(node, TOKEN_MAX_WIDTH, &box->max[AXIS_H], 0.0);
	box->mode_max[AXIS_V]          = (unsigned char)read_as_float(node, TOKEN_MAX_HEIGHT, &box->max[AXIS_V], 0.0);
	box->mode_pad_lower[AXIS_H]    = (unsigned char)read_as_float(node, TOKEN_PADDING_LEFT, &box->pad_lower[AXIS_H]);
	box->mode_pad_upper[AXIS_H]    = (unsigned char)read_as_float(node, TOKEN_PADDING_RIGHT, &box->pad_upper[AXIS_H]);
	box->mode_pad_lower[AXIS_V]    = (unsigned char)read_as_float(node, TOKEN_PADDING_TOP, &box->pad_lower[AXIS_V]);
	box->mode_pad_upper[AXIS_V]    = (unsigned char)read_as_float(node, TOKEN_PADDING_BOTTOM, &box->pad_upper[AXIS_V]);
	box->mode_margin_lower[AXIS_H] = (unsigned char)read_as_float(node, TOKEN_MARGIN_LEFT, &box->margin_lower[AXIS_H]);
	box->mode_margin_upper[AXIS_H] = (unsigned char)read_as_float(node, TOKEN_MARGIN_RIGHT, &box->margin_upper[AXIS_H]);
	box->mode_margin_lower[AXIS_V] = (unsigned char)read_as_float(node, TOKEN_MARGIN_TOP, &box->margin_lower[AXIS_V]);
	box->mode_margin_upper[AXIS_V] = (unsigned char)read_as_float(node, TOKEN_MARGIN_BOTTOM, &box->margin_upper[AXIS_V]);
	box->mode_growth[GDIR_GROW]    = (unsigned char)read_as_float(node, TOKEN_GROW, &box->growth[GDIR_GROW], 0.0f);
	box->mode_growth[GDIR_SHRINK]  = (unsigned char)read_as_float(node, TOKEN_SHRINK, &box->growth[GDIR_SHRINK], 0.0f);

	box->arrangement = read_mode(node, TOKEN_ARRANGE, ALIGN_START);
	box->alignment = read_mode(node, TOKEN_ALIGN, ALIGN_START);
	box->clip_box = read_mode(node, TOKEN_CLIP_BOX, BBOX_OUTER);

	unsigned clip_edges = read_mode(node, TOKEN_CLIP, EDGE_FLAG_ALL);
	box->flags |= edge_set_to_box_clip_flags(clip_edges);

	float ideal_width, ideal_height;
	DimensionMode mode_width  = (DimensionMode)read_as_float(node, TOKEN_WIDTH, &ideal_width, 0.0f);
	DimensionMode mode_height = (DimensionMode)read_as_float(node, TOKEN_HEIGHT, &ideal_height, 0.0f);
	set_ideal_size(document, box, AXIS_H, mode_width, ideal_width);
	set_ideal_size(document, box, AXIS_V, mode_height, ideal_height);

	set_box_dimensions_from_image(document, node, box);
	node->flags |= NFLAG_UPDATE_SELECTION_LAYERS | NFLAG_UPDATE_BOX_LAYERS;

	update_layout_info(document, box);
}

Box *build_line_box(Document *document, Node *node, 
	Justification justification)
{
	Box *box = create_box(document, node);
	initialize_dimensions(document, box);
	box->axis = AXIS_H;
	set_ideal_size(document, box, AXIS_H, DMODE_AUTO);
	set_ideal_size(document, box, AXIS_V, DMODE_AUTO);
	switch (justification) {
		case JUSTIFY_RIGHT:
			box->alignment = ALIGN_END;
			break;
		case JUSTIFY_CENTER:
			box->alignment = ALIGN_MIDDLE;
			break;
		case JUSTIFY_LEFT:
		case JUSTIFY_FLUSH:
		default:
			box->alignment = ALIGN_START;
			break;
	}
	update_layout_info(document, box);
	return box;
}

/* Builds a box used to position a sequence of tokens in an inline context. */
Box *build_text_box(Document *document, Node *owner, 
	const char *text, unsigned text_length)
{
	Box *box = create_box(document, owner);
	box->axis = AXIS_H;
	initialize_dimensions(document, box);
	box->alignment = ALIGN_MIDDLE;
	box->flags |= BOXFLAG_SELECTION_ANCHOR | BOXFLAG_HIT_OUTER | BOXFLAG_NO_LABEL;
	set_box_debug_string(box, "subword \"%.*s\"", text_length, text);
	update_layout_info(document, box);
	return box;
}

/* Sets a box's document position, the first value 'a' being applied to the
 * specified axis, and the second, 'b' to the orthogonal axis. */
static bool set_box_position(Document *document, Box *box, float a, float b, 
	Axis axis_a = AXIS_H)
{
	document;
	Axis axis_b = Axis(axis_a ^ 1);
	bool changed = (box->flags & BOXFLAG_BOUNDS_DEFINED) == 0 || 
		!sizes_equal(a, box->pos[axis_a]) || 
		!sizes_equal(b, box->pos[axis_b]);
	box->pos[axis_a] = a;
	box->pos[axis_b] = b;
	if (changed) {
		/* The bounds of this box are now final. */
		box->flags |= BOXFLAG_BOUNDS_DEFINED;
		/* Moving this box will move its children. */
		box->flags &= ~(BOXFLAG_CHILD_BOUNDS_VALID | BOXFLAG_TREE_BOUNDS_VALID);
		/* Reinsert the box into the box grid. */
		grid_insert(document, box);
		/* If this box is the primary box of its owning node, and it has moved, 
		 * the node needs to rebuild visual layers that depend on the document 
		 * position of its box. */
		if (box->owner != NULL && box == box->owner->box)
			box->owner->flags |= NFLAG_UPDATE_TEXT_LAYERS | 
				NFLAG_UPDATE_BOX_LAYERS;
	} else if (box->cell_code == INVALID_CELL_CODE) {
		/* The box hasn't moved, but it isn't in the grid (boxes are removed
		 * from the grid when they are hidden or change parents). Now we know
		 * the box's bounds, reinsert the box into the grid. */
		grid_insert(document, box);
	}

	return changed;
}

/* Calculates the size of a box from its children. */
static void compute_size_from_children(Document *document, SizingPass pass, 
	Box *box, Axis axis)
{
	bool dim_defined = true;
	float dim = 0.0f;
	unsigned defined_flag = BOXFLAG_WIDTH_DEFINED << axis;
	if (axis == box->axis) {
		/* The size of the parent is the sum of the sizes of the children. */
		for (Box *child = box->first_child; child != NULL; 
			child = child->next_sibling) {
			if ((child->flags & defined_flag) == 0) {
				dim_defined = false;
				break;
			}
			dim += get_active_size(child, axis) + 
				padding_and_margins(child, axis);
		}
	} else {
		/* The size of the largest child defines the size of the box. */
		for (Box *child = box->first_child; child != NULL; 
			child = child->next_sibling) {
			if ((child->flags & defined_flag) == 0) {
				dim_defined = false;
				break;
			}
			float outer = get_active_size(child, axis) + 
				padding_and_margins(child, axis);
			dim = (child->prev_sibling != NULL) ? std::max(dim, outer) : outer;
		}	
	}
 	if (dim_defined)
		set_provisional_size(document, pass, box, axis, dim, false);
}

/* Determines a box dimension from the corresponding parent dimension. If the
 * requested dimension is not defined in terms of the parent, returns the
 * size computed from children. */
static bool compute_basis(SizingPass pass, const Box *box, Axis axis, 
	float parent_size, float *out_basis)
{
	switch (box->mode_dim[axis]) {
		case DMODE_ABSOLUTE:
			*out_basis = box->ideal[axis];
			return true;
		case DMODE_FRACTIONAL:
			*out_basis = box->ideal[axis] * parent_size - 
				padding_and_margins(box, axis);
			return true;
	}
	if ((box->flags & (BOXFLAG_WIDTH_DEFINED << axis)) == 0)
		return false;
	if ((box->pass_flags[pass] & 
		(PASSFLAG_COMPUTE_WIDTH_FROM_CHILDREN << axis)) != 0) {
		*out_basis = get_size_directional(box, axis, false);
		return true;
	}
	return false;
}

/* Sets the size of children along a box's major axis. */
static void size_major_axis(Document *document, SizingPass pass, Box *box)
{
	Axis major = (Axis)box->axis;
	unsigned defined_flag = BOXFLAG_WIDTH_DEFINED << major;
	if ((box->flags & defined_flag) == 0)
		return;

	/* Compute basis sizes and add up the grow and shrink factors. Children
	 * that do not define a basis are given an artifical basis of zero with
	 * default grow and shrink factors of one. This default is important for
	 * inline containers, which must establish a text layout width in the 
	 * first pass, and yet in the absence of an explicit dimension have no 
	 * basis. */
	float total_basis_size = 0.0f;
	float scale[2] = { 0.0f, 0.0f };
	float parent_dim = get_active_size(box, major);
	for (Box *child = box->first_child; child != NULL; 
			child = child->next_sibling) {
		float basis, grow, shrink;
		if (compute_basis(pass, child, major, parent_dim, &basis)) {
			total_basis_size += basis;
			shrink = child->growth[GDIR_SHRINK];
			grow = child->growth[GDIR_GROW];
			child->basis = basis;
			child->flags |= BOXFLAG_MARK;
		} else {
			shrink = (child->mode_growth[GDIR_SHRINK] != ADEF_UNDEFINED) ? 
				child->growth[GDIR_SHRINK] : 1.0f;
			grow = (child->mode_growth[GDIR_GROW] != ADEF_UNDEFINED) ? 
				child->growth[GDIR_GROW] : 1.0f;
			child->flags &= ~BOXFLAG_MARK;
		}
		scale[GDIR_SHRINK] += shrink;
		scale[GDIR_GROW] += grow;
		total_basis_size += padding_and_margins(child, major);
	}

	/* Calculate the total adjustment. If the adjustment is negative, use
	 * the shrink factors. If it's possitive, use the grow factors. */
	float adjustment = parent_dim - total_basis_size;
	GrowthDirection gdir = adjustment >= 0.0f ? GDIR_GROW : GDIR_SHRINK;
	if (fabsf(scale[gdir]) > FLT_EPSILON)
		adjustment /= scale[gdir];
			 
	/* Distribute the space between the children */
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
		float adjusted;
		if ((child->flags & BOXFLAG_MARK) != 0) {
			adjusted = child->basis + adjustment * child->growth[gdir];
		} else {
			adjusted = adjustment;
			if (child->mode_growth[gdir] != ADEF_UNDEFINED)
				adjusted *= child->growth[gdir];
		}
		set_provisional_size(document, pass, child, major, adjusted, true);
	}
}

/* Sets the size of children along a box's minor axis. */
static void size_minor_axis(Document *document, SizingPass pass, Box *box)
{
	Axis minor = Axis(box->axis ^ 1);
	unsigned defined_flag = BOXFLAG_WIDTH_DEFINED << minor;
	if ((box->flags & defined_flag) == 0)
		return;
	float parent_dim = get_active_size(box, minor);
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
		float child_dim;
		int dmode = child->mode_dim[minor];
		if ((dmode != DMODE_ABSOLUTE && dmode != DMODE_FRACTIONAL) ||
			!compute_basis(pass, child, minor, parent_dim, &child_dim))
			child_dim = parent_dim - padding_and_margins(child, minor);
		set_provisional_size(document, pass, child, minor, child_dim, true);
	}
}


/* Visits a tree of boxes in preorder, propagating size constraints down and
 * up. */
bool compute_box_sizes(Document *document, SizingPass pass, Box *box)
{
	/* Do any boxes in this subtree need visiting? */
	if ((box->flags & BOXFLAG_TREE_SIZE_STABLE) != 0)
		return true;

	/* There's no reason to visit inline container boxes before text layout
	 * has been performed. */
	if (pass == PASS_PRE_TEXT_LAYOUT && is_inline_container_box(box))
		return false;

	/* Optimistically assume that all boxes in the subtree will be marked 
	 * stable. This will be cleared if any box is marked unstable. */
	box->flags |= BOXFLAG_TREE_SIZE_STABLE;
	
	size_major_axis(document, pass, box);
	size_minor_axis(document, pass, box);
	for (Box *child = box->first_child; child != NULL; child = child->next_sibling)
		compute_box_sizes(document, pass, child);
	if ((box->pass_flags[pass] & PASSFLAG_COMPUTE_WIDTH_FROM_CHILDREN) != 0)
		compute_size_from_children(document, pass, box, AXIS_H);
	if ((box->pass_flags[pass] & PASSFLAG_COMPUTE_HEIGHT_FROM_CHILDREN) != 0)
		compute_size_from_children(document, pass, box, AXIS_V);

	return (box->flags & BOXFLAG_TREE_SIZE_STABLE) != 0;
}

/* Computes document positions for the children of a box. */
static void position_children(Document *document, Box *box)
{
	if (box->first_child == NULL)
		return;

	/* Choose a major axis starting position according to the box's arrangement. */
	Axis major = Axis(box->axis), minor = Axis(major ^ 1);
	float pos_major = content_edge_lower(box, major);
	if (box->arrangement > ALIGN_START) {
		float total_child_dim = 0.0f;
		for (const Box *child = box->first_child; child != NULL; 
			child = child->next_sibling)
			total_child_dim += outer_dim(child, major);
		float slack = get_size(box, major) - total_child_dim;
		pos_major += (box->arrangement == ALIGN_MIDDLE) ? 0.5f * slack : slack;
	}

	/* Position each child along the major axis. */
	float dim_minor = get_size(box, minor);
	for (Box *child = box->first_child; child != NULL; child = child->next_sibling) {
		/* Determine the minor axis position of the child from its alignment. */
		float pos_minor = content_edge_lower(box, minor);
		if (child->alignment > ALIGN_START) {
			float slack = dim_minor - outer_dim(child, minor);
			pos_minor += (child->alignment == ALIGN_MIDDLE) ? 
				0.5f * slack : slack;
		}
		/* Position the child. */
		set_box_position(document, child, pos_major, pos_minor, major);
		pos_major += outer_dim(child, major);
	}
}

/* Computes document positions for a tree of boxes. */
void compute_box_bounds(Document *document, Box *box, bool parent_valid)
{
	/* Nothing to do if this box and all its children have correct bounds. */
	if (parent_valid && (box->flags & BOXFLAG_TREE_BOUNDS_VALID) != 0)
		return;

	/* Reposition the immediate children of this box if required. */
	if (!parent_valid || (box->flags & BOXFLAG_CHILD_BOUNDS_VALID) == 0) {
		/* The root doesn't have a parent to position it, so it has to position
		 * itself, at (0, 0). */
		if (box->parent == NULL)
			set_box_position(document, box, 0.0f, 0.0f);
		/* Position the chidlren.  */
		position_children(document, box);
		box->flags |= BOXFLAG_CHILD_BOUNDS_VALID;
		parent_valid = false;
	}

	/* Visit each child. */
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling)
		compute_box_bounds(document, child, parent_valid);

	/* The bounds of this box and its children are now set. */
	box->flags |= BOXFLAG_TREE_BOUNDS_VALID;
	box->flags &= ~BOXFLAG_TREE_CLIP_VALID;
	clear_flag_in_parents(document, box, BOXFLAG_TREE_CLIP_VALID);
}

void clear_box_tree_flags(Document *document, Box *box, unsigned mask)
{
	box->flags &= ~mask;
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling)
		clear_box_tree_flags(document, child, mask);
}

/* Updates clip rectangles and depth values for a box subtree. */
void update_box_clip(Document *document, Box *box, const float *parent_clip, 
	int depth, bool must_update)
{
	if (!must_update && (box->flags & BOXFLAG_TREE_CLIP_VALID) != 0) 
		return;
	build_clip_rectangle(box, box->clip);
	intersect(parent_clip, box->clip, box->clip);
	box->depth = saturate16(depth);
	depth += box->depth_interval;
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling)
		update_box_clip(document, child, box->clip, depth, true);
	box->flags |= BOXFLAG_TREE_CLIP_VALID;
}

} // namespace stkr

