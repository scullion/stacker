#include "stacker_node.h"

#include <cstring>

#include <algorithm>

#include "stacker_util.h"
#include "stacker_attribute.h"
#include "stacker_attribute_buffer.h"
#include "stacker_system.h"
#include "stacker_document.h"
#include "stacker_layer.h"
#include "stacker_box.h"
#include "stacker_inline.h"
#include "stacker_rule.h"
#include "stacker_paragraph.h"
#include "stacker.h"
#include "url_cache.h"

namespace stkr {

using namespace urlcache;

extern const char * const NODE_TYPE_STRINGS[NUM_NODE_TYPES] = {
	"basic",
	"text",
	"hbox",
	"vbox",
	"paragraph",
	"heading",
	"hyperlink",
	"image",
	"user"
};

NodeType get_type(const Node *node)
{
	return (NodeType)node->type;
}

Token get_token(const Node *node)
{
	return (Token)node->token;
}

const Box *get_box(const Node *node)
{
	return node->box;
}

Box *get_box(Node *node)
{
	return node->box;
}

unsigned get_text_length(const Node *node)
{
	return node->text_length;
}

const char *get_text(const Node *node)
{
	return node->text;
}

Layout get_layout(const Node *node)
{
	return (Layout)node->layout;
}

unsigned get_flags(const Node *node)
{
	return node->flags;
}

const NodeStyle *get_style(const Node *node)
{
	return &node->style;
}

NodeStyle *get_style(Node *node)
{
	return &node->style;
}

const Node *parent(const Node *node)
{
	return node->parent;
}

const Node *next_sibling(const Node *node)
{
	return node->next_sibling;
}

const Node *previous_sibling(const Node *node)
{
	return node->prev_sibling;
}

const Node *first_child(const Node *node)
{
	return node->first_child;
}

const Node *last_child(const Node *node)
{
	return node->last_child;
}

Node *parent(Node *node)
{
	return node->parent;
}

Node *next_sibling(Node *node)
{
	return node->next_sibling;
}

Node *previous_sibling(Node *node)
{
	return node->prev_sibling;
}

Node *first_child(Node *node)
{
	return node->first_child;
}

Node *last_child(Node *node)
{
	return node->last_child;
}

static bool refold_attributes(Document *document, Node *base);

/* Searches for an attribute in the buffers of a node and its matched rules. */
const Attribute *find_attribute(const Node *node, int name)
{
	AttributeIterator iterator;
	const Attribute *attribute = node_first_attribute(node, &iterator);
	while (attribute != NULL && attribute->name != name)
		attribute = node_next_attribute(&iterator);
	return attribute;
}

/* Searches for an attribute in a node's private attribute buffer. */
const Attribute *find_attribute_no_rules(const Node *node, int name)
{
	refold_attributes((Document *)node->document, (Node *)node);
	const Attribute *attribute = abuf_first(&node->attributes);
	while (attribute != NULL && (attribute->name != name || 
		attribute->op > AOP_OVERRIDE))
		attribute = abuf_next(&node->attributes, attribute);
	return attribute;
}

/* Searches for an attribute in a node and its parents. */
const Attribute *find_inherited_attribute(const Node *node, int name, 
	const Node **owner)
{
	do {
		const Attribute *attribute = find_attribute(node, name);
		if (attribute != NULL) {
			if (owner != NULL)
				*owner = node;
			return attribute;
		}
		node = node->parent;
	} while (node != NULL);
	if (owner != NULL)
		*owner = NULL;
	return NULL;
}

int read_mode(const Node *node, int name, int defmode)
{
	return abuf_read_mode(find_attribute(node, name), defmode);
}

int read_as_integer(const Node *node, int name, int32_t *result, 
	int32_t defval)
{
	return abuf_read_integer(find_attribute(node, name), result, defval);
}

int read_as_float(const Node *node, int name, float *result, 
	float defval)
{
	return abuf_read_float(find_attribute(node, name), result, defval);
}

int read_as_string(const Node *node, int name, const char **out_data,
	unsigned *out_length, const char *defval)
{
	return abuf_read_string(find_attribute(node, name), out_data, 
		out_length, defval);
}

int read_as_string(const Node *node, int name, char *buffer, 
	unsigned buffer_size, unsigned *out_length, const char *defval, 
	StringSetRepresentation ssr)
{
	return abuf_read_string(find_attribute(node, name), buffer, buffer_size,
		out_length, defval, ssr);
}

int read_as_url(const Node *node, int name, ParsedUrl **out_url, 
	char *buffer, unsigned buffer_size)
{
	const char *s;
	unsigned slen;
	int mode = abuf_read_string(find_attribute(node, name), &s, &slen);
	*out_url = parse_url(s, slen, buffer, buffer_size);
	return mode;
}

int set_integer_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, int value, AttributeOperator op)
{
	int rc = abuf_set_integer(&node->attributes, name, vs, value, op, false);
	if (rc == 1) 
		attribute_changed(document, node, name);
	return rc;
}

int set_float_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, float value, AttributeOperator op)
{
	int rc = abuf_set_float(&node->attributes, name, vs, value, op, false);
	if (rc == 1)
		attribute_changed(document, node, name);
	return rc;
}

int set_string_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, const char *value, int length, AttributeOperator op)
{
	int rc = abuf_set_string(&node->attributes, name, vs, value, length, op, false);
	if (rc == 1)
		attribute_changed(document, node, name);
	return rc;
}

int fold_integer_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, int value, AttributeOperator op)
{
	int rc = abuf_set_integer(&node->attributes, name, vs, value, op, true);
	if (rc == 1) 
		attribute_changed(document, node, name);
	return rc;
}

int fold_float_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, float value, AttributeOperator op)
{
	int rc = abuf_set_float(&node->attributes, name, vs, value, op, true);
	if (rc == 1)
		attribute_changed(document, node, name);
	return rc;
}

int fold_string_attribute(Document *document, Node *node, int name, 
	ValueSemantic vs, const char *value, int length, AttributeOperator op)
{
	int rc = abuf_set_string(&node->attributes, name, vs, value, length, op, true);
	if (rc == 1)
		attribute_changed(document, node, name);
	return rc;
}

/* Sets a node's text buffer. */
void set_node_text(Document *document, Node *node, const char *text, int length)
{
	if (length < 0)
		length = (int)strlen(text);
	if (length + 1 > (int)node->text_length) {
		if ((node->flags & NFLAG_HAS_STATIC_TEXT) == 0)
			delete [] node->text;
		node->text = new char[length + 1];
		node->flags &= ~NFLAG_HAS_STATIC_TEXT;
	}
	memcpy(node->text, text, length);
	node->text[length] = '\0';
	node->text_length = length;
	set_node_flags(document, node, NFLAG_REBUILD_INLINE_CONTEXT, true);
}

/* Sets the total width or height of a node, accounting for its padding. */
void set_outer_dimension(Document *document, Node *node, 
	Axis axis, int dim)
{
	Token token_pad_lower, token_pad_upper, token_dim;
	if (axis == AXIS_H) {
		token_pad_lower = TOKEN_PADDING_LEFT;
		token_pad_upper = TOKEN_PADDING_RIGHT;
		token_dim = TOKEN_WIDTH;
	} else {
		token_pad_lower = TOKEN_PADDING_TOP;
		token_pad_upper = TOKEN_PADDING_BOTTOM;
		token_dim = TOKEN_HEIGHT;
	}
	int padding_lower, padding_upper;
	read_as_integer(node, token_pad_lower, &padding_lower);
	read_as_integer(node, token_pad_upper, &padding_upper);
	int content_dim = std::max(0, dim - padding_upper - padding_lower);
	set_integer_attribute(document, node, token_dim, VSEM_NONE, content_dim);
}

/* Builds an array of attribute buffer pointers for a node, highest priority
 * first. */
static unsigned sort_attribute_buffers(const Node *node, 
	const AttributeBuffer *buffers[1 + NUM_RULE_SLOTS])
{
	unsigned num_buffers = 0;
	const AttributeBuffer *nb = node->attributes.num_attributes != 0 ? 
		&node->attributes : NULL;
	for (unsigned i = 0; i < node->num_matched_rules; ++i) {
		const Rule *rule = node->rule_slots[i].rule;
		if ((rule->flags & RFLAG_ENABLED) == 0 || 
			rule->attributes.num_attributes == 0)
			continue;
		if (nb != NULL && (rule->priority >> RULE_PRIORITY_SHIFT) > 
			RULE_PRIORITY_OVERRIDE) {
			buffers[num_buffers++] = nb;
			nb = NULL;
		}
		buffers[num_buffers++] = &rule->attributes;
	}
	if (nb != NULL)
		buffers[num_buffers++] = nb;
	return num_buffers;
}

const Attribute *node_first_attribute(const Node *node, AttributeIterator *ai)
{
	refold_attributes((Document *)node->document, (Node *)node);
	ai->node = node;
	memset(ai->visited, 0, sizeof(ai->visited));
	ai->num_buffers = (int)sort_attribute_buffers(node, ai->buffers);
	ai->index = unsigned(-1);
	ai->attribute = NULL;
	return node_next_attribute(ai);
}

