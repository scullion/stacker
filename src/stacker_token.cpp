#include "stacker_token.h"

#include <cstddef>
#include <cstring>

#include "stacker_shared.h"

namespace stkr {

extern const char * const TOKEN_STRINGS[NUM_TOKENS] = {
	"invalid-token",
	"eof",
	"text",
	"blank",
	"break",
	"<",
	">",
	"</",
	"/>",
	"(",
	")",
	"=",
	"+=",
	"-=",
	"*=",
	"/=",
	":=",
	",",
	"boolean",
	"integer",
	"float",
	"perecentage",
	"string",
	"color-literal",
	"url-literal",

	/* Tag Names. */
	"document",
	"hbox",
	"vbox",
	"rule",
	"p",
	"code",
	"h1",
	"h2",
	"h3",
	"a",
	"img",

	/* Attributes. */
	"match",
	"class",
	"global",
	"width",
	"height",
	"min-width",
	"min-height",
	"max-width",
	"max-height",
	"grow",
	"shrink",
	"pad",
	"pad-left",
	"pad-right",
	"pad-top",
	"pad-bottom",
	"margin",
	"margin-left",
	"margin-right",
	"margin-top",
	"margin-bottom",
	"arrange",
	"align",
	"justify",
	"leading",
	"indent",
	"color",
	"selection-color",
	"selection-fill-color",
	"url",
	"layout",
	"font",
	"font-size",
	"bold",
	"italic",
	"underline",
	"white-space",
	"wrap",
	"background",
	"background-color",
	"background-width",
	"background-height",
	"background-size",
	"background-offset-x",
	"background-offset-y",
	"background-horizontal-alignment",
	"background-vertical-alignment",
	"background-box",
	"border-color",
	"border-width",
	"tint",
	"clip",
	"clip-left",
	"clip-right",
	"clip-top",
	"clip-bottom",
	"clip-box",
	"cursor",
	"enabled",

	/* Shared attribute keywords. */
	"undefined",
	"none",
	"all",
	"auto",
	"default",
	"false",
	"true",
	"left",
	"right",
	"top",
	"bottom",
	"horizontal",
	"vertical",
	"rgb",
	"rgba",
	"alpha",

	/* Layout modes. */
	"block",
	"inline",
	"inline-container",

	/* Aligment and arrangement. */
	"start",
	"middle",
	"end",

	/* Justification. */
	"center",
	"flush",

	/* Special background sizing modes. */
	"fit",
	"fill",

	/* Background placement. */
	"content-box",
	"padding-box",
	"margin-box",

	/* Pane types. */
	"flat",
	"sunken",
	"raised",

	/* White space modes. */
	"normal",
	"preserve",

	/* Word wrap modes. */
	"word-wrap",
	"character-wrap",

	/* Cursor types. */
	"hand",
	"caret",
	"crosshair",
	"move",
	"size-ns",
	"size-we",
	"wait",
};

bool is_keyword(int token)
{
	return token >= TOKEN_KEYWORD_FIRST && token < TOKEN_KEYWORD_LAST;
}

int find_keyword(const char *s, unsigned length)
{
	length;
	for (unsigned i = TOKEN_KEYWORD_FIRST; i != TOKEN_KEYWORD_LAST; ++i) {
		const char *ts = TOKEN_STRINGS[i];
		assertb(ts != NULL);
		if (0 == strcmp(ts, s))
			return (int)i;
	}
	return TOKEN_INVALID;
}

/* True if 'token' is the value for a multiple choice attribute semantic. */
bool is_enum_token(int token)
{
	switch (token) {
		case TOKEN_UNDEFINED:
		case TOKEN_NONE:
		case TOKEN_ALL:
		case TOKEN_AUTO:
		case TOKEN_DEFAULT:
		case TOKEN_START:
		case TOKEN_MIDDLE:
		case TOKEN_END:
		case TOKEN_LEFT:
		case TOKEN_RIGHT:
		case TOKEN_TOP:
		case TOKEN_BOTTOM:
		case TOKEN_HORIZONTAL:
		case TOKEN_VERTICAL:
		case TOKEN_CENTER:
		case TOKEN_FLUSH:
		case TOKEN_FLAT:
		case TOKEN_SUNKEN:
		case TOKEN_RAISED:
		case TOKEN_NORMAL:
		case TOKEN_PRESERVE:
		case TOKEN_FIT:
		case TOKEN_FILL:
		case TOKEN_CONTENT_BOX:
		case TOKEN_PADDING_BOX:
		case TOKEN_MARGIN_BOX:
		case TOKEN_BLOCK:
		case TOKEN_INLINE:
		case TOKEN_INLINE_CONTAINER:
		case TOKEN_WORD_WRAP:
		case TOKEN_CHARACTER_WRAP:
		case TOKEN_CURSOR_HAND:
		case TOKEN_CURSOR_CARET:
		case TOKEN_CURSOR_CROSSHAIR:
		case TOKEN_CURSOR_MOVE:
		case TOKEN_CURSOR_SIZE_NS:
		case TOKEN_CURSOR_SIZE_EW:
		case TOKEN_CURSOR_WAIT:
			return true;
		default:
			return false;
	}
}

/* True if a change to the specified attribute should cause a node's background 
 * layers to be updated. */
bool is_background_attribute(int token)
{
	switch (token) {
		case TOKEN_CLIP:
		case TOKEN_CLIP_LEFT:
		case TOKEN_CLIP_RIGHT:
		case TOKEN_CLIP_TOP:
		case TOKEN_CLIP_BOTTOM:
		case TOKEN_CLIP_BOX:
		case TOKEN_BACKGROUND:
		case TOKEN_BACKGROUND_COLOR:
		case TOKEN_BACKGROUND_WIDTH:
		case TOKEN_BACKGROUND_HEIGHT:
		case TOKEN_BACKGROUND_SIZE:
		case TOKEN_BACKGROUND_OFFSET_X:
		case TOKEN_BACKGROUND_OFFSET_Y:
		case TOKEN_BACKGROUND_HORIZONTAL_ALIGNMENT:
		case TOKEN_BACKGROUND_VERTICAL_ALIGNMENT:
		case TOKEN_BACKGROUND_BOX:
		case TOKEN_BORDER_COLOR:
		case TOKEN_BORDER_WIDTH:
		case TOKEN_TINT:
			return true;
	}
	return false;
}

/* True if a change to the specified attribute should cause a node's cascaded
 * style to be updated. */
bool is_cascaded_style_attribute(int token)
{
	switch (token) {
		case TOKEN_ARRANGE:
		case TOKEN_ALIGN:
		case TOKEN_JUSTIFY:
		case TOKEN_LEADING:
		case TOKEN_INDENT:
		case TOKEN_COLOR:
		case TOKEN_SELECTION_COLOR:
		case TOKEN_SELECTION_FILL_COLOR:
		case TOKEN_FONT:
		case TOKEN_FONT_SIZE:
		case TOKEN_BOLD:
		case TOKEN_ITALIC:
		case TOKEN_UNDERLINE:
		case TOKEN_WHITE_SPACE:
		case TOKEN_WRAP:
		case TOKEN_TINT:
		case TOKEN_ENABLED:
			return true;
	}
	return false;
}

/* True if a change to the specified attribute should cause a node's boxes
 * to be rebuilt. */
bool is_layout_attribute(int token)
{
	switch (token) {
		case TOKEN_WIDTH:
		case TOKEN_HEIGHT:
		case TOKEN_MIN_WIDTH:
		case TOKEN_MIN_HEIGHT:
		case TOKEN_MAX_WIDTH:
		case TOKEN_MAX_HEIGHT:
		case TOKEN_GROW:
		case TOKEN_SHRINK:
		case TOKEN_PADDING:
		case TOKEN_PADDING_LEFT:
		case TOKEN_PADDING_RIGHT:
		case TOKEN_PADDING_TOP:
		case TOKEN_PADDING_BOTTOM:
		case TOKEN_MARGIN:
		case TOKEN_MARGIN_LEFT:
		case TOKEN_MARGIN_RIGHT:
		case TOKEN_MARGIN_TOP:
		case TOKEN_MARGIN_BOTTOM:
		case TOKEN_LAYOUT:
			return true;
	}
	return false;
}

} // namespace stkr
