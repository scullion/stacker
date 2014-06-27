#include "stacker_layout.h"

#include <cmath>
#include <cfloat>

#include <algorithm>

#include "stacker_token.h"
#include "stacker_attribute.h"
#include "stacker_box.h"
#include "stacker_node.h"
#include "stacker_util.h"
#include "stacker_document.h"
#include "stacker_paragraph.h"

namespace stkr {

static void compute_box_size(Document *document, Box *box, 
	unsigned sizing_flags = 0);

/* Returns the vertical axis if 'axis' is horizontal and vice versa. */
inline Axis transverse(Axis axis)
{
	return Axis(axis ^ 1);
}

/* True if 'box' is the main box of its owning node. */
static bool is_main_box(const Box *box)
{
	return box->owner != NULL && box->owner->box == box;
}

/* True if 'box' is the main box of an inline context node. */
static bool is_inline_container_box(const Box *box)
{
	return is_main_box(box) && box->owner->layout == LAYOUT_INLINE_CONTAINER;
}

/* True if a box will be subject to grow-shrink adjustment along its parent's
 * major axis. */
static bool is_flexible(const Box *box)
{
	return box->growth[GDIR_GROW] != 0.0f || box->growth[GDIR_SHRINK] != 0.0f;
}

static bool sized_by_flex(const Box *box, Axis axis)
{
	return box->parent != NULL && (Axis)box->parent->axis == axis &&
		(box->parent->layout_flags & BLFLAG_HAS_FLEXIBLE_CHILD) != 0;
}

bool size_depends_on_parent(const Box *box, Axis axis)
{
	DimensionMode dmode = (DimensionMode)box->axes[axis].mode_dim;
	return dmode != DMODE_ABSOLUTE || 
		(sized_by_flex(box, axis) && is_flexible(box));
}

bool size_depends_on_parent(const Box *box)
{
	return size_depends_on_parent(box, AXIS_H) || 
	       size_depends_on_parent(box, AXIS_V);
}

bool size_depends_on_children(const Box *box, Axis axis)
{
	DimensionMode dmode = (DimensionMode)box->axes[axis].mode_dim;
	return dmode <= DMODE_AUTO || dmode == DMODE_SHRINK;
}

bool size_depends_on_children(const Box *box)
{
	return size_depends_on_children(box, AXIS_H) || 
	       size_depends_on_children(box, AXIS_V);
}

inline unsigned normalize_clear(const Box *box, Axis axis, unsigned flags)
{
	unsigned preferred_valid = axisflag(axis, AXISFLAG_PREFERRED_VALID);
	unsigned intrinsic_valid = axisflag(axis, AXISFLAG_INTRINSIC_VALID);
	if ((flags & preferred_valid) != 0)
		flags |= intrinsic_valid;
	if ((flags & intrinsic_valid) != 0 && 
		(box->layout_flags & axisflag(axis, AXISFLAG_DEPENDS_ON_CHILDREN)) != 0)
		flags |= axisflag(axis, AXISFLAG_EXTRINSIC_VALID);
	return flags;
}

static unsigned modify_clear(Document *, Box *box, Axis axis, 
	unsigned to_clear, unsigned cleared_in_child)
{
	unsigned valid_mask = axisflag(axis, AXISFLAG_ALL_VALID_MASK);

	cleared_in_child = normalize_clear(box, axis, cleared_in_child);
	if ((cleared_in_child & valid_mask) != 0) {
		/* The containing box must be visited. */
		to_clear |= BLFLAG_TREE_VALID;
		/* A child size has changed. If this box is sized from its children,
		 * then its size may also have changed. */
		if ((box->layout_flags & axisflag(axis, AXISFLAG_DEPENDS_ON_CHILDREN)) != 0)
			to_clear |= cleared_in_child & valid_mask;
		/* When a child's size changes, its parent must recalculate flex 
		 * adjustment if there are any flexible children. */
		if ((box->layout_flags & BLFLAG_HAS_FLEXIBLE_CHILD) != 0)
			to_clear |= BLFLAG_FLEX_VALID;
		/* When a child's size changes, its siblings may move. */
		to_clear |= BLFLAG_CHILD_BOUNDS_VALID | BLFLAG_TREE_BOUNDS_VALID |
			BLFLAG_TREE_CLIP_VALID;
	}
	
	/* Some flags should be cleared in the parent if they are cleared in the 
	 * child. */
	to_clear |= cleared_in_child & (axismask(AXISFLAG_PREFERRED_VALID) |
		BLFLAG_LAYOUT_INFO_VALID | BLFLAG_TREE_VALID | 
		BLFLAG_TREE_BOUNDS_VALID | BLFLAG_TREE_CLIP_VALID);

	if ((to_clear & valid_mask) != 0) {
		/* The size of dependent children must be recalculated. */
		if ((box->layout_flags & axisflag(axis, AXISFLAG_HAS_DEPENDENT_CHILD)) != 0)
			to_clear |= axisflag(axis, AXISFLAG_CHILD_SIZES_MAY_BE_VALID) | BLFLAG_TREE_VALID;
		/* A size change on the major axis invalidates flex adjustment. */
		if ((box->layout_flags & BLFLAG_HAS_FLEXIBLE_CHILD) != 0 && 
			axis == (Axis)box->axis)
			to_clear |= BLFLAG_FLEX_VALID;
		/* Changing the width of an inline container invalidates its paragraph
		 * layout. */
		if (axis == AXIS_H && is_inline_container_box(box))
			to_clear |= BLFLAG_PARAGRAPH_VALID;
	}

	return to_clear;
}

static unsigned modify_clear(Document *document, Box *box, unsigned to_clear, 
	unsigned cleared_in_child)
{
	return modify_clear(document, box, AXIS_H, to_clear, cleared_in_child) |
	       modify_clear(document, box, AXIS_V, to_clear, cleared_in_child);
}

void clear_flags(Document *document, Box *box, Axis axis, 
	unsigned to_clear, unsigned cleared_in_children)
{
	do {
		if ((box->layout_flags & BLFLAG_PROTECT) != 0)
			break;
		cleared_in_children = modify_clear(document, box, axis, to_clear, 
			cleared_in_children);
		box->layout_flags &= ~cleared_in_children;
		to_clear = 0;
		box = box->parent;
	} while (box != NULL);
}

void clear_flags(Document *document, Box *box, unsigned to_clear,
	unsigned cleared_in_children)
{
	do {
		if ((box->layout_flags & BLFLAG_PROTECT) != 0)
			break;
		cleared_in_children = modify_clear(document, box, to_clear, 
			cleared_in_children);
		box->layout_flags &= ~cleared_in_children;
		to_clear = 0;
		box = box->parent;
	} while (box != NULL);
}

/* Applies a box's size limits to 'dim'. */
static float apply_min_max(const Box *box, Axis axis, float dim)
{
	dim = std::max(dim, box->axes[axis].min);
	dim = std::min(dim, box->axes[axis].max);
	return dim;
}

/* Sets the ideal or initial dimension of a box. */
bool set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim)
{
	dim = apply_min_max(box, axis, dim);
	BoxAxis *a = box->axes + axis;
	if (mode == (DimensionMode)a->mode_dim && 
		sizes_equal(a->sizes[SSLOT_IDEAL], dim))
		return false;
	a->mode_dim = mode;
	a->sizes[SSLOT_IDEAL] = dim;
	box->layout_flags |= axisflag(axis, AXISFLAG_IDEAL_VALID);
	clear_flags(document, box, axis, axisflag(axis, AXISFLAG_ALL_VALID_MASK));
	lmsg("ideal changed: box: %s axis: %d new: %.2f\n", 
		get_box_debug_string(box), axis, dim);
	return true;
}

