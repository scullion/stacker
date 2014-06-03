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
	BOXFLAG_WIDTH_FROM_PARENT_DEFINED  = 1 <<  2, // Width from parent is defined.
	BOXFLAG_HEIGHT_FROM_PARENT_DEFINED = 1 <<  3, // Height from parent is defined.
	BOXFLAG_WIDTH_STABLE               = 1 <<  4, // There's nothing to be gained from visiting this box's horizontal axis.
	BOXFLAG_HEIGHT_STABLE              = 1 <<  5, // There's nothing to be gained from visiting this box's vertical axis.
	BOXFLAG_TREE_SIZE_STABLE           = 1 <<  6, // Both STABLE bits are set on all boxes in this subtree.

	BOXFLAG_WIDTH_DEPENDS_ON_PARENT    = 1 <<  7, // Box's width may change when either parent dimension changes.
	BOXFLAG_HEIGHT_DEPENDS_ON_PARENT   = 1 <<  8, // Box's height may change when either parent dimension changes.
	BOXFLAG_WIDTH_DEPENDS_ON_CHILDREN  = 1 <<  9, // Box's width may change when any child dimension changes.
	BOXFLAG_HEIGHT_DEPENDS_ON_CHILDREN = 1 << 10, // Box's height may change when any child dimension changes.
	BOXFLAG_WIDTH_SET_BY_PARENT        = 1 << 11, // Provisional width came from above.
	BOXFLAG_HEIGHT_SET_BY_PARENT       = 1 << 12, // Provisional height came from above.

	BOXFLAG_PARAGRAPH_VALID            = 1 << 13, // Box width has not changed since the last paragraph layout.
							           
	BOXFLAG_BOUNDS_DEFINED             = 1 << 14, // The bounds of this box have been set at some time in the past.
	BOXFLAG_CHILD_BOUNDS_VALID         = 1 << 15, // The bounds of the immediate children of this box are up to date.
	BOXFLAG_TREE_BOUNDS_VALID          = 1 << 16, // CHILD_BOUNDS_VALID is set for all boxes in this subtree.

	BOXFLAG_TREE_CLIP_VALID            = 1 << 17, // The depths of all recursive children are valid.
	
	BOXFLAG_HIT_TEST                   = 1 << 18, // Mouse events within the box generate interaction events for the owning node.
	BOXFLAG_HIT_OUTER                  = 1 << 19, // Count the margin as part of the box for the purposes of hit testing.
	BOXFLAG_SELECTION_ANCHOR           = 1 << 20, // The box can be the anchor that determines the extent of a mouse selection.
	BOXFLAG_NO_LABEL                   = 1 << 21, // Don't draw debug labels for this box.

	BOXFLAG_CLIP_LEFT                  = 1 << 22, // Don't draw pixels left of the box's left edge.
	BOXFLAG_CLIP_RIGHT                 = 1 << 23, // Don't draw pixels right of the box's right edge.
	BOXFLAG_CLIP_TOP                   = 1 << 24, // Don't draw pixels above the box's top edge.
	BOXFLAG_CLIP_BOTTOM                = 1 << 25  // Don't draw pixels below the box's bottom edge.
};

const unsigned BOXFLAG_DEFINED_MASK              = BOXFLAG_WIDTH_DEFINED | BOXFLAG_HEIGHT_DEFINED;
const unsigned BOXFLAG_DEFINED_PARENT_MASK       = BOXFLAG_WIDTH_FROM_PARENT_DEFINED | BOXFLAG_HEIGHT_FROM_PARENT_DEFINED;
const unsigned BOXFLAG_STABLE_MASK               = BOXFLAG_WIDTH_STABLE | BOXFLAG_HEIGHT_STABLE;
const unsigned BOXFLAG_DEPENDS_MASK_WIDTH        = BOXFLAG_WIDTH_DEPENDS_ON_CHILDREN | BOXFLAG_WIDTH_DEPENDS_ON_PARENT;
const unsigned BOXFLAG_DEPENDS_MASK_HEIGHT       = BOXFLAG_HEIGHT_DEPENDS_ON_CHILDREN | BOXFLAG_HEIGHT_DEPENDS_ON_PARENT;
const unsigned BOXFLAG_DEPENDS_ON_PARENT_MASK    = BOXFLAG_WIDTH_DEPENDS_ON_PARENT | BOXFLAG_HEIGHT_DEPENDS_ON_PARENT;
const unsigned BOXFLAG_DEPENDS_ON_CHILDREN_MASK  = BOXFLAG_WIDTH_DEPENDS_ON_CHILDREN | BOXFLAG_HEIGHT_DEPENDS_ON_CHILDREN;
const unsigned BOXFLAG_SET_BY_PARENT_MASK        = BOXFLAG_WIDTH_SET_BY_PARENT | BOXFLAG_HEIGHT_SET_BY_PARENT;
const unsigned BOXFLAG_LAYOUT_MASK               = BOXFLAG_STABLE_MASK | BOXFLAG_TREE_SIZE_STABLE | 
                                                   BOXFLAG_BOUNDS_DEFINED | BOXFLAG_CHILD_BOUNDS_VALID | 
                                                   BOXFLAG_TREE_BOUNDS_VALID;

const unsigned BOXFLAG_CLIP_X  = BOXFLAG_CLIP_LEFT | BOXFLAG_CLIP_RIGHT;
const unsigned BOXFLAG_CLIP_Y  = BOXFLAG_CLIP_TOP | BOXFLAG_CLIP_BOTTOM;
const unsigned BOXFLAG_CLIP_ALL= BOXFLAG_CLIP_X | BOXFLAG_CLIP_Y;

/* Whether a provisional size is being set by the box's parent, from the sum
 * of its children, or is an ideal size. */
enum ProvisionalSizeSource { PSS_ABOVE, PSS_BELOW, PSS_IDEAL };

struct Box {
	struct Node *owner;

	Box *parent;
	Box *first_child;
	Box *last_child;
	Box *prev_sibling;
	Box *next_sibling;
	Box *owner_next;

	float ideal[2];
	float size[2];
	float size_from_parent[2];
	float pos[2];
	float pad_lower[2];
	float pad_upper[2];
	float margin_lower[2];
	float margin_upper[2];
	float min[2];
	float max[2];
	float clip[4];
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

	uint32_t flags;
	uint32_t size_stamp[2];
	uint32_t mouse_hit_stamp;
	uint32_t token_start;
	uint32_t token_end;
	uint16_t depth_interval;
	uint16_t depth;
	
	struct GridCell *cell;
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
Box *build_block_box(Document *document, Node *node, Axis axis);
Box *build_line_box(Document *document, Node *node, 
	Justification justification);
Box *build_text_box(Document *document, Node *owner, 
	const char *text, unsigned text_length);

void remove_from_parent(Document *document, Box *box);
void append_child(Document *document, Box *parent, Box *child);
void remove_all_children(Document *document, Box *parent);

void set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim = 0.0f);
bool set_provisional_size(Document *document, Box *box, Axis axis, 
	float dim, ProvisionalSizeSource source = PSS_BELOW,
	bool mark_unstable = true, bool post_text_layout = false);
bool compute_box_sizes(Document *document, Box *box, bool post_text_layout);
void compute_box_bounds(Document *document, Box *box);
void clear_box_tree_flags(Document *document, Box *box, unsigned mask);
void update_box_clip(Document *document, Box *box, 
	const float *parent_clip, int depth, bool must_update = false);

} // namespace stkr
