#include "stacker_diagnostics.h"
#include "stacker_shared.h"
#include "stacker_util.h"
#include "stacker_token.h"
#include "stacker_document.h"
#include "stacker_node.h"
#include "stacker_box.h"
#include "stacker_inline2.h"
#include "stacker_paragraph.h"

namespace stkr {

void dump_discard(void *data, const char *fmt, va_list args) { data; fmt; args; }

/* Prints a list of paragraph elements. */
void dump_paragraph_elements(const Document *document, 
	const ParagraphElement *elements, unsigned count)
{
	dmsg("PARAGRAPH ELEMENTS [num_elements: %u]\n", count);
	for (unsigned i = 0; i < count; ++i) {
		ParagraphElement e = elements[i];
		char character = (e.code_point >= 0x20 && e.code_point <= 0x7F) ? 
			(char)e.code_point : '?';
		dmsg("\t%3u: code_point: U+%04X (\"%c\") "
			"advance: %4u "
			"penalty_type: %u " 
			"is_word_end: %u "
			"is_inline_object: %u "
			"is_node_first: %u "
			"is_selected: %u\n",
			i, e.code_point, character, e.advance,
			e.penalty_type,
			e.is_word_end, 
			e.is_inline_object, 
			e.is_node_first,
			e.is_selected
		);
	}
	dmsg("END PARAGRAPH ELEMENTS\n");
}

/* Prints a paragraph line list. */
void dump_line_list(const Document *document, const LineList *lines)
{
	dmsg("LINE LIST [num_lines: %u line_width: %d capacity: %u]\n", 
		lines->num_lines, lines->max_width, lines->capacity);
	for (unsigned i = 0; i < lines->num_lines; ++i) {
		const ParagraphLine *line = lines->lines + i;
		dmsg("\t%3u: a:%3u b:%3u "
			"adjustment_ratio: %8.3f "
			"line_demerits: %10d "
			"cumulative_demerits: %10d "
			"width: %5u "
			"height: %5u\n", 
			i, line->a, line->b, 
			fixed_to_double(line->adjustment_ratio, TEXT_METRIC_PRECISION), 
			line->line_demerits,
			line->demerits,
			line->width,
			line->height);
	}
	dmsg("END LINE LIST\n");
}

/* Prints node information. */
void dump_node(const Document *document, const Node *node, unsigned indent)
{
	/* Print the node's name. */
	dmsg("% *s-> %s sel: %u", 
		indent, "", get_node_debug_string(node),
		(node->t.flags & NFLAG_IN_SELECTION_CHAIN) != 0);

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
		child = child->t.next.node)
		dump_node(document, child, indent + 6);
}

/* Prints all information in an inline context. */
void dump_inline_context(const Document *document, const Node *node)
{
	const InlineContext *icb = node->icb;
	if (icb == NULL) {
		dmsg("%s node %.8Xh has no inline context.", 
			NODE_TYPE_STRINGS[get_type(node)], uint32_t(node));
	} else {
		dump_paragraph_elements(document, icb->elements, icb->num_elements);
		if (icb->lines != NULL)
			dump_line_list(document, icb->lines);
		else
			dmsg("--- NO LINE LIST ---\n");
	}
	dmsg("\n");
}

/* Prints all inline contexts under the specified root. */
void dump_all_inline_contexts(const Document *document, const Node *root)
{
	const Node *child = root;
	do {
		if (get_layout(child) == LAYOUT_INLINE_CONTAINER)
			dump_inline_context(document, child);
		child = (const Node *)tree_next(&root->t, 
			&child->t);
	} while (child != NULL);
}