const Attribute *node_next_attribute(AttributeIterator *ai)
{
	const Attribute *a = ai->attribute;
	for (;;) {
		if (a != NULL)
			a = abuf_next(ai->buffers[ai->index], a);
		if (a == NULL) {
			if (ai->index + 1 != ai->num_buffers)
				a = abuf_first(ai->buffers[++ai->index]);
			if (a == NULL)
				break;
		}
		if (amask_test(ai->visited, a->name))
			continue;
		amask_or(ai->visited, a->name);
		if (a->mode != ADEF_UNDEFINED && a->op <= AOP_OVERRIDE)
			break;
	}
	ai->attribute = a;
	return a;
}

static const unsigned MAX_MODIFIERS = 2048;

struct VisitedAttribute { 
	short name;
	bool must_fold;
	const Attribute *lhs;
	unsigned offset;
	unsigned count;
};

struct AttributeFoldingState {
	Node *base;

	/* A list of non-SET attributes to be applied in reverse order. */
	unsigned num_modifiers;
	const Attribute *modifiers[MAX_MODIFIERS];
	const Attribute *sorted_modifiers[MAX_MODIFIERS];
	uint32_t required[ATTRIBUTE_MASK_WORDS];

	/* A set recording the attributes we have seen so far, their first SET (the
	 * "left hand side" of the folding chain), and a modifier count. */
	VisitedAttribute visited[NUM_ATTRIBUTE_TOKENS];
	unsigned num_visited;
	unsigned visited_map[NUM_ATTRIBUTE_TOKENS];

	/* Working state used to build the style. */
	NodeStyle style;
	const NodeStyle *inherited;
	LogicalFont descriptor;
	bool have_font_face;
	bool have_font_size;
	bool must_update_font_id;
	bool text_style_changed;
};

static VisitedAttribute *afs_add_visited(AttributeFoldingState *s, int name)
{
	VisitedAttribute *va;
	unsigned index = name - TOKEN_ATTRIBUTE_FIRST;
	assertb(index < NUM_ATTRIBUTE_TOKENS);
	unsigned visited_index = s->visited_map[index];
	if (visited_index >= s->num_visited || 
		s->visited[visited_index].offset != index) {
		s->visited_map[index] = s->num_visited;
		va = &s->visited[s->num_visited++];
		va->name = (short)name;
		va->must_fold = false;
		va->offset = index;
		va->count = 0;
		va->lhs = NULL;
	} else {
		va = &s->visited[visited_index];
	}
	return va;
}

/* Adds a modifier to the folding list and increments the attribute's modifier
 * count. */
static void afs_add_modifier(AttributeFoldingState *s, const Attribute *b)
{
	if (s->num_modifiers == MAX_MODIFIERS)
		return;
	s->modifiers[s->num_modifiers++] = b;
	VisitedAttribute *va = afs_add_visited(s, b->name);
	va->count++;
}

/* Groups modifiers for each attribute together and reverses the order of 
 * modifiers for each attribute so that the leftmost comes first. */
static void afs_sort_modifiers(AttributeFoldingState *s)
{
	unsigned pos = 0;
	for (unsigned i = 0; i < s->num_visited; ++i) {
		pos += s->visited[i].count;
		s->visited[i].offset = pos;
	}
	for (unsigned i = 0; i < s->num_modifiers; ++i) {
		const Attribute *a = s->modifiers[i];
		unsigned index = a->name - TOKEN_ATTRIBUTE_FIRST;
		assertb(index < NUM_ATTRIBUTE_TOKENS);
		unsigned j = s->visited_map[index];
		s->sorted_modifiers[--s->visited[j].offset] = a;
	}
}

/* Builds a set of attributes that must be computed for 'base' and a list of
 * the modifiers needed to compute them. */
static void afs_add_modifiers(AttributeFoldingState *fs)
{
	uint32_t have_lhs[ATTRIBUTE_MASK_WORDS] = { 0 };

	for (const Node *node = fs->base; node != NULL; node = node->parent) {
		/* Get the attribute buffers of this node and its matched rules. */
		const AttributeBuffer *buffers[1 + NUM_RULE_SLOTS];
		unsigned num_buffers = sort_attribute_buffers(node, buffers);

		uint32_t lhs_this_level[ATTRIBUTE_MASK_WORDS] = { 0 };
		for (unsigned i = 0; i < num_buffers; ++i) {
			for (const Attribute *b = abuf_first(buffers[i]); b != NULL; 
				b = abuf_next(buffers[i], b)) {
				/* Ignore this attribute if it's not in the set of attributes
				 * we're looking for. */
				amask_or(fs->required, b->name, node == fs->base);
				if (!amask_test(fs->required, b->name))
					continue;
				/* Ignore the attribute if it was completed in a child node. */
				if (amask_test(have_lhs, b->name))
					continue;
				/* Ignore parent values for non-inheritable attributes. */
				if (node != fs->base && !is_inheritable(b->name))
					continue;

				/* If this is is a SET, the attribute's folding chain is 
				 * completed on this level. We continue to add modifiers at
				 * this level to implement the rule that SETs are reordered
				 * past modifiers on the same level. */
				if (b->op <= AOP_OVERRIDE) {
					/* Ignore stale folded results at the base level. */
					if (b->folded && node == fs->base)
						continue;
					/* If this is the first entry eligible to be a LHS for this 
					 * attribute, or it has higher priority than the existing 
					 * LHS, this entry becomes the LHS. */
					VisitedAttribute *va = afs_add_visited(fs, b->name);
					if (va->lhs != NULL && b->op <= va->lhs->op)
						continue;
					/* Sets to "auto" don't become the LHS, but they mark mark
					 * the attribute for folding. If no non-auto SET is 
					 * encountered, an auto LHS value will be calculated. */
					if (is_auto_mode(b->name, b->mode)) {
						va->must_fold = true;
						continue;
					}
					/* Overrides must be folded even if there are no modifiers
					 * so that the folded attribute is visible to searches
					 * before any subsequent non-override SETs. */
					if (b->op == AOP_OVERRIDE)
						va->must_fold = true;
					amask_or(lhs_this_level, b->name);
					va->lhs = b;
				} else {
					/* A modifier. */
					afs_add_modifier(fs, b);
				}
			}
		}

		/* If we have a complete folding chain for all the requested attributes,
		 * there's no need to walk up the tree any further. */
		amask_union(have_lhs, lhs_this_level);
		if (amask_is_subset(have_lhs, fs->required))
			break;
	}
}

/* Recalculates the style's font ID if font-related attributes have changed. */
static void afs_maybe_update_font(AttributeFoldingState *fs)
{
	if (!fs->must_update_font_id)
		return;

	/* Start with the descriptor of the inherited font and overwrite 
	 * fields defined by attributes of the base node. */
	System *system = fs->base->document->system;
	const LogicalFont *inherited_descriptor = get_font_descriptor(
		system, fs->style.text.font_id);
	if (inherited_descriptor != NULL) {
		if (!fs->have_font_face) {
			memcpy(fs->descriptor.face, inherited_descriptor->face, 
				sizeof(fs->descriptor.face));
		}
		if (!fs->have_font_size)
			fs->descriptor.font_size = inherited_descriptor->font_size;
	}
	fs->descriptor.flags = fs->style.flags & FONT_STYLE_MASK;

	/* Make a new font ID from the descriptor. */
	fs->style.text.font_id = get_font_id(system, &fs->descriptor);
	fs->style.text.flags = fs->descriptor.flags;
	fs->text_style_changed = true;
	fs->must_update_font_id = false;
}

/* Makes a default LHS value for use when an attribute is undefined. */
static Attribute *afs_build_auto_value(AttributeFoldingState *fs,
	AttributeBuffer *abuf, int name)
{
	const FontMetrics *metrics = NULL;
	switch (name) {
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
			return abuf_append_integer(abuf, name, VSEM_NONE, 0);
		case TOKEN_LEADING:
			afs_maybe_update_font(fs);
			metrics = get_font_metrics(fs->base->document->system, 
				fs->style.text.font_id);
			return abuf_append_integer(abuf, name, VSEM_NONE, 
				metrics->height / 8);
		case TOKEN_INDENT:
			afs_maybe_update_font(fs);
			metrics = get_font_metrics(fs->base->document->system, 
				fs->style.text.font_id);
			return abuf_append_integer(abuf, name, VSEM_NONE, 
				metrics->paragraph_indent_width);
	}

	AttributeSemantic semantic = attribute_semantic(name);
	switch (semantic) {
		case ASEM_EDGES:
			return abuf_append_integer(abuf, name, VSEM_TOKEN, TOKEN_NONE);
		case ASEM_STRING_SET:
			return abuf_append_string(abuf, name, VSEM_LIST, "", 0);
	}

	return NULL;
}

static bool maybe_switch_layout(Document *document, Node *node, 
	Layout context);

/* Computes the final value for each visited attribute, storing the results as
 * folded attributes at the start of 'dest'. */