static void update_dependency_flags_preorder(Document *, Box *box)
{
	unsigned flags = 0;

	/* Do the axes of this box depend on the axes of its parent and children? */
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		if (size_depends_on_parent(box, axis))
			flags |= axisflag(axis, AXISFLAG_DEPENDS_ON_PARENT);
		if (size_depends_on_children(box, axis))
			flags |= axisflag(axis, AXISFLAG_DEPENDS_ON_CHILDREN);
	}

	/* Does this box have flexible children? */
	for (const Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
		if (is_flexible(child))
			flags |= BLFLAG_HAS_FLEXIBLE_CHILD;
	}

	box->layout_flags &= ~BLFLAG_DEPENDENCY_MASK;
	box->layout_flags |= flags;
}

/* Updates bits used by layout to minimize the amount of work scheduled when a
 * dimension changes. */
static void update_dependency_flags_postorder(Document *, Box *box)
{
	unsigned flags = 0;

	/* Does this box have children whose sizes depend on it? */
	if (is_inline_container_box(box)) {
		/* Inline container boxes are a special case because we add their
		 * children (the line boxes) dynamically. */
		flags |= axisflag(AXIS_H, AXISFLAG_HAS_DEPENDENT_CHILD | 
			AXISFLAG_HAS_DEPENDENT_ANCESTOR);
	} else {
		for (const Box *child = box->first_child; child != NULL; 
			child = child->next_sibling) {
			for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
				unsigned depends_on_parent = axisflag(axis, AXISFLAG_DEPENDS_ON_PARENT);
				unsigned has_dependent_ancestor = axisflag(axis, AXISFLAG_HAS_DEPENDENT_ANCESTOR);
				if ((child->layout_flags & depends_on_parent) != 0)
					flags |= axisflag(axis, AXISFLAG_HAS_DEPENDENT_CHILD);
				flags |= child->layout_flags & has_dependent_ancestor;
			}
		}
	}

	box->layout_flags &= ~axismask(AXISFLAG_HAS_DEPENDENT_CHILD | 
		AXISFLAG_HAS_DEPENDENT_ANCESTOR);
	box->layout_flags |= flags;
}

