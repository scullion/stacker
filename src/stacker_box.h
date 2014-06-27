#pragma once

#include <cstdint>

#include "stacker.h"

namespace stkr {

struct Document;
enum DimensionMode;
enum BoundingBox;
enum Justification;

enum AxisFlag {
	AXISFLAG_EXTRINSIC_VALID               = 1 << 0, // Extrinsic size is valid.
	AXISFLAG_INTRINSIC_VALID               = 1 << 1, // Intrinsic size is valid.
	AXISFLAG_PREFERRED_VALID               = 1 << 2, // Preferred size is valid.
	AXISFLAG_IDEAL_VALID                   = 1 << 3, // Ideal size is valid.
	AXISFLAG_DEPENDS_ON_PARENT             = 1 << 4, // This axis changes when the corresponding parent axis changes.
	AXISFLAG_DEPENDS_ON_CHILDREN           = 1 << 5, // When the size of a child changes, the size of this axis changes.
	AXISFLAG_CHILD_SIZES_MAY_BE_VALID      = 1 << 6, // Child sizes have not been invalidated by a change in parent size (but may still be individually invalid).
	AXISFLAG_HAS_DEPENDENT_CHILD           = 1 << 7, // When the size of this axis changes, the sizes of one or more immediate children change.
	AXISFLAG_HAS_DEPENDENT_ANCESTOR        = 1 << 8  // When the size of this axis changes, the sizes of one or more children in this subtree change.
};
const unsigned NUM_AXIS_FLAGS = 9; 

const unsigned AXISFLAG_DEPENDS_MASK = AXISFLAG_DEPENDS_ON_PARENT | AXISFLAG_DEPENDS_ON_CHILDREN;
const unsigned AXISFLAG_HAS_DEPENDENT_MASK = AXISFLAG_HAS_DEPENDENT_CHILD | AXISFLAG_HAS_DEPENDENT_ANCESTOR;
const unsigned AXISFLAG_SIZES_VALID_MASK = AXISFLAG_EXTRINSIC_VALID | AXISFLAG_INTRINSIC_VALID;
const unsigned AXISFLAG_ALL_VALID_MASK = AXISFLAG_SIZES_VALID_MASK | AXISFLAG_PREFERRED_VALID;

enum BoxFlag {
	BOXFLAG_HIT_TEST                           = 1 << 0, // Mouse events within the box generate interaction events for the owning node.
	BOXFLAG_HIT_OUTER                          = 1 << 1, // Count the margin as part of the box for the purposes of hit testing.
	BOXFLAG_SELECTION_ANCHOR                   = 1 << 2, // The box can be the anchor that determines the extent of a mouse selection.
	BOXFLAG_NO_LABEL                           = 1 << 3, // Don't draw debug labels for this box.

	BOXFLAG_CLIP_LEFT                          = 1 << 4, // Don't draw pixels left of the box's left edge.
	BOXFLAG_CLIP_RIGHT                         = 1 << 5, // Don't draw pixels right of the box's right edge.
	BOXFLAG_CLIP_TOP                           = 1 << 6, // Don't draw pixels above the box's top edge.
	BOXFLAG_CLIP_BOTTOM                        = 1 << 7, // Don't draw pixels below the box's bottom edge.
};

const unsigned BOXFLAG_CLIP_X           = BOXFLAG_CLIP_LEFT | BOXFLAG_CLIP_RIGHT;
const unsigned BOXFLAG_CLIP_Y           = BOXFLAG_CLIP_TOP | BOXFLAG_CLIP_BOTTOM;
const unsigned BOXFLAG_CLIP_ALL         = BOXFLAG_CLIP_X | BOXFLAG_CLIP_Y;

enum BoxLayoutFlag {
	BLFLAG_LAYOUT_INFO_VALID                  = 1 <<  0, // Dependency flags are valid.
	BLFLAG_TREE_VALID                         = 1 <<  1, // All children, and all their children, and so on, have valid sizes. Does not imply that THIS node has valid sizes.
	BLFLAG_FLEX_VALID                         = 1 <<  2, // Flexible immediate children have valid final sizes.
	BLFLAG_HAS_FLEXIBLE_CHILD                 = 1 <<  3, // If the size of a child has changed, this box must perform flex adjustment.
	BLFLAG_PROTECT                            = 1 <<  4, // Prevent invalidation.
	
	BLFLAG_PARAGRAPH_VALID                    = 1 <<  5, // Box width has not changed since the last paragraph layout.