static void afs_reduce(AttributeFoldingState *fs)
{
	Node *base = fs->base;
	Document *document = base->document;
	AttributeBuffer *dest = &base->attributes;
	Layout new_layout = natural_layout((NodeType)base->type);

	/* If this is the root, it defines the global text selection colours. */
	if (base == document->root) {
		document->selected_text_color = DEFAULT_SELECTED_TEXT_COLOR;
		document->selected_text_fill_color = DEFAULT_SELECTED_TEXT_FILL_COLOR;
	}

	/* Fold attributes and update the computed style. */
	char work_buffer[256];
	AttributeBuffer working;
	abuf_init(&working, work_buffer, sizeof(work_buffer));
	for (unsigned i = 0; i < fs->num_visited; ++i) {
		/* A set with no modifiers need not have a folded attribute because it
		 * will be found by ordinary traversal. */
		VisitedAttribute *va = fs->visited + i;
		Attribute *lhs = (Attribute *)va->lhs;
		if (va->count != 0 || va->must_fold) {
			/* If the chain contains no explicit set, the value is undefined 
			 * unless this attribute has a static default LHS. */
			if (lhs == NULL) {
				lhs = afs_build_auto_value(fs, &working, va->name);
				if (lhs == NULL)
					continue;
			} else {
				lhs = abuf_append(&working, lhs);
			}
			/* Fold in any modifiers. */
			for (unsigned j = 0; j < va->count; ++j) {
				const Attribute *rhs = fs->sorted_modifiers[va->offset + j];
				abuf_fold(&working, lhs, rhs, &lhs);
			}
			lhs->folded = true;
		}
				
		/* Read the attribute and update the style. */
		int32_t integer_value;
		int mode;
		switch (lhs->name) {
			case TOKEN_LAYOUT:
				new_layout = (Layout)abuf_read_mode(lhs, new_layout);
				break;
			case TOKEN_FONT:
				abuf_read_string(lhs, fs->descriptor.face, 
					sizeof(fs->descriptor.face), NULL, DEFAULT_FONT_FACE);
				fs->have_font_face = true;
				fs->must_update_font_id = true;
				break;
			case TOKEN_FONT_SIZE:
				abuf_read_integer(lhs, &fs->descriptor.font_size, 
					DEFAULT_FONT_SIZE);
				fs->have_font_size = true;
				fs->must_update_font_id = true;
				break;
			case TOKEN_COLOR:
				if (abuf_read_integer(lhs, &integer_value) != ADEF_UNDEFINED) {
					fs->style.text.color = (uint32_t)integer_value;
					fs->text_style_changed = true;
				}
				break;
			case TOKEN_TINT:
				if (abuf_read_integer(lhs, &integer_value) != ADEF_UNDEFINED) {
					fs->style.text.tint = (uint32_t)integer_value;
					fs->text_style_changed = true;
				}
				break;
			case TOKEN_BOLD:
				if ((mode = abuf_read_mode(lhs)) != ADEF_UNDEFINED)
					fs->style.flags = set_or_clear(fs->style.flags, STYLE_BOLD, 
						(mode == FLAGMODE_TRUE));
				fs->must_update_font_id = true;
				break;
			case TOKEN_ITALIC:
				if ((mode = abuf_read_mode(lhs)) != ADEF_UNDEFINED)
					fs->style.flags = set_or_clear(fs->style.flags, 
						STYLE_ITALIC, (mode == FLAGMODE_TRUE));
				fs->must_update_font_id = true;
				break;
			case TOKEN_UNDERLINE:
				if ((mode = abuf_read_mode(lhs)) != ADEF_UNDEFINED)
					fs->style.flags = set_or_clear(fs->style.flags, 
						STYLE_UNDERLINE, (mode == FLAGMODE_TRUE));
				fs->must_update_font_id = true;
				break;
			case TOKEN_JUSTIFY:
				if ((mode = abuf_read_mode(lhs)) != ADEF_UNDEFINED)
					fs->style.justification = (Justification)mode;
				break;
			case TOKEN_LEADING:
				mode = abuf_read_integer(lhs, &integer_value);
				if (mode > DMODE_AUTO)
					fs->style.leading = saturate16(integer_value);
				break;
			case TOKEN_INDENT:
				mode = abuf_read_integer(lhs, &integer_value);
				 if (mode > DMODE_AUTO)
					fs->style.hanging_indent = saturate16(integer_value);
				break;
			case TOKEN_WHITE_SPACE:
				if ((mode = abuf_read_mode(lhs)) != ADEF_UNDEFINED)
					fs->style.white_space_mode = (WhiteSpaceMode)mode; 
				break;
			case TOKEN_WRAP:
				if ((mode = abuf_read_mode(lhs)) != ADEF_UNDEFINED)
					fs->style.wrap_mode = (WrapMode)mode; 
				break;
			case TOKEN_ENABLED:
				if ((mode = abuf_read_mode(lhs)) != ADEF_UNDEFINED)
					fs->style.flags = set_or_clear(fs->style.flags, STYLE_ENABLED, 
						(mode == FLAGMODE_TRUE));
				break;
			case TOKEN_SELECTION_COLOR:
				if (base != document->root)
					continue;
				abuf_read_integer(lhs, 
					(int32_t *)&document->selected_text_color, 
					DEFAULT_SELECTED_TEXT_COLOR);
				break;
			case TOKEN_SELECTION_FILL_COLOR:
				if (base != document->root)
					continue;
				abuf_read_integer(lhs, 
					(int32_t *)&document->selected_text_fill_color, 
					DEFAULT_SELECTED_TEXT_FILL_COLOR);
				break;
		}
	}

	/* Replace any existing folded attributes at the start of the destination 
	 * buffer with the attributes in the working buffer. */
	const Attribute *end = abuf_first(dest);
	while (end != NULL && end->folded)
		end = abuf_next(dest, end);
	abuf_replace_range(dest, abuf_first(dest), end, &working);
	abuf_clear(&working);

	/* Update the layout mode. If the new mode is no-layout, leave the styles
	 * as they are. This is a trick to avoid layout when a node is hidden
	 * and then shown again. It helps in the situtation that the layout 
	 * attribute is changed by a rule that also applies some other style 
	 * attributes. If the rule is enabled and disabled to hide and show the
	 * node, the other styles will change and change back again to no effect. */
	if (maybe_switch_layout(document, base, new_layout))
		base->flags |= NFLAG_RECOMPOSE_CHILD_BOXES;
	if (base->layout == LAYOUT_NONE)
		fs->style = fs->base->style;
}

static void afs_init(AttributeFoldingState *fs, Node *base)
{
	fs->base = base;
	fs->num_modifiers = 0;
	fs->num_visited = 0;
	memset(fs->required, 0, sizeof(fs->required));
	fs->inherited = base->parent != NULL ? &base->parent->style : 
		&DEFAULT_NODE_STYLE;
	fs->style = *fs->inherited;
	fs->have_font_face = false;
	fs->have_font_size = false;
	fs->text_style_changed = false;
	fs->must_update_font_id = (fs->style.text.font_id == INVALID_FONT_ID);
}

static void afs_finalize(AttributeFoldingState *fs)
{
	afs_maybe_update_font(fs);
	if (fs->text_style_changed)
		update_text_style_key(&fs->style.text);
	/* Store the final style, invalidating text layers and layout depending on 
	 * what changed. */
	Node *base = fs->base;
	unsigned diff = compare_styles(&fs->style, &base->style);
	if (diff != 0) {
		if ((diff & STYLECMP_MUST_RETOKENIZE) != 0)
			base->flags |= NFLAG_REBUILD_INLINE_CONTEXT;
		if ((diff & STYLECMP_MUST_RETOKENIZE) != 0)
			base->flags |= NFLAG_REMEASURE_INLINE_TOKENS;
		if ((diff & STYLECMP_MUST_REPAINT) != 0) 
			base->flags |= NFLAG_UPDATE_TEXT_LAYERS;
		base->style = fs->style;
	}
}

/* Disable debug initialization of AttributeFoldingState which makes debug
 * builds extremely slow. */
#pragma runtime_checks("", off)

/* Recalculates the values of attributes defined by a node or its matched rules
 * that have one or more modifiers, storing the results as folded attributes at 
 * the start of the node's attribute buffer. */
static bool refold_attributes(Document *document, Node *base)
{
	if ((base->flags & NFLAG_FOLD_ATTRIBUTES) == 0 && 
		(base->parent == NULL || !refold_attributes(document, base->parent)))
		return false;
	AttributeFoldingState fs;
	afs_init(&fs, base);
	afs_add_modifiers(&fs);
	afs_sort_modifiers(&fs);
	afs_reduce(&fs);
	afs_finalize(&fs);
	base->flags &= ~NFLAG_FOLD_ATTRIBUTES;
	return true;
}

void attribute_changed(Document *document, Node *node, int name)
{
	set_node_flags(document, node, NFLAG_FOLD_ATTRIBUTES, true);
	if (is_background_attribute(name))
		set_node_flags(document, node, NFLAG_UPDATE_BACKGROUND_LAYERS, true);
	if (is_layout_attribute(name))
		set_node_flags(document, node, NFLAG_REBUILD_BOXES, true);
	if (name == TOKEN_CLASS)
		set_node_flags(document, node, NFLAG_UPDATE_RULE_KEYS, true);
}

/*
 * Node Tree
 */

 /* Returns the tree successor of 'node', not descending into children, and
  * stopping at 'root'. */
const Node *tree_next_up(const Document *document, const Node *root, 
	const Node *node)
{
	document;
	while (node != NULL && node != root) {
		if (node->next_sibling != NULL)
			return node->next_sibling;
		node = node->parent;
	}
	return NULL;
}

/* Yields nodes in a subtree in preorder, stopping at 'root'. */
const Node *tree_next(const Document *document, const Node *root, 
	const Node *node)
{
	return (node != NULL && node->first_child != NULL) ? 
		node->first_child : tree_next_up(document, root, node);
}

/* Returns nodes in preorder, not descending into nodes that establish a layout 
 * context. */
