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
#include "stacker_inline2.h"

namespace stkr {

#define reqflag(axis, slot) (((AXISFLAG_PREFERRED_VALID << (slot)) * FF_REQUEST_BASE) << (2 * (axis)))
#define satflag(axis, slot) (((AXISFLAG_PREFERRED_VALID << (slot)) * FF_SATISFY_BASE) << (2 * (axis)))

enum FrameFlag {
	FF_PARENT_CHANGED_H    = 1 <<  0, /* Recalculate child widths that depend on their parent width. */
	FF_PARENT_CHANGED_V    = 1 <<  1, /* Recalculate child heights that depend on their parent height. */
	FF_ANCESTOR_CHANGED_H  = 1 <<  2, /* Recalculate child widths that depend on their parent width. */
	FF_ANCESTOR_CHANGED_V  = 1 <<  3, /* Recalculate child heights that depend on their parent height. */
	FF_INVALIDATE_H        = 1 <<  4, /* Force recalculate child widths. */
	FF_INVALIDATE_V        = 1 <<  5, /* Force recalculate child heights. */
	FF_REPEAT              = 1 <<  6, /* A second or later pass is in progress for this box. */
	FF_PARENT_REPEAT       = 1 <<  7, /* A second or later pass is in progress for the parent. */
	FF_SIZING_FLAGS_VALID  = 1 <<  8, /* The box's extrinsic size validity bits are meaningful. */
	FF_REQUEST_BASE        = 1 <<  9, /* Four flags that says which axes a boxes needs to compute. */
	FF_SATISFY_BASE        = 1 << 13  /* Four flags that say whether the frame's accumulator slots are valid. */
};

enum InfoFrameFlag {
	IFF_HAS_DEPENDENT_DESCENDANT_H = 1 << 0,
	IFF_HAS_DEPENDENT_DESCENDANT_V = 1 << 1,
	IFF_DESCENDANT_IS_GROW_H = 1 << 2,
	IFF_DESCENDANT_IS_GROW_V = 1 << 3
};

const unsigned FF_REQUEST_INTRINSIC_MASK = reqflag(AXIS_H, SSLOT_INTRINSIC) | reqflag(AXIS_V, SSLOT_INTRINSIC);
const unsigned FF_REQUEST_PREFERRED_MASK = reqflag(AXIS_H, SSLOT_PREFERRED) | reqflag(AXIS_V, SSLOT_PREFERRED);
const unsigned FF_REQUEST_ALL = FF_REQUEST_PREFERRED_MASK | FF_REQUEST_INTRINSIC_MASK;

const unsigned FF_SATISFY_INTRINSIC_MASK = satflag(AXIS_H, SSLOT_INTRINSIC) | satflag(AXIS_V, SSLOT_INTRINSIC);
const unsigned FF_SATISFY_PREFERRED_MASK = satflag(AXIS_H, SSLOT_PREFERRED) | satflag(AXIS_V, SSLOT_PREFERRED);
const unsigned FF_SATISFY_ALL = FF_SATISFY_PREFERRED_MASK | FF_SATISFY_INTRINSIC_MASK;

/* Stack frame for the info update pass. */
struct InfoUpdateFrame {
	unsigned flags;
};

/* Stack frame for the size update pass. */
struct SizingFrame {
	unsigned flags;
	unsigned cflags;
	unsigned clear_mask;
	SizingStage stage;
	SizingStage jump_stage;
	float sizes[2][2];
};

/* Stack frame for the bounds update pass. */
struct BoundsUpdateFrame {
	bool parent_valid;
};

/* Stack frame for incremental clip/depth updates. */
struct ClipUpdateFrame {
	Box *ancestor;
	const float *ancestor_clip;
	int depth;
	bool must_update;
};

/* Returns the vertical axis if 'axis' is horizontal and vice versa. */
inline Axis transverse(Axis axis)
{
	return Axis(axis ^ 1);
}

/* True if 'box' is the main box of its owning node. */
static bool is_main_box(const Box *box)
{
	return box->t.counterpart.node != NULL && 
		box->t.counterpart.node->t.counterpart.box == box;
}

/* True if 'box' is the main box of an inline container node. */
static bool is_inline_container_box(const Box *box)
{
	return is_main_box(box) && box->t.counterpart.node->layout == LAYOUT_INLINE_CONTAINER;
}

/* True if a box will be subject to grow-shrink adjustment along its parent's
 * major axis. */
static bool is_flexible(const Box *box)
{
	return box->growth[GDIR_GROW] != 0.0f || box->growth[GDIR_SHRINK] != 0.0f;
}

/* True if a box axis is on major axis of a flex parent. */
static bool sized_by_flex(const Box *box, Axis axis)
{
	return box->t.parent.box != NULL && 
		box_axis(box->t.parent.box) == axis &&
		(box->t.parent.box->layout_flags & BLFLAG_HAS_FLEXIBLE_CHILD) != 0;
}

/* True if the extrinsic size of an axis depends on the extrinsic size of the
 * corresponding axis of its immediate parent box. */
bool size_depends_on_parent(const Box *box, Axis axis)
{
	DimensionMode dmode = (DimensionMode)box->axes[axis].mode_dim;
	if (int(dmode) == 0)
		dmode = DMODE_AUTO; /* FIXME (TJM): !!! hack. we should never see undefined values here. */
	switch (dmode) {
		case DMODE_ABSOLUTE:
			return sized_by_flex(box, axis) && is_flexible(box);
		case DMODE_FRACTIONAL:
		case DMODE_GROW:
			return true;
		case DMODE_AUTO:
		case DMODE_SHRINK:
			return false;
	}
	assertb(false);
	return false;
}

/* True if the extrinsic width of height of a box depend on the corresponding 
 * extrinsic size of its immediate parent. */
bool size_depends_on_parent(const Box *box)
{
	return size_depends_on_parent(box, AXIS_H) || 
	       size_depends_on_parent(box, AXIS_V);
}

/* True if the extrinsic size of the an axis might depend on the size of a box
 * above its immediate parent. */
bool size_depends_on_ancestor(const Box *box, Axis axis)
{
	return axis == AXIS_H && box->axes[axis].mode_dim == DMODE_GROW;
}

/* True if a box's intrinsic size depends on its extrinsic size. Currently only
 * the case for inline containers, which don't define an intrinsic width. */
bool intrinsic_depends_on_extrinsic(const Box *box, Axis axis)
{
	return axis == AXIS_H && is_inline_container_box(box);
}

/* True if the extrinsic size of an axis depends on the size of the 
 * corresponding axes of immediate children. */
bool size_depends_on_children(const Box *box, Axis axis)
{
	/* Boxes that grow may depend on their children if no bound is defined in
	 * the parent chain, in which case they are sized intrinsically. */
	DimensionMode dmode = (DimensionMode)box->axes[axis].mode_dim;
	return dmode <= DMODE_AUTO || dmode == DMODE_SHRINK || dmode == DMODE_GROW;
}

/* True if the extrinsic size of either axis of a box depends on the sizes of
 * the box's immediate children. */
bool size_depends_on_children(const Box *box)
{
	return size_depends_on_children(box, AXIS_H) || 
	       size_depends_on_children(box, AXIS_V);
}

/* Enforces invariants between validity flags. */
inline unsigned normalize_clear(const Box *box, Axis axis, unsigned flags)
{
	/* If the preferred size is invalid, the intrinsic size is invalid. */
	unsigned preferred_valid = axisflag(axis, AXISFLAG_PREFERRED_VALID);
	unsigned intrinsic_valid = axisflag(axis, AXISFLAG_INTRINSIC_VALID);
	if ((flags & preferred_valid) != 0)
		flags |= intrinsic_valid;

	/* If the intrinsic size is invalid, and the extrinsic depends on the 
	 * intrinsic, then the extrinsic size is invalid. */
	if ((flags & intrinsic_valid) != 0 && 
		(box->layout_flags & axisflag(axis, AXISFLAG_DEPENDS_ON_CHILDREN)) != 0)
		flags |= axisflag(axis, AXISFLAG_EXTRINSIC_VALID);
	return flags;
}

/* Enforces parent-child invariants between box layout flags given: 1) a set of
 * flags that have been cleared from an immediate child, and, 2) a set
 * of flags that must be cleared from the box itself. */
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
	to_clear |= cleared_in_child & (
		BLFLAG_LAYOUT_INFO_VALID | BLFLAG_TREE_VALID | 
		BLFLAG_TREE_BOUNDS_VALID | BLFLAG_TREE_CLIP_VALID);