/* Performs a traversal of the tree under 'box' to precalculate dependency bits 
 * and other info used by layout. */
void update_layout_info(Document *document, Box *box)
{
	if ((box->layout_flags & BLFLAG_LAYOUT_INFO_VALID) != 0)
		return;
	update_dependency_flags_preorder(document, box);
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling)
		update_layout_info(document, child);
	update_dependency_flags_postorder(document, box);
	box->layout_flags |= BLFLAG_LAYOUT_INFO_VALID;
}

/* Called when a final axis size is set during layout. */
static void notify_size_changed(Document *, Box *box, Axis axis)
{
	/* If this is the main box of a node, set the appropriate size-changed 
	 * flag on the node, and expansion flags in the node's parent chain. */
	if (is_main_box(box)) {
		box->owner->flags |= (NFLAG_WIDTH_CHANGED << axis);
		propagate_expansion_flags(box->owner, 1 << axis);
	}
	box->layout_flags &= ~(BLFLAG_CHILD_BOUNDS_VALID | BLFLAG_TREE_CLIP_VALID);
	Box *parent = box->parent;
	if (parent != NULL) {
		parent->layout_flags &= ~BLFLAG_CHILD_BOUNDS_VALID;
		do {
			parent->layout_flags &= ~(BLFLAG_TREE_BOUNDS_VALID | 
				BLFLAG_TREE_CLIP_VALID);
			parent = parent->parent;
		} while (parent != NULL); 
	}

	if ((box->layout_flags & axisflag(axis, AXISFLAG_HAS_DEPENDENT_CHILD)) != 0) {
		box->layout_flags &= ~axisflag(axis, AXISFLAG_CHILD_SIZES_MAY_BE_VALID);
		box->layout_flags &= ~BLFLAG_TREE_VALID;
	}
	if (axis == AXIS_H)
		box->layout_flags &= ~BLFLAG_PARAGRAPH_VALID;
	if (axis == (Axis)box->axis)
		box->layout_flags &= ~BLFLAG_FLEX_VALID;
}