const Node *inline_next(const Document *document, const Node *root,
	const Node *node)
{
	return (node != NULL && (node->layout == LAYOUT_INLINE || node == root)) ? 
		tree_next(document, root, node) : tree_next_up(document, root, node);
}

/* Returns the axis a node's main box should have. */
static Axis structural_axis(NodeType type)
{
	if (type == LNODE_VBOX)
		return AXIS_V;
	if (natural_layout(type) == LAYOUT_INLINE_CONTAINER)
		return AXIS_V;
	return AXIS_H;
}

NodeType node_type_for_tag(int tag_name)
{
	switch (tag_name) {
		case TOKEN_HBOX:
			return LNODE_HBOX;
		case TOKEN_VBOX:
			return LNODE_VBOX;
		case TOKEN_H1:
		case TOKEN_H2:
		case TOKEN_H3:
			return LNODE_HEADING;
		case TOKEN_PARAGRAPH:
		case TOKEN_CODE:
			return LNODE_PARAGRAPH;
		case TOKEN_IMG:
			return LNODE_IMAGE;
		case TOKEN_A:
			return LNODE_HYPERLINK;
		default:
			/* Attribute names create basic nodes. */
			if (attribute_semantic(tag_name) != ASEM_INVALID)
				return LNODE_BASIC;
			break;

	}
	return LNODE_INVALID;
}

/* Returns the layout context established for the children of a particular kind 
 * of node. The result may be LAYOUT_NONE, which means that it depends on the
 * parent layout. */
Layout natural_layout(NodeType type)
{
	if (type == LNODE_TEXT || type == LNODE_PARAGRAPH || type == LNODE_HEADING)
		return LAYOUT_INLINE_CONTAINER;
	if (type == LNODE_VBOX || type == LNODE_HBOX || type == LNODE_IMAGE)
		return LAYOUT_BLOCK;
	return LAYOUT_INLINE;
}

/* Returns the layout context determined by the node type associated with a tag. */
Layout token_natural_layout(int token)
{
	NodeType type = node_type_for_tag(token);
	return type != LNODE_INVALID ? natural_layout(type) : LAYOUT_NONE;
}

/* Returns the layout a node should estabished based on its requested layout
 * and the layout of its current parents. */
static Layout established_layout(const Document *document, 
	const Node *node, Layout requested)
{
	document;

	/* Does the node determine its own layout? */
	if (requested == LAYOUT_NONE || requested == LAYOUT_BLOCK)
		return requested;
	/* Find the first block or inline node in the parent chain. */
	for (node = node->parent; node != NULL; node = node->parent) {
		Layout parent_layout = (Layout)node->layout;
		switch (parent_layout) {
			case LAYOUT_NONE:
				return LAYOUT_NONE;
			case LAYOUT_BLOCK:
				/* Transparent nodes within blocks establish a block. */
				return (requested == LAYOUT_INLINE) ? LAYOUT_BLOCK : requested;
			case LAYOUT_INLINE_CONTAINER:
				/* Non-blocks within inline containers are inline. */
				 return LAYOUT_INLINE;
			default:
				/* Walk up through inline parents. */
				break;
		}	
	}
	return LAYOUT_INLINE_CONTAINER;
}

void remove_from_parent(Document *document, Node *child)
{
	document;

	Node *parent = child->parent;
	if (parent != NULL) {
		propagate_expansion_flags(child, AXIS_BIT_H | AXIS_BIT_V);
		list_remove((void **)&parent->first_child, (void **)&parent->last_child, 
			child, 	offsetof(Node, prev_sibling));
		parent->flags |= NFLAG_RECOMPOSE_CHILD_BOXES;
		document_notify_node_changed(document, parent);
		child->parent = NULL;
	}
	child->flags |= NFLAG_PARENT_CHANGED | NFLAG_FOLD_ATTRIBUTES;
	document->change_clock++;
	document_notify_node_changed(document, child);
}

void insert_child_before(Document *document, Node *parent, Node *child, 
	Node *before)
{
	remove_from_parent(document, child);
	list_insert_before(
		(void **)&parent->first_child, 
		(void **)&parent->last_child, 
		child, before, offsetof(Node, prev_sibling));
	child->parent = parent;
	parent->flags |= NFLAG_RECOMPOSE_CHILD_BOXES;
	propagate_expansion_flags(child, AXIS_BIT_H | AXIS_BIT_V);
	child->flags |= NFLAG_PARENT_CHANGED | NFLAG_FOLD_ATTRIBUTES;
	document->change_clock++;
	document_notify_node_changed(document, parent);
}

void append_child(Document *document, Node *parent, Node *child)
{
	insert_child_before(document, parent, child, NULL);
}

void prepend_child(Document *document, Node *parent, Node *child)
{
	insert_child_before(document, parent, child, parent->first_child);
}

/* Sets expansion flags in the parent chain of 'child'. This function is called
 * to indicate that size of 'child' has changed on the specified axes. */
void propagate_expansion_flags(Node *child, unsigned axes)
{
	Node *parent = child->parent;
	while (parent != NULL) {
		Axis parent_axis = structural_axis((NodeType)parent->type);
		if (((1 << parent_axis) & axes) != 0 && 
			parent->first_child != parent->last_child) {
			unsigned flags = 0;
			if (child == parent->first_child)
				flags |= NFLAG_EXPANDED_LEFT;
			if (child == parent->last_child)
				flags |= NFLAG_EXPANDED_RIGHT;
			parent->flags |= flags << (2 * parent_axis);
		}
		child = parent;
		parent = child->parent;
	}
}

/* Sends a message notifying anyone listening that a node has expanded, and
 * clears all expansion flags. */
static void notify_expansion(Document *document, Node *node)
{
	Message message;
	message.type = MSG_NODE_EXPANDED;
	message.flags = (node->flags & NFLAG_EXPANSION_MASK) / NFLAG_EXPANDED_LEFT;
	message.expansion.node = node;
	enqueue_message(document, &message);
	node->flags &= ~NFLAG_EXPANSION_MASK;
}

/* Returns the first node in the parent chain of 'child', including 'child'
 * itself, that is an immediate child of 'parent'. */
static const Node *find_immediate_child(const Node *child, const Node *parent)
{
	while (child != NULL && child->parent != parent)
		child = child->parent;
	return child;
}

/* True if 'child' is in the subtree of 'parent'. */
bool is_child(const Node *child, const Node *parent)
{
	return find_immediate_child(child, parent) != NULL;
}


/* True if a flag is set on 'node' or any of its parents. */
bool is_flag_set_in_parent(const Node *node, unsigned mask)
{
	while (node != NULL) {
		if ((node->flags & mask) != 0)
			return true;
		node = node->parent;
	}
	return false;
}

/* True if A is before B in the tree. */
bool node_before(const Node *a, const Node *b)
{
	const Node *ba, *bb;
	const Node *ancestor = (const Node *)lowest_common_ancestor(
		(const void *)a, 
		(const void *)b, 
		(const void **)&ba, 
		(const void **)&bb,
		offsetof(Node, parent));
	ensure(ancestor != NULL); /* Undefined if A and B are not in the same tree. */
	if (ancestor == b) return false; /* A is a child of B or A = B. */
	if (ancestor == a) return true; /* B is a child of A. */
	while (ba != NULL) {
		if (ba == bb)
			return true;
		ba = ba->next_sibling;
	}
	return false;
}

const char *get_node_debug_string(const Node *node, 
	const char *value_if_null)
{
	if (node == NULL)
		return value_if_null;
#if defined(STACKER_DIAGNOSTICS)
	return node->debug_info;
#else
	return "node";
#endif
}

void set_node_debug_string(Node *node, const char *fmt, ...)
{
#if defined(STACKER_DIAGNOSTICS)
	va_list args;
	va_start(args, fmt);
	vsnprintf(node->debug_info, sizeof(node->debug_info), fmt, args);
	va_end(args);
	node->debug_info[sizeof(node->debug_info) - 1] = '\0';
#else
	node; fmt;
#endif
}

static void update_node_debug_string(Document *document, Node *node)
{
#if defined(STACKER_DIAGNOSTICS)
	char buf[1024];
	make_node_debug_string(document, node, buf, sizeof(buf));
	set_node_debug_string(node, buf);
#else
	document; node;
#endif
}

unsigned make_node_debug_string(const Document *document, 
	const Node *node, char *buffer, unsigned buffer_size)
{
	static const unsigned TEXT_SAMPLE_CHARS = 20;
	static const unsigned SUFFIX_EXTRA = 32;

	const char *rt = NULL;
	unsigned rt_length = 0;
	for (const Node *child = node; child != NULL && rt_length == 0; 
		child = tree_next(document, node, child)) {
		rt = child->text;
		rt_length = child->text_length;
	}
	char *suffix = "";
	if (rt_length != 0) {
		suffix = new char[rt_length + 1 + SUFFIX_EXTRA];
		unsigned suffix_length = 0;
		suffix[suffix_length++] = ' ';
		suffix[suffix_length++] = '[';
		if (rt != node->text)
			suffix[suffix_length++] = '>';
		suffix[suffix_length++] = '"';
		for (unsigned i = 0; i < rt_length && i < TEXT_SAMPLE_CHARS; ++i) {
			char ch = rt[i];
			if (ch == '\n' || ch == '\r')
				ch = ' ';
			suffix[suffix_length++] = ch;
		}
		suffix[suffix_length++] = '"';
		suffix[suffix_length++] = '.';
		suffix[suffix_length++] = '.';
		suffix[suffix_length++] = '.';
		suffix[suffix_length++] = ']';
		suffix[suffix_length] = '\0';
	}
	int length = snprintf(buffer, buffer_size, "%s/%s%s", 
		NODE_TYPE_STRINGS[node->type], 
		TOKEN_STRINGS[node->token],
		suffix);
	if (rt_length != 0)
		delete [] suffix;
	buffer[buffer_size - 1] = '\0';
	if (length < 0)
		length = int(buffer_size) - 1;
	return (unsigned)length;
}

