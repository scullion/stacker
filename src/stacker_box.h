#pragma once

#include <cstdint>

namespace stkr {

struct Document;
enum Axis;
enum DimensionMode;
enum BoundingBox;
enum Justification;

enum BoxFlag {
	BOXFLAG_WIDTH_DEFINED              = 1 <<  0, // Width has been calculated in this or a previous tick.
	BOXFLAG_HEIGHT_DEFINED             = 1 <<  1, // Height has been calculated in this or a previous tick.
	BOXFLAG_WIDTH_STABLE               = 1 <<  2, // There's nothing to be gained from visiting this box's horizontal axis.
	BOXFLAG_HEIGHT_STABLE              = 1 <<  3, // There's nothing to be gained from visiting this box's vertical axis.
	BOXFLAG_TREE_SIZE_STABLE           = 1 <<  4, // Both STABLE bits are set on all boxes in this subtree.
	BOXFLAG_WIDTH_FIXED                = 1 <<  5, // Provisional width always equals ideal width and need not be initialized.
	BOXFLAG_HEIGHT_FIXED               = 1 <<  6, // Provisional height always equals ideal height and need not be initialized.
	
	BOXFLAG_WIDTH_ACTIVE_ABOVE         = 1 <<  7, // The "above" half of the width slot is the active provisional size.
	BOXFLAG_HEIGHT_ACTIVE_ABOVE        = 1 <<  8, // The "above" half of the height slot is the active provisional size.
	BOXFLAG_WIDTH_PRIMARY_ABOVE        = 1 <<  9, // The box's width is defined by the "above" half of the width slot.
	BOXFLAG_HEIGHT_PRIMARY_ABOVE       = 1 << 10, // The box's height is defined by the "above" half of the height slot. 

	BOXFLAG_PARAGRAPH_VALID            = 1 << 11, // Box width has not changed since the last paragraph layout.

	BOXFLAG_BOUNDS_DEFINED             = 1 << 12, // The bounds of this box have been set at some time in the past.
	BOXFLAG_CHILD_BOUNDS_VALID         = 1 << 13, // The bounds of the immediate children of this box are up to date.
	BOXFLAG_TREE_BOUNDS_VALID          = 1 << 14, // CHILD_BOUNDS_VALID is set for all boxes in this subtree.

	BOXFLAG_TREE_CLIP_VALID            = 1 << 15, // The depths of all recursive children are valid.
	
	BOXFLAG_HIT_TEST                   = 1 << 16, // Mouse events within the box generate interaction events for the owning node.
	BOXFLAG_HIT_OUTER                  = 1 << 17, // Count the margin as part of the box for the purposes of hit testing.
	BOXFLAG_SELECTION_ANCHOR           = 1 << 18, // The box can be the anchor that determines the extent of a mouse selection.
	BOXFLAG_NO_LABEL                   = 1 << 19, // Don't draw debug labels for this box.

	BOXFLAG_CLIP_LEFT                  = 1 << 20, // Don't draw pixels left of the box's left edge.
	BOXFLAG_CLIP_RIGHT                 = 1 << 21, // Don't draw pixels right of the box's right edge.
	BOXFLAG_CLIP_TOP                   = 1 << 22, // Don't draw pixels above the box's top edge.
	BOXFLAG_CLIP_BOTTOM                = 1 << 23, // Don't draw pixels below the box's bottom edge.