/* Sets a box's document position, the first value 'a' being applied to the
 * specified axis, and the second, 'b' to the orthogonal axis. */
static bool set_box_position(Document *document, Box *box, float a, float b, 
	Axis axis_a = AXIS_H)
{
	document;
	Axis axis_b = transverse(axis_a);
	bool changed = (box->layout_flags & BLFLAG_BOUNDS_DEFINED) == 0 || 
		!sizes_equal(a, box->axes[axis_a].pos) || 
		!sizes_equal(b, box->axes[axis_b].pos);
	box->axes[axis_a].pos = a;
	box->axes[axis_b].pos = b;
	if (changed) {
		/* The bounds of this box are now final. */
		box->layout_flags |= BLFLAG_BOUNDS_DEFINED;
		/* Moving this box will move its children. */
		box->layout_flags &= ~(BLFLAG_CHILD_BOUNDS_VALID | BLFLAG_TREE_BOUNDS_VALID);
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

static bool update_intrinsic_or_preferred_size(Document *document, Box *box, 
	SizeSlot slot, Axis axis);

/* The size of the parent is the sum of the sizes of the children. */
static float major_axis_content_extent(Document *document, Box *box, 
	SizeSlot slot, Axis axis)
{	
	float dim = 0.0f;
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
		update_intrinsic_or_preferred_size(document, child, slot, axis);
		dim += get_size(child, slot, axis) + padding_and_margins(child, axis);
	}
	return dim;
}

/* The size of the largest child defines the size of the box. */
static float minor_axis_content_extent(Document *document, Box *box, 
	SizeSlot slot, Axis axis)
{
	float dim = box->first_child != NULL ? -FLT_MAX : 0.0f;
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
		update_intrinsic_or_preferred_size(document, child, slot, axis);
		float outer = get_size(child, slot, axis) + 
			padding_and_margins(child, axis);
		if (outer > dim)
			dim = outer;
	}
	return dim;
}

static float content_extent(Document *document, Box *box, SizeSlot slot, 
	Axis axis)
{
	return (axis == (Axis)box->axis) ? 
		major_axis_content_extent(document, box, slot, axis) :
		minor_axis_content_extent(document, box, slot, axis);
}

static float resolve_fractional_size(const Box *box, Axis axis, 
	float parent_size)
{
	float fraction = get_size(box, SSLOT_IDEAL, axis);
	return fraction * parent_size - padding(box, axis);
}

/* Computes document positions for the children of a box. */
static void position_children(Document *document, Box *box)
{
	if (box->first_child == NULL)
		return;

	/* Choose a major axis starting position according to the box's arrangement. */
	Axis major = Axis(box->axis), minor = transverse(major);
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
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
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
static void compute_box_bounds(Document *document, Box *box, 
	bool parent_valid = true)
{
	/* Reposition the immediate children of this box if required. */
	if (!parent_valid || (box->layout_flags & BLFLAG_CHILD_BOUNDS_VALID) == 0) {
		/* The root doesn't have a parent to position it, so it has to position
		 * itself, at (0, 0). */
		if (box->parent == NULL)
			set_box_position(document, box, 0.0f, 0.0f);
		/* Position the chidlren.  */
		position_children(document, box);
		box->layout_flags |= BLFLAG_CHILD_BOUNDS_VALID;
		parent_valid = false;
	}

	/* Nothing further to do if all children have correct bounds. */
	if (parent_valid && (box->layout_flags & BLFLAG_TREE_BOUNDS_VALID) != 0)
		return;

	/* Visit each child. */
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling)
		compute_box_bounds(document, child, parent_valid);

	/* The bounds of this box and its children are now set. */
	box->layout_flags |= BLFLAG_TREE_BOUNDS_VALID;
}