/* Performs type-specific node initialization. */
static void initialize_node(Document *document, Node *node)
{
	update_node_debug_string(document, node);
}

/* Makes a provisional set of rule keys for a node before the actually exists.
 * We do this so we can allocate an initial rule key buffer as part of the
 * node data. */
static unsigned make_initial_rule_keys(const System *system, int tag_name,
	uint64_t *rule_keys, const AttributeAssignment *assignments, 
	unsigned num_assignments)
{
	const char *cls = NULL;
	unsigned cls_length = 0;
	char parsed_classes[512];
	for (unsigned i = 0; i < num_assignments; ++i) {
		if (assignments[i].name == TOKEN_CLASS) {
			cls = assignments[i].value.string.data;
			cls_length = assignments[i].value.string.length;
			break;
		}
	}
	if (cls != NULL) {
		int rc = parse_string_list(cls, cls_length, parsed_classes, 
			sizeof(parsed_classes));
		if (rc >= 0) {
			cls = parsed_classes;
			cls_length = rc;
		}
	}
	return make_node_rule_keys(system, tag_name, 0,
		cls, cls_length, rule_keys, MAX_NODE_RULE_KEYS); 
}

/* Creates a node object from an initial attribute set and text content.  */
int create_node(Node **result, Document *document, NodeType type, int tag_name,
	const AttributeAssignment *assignments, unsigned num_assignments, 
	const char *text, uint32_t text_length)
{
	/* The node's rule keys upon creation are allocated in a static block
	 * after the node. Rule keys depend on the class attribute, so we have to
	 * find that in the VA list, and generate rule keys into a temporary 
	 * buffer before allocating the node. */
	uint64_t rule_keys[MAX_NODE_RULE_KEYS];
	unsigned num_rule_keys = make_initial_rule_keys(document->system, tag_name,
		rule_keys, assignments, num_assignments);
	unsigned rule_key_capacity = std::min(2 * num_rule_keys, MAX_NODE_RULE_KEYS);
	
	/* Determine the size of the node and its initial attribute block. */
	uint32_t bytes_required = sizeof(Node);
	uint32_t attribute_block_size = 0;
	for (unsigned i = 0; i < num_assignments; ++i) {
		int rc = abuf_set(NULL, assignments[i].name, &assignments[i].value);
		if (rc < 0)
			return rc;
		attribute_block_size += (unsigned)rc;
	}
	bytes_required += attribute_block_size;
	bytes_required += rule_key_capacity * sizeof(uint64_t);
	bytes_required += text_length + 1;

	/* Initialize the header. */
	char * block = new char[bytes_required];
	Node *node = (Node *)block;
	block += sizeof(Node);
	node->document = document;
	node->type = (uint8_t)type;
	node->layout = (uint8_t)LAYOUT_NONE;
	node->current_layout = (uint8_t)LAYOUT_NONE;
	node->target_layout = (uint8_t)LAYOUT_NONE;
	node->token = (uint8_t)tag_name;
	node->flags = 
		NFLAG_PARENT_CHANGED | 
		NFLAG_UPDATE_TEXT_LAYERS | 
		NFLAG_UPDATE_BACKGROUND_LAYERS | 
		NFLAG_FOLD_ATTRIBUTES |
		NFLAG_REBUILD_BOXES | 
		NFLAG_UPDATE_MATCHED_RULES |
		NFLAG_HAS_STATIC_TEXT |
		NFLAG_HAS_STATIC_RULE_KEYS;
	node->num_rule_keys = (uint8_t)num_rule_keys;
	node->rule_key_capacity = (uint8_t)rule_key_capacity;
	node->num_matched_rules = 0;
	node->first_child = NULL;
	node->last_child = NULL;
	node->next_sibling = NULL;
	node->prev_sibling = NULL;
	node->parent = NULL;
	node->hit_prev = NULL;
	node->hit_next = NULL;
	node->selection_prev = NULL;
	node->selection_next = NULL;
	node->text_length = text_length;
	node->box = NULL;
	node->layers = NULL;
	node->inline_context = NULL;
	node->style = DEFAULT_NODE_STYLE;
	
	/* Copy in the node's text. */
	if (text != NULL)
		memcpy(block, text, text_length);
	block[text_length] = '\0';
	node->text = block;
	block += text_length + 1;

	/* Initialize the attribute buffer with the static attribute block as its
	 * initial storage, and populate it with the supplied parsed attributes. */
	abuf_init(&node->attributes, block, attribute_block_size);
	for (unsigned i = 0; i < num_assignments; ++i)
		abuf_set(&node->attributes, assignments[i].name, &assignments[i].value,
			assignments[i].op);
	block += attribute_block_size;

	/* Copy in the rule keys. */
	node->rule_keys = (uint64_t *)block;
	memcpy(node->rule_keys, rule_keys, num_rule_keys * sizeof(uint64_t));
	block += rule_key_capacity * sizeof(uint64_t);

	/* Perform any node-type specific initialization. */
	initialize_node(document, node);

	document->system->total_nodes++;
	*result = node;
	return STKR_OK;
}

static void destroy_node_boxes(Document *document, Node *node);

void destroy_node(Document *document, Node *node, bool recursive)
{
	document->system->total_nodes--;
	document_notify_node_destroy(document, node);
	if (node->inline_context != NULL)
		destroy_inline_context(document, node);
	remove_from_parent(document, node);
	destroy_node_boxes(document, node);
	release_layer_chain(document, VLCHAIN_NODE, node->layers);
	node->layers = NULL;
	if (recursive) {
		destroy_children(document, node);
	} else {
		for (Node *child = node->first_child; child != NULL; 
			child = child->next_sibling)
			child->parent = NULL;
	}
	abuf_clear(&node->attributes);
	if ((node->flags & NFLAG_HAS_STATIC_RULE_KEYS) == 0)
		delete [] node->rule_keys;
	if ((node->flags & NFLAG_HAS_STATIC_TEXT) == 0)
		delete [] node->text;
	delete [] (char *)node;
}

void destroy_children(Document *document, Node *node)
{
	for (Node *child = node->first_child, *next; child != NULL; child = next) {
		next = child->next_sibling;
		destroy_node(document, child, true);
	}
}

static void destroy_node_boxes(Document *document, Node *node)
{
	if (node->box == NULL)
		return;
	if (node->current_layout == LAYOUT_INLINE_CONTAINER) {
		/* A text container owns its container box and the line boxes, which are
		 * the container box's immediate children. Its text boxes are destroyed
		 * with the inline context. */
		destroy_sibling_chain(document, node->box->first_child, false);
		destroy_box(document, node->box, false);
		node->box = NULL;
	} else {
		destroy_owner_chain(document, node->box, false);
		node->box = NULL;
	}
}

/* Sets or clears a mask of node flags. */
void set_node_flags(Document *document, Node *node, unsigned mask, bool value)
{
	unsigned new_flags = set_or_clear(node->flags, mask, value);
	unsigned changed = node->flags ^ new_flags;
	node->flags = new_flags;
	if (changed != 0)
		document->change_clock++;
}

/* Creates a new layer of the specified types and adds it to the node's layer
 * stack. */
static VisualLayer *add_node_layer(Document *document, Node *node, 
	VisualLayerType type, LayerKey key)
{
	VisualLayer *layer = create_layer(document, node, type);
	layer_chain_insert(VLCHAIN_NODE, &node->layers, layer, key);
	set_node_flags(document, node, NFLAG_UPDATE_BOX_LAYERS, true);
	return layer;
}

/* Removes a layer from the layer chains of its node and box and destroys the
 * layer. */
static void remove_node_layer(Document *document, Node *node, VisualLayer *layer)
{
	if (layer != NULL) {
		layer_chain_remove(VLCHAIN_NODE, &node->layers, layer);
		set_node_flags(document, node, NFLAG_UPDATE_BOX_LAYERS, true);
		release_layer(document, layer);
	}
}

/* True if a node's rule keys are out of date. */
bool must_update_rule_keys(const Node *node)
{
	unsigned mask = NFLAG_UPDATE_RULE_KEYS | NFLAG_UPDATE_MATCHED_RULES;
	if ((node->flags & mask) != 0)
		return true;
	return is_flag_set_in_parent(node->parent, mask | NFLAG_UPDATE_CHILD_RULES);
}

/* Updates the list of rule table keys identifying selectors a node can 
 * match. */