	if ((to_clear & valid_mask) != 0) {
		/* The size of dependent children must be recalculated. */
		if ((box->layout_flags & axisflag(axis, AXISFLAG_HAS_DEPENDENT_CHILD)) != 0)
			to_clear |= axisflag(axis, AXISFLAG_CHILD_SIZES_NOT_INVALIDATED) | BLFLAG_TREE_VALID;
		/* A size change on the major axis invalidates flex adjustment. */
		if ((box->layout_flags & BLFLAG_HAS_FLEXIBLE_CHILD) != 0 && 
			axis == box_axis(box))
			to_clear |= BLFLAG_FLEX_VALID;
		/* Changing the width of an inline container invalidates its paragraph
		 * layout. */
		if (axis == AXIS_H && is_inline_container_box(box))
			to_clear |= (BLFLAG_TEXT_VALID | BLFLAG_INLINE_BOXES_VALID);
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
		cleared_in_children = modify_clear(document, box, axis, to_clear, 
			cleared_in_children);
		box->layout_flags &= ~cleared_in_children;
		to_clear = 0;
		box = box->t.parent.box;
	} while (box != NULL);
}

void clear_flags(Document *document, Box *box, unsigned to_clear,
	unsigned cleared_in_children)
{
	do {
		cleared_in_children = modify_clear(document, box, to_clear, 
			cleared_in_children);
		box->layout_flags &= ~cleared_in_children;
		to_clear = 0;
		box = box->t.parent.box;
	} while (box != NULL);
}

/* Applies a box's size limits to 'dim'. */
static float apply_min_max(const Box *box, Axis axis, float dim)
{
	if (box->axes[axis].mode_min == DMODE_ABSOLUTE && dim < box->axes[axis].min)
		dim = box->axes[axis].min;
	if (box->axes[axis].mode_max == DMODE_ABSOLUTE && dim > box->axes[axis].max)
		dim = box->axes[axis].max;
	return dim;
}

/* Sets the ideal or initial dimension of a box. */
bool set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim)
{
	dim = apply_min_max(box, axis, dim);
	BoxAxis *a = box->axes + axis;
	if (mode == (DimensionMode)a->mode_dim && sizes_equal(a->sizes[SSLOT_IDEAL], dim))
		return false;
	a->mode_dim = mode;
	a->sizes[SSLOT_IDEAL] = dim;
	box->layout_flags |= axisflag(axis, AXISFLAG_IDEAL_VALID);
	clear_flags(document, box, axis, axisflag(axis, AXISFLAG_ALL_VALID_MASK));
	lmsg("ideal changed: box: %s axis: %d new: %.2f\n", get_box_debug_string(box), axis, dim);
	return true;
}

static void update_dependency_flags_preorder(Document *, Box *box)
{
	/* Do the axes of this box depend on the axes of its parent and children? */
	unsigned flags = 0;
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		if (size_depends_on_parent(box, axis))
			flags |= axisflag(axis, AXISFLAG_DEPENDS_ON_PARENT);
		if (size_depends_on_ancestor(box, axis))
			flags |= axisflag(axis, AXISFLAG_DEPENDS_ON_ANCESTOR);
		if (size_depends_on_children(box, axis))
			flags |= axisflag(axis, AXISFLAG_DEPENDS_ON_CHILDREN);
	}

	/* Does this box have flexible children? */
	for (const Box *child = box->t.first.box; child != NULL; 
		child = child->t.next.box) {
		if (is_flexible(child))
			flags |= BLFLAG_HAS_FLEXIBLE_CHILD;
	}

	box->layout_flags &= ~BLFLAG_DEPENDENCY_MASK;
	box->layout_flags |= flags;
}

enum BoundStatus {
	BSTATUS_BOUNDED,   /* Bound returned. */
	BSTATUS_UNBOUNDED, /* No bound defined. */
	BSTATUS_WAIT       /* Bounded, but the bounding size is invalid. */
};

/* True if the size of an axis can serve as a constraint on the size of 
 * descendants. */
static bool defines_bound(const Box *box, Axis axis)
{
	return box->axes[axis].mode_dim > DMODE_SHRINK;
}

/* Calculates the width available to a box within its parent. The result is safe
 * for use as a text layout width because extrinsic widths are independent of
 * text layout. The same cannot be said of extrinsic heights, which is why there
 * is no height_bound() function. */
static BoundStatus width_bound(IncrementalLayoutState *s, 
	Box *box, float *result)
{
	/* Find the first box in the parent chain with a fixed or fractional 
	 * width. */
	unsigned n = 0;
	while (!defines_bound(box, AXIS_H)) {
		box = box->t.parent.box;
		if (box == NULL)
			return BSTATUS_UNBOUNDED;
		n++;
	}

	/* Is the extrinsic width of the bounding box valid? */
	if (!size_valid(box, SSLOT_EXTRINSIC, AXIS_H))
		return BSTATUS_WAIT;

	/* This function is called durining intrinsic sizing, and may encounter 
	 * boxes yet to be visited by the main sizing pass. The size validity bits 
	 * of such boxes cannot be trusted. */
	const SizingFrame *f = (const SizingFrame *)tree_iterator_peek(&s->iterator, n);
	if ((f->flags & FF_SIZING_FLAGS_VALID) == 0)
		return BSTATUS_WAIT;

	/* The bound is available. */
	*result = get_size(box, SSLOT_EXTRINSIC, AXIS_H);
	return BSTATUS_BOUNDED;
}

/* Updates bits used by layout to minimize the amount of work done when a
 * dimension changes. */
static void update_dependency_flags_postorder(Document *, Box *box, 
	InfoUpdateFrame *frame)
{
	Box *parent = box->t.parent.box;
	if (parent == NULL)
		return;

	unsigned flags = box->layout_flags;
	unsigned flags_for_parent = 0;
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		/* Propagate the in-ancestral-dependence-chain flag upwards, stopping if
		 * this node defines a width bound that satisfies the dependency. */
		unsigned doa = axisflag(axis, AXISFLAG_DEPENDS_ON_ANCESTOR);
		unsigned iadc = axisflag(axis, AXISFLAG_IN_ANCESTRAL_DEPENDENCE_CHAIN);
		if ((flags & doa) != 0 || ((flags & iadc) != 0 && !defines_bound(box, axis)))
			flags_for_parent |= iadc;

		/* Detect reciprocal dependencies. */
		unsigned doc = axisflag(axis, AXISFLAG_DEPENDS_ON_CHILDREN);
		unsigned dop = axisflag(axis, AXISFLAG_DEPENDS_ON_PARENT);
		unsigned cyc = axisflag(axis, AXISFLAG_CYCLE);
		unsigned ddesc = IFF_HAS_DEPENDENT_DESCENDANT_H << axis;
		unsigned ddesc_grow = IFF_DESCENDANT_IS_GROW_H << axis;

		/* FIXME (TJM): can we determine in the analysis phase whether DMODE_GROW
		 * bounds will actually be satisfied? If they can't be, they revert to auto
		 * sizing, which means they can generate cycles. We need to know that here
		 * (the (flags & depends_on_children) test).
		 * 
		 * Currently DOC is set conservatively on grow axes, but that causes an 
		 * unwanted reversion to preferred sizing when the grow finds a bound,
		 * e.g. in the columns test case. */

		/* If a box below depends on shrink-sized boxes, and this box is 
		 * shrink-sized, this box is the top of a cycle. */
		if ((frame->flags & (ddesc | ddesc_grow)) == ddesc && (flags & doc) != 0)
			box->layout_flags |= cyc;

		if ((flags & dop) != 0) {
			frame->flags |= ddesc;
			frame->flags = set_or_clear(frame->flags, ddesc_grow, 
				box->axes[axis].mode_dim == DMODE_GROW);
		} else {
			frame->flags &= ~(ddesc | ddesc_grow);
		}	
		
		/* If this axis depends on the corresponding parent axis, set 
		 * has-dependent-child in the parent. */
		if ((flags & axisflag(axis, AXISFLAG_DEPENDS_ON_PARENT)) != 0)
			flags_for_parent |= axisflag(axis, AXISFLAG_HAS_DEPENDENT_CHILD);
	}
	parent->layout_flags |= flags_for_parent;
	frame[-1].flags = frame->flags;
}

/* Marks a box's extrinsic size as invalid if the box is intrinsically sized. */
static void notify_intrinsic_changed(SizingFrame *frame, Box *box, Axis axis)
{
	/* Changing an intrinsic changes the parent intrinsic. */
	frame->clear_mask |= axisflag(axis, AXISFLAG_INTRINSIC_VALID);

	/* If the corresponding extrinsic comes from the intrinsic, invalidate the 
	 * extrinsic. */
	if (box->axes[axis].mode_dim <= DMODE_AUTO)
		box->layout_flags &= ~axisflag(axis, AXISFLAG_EXTRINSIC_VALID);
}

