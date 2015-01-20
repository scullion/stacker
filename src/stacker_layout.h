#pragma once

#include <cstdint>

#include "stacker_tree.h"
#include "stacker_inline2.h"
#include "stacker_paragraph.h"

namespace stkr {

struct Document;
struct Box;
enum Axis;
enum DimensionMode;

/* Incremental layout high-level passes. */
enum LayoutStage {
	LSTG_UPDATE_INFO,    /* Update dependency flags. */
	LSTG_COMPUTE_SIZES,  /* Multi-pass box sizing. */
	LSTG_COMPUTE_BOUNDS, /* Update box bounds. */
	LSTG_UPDATE_CLIP,    /* Update clip boxes and depth. */
	LSTG_COMPLETE,       /* Layout finished. */

	FIRST_LAYOUT_STAGE = LSTG_UPDATE_INFO
};


enum SizingStage {
	SSTG_EXTRINSIC_MAIN,        /* Start of the maining sizing chain. */
	SSTG_EXTRINSIC,             /* Calculate extrinsic sizes. */
	SSTG_INDEPENDENT_EXTRINSIC, /* Calculate extrinsic sizes. Fail if required intrinsics undefined. */
	SSTG_BREAK_FINAL,           /* Resume final line breaking. */
	SSTG_DO_FLEX,               /* Do flex sizing. */
	SSTG_VISIT_CHILDREN,        /* Prepare to visit children. */

	SSTG_INTRINSIC_MAIN,        /* Start of intrinsic sizing chain. */
	SSTG_TEXT_MEASUREMENT,      /* Resume incremental text measurement. */
	SSTG_BREAK_IDEAL,           /* Resume infinite width line breaking. */
	SSTG_INLINE_BOX_UPDATE,     /* Resume inline box update. */

	SSTG_COMPLETE               /* Done. */
};


/* State used during incremental box layout. */
struct IncrementalLayoutState {
	LayoutStage layout_stage;
	TreeIterator iterator;
	TextMeasurementState measurement_state;
	IncrementalBreakState break_state;
	InlineBoxUpdateState box_update_state;
	Box *box;
};

void init_layout(IncrementalLayoutState *s);
void deinit_layout(IncrementalLayoutState *s);
void begin_layout(IncrementalLayoutState *s, Document *document, Box *root,
	uint8_t *scratch_buffer = 0, unsigned buffer_size = 0);
bool continue_layout(IncrementalLayoutState *s, Document *document);
void clear_flags(Document *document, Box *box, unsigned to_clear,
	unsigned cleared_in_children = 0);
void clear_flags(Document *document, Box *box, Axis axis, 
	unsigned to_clear, unsigned cleared_in_children = 0);
bool set_ideal_size(Document *document, Box *box, Axis axis, 
	DimensionMode mode, float dim = 0.0f);
bool size_depends_on_parent(const Box *box, Axis axis);
bool size_depends_on_parent(const Box *box);

};