/* Marks the intrinsic sizes of shrink-fit parents of 'box' invalid. */
static void notify_intrinsic_changed(Box *box)
{
	unsigned depends_mask = axismask(AXISFLAG_HAS_DEPENDENT_CHILD);
	for (box = box->parent; box != NULL; box = box->parent) {
		if ((box->layout_flags & depends_mask) == 0)
			break;
		for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
			unsigned depends_flag = axisflag(axis, AXISFLAG_HAS_DEPENDENT_CHILD);
			unsigned valid_mask = axisflag(axis, AXISFLAG_INTRINSIC_VALID);
			if ((box->layout_flags & depends_flag) == 0)
				continue;
			box->layout_flags &= ~(valid_mask | BLFLAG_TREE_VALID);
		}
	}
}

static float calculate_intrinsic_or_preferred(Document *document, Box *box, 
	SizeSlot slot, Axis axis)
{
	float new_size =  0.0f;
	if (box->axes[axis].mode_dim == DMODE_ABSOLUTE) {
		new_size = get_size(box, SSLOT_IDEAL, axis);
	} else if (is_inline_container_box(box)) {
		if (axis == AXIS_H) {
			/* Special case: the intrinsic width of an inline container is 
			 * always its preferred width. This ensures that no text layout
			 * width is ever changed as a result of text layout. */
			if (slot == SSLOT_INTRINSIC) {
				update_intrinsic_or_preferred_size(document, box, 
					SSLOT_PREFERRED, AXIS_H);
				new_size = get_size(box, SSLOT_PREFERRED, axis);
			} else {
				new_size = 0.0f;
			}
		} else {
			if (slot == SSLOT_INTRINSIC)
				new_size = content_extent(document, box, slot, axis); 
			else
				new_size = 0.0f;
		}
	} else {
		new_size = content_extent(document, box, slot, axis); 
	}
	new_size = apply_min_max(box, axis, new_size);
	return new_size;
}

static bool update_intrinsic_or_preferred_size(Document *document, Box *box, 
	SizeSlot slot, Axis axis)
{
	unsigned valid_flag = slotflag(slot, axis);
	if ((box->layout_flags & valid_flag) != 0)
		return false;
	float new_size = calculate_intrinsic_or_preferred(document, box, 
		slot, axis);
	return set_size(box, slot, axis, new_size);
}

static bool update_extrinsic_size(Document *document, Box *box, Axis axis)
{
	if (size_valid(box, SSLOT_EXTRINSIC, axis))
		return false;

	float new_size;
	DimensionMode dmode = (DimensionMode)box->axes[axis].mode_dim;
	if (dmode == DMODE_ABSOLUTE) {
		new_size = get_size(box, SSLOT_IDEAL, axis);
	} else {
		/* A parent axis size is required for non-absolute modes. */
		float parent_size = 0.0f;
		if (box->parent != NULL) {
			if (!size_valid(box->parent, SSLOT_EXTRINSIC, axis)) {
				/* Revisit when we have a parent extrinsic. */
				box->parent->layout_flags &= ~BLFLAG_TREE_VALID; 
				return false;
			}
			parent_size = get_size(box->parent, axis);
		}

		if (dmode == DMODE_FRACTIONAL) {
			new_size = resolve_fractional_size(box, axis, parent_size);
		} else {
			new_size = get_size(box, SSLOT_INTRINSIC, axis);
			if (dmode == DMODE_GROW && parent_size > new_size)
				new_size = parent_size;
		}
		new_size = apply_min_max(box, axis, new_size);
	}

	bool changed = set_size(box, SSLOT_EXTRINSIC, axis, new_size);
	if (changed)
		notify_size_changed(document, box, axis); 
	return changed;
}

enum SizingFlag {
	SZFLAG_PARENT_CHANGED_H = 1 << 0,
	SZFLAG_PARENT_CHANGED_V = 1 << 1,
	SZFLAG_RECALCULATE_H    = 1 << 2,
	SZFLAG_RECALCULATE_V    = 1 << 3
};