	BLFLAG_BOUNDS_DEFINED                     = 1 <<  6, // The bounds of this box have been set at some time in the past.
	BLFLAG_CHILD_BOUNDS_VALID                 = 1 <<  7, // The bounds of the immediate children of this box are up to date.
	BLFLAG_TREE_BOUNDS_VALID                  = 1 <<  8, // CHILD_BOUNDS_VALID is set for all boxes in this subtree.

	BLFLAG_TREE_CLIP_VALID                    = 1 <<  9, // The depths of all recursive children are valid.

	BLFLAG_AXIS_BASE                          = 1 << 10, // Base for per-axis flags.
	BLFLAG_AXIS_H                             = BLFLAG_AXIS_BASE << (AXIS_H * NUM_AXIS_FLAGS),
	BLFLAG_AXIS_V                             = BLFLAG_AXIS_BASE << (AXIS_V * NUM_AXIS_FLAGS)
};

enum SizeSlot { 
	SSLOT_EXTRINSIC, 
	SSLOT_INTRINSIC, 
	SSLOT_PREFERRED,
	SSLOT_IDEAL,
	NUM_SIZE_SLOTS
};

inline unsigned axisflag(Axis axis, unsigned m)    { return (m * BLFLAG_AXIS_BASE) << (axis * NUM_AXIS_FLAGS); }
inline unsigned axismask(unsigned m)               { return axisflag(AXIS_H, m) | axisflag(AXIS_V, m); }
inline unsigned slotflag(SizeSlot slot, Axis axis) { return axisflag(axis, AxisFlag(AXISFLAG_EXTRINSIC_VALID << slot)); } 

const unsigned BLFLAG_FLEX_VALID_MASK   = BLFLAG_FLEX_VALID | BLFLAG_HAS_FLEXIBLE_CHILD;
const unsigned BLFLAG_DEPENDENCY_MASK   = BLFLAG_HAS_FLEXIBLE_CHILD | axismask(AXISFLAG_DEPENDS_MASK | AXISFLAG_HAS_DEPENDENT_MASK);

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
	struct Node *owner;

	Box *parent;
	Box *first_child;
	Box *last_child;
	Box *prev_sibling;
	Box *next_sibling;
	Box *owner_next;

	unsigned layout_flags;
	unsigned axis        :   1;
	unsigned arrangement :   2;
	unsigned alignment   :   2;
	unsigned clip_box    :   2;
	unsigned flags       :  25;

	float clip[4];
	float growth[2];

	BoxAxis axes[2];

	uint32_t mouse_hit_stamp;
	uint32_t token_start;
	uint32_t token_end;
	uint16_t depth_interval;
	uint16_t depth;
	
	unsigned cell_code;
	Box *cell_prev;
	Box *cell_next;

	struct VisualLayer *layers;

#if defined(STACKER_DIAGNOSTICS)
	char debug_info[64];
#endif
};

bool sizes_equal(float a, float b);
bool size_valid(const Box *box, SizeSlot slot, Axis axis);
float get_size(const Box *box, Axis axis);
float get_size(const Box *box, SizeSlot slot, Axis axis);
float get_provisional_size(const Box *box, Axis axis);
float get_provisional_size(const Box *box, SizeSlot slot, Axis axis);
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
void content_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1);
void padding_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1);
void outer_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1);
void hit_rectangle(const Box *box, 
	float *x0, float *x1, float *y0, float *y1);
unsigned edge_set_to_box_clip_flags(unsigned edges);
void bounding_box_rectangle(const Box *box, BoundingBox bbox, 
	float *bounds);
void build_clip_rectangle(const Box *box, float *r);
unsigned box_tree_depth(const Box *box);
bool better_anchor(float x, float y, const Box *a, const Box *b);
void depth_sort_boxes(const Box **boxes, unsigned count);
bool is_mouse_over(const Document *document, const Box *box);

Box *create_box(Document *document, Node *owner);
void destroy_box(Document *document, Box *box, bool destroy_children);
void destroy_sibling_chain(Document *document, Box *first, bool destroy_children);
void destroy_owner_chain(Document *document, Box *first, bool destroy_children);
void configure_container_box(Document *document, Node *node, Axis axis, Box *box);
Box *build_line_box(Document *document, Node *node, 
	Justification justification);
Box *build_text_box(Document *document, Node *owner, 
	const char *text, unsigned text_length);

void remove_from_parent(Document *document, Box *box);
void append_child(Document *document, Box *parent, Box *child);
void remove_all_children(Document *document, Box *parent);
void clear_box_tree_flags(Document *document, Box *box, unsigned mask);
void update_box_clip(Document *document, Box *box, 
	const float *parent_clip, int depth, bool must_update = false);

} // namespace stkr