/* Prints a box tree. */
void dump_boxes(const Document *document, const Box *box, unsigned indent)
{
	if (box == NULL) {
		dmsg("Empty box tree.\n");
		return;
	}
		
	const Node *node = box->t.counterpart.node;
	const char *node_name = node != NULL ? 
		NODE_TYPE_STRINGS[get_type(node)] : "NULL";

	unsigned num_children = tree_count_children(&box->t);

	dmsg( 
		"% *sBox "
		"[%s] node: %s axis: %d children: %d "
		"\n% *s    | "
		"ideal: (%.2f/%d, %.2f/%d) "
		"extrinsic: (%.2f, %.2f) "
		"intrinsic: (%.2f, %.2f), "
		"preferred: (%.2f, %.2f), "
		"\n% *s    | "
		"pos: (%.2f, %.2f) "
		"clip: (%.2f, %.2f, %.2f, %.2f) "
		"\n% *s    | "
		"mm_x: (%.2g/%d, %.2g/%d), "
		"mm_y: (%.2g/%d, %.2g/%d), "
		"align: %d, arrange: %d, "
		"\n% *s    | "
		"pad: (%.2f/%d, %.2f/%d, %.2f/%d, %.2f/%d) "
		"mrg: (%.2f/%d, %.2f/%d, %.2f/%d, %.2f/%d)"
		"\n% *s    | "
		"cell_code: %.8Xh "
		"paragraph_elements: [%d, %d)"
		"\n% *s    | "
		"dop: (%d, %d) doa: (%d, %d) doc: (%d, %d) hdc: (%d, %d) iadc: (%d, %d) cyc: (%d, %d)"
		"\n", 
		indent, "", get_box_debug_string(box), node_name, box_axis(box), num_children,
		indent, "",
		box->axes[AXIS_H].sizes[SSLOT_IDEAL], box->axes[AXIS_H].mode_dim, box->axes[AXIS_V].sizes[SSLOT_IDEAL], box->axes[AXIS_V].mode_dim,
		box->axes[AXIS_H].sizes[SSLOT_EXTRINSIC], box->axes[AXIS_V].sizes[SSLOT_EXTRINSIC],
		box->axes[AXIS_H].sizes[SSLOT_INTRINSIC], box->axes[AXIS_V].sizes[SSLOT_INTRINSIC],
		box->axes[AXIS_H].sizes[SSLOT_PREFERRED], box->axes[AXIS_V].sizes[SSLOT_PREFERRED],
		indent, "",
		box->axes[AXIS_H].pos, box->axes[AXIS_V].pos,
		box->clip[0], box->clip[1], box->clip[2], box->clip[3],
		indent, "",
		box->axes[AXIS_H].min, box->axes[AXIS_H].mode_min, box->axes[AXIS_H].max, box->axes[AXIS_H].mode_max,
		box->axes[AXIS_V].min, box->axes[AXIS_V].mode_min, box->axes[AXIS_V].max, box->axes[AXIS_V].mode_max,
		box_alignment(box), box_arrangement(box),
		indent, "",
		box->axes[AXIS_H].pad_lower, box->axes[AXIS_H].mode_pad_lower, box->axes[AXIS_V].pad_lower, box->axes[AXIS_V].mode_pad_lower,
		box->axes[AXIS_H].pad_upper, box->axes[AXIS_H].mode_pad_upper, box->axes[AXIS_V].pad_upper, box->axes[AXIS_V].mode_pad_upper,
		box->axes[AXIS_H].margin_lower, box->axes[AXIS_H].mode_margin_lower, box->axes[AXIS_V].margin_lower, box->axes[AXIS_V].mode_margin_lower,
		box->axes[AXIS_H].margin_upper, box->axes[AXIS_H].mode_margin_upper, box->axes[AXIS_V].margin_upper, box->axes[AXIS_V].mode_margin_upper,
		indent, "",
		box->cell_code, box->first_element, box->last_element,
		indent, "",
		(box->layout_flags & axisflag(AXIS_H, AXISFLAG_DEPENDS_ON_PARENT)) != 0,
		(box->layout_flags & axisflag(AXIS_V, AXISFLAG_DEPENDS_ON_PARENT)) != 0,
		(box->layout_flags & axisflag(AXIS_H, AXISFLAG_DEPENDS_ON_ANCESTOR)) != 0,
		(box->layout_flags & axisflag(AXIS_V, AXISFLAG_DEPENDS_ON_ANCESTOR)) != 0,
		(box->layout_flags & axisflag(AXIS_H, AXISFLAG_DEPENDS_ON_CHILDREN)) != 0,
		(box->layout_flags & axisflag(AXIS_V, AXISFLAG_DEPENDS_ON_CHILDREN)) != 0,
		(box->layout_flags & axisflag(AXIS_H, AXISFLAG_HAS_DEPENDENT_CHILD)) != 0,
		(box->layout_flags & axisflag(AXIS_V, AXISFLAG_HAS_DEPENDENT_CHILD)) != 0,
		(box->layout_flags & axisflag(AXIS_H, AXISFLAG_IN_ANCESTRAL_DEPENDENCE_CHAIN)) != 0,
		(box->layout_flags & axisflag(AXIS_V, AXISFLAG_IN_ANCESTRAL_DEPENDENCE_CHAIN)) != 0,
		(box->layout_flags & axisflag(AXIS_H, AXISFLAG_CYCLE)) != 0,
		(box->layout_flags & axisflag(AXIS_V, AXISFLAG_CYCLE)) != 0
	);

	/* Print the box's children. */
	for (const Box *child = box->t.first.box; child != NULL; 
		child = child->t.next.box)
		dump_boxes(document, child, indent + 4);
}

} // namespace stkr

