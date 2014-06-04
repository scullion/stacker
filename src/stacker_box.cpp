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

float padded_dim(const Box *box, Axis axis)
{
	return box->size[axis] + padding(box, axis);
}

float outer_dim(const Box *box, Axis axis)
{
	return box->size[axis] + padding_and_margins(box, axis);
}

float content_edge_lower(const Box *box, Axis axis)
{
	return box->pos[axis] + box->margin_lower[axis] + box->pad_lower[axis];
}

float content_edge_upper(const Box *box, Axis axis)
{
	return content_edge_lower(box, axis) + box->size[axis];
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
	r[1] = r[0] + box->size[AXIS_H];
	r[3] = r[2] + box->size[AXIS_V];
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
	*x1 = *x0 + box->size[AXIS_H];
	*y1 = *y0 + box->size[AXIS_V];
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
	if ((parent->flags & BOXFLAG_DEPENDS_ON_CHILDREN_MASK) != 0) {
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
	/* If the child's size is a function of its parent's, it will need to be
	 * recalculated. */
	unsigned clear_in_parents = 0;
	if ((child->flags & BOXFLAG_DEPENDS_ON_PARENT_MASK) != 0) {
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


/* True if a box's size on the specified axis has been computed at least once
 * in this layout tick, or is fixed and therefore always valid. */
static bool has_provisional_size(const Document *document, const Box *box, Axis axis)
{
	return ((box->flags & (BOXFLAG_DEPENDS_MASK_WIDTH << axis)) == 0) ||
		(box->size_stamp[axis] == get_layout_clock(document));
}

/* Like has_provisional_size(), but also marks the box for a visit if its size
 * is unavailable. */
static bool require_provisional_size(Document *document, Box *box, Axis axis)
{
	if (has_provisional_size(document, box, axis))
		return true;
	unsigned stable_flag = BOXFLAG_WIDTH_STABLE << axis;
	box->flags &= ~(stable_flag | BOXFLAG_TREE_SIZE_STABLE);
	clear_flag_in_parents(document, box, BOXFLAG_TREE_SIZE_STABLE);
	lmsg("\t%s of %s required for calculation of size of parent %s, but "
		"not defined. Marked unstable.\n", 
		((axis == AXIS_H) ? "Width" : "Height"), get_box_debug_string(box), 
		get_box_debug_string(box->parent));
	return false;
}

/* True if 'box' is the main box of an inline context node. */
static bool is_inline_container_box(const Box *box)
{
	return box->owner != NULL && box == get_box(box->owner) && 
		get_layout(box->owner) == LAYOUT_INLINE_CONTAINER;
}

/* Can the size of a box be set from above? It can if the dimension is not 
 * absolute, and has either not yet been set this tick, or has only been set by
 * the parent. */
static bool may_set_size_from_parent(const Document *document, 
	const Box *box, Axis axis, bool post_text_layout)
{
	return (box->flags & (BOXFLAG_WIDTH_DEPENDS_ON_PARENT << axis)) != 0 &&
		((box->size_stamp[axis] != get_layout_clock(document)) || 
		(box->flags & (BOXFLAG_WIDTH_SET_BY_PARENT << axis)) != 0) &&
		!(post_text_layout && is_inline_container_box(box));
}

/* Can the size of a box be set from below? */
static bool may_set_size_from_children(const Document *document, 
	const Box *box, Axis axis, bool post_text_layout)
{
	document; post_text_layout;
	return (box->flags & (BOXFLAG_WIDTH_DEPENDS_ON_CHILDREN << axis)) != 0;
}

/* Sets the provisional width or height of a box, updating flags accordingly. 
 * Returns true if the new constrained dimension is different from the old 
 * one. */
bool set_provisional_size(Document *document, Box *box, Axis axis, 
	float dim, ProvisionalSizeSource source, bool mark_unstable, 
	bool post_text_layout)
{
	assertb(source != PSS_ABOVE || may_set_size_from_parent(document, box, 
		axis, post_text_layout));

	/* Set dependency flags according to the new mode. */
	unsigned depends_on_children_flag = BOXFLAG_WIDTH_DEPENDS_ON_CHILDREN  << axis;
	unsigned set_by_parent_flag       = BOXFLAG_WIDTH_SET_BY_PARENT        << axis;
	unsigned defined_flag             = BOXFLAG_WIDTH_DEFINED              << axis;
	unsigned defined_parent_flag      = BOXFLAG_WIDTH_FROM_PARENT_DEFINED  << axis;
	unsigned stable_flag              = BOXFLAG_WIDTH_STABLE               << axis;

	/* Apply the min and max constraints if they are set. */
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

	/* Is the constrained dimension different from the one stored? */
	bool updated = !has_provisional_size(document, box, axis);
	bool changed = (box->flags & defined_flag) == 0 || 
		fabsf(dim - box->size[axis]) >= 0.5f;
	bool from_parent_changed = (source != PSS_BELOW) && 
		((box->flags & defined_parent_flag) == 0 || 
		fabsf(dim - box->size_from_parent[axis]) >= 0.5f);

	/* The dimension is now provisionally defined. */
	box->flags |= defined_flag;
	box->size_stamp[axis] = get_layout_clock(document);

	/* The set-by-parent flag says whether the main provisional dimension came
	 * from above or below. Dimensions calculated from below cannot be
  	 * overwritten by those from above. This rule exists to resolve cyclic 
 	 * dependencies.*/
	box->flags = set_or_clear(box->flags, set_by_parent_flag, 
		(source == PSS_ABOVE));

	/* The size_from_parent fields are copies of the main size that are only
	 * updated when set_by_parent is true. They have separate "defined" flags
	 * but share the other flags with the main size. */
	if (from_parent_changed) {
		box->flags |= defined_parent_flag;
		box->size_from_parent[axis] = dim;
		/* size_from_parent[AXIS_H] is the basis for text layout. */
		if (axis == AXIS_H)
			box->flags &= ~BOXFLAG_PARAGRAPH_VALID;
	}

	/* If the dimension was already defined to the same value this tick, the
	 * box remains stable. */
	if (!(updated || changed))
		return false;

	/* If the parent box calculates its size from its children, it must
	 * revisited, regardless of whether the dimension has actually change or
	 * we have just defined it for the first time this tick. */
	unsigned clear_in_parents = 0;
	if (box->parent != NULL) {
		if ((box->parent->flags & depends_on_children_flag) != 0) {
			box->parent->flags &= ~stable_flag;
			clear_in_parents |= BOXFLAG_TREE_SIZE_STABLE;
		}
	}

	if (changed) {
		/* The box should be revisited to propagate the new size to its 
		 * children. However, if this size update is being done during an
		 * axis update for the box, the new size will be immediately propagated
		 * to the box's children. In this situation 'mark_unstable' is passed 
		 * as false. */
		if (mark_unstable) {
			box->flags &= ~(stable_flag | BOXFLAG_TREE_SIZE_STABLE);
			clear_in_parents |= BOXFLAG_TREE_SIZE_STABLE;
		}

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

		/* Log a helpful message. */
		const char *source_name = (source == PSS_ABOVE) ? "parent" : 
			(source == PSS_BELOW ? "child" : "init");
		lmsg("\t%s of %s changed from %.2f to %.2f by %s.\n", 
			(axis == AXIS_H ? "Width" : "Height"), 
			get_box_debug_string(box), box->size[axis], dim, source_name);

		/* Store the new size. */
		box->size[axis] = dim;
	}

	if (clear_in_parents != 0)
		clear_flag_in_parents(document, box, clear_in_parents);

	return changed;
}

/* Sets the ideal or initial dimension of a box. For absolute modes this also
 * immediately defines the provisional size. Note that the ideal size must be
 * separate from the provisional size to handle fractional sizes. */
void set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim)
{
	/* Set dependency flags according to the new mode. */
	unsigned depends_on_parent_flag   = BOXFLAG_WIDTH_DEPENDS_ON_PARENT   << axis;
	unsigned depends_on_children_flag = BOXFLAG_WIDTH_DEPENDS_ON_CHILDREN << axis;
	unsigned defined_flag             = BOXFLAG_WIDTH_DEFINED             << axis;
	unsigned defined_parent_flag      = BOXFLAG_WIDTH_FROM_PARENT_DEFINED << axis;
	if (mode <= DMODE_AUTO) {
		box->flags |= depends_on_parent_flag | depends_on_children_flag;
	} else {
		box->flags &= ~(depends_on_children_flag | depends_on_parent_flag);
		if (mode == DMODE_FRACTIONAL)
			box->flags |= depends_on_parent_flag;
	}

	/* Are the size or mode really being changed? */
	if (mode == box->mode_dim[axis] && box->ideal[axis] == dim) {
		lmsg("Ideal size of %s unchanged at %.2f.\n", get_box_debug_string(box), dim);
		return;
	}

	box->mode_dim[axis] = (unsigned char)mode;
	box->ideal[axis] = dim;

	/* Setting the ideal size invalidates the provisional size. */
	box->flags &= ~(defined_flag | defined_parent_flag);

	/* Absolute dimensions have a permanent provisional size which we initialize
	 * when setting the ideal size. */
	if (mode == DMODE_ABSOLUTE)
		set_provisional_size(document, box, axis, dim, PSS_IDEAL);
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
	box->parent = NULL;
	box->first_child = NULL;
	box->last_child = NULL;
	box->next_sibling = NULL;
	box->prev_sibling = NULL;
	box->owner_next = NULL;
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
	set_ideal_size(document, box, AXIS_H, (DimensionMode)ADEF_UNDEFINED, 0.0f);
	set_ideal_size(document, box, AXIS_V, (DimensionMode)ADEF_UNDEFINED, 0.0f);
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
	
	float ideal_width, ideal_height;
	DimensionMode mode_width  = (DimensionMode)read_as_float(node, TOKEN_WIDTH, &ideal_width, 0.0f);
	DimensionMode mode_height = (DimensionMode)read_as_float(node, TOKEN_HEIGHT, &ideal_height, 0.0f);
	set_ideal_size(document, box, AXIS_H, mode_width, ideal_width);
	set_ideal_size(document, box, AXIS_V, mode_height, ideal_height);

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

	box->arrangement = read_mode(node, TOKEN_ARRANGE, ALIGN_START);
	box->alignment = read_mode(node, TOKEN_ALIGN, ALIGN_START);
	box->clip_box = read_mode(node, TOKEN_CLIP_BOX, BBOX_OUTER);

	unsigned clip_edges = read_mode(node, TOKEN_CLIP, EDGE_FLAG_ALL);
	box->flags |= edge_set_to_box_clip_flags(clip_edges);

	/* Inline container boxes expand to fit the width of their container
	 * unless otherwise specified. */
	if (node->layout == LAYOUT_INLINE_CONTAINER && 
		box->mode_dim[AXIS_H] == ADEF_UNDEFINED) {
		box->mode_dim[AXIS_H] = DMODE_FRACTIONAL;
		box->ideal[AXIS_H] = 1.0f;
	}

	set_box_dimensions_from_image(document, node, box);
	node->flags |= NFLAG_UPDATE_SELECTION_LAYERS | NFLAG_UPDATE_BOX_LAYERS;
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
		fabsf(a - box->pos[axis_a]) >= 0.5f ||
		fabsf(b - box->pos[axis_b]) >= 0.5f;
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
		 * the box's bounds, reinsert it into the grid. */
		grid_insert(document, box);
	}

	return changed;
}

/* Visits one axis of a box, calculating its size from its children, then
 * setting the size of its children based on its own size. */
static void update_box_axis(Document *document, Box *box, Axis axis, 
	bool post_text_layout)
{
	unsigned depends_on_parent_flag   = BOXFLAG_WIDTH_DEPENDS_ON_PARENT   << axis;
	unsigned stable_flag              = BOXFLAG_WIDTH_STABLE              << axis;

	/* Optimistically mark the box as stable. This may be cleared if we alter 
	 * a child size. */
	box->flags |= stable_flag;
	
	/* If the box's size may be determined by the sizes of its children, try to
	 * recalculate it. */
	if (may_set_size_from_children(document, box, axis, post_text_layout)) {
		bool dim_defined = (box->first_child == NULL);
		float dim = 0.0f;
		if (axis == box->axis) {
			/* The size of the parent is the sum of the sizes of the children. */
			for (Box *child = box->first_child; child != NULL; 
				child = child->next_sibling) {
				if (require_provisional_size(document, child, axis)) {
					dim += outer_dim(child, axis);
					dim_defined = true;
				}
			}
		} else {
			/* The size of the largest child defines the size of the box. */
			for (Box *child = box->first_child; child != NULL; 
				child = child->next_sibling) {
				if (require_provisional_size(document, child, axis)) {
					float outer = outer_dim(child, axis);
					dim = dim_defined ? std::max(dim, outer) : outer;
					dim_defined = true;
				}
			}	
		}
		if (dim_defined) {
			/* Apply this box's size constraints and store the new size. */
			set_provisional_size(document, box, axis, dim, PSS_BELOW, false, 
				post_text_layout);
		}
	}
	
	/* If we have a provisional size, propagate it to our children. */
	if (has_provisional_size(document, box, axis)) {
		/* Read the dimension to propagate. */
		float dim = box->size[axis];
		
		/* Propagate the dimension to our children. */
		for (Box *child = box->first_child; child != NULL;
			child = child->next_sibling) {
			/* May we set this child's size? */
			if (!may_set_size_from_parent(document, child, axis, 
				post_text_layout))
				continue;

			/* Set the child's size according to the child's mode in this 
			 * axis. */
			int dmode = child->mode_dim[axis];
			if (dmode == DMODE_FRACTIONAL) {
				float child_dim = dim * child->ideal[axis] - 
					padding_and_margins(child, axis);
				set_provisional_size(document, child, axis, child_dim, 
					PSS_ABOVE, true, post_text_layout);
			} else if (dmode <= DMODE_AUTO && axis != box->axis && 
				!has_provisional_size(document, child, axis)) {
				/* Expand children with undefined dimensions to fit the minor
				 * axis of their parent, in the absence of a shrink-fit size
				 * from below. */
				float child_dim = dim - padding_and_margins(child, axis);
					child->margin_upper[axis];
				set_provisional_size(document, child, axis, child_dim, 
					PSS_ABOVE, true, post_text_layout);
			}
		}
	} else {
		/* This box has no provisional size. If there is a prospect of one
		 * being supplied by its parent, mark the parent for a visit. */
		if (box->parent != NULL && (box->flags & depends_on_parent_flag) != 0) {
			box->parent->flags &= ~stable_flag;
			clear_flag_in_parents(document, box, BOXFLAG_TREE_SIZE_STABLE);
		}
	}
}

/* Visits a tree of boxes in preorder, propagating size constraints down and
 * up. */
bool compute_box_sizes(Document *document, Box *box, bool post_text_layout)
{
	/* Do any boxes in this subtree need visiting? */
	if ((box->flags & BOXFLAG_TREE_SIZE_STABLE) != 0)
		return true;

	/* There's no reason to visit inline container boxes before text layout
	 * has been performed. */
	if (is_inline_container_box(box) &&
		(box->flags & BOXFLAG_PARAGRAPH_VALID) == 0)
		return false;

	/* Optimistically assume that all boxes in the subtree will be marked 
	 * stable. This will be cleared if any box is marked unstable. */
	box->flags |= BOXFLAG_TREE_SIZE_STABLE;

	/* Visit boxes in preorder. */
	for (unsigned axis = 0; axis < 2; ++axis) {
		if ((box->flags & (BOXFLAG_WIDTH_STABLE << axis)) == 0)
			update_box_axis(document, box, (Axis)axis, post_text_layout);
	}
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling)
		compute_box_sizes(document, child, post_text_layout);

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
		float slack = box->size[major] - total_child_dim;
		pos_major += (box->arrangement == ALIGN_MIDDLE) ? 0.5f * slack : slack;
	}

	/* Position each child along the major axis. */
	float dim_minor = box->size[minor];
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
