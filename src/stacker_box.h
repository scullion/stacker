#pragma once

#include <cstdint>

#include "stacker.h"
#include "stacker_tree.h"

namespace stkr {

struct Document;
enum DimensionMode;
enum BoundingBox;
enum Justification;

enum AxisFlag {
	AXISFLAG_PREFERRED_VALID               = 1 <<  0, // Preferred size is valid.
	AXISFLAG_INTRINSIC_VALID               = 1 <<  1, // Intrinsic size is valid.
	AXISFLAG_EXTRINSIC_VALID               = 1 <<  2, // Extrinsic size is valid.
	AXISFLAG_IDEAL_VALID                   = 1 <<  3, // Ideal size is valid.
	AXISFLAG_DEPENDS_ON_PARENT             = 1 <<  4, // Extrinsic size depends on extrinsic size of immediate parent.
	AXISFLAG_DEPENDS_ON_CHILDREN           = 1 <<  5, // Extrinsic size depends on sizes of immediate children.
	AXISFLAG_DEPENDS_ON_ANCESTOR           = 1 <<  6, // Extrinsic size depends on non-immediate parent. Does not imply AXISFLAG_DEPENDS_ON_PARENT.
	AXISFLAG_CHILD_SIZES_NOT_INVALIDATED   = 1 <<  7, // Child sizes have not been invalidated by a change in parent size (but may still be individually invalid).
	AXISFLAG_HAS_DEPENDENT_CHILD           = 1 <<  8, // Extrinsic sizes of one or more immediate children depend on the extrinsic size of this axis.
	AXISFLAG_IN_ANCESTRAL_DEPENDENCE_CHAIN = 1 <<  9, // A descendant has AXISFLAG_DEPENDS_ON_ANCESTOR set.
	AXISFLAG_CYCLE                         = 1 << 10  // Use preferred instead of intrinsic for auto sizing.
};
const unsigned NUM_AXIS_FLAGS = 11; 

const unsigned AXISFLAG_DEPENDS_MASK     = AXISFLAG_DEPENDS_ON_PARENT | AXISFLAG_DEPENDS_ON_ANCESTOR | AXISFLAG_DEPENDS_ON_CHILDREN;
const unsigned AXISFLAG_ALL_VALID_MASK   = AXISFLAG_EXTRINSIC_VALID | AXISFLAG_INTRINSIC_VALID | AXISFLAG_PREFERRED_VALID;

const unsigned MAX_VIEWS_PER_DOCUMENT = 8;

enum BoxFlag {
	BOXFLAG_HIT_TEST                = 1 <<  0, // Mouse events within the box generate interaction events for the owning node.
	BOXFLAG_HIT_OUTER               = 1 <<  1, // Count the margin as part of the box for the purposes of hit testing.
	BOXFLAG_SELECTION_ANCHOR        = 1 <<  2, // The box can be the anchor that determines the extent of a mouse selection.
	BOXFLAG_NO_LABEL                = 1 <<  3, // Don't draw debug labels for this box.
							  	     	 
	BOXFLAG_CLIP_LEFT               = 1 <<  4, // Don't draw pixels left of the box's left edge.
	BOXFLAG_CLIP_RIGHT              = 1 <<  5, // Don't draw pixels right of the box's right edge.
	BOXFLAG_CLIP_TOP                = 1 <<  6, // Don't draw pixels above the box's top edge.
	BOXFLAG_CLIP_BOTTOM             = 1 <<  7, // Don't draw pixels below the box's bottom edge.
							  	     	 
	BOXFLAG_IS_TEXT_BOX             = 1 <<  8, // Set if the box is displaying text.
	BOXFLAG_IS_LINE_BOX             = 1 <<  9, // Set for line boxes inside inline containers.
	BOXFLAG_SAME_PARAGRAPH          = 1 << 10, // Set if paragraph is unchanged since last inline box update.
	BOXFLAG_TEXT_LAYER_MAY_BE_VALID = 1 << 11, // If clear, the box's text layer must be rebuilt.
	BOXFLAG_TEXT_LAYER_KNOWN_VALID  = 1 << 12, // Text layer is known to match box's element range, style and spacing.