/* Called when an extrinsic size is set during layout. */
static void notify_extrinsic_changed(SizingFrame *frame, Box *box, Axis axis)
{
	/* If this is the main box of a node, set the appropriate size-changed 
	 * flag on the node, and expansion flags in the node's parent chain. */
	if (is_main_box(box)) {
		box->t.counterpart.node->t.flags |= (NFLAG_WIDTH_CHANGED << axis);
		propagate_expansion_flags(box->t.counterpart.node, 1 << axis);
	}

	/* Invalidate clip and bounds up to the root. */
	box->layout_flags &= ~(BLFLAG_CHILD_BOUNDS_VALID | BLFLAG_TREE_CLIP_VALID);
	frame->clear_mask |= BLFLAG_TREE_BOUNDS_VALID | BLFLAG_TREE_CLIP_VALID;

	/* Invalidate extrinsic sizes of immediate children that depend on the 
	 * changed size. */
	if ((box->layout_flags & axisflag(axis, AXISFLAG_HAS_DEPENDENT_CHILD)) != 0) {
		box->layout_flags &= ~axisflag(axis, AXISFLAG_CHILD_SIZES_NOT_INVALIDATED);
		box->layout_flags &= ~BLFLAG_TREE_VALID;
	}

	/* Text is broken to the extrinsic width. The intrinsic height of a text
	 * box depends on its extrinsic width. */
	if (axis == AXIS_H && is_inline_container_box(box)) {
		box->layout_flags &= ~axisflag(AXIS_V, AXISFLAG_INTRINSIC_VALID);
		box->layout_flags &= ~BLFLAG_TEXT_VALID;
		notify_intrinsic_changed(frame, box, AXIS_V);
	}

	/* A change in the major axis invalidates flex. */
	if (axis == box_axis(box))
		box->layout_flags &= ~BLFLAG_FLEX_VALID;
}

/* Sets a box's document position, the first value 'a' being applied to the
 * specified axis, and the second, 'b' to the orthogonal axis. */
static bool set_box_position(Document *document, Box *box, float a, float b, 
	Axis axis_a = AXIS_H)
{
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
		if (is_main_box(box))
			box->t.counterpart.node->t.flags |= NFLAG_UPDATE_BOX_LAYERS;
	} else if (box->cell_code == INVALID_CELL_CODE) {
		/* The box hasn't moved, but it isn't in the grid (boxes are removed
		 * from the grid when they are hidden or change parents). Now we know
		 * the box's bounds, reinsert the box into the grid. */
		grid_insert(document, box);
	}

	return changed;
}

static float resolve_fractional_size(const Box *box, Axis axis, 
	float parent_size)
{
	float fraction = get_size(box, SSLOT_IDEAL, axis);
	return fraction * parent_size - padding_and_margins(box, axis);
}

/* Sets the parent-changed and ancestor-changed bits in a set of frame flags
 * to be passed down to a new sizing frame. */
static unsigned down_propagate_change_flags(unsigned parent_lflags, unsigned frame_flags)
{
	frame_flags &= ~(FF_PARENT_CHANGED_H | FF_PARENT_CHANGED_V);
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		unsigned pcflag = FF_PARENT_CHANGED_H << axis;
		unsigned acflag = FF_ANCESTOR_CHANGED_H << axis;
		unsigned csvflag = axisflag(axis, AXISFLAG_CHILD_SIZES_NOT_INVALIDATED);
		unsigned adcflag = axisflag(axis, AXISFLAG_IN_ANCESTRAL_DEPENDENCE_CHAIN);
		if ((parent_lflags & csvflag) == 0) {
			frame_flags |= pcflag;
		}
		if ((frame_flags & pcflag) != 0) {
			/* Parent-changed implies ancestor-changed. */
			frame_flags |= acflag;
		} else if ((parent_lflags & adcflag) == 0) {
			/* Parent unchanged. Clear ancestor-changed if no longer in an 
			 * ancestral dependence chain. */
			frame_flags &= ~acflag;
		}
	}
	return frame_flags;
}

/* Reinitializes the repeat bits in a set of sizing flags based on whether this
 * subtree is in the second or a later pass. */
static unsigned reset_repeat_flags(unsigned flags)
{
	return set_or_clear(flags, FF_REPEAT | FF_SIZING_FLAGS_VALID, 
		(flags & FF_PARENT_REPEAT) != 0);
}


/* Initializes repeat flags for a new sizing frame. */
static unsigned down_propagate_repeat_flags(unsigned flags)
{
	if ((flags & FF_REPEAT) != 0)
		flags |= FF_PARENT_REPEAT;
	return reset_repeat_flags(flags);
}

/* Clears validity bits on a box based on flags passed down by the parent during 
 * sizing. */
static void apply_change_flags(Box *box, SizingFrame *f)
{
	/* Do nothing if this is a repeat pass. */
	unsigned sflags = f->flags;
	f->flags |= FF_SIZING_FLAGS_VALID;
	if ((sflags & FF_REPEAT) != 0)
		return;

	unsigned bf = box->layout_flags;
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
		/* If a size above that this axis depends on has changed, invalidate the 
		 * axis' extrinsic size. */
		bool parent_changed = (sflags & (FF_PARENT_CHANGED_H << axis)) != 0;
		bool ancestor_changed = (sflags & (FF_ANCESTOR_CHANGED_H << axis)) != 0;
		bool depends_on_parent = (bf & axisflag(axis, AXISFLAG_DEPENDS_ON_PARENT)) != 0;
		bool depends_on_ancestor = (bf & axisflag(axis, AXISFLAG_DEPENDS_ON_ANCESTOR)) != 0;
		bool force = (sflags & (FF_INVALIDATE_H << axis)) != 0;
		if (force || (parent_changed && depends_on_parent) || 
			(ancestor_changed && depends_on_ancestor)) {
			/* Invalidate the extrinsic. */
			bf &= ~axisflag(axis, AXISFLAG_EXTRINSIC_VALID);
			bf &= ~BLFLAG_TEXT_VALID;
			/* Dependencies on ancestors (i.e. DMODE_GROW widths) also involve 
			 * the intrinsic. */
			if (ancestor_changed && depends_on_ancestor)
				bf &= ~axisflag(axis, AXISFLAG_INTRINSIC_VALID);
		}

		/* If the size of an ancestor has changed on this axis and boxes below
		 * depend may depend on such sizes, clear BLFLAG_TREE_VALID to ensure
		 * we keep traversing downwards. */
		bool in_ancestral_dependence_chain = (bf & axisflag(axis, 
			AXISFLAG_IN_ANCESTRAL_DEPENDENCE_CHAIN)) != 0;
		if (ancestor_changed && in_ancestral_dependence_chain)
			bf &= ~BLFLAG_TREE_VALID;
	}
	box->layout_flags = bf;
}

/* If the tree is an inline container box, or non-inline-container node, swaps
 * between nodes and boxes. This is a helper for the layout step, which 
 * traverses inline containers as nodes and other trees as boxes. */
inline const Tree *maybe_swap_trees(const Tree *tree, const Tree *parent)
{
	if (parent != NULL && (((tree->flags << 1) ^ parent->flags) & 
		TREEFLAG_IS_INLINE_CONTAINER) == 0)
		tree = tree->counterpart.tree;
	return tree;
}

/* A replacement for tree_iterator_step() implementing a special mode for
 * inline containers, in which only inline object boxes are visited. */
static unsigned box_tree_step(IncrementalLayoutState *s, TreeIteratorMode mode)
{
	TreeIterator *ti = &s->iterator;
	if (ti->node == NULL)
		return TIF_END;
			
	/* If the next step is down or up, check whether we're moving into or out
	 * of an inline container, and switch the iterator between nodes and boxes
	 * accordingly. */
	TreeIteratorStep step = tree_iterator_query_step(ti, mode);
	const Tree *next = NULL;
	unsigned flags = TIF_END;
	if (step == TISTEP_RIGHT || step == TISTEP_DOWN) {
		const Tree *ref;
		if (step == TISTEP_RIGHT) {
			next = ti->node->next.tree;
		} else {
			ref = maybe_swap_trees(ti->node, ti->node);
			next = ref->first.tree;
		}
		ref = maybe_swap_trees(next, next); 
		flags = tree_iterator_flags(ref, step);
	} else if (step == TISTEP_UP) {
		next = ti->node->parent.tree;
		next = maybe_swap_trees(next, next->parent.tree);
		flags = TIF_VISIT_POSTORDER;
	}

	/* Set the current box. */
	s->box = next != NULL ? tree_box((Tree *)next) : NULL;
	if (s->box == NULL && step != TISTEP_NONE)
		flags = 0;
	return tree_iterator_jump(ti, next, flags);
}