	BOXFLAG_MARK                       = 1 << 31  // Temporary marker.
};

const unsigned BOXFLAG_DEFINED_MASK              = BOXFLAG_WIDTH_DEFINED | BOXFLAG_HEIGHT_DEFINED;
const unsigned BOXFLAG_STABLE_MASK               = BOXFLAG_WIDTH_STABLE | BOXFLAG_HEIGHT_STABLE;
const unsigned BOXFLAG_SET_BY_PARENT_MASK        = BOXFLAG_WIDTH_ACTIVE_ABOVE | BOXFLAG_HEIGHT_ACTIVE_ABOVE;
const unsigned BOXFLAG_LAYOUT_MASK               = BOXFLAG_STABLE_MASK | BOXFLAG_TREE_SIZE_STABLE | 
                                                   BOXFLAG_BOUNDS_DEFINED | BOXFLAG_CHILD_BOUNDS_VALID | 
                                                   BOXFLAG_TREE_BOUNDS_VALID;
const unsigned BOXFLAG_CLIP_X                    = BOXFLAG_CLIP_LEFT | BOXFLAG_CLIP_RIGHT;
const unsigned BOXFLAG_CLIP_Y                    = BOXFLAG_CLIP_TOP | BOXFLAG_CLIP_BOTTOM;
const unsigned BOXFLAG_CLIP_ALL                  = BOXFLAG_CLIP_X | BOXFLAG_CLIP_Y;

/* Per-pass bits that enable particular constraints for a box. */
enum PassFlag {
	PASSFLAG_COMPUTE_WIDTH_FROM_PARENT    = 1 << 0,
	PASSFLAG_COMPUTE_HEIGHT_FROM_PARENT   = 1 << 1,
	PASSFLAG_COMPUTE_WIDTH_FROM_CHILDREN  = 1 << 2,
	PASSFLAG_COMPUTE_HEIGHT_FROM_CHILDREN = 1 << 3
};

const unsigned PASSFLAG_PARENT_MASK   = PASSFLAG_COMPUTE_WIDTH_FROM_PARENT | PASSFLAG_COMPUTE_HEIGHT_FROM_PARENT;
const unsigned PASSFLAG_CHILDREN_MASK = PASSFLAG_COMPUTE_WIDTH_FROM_CHILDREN | PASSFLAG_COMPUTE_HEIGHT_FROM_CHILDREN;
const unsigned PASSFLAG_ALL           = PASSFLAG_PARENT_MASK | PASSFLAG_CHILDREN_MASK;

enum GrowthDirection { GDIR_GROW, GDIR_SHRINK };

enum SizingPass { PASS_PRE_TEXT_LAYOUT, PASS_POST_TEXT_LAYOUT };

struct Box {
	struct Node *owner;

	Box *parent;
	Box *first_child;
	Box *last_child;
	Box *prev_sibling;
	Box *next_sibling;
	Box *owner_next;

	float ideal[2];
	float sizes[2][2];
	float pos[2];
	float pad_lower[2];
	float pad_upper[2];
	float margin_lower[2];
	float margin_upper[2];
	float min[2];
	float max[2];
	float clip[4];
	float growth[2];
	float basis;
	unsigned char axis        : 1;
	unsigned char arrangement : 2;
	unsigned char alignment   : 2;
	unsigned char clip_box    : 2;
	unsigned char mode_dim[2];
	unsigned char mode_min[2];
	unsigned char mode_max[2];
	unsigned char mode_pad_lower[2];
	unsigned char mode_pad_upper[2];
	unsigned char mode_margin_lower[2];
	unsigned char mode_margin_upper[2];
	unsigned char mode_growth[2];

	uint32_t flags;
	uint32_t size_stamp[2];
	uint32_t mouse_hit_stamp;
	uint32_t token_start;
	uint32_t token_end;
	uint16_t depth_interval;
	uint16_t depth;
	uint8_t pass_flags[2];
	
	unsigned cell_code;
	Box *cell_prev;
	Box *cell_next;

	struct VisualLayer *layers;

#if defined(STACKER_DIAGNOSTICS)
	char debug_info[64];
#endif
};

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
void get_bounding_box_rectangle(const Box *box, BoundingBox bbox, 
	float *bounds);
void build_clip_rectangle(const Box *box, float *r);
unsigned box_tree_depth(const Box *box);
bool better_anchor(float x, float y, const Box *a, const Box *b);
void depth_sort_boxes(const Box **boxes, unsigned count);
bool is_mouse_over(const Document *document, const Box *box);
void clear_flag_in_parents(Document *document, Box *box, unsigned mask);

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

float get_size(const Box *box, Axis axis);
float get_size_directional(const Box *box, Axis axis, bool from_parent);
void set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim = 0.0f);
bool compute_box_sizes(Document *document, SizingPass pass, Box *box);
void compute_box_bounds(Document *document, Box *box, bool parent_valid = true);
void clear_box_tree_flags(Document *document, Box *box, unsigned mask);
void update_box_clip(Document *document, Box *box, 
	const float *parent_clip, int depth, bool must_update = false);

} // namespace stkr