	BOXFLAG_FIELD_SHIFT             = 14      // Start of fields packed into the flag word.
};

const unsigned BOXFLAG_AXIS_BITS        = 1;
const unsigned BOXFLAG_ARRANGEMENT_BITS = 2;
const unsigned BOXFLAG_ALIGNMENT_BITS   = 2;
const unsigned BOXFLAG_CLIP_BOX_BITS    = 2;

const unsigned BOXFLAG_AXIS_SHIFT        = BOXFLAG_FIELD_SHIFT       + 0;
const unsigned BOXFLAG_ARRANGEMENT_SHIFT = BOXFLAG_AXIS_SHIFT        + BOXFLAG_AXIS_BITS;
const unsigned BOXFLAG_ALIGNMENT_SHIFT   = BOXFLAG_ARRANGEMENT_SHIFT + BOXFLAG_ARRANGEMENT_BITS;
const unsigned BOXFLAG_CLIP_BOX_SHIFT    = BOXFLAG_ALIGNMENT_SHIFT   + BOXFLAG_ALIGNMENT_BITS;
const unsigned BOXFLAG_VISIBLE_SHIFT     = BOXFLAG_CLIP_BOX_SHIFT    + BOXFLAG_CLIP_BOX_BITS;

const unsigned BOXFLAG_AXIS_MASK         = ((1 << BOXFLAG_AXIS_BITS)        - 1) << BOXFLAG_AXIS_SHIFT;
const unsigned BOXFLAG_ARRANGEMENT_MASK  = ((1 << BOXFLAG_ARRANGEMENT_BITS) - 1) << BOXFLAG_ARRANGEMENT_SHIFT;
const unsigned BOXFLAG_ALIGNMENT_MASK    = ((1 << BOXFLAG_ALIGNMENT_BITS)   - 1) << BOXFLAG_ALIGNMENT_SHIFT;
const unsigned BOXFLAG_CLIP_BOX_MASK     = ((1 << BOXFLAG_CLIP_BOX_BITS)    - 1) << BOXFLAG_CLIP_BOX_SHIFT;
const unsigned BOXFLAG_FIELD_MASK        = BOXFLAG_AXIS_MASK | BOXFLAG_ARRANGEMENT_MASK | 
                                           BOXFLAG_ALIGNMENT_MASK | BOXFLAG_CLIP_BOX_MASK;

const unsigned BOXFLAG_CLIP_X           = BOXFLAG_CLIP_LEFT | BOXFLAG_CLIP_RIGHT;
const unsigned BOXFLAG_CLIP_Y           = BOXFLAG_CLIP_TOP | BOXFLAG_CLIP_BOTTOM;
const unsigned BOXFLAG_CLIP_ALL         = BOXFLAG_CLIP_X | BOXFLAG_CLIP_Y;
const unsigned BOXFLAG_VISIBILITY_MASK  = (MAX_VIEWS_PER_DOCUMENT - 1) << BOXFLAG_VISIBLE_SHIFT;

const unsigned BOXFLAG_TEXT_LAYER_VALID_MASK = BOXFLAG_TEXT_LAYER_MAY_BE_VALID | BOXFLAG_TEXT_LAYER_KNOWN_VALID;

enum BoxLayoutFlag {
	BLFLAG_LAYOUT_INFO_VALID                  = 1 <<  0, // Dependency flags are valid.
	BLFLAG_TREE_VALID                         = 1 <<  1, // All descendants have valid extrinsic sizes. Does not imply that this node has valid sizes.
	BLFLAG_FLEX_VALID                         = 1 <<  2, // Flexible immediate children have valid final sizes.
	BLFLAG_HAS_FLEXIBLE_CHILD                 = 1 <<  3, // If the size of a child has changed, this box must perform flex adjustment.
	
	BLFLAG_TEXT_VALID                         = 1 <<  4, // Box width has not changed since the last paragraph layout.
	BLFLAG_INLINE_BOXES_VALID                 = 1 <<  5, // Inline boxes are synchronized with the paragraph and breakpoints.  