/* Resets visit flags in the tree iterator as if the current box had just been
 * encountered while traversing downwards. */
void box_tree_revisit_current(IncrementalLayoutState *s)
{
	const Tree *ref = maybe_swap_trees(s->iterator.node, s->iterator.node);
	s->iterator.flags = (uint8_t)tree_iterator_flags(ref, TISTEP_DOWN);
}

/* Initiates bottom-up sizing calculation for the current node. The active 
 * sizing stage is suspended and will be restarted when the bottom-up operation
 * completes. */
static void begin_bottom_up(IncrementalLayoutState *s, 
	Document *, SizingStage return_stage, unsigned sflags)
{
	SizingFrame *frame = (SizingFrame *)tree_iterator_peek(&s->iterator, 0);
	if (frame->stage != SSTG_INTRINSIC_MAIN) {
		frame->stage = SSTG_INTRINSIC_MAIN;
		frame->jump_stage = return_stage;
	}
	frame->flags |= sflags;
	box_tree_revisit_current(s);
}

static bool update_extrinsic_size(IncrementalLayoutState *s, Document *d, 
	Box *box, Axis axis, SizingFrame *frame, bool may_compute_intrinsic)
{
	if (size_valid(box, SSLOT_EXTRINSIC, axis))
		return true;

	float new_size;
	DimensionMode dmode = (DimensionMode)box->axes[axis].mode_dim;
	if (dmode == DMODE_ABSOLUTE) {
		new_size = get_size(box, SSLOT_IDEAL, axis);
	} else if ((dmode == DMODE_GROW && axis == AXIS_V) || dmode == DMODE_FRACTIONAL) {
		/* This size is a function of the corresponding parent extrinsic size.
		 * If the parent size is unavailable, we have to wait for the next 
		 * pass. Note that grow widths and heights are handled very differently
		 * for reasons beyond the scope of a comment -- see the notes about
		 * layout dependencies elsewhere. */
		float parent_size = 0.0f;
		const Box *pbox = box->t.parent.box;
		if (pbox != NULL) {
			if (!size_valid(pbox, SSLOT_EXTRINSIC, axis))
				return true;
			parent_size = get_slot(pbox, SSLOT_EXTRINSIC, axis);
		}
		if (dmode == DMODE_GROW) {
			new_size = parent_size;
		} else {
			new_size = resolve_fractional_size(box, axis, parent_size);
		}
	} else {
		/* An automatic size or a DMODE_GROW width. The extrinsic comes from
		 * either the preferred or the intrinsic. */
		SizeSlot slot = (box->layout_flags & axisflag(axis, AXISFLAG_CYCLE)) != 0 ?
			SSLOT_PREFERRED : SSLOT_INTRINSIC;
		if (size_valid(box, slot, axis)) {
			new_size = get_size(box, slot, axis);
		} else if (may_compute_intrinsic) {
			begin_bottom_up(s, d, SSTG_INDEPENDENT_EXTRINSIC, reqflag(axis, slot));
			return false;
		} else {
			return true;
		}
	}
	new_size = apply_min_max(box, axis, new_size);
	set_size(box, SSLOT_EXTRINSIC, axis, new_size);
	notify_extrinsic_changed(frame, box, axis);
	return true;
}

static bool do_compute_extrinsic_stage(IncrementalLayoutState *s, 
	Document *document, Box *box, SizingFrame *frame, bool may_calculate_intrinsic)
{
	bool may_continue = true;
	for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1))
		if (!update_extrinsic_size(s, document, box, axis, frame, may_calculate_intrinsic))
			may_continue = false;
	return may_continue;
}

/* Calculates a flex basis size. This is an extrinsic size based on the
 * box's preferred, rather than its intrinsic size. */
bool basis_size(IncrementalLayoutState *s, Document *document, 
	Box *box, Axis axis, float *result)
{
	float size;
	if (box->axes[axis].mode_dim == DMODE_FRACTIONAL) {
		float parent_size = 0.0f;
		if (box->t.parent.box != NULL && !basis_size(s, document, 
			box->t.parent.box, axis, &parent_size))
			return false;
		size = resolve_fractional_size(box, axis, parent_size);
	} else {
		if (!size_valid(box, SSLOT_PREFERRED, axis)) {
			begin_bottom_up(s, document, SSTG_COMPLETE, reqflag(axis, SSLOT_PREFERRED));
			return false;
		}
		size = get_size(box, SSLOT_PREFERRED, axis);
	}
	*result = apply_min_max(box, axis, size);
	return true;
}

