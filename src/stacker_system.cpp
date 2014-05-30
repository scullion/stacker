#include "stacker_system.h"

#include "stacker_shared.h"
#include "stacker_attribute_buffer.h"
#include "stacker_util.h"
#include "stacker_platform.h"
#include "stacker_document.h"
#include "stacker_layer.h"

namespace stkr {

static void make_font_descriptor(LogicalFont *descriptor, const char *face,
	unsigned size, unsigned flags)
{
	if (face != NULL) {
		strncpy(descriptor->face, face, sizeof(descriptor->face));
		descriptor->face[sizeof(descriptor->face) - 1] = '\0';
	} else {
		descriptor->face[0] = '\0';
	}
	descriptor->font_size = size;
	descriptor->flags = (uint16_t)flags;
}
  
static void initialize_font_cache(System *system)
{
	system->default_font_id = INVALID_FONT_ID;
	system->font_cache_entries = 0;
	make_font_descriptor(&system->default_font_descriptor, 
		DEFAULT_FONT_FACE, DEFAULT_FONT_SIZE, DEFAULT_FONT_FLAGS);
	system->default_font_id = get_font_id(system, 
		&system->default_font_descriptor);
	ensure(system->default_font_id != INVALID_FONT_ID);
	system->debug_label_font_id = INVALID_FONT_ID;
}

/* Returns a key uniquely identifying a font specification. */
static uint32_t make_font_key(const LogicalFont *logfont)
{
	uint32_t seed = logfont->font_size | (logfont->flags << 16);
	return murmur3_32(logfont->face, strlen(logfont->face), seed);
}

/* Calculates round(n * (a / b)). */
inline unsigned iscale(unsigned n, unsigned a, unsigned b)
{
	return (n * a + b / 2) / b;
}

/* Precalculates numbers needed for typesetting from the system font metrics. */
static void calculate_derived_font_metrics(FontMetrics *metrics)
{
	/* w = (1/3)em, y = (1/6)em, z = (1/9)em */
	metrics->space_width            = iscale(metrics->em_width, 1000, 3000);
	metrics->space_stretch          = iscale(metrics->em_width, 1000, 6000);
	metrics->space_shrink           = iscale(metrics->em_width, 1000, 9000);
	metrics->paragraph_indent_width = metrics->em_width;
}

/* Returns the ID of a font from the font cache, creating it if necessary. */
int16_t get_font_id(System *system, const LogicalFont *logfont)
{
	uint32_t key = make_font_key(logfont);
	for (unsigned i = 0; i < system->font_cache_entries; ++i)
		if (system->font_cache[i].key == key)
			return (int16_t)i;
 	if (system->font_cache_entries == MAX_CACHED_FONTS)
		return 0;
	void *handle = platform_match_font(system->back_end, logfont);
	if (handle == NULL)
		return system->default_font_id;
	CachedFont *cf = system->font_cache + system->font_cache_entries;
	cf->key = key;
	cf->handle = handle;
	cf->descriptor = *logfont;
	platform_font_metrics(system->back_end, handle, &cf->metrics);
	calculate_derived_font_metrics(&cf->metrics);
	return int16_t(system->font_cache_entries++);
}

/* Returns the system handle for a cached font. */
void *get_font_handle(System *system, int16_t font_id)
{
	assertb(unsigned(font_id) < system->font_cache_entries);
	return system->font_cache[font_id].handle;
}

/* Returns the logical font used to create a font ID. */
const LogicalFont *get_font_descriptor(System *system, int16_t font_id)
{
	if (font_id != INVALID_FONT_ID) {
		assertb(unsigned(font_id) < system->font_cache_entries);
		return &system->font_cache[font_id].descriptor;
	}
	return &system->default_font_descriptor;
}

const FontMetrics *get_font_metrics(System *system, int16_t font_id)
{
	assertb(unsigned(font_id) < system->font_cache_entries);
	return &system->font_cache[font_id].metrics;
}

void measure_text(System *system, int16_t font_id, 
	const char *text, unsigned text_length, unsigned *width, unsigned *height,
	unsigned *character_widths)
{
	void *font_handle = get_font_handle(system, font_id);
	platform_measure_text(system->back_end, font_handle, text, text_length, 
		width, height, character_widths);
}

/* Precomputes hashed rule names for tag tokens and pseudo classes. */
static void make_built_in_rule_names(System *system)
{
	system->rule_name_all         = murmur3_64_cstr("*");
	system->rule_name_active      = murmur3_64_cstr(":active");
	system->rule_name_highlighted = murmur3_64_cstr(":highlighted");
	for (unsigned i = 0; i < NUM_KEYWORDS; ++i) {
		system->token_rule_names[i] = murmur3_64_cstr(
			TOKEN_STRINGS[TOKEN_KEYWORD_FIRST + i]);
	}
}

static AttributeAssignment make_assignment(Token name, int value, 
	ValueSemantic vs = VSEM_NONE, AttributeOperator op = AOP_SET)
{
	AttributeAssignment assignment;
	assignment.name = name;
	assignment.op = op;
	variant_set_integer(&assignment.value, value, vs);
	return assignment;
}

static AttributeAssignment make_assignment(Token name, unsigned value, 
	ValueSemantic vs = VSEM_NONE, AttributeOperator op = AOP_SET)
{
	AttributeAssignment assignment;
	assignment.name = name;
	assignment.op = op;
	variant_set_integer(&assignment.value, value, vs);
	return assignment;
}

static AttributeAssignment make_assignment(Token name, float value, 
	ValueSemantic vs = VSEM_NONE, AttributeOperator op = AOP_SET)
{
	AttributeAssignment assignment;
	assignment.name = name;
	assignment.op = op;
	variant_set_float(&assignment.value, value, vs);
	return assignment;
}

static AttributeAssignment make_assignment(Token name, const char *value,
	ValueSemantic vs = VSEM_NONE, AttributeOperator op = AOP_SET)
{
	AttributeAssignment assignment;
	assignment.name = name;
	assignment.op = op;
	variant_set_string(&assignment.value, value, vs);
	return assignment;
}

static void add_default_rules(System *system)
{
	static const unsigned MAX_ROOT_ATTRIBUTES = 32;

	AttributeAssignment attributes[MAX_ROOT_ATTRIBUTES];
	unsigned count = 0;
	
	attributes[count++] = make_assignment(TOKEN_COLOR, DEFAULT_TEXT_COLOR);
	attributes[count++] = make_assignment(TOKEN_FONT, DEFAULT_FONT_FACE);
	attributes[count++] = make_assignment(TOKEN_FONT_SIZE, DEFAULT_FONT_SIZE);
	attributes[count++] = make_assignment(TOKEN_BOLD, 
		(DEFAULT_FONT_FLAGS & STYLE_BOLD) != 0, VSEM_BOOLEAN);
	attributes[count++] = make_assignment(TOKEN_ITALIC, 
		(DEFAULT_FONT_FLAGS & TOKEN_ITALIC) != 0, VSEM_BOOLEAN);
	attributes[count++] = make_assignment(TOKEN_UNDERLINE, 
		(DEFAULT_FONT_FLAGS & STYLE_UNDERLINE) != 0, VSEM_BOOLEAN);
	attributes[count++] = make_assignment(TOKEN_INDENT, TOKEN_AUTO, VSEM_TOKEN);
	attributes[count++] = make_assignment(TOKEN_JUSTIFY, TOKEN_LEFT, VSEM_TOKEN);
	attributes[count++] = make_assignment(TOKEN_WRAP, TOKEN_WORD_WRAP, VSEM_TOKEN);
	attributes[count++] = make_assignment(TOKEN_WHITE_SPACE, TOKEN_NORMAL, VSEM_TOKEN);

	add_rule(NULL, system, NULL, "document", -1, attributes, count, 
		RFLAG_ENABLED | RFLAG_GLOBAL, RULE_PRIORITY_LOWEST);
}

static void initialize_url_notifications(System *system, UrlCache *url_cache)
{
	if (url_cache != NULL) {
		system->image_layer_notify_id = url_cache->add_notify_sink(
			(urlcache::NotifyCallback)&image_layer_notify_callback, system);
		system->document_notify_id = url_cache->add_notify_sink(
			(urlcache::NotifyCallback)&document_fetch_notify_callback, system);
	} else {
		system->image_layer_notify_id = urlcache::INVALID_NOTIFY_SINK_ID;
	}
}

int16_t get_debug_label_font_id(System *system)
{
	if (system->debug_label_font_id == INVALID_FONT_ID) {
		LogicalFont descriptor;
		make_font_descriptor(&descriptor, DEBUG_LABEL_FONT_FACE, 
			DEBUG_LABEL_FONT_SIZE, DEBUG_LABEL_FONT_FLAGS);
		system->debug_label_font_id = get_font_id(system, &descriptor);
	}
	return system->debug_label_font_id;
}

System *create_system(unsigned flags, BackEnd *back_end, UrlCache *url_cache)
{
	System *system = new System();
	system->flags = flags;
	system->back_end = back_end;
	system->url_cache = url_cache;
	system->rule_table_revision = 0;
	system->rule_revision_counter = 0;
	system->total_boxes = 0;
	system->total_nodes = 0;
	initialize_font_cache(system);
	make_built_in_rule_names(system);
	initialize_url_notifications(system, url_cache);
	add_default_rules(system);
	return system;
}

void destroy_system(System *system)
{
	assertb(system->total_nodes == 0);
	assertb(system->total_boxes == 0);
	clear_rule_table(&system->global_rules);
	for (unsigned i = 0; i < system->font_cache_entries; ++i)
		platform_release_font(system->back_end, system->font_cache[i].handle);
	delete system;
}

BackEnd *get_back_end(System *system)
{
	return system->back_end;
}

unsigned get_total_nodes(const System *system)
{
	return system->total_nodes;
}

unsigned get_total_boxes(const System *system)
{
	return system->total_boxes;
}

} // namespace stkr