	BLFLAG_BOUNDS_DEFINED                     = 1 <<  6, // The bounds of this box have been set at some time in the past.
	BLFLAG_CHILD_BOUNDS_VALID                 = 1 <<  7, // The bounds of the immediate children of this box are up to date.
	BLFLAG_TREE_BOUNDS_VALID                  = 1 <<  8, // CHILD_BOUNDS_VALID is set for all boxes in this subtree.

	BLFLAG_TREE_CLIP_VALID                    = 1 <<  9, // The depths of all recursive children are valid.

	BLFLAG_AXIS_BASE                          = 1 << 10, // Base for per-axis flags.
	BLFLAG_AXIS_H                             = BLFLAG_AXIS_BASE << (AXIS_H * NUM_AXIS_FLAGS),
	BLFLAG_AXIS_V                             = BLFLAG_AXIS_BASE << (AXIS_V * NUM_AXIS_FLAGS)
};

/* Each axis of each box has four sizes.
 * 
 * EXTRINSIC: The final size of the axis, accounting for constraints inloving 
 *            the parent like fractional and grow dimensions.
 * INTRINSIC: The size the axis must be to enclose the box's children laid out
 *            at their intrinsic sizes. Never depends on any extrinsic size in
 *            the same axis, but may depend on an extrinsic size in the other
 *            axis.
 * PREFERRED: An intrinsic size that counts all inline containers in the tree as
 *            zero-sized, and thus is unaffected by paragraph layout.
 * IDEAL:     The axis's user-specified dimension. May be auto, grow/shrink, a 
 *            fixed size or a fraction of the parent dimension.
 */
/* FIXME (TJM): update this comment. */


/* Note: these must be in the same order as the validity bits in 
 * Box::layout_flags. */
enum SizeSlot { 
	SSLOT_PREFERRED,
	SSLOT_INTRINSIC, 
	SSLOT_EXTRINSIC, 
	SSLOT_IDEAL,
	NUM_SIZE_SLOTS
};

#define maskcvt(n, s, d) ((n) / ((s) & -(s)) * ((d) & -(d)))
#define axismaskcvt(n, s, d) maskcvt(n, axismask(s), axismask(d))
#define axisflag(axis, m) (((m) * BLFLAG_AXIS_BASE) << ((axis) * NUM_AXIS_FLAGS))
#define axismask(m) (axisflag(AXIS_H, m) | axisflag(AXIS_V, m))
#define slotflag(slot, axis) axisflag((axis), AXISFLAG_PREFERRED_VALID << (slot)) 

const unsigned BLFLAG_FLEX_VALID_MASK   = BLFLAG_FLEX_VALID | BLFLAG_HAS_FLEXIBLE_CHILD;
const unsigned BLFLAG_DEPENDENCY_MASK   = BLFLAG_HAS_FLEXIBLE_CHILD | axismask(AXISFLAG_DEPENDS_MASK | AXISFLAG_HAS_DEPENDENT_CHILD);
const unsigned BLFLAG_BOUNDS_VALID_MASK = BLFLAG_CHILD_BOUNDS_VALID | BLFLAG_TREE_BOUNDS_VALID;

enum GrowthDirection { GDIR_GROW, GDIR_SHRINK };

struct BoxAxis {
	unsigned mode_dim          : 3;
	unsigned mode_min          : 3;
	unsigned mode_max          : 3;
	unsigned mode_pad_lower    : 3;
	unsigned mode_pad_upper    : 3;
	unsigned mode_margin_lower : 3;
	unsigned mode_margin_upper : 3;
	unsigned mode_growth       : 2; // AXIS_H -> grow, AXIS_V -> shrink.
	float sizes[NUM_SIZE_SLOTS];
	float pos;
	float pad_lower;
	float pad_upper;
	float margin_lower;
	float margin_upper;
	float min;
	float max;
};

struct Box {
	Tree t;

	unsigned layout_flags;