/* Adjust the sizes of flexible children along the major axis of a box. */
static bool do_flex_adjustment(IncrementalLayoutState *s, 
	Document *document, SizingFrame *frame, Box *box)
{
	/* Do nothing if flex is already valid or the box has no flexible 
	 * children. */
	if ((box->layout_flags & BLFLAG_FLEX_VALID_MASK) != BLFLAG_HAS_FLEXIBLE_CHILD)
		return true;

	/* Flex adjustment requires the box's extrinsic size. */
	if (!size_valid(box, SSLOT_EXTRINSIC, box_axis(box)))
		return true;

	/* Add up basis widths and growth factors. */
	Axis major = box_axis(box);
	float basis_total = 0.0f;
	float scale[2] = { 0.0f, 0.0f };
	float parent_dim = get_size(box, major);
	for (Box *child = box->t.first.box; child != NULL; child = child->t.next.box) {
		float unadjusted;
		if (!basis_size(s, document, child, major, &unadjusted))
			return false;
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
	for (Box *child = box->t.first.box; child != NULL; child = child->t.next.box) {
		float unadjusted;
		if (!basis_size(s, document, child, major, &unadjusted))
			return false;
		float adjusted = unadjusted + adjustment * child->growth[gdir];
		adjusted = apply_min_max(child, major, adjusted);
		if (set_size(child, SSLOT_EXTRINSIC, major, adjusted))
			notify_extrinsic_changed(frame, child, major);
	}
	box->layout_flags |= BLFLAG_FLEX_VALID;
	box->layout_flags |= axisflag(major, AXISFLAG_CHILD_SIZES_NOT_INVALIDATED);

	/* FIXME (TJM): calculating basis size twice above. find somewhere to stash it. */

	return true;
}

/* True if the size of paragraph elements inside an inline container should be
 * recalculated. */
static bool requires_text_measurement(const Box *box) 
{
	assertb(is_inline_container_box(box));
	Node *node = box->t.counterpart.node;
	return (node->t.flags & NFLAG_REMEASURE_PARAGRAPH_ELEMENTS) != 0;
}

/* Initializes incremental text measurement for the current node if required. */
static bool maybe_start_text_measurement(IncrementalLayoutState *s, 
	Document *document, Box *box, SizingFrame *frame)
{
	if (!requires_text_measurement(box))
		return false;
	Node *node = box->t.counterpart.node;
	measurement_init(&s->measurement_state, document, node);
	frame->stage = SSTG_TEXT_MEASUREMENT;
	return true;
}

/* If the sizing flags indicate that preferred sizes must be calculated, 
 * initiates an infinite-width incremental break operation used to compute
 * those sizes and returns true. */
static bool maybe_start_ideal_break(IncrementalLayoutState *s, 
	Document *document, Box *box, SizingFrame *frame)
{
	assertb(is_inline_container_box(box));
	if ((box->layout_flags & axismask(AXISFLAG_PREFERRED_VALID)) == axismask(AXISFLAG_PREFERRED_VALID))
		return false;
	incremental_break_init(&s->break_state);
	incremental_break_begin(&s->break_state, document, box->t.counterpart.node, INFINITE_LINE_WIDTH);
	frame->stage = SSTG_BREAK_IDEAL;
	return true;
}

/* Handles boxes with a DMODE_GROW width in trees without a width bound by
 * setting the intrinsic width to the preferred width. */
static void maybe_handle_unbounded_grow_width(IncrementalLayoutState *, 
	Document *, Box *box, SizingFrame *frame)
{
	/* Only DMODE_GROW widths that were not handled by compute_trivial_sizes()
	 * should be handled here. The request bit will be set if we should 
	 * handle the size. */
	if (box->axes[AXIS_H].mode_dim != DMODE_GROW || 
		(frame->cflags & reqflag(AXIS_H, SSLOT_INTRINSIC)) == 0)
		return; 
	float preferred_width = get_size(box, SSLOT_PREFERRED, AXIS_H);
	if (set_size(box, SSLOT_INTRINSIC, AXIS_H, preferred_width))
		notify_intrinsic_changed(frame, box, AXIS_H);
	frame->cflags &= ~reqflag(AXIS_H, SSLOT_INTRINSIC);
}


/* If the frame's sizing flags indicate that line breaking is required, 
 * initializes an incremental break operation and returns true. */
static bool maybe_start_final_break(IncrementalLayoutState *s, 
	Document *d, Box *box, SizingFrame *frame)
{
	assertb(is_inline_container_box(box));
	
	/* Nothing to do if the breakpoints are valid. */
	if ((box->layout_flags & BLFLAG_TEXT_VALID) != 0) {
		validate_size(box, SSLOT_INTRINSIC, AXIS_V);
		return false;
	}
	
	/* Final break width is determined by the box's extrinsic width. If that
	 * hasn't been calculated, we have to wait for the next pass. */
	if (!size_valid(box, SSLOT_EXTRINSIC, AXIS_H))
		return false;
	int max_width = round_signed(get_size(box, SSLOT_EXTRINSIC, AXIS_H));

	/* OPTIMIZE (TJM): Maybe keep the ideal break result and reuse it here if 
	 * the preferred width has become the extrinsic width. Note that it's 
	 * possible we're inside a flex container, which uses the preferred width
	 * as a basis width but hands us down a completely different extrinsic
	 * width for final breaking, so it's not as simple as just special casing
	 * shrink width containers. */

	/* Begin incremental breaking. */
	box->layout_flags &= ~BLFLAG_INLINE_BOXES_VALID;
	incremental_break_init(&s->break_state);
	incremental_break_begin(&s->break_state, d, box->t.counterpart.node, max_width);
	frame->stage = SSTG_BREAK_FINAL;
	return true;
}

/* If 'box' is an inline container that requires a box update, initializes an
 * incremental box update and returns true. */
static bool maybe_start_inline_box_update(IncrementalLayoutState *s, 
	Document *document, Box *box, SizingFrame *frame)
{
	if (!is_inline_container_box(box))
		return false;
	if ((box->layout_flags & BLFLAG_INLINE_BOXES_VALID) != 0)
		return false;
	if ((box->layout_flags & BLFLAG_TEXT_VALID) == 0)
		return false;
	box_update_init(&s->box_update_state, document, box->t.counterpart.node);
	frame->stage = SSTG_INLINE_BOX_UPDATE;
	return true;
}

/* Makes a new sizing frame. */
static SizingFrame *push_sizing_frame(IncrementalLayoutState *s, 
	SizingStage stage, unsigned parent_lflags, unsigned frame_flags)
{
	SizingFrame *f = (SizingFrame *)tree_iterator_push(&s->iterator);
	f->stage = stage;
	f->jump_stage = SSTG_COMPLETE;
	frame_flags = down_propagate_repeat_flags(frame_flags);
	frame_flags = down_propagate_change_flags(parent_lflags, frame_flags);
	f->flags = frame_flags;
	f->clear_mask = 0;
	return f;
}

/* Helper for extrinsic sizing. Pushes a frame and returns true if the children
 * of a box require a size update. */
static SizingFrame *maybe_prepare_to_visit_children(IncrementalLayoutState *s, 
	Document *, Box *box, unsigned frame_flags)
{
	unsigned parent_lflags = box->layout_flags;
	if ((parent_lflags & BLFLAG_TREE_VALID) != 0)
		return NULL;

	/* Optimistically assume that sizing will complete in one pass for the tree
	 * below this node. */
	box->layout_flags |= BLFLAG_TREE_VALID;
	box->layout_flags |= axismask(AXISFLAG_CHILD_SIZES_NOT_INVALIDATED);

	/* Avoid pushing and popping a frame for leaf boxes. */
	if (s->iterator.flags == TIF_VISIT_LEAF)
		return NULL;
			
	/* Make a new stack frame. */
	return push_sizing_frame(s, SSTG_EXTRINSIC_MAIN, parent_lflags, frame_flags);
}

/* True if a second sizing pass should be initiated to recalculate the axis
 * sizes of this box and its dependent children. It's desirable to start the
 * recalculation as low as possible in the tree. The idea here is to save time
 * by skipping over boxes where we can tell from the flags that the 
 * recalculation would fail. */
static bool should_repeat_sizing(const Box *box)
{
	/* If the extrinsic sizes of this box and every child are valid, there's
	 * no need to repeat. */
	unsigned valid_mask = axismask(AXISFLAG_EXTRINSIC_VALID);
	unsigned valid_axes = box->layout_flags & valid_mask;
	if (valid_axes == valid_mask && (box->layout_flags & BLFLAG_TREE_VALID) != 0)
		return false;

	/* If there's no parent to defer to, start at this box. */
	Box *parent = box->t.parent.box;
	if (parent == NULL)
		return true;

	/* Which of the axes of this box might we be able to recalcuate? */
	unsigned available_axes = 0;
	if ((box->layout_flags & axisflag(AXIS_H, AXISFLAG_DEPENDS_ON_PARENT)) == 0)
		available_axes |= axisflag(AXIS_H, AXISFLAG_EXTRINSIC_VALID);
	if ((box->layout_flags & axisflag(AXIS_V, AXISFLAG_DEPENDS_ON_PARENT)) == 0)
		available_axes |= axisflag(AXIS_V, AXISFLAG_EXTRINSIC_VALID);
	available_axes |= parent->layout_flags & valid_mask;

	/* If both axes can be calculated, start at this box. If not, defer to the
	 * parent. */
	return (valid_axes | available_axes) == valid_mask;
}

/* A helper to step over the current node and pop the stack if the iterator
 * moves upwards. */
static unsigned next_up(IncrementalLayoutState *s)
{
	unsigned flags = box_tree_step(s, TIMODE_UP);
	if (flags == TIF_VISIT_POSTORDER || flags == TIF_END)
		tree_iterator_pop(&s->iterator);
	return flags;
}

/* Called before a sizing frame is popped to propagate size invalidations to
 * the parent box. */
void propagate_flags_upwards(SizingFrame *frame, Box *box)
{
	Box *parent = box->t.parent.box;
	if (parent == NULL)
		return;

	unsigned mask = frame->clear_mask;

	/* Propagate BLFLAG_TREE_VALID from child to parent. */
	mask |= ~box->layout_flags & BLFLAG_TREE_VALID;

	/* Allow BLFLAG_TREE_VALID to be set in the parent only if this box's 
	 * extrinsics are both valid. */
	unsigned extrinsics_valid = axismask(AXISFLAG_EXTRINSIC_VALID);
	if ((box->layout_flags & extrinsics_valid) != extrinsics_valid)
		mask |= BLFLAG_TREE_VALID;

	/* For each axis, if the parent box is intrinsically sized (which is the
	 * case if depends-on-children is set), and the intrinsic size is being
	 * invalidated, also invalidate the extrinsic size. */
	unsigned emask = axismaskcvt(mask, AXISFLAG_INTRINSIC_VALID, AXISFLAG_EXTRINSIC_VALID);
	emask &= axismaskcvt(parent->layout_flags, AXISFLAG_DEPENDS_ON_CHILDREN, AXISFLAG_EXTRINSIC_VALID);
	mask |= emask;

	parent->layout_flags &= ~mask;
	frame->clear_mask = 0;
}

static bool handle_main_wheel(IncrementalLayoutState *s, Document *document, 
	Box *box, unsigned flags, SizingFrame *frame)
{
	SizingStage stage = frame->stage;
	
	if (stage == SSTG_EXTRINSIC_MAIN) {
		if ((flags & TIF_VISIT_PREORDER) != 0) {
			apply_change_flags(box, frame);
			stage = SSTG_EXTRINSIC;
		} else {
			stage = SSTG_VISIT_CHILDREN;
		}
	}

	if (stage == SSTG_EXTRINSIC || stage == SSTG_INDEPENDENT_EXTRINSIC) {
		if (!do_compute_extrinsic_stage(s, document, box, frame, stage == SSTG_EXTRINSIC))
			return true;
		stage = SSTG_DO_FLEX;
	}

	if (stage == SSTG_DO_FLEX) {
		if (!do_flex_adjustment(s, document, frame, box))
			return true;
		stage = SSTG_VISIT_CHILDREN;
	}

	if (stage == SSTG_VISIT_CHILDREN) {
		if ((flags & TIF_VISIT_PREORDER) != 0) {
			SizingFrame *child_frame = maybe_prepare_to_visit_children(
				s, document, box, frame->flags);
			if (child_frame != NULL) {
				box_tree_step(s, TIMODE_DEFAULT);
				frame->stage = stage;
				return true;
			} else {
				flags |= TIF_VISIT_POSTORDER;
			}
		}
		if ((flags & TIF_VISIT_POSTORDER) != 0) {
			/* Revisit this subtree if required. */
			stage = SSTG_EXTRINSIC_MAIN;
			if (should_repeat_sizing(box)) { 
				frame->flags |= FF_REPEAT; /* Second pass. */
				box_tree_revisit_current(s);
			} else {
				propagate_flags_upwards(frame, box);
				flags = next_up(s);
				if (flags == TIF_VISIT_POSTORDER || flags == TIF_END)
					return true; /* Frame popped, don't set stage. */
				/* Reuse the frame for the next sibling. */
				frame->flags = reset_repeat_flags(frame->flags);
			}
		}
	}

	frame->stage = stage;
	return true;
}

/* Helper for intrinsic sizing. Attempts to update any slots that can be 
 * satisfied without tree traversal. */
static void compute_trivial_sizes(IncrementalLayoutState *s, Document *, 
	Box *box, SizingFrame *frame)
{
	for (SizeSlot slot = SSLOT_PREFERRED; slot <= SSLOT_INTRINSIC; slot = SizeSlot(slot + 1)) {
		for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
			/* If the size is already valid, clear the REQUEST bit so that we 
			 * can stop traversing if we don't need any other sizes. Also clear
			 * the SATISFY bit to indicate that the size computed from our 
			 * children should not be used if we do continue traversal. */
			if (size_valid(box, slot, axis)) {
				frame->cflags &= ~reqflag(axis, slot);
				frame->cflags &= ~satflag(axis, slot);
				continue;
			}

			DimensionMode mode = (DimensionMode)box->axes[axis].mode_dim;
			float new_size;
			if (mode == DMODE_ABSOLUTE) {
				new_size = get_size(box, SSLOT_IDEAL, axis);
				new_size = apply_min_max(box, axis, new_size);
			} else if (mode == DMODE_GROW && slot == SSLOT_INTRINSIC && axis == AXIS_H) {
				BoundStatus status = width_bound(s, box, &new_size);
				if (status != BSTATUS_BOUNDED) {
					if (status == BSTATUS_WAIT) {
						/* The bound will not be available until the next pass.
						 * Clear the request bit to halt traversal if this is the
						 * only dimension requested. */
						frame->cflags &= ~reqflag(AXIS_H, SSLOT_INTRINSIC);
						/* Clear the SATISFY flag to prevent use of any value computed
						 * from our children. */
						frame->cflags &= ~satflag(AXIS_H, SSLOT_INTRINSIC);
					} else if (status == BSTATUS_UNBOUNDED) {
						/* A DMODE_GROW width in a tree without a width bound 
						 * is set to the box's preferred width. Allow intrinsic 
						 * sizing to continue, marking the preferred width as
						 * required. DMODE_GROW will be detected in postorder 
						 * and the preferred size will be copied into the 
						 * intrinsic. */
						frame->cflags |= reqflag(AXIS_H, SSLOT_PREFERRED);
					}
					continue;
				}
				new_size -= padding_and_margins(box, axis);
				new_size = apply_min_max(box, axis, new_size);
			} else {
				/* Size is the sum or maximum of child sizes and is computed
				 * by the main bottom-up sizing process. */
				continue;
			}

			/* Set the intrinsic or preferred size. */
			if (set_size(box, slot, axis, new_size) && slot == SSLOT_INTRINSIC)
				notify_intrinsic_changed(frame, box, axis);
			
			/* Shortcut: DMODE_GROW determines both intrinsic and extrinsic 
			 * sizes. Set the extrinsic immediately so we don't need to wait
			 * for the next pass to use it for paragraph layout. */
			if (mode == DMODE_GROW && set_size(box, SSLOT_EXTRINSIC, AXIS_H, new_size))
				notify_extrinsic_changed(frame, box, AXIS_H);

			/* Clear the request flag to avoid traversing any further if we 
			 * only need this size. */
			frame->cflags &= ~reqflag(axis, slot);

			/* Clear the SATISFY flag to prevent the use of any value computed 
			 * from our children. */
			frame->cflags &= ~satflag(axis, slot);
		}
	}
}