static void update_node_rule_keys(Document *document, Node *node, 
	bool ignore_class_modifiers = false)
{
	uint64_t keys[MAX_NODE_RULE_KEYS];

	/* The class and the set of matched rules have a reciprocal relationship.
	 * To break the cycle we use a version of the class attribute that is not 
	 * modified by rules in the first iteration of rule matching. */
	const char *cls = NULL;
	unsigned cls_length = 0;
	if (ignore_class_modifiers) {
		const Attribute *attribute = find_attribute_no_rules(node, TOKEN_CLASS);
		abuf_read_string(attribute, &cls, &cls_length);
	} else {
		read_as_string(node, TOKEN_CLASS, &cls, &cls_length);
	}
	
	unsigned num_keys = make_node_rule_keys(document->system, 
		(Token)node->token, node->flags, cls, cls_length, 
		keys, MAX_NODE_RULE_KEYS);
	if (num_keys > node->rule_key_capacity) {
		if ((node->flags & NFLAG_HAS_STATIC_RULE_KEYS) == 0)
			delete [] node->rule_keys;
		node->rule_keys = new uint64_t[num_keys];
		node->flags &= ~NFLAG_HAS_STATIC_RULE_KEYS;
	}
	memcpy(node->rule_keys, keys, num_keys * sizeof(uint64_t));
	node->num_rule_keys = (uint8_t)num_keys;
	node->rule_key_capacity = (uint8_t)num_keys;
	node->flags &= ~NFLAG_UPDATE_RULE_KEYS;
}

/* Rebuilds a node's array of matched rule references. */
static bool update_rule_slots(Document *document, Node *node)
{
	const Rule *matched[NUM_RULE_SLOTS];
	unsigned num_matched = match_rules(document, node, 
		matched, NUM_RULE_SLOTS, &document->rules, 
		&document->system->global_rules);
	bool changed = num_matched != node->num_matched_rules;
	unsigned i;
	for (i = 0; i < num_matched; ++i) {
		RuleSlot *slot = node->rule_slots + i;
		if (i >= node->num_matched_rules || slot->rule != matched[i]) {
			slot->rule = matched[i];
			slot->revision = slot->rule->revision - 1;
			changed = true;
		}
	}
	node->num_matched_rules = (uint8_t)num_matched;
	node->flags &= ~NFLAG_UPDATE_MATCHED_RULES;
	if (changed) 
		node->flags |= NFLAG_FOLD_ATTRIBUTES;
	return changed;
}

/* Looks at the rules matched by a node, and if their attributes have changed
 * (or the rules themselves have changed), sets the relevant update bits. */
static void check_rule_slots(Document *document, Node *node)
{
	document;
	bool rules_changed = false;
	for (unsigned i = 0; i < node->num_matched_rules; ++i) {
		RuleSlot *slot = node->rule_slots + i;
		if (slot->revision != slot->rule->revision) {
			slot->revision = slot->rule->revision;
			rules_changed = true;
		}
	}
	if (rules_changed)
		node->flags |= NFLAG_FOLD_ATTRIBUTES;
}

/* If necessary, rebuilds a node's rule keys from its class attribute and 
 * updates its set of matched rules. Rules can add and remove classes, so a
 * change in matched rules may necessitate a rebuild of the keys, which may
 * change the set of mactched rules again, and so on ad infinitum. Cycles are
 * broken by stopping the process as soon as a previously matched rule with a
 * class modifier is removed from the match set. */
void update_matched_rules(Document *document, Node *node)
{
	static const unsigned MAX_VISITED = 16;
	
	const Rule *visited[MAX_VISITED];
	unsigned num_visited = 0;
 	bool ignore_class_modifiers = true;
	do {
		/* Rebuild the rule keys from the class attribute and rematch 
		 * rules. */
		if ((node->flags & NFLAG_UPDATE_RULE_KEYS) != 0)
			update_node_rule_keys(document, node, ignore_class_modifiers);
		if (!update_rule_slots(document, node))
			break;
		ignore_class_modifiers = false;
		/* The rule set has changed. If any of the rules now matched modify
		 * the class attribute, we must match again. */
		unsigned quota = num_visited;
		for (unsigned i = 0; i < node->num_matched_rules; ++i) {
			const Rule *rule = node->rule_slots[i].rule;
			if ((rule->flags & RFLAG_MODIFIES_CLASS) == 0)
				continue;
			unsigned j;
			for (j = 0; j < num_visited; ++j)
				if (visited[j] == rule)
					break;
			if (j == num_visited) {
				if (num_visited == MAX_VISITED) {
					node->flags &= ~NFLAG_UPDATE_RULE_KEYS;
					break;
				}
				visited[num_visited++] = rule;
				node->flags |= NFLAG_UPDATE_RULE_KEYS;
			} else {
				quota--;
			}
		}
		/* If there are class-changing rules in the visited set that are no
		 * longer matched, we have a cycle, so give up. */
		if (quota != 0)
			break;
	} while ((node->flags & NFLAG_UPDATE_RULE_KEYS) != 0);

	/* The children of this node may now match different rules, even if their
	 * clasess haven't changed, because selectors can match parent nodes. */
	node->flags |= NFLAG_UPDATE_CHILD_RULES;
}

/* Builds a LayerPosition structure by reading background attributes. */
static void read_layer_position(Node *node, LayerPosition *lp)
{
	for (unsigned axis = 0; axis < 2; ++axis) {
		lp->alignment[axis] = (unsigned char)read_mode(node, 
			TOKEN_BACKGROUND_HORIZONTAL_ALIGNMENT + axis, ALIGN_START);
		lp->mode_size[axis] = (unsigned char)read_as_float(node, 
			TOKEN_BACKGROUND_WIDTH + axis, &lp->dims[axis]);
		lp->mode_offset[axis] = (unsigned char)read_as_float(node, 
			TOKEN_BACKGROUND_OFFSET_X + axis, &lp->offsets[axis]);
	}
	lp->placement = (unsigned char)read_mode(node, 
		TOKEN_BACKGROUND_BOX, BBOX_PADDING);
	lp->positioning_mode = (unsigned char)read_mode(node, 
		TOKEN_BACKGROUND_SIZE, VLPM_STANDARD);
}

/* Synchronizes a node's background layer with its attributes. */
static void update_background_layer(Document *document, Node *node)
{
	/* Read "background" and associated attributes to determine the kind of
	 * background layer the node should have. */
	const char *image_url = NULL;
	VisualLayerType bglayer_type = VLT_NONE;
	PaneType pane_type = PANE_FLAT;
	uint32_t background_color = 0, border_color = 0;
	float border_width = 0.0f;
	bool have_pane_color = false;
	const Attribute *attr = find_attribute(node, TOKEN_BACKGROUND);
	if (attr != NULL) {
		if (attr->mode == BGMODE_URL) {
			abuf_read_string(attr, &image_url);
			bglayer_type = VLT_IMAGE;
		} else if (attr->mode == BGMODE_COLOR) {
			abuf_read_integer(attr, (int32_t *)&background_color);
			have_pane_color = true;
			bglayer_type = VLT_PANE;
			pane_type = PANE_FLAT;
		} else if (attr->mode >= BGMODE_PANE_FIRST && 
			attr->mode <= BGMODE_PANE_LAST) {
			bglayer_type = VLT_PANE;
			pane_type = PaneType(attr->mode - BGMODE_PANE_FIRST);
		}
	}

	/* If there's no 'background' attribute but a border is defined, we make
	 * an unfilled pane background.*/
	if (bglayer_type == VLT_NONE || bglayer_type == VLT_PANE) {
		int border_color_mode = read_as_integer(node, TOKEN_BORDER_COLOR, 
			(int32_t *)&border_color, 0xFF000000);
		int border_width_mode = read_as_float(node, TOKEN_BORDER_WIDTH, 
			&border_width, 1.0f);
		if (bglayer_type == VLT_NONE && (border_color_mode != ADEF_UNDEFINED ||
			border_width_mode != ADEF_UNDEFINED)) {
			bglayer_type = VLT_PANE;
			pane_type = PANE_FLAT;
		}
	}

	/* Make sure the node's background layer object is the right sort of 
	 * layer. */
	VisualLayer *layer = layer_chain_find(VLCHAIN_NODE, node->layers, LKEY_BACKGROUND);
	if (bglayer_type == VLT_NONE) {
		if (layer != NULL)
			remove_node_layer(document, node, layer);
		return;
	}
	if (layer == NULL || layer->type != bglayer_type) {
		if (layer != NULL)
			remove_node_layer(document, node, layer);
		layer = add_node_layer(document, node, bglayer_type, LKEY_BACKGROUND);
	}

	/* Everything has a tint. */
	uint32_t tint = node->style.text.tint;

	/* Synchronize the background layer with the node's properties. */
	if (bglayer_type == VLT_IMAGE) {
		layer->image.tint = tint;
		read_layer_position(node, &layer->image.position);
		set_image_layer_url(document, node, layer, image_url);
		poll_network_image(document, node, layer);
	} else if (bglayer_type == VLT_PANE) {
		read_layer_position(node, &layer->pane.position);
		layer->pane.border_color = blend32(border_color, tint);
		layer->pane.border_width = border_width;
		if (!have_pane_color) {
			read_as_integer(node, TOKEN_BACKGROUND_COLOR, 
				(int32_t *)&background_color, 0x00000000);
		}
		layer->pane.fill_color = blend32(background_color, tint);
		layer->pane.pane_type = (PaneType)pane_type;
	}
}