	unsigned cell_code;
	Box *cell_prev;
	Box *cell_next;

	Box *clip_ancestor;
	float clip[4];
	float growth[2];

	BoxAxis axes[2];

	uint32_t visibility_stamp;
	uint32_t mouse_hit_stamp;
	uint16_t depth_interval;
	uint16_t depth;	

	struct VisualLayer *layers;

	unsigned line_number;
	unsigned first_element;
	unsigned last_element;

#if defined(STACKER_DIAGNOSTICS)
	char debug_info[64];
	unsigned debug_stamp;
#endif
};

Axis box_axis(const Box *box) ;
Alignment box_arrangement(const Box *box);
Alignment box_alignment(const Box *box);
BoundingBox box_clip_box(const Box *box);
Box *tree_box(Tree *tree);
bool sizes_equal(float a, float b);
bool size_valid(const Box *box, SizeSlot slot, Axis axis);
void validate_size(Box *box, SizeSlot slot, Axis axis);
float get_size(const Box *box, Axis axis);
float get_size(const Box *box, SizeSlot slot, Axis axis);
float get_slot(const Box *box, Axis axis);
float get_slot(const Box *box, SizeSlot slot, Axis axis);
void set_slot(Box *box, SizeSlot slot, Axis axis, float new_size);
bool set_size(Box *box, SizeSlot slot, Axis axis, float new_size);
float padded_dim(const Box *box, Axis axis);
float outer_dim(const Box *box, Axis axis);
float content_edge_lower(const Box *box, Axis axis);
float content_edge_upper(const Box *box, Axis axis);
float padding_edge_lower(const Box *box, Axis axis);
float padding_edge_upper(const Box *box, Axis axis);
float outer_edge_lower(const Box *box, Axis axis);
float outer_edge_upper(const Box *box, Axis axis);
float padding(const Box *box, Axis axis);
float margins(const Box *box, Axis axis);
float padding_and_margins(const Box *box, Axis axis);
void content_rectangle(const Box *box, float *r);
void padding_rectangle(const Box *box, float *r);
void outer_rectangle(const Box *box, float *r);
void content_rectangle(const Box *box, float *x0, float *x1, float *y0, float *y1);
void padding_rectangle(const Box *box, float *x0, float *x1, float *y0, float *y1);
void outer_rectangle(const Box *box, float *x0, float *x1, float *y0, float *y1);
void hit_rectangle(const Box *box, float *x0, float *x1, float *y0, float *y1);
unsigned edge_set_to_box_clip_flags(unsigned edges);
void bounding_box_rectangle(const Box *box, BoundingBox bbox, float *bounds);
void build_clip_rectangle(const Box *box, float *r);
unsigned box_tree_depth(const Box *box);
bool better_anchor(float x, float y, const Box *a, const Box *b);
void depth_sort_boxes(const Box **boxes, unsigned count);
bool is_mouse_over(const Document *document, const Box *box);
bool box_is_visible(const Document *document, const Box *box, const View *view);
bool box_is_visible(const Document *document, const Box *box);
void box_advise_visible(Document *document, Box *box, const View *view);
int last_paragraph_element(const Document *document, const Box *box);

Box *create_box(Document *document, Node *owner);
void destroy_box_internal(Document *document, Box *box);
void destroy_box_tree(Document *document, Box *box);
void remove_and_destroy_box(Document *document, Box *box);
void remove_and_destroy_siblings(Document *document, Box *first);
void destroy_box(Document *document, Box *box, bool destroy_children);
void configure_container_box(Document *document, Node *node, Axis axis, Box *box);
Box *build_line_box(Document *document, Node *node, Justification justification, unsigned line_number);
Box *build_text_box(Document *document, Node *owner, const char *text, unsigned text_length);

void remove_from_parent(Document *document, Box *box);
void append_child(Document *document, Box *parent, Box *child);
void insert_child_before(Document *document, Box *parent, Box *child, Box *before);
void remove_all_children(Document *document, Box *parent);
void clear_box_tree_flags(Document *document, Box *box, unsigned mask);

} // namespace stkr