/* Sets a box's sizes from the computed child-sum or child-max stored in a 
 * completed sizing frame. */
static void set_sizes_from_frame(Box *box, SizingFrame *frame)
{
	for (SizeSlot slot = SSLOT_PREFERRED; slot <= SSLOT_INTRINSIC; slot = SizeSlot(slot + 1)) {
		for (Axis axis = AXIS_H; axis <= AXIS_V; axis = Axis(axis + 1)) {
			if ((frame->cflags & satflag(axis, slot)) == 0)
				continue;
			float new_size = apply_min_max(box, axis, frame->sizes[slot][axis]);
			if (set_size(box, slot, axis, new_size) && slot == SSLOT_INTRINSIC)
				notify_intrinsic_changed(frame, box, axis);
		}
	}
}

/* Helper for intrinsic sizing. Adds or maxes the dimensions of a child box 
 * into the accumulators of the frame of its parent. */
static void accumulate_sizes(IncrementalLayoutState *s, Box *b)
{
	SizingFrame *pf = (SizingFrame *)tree_iterator_peek(&s->iterator, 1);
	if (pf == NULL || pf->stage != SSTG_INTRINSIC_MAIN)
		return;
	assertb(b->t.parent.box != NULL);

	unsigned unsatisfied = 0;
	for (SizeSlot slot = SSLOT_PREFERRED; slot <= SSLOT_INTRINSIC; slot = SizeSlot(slot + 1)) {
		Axis major = box_axis(b->t.parent.box);
		if (size_valid(b, slot, major)) {
			float size_major = get_size(b, slot, major) + padding_and_margins(b, major);
			pf->sizes[slot][major] += size_major;
		} else {
			unsatisfied |= satflag(major, slot);
		}
		Axis minor = transverse(major);
		if (size_valid(b, slot, minor)) {
			float size_minor = get_size(b, slot, minor) + padding_and_margins(b, minor);
			if (size_minor > pf->sizes[slot][minor])
				pf->sizes[slot][minor] = size_minor;
		} else {
			unsatisfied |= satflag(minor, slot);
		}
	}
	pf->cflags &= ~unsatisfied;
}


/* Calculates the max-content width and height of an inline container from a
 * completed incremental break state object, storing the results in the 
 * container's preferred size slots. */
static void complete_ideal_break(IncrementalLayoutState *s, 
	Document *, Box *box, SizingFrame *frame)
{
	unsigned width, height;
	incremental_break_compute_size(&s->break_state, &width, &height);
	set_slot(box, SSLOT_PREFERRED, AXIS_H, (float)width);
	set_slot(box, SSLOT_PREFERRED, AXIS_V, (float)height);
	/* A shrink fit intrinsic width on a paragraph is set to the corresponding 
	 * preferred width, because it would not otherwise be set by the bottom-up 
	 * sizing process. Shrink fit heights are different. They are computed by
	 * the final break. */
	if (box->axes[AXIS_H].mode_dim <= DMODE_SHRINK && 
		set_size(box, SSLOT_INTRINSIC, AXIS_H, (float)width))
		notify_intrinsic_changed(frame, box, AXIS_H);
}

/* Given a completed break state, updates a node's line list and the container 
 * box's intrinsic height. */
static void complete_final_break(IncrementalLayoutState *s, 
	Document *, Box *box, SizingFrame *frame)
{
	InlineContext *icb = box->t.counterpart.node->icb;
	unsigned height;
	icb->lines = incremental_break_build_lines(&s->break_state, icb->lines, NULL, &height);
	if (set_size(box, SSLOT_INTRINSIC, AXIS_V, (float)height))
		notify_intrinsic_changed(frame, box, AXIS_V);
	frame->cflags &= ~reqflag(AXIS_V, SSLOT_INTRINSIC);
	box->layout_flags |= BLFLAG_TEXT_VALID;
}