/* Creates or updates a node's content image layer. */
static void update_image_layer(Document *document, Node *node)
{
	if (node->type != LNODE_IMAGE)
		return;
	
	const char *image_url = NULL;
	read_as_string(node, TOKEN_URL, &image_url);
	VisualLayer *layer = layer_chain_find(VLCHAIN_NODE, node->layers, LKEY_CONTENT);

	/* Destroy any existing image layer if we have no URL. */
	if (image_url == NULL && layer != NULL) {
		set_image_layer_url(document, node, layer, NULL);
		remove_node_layer(document, node, layer);
		return;
	}

	/* Create or replace the image layer if necessary. */
	if (layer == NULL || layer->type != VLT_IMAGE) {
		if (layer != NULL)
			remove_node_layer(document, node, layer);
		layer = add_node_layer(document, node, VLT_IMAGE, LKEY_CONTENT);
	}
	read_layer_position(node, &layer->image.position);
	layer->image.position.placement = BBOX_CONTENT;
	layer->image.tint = node->style.text.tint;

	/* Set the URL and poll for updates. */
	set_image_layer_url(document, node, layer, image_url);
	poll_network_image(document, node, layer);
}

/* Updates anything that needs to changed when a node is moved in the graph. */
static void handle_node_parent_changed(Document  *document, Node *node)
{
	document;
	/* Effective layout depends on tree position. */
	node->flags |= NFLAG_FOLD_ATTRIBUTES;
}

/* Synchronizes a node's background and image layers with its attributes. */
static void update_background_layers(Document *document, Node *node)
{
	update_background_layer(document, node);
	update_image_layer(document, node);
}


/* Updates a node's selection highlight layers. */
static void update_selection_layers(Document *document, Node *node)
{
	if (node->layout == LAYOUT_INLINE_CONTAINER) {
		update_inline_selection_layers(document, node);
		node->flags &= ~NFLAG_UPDATE_SELECTION_LAYERS;
		node->flags |= NFLAG_UPDATE_BOX_LAYERS;
	}
}

/* True if a node permits interaction. */
bool is_enabled(const Node *node)
{
	return (node->style.flags & STYLE_ENABLED) != 0;
}

/* Checks for a change in a node's layout attribute and, if required, switches
 * the node's layout to the one requested. */
static bool maybe_switch_layout(Document *document, Node *node, 
	Layout requested)
{
	/* Determine the layout established by the node given its current tree 
	 * position. */
	Layout new_layout = established_layout(document, node, requested);

	/* Has the actual layout changed? */
	bool layout_changed = false;
	if (new_layout != (Layout)node->layout) {
		node->layout = (uint8_t)new_layout;
		layout_changed = true;
	}
	
	/* If the node is being hidden, maybe cache the computed layout. */
	Layout target = new_layout;
	if (new_layout == LAYOUT_NONE && node->current_layout != LAYOUT_NONE && 
		(document->system->flags & SYSFLAG_CACHE_HIDDEN_NODE_LAYOUTS) != 0)
		target = (Layout)node->current_layout;

	/* Boxes are rebuilt when the target layout changes. */
	bool target_changed = (target != (Layout)node->target_layout);
	if (target_changed) {
		node->target_layout = (uint8_t)target;
		node->flags |= NFLAG_REBUILD_BOXES;
	} else {
		/* We're not changing target layout (meaning we're not rebuilding 
		 * boxes), but if the node's actual layout changed, we still need to
		 * tell the parent to recompose its child boxes, because this box may
		 * need to be excluded if it has been hidden or included if it has been
		 * shown. */
		if (layout_changed && node->parent != NULL) 
			node->parent->flags |= NFLAG_RECOMPOSE_CHILD_BOXES;
	}
	return target_changed;
}

/* Returns the first node in a parent chain, including 'node' itself, that
 * establishes a layout context. */
const Node *find_context_node(const Document *document, const Node *node)
{
	document;

	/* An inline container node is always its own context. */
	if (node == NULL || node->layout == LAYOUT_INLINE_CONTAINER)
		return node;

	/* Find the first non-LCTX_NONE node in the parent chain. */
	const Node *context;
	for (context = node->parent; context != NULL; context = context->parent) {
		if (context->layout != LAYOUT_INLINE) {
			/* A block inside an inline uses the inline as its context, whereas
			 * a block inside a block defines its own context. */
			if (context->layout == LAYOUT_BLOCK && node->layout == LAYOUT_BLOCK)
				return node;
			break;
		}
	}
	return context;
}

/* If 'node' is an inline child, returns its inline container, otherwise NULL.
 * Always returns NULL for inline containters themselves. */
const Node *find_inline_container(const Document *document, const Node *node)
{
	const Node *context = find_context_node(document, node);
	return (context != NULL && context != node && 
		context->layout == LAYOUT_INLINE_CONTAINER) ? context : NULL;
}

/* Finds the first inline container in the parent chain of a node. Whereas
 * find_inline_container() will return non-null only if the node is an inline
 * child, this method will return the ultimate container for nodes nested in
 * blocks inside an inline container. */
const Node *find_chain_inline_container(const Document *document, 
	const Node *node)
{
	document;
	const Node *container = node->parent;
	while (container != NULL && container->layout != LAYOUT_INLINE_CONTAINER)
		container = container->parent;
	return container;
}

/* Update the layers on inline context nodes responsible for rendering the
 * text of their children.*/
static void update_text_layer(Document *document, Node *node)
{
	if (node->layout != LAYOUT_INLINE_CONTAINER)
		return;
	VisualLayer *text_stack = build_text_layer_stack(document, node);
	VisualLayer *old_stack = layer_chain_replace(VLCHAIN_NODE, &node->layers,
		LKEY_TEXT, text_stack);
	release_layer_chain(document, VLCHAIN_NODE, old_stack);
}

/* Rebuilds a node's box's layer stack. */
static void update_node_box_layers(Document *document, Node *node)
{
	document;
	Box *box = node->box;
	if (box == NULL)
		return;
	release_layer_chain(document, VLCHAIN_BOX, box->layers);
	box->layers = layer_chain_mirror(node->layers, VLCHAIN_NODE, VLCHAIN_BOX);
	/* Recursively recalculate depths if the layer count has changed. */
	unsigned depth_interval = layer_chain_count_keys(VLCHAIN_BOX, box->layers);
	if (depth_interval != box->depth_interval) {
		box->depth_interval = (uint16_t)depth_interval;
		box->flags &= ~BOXFLAG_TREE_CLIP_VALID;
		clear_flag_in_parents(document, box, BOXFLAG_TREE_CLIP_VALID);
	}
}

 /* Sets interaction state bits on a node. */
void set_interaction_state(Document *document, Node *node, 
	unsigned mask, bool value)
{
	node->flags = set_or_clear(node->flags, mask, value);
	/* Interation bits cause pseudo-classes to appear and disappear on the
	 * node. This might result in the node or any of its children matching
	 * different rules. */
	node->flags |= NFLAG_UPDATE_RULE_KEYS | NFLAG_UPDATE_MATCHED_RULES;
	document->change_clock++;
}

/* Creates or updates a node's boxes, making the node's computed layout the
 * same as its layout. Assumes all attributes affecting box layout have changed,
 * so that boxes must be recreated or reconfigured. */
static void update_node_boxes(Document *document, Node *node)
{
	/* If the current set of boxes is for a different layout mode, remake 
	 * them.*/
	Layout target = (Layout)node->target_layout;
	bool needs_container = target == LAYOUT_BLOCK || 
		target == LAYOUT_INLINE_CONTAINER;
	Box *container = NULL;
	if (node->current_layout != target) {
		destroy_node_boxes(document, node);
		/* Nodes that establish a block or inline context for their children
		 * need a container box. */
		if (needs_container) {
			container = create_box(document, node);
			set_box_debug_string(container, "%s block \"%s\"", 
				NODE_TYPE_STRINGS[(NodeType)node->type],
				random_word((uintptr_t)node));
		}
		node->flags |= NFLAG_RECOMPOSE_CHILD_BOXES;
		if (node->parent != NULL)
			node->flags |= NFLAG_RECOMPOSE_CHILD_BOXES;
	} else {
		container = node->box;
	}
	/* Update the box's properties, assuming all corresponding node attributes
	 * have changed. */
	if (needs_container) {
		Axis axis = structural_axis((NodeType)node->type);
		configure_container_box(document, node, axis, container);
		node->box = container;
	}
	/* Make sure the node has an inline context if it needs one. */
	if (target == LAYOUT_INLINE_CONTAINER && node->inline_context == NULL)
		node->flags |= NFLAG_REBUILD_INLINE_CONTEXT;
	node->current_layout = (uint8_t)target;
	node->flags &= ~NFLAG_REBUILD_BOXES;
}

/* Attaches the boxes of child nodes to the box tree of this node. */
static void compose_child_boxes(Document *document, Node *node)
{
	Box *box = node->box;
	if (box == NULL)
		return;

	/* Text blocks handle their children differently. */
	if (node->layout != LAYOUT_BLOCK)
		return;

	/* Add the boxes of all child nodes as children of our box. */
	remove_all_children(document, box);
	for (Node *child = node->first_child; child != NULL; 
		child = child->next_sibling) {
		if (child->layout == LAYOUT_NONE)
			continue;
		Box *child_box = child->box;
		if (child_box != NULL)
			append_child(document, box, child_box);
	}
}