/* Calculates a flex basis size. This is an extrinsic size based on the
* box's preferred, rather than its intrinsic size. */
float basis_size(Document *document, Box *box, Axis axis)
{
	float size;
	if (box->axes[axis].mode_dim == DMODE_FRACTIONAL) {
		float parent_size = box->parent != NULL ?
			basis_size(document, box->parent, axis) : NULL;
		size = resolve_fractional_size(box, axis, parent_size);
	}
	else {
		update_intrinsic_or_preferred_size(document, box, SSLOT_PREFERRED, axis);
		size = get_size(box, SSLOT_PREFERRED, axis);
	}
	return apply_min_max(box, axis, size);
}

/* Adjust the sizes of flexible children along the major axis of a box. */
static void do_flex_adjustment(Document *document, Box *box)
{
	/* Do nothing if flex is already valid or the box has no flexible 
	 * children. */
	if ((box->layout_flags & BLFLAG_FLEX_VALID_MASK) != 
		BLFLAG_HAS_FLEXIBLE_CHILD)
		return;

	/* Flex adjustment requires the box's extrinsic size. */
	if (!size_valid(box, SSLOT_EXTRINSIC, (Axis)box->axis)) {
		if (box->parent != NULL)
			box->parent->layout_flags &= ~BLFLAG_TREE_VALID;
		return;
	}

	/* Add up basis widths and growth factors. */
	Axis major = Axis(box->axis);
	float basis_total = 0.0f;
	float scale[2] = { 0.0f, 0.0f };
	float parent_dim = get_size(box, major);
	for (Box *child = box->first_child; child != NULL; child = child->next_sibling) {
		float unadjusted = basis_size(document, child, major);
		basis_total += unadjusted + padding_and_margins(child, major);
		scale[GDIR_SHRINK] += child->growth[GDIR_SHRINK];
		scale[GDIR_GROW] += child->growth[GDIR_GROW];
	}

	/* Calculate the total adjustment. If the adjustment is negative, use
	 * the shrink factors. If it's positive, use the grow factors. */
	float adjustment = parent_dim - basis_total;
	GrowthDirection gdir = adjustment >= 0.0f ? GDIR_GROW : GDIR_SHRINK;
	if (fabsf(scale[gdir]) > FLT_EPSILON)
		adjustment /= scale[gdir];

	/* Distribute the adjustment between the children */
	for (Box *child = box->first_child; child != NULL; child = child->next_sibling) {
		float unadjusted = basis_size(document, child, major);
		float adjusted = unadjusted + adjustment * child->growth[gdir];
		adjusted = apply_min_max(child, major, adjusted);
		if (set_size(child, SSLOT_EXTRINSIC, major, adjusted))
			notify_size_changed(document, child, major);
	}
	box->layout_flags |= BLFLAG_FLEX_VALID;
	box->layout_flags |= axisflag(major, AXISFLAG_CHILD_SIZES_MAY_BE_VALID);

	/* FIXME (TJM): calculating basis size twice above. find somewhere to stash it. */
}

/* Clears validity bits in a box flag word based on flags passed down by the
 * parent during sizing. */
static void apply_sizing_flags(Box *box, unsigned sizing_flags)
{
	unsigned bf = box->layout_flags;
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		/* If the parent size has changed or this is a forced layout, invalidate
		 * the extrinsic size. */
		unsigned depends_flag = axisflag(axis, AXISFLAG_DEPENDS_ON_PARENT);
		bool parent_changed = (sizing_flags & (SZFLAG_PARENT_CHANGED_H << axis)) != 0;
		bool depends_on_parent = (bf & depends_flag) != 0;
		bool force = (sizing_flags & (SZFLAG_RECALCULATE_H << axis)) != 0;
		if (force || (parent_changed && depends_on_parent))
			bf &= ~axisflag(axis, AXISFLAG_EXTRINSIC_VALID);
	}
	box->layout_flags = bf;
}