static bool handle_intrinsic_main(IncrementalLayoutState  *s, 
	Document *document, Box *box, unsigned flags, SizingFrame *frame)
{
	bool is_inline_container = is_inline_container_box(box);
	if ((flags & TIF_VISIT_PREORDER) != 0) {
		/* Copy the frame's flags (since we need to leave them intact for our
		 * siblings), and start off with the assumption that all sizes should
		 * be set from the frame's slots after visiting children. */
		frame->cflags = frame->flags | FF_SATISFY_ALL;
		/* Special case: to calculate the intrinsic width of an inline
		 * container, we need its preferred width. */
		if (is_inline_container && (frame->cflags & reqflag(AXIS_H, SSLOT_INTRINSIC)) != 0)
			frame->cflags |= reqflag(AXIS_H, SSLOT_PREFERRED);

		/* Handle sizes that can be computed without further traversal. */
		compute_trivial_sizes(s, document, box, frame);

		/* If any REQUEST flags remain set, we need to visit this box's 
		 * children. A single stack frame is reused for all children. */
		memset(frame->sizes, 0, sizeof(frame->sizes));
		if ((frame->cflags & FF_REQUEST_ALL) != 0 && flags != TIF_VISIT_LEAF) {
			/* Text measurement uses the intrinsic size of inline objects. */
			unsigned child_flags = frame->cflags;
			if (is_inline_container && requires_text_measurement(box))
				child_flags |= FF_REQUEST_INTRINSIC_MASK;
			push_sizing_frame(s, SSTG_INTRINSIC_MAIN, box->layout_flags, child_flags);
			box_tree_step(s, TIMODE_DEFAULT);
			return true;
		} else {
			flags |= TIF_VISIT_POSTORDER;
		}
	}

	if ((flags & TIF_VISIT_POSTORDER) != 0) {
		/* If a box makes it here with a DMODE_GROW width that hasn't been 
		 * satisfied, satisfy it now with the preferred width. */
		maybe_handle_unbounded_grow_width(s, document, box, frame);
		
		/* Special operations for inline containers. */
		if (is_inline_container) {
			if (maybe_start_text_measurement(s, document, box, frame))
				return false;
			if (maybe_start_ideal_break(s, document, box, frame))
				return false;
			if (maybe_start_final_break(s, document, box, frame))
				return false;
		}

		/* Bottom up intrinsic size accumulation. */
		if (!is_inline_container)
			set_sizes_from_frame(box, frame);
		if ((s->iterator.node->flags & TREEFLAG_IS_BOX) != 0)
			accumulate_sizes(s, box);

		/* Intrinsic sizing wheel is complete. Jump back to whatever stage we
		 * interrupted, if applicable. */
		if (frame->jump_stage != SSTG_COMPLETE) {
			box_tree_revisit_current(s);
			frame->stage = frame->jump_stage;
			return false;
		}

		/* Move to the next sibling or to the parent, reusing the frame for
		 * siblings. */
		propagate_flags_upwards(frame, box);
		flags = next_up(s);
	}

	return true;
}

static bool handle_text_measurement(IncrementalLayoutState *s, 
	Document *document, Box *box, unsigned, SizingFrame *frame)
{
	Node *node = box->t.counterpart.node;
	if (!measurement_continue(&s->measurement_state, document, node))
		return true;
	measurement_deinit(&s->measurement_state);
	frame->stage = SSTG_INTRINSIC_MAIN;
	node->t.flags &= ~NFLAG_REMEASURE_PARAGRAPH_ELEMENTS;
	return false;
}

static bool handle_break_ideal(IncrementalLayoutState *s, Document *document, 
	Box *box, unsigned, SizingFrame *frame)
{
	if (!incremental_break_update(&s->break_state, document))
		return true;
	complete_ideal_break(s, document, box, frame);
	incremental_break_deinit(&s->break_state);
	frame->stage = SSTG_INTRINSIC_MAIN;
	frame->cflags &= ~FF_REQUEST_PREFERRED_MASK;
	return false;
}

static bool handle_break_final(IncrementalLayoutState *s, Document *document, 
	Box *box, unsigned, SizingFrame *frame)
{
	if (!incremental_break_update(&s->break_state, document))
		return true;
	complete_final_break(s, document, box, frame);
	incremental_break_deinit(&s->break_state);
	bool changed_state = maybe_start_inline_box_update(s, document, box, frame);
	assertb(changed_state);
	return false;
}

static bool handle_inline_box_update(IncrementalLayoutState *s, 
	Document *document, Box *box, unsigned, SizingFrame *frame)
{
	if (!box_update_continue(&s->box_update_state, document))
		return true;
	box->layout_flags |= BLFLAG_INLINE_BOXES_VALID;
	frame->stage = SSTG_INTRINSIC_MAIN;
	return false;
}

static bool handle_intrinsic_wheel(IncrementalLayoutState *s, 
	Document *document, Box *box, unsigned flags, SizingFrame *frame)
{
	switch (frame->stage) {
		case SSTG_INTRINSIC_MAIN:
			return handle_intrinsic_main(s, document, box, flags, frame);
		case SSTG_TEXT_MEASUREMENT:
			return handle_text_measurement(s, document, box, flags, frame);
		case SSTG_BREAK_IDEAL:
			return handle_break_ideal(s, document, box, flags, frame);
		case SSTG_BREAK_FINAL:
			return handle_break_final(s, document, box, flags, frame);
		case SSTG_INLINE_BOX_UPDATE:
			return handle_inline_box_update(s, document, box, flags, frame);
	}
	assertb(false);
	return true;
}

/* Computes document positions for the children of a box. */
static void position_children(Document *document, Box *box)
{
	if (box->t.first.box == NULL)
		return;

	/* Choose a major axis starting position according to the box's arrangement. */
	Axis major = box_axis(box), minor = transverse(major);
	float pos_major = content_edge_lower(box, major);
	Alignment arrangement = box_arrangement(box);
	if (arrangement > ALIGN_START) {
		float total_child_dim = 0.0f;
		for (const Box *child = box->t.first.box; child != NULL; 
			child = child->t.next.box)
			total_child_dim += outer_dim(child, major);
		float slack = get_size(box, major) - total_child_dim;
		pos_major += (arrangement == ALIGN_MIDDLE) ? 0.5f * slack : slack;
	}

	/* Position each child along the major axis. */
	float dim_minor = get_size(box, minor);
	for (Box *child = box->t.first.box; child != NULL; 
		child = child->t.next.box) {
		/* Determine the minor axis position of the child from its alignment. */
		float pos_minor = content_edge_lower(box, minor);
		Alignment alignment = box_alignment(child);
		if (alignment > ALIGN_START) {
			float slack = dim_minor - outer_dim(child, minor);
			pos_minor += (alignment == ALIGN_MIDDLE) ? 0.5f * slack : slack;
		}
		/* Position the child. */
		set_box_position(document, child, pos_major, pos_minor, major);
		pos_major += outer_dim(child, major);
	}
}

/* Performs one step in an incremental size update. Returns true if the update
 * is complete. */
static bool continue_size_update(IncrementalLayoutState *s, Document *document)
{
	SizingFrame *frame = (SizingFrame *)tree_iterator_peek(&s->iterator);
	unsigned flags = s->iterator.flags;
	if ((flags & (TIF_VISIT_PREORDER | TIF_VISIT_POSTORDER)) == 0) {
		if (flags == 0)
			flags = next_up(s);
		return flags == TIF_END;
	}

	Box *box = s->box;
	bool handled = false;
	do {
		switch (frame->stage) {
			case SSTG_EXTRINSIC_MAIN:    
			case SSTG_EXTRINSIC:
			case SSTG_INDEPENDENT_EXTRINSIC:
			case SSTG_DO_FLEX:   
			case SSTG_VISIT_CHILDREN:     
				handled = handle_main_wheel(s, document, box, flags, frame);
				break;
			case SSTG_INTRINSIC_MAIN:
			case SSTG_TEXT_MEASUREMENT:
			case SSTG_BREAK_IDEAL:
			case SSTG_BREAK_FINAL:
			case SSTG_INLINE_BOX_UPDATE: 
				handled = handle_intrinsic_wheel(s, document, box, flags, frame);
				break;
			default:
				assertb(false);
		}
		flags = s->iterator.flags;
	} while (!handled);
	return (flags == TIF_END);
}

/* Performs one step in an incremental layout info update. Returns true if the
 * update is complete. */
static bool continue_info_update(IncrementalLayoutState *s, Document *document)
{
	unsigned flags = s->iterator.flags;
	if ((flags & (TIF_VISIT_PREORDER | TIF_VISIT_POSTORDER)) == 0) {
		if (flags == 0)
			flags = next_up(s);
		return flags == TIF_END;
	}

	Box *box = s->box;
	InfoUpdateFrame *frame = (InfoUpdateFrame *)tree_iterator_peek(&s->iterator);
	if ((flags & TIF_VISIT_PREORDER) != 0) {
		frame->flags = 0;
		if ((box->layout_flags & BLFLAG_LAYOUT_INFO_VALID) == 0) {
			update_dependency_flags_preorder(document, box);
			if ((flags & TIF_VISIT_POSTORDER) == 0) {
				flags = box_tree_step(s, TIMODE_DEFAULT);
				frame = (InfoUpdateFrame *)tree_iterator_push(&s->iterator);
				return false;
			}
		} else {
			flags |= TIF_VISIT_POSTORDER; /* Step over. */
		}
	}

	if ((flags & TIF_VISIT_POSTORDER) != 0) {
		update_dependency_flags_postorder(document, box, frame);
		box->layout_flags |= BLFLAG_LAYOUT_INFO_VALID;
	}

	return next_up(s) == TIF_END;
}

