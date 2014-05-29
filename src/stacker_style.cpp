#include "stacker_style.h"

#include "stacker.h"
#include "stacker_util.h"
#include "stacker_system.h"

namespace stkr {

extern const NodeStyle DEFAULT_NODE_STYLE = {
	STYLE_ENABLED,                            // flags
	JUSTIFY_FLUSH,                            // justification
	WSM_NORMAL,                               // white_space_mode
	WRAPMODE_WORD,                            // wrap_mode
	{ 
		0,                                    // key
		INVALID_FONT_ID,                      // font_id
		0,                                    // flags
		0xFF000000,                           // color
		0xFFFFFFFF                            // tint
	},
	0,                                        // hanging_indent 
	0                                         // leading
};

/* Makes a unique key identifying a (font, colour) combination. These are used
 * to bucket characters that can be drawn together. Collisions aren't 
 * catastrophic. */
void update_text_style_key(TextStyle *style)
{
	style->key = 0;
	style->key = murmur3_32(style, sizeof(TextStyle));
}

/* Returns a mask summarizing the differences between two style objects. */
unsigned compare_styles(const NodeStyle *a, const NodeStyle *b)
{
	unsigned result = 0, changed = a->flags ^ b->flags;

	if (a->white_space_mode != b->white_space_mode ||
		a->wrap_mode        != b->wrap_mode) {
		result |= STYLECMP_MUST_RETOKENIZE | STYLECMP_MUST_REMEASURE | 
			STYLECMP_MUST_REPAINT;
	}
	if ((changed & FONT_STYLE_MASK) != 0 ||
	    a->justification    != b->justification ||
		a->hanging_indent   != b->hanging_indent ||
		a->leading          != b->leading ||
		a->text.font_id     != b->text.font_id) {
		result |= STYLECMP_MUST_REMEASURE | STYLECMP_MUST_REPAINT;
	}
	if (a->text.color != b->text.color ||
	    a->text.tint  != b->text.tint) {
		result |= STYLECMP_MUST_REPAINT;
	}
	return result;
}

} // namespace stkr
