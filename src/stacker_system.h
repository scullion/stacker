#pragma once

#include <cstdint>

#include "stacker_rule.h"
#include "url_cache.h"

namespace stkr {

struct BackEnd;

const unsigned MAX_FONT_FACE_LENGTH = 31;

/* A description of a font. Used as input to the font selector. */
struct LogicalFont {
	char face[MAX_FONT_FACE_LENGTH + 1];
	int font_size;
	uint16_t flags;
};

struct FontMetrics {
	unsigned height;
	unsigned em_width;
	unsigned space_width;
	unsigned space_stretch;
	unsigned space_shrink;
	unsigned paragraph_indent_width;
};

const unsigned MAX_CACHED_FONTS = 32;
const int16_t  INVALID_FONT_ID  = -1;
const unsigned MAX_USER_POINTERS = 32;

/* Remembers the system font handle returned by the matcher for a particular
 * logical font. */
struct CachedFont {
	uint32_t key;
	void *handle;
	FontMetrics metrics;
	LogicalFont descriptor;
};

struct System {
	unsigned flags;
	BackEnd *back_end;
	
	/* Font handling. */
	CachedFont font_cache[MAX_CACHED_FONTS];
	unsigned font_cache_entries;
	LogicalFont default_font_descriptor;
	int16_t default_font_id;
	int16_t debug_label_font_id;
	
	/* Rules. */
	RuleTable global_rules;
	unsigned rule_table_revision;
	unsigned rule_revision_counter;
	uint64_t rule_name_all;                    // *
	uint64_t rule_name_highlighted;            // :highlighted
	uint64_t rule_name_active;                 // :active
	uint64_t token_rule_names[NUM_KEYWORDS];   // Hashed names of all keywords.

	/* URL cache. */
	urlcache::UrlCache *url_cache;
	int document_notify_id;
	int image_layer_notify_id;

	/* Diagnostics. */
	unsigned total_nodes;
	unsigned total_boxes;
};

int16_t get_font_id(System *system, const LogicalFont *logfont);
void *get_font_handle(System *system, int16_t font_id);
const FontMetrics *get_font_metrics(System *system, int16_t font_id);
const LogicalFont *get_font_descriptor(System *system, int16_t font_id);
void measure_text(System *system, int16_t font_id, 
	const char *text, unsigned text_length, 
	unsigned *width, unsigned *height, 
	unsigned *character_widths); 
int16_t get_debug_label_font_id(System *system);

} // namespace stkr