/* Recursively updates nodes before layout. */
unsigned update_nodes_pre_layout(Document *document, Node *node, 
	unsigned propagate_down, bool rule_tables_changed)
{
	unsigned propagate_up = 0;
	node->flags |= propagate_down;

	update_node_debug_string(document, node);

	/* Rematch rules and/or rebuild rule keys for this node if its classes or 
	 * the contents of the rule tables have changed. */
	if (rule_tables_changed)
		node->flags |= NFLAG_UPDATE_MATCHED_RULES;
	if ((node->flags & (NFLAG_UPDATE_RULE_KEYS | 
		NFLAG_UPDATE_MATCHED_RULES)) != 0) {
		update_matched_rules(document, node);
		propagate_down |= NFLAG_UPDATE_MATCHED_RULES;
	}
	check_rule_slots(document, node);
	
	/* Recursively rematch child rules if required. */
	if ((node->flags & NFLAG_UPDATE_CHILD_RULES) != 0) {
		propagate_down |= NFLAG_UPDATE_MATCHED_RULES;
		node->flags &= ~NFLAG_UPDATE_CHILD_RULES;
	}

	if ((node->flags & NFLAG_PARENT_CHANGED) != 0) {
		handle_node_parent_changed(document, node);
		node->flags &= ~NFLAG_PARENT_CHANGED;
		node->flags |= NFLAG_FOLD_ATTRIBUTES | NFLAG_REBUILD_BOXES;
		propagate_up |= NFLAG_REBUILD_INLINE_CONTEXT;
	}

	/* Constrain the root after rule updates, so the padding is in place,
	 * but before we rebuild boxes. */
	if (node == document->root)
		impose_root_constraints(document);
	
	/* When a node's style is changed, the styles of its children must be 
	 * recalculated. */
	if ((node->flags & NFLAG_FOLD_ATTRIBUTES) != 0) {
		refold_attributes(document, node);
		propagate_down |= NFLAG_FOLD_ATTRIBUTES;
	}

	if ((node->flags & NFLAG_UPDATE_BACKGROUND_LAYERS) != 0) {
		update_background_layers(document, node);
		node->flags &= ~NFLAG_UPDATE_BACKGROUND_LAYERS;
	}

	/* Process our children. */
	for (Node *child = node->first_child; child != NULL; 
		child = child->next_sibling) {
		propagate_up |= update_nodes_pre_layout(document, child, 
			propagate_down, rule_tables_changed);
	}

	/* Some flags propagate up automatically. */
	propagate_up |= (node->flags & (NFLAG_UPDATE_TEXT_LAYERS | 
		NFLAG_REMEASURE_INLINE_TOKENS | NFLAG_REBUILD_INLINE_CONTEXT));
	node->flags |= propagate_up;

	/* Rebuild this node's box. */
	if ((node->flags & NFLAG_REBUILD_BOXES) != 0) {
		update_node_boxes(document, node);
		propagate_up |= NFLAG_RECOMPOSE_CHILD_BOXES | NFLAG_UPDATE_TEXT_LAYERS;
	}

	/* If we've rebuilt our own box tree, or child boxes have changed,
	 * recompose the child boxes into our tree. */
	if ((node->flags & NFLAG_RECOMPOSE_CHILD_BOXES) != 0) {
		compose_child_boxes(document, node);
		node->flags &= ~NFLAG_RECOMPOSE_CHILD_BOXES;
	}

	/* Update inline contexts. */
	if (node->layout == LAYOUT_INLINE_CONTAINER) {
		if ((node->flags & NFLAG_REBUILD_INLINE_CONTEXT) != 0)
			rebuild_inline_context(document, node);
		if ((node->flags & NFLAG_REMEASURE_INLINE_TOKENS) != 0)
			measure_inline_tokens(document, node);
	} else {
		propagate_up |= node->flags & (NFLAG_REBUILD_INLINE_CONTEXT | 
			NFLAG_REMEASURE_INLINE_TOKENS | NFLAG_UPDATE_TEXT_LAYERS);
	}
	node->flags &= ~(NFLAG_REBUILD_INLINE_CONTEXT | 
		NFLAG_REMEASURE_INLINE_TOKENS);

	return propagate_up;
}

/* Recursively updates nodes after layout. */
unsigned update_nodes_post_layout(Document *document, Node *node, 
	unsigned propagate_down)
{
	unsigned propagate_up = 0;
	for (Node *child = node->first_child; child != NULL; 
		child = child->next_sibling)
		propagate_up |= update_nodes_post_layout(document, child, propagate_down);
	unsigned flags = node->flags | propagate_down | propagate_up;
	if ((flags & NFLAG_UPDATE_TEXT_LAYERS) != 0) {
		update_text_layer(document, node);
		node->flags &= ~NFLAG_UPDATE_TEXT_LAYERS;
		flags |= NFLAG_UPDATE_BOX_LAYERS;
		if (node->layout == LAYOUT_INLINE)
			propagate_up |= NFLAG_UPDATE_TEXT_LAYERS;
	}
	if ((flags & NFLAG_UPDATE_SELECTION_LAYERS) != 0) {
		update_selection_layers(document, node);
		propagate_up |= NFLAG_UPDATE_SELECTION_LAYERS;
	}
	if ((flags & NFLAG_UPDATE_BOX_LAYERS) != 0) {
		update_node_box_layers(document, node);
		node->flags &= ~NFLAG_UPDATE_BOX_LAYERS;
		if (node->layout == LAYOUT_INLINE)
			propagate_up |= NFLAG_UPDATE_BOX_LAYERS;
	}
	if ((flags & (NFLAG_WIDTH_CHANGED | NFLAG_HEIGHT_CHANGED)) != 0) {
		if ((node->flags & NFLAG_NOTIFY_EXPANSION) != 0)
			notify_expansion(document, node);
		node->flags &= ~(NFLAG_WIDTH_CHANGED | NFLAG_HEIGHT_CHANGED);
	}
	return propagate_up;
}

/* Disable initialization of large stack arrays which makes debug builds 
 * very slow. */
#pragma runtime_checks("", off)

/* A second box building pass that constructs line boxes for paragraphs and
 * performs paragraph layout. This has to be done in a second pass because the 
 * number of line boxes required depends on the final size of the paragraphs. */
void do_text_layout(Document *document, Node *node)
{
	/* Visit children in preorder. */
	for (Node *child = node->first_child; child != NULL; 
		child = child->next_sibling)
		do_text_layout(document, child);
	if (node->layout != LAYOUT_INLINE_CONTAINER)
		return;

	/* Determine the paragraph width. We use width -1, meaning "no breaking"
	 * if the parent's width is undefined; the parent's width may then be 
	 * determined by the total width of the unbroken text. Line breaking also
	 * doesn't make much sense if the container is a horizontal box, because
	 * the "lines" will just be placed next to each other horizontally, so we
	 * only want a single line in that case too. */
	Box *container_box = node->box;
	ensure(container_box != NULL);
	int line_width = UNBOUNDED_LINE_WIDTH;
	if (container_box->axis == AXIS_V && (container_box->flags & 
		BOXFLAG_WIDTH_DEFINED) != 0) {
		float dim = get_size_directional(container_box, AXIS_H, true);
		line_width = (unsigned)round_signed(dim);
	}

	/* Do we need to redo paragraph layout? */
	if ((node->flags & NFLAG_UPDATE_TEXT_LAYERS) == 0 &&
		(container_box->flags & BOXFLAG_PARAGRAPH_VALID) != 0)
		return;

	/* Read paragraph style attributes. */
	Justification justification = (Justification)node->style.justification;
	if (justification == ADEF_UNDEFINED)
		justification = JUSTIFY_FLUSH;
	int hanging_indent = node->style.hanging_indent;
	float leading = node->style.leading < 0 ? 0.0f : (float)node->style.leading;
	const FontMetrics *metrics = get_font_metrics(document->system, 
		node->style.text.font_id);

	/* Make a paragraph object. */
	Paragraph paragraph;
	paragraph_init(&paragraph, line_width);
	build_paragraph(document, node, &paragraph, hanging_indent);

	/* Break the paragraph into lines. */
	ParagraphLine line_buffer[NUM_STATIC_PARAGRAPH_ELEMENTS], *lines = NULL;
	unsigned num_lines = determine_breakpoints(&paragraph, &lines, 
		line_buffer, NUM_STATIC_PARAGRAPH_ELEMENTS);
	if ((get_flags(document) & DOCFLAG_DEBUG_PARAGRAPHS) != 0) {
		dump_paragraph(document, &paragraph);
		dump_paragraph_lines(document, lines, num_lines);
	}

	/* Create a vertical box for each line and put the word boxes inside 
	 * them. */
	update_inline_boxes(document, node, justification, &paragraph, lines,
		num_lines, (float)leading, (float)metrics->height);

	/* Deallocate the paragraph and any heap buffer used for to store lines. */
	paragraph_clear(&paragraph);
	if (lines != line_buffer)
		delete [] lines;

	/* No need to do paragraph layout again unless the container's width 
	 * changes. */
	container_box->flags |= BOXFLAG_PARAGRAPH_VALID;
	if ((node->flags & NFLAG_IN_SELECTION_CHAIN) != 0)
		node->flags |= NFLAG_UPDATE_SELECTION_LAYERS;
}

/* Iteratively computes box sizes. */
void compute_sizes_iteratively(Document *document, SizingPass pass, Node *root)
{
	unsigned repetitions = 0;
 	for (repetitions = 0; repetitions < 10; ++repetitions)
		if (compute_box_sizes(document, pass, root->box))
			break;
}

} // namespace stkr
