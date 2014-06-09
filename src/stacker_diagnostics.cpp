#include "stacker_diagnostics.h"
#include "stacker_shared.h"
#include "stacker_util.h"
#include "stacker_token.h"
#include "stacker_document.h"
#include "stacker_node.h"
#include "stacker_box.h"
#include "stacker_inline.h"
#include "stacker_paragraph.h"

namespace stkr {

void dump_discard(void *data, const char *fmt, va_list args) { data; fmt; args; }

void dump_paragraph(Document *document, const Paragraph *p)
{
	dmsg("PARAGRAPH elements=%u, line_width=%u\n", p->num_elements, 
		p->line_width);
	for (unsigned i = 0; i < p->num_elements; ++i) {
		const ParagraphElement *e = p->elements + i;
		dmsg("\t%3u: %-9s width: %6d stretch: %6d shrink: %6d "
			"penalty: %6d has_token: %d empty: %u\n", 
			i, PARAGRAPH_ELEMENT_TYPE_STRINGS[e->type],
			e->width, e->stretch, e->shrink, 
			e->penalty, e->has_token, e->empty);
	}
	dmsg("END PARAGRAPH\n");
}

void dump_paragraph_lines(Document *document, const ParagraphLine *lines, 
	unsigned count)
{
	float total_demerits = 0.0f;
	for (unsigned i = 0; i < count; ++i)
		total_demerits += lines[i].line_demerits;
	dmsg("PARAGRAPH LINES n=%u total_demerits = %.3f\n", count, total_demerits);
	for (unsigned i = 0; i < count; ++i) {
		const ParagraphLine *line = lines + i;
		dmsg("\t%03u\tr = %6.3f, demerits = %10.3f, unscaled_width = % -6u\n", 
			i, line->adjustment_ratio, line->line_demerits, line->unscaled_width);
	}
	dmsg("END PARAGRAPH LINES\n");
}


void dump_node(const Document *document, const Node *node, unsigned indent)
{
	/* Print the node's name. */
	dmsg("% *s-> %s sel: %u", 
		indent, "", get_node_debug_string(node),
		(node->flags & NFLAG_IN_SELECTION_CHAIN) != 0);

	/* Print the node's attributes. */
	AttributeIterator iter;
	const Attribute *attribute = node_first_attribute(node, &iter);
	if (attribute != NULL) {
		
		bool first_attribute = true;
		while (attribute != NULL) {
			char value_string[256];
			const char *attribute_name = TOKEN_STRINGS[attribute->name];
			attribute_value_string(value_string, sizeof(value_string), attribute);
			if (first_attribute) {
				first_attribute = false;
				dmsg("\n");
			} else {
				dmsg(",\n");
			}
			dmsg("% *s%s: %s", indent + 6, "", attribute_name, value_string);
			attribute = node_next_attribute(&iter);
		}
	}
	dmsg("\n");
	
	/* Print any text attached to the node. */
	//  if (get_text_length(node) != 0) {
	//  	dmsg(" %u bytes of text: %.*s", get_text_length(node),
	//  		get_text_length(node), get_text(node));
	//  }

	/* Print the node's children. */
	for (const Node *child = first_child(node); child != NULL; 
		child = next_sibling(child))
		dump_node(document, child, indent + 6);
}

/* Prints an inline context's token buffer. */
void dump_inline_context(const Document *document, const Node *node)
{
	const InlineContext *icb = node->inline_context;
	if (icb == NULL) {
		dmsg("%s node %.8Xh has no inline context.", 
			NODE_TYPE_STRINGS[get_type(node)], uint32_t(node));
		return;
	}
	dmsg("Inline context for %s node %.8Xh num_tokens: %u text_length: %u\n", 
		NODE_TYPE_STRINGS[get_type(node)], uint32_t(node), icb->num_tokens, icb->text_length);
	for (unsigned i = 0; i < icb->num_tokens; ++i) {
		const InlineToken *token = icb->tokens + i;
		const char *child_type_string = token->child != NULL ? 
			NODE_TYPE_STRINGS[get_type(token->child)] : "none";
		const Box *child_box = token->child != NULL ? 
			get_box(token->child) : NULL;
		const Box *token_box = (token->type != TTT_CHILD) ? 
			token->text_box : child_box;
		const char *token_box_string = token_box != NULL ? 
			get_box_debug_string(token_box) : "none";
		uint32_t text_style_key = token->child != NULL ? 
			get_style(token->child)->text.key : 0;
		dmsg("%3u: %8s start: %4u end: %4u width: %.2f height: %.2f "
			"child: %s/%.8Xh tsk: %.8Xh box: %s "
			"positioned: %u multipart_head: %u multipart_tail: %u\n",
			i, INLINE_TOKEN_STRINGS[token->type], 
			token->start, token->end,
			token->width, token->height,
			child_type_string, uint32_t(token->child),
			text_style_key,
			token_box_string,
			(token->flags & ITF_POSITIONED) != 0,
			(token->flags & ITF_MULTIPART_HEAD) != 0,
			(token->flags & ITF_MULTIPART_TAIL) != 0);
	}
	dmsg("\n");
}

/* Prints all inline contexts under the specified root. */
void dump_all_inline_contexts(const Document *document, const Node *root)
{
	for (const Node *child = root; child != NULL; 
		child = tree_next(document, root, child)) {
		if (get_layout(child) == LAYOUT_INLINE_CONTAINER)
			dump_inline_context(document, child);
	}
}

void dump_boxes(const Document *document, const Box *box, unsigned indent)
{
	if (box == NULL) {
		dmsg("Empty box tree.\n");
		return;
	}
		
	const Node *node = box->owner;
	const char *node_name = node != NULL ? 
		NODE_TYPE_STRINGS[get_type(node)] : "NULL";

	/* Count the box's children. */
	unsigned num_children = 0;
	for (const Box *child = box->first_child; child != NULL; 
		child = child->next_sibling)
		num_children++;

	/* Print debug information. */
	dmsg( 
		"% *sBox "
		"[%s] node=%s axis=%d children=%d "
		"\n% *s    | "
		"ideal=(%.2f/%d, %.2f/%d) "
		"sizes_from_child=(%.2f, %.2f) sizes_from_parent=(%.2f, %.2f) "
		"pos=(%.2f, %.2f) "
		"clip=(%.2f, %.2f, %.2f, %.2f) "
		"\n% *s    | "
		"mm_x=(%.2f/%d, %.2f/%d), "
		"mm_y=(%.2f/%d, %.2f/%d), "
		"align=%d, arrange=%d, "
		"pad=(%.2f/%d, %.2f/%d, %.2f/%d, %.2f/%d) "
		"mrg=(%.2f/%d, %.2f/%d, %.2f/%d, %.2f/%d)"
		"\n% *s    | "
		"depends_on_parent[pre_text_layout]=(%u, %u), "
		"depends_on_children[pre_text_layout]=(%u, %u), "
		"\n% *s    | "
		"depends_on_parent[post_text_layout]=(%u, %u), "
		"depends_on_children[post_text_layout]=(%u, %u), "
		"\n% *s    | "
		"preorder[pre_text_layout]=(%u, %u), "
		"postorder[pre_text_layout]=(%u, %u), "
		"\n% *s    | "
		"preorder[post_text_layout]=(%u, %u), "
		"postorder[post_text_layout]=(%u, %u), "
		"\n% *s    | "
		"set_by_parent=(%u, %u)"
		"\n% *s    | "
		"tokens=(%d, %d)"
		"\n", 
		indent, "", get_box_debug_string(box), node_name, (int)box->axis, num_children,
		indent, "",
		box->ideal[AXIS_H], (int)box->mode_dim[AXIS_H], box->ideal[AXIS_V], (int)box->mode_dim[AXIS_V],
		get_size_directional(box, AXIS_H, false), 
		get_size_directional(box, AXIS_V, false),
		get_size_directional(box, AXIS_H, true), 
		get_size_directional(box, AXIS_V, true),
		box->pos[AXIS_H], box->pos[AXIS_V],
		box->clip[0], box->clip[1], box->clip[2], box->clip[3],
		indent, "",
		box->min[AXIS_H], (int)box->mode_min[AXIS_H], box->max[AXIS_H], (int)box->mode_max[AXIS_H],
		box->min[AXIS_V], (int)box->mode_min[AXIS_V], box->max[AXIS_V], (int)box->mode_max[AXIS_V],
		(int)box->alignment, (int)box->arrangement,
		box->pad_lower[AXIS_H], box->mode_pad_lower[AXIS_H], box->pad_lower[AXIS_V], box->mode_pad_lower[AXIS_V],
		box->pad_upper[AXIS_H], box->mode_pad_upper[AXIS_H], box->pad_upper[AXIS_V], box->mode_pad_upper[AXIS_V],
		box->margin_lower[AXIS_H], box->mode_margin_lower[AXIS_H], box->margin_lower[AXIS_V], box->mode_margin_lower[AXIS_V],
		box->margin_upper[AXIS_H], box->mode_margin_upper[AXIS_H], box->margin_upper[AXIS_V], box->mode_margin_upper[AXIS_V],
		indent, "",
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_WIDTH_DEPENDS_ON_PARENT) != 0,
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_HEIGHT_DEPENDS_ON_PARENT) != 0,
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_WIDTH_DEPENDS_ON_CHILDREN) != 0,
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_HEIGHT_DEPENDS_ON_CHILDREN) != 0,
		indent, "",
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_WIDTH_DEPENDS_ON_PARENT) != 0,
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_HEIGHT_DEPENDS_ON_PARENT) != 0,
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_WIDTH_DEPENDS_ON_CHILDREN) != 0,
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_HEIGHT_DEPENDS_ON_CHILDREN) != 0,
		indent, "",
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_WIDTH_PREORDER) != 0,
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_HEIGHT_PREORDER) != 0,
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_WIDTH_POSTORDER) != 0,
		(box->pass_flags[PASS_PRE_TEXT_LAYOUT] & PASSFLAG_HEIGHT_POSTORDER) != 0,
		indent, "",
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_WIDTH_PREORDER) != 0,
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_HEIGHT_PREORDER) != 0,
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_WIDTH_POSTORDER) != 0,
		(box->pass_flags[PASS_POST_TEXT_LAYOUT] & PASSFLAG_HEIGHT_POSTORDER) != 0,
		indent, "",
		(box->flags & BOXFLAG_WIDTH_SET_BY_PARENT) != 0,
		(box->flags & BOXFLAG_HEIGHT_SET_BY_PARENT) != 0,
		indent, "",
		box->token_start, box->token_end
	);

	/* Print the box's children. */
	for (const Box *child = box->first_child; 
		child != NULL; child = child->next_sibling)
		dump_boxes(document, child, indent + 4);
}


} // namespace stkr