/* Performs one step in an incremental box bounds update. Returns true if the
 * update is complete. */
static bool continue_bounds_update(IncrementalLayoutState *s, Document *document)
{
	unsigned flags = s->iterator.flags;
	if (flags == TIF_END)
		return true;

	const BoundsUpdateFrame *frame = (const BoundsUpdateFrame *)s->iterator.frame;
	Box *box = (Box *)s->iterator.node;
	TreeIteratorMode mode = TIMODE_DEFAULT;
	if ((flags & TIF_VISIT_PREORDER) != 0) {
		/* Reposition the immediate children of this box if required. */
		bool parent_valid = frame->parent_valid;
		if (!parent_valid || (box->layout_flags & BLFLAG_CHILD_BOUNDS_VALID) == 0) {
			/* The root doesn't have a parent to position it, so it positions 
			 * itself, at (0, 0). */
			if (box->t.parent.box == NULL)
				set_box_position(document, box, 0.0f, 0.0f);
			/* Position the chidlren.  */
			position_children(document, box);
			box->layout_flags |= BLFLAG_CHILD_BOUNDS_VALID;
		}

		/* Step over this box if none of its descendants need a bounds update. */
		if ((flags & TIF_VISIT_POSTORDER) == 0 && (!frame->parent_valid || 
			(box->layout_flags & BLFLAG_TREE_BOUNDS_VALID) == 0)) {
			tree_iterator_push(&s->iterator);
		} else {
			mode = TIMODE_UP;
		}

		/* The bounds of this box and its immediate children are now set. */
		box->layout_flags |= BLFLAG_TREE_BOUNDS_VALID; 
	} else {
		tree_iterator_pop(&s->iterator);
	}

	return tree_iterator_step(&s->iterator, mode) == TIF_END;
}

/* Performs one step in an incremental clip/depth update. Returns true if the
 * update is complete. */
static bool continue_clip_update(IncrementalLayoutState *s, Document *)
{
	unsigned flags = s->iterator.flags;
	if (flags == TIF_END)
		return true;

	const ClipUpdateFrame *pf = (const ClipUpdateFrame *)s->iterator.frame;
	Box *box = (Box *)s->iterator.node;
	TreeIteratorMode mode = TIMODE_DEFAULT;
	if ((flags & TIF_VISIT_PREORDER) != 0) {
		if (pf->must_update || (box->layout_flags & BLFLAG_TREE_CLIP_VALID) == 0) {
			/* Calculate the clip rectangle. */
			Box *ancestor = pf->ancestor;
			if ((box->t.flags & BOXFLAG_CLIP_ALL) != 0) {
				build_clip_rectangle(box, box->clip);
				rect_intersect(pf->ancestor_clip, box->clip, box->clip);
				ancestor = box;
			} else {
				memcpy(box->clip, pf->ancestor_clip, sizeof(box->clip));
			}
			box->clip_ancestor = ancestor;

			/* Store the box depth. */
			box->depth = saturate16(pf->depth);

			/* Push a frame for our children if this isn't a leaf box. */
			if ((flags & TIF_VISIT_POSTORDER) == 0) {
				ClipUpdateFrame *cf = (ClipUpdateFrame *)
					tree_iterator_push(&s->iterator);
				cf->ancestor = ancestor;
				cf->ancestor_clip = ancestor->clip;
				cf->depth = pf->depth + box->depth_interval;
				cf->must_update = true;
			}
		} else {
			/* Clip is valid for this subtree. Step over the node. */
			mode = TIMODE_UP;
		}
	} else {
		box->layout_flags |= BLFLAG_TREE_CLIP_VALID;
		tree_iterator_pop(&s->iterator);
	}

	return tree_iterator_step(&s->iterator, mode) == TIF_END;
}

/* Starts the info update stage in an incremental layout. */
static void begin_info_update_stage(IncrementalLayoutState *s, 
	Document *document, Box *root)
{
	tree_iterator_begin(
		&s->iterator, document, 
		&root->t, 
		&root->t, 
		sizeof(InfoUpdateFrame));
	s->box = root;
	s->layout_stage = LSTG_UPDATE_INFO;

	/* Create a frame for the root. */
	InfoUpdateFrame *frame = (InfoUpdateFrame *)tree_iterator_push(&s->iterator);
	frame->flags = 0;
}

/* Starts the size update stage in an incremental layout. */
static void begin_sizing_stage(IncrementalLayoutState *s, 
	Document *document, Box *root)
{
	tree_iterator_begin(
		&s->iterator, document, 
		&root->t, 
		&root->t, 
		sizeof(SizingFrame));
	s->box = root;
	s->layout_stage = LSTG_COMPUTE_SIZES;
	push_sizing_frame(s, SSTG_EXTRINSIC_MAIN, root->layout_flags, 0);
}

/* Starts the bounds update stage in an incremental layout. */
static void begin_bounds_update_stage(IncrementalLayoutState *s, 
	Document *document, Box *root)
{
	tree_iterator_begin(
		&s->iterator, document, 
		&root->t, 
		&root->t, 
		sizeof(BoundsUpdateFrame));
	BoundsUpdateFrame *frame = (BoundsUpdateFrame *)
		tree_iterator_push(&s->iterator);
	frame->parent_valid = true;
	s->layout_stage = LSTG_COMPUTE_BOUNDS;
}

/* Starts the clip/depth update stage in an incremental layout. */
static void begin_clip_update_stage(IncrementalLayoutState *s, 
	Document *document, Box *root)
{
	tree_iterator_begin(
		&s->iterator, document, 
		&root->t, 
		&root->t,
		sizeof(ClipUpdateFrame));
	ClipUpdateFrame *frame = (ClipUpdateFrame *)
		tree_iterator_push(&s->iterator);
	frame->ancestor = NULL;
	frame->ancestor_clip = INFINITE_RECTANGLE;
	frame->depth = 0;
	frame->must_update = false;
	s->layout_stage = LSTG_UPDATE_CLIP;
}

/* Finalizes an incremental layout. */
static void complete_layout(IncrementalLayoutState *s, Document *)
{
	s->layout_stage = LSTG_COMPLETE;
}

/* Performs one incremental layout operation. Returns true if the layout 
 * completes. */
static bool do_layout_step(IncrementalLayoutState *s, Document *document)
{
	Box *root = document->root->t.counterpart.box;
	switch (s->layout_stage) {
		case LSTG_UPDATE_INFO:
			if (continue_info_update(s, document))
				begin_sizing_stage(s, document, root);
			break;
		case LSTG_COMPUTE_SIZES:
			if (continue_size_update(s, document))
				begin_bounds_update_stage(s, document, root);
			break;
		case LSTG_COMPUTE_BOUNDS:
			if (continue_bounds_update(s, document))
				begin_clip_update_stage(s, document, root);
			break;
		case LSTG_UPDATE_CLIP:
			if (continue_clip_update(s, document)) {
				complete_layout(s, document);
				return true;
			}
			break;
		case LSTG_COMPLETE:
			return true;
	}
	return false;
}

/* Initializes an incremental layout state object. State objects may be reused
 * for multiple layouts. */
void init_layout(IncrementalLayoutState *s)
{
	tree_iterator_init(&s->iterator);
	s->layout_stage = LSTG_COMPLETE;
}

/* Deinitializes an incremental layout state object. */
void deinit_layout(IncrementalLayoutState *s)
{
	tree_iterator_deinit(&s->iterator);
}

/* Begins a new layout. Layouts cannot be interrupted in the middle, so the
 * state object must be in the LSTG_COMPLETE state. The optional scratch buffer
 * is an area of temporary memory for use by the layout, which, if supplied,
 * must remain valid until the layout completes. */
void begin_layout(IncrementalLayoutState *s, Document *document, Box *root,
	uint8_t *scratch_buffer, unsigned buffer_size)
{
	tree_check(&root->t);
	tree_iterator_set_buffer(&s->iterator, scratch_buffer, buffer_size);
	begin_info_update_stage(s, document, root);
}

/* Performs incremental layout steps until the layout completes or the update
 * is interrupted. */
bool continue_layout(IncrementalLayoutState *s, Document *document)
{
	while (!do_layout_step(s, document))
		if (check_interrupt(document))
			return false;
	return true;
}

} // namespace stkr