static void compute_child_sizes(Document *document, Box *box, 
	unsigned sizing_flags)
{
	if ((box->layout_flags & BLFLAG_TREE_VALID) != 0)
		return;
	
	/* Set parent-changed flags. */
	sizing_flags &= ~(SZFLAG_PARENT_CHANGED_H | SZFLAG_PARENT_CHANGED_V);
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		if ((box->layout_flags & axisflag(axis, AXISFLAG_CHILD_SIZES_MAY_BE_VALID)) == 0)
			sizing_flags |= SZFLAG_PARENT_CHANGED_H << axis;
	}

	/* Visit each child. */
	box->layout_flags |= BLFLAG_TREE_VALID;
	box->layout_flags |= axismask(AXISFLAG_CHILD_SIZES_MAY_BE_VALID);
	for (Box *child = box->first_child; child != NULL; 
		child = child->next_sibling) {
		compute_box_size(document, child, sizing_flags);
	}

	/* Propagate tree flags upwards. */
	if (box->parent != NULL)
		box->parent->layout_flags &= box->layout_flags | ~BLFLAG_TREE_VALID;
}

static void compute_axis_sizes(Document *document, Box *box)
{
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		update_intrinsic_or_preferred_size(document, box, SSLOT_INTRINSIC, axis);
		update_extrinsic_size(document, box, axis);
	}
}

/* True if a second sizing pass should be initiated to recalculate the axis
 * sizes of this box and its dependent children. */
static bool should_recalculate_extrinsic_sizes(const Box *box)
{
	/* If we've reached the root and descendant sizes are invalid, always do a
	 * second pass, because there's no prospect of one being initiated further 
	 * up. */
	if (box->parent == NULL && (box->layout_flags & BLFLAG_TREE_VALID) == 0)
		return true;

	/* Repeat sizing should run if all invalid axes for this box can be 
	 * calculated. */
	unsigned valid_mask = axismask(AXISFLAG_EXTRINSIC_VALID);
	unsigned valid_axes = box->layout_flags & valid_mask;
	if (valid_axes == valid_mask)
		return false;

	/* Axes that don't depend on the parent are always available. */
	unsigned available_axes = 0;
	if ((box->layout_flags & axisflag(AXIS_H, AXISFLAG_DEPENDS_ON_PARENT)) == 0)
		available_axes |= axisflag(AXIS_H, AXISFLAG_EXTRINSIC_VALID);
	if ((box->layout_flags & axisflag(AXIS_V, AXISFLAG_DEPENDS_ON_PARENT)) == 0)
		available_axes |= axisflag(AXIS_V, AXISFLAG_EXTRINSIC_VALID);

	/* If the parent has a valid extrinsic size for an axis, we can use it to
	 * calculate the extrinsic size for the same axis of this box. */
	if (box->parent != NULL)
		available_axes |= box->parent->layout_flags & valid_mask;
	else
		available_axes |= valid_mask;
	return (valid_axes | available_axes) == valid_mask;
}

static void maybe_update_inline_boxes(Document *document, Box *box)
{
	if (!is_inline_container_box(box))
		return;
	if ((box->layout_flags & BLFLAG_PARAGRAPH_VALID) != 0)
		return;
	float width = get_provisional_size(box, AXIS_H);
	update_inline_boxes(document, box, width);
	update_layout_info(document, box);
}

static void maybe_recalculate_extrinsic_sizes(Document *document, Box *box)
{
	if (should_recalculate_extrinsic_sizes(box))
		compute_box_size(document, box);
}

/* Updates sizes for a tree of boxes. */
static void compute_box_size(Document *document, Box *box, 
	unsigned sizing_flags)
{
	apply_sizing_flags(box, sizing_flags);
	compute_axis_sizes(document, box);
	do_flex_adjustment(document, box);
	maybe_update_inline_boxes(document, box);
	compute_child_sizes(document, box, sizing_flags);
	maybe_recalculate_extrinsic_sizes(document, box);
}

/* Updates sizes and positions for a tree of boxes. */
void layout(Document *document, Box *root)
{
	update_layout_info(document, root);
	compute_box_size(document, root);
	compute_box_bounds(document, root);
}

} // namespace stkr
