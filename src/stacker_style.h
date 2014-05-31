#pragma once

#include <cstdint>

namespace stkr {

/* Node style flags. */
enum StyleFlag {
	STYLE_BOLD      = 1 << 0,
	STYLE_ITALIC    = 1 << 1,
	STYLE_UNDERLINE = 1 << 2,
	STYLE_ENABLED   = 1 << 3
};

const unsigned FONT_STYLE_MASK = STYLE_BOLD | STYLE_ITALIC | STYLE_UNDERLINE;

/* Flags present in only in the flags field of style segments. */
enum SegmentStyleFlag {
	SSF_SELECTED  = 1 << 6, // The segment is part of the mouse selection.
	SSF_REMEASURE = 1 << 7  // The font has changed.
};

/* The subset of style information required to measure and draw text runs. */
struct TextStyle {
	uint32_t key;
	int16_t font_id;
	uint16_t flags;
	uint32_t color;
	uint32_t tint;
};

/* Information required to render text and graphics associated with a node. */
struct NodeStyle {
	uint16_t flags            : 10;
	uint16_t justification    : 2;
	uint16_t white_space_mode : 2;
	uint16_t wrap_mode        : 2;
	TextStyle text;
	short hanging_indent;
	short leading;
};

/* Layout operations necessitated by the change in a pair of styles. */
enum StyleCompareFlag {
	STYLECMP_MUST_RETOKENIZE = 1 << 0,
	STYLECMP_MUST_REMEASURE  = 1 << 1,
	STYLECMP_MUST_REPAINT    = 1 << 2
};

/* These are defined by the back end. */
extern const char * const DEFAULT_FONT_FACE;
extern const unsigned DEFAULT_FONT_SIZE;
extern const unsigned DEFAULT_FONT_FLAGS;
extern const char * const DEFAULT_FIXED_FONT_FACE;
extern const unsigned DEFAULT_FIXED_FONT_SIZE;
extern const unsigned DEFAULT_FIXED_FONT_FLAGS;
extern const char * const DEBUG_LABEL_FONT_FACE;
extern const unsigned DEBUG_LABEL_FONT_SIZE;
extern const unsigned DEBUG_LABEL_FONT_FLAGS;

const uint32_t DEFAULT_TEXT_COLOR               = 0xFF000000;
const uint32_t DEFAULT_SELECTED_TEXT_COLOR      = 0xFFFFFFFF;
const uint32_t DEFAULT_SELECTED_TEXT_FILL_COLOR = 0xC0FF00EA;
const uint32_t DEFAULT_LINK_COLOR               = 0xFFDE409C;
const uint32_t DEFAULT_HIGHLIGHTED_LINK_COLOR   = 0xFFFCBBE1;
const uint32_t DEFAULT_ACTIVE_LINK_COLOR        = 0xFF0FFABB;

extern const NodeStyle DEFAULT_NODE_STYLE;

void update_text_style_key(TextStyle *style);
unsigned compare_styles(const NodeStyle *a, const NodeStyle *b);

} // namespace stkr
