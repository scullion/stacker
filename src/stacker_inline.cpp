#include "stacker_inline.h"

#include <algorithm>
#include <numeric>

#include "stacker_shared.h"
#include "stacker_util.h"
#include "stacker_node.h"
#include "stacker_document.h"
#include "stacker_box.h"
#include "stacker_layout.h"
#include "stacker_paragraph.h"
#include "stacker_layer.h"
#include "stacker_platform.h"
#include "stacker_system.h"

namespace stkr {

/* Returns the caret address before a node. */
CaretAddress start_address(const Document *document, const Node *node)
{
	CaretAddress address = { node, 0 };
	return canonical_address(document, address);
}

/* Returns the caret address after a node. */
CaretAddress end_address(const Document *document, const Node *node)
{
	CaretAddress address = { node, IA_END };
	return canonical_address(document, address);
}

/* Modifies a caret address so that if its node is the child of an inline 
 * container, the modified address refers to the container, and gives the
 * position of the child as an internal address. */
CaretAddress canonical_address(const Document *document, CaretAddress address)
{
	const Node *node = address.node;
	if (node != NULL) {
		const Node *container = find_inline_container_not_self(document, node);
		if (container != NULL) {
			address.node = container;
			address.offset = inline_before(container->icb, node);
		}
	}
	return address;
}

unsigned inline_element_index(const InlineContext *icb, unsigned element)
{
	/* Note that if num_elements is zero here, the result wraps around to 
	 * IA_END. */
	return (element == IA_END) ? icb->num_elements - 1 : element;
}

ParagraphElement inline_element(const InlineContext *icb, unsigned index)
{
	index = inline_element_index(icb, index);
	return index < icb->num_elements ? icb->elements[index] : 0;
}

/* True if two internal addreses refer to the same position. */
bool same_internal_address(const Node *node, unsigned a, unsigned b)
{
	unsigned ea = inline_element_index(node, a);
	unsigned eb = inline_element_index(node, b);
	return ea == eb;
}

/* Returns the index of the first token in an inline container with the 
 * specified  child node, or IA_END if if there is no such token. */
unsigned inline_find_child_token(const InlineContext *icb, const Node *child)
{
	unsigned token;
	for (token = 0; token < icb->num_tokens; ++token)
		if (icb->tokens[token].child == child)
			return token;
	return IA_END;
}

unsigned inline_before(const InlineContext *icb, const Node *child)
{
	return child->first_element;
}

unsigned inline_after(const InlineContext *icb, const Node *child)
{
	return child->first_element + child->text_length;
}

static InternalAddress closer_end(const Node *node, InternalAddress ia, 
	AddressRewriteMode mode)
{
	if (node->layout == LAYOUT_INLINE_CONTAINER) {
		const InlineContext *icb = node->icb;
		bool after;
		if (mode == ARW_TIES_TO_CLOSER) {
			unsigned icb_offset = address_to_icb_offset(icb, ia);
			after = icb_offset >= icb->text_length / 2;
		} else {
			if (same_internal_address(node, ia, INLINE_START))
				after = false;
			else if (same_internal_address(node, ia, INLINE_END))
				after = true;
			else 
				after = (mode == ARW_TIES_TO_END);
		}
		ia = after ? INLINE_END : INLINE_START;
	}
	return ia;
}

CaretAddress rewrite_address(const Document *document, const Node *parent, 
	CaretAddress address, AddressRewriteMode mode = ARW_TIES_TO_CLOSER)
{
	while (address.node != NULL && address.node != parent) {
		address.ia = closer_end(address.node, address.ia, mode);
		const Node *container = find_inline_container_not_self(document, address.node);
		if (container != NULL) {
			address.ia.token = inline_find_child_token(
				container->icb, address.node);
			address.node = container;
		} else {
			address.node = address.node->parent;
		}
	}
	return canonical_address(document, address);
}

inline bool internal_address_less(InternalAddress a, InternalAddress b)
{
	return (a.token != b.token) ? a.token < b.token : a.offset < b.offset;
}

InternalAddress closest_internal_address(const Document *document, 
	const Node *node, CaretAddress address, AddressRewriteMode mode)
{
	/* Try to rewrite the address in terms of 'node'. If we succeed, the address
	 * is inside the subtree of 'node' and we have the closest position. */
	CaretAddress b_wrt_a = rewrite_address(document, node, address, mode);
	if (b_wrt_a.node != NULL)
		return b_wrt_a.ia;

	/* Construct positions at the ends of 'node' and try to rewrite them in
	 * terms of address.node. If we succeed, 'node' is within the subtree of
	 * 'address.node', and the result is whichever end of 'node' is closer
	 * to the address. */
	CaretAddress a0_wrt_b = rewrite_address(document, address.node, 
		start_address(document, node), mode);
	CaretAddress a1_wrt_b = rewrite_address(document, address.node, 
		end_address(document, node), mode);
	if (a0_wrt_b.node != NULL) {
		InternalAddress ia_a0 = expand_internal_address(node, a0_wrt_b.ia);
		InternalAddress ia_a1 = expand_internal_address(node, a1_wrt_b.ia);
		InternalAddress ia_b = expand_internal_address(address.node, address.ia);
		if (!internal_address_less(ia_a0, ia_b)) return INLINE_START;
		if (!internal_address_less(ia_b, ia_a1)) return INLINE_END;
		/* The address is inside the interval occupied by 'node' within 
		 * 'address.node'. */
		return closer_end(node, ia_b, mode);
	}

	/* The node and the address are in different subtrees. Return a position at 
	 * the  beginning or end of the node depending on their tree order. */
	return node_before(address.node, node) ? INLINE_START : INLINE_END;
}

const Node *inline_node_at(const InlineContext *icb, InternalAddress ia)
{
	const InlineToken *token = inline_element(icb, ia.token);
	return token != NULL ? token->child : NULL;
}

/* FIXME (TJM): maybe define internal addresses as (child, offset). not like
 * it's any more  fragile than what we do now, since tokens get rebuilt if the
 * children change at all. 
 * 
 * why not just use CaretPosition everywhere instead? that's already (node, offset). seems more natural now.
 * 
 * currently the way we use style segments is to just iterate over all tokens, then each style segment within a token.
 * we don't have tokens any more. */

/* Returns the node containing a caret address. This is different from the 
 * 'node' field in the address structure, which will contain the inline 
 * container that the address's internal adress refers to. */
const Node *node_at_caret(CaretAddress address)
{
	const Node *node = address.node;
	if (node != NULL && node->layout == LAYOUT_INLINE_CONTAINER)
		node = inline_node_at(node->icb, address.ia);
	return node;
}

/* True if everything inside a node is selected. */
bool is_fully_selected(const Document *document, const Node *node)
{
	if ((node->flags & NFLAG_IN_SELECTION_CHAIN) == 0)
		return false;
	if (node->layout != LAYOUT_INLINE_CONTAINER)
		return node->t.first == NULL || 
			is_fully_selected(document, node->first_child) && 
			is_fully_selected(document, node->last_child);
	const InlineContext *icb = node->icb;
	return same_internal_address(node, icb->selection_start, INLINE_START) && 
		same_internal_address(node, icb->selection_end, INLINE_END);
}

/* Initializes a caret-walk iterator, returning the first node. The mask is a
 * node flag the walker needs to keep track of nodes it has visited. It must
 * be clear in all nodes before iteration starts. */
Node *cwalk_first(Document *document, CaretWalker *w, CaretAddress start, 
	CaretAddress end, unsigned mask)
{
	w->start = canonical_address(document, start);
	w->end = canonical_address(document, end);
	if (caret_before(w->end, w->start))
		std::swap(w->start, w->end);
	w->end_node = (Node *)node_at_caret(w->end);
	w->node = (Node *)w->start.node;
	w->back = (Node *)node_at_caret(w->start);
	if (w->back == w->node)
		w->back = NULL;
	w->mask = mask;
	w->node->t.flags |= mask;
	return w->node;
}

/* Returns the next node between two caret positions. */
Node *cwalk_next(Document *document, CaretWalker *w)
{
	if (w->node == NULL || w->node == w->end_node)
		return NULL;
	if (w->back != NULL) {
		w->node = w->back;
		w->back = NULL;
	} else if (w->node->t.first != NULL && 
		(w->node->t.first->flags & w->mask) == 0) {
		w->node = w->node->t.first.node;
	} else if (w->node->t.next != NULL && 
		(w->node->t.next->flags & w->mask) == 0) {
		w->node = w->node->t.next.node;
		/* If the current node is an inline child, make sure we've visited
		 * its parent before moving on to the next sibling. */
		Node *container = (Node *)find_inline_container_not_self(document, w->node);
		if (container != NULL && (container->flags & w->mask) == 0) {
			w->back = w->node;
			w->node = container;
		}
	} else {
		w->node = w->node->parent;
		while (w->node != NULL && (w->node->flags & w->mask) != 0)
			w->node = (Node *)tree_next_up(document, NULL, w->node);
	}
	if (w->node != NULL)
		w->node->flags |= w->mask;
	return w->node;
}

/* Determines the (token, character) position in a sequence of tokens, which
 * are taken to be horizontally adjacent, at which a caret should be placed
 * in order to be as close as possible to the offset 'x'. */
CaretAddress caret_position(Document *document, const Box *box, float x)
{
	CaretAddress address = { NULL, 0, 0 };
	address.node = find_layout_node(document, box->node);
	if (address.node == NULL)
		return address;

	/* If the box represents a block node, position the caret at offset zero or
	 * one according to whether the query offset is left or right of centre. */
	float dx = x - box->axes[AXIS_H].pos;
	if (address.node->layout == LAYOUT_BLOCK) {
		float mid = 0.5f * outer_dim(box, AXIS_H);
		address.ia.offset = dx < mid ? 0 : IA_END;
		return address;
	}

	/* The box is part of an inline context. */
	const InlineContext *icb = address.node->icb;
	unsigned token_start = box->token_start;
	unsigned token_end = box->token_end;

	/* Iterate over the segments of the tokens the box positions to find the 
	 * inline child the query point is within. */
	float token_x0 = box->axes[AXIS_H].margin_lower + box->axes[AXIS_H].pad_lower;
	bool hit_token = false;
	for (unsigned i = token_start; i != token_end && !hit_token; ++i) {
		/* Is the query offset within this token? */
		const InlineToken *token = icb->tokens + i;
		float token_x1 = token_x0 + token->width;
		if (dx >= token_x1) {
			if (i + 1 == token_end) {
				/* The offset is to the right of the rightmost token. */
				address.ia.token = i;
				address.ia.offset = IA_END;
				break;
			}
			token_x0 = token_x1;
			continue;
		}

		/* For non-text tokens, report offset zero or one based on whether the
		 * query offset is in the left or right hand side of the box. */
		address.ia.token = i;
		hit_token = true;
		if (token->type == TTT_CHILD) {
			float mid = 0.5f * (token_x0 + token_x1);
			address.ia.offset = dx < mid ? 0 : IA_END;
		} else {
			float char_x0 = token_x0;
			unsigned j;
			for (j = token->start; j != token->end; ++j) {
				float char_x1 = char_x0 + float(icb->advances[j]);
				float char_mid = 0.5f * (char_x0 + char_x1);
				if (dx <= char_mid)
					break;
				char_x0 = char_x1;
			}
			address.ia.offset = j - token->start;
		}
	}
	return address;
}

/* True if position A is before position B. */
bool caret_before(CaretAddress a, CaretAddress b)
{
	const Node *na = node_at_caret(a);
	const Node *nb = node_at_caret(b);
	if (na != nb)
		return node_before(na, nb);
	if (a.ia.token != b.ia.token)
		return a.ia.token < b.ia.token;
	return a.ia.offset < b.ia.offset;
}

/* True if position A is equal to position B. */
bool caret_equal(CaretAddress a, CaretAddress b)
{
	return a.node == b.node && 
		a.ia.token == b.ia.token && 
		a.ia.offset == b.ia.offset;
}

/* Determines the document space horizontal interval occupied by the selected 
 * part of a token range. */
static bool selection_interval(const Document *document, const Node *node,
	unsigned token_start, unsigned token_end, float *sel_x0, float *sel_x1)
{
	assertb(node->layout == LAYOUT_INLINE_CONTAINER);
	const InlineContext *icb = node->icb;

	/* Does the token intersect with the ICB's selection? Note that j is the
	 * token where the selection ends, so [i, j] is an open interval. */
	unsigned i = inline_element_index(icb, icb->selection_start.token);
	unsigned j = inline_element_index(icb, icb->selection_end.token);
	if (!overlap(token_start, token_end, i, j + 1))
		return false;

	const InlineToken *first, *last;
	unsigned offset_first, offset_last;
	if (i >= token_start && i < token_end) {
		first = icb->tokens + i;
		offset_first = icb->selection_start.offset;
	} else {
		first = icb->tokens + token_start;
		offset_first = 0;
	}
	if (j >= token_start && j < token_end) {
		last = icb->tokens + j;
		offset_last = icb->selection_end.offset;
	} else {
		last = icb->tokens + token_end - 1;
		offset_last = IA_END;
	}
	*sel_x0 = token_character_position(document, node, first, offset_first);
	*sel_x1 = token_character_position(document, node, last, offset_last);
	return true;
}

/* Returns the range of tokens whose positions are determined by the children
 * of the supplied box. */
static bool line_box_token_range(const Box *line_box, unsigned *token_start, 
	unsigned *token_end)
{
	const Box *first = line_box->first_child;
	const Box *last = line_box->last_child;
	if (first == NULL)
		return false;
	*token_start = first->token_start;
	*token_end = last->token_end;
	return true;
}

/* Recreates the plane layer used by line boxes as a text selection 
 * background. */
static void update_line_box_selection_layer(Document *document, 
	Node *node, Box *line_box)
{
	VisualLayer *layer = layer_chain_find(VLCHAIN_BOX, line_box->layers, 
		LKEY_SELECTION);

	/* If the node is an inline child that is fully selected, let the parent
	 * draw the selection box. */
	const Node *container = find_chain_inline_container(document, node);
	bool selected_in_parent = container != NULL && 
		(container->t.flags & NFLAG_IN_SELECTION_CHAIN) != 0 &&
		is_fully_selected(document, node);

	/* Determine the selection interval. */
	bool layer_required = false;
	float sel_x0 = 0.0f, sel_x1 = 0.0f;
	if (!selected_in_parent && (node->t.flags & NFLAG_IN_SELECTION_CHAIN) != 0) {
		unsigned token_start, token_end;
		if (line_box_token_range(line_box, &token_start, &token_end)) {
			layer_required = selection_interval(document, node, 
				token_start, token_end, &sel_x0, &sel_x1);
		}
	}

	/* If there's no selection, delete any existing layer. */
	if (!layer_required) {
		if (layer != NULL) {
			layer_chain_remove(VLCHAIN_BOX, &line_box->layers, layer);
			destroy_layer(document, layer);
		}
		return;
	}

	/* Create or update the selection layer. */
	if (layer == NULL) {
		layer = create_layer(document, node, VLT_PANE);
		layer_chain_insert(VLCHAIN_BOX, &line_box->layers, layer, LKEY_SELECTION);
		layer->pane.pane_type = PANE_FLAT;
		layer->pane.fill_color = document->selected_text_fill_color;
		layer->depth_offset = -1; /* Behind the text layers of the inline container. */
	}
	float sel_width = sel_x1 - sel_x0;
	layer->pane.position.alignment[AXIS_H] = ALIGN_START;
	layer->pane.position.mode_size[AXIS_H] = DMODE_ABSOLUTE;
	layer->pane.position.dims[AXIS_H] = sel_width;
	layer->pane.position.mode_offset[AXIS_H] = DMODE_ABSOLUTE;
	layer->pane.position.offsets[AXIS_H] = sel_x0 - line_box->axes[AXIS_H].pos;
}

/* Recreates selection highlight layers for an inline context. */
void update_inline_selection_layers(Document *document, Node *node)
{
	Box *container = node->t.counterpart.box;
	for (Box *line_box = first_child_box(container); line_box != NULL; 
		line_box = line_box->t.next.box) {
		update_line_box_selection_layer(document, node, line_box);
	}
}

static int itok_next_char(InlineTokenizer *tt)
{
	if (tt->repeat_count != 0) {
		tt->repeat_count--;
		return tt->pos.next_char;
	}
	/* Advance to the next character, skipping non-text nodes and empty text
	 * nodes. */
	if (*tt->pos.text != '\0') {
		tt->pos.text++;
		tt->pos.child_offset++;
	}
	while (*tt->pos.text == '\0') {
		tt->pos.child = inline_next(tt->document, tt->root, tt->pos.child);
		if (tt->pos.child == NULL) {
			tt->pos.next_char = 0;
			tt->pos.child_offset = 0;
			return 0;
		} else if (tt->pos.child->layout != LAYOUT_INLINE) {
			tt->pos.next_char = ITOK_CHILD;
			tt->pos.child_offset = 0;
			return ITOK_CHILD;
		}
		tt->pos.text = tt->pos.child->text;
		tt->pos.child_offset = 0;
	}
	tt->pos.next_char = int(*tt->pos.text);
	if (tt->mode == WSM_PRESERVE) {
		if (tt->pos.next_char == '\t') {
			tt->pos.next_char = ' ';
			tt->repeat_count = TAB_WIDTH;
		} else if (tt->pos.next_char == '\r') {
			return itok_next_char(tt);
		}
	}
	return tt->pos.next_char;
}

inline void itok_push_char(InlineTokenizer *tt, InlineTokenType type, char ch)
{
	bool emit = (tt->mask >> type) & 1;
	if (emit && tt->text != NULL)
		tt->text[tt->text_length] = ch;
	tt->text_length += (unsigned)emit;
}

static void itok_init(InlineTokenizer *tt, const Document *document, 
	const Node *root, WhiteSpaceMode mode, unsigned max_chunk_length = UINT_MAX, 
	char *text = NULL, InlineToken *tokens = NULL)
{
	tt->document = document;
	tt->root = root;
	tt->mask = (1 << TTT_WORD) | (1 << TTT_CHILD);
	if (mode == WSM_PRESERVE)
		tt->mask |= (1 << TTT_SPACE) | (1 << TTT_BREAK);
	tt->mode = mode;
	tt->repeat_count = 0;
	tt->pos.child_offset = 0;
	tt->text = text;
	tt->text_length = 0;
	tt->token.type = TTT_EOS;
	tt->token.flags = 0;
	tt->token.start = 0;
	tt->token.end = 0;
	tt->tokens = tokens;
	tt->num_tokens = 0;
	tt->chunk_length = 0;
	tt->max_chunk_length = max_chunk_length;
	tt->pos.child = root;
	tt->pos.text = root->text;
	tt->pos.next_char = *root->text;
	if (tt->pos.next_char == 0)
		itok_next_char(tt);
}

static InlineTokenType itok_next(InlineTokenizer *tt)
{
	InlineToken *token = &tt->token;
	token->text_box = NULL;
	
	token->start = token->end;
	do {
		unsigned last_token_flags = token->flags;
		token->child = tt->pos.child;
		token->child_offset = tt->pos.child_offset;
		token->flags = 0;
		
		int ch = tt->pos.next_char;
		if (ch == '\n') {
			token->type = TTT_BREAK;
			itok_push_char(tt, TTT_BREAK, (char)ch);
			itok_next_char(tt);
		} else if (ch == '\0') {
			token->type = TTT_EOS;
			return TTT_EOS;
		} else if (ch == ITOK_CHILD) {
			token->type = TTT_CHILD;
			itok_next_char(tt);
		} else if (!isspace(ch)) {
			InlineTokenizerPosition start_pos = tt->pos;
			unsigned word_start = tt->text_length;
			token->type = TTT_WORD;
			bool hyphen;
			unsigned word_length = 0;
			bool chunk = tt->chunk_length != 0;
			do {
				++word_length;
				hyphen = (ch == '-');
				if (!chunk)
					itok_push_char(tt, TTT_WORD, (char)ch);
				ch = itok_next_char(tt);
				if (chunk && (word_length == tt->chunk_length || 
					token->start + word_length == tt->text_length))
					break;
			} while (ch > 0 && !isspace(ch) && ch != '\0' && !hyphen);
			if ((last_token_flags & (ITF_MULTIPART_HEAD | ITF_MULTIPART_TAIL)) != 0)
				token->flags |= ITF_MULTIPART_TAIL;
			else if (hyphen)
				token->flags |= ITF_MULTIPART_HEAD;
			if (chunk) {
				token->end = token->start + word_length;
				if (token->end == tt->text_length)
					tt->chunk_length = 0;
				return token->type;
			} else if (word_length > tt->max_chunk_length) {
				/* If the word is longer that the maximum allowed chunk length,
				 * break it into equal parts. */
				unsigned num_chunks = word_length / tt->max_chunk_length;
				num_chunks += (num_chunks * tt->max_chunk_length) != word_length;
				tt->chunk_length = word_length / num_chunks;
				/* Rewind to the beginning of the word and read it again in
				 * chunks. */
				tt->pos = start_pos;
				token->end = word_start;
				itok_next(tt);
				token->flags = ITF_MULTIPART_HEAD;
				return token->type;
			}
		} else {
			token->type = TTT_SPACE;
			do {
				itok_push_char(tt, TTT_SPACE, (char)ch);
				ch = itok_next_char(tt);
			} while (ch > 0 && (ch == ' ' || ch == '\t' || ch == '\r'));
		}
	} while ((tt->mask & (1 << token->type)) == 0);
	token->end = tt->text_length;
	return token->type;
}

static void itok_tokenize(InlineTokenizer *tt)
{
	/* Read until EOS, appending each token to the output array. */
	InlineTokenType type;
	while ((type = itok_next(tt)) != TTT_EOS) {
		if (tt->tokens != NULL)
			tt->tokens[tt->num_tokens] = tt->token;
		tt->num_tokens++;
	}
}

TextSegment token_first_segment(const Node *container, const InlineToken *token)
{
	container;
	TextSegment segment;
	unsigned child_end = token->start + token->child->text_length - 
		token->child_offset;
	segment.start = token->start;
	segment.end = std::min(token->end, child_end);
	segment.child_offset = token->child_offset;
	segment.child = token->child;
	return segment;
}

TextSegment token_next_segment(const Node *container, const InlineToken *token, 
	const TextSegment *segment)
{
	TextSegment next;
	if (segment->end != token->end) {
		/* If the segment doesn't include all text from the current child within
		 * the token, it has been truncated. In that case the next segment is 
		 * the remainder of the text from the same child. Otherwise, the next 
		 * segment starts at the beginning of the next child. */
		unsigned child_remaining_in_token = std::min(
			segment->child->text_length - segment->child_offset, 
			token->end - segment->start);
		unsigned length = segment->end - segment->start;
		next.child = segment->child;
		if (length < child_remaining_in_token) {
			next.child_offset = segment->child_offset + length;
		} else {
			do {
				next.child = inline_next(segment->child->document, 
					container, next.child);
			} while (next.child->text_length == 0);
			next.child_offset = 0;
		}
		next.start = segment->end;
		next.end = std::min(token->end, next.start + next.child->text_length); 
	} else {
		next.child = NULL;
		next.child_offset = 0;
		next.start = next.end = token->end;
	}
	return next;
}

/* Returns the document space position of the L.H.S. of a particular character
 * in a token. */
float token_character_position(const Document *document, 
	const Node *node, const InlineToken *token, unsigned offset)
{
	document;

	const InlineContext *icb = node->icb;

	/* Calculate the L.H.S. position of the token within its run by adding up
	 * the widths of preceding tokens until we reach the first token positioned
	 * by the box. */
	const Box *box;
	float char_x0 = 0.0f;
	if (token->type != TTT_CHILD) {
		const InlineToken *t0 = token;
		while ((t0->flags & ITF_POSITIONED) == 0)
			char_x0 += (--t0)->width;
		box = t0->text_box;
	} else {
		box = token->child->box;
	}
	assertb(box != NULL);
	char_x0 += padding_edge_lower(box, AXIS_H);

	/* Fast paths for the L.H.S. and R.H.S. This also handles non-text tokens,
	 * which behave like a single character. */
	if (offset == 0)
		return char_x0;
	if (token->start + offset == token->end)
		offset = IA_END;
	if (offset == IA_END || token->type == TTT_CHILD)
		return char_x0 + token->width;

	/* Add in the width of the characters before the offset. */
	char_x0 += (float)std::accumulate(icb->advances + token->start,
		icb->advances + token->start + offset, 0);
	return char_x0;
}

/* Intersects a style segment with the containing inline context's selection
 * interval and, if required, truncates the segment and applies selection colour
 * modifications to the text style so that a style segment iteration stops at
 * selection boundaries. */
static void apply_selection_to_style_segment(const Document *document, 
	const Node *node, TextStyleSegment *ss)
{
	if ((node->t.flags & NFLAG_IN_SELECTION_CHAIN) == 0)
		return;
	const InlineContext *icb = node->icb;
	unsigned sel_start = address_to_icb_offset(icb, icb->selection_start);
	unsigned sel_end = address_to_icb_offset(icb, icb->selection_end);
	if (ss->segment.start < sel_start && ss->segment.end > sel_start) {
		/* The segment straddles the selection start. Truncate it to end at the
		 * selection start. */
		ss->segment.end = sel_start;
	} else if (ss->segment.start < sel_end && ss->segment.end > sel_end) {
		/* The segment straddles the selection end. Truncate it to end at
		 * the selection end. */
		ss->segment.end = sel_end;
	}
	/* If the segment starts inside the selection, apply selection colouring. */
	if (ss->segment.start >= sel_start && ss->segment.start < sel_end) {
		ss->style.color = document->selected_text_color;
		ss->style.flags |= SSF_SELECTED;
		update_text_style_key(&ss->style);
	}
}

TextStyleSegment token_first_style_segment(const Document *document,
	const Node *node, const InlineToken *token)
{
	TextStyleSegment ss;
	if (token->type == TTT_BREAK) {
		ss.segment.child = NULL;
		return ss;
	}
	ss.segment = token_first_segment(node, token);
	while (ss.segment.child != NULL && ss.segment.start == ss.segment.end)
		ss.segment = token_next_segment(node, token, &ss.segment);
	const Node *child = ss.segment.child;
	if (child != NULL) {
		ss.style = child->style.text;
		apply_selection_to_style_segment(document, node, &ss);
		ss.style.flags |= SSF_REMEASURE;
	}
	return ss;
}

TextStyleSegment token_next_style_segment(const Document *document,
	const Node *node, const InlineToken *token, const TextStyleSegment *ss)
{
	TextStyleSegment next;
	next.segment = token_next_segment(node, token, &ss->segment);
	while (next.segment.child != NULL && 
		next.segment.start == next.segment.end)
		next.segment = token_next_segment(node, token, &ss->segment);
	if (next.segment.child != NULL) {
		next.style = next.segment.child->style.text;
		apply_selection_to_style_segment(document, node, &next);
		if (ss->style.font_id != next.style.font_id)
			next.style.flags |= SSF_REMEASURE;
	}
	return next;
}

/* Sets the size of a TTT_CHILD token from the dimensions of the child box. */
static void update_child_token_size(Document *document, Node *node, 
	InlineToken *token)
{
	node;

	const Box *box = token->child->box;
	token->width = 0.0f;
	token->height = 0.0f;
	if (box != NULL) {
		/* Do a sizing pass on this node's subtree. This is necessary because
		 * if the node contains text, its size may depend on the layout of
		 * that text, which has only just been performed. We need to compute the
		 * size before building the element that represents the node in the 
		 * parent's paragraph. */
		layout(document, ((Node *)token->child)->box);
		/* Notice that the margins of the box are not included, because
		 * inline container itself controls the margins of inline 
		 * boxes. */
		token->width = padded_dim(box, AXIS_H);
		token->height = padded_dim(box, AXIS_V);
	}
}

/* Updates the size of each token in an inline context. */
void measure_inline_tokens(Document *document, Node *node, bool use_positioning)
{	
	InlineContext *icb = node->icb;
	int16_t measure_font_id = INVALID_FONT_ID;
	unsigned measure_start = 0, measure_end = 0;
	unsigned measure_height = 0;
	unsigned stop_mask = use_positioning ? 
		ITF_MULTIPART_TAIL | ITF_POSITIONED : 
		ITF_MULTIPART_TAIL;
	for (unsigned i = 0; i < icb->num_tokens; ++i) {
		InlineToken *token = icb->tokens + i;
		if (token->type != TTT_CHILD) {
			/* A text token. */
			token->height = 0.0f;
			token->width = 0.0f;
			TextStyleSegment ss = token_first_style_segment(document, node, token);
			while (ss.segment.child != NULL) {
				/* If the font changes here, remeasure to the end of the lexical
				 * token. It's important not to remeasure at segment boundaries 
				 * where only the colour changes, because measuring intra-word 
				 * segments separately causes visual problems with selection due 
				 * to the text being kerned differently when broken apart. */
				int16_t font_id = ss.style.font_id;
				if (ss.segment.start >= measure_end || measure_font_id != font_id) {
					/* Speculatively include any multipart-tail tokens immediately 
					 * following, stopping if we hit an ITF_POSITIONED tail (an
					 * itra-word break), since those should be measured 
					 * separately.*/
					unsigned j;
					for (j = i + 1; j < icb->num_tokens; ++j)
						if ((icb->tokens[j].flags & stop_mask) == 0)
							break;	
					measure_end = j != icb->num_tokens ? 
						icb->tokens[j].start : icb->text_length;
					measure_start = ss.segment.start;

					/* Measure the text. */
					unsigned measure_length = measure_end - measure_start;
					measure_text(document->system, ss.style.font_id, 
						icb->text + ss.segment.start, measure_length, 
						NULL, &measure_height, icb->advances + measure_start);
					measure_font_id = ss.style.font_id;
				}

				/* Expand the token. */
				token->width += (float)std::accumulate(
					icb->advances + ss.segment.start,
					icb->advances + ss.segment.end, 0);
				token->height = std::max(token->height, (float)measure_height);
				ss = token_next_style_segment(document, node, token, &ss);
			}
		} else  {
			/* A token representing a non-text child node. Its size is the
			 * size of the child's box. */
			update_child_token_size(document, node, token);
		}
	}
	node->t.flags &= ~NFLAG_REMEASURE_INLINE_TOKENS;
}

/* Destroys a node's inline context. */
void destroy_inline_context(Document *document, Node *node)
{
	InlineContext *ctx = node->icb;
	if (ctx != NULL) {
		destroy_owner_chain(document, ctx->text_boxes, false);
		delete [] (char *)ctx;
		node->icb = NULL;
	}
}

/* Rebuilds the inline context of a text container node. */
void rebuild_inline_context(Document *document, Node *node)
{
	destroy_inline_context(document, node);

	/* Read paragraph styles. */
	WhiteSpaceMode space_mode = (WhiteSpaceMode)node->style.white_space_mode;
	WrapMode wrap_mode = (WrapMode)node->style.wrap_mode;
	assertb((int)space_mode != ADEF_UNDEFINED);
	assertb((int)wrap_mode != ADEF_UNDEFINED);

	/* The wrap mode determines maximum chunk length. */
	unsigned max_chunk = (wrap_mode == WRAPMODE_CHARACTER) ? 1 : 8;

	/* Do a first pass to count the number of tokens and segments. */
	InlineTokenizer tt;
	itok_init(&tt, document, node, space_mode, max_chunk);
	itok_tokenize(&tt);
	
	/* Allocate a buffer big enough to hold the text of the token's we'll be
	 * storing and a segment array. */
	unsigned bytes_required = sizeof(InlineContext);
	bytes_required += tt.text_length * sizeof(char); // Text.
	bytes_required += tt.text_length * sizeof(unsigned); // Offsets.
	bytes_required += tt.num_tokens * sizeof(InlineToken); // Tokens.
	delete [] (char *)node->icb;
	char *block = new char[bytes_required];
	InlineContext *icb = (InlineContext *)block;
	block += sizeof(InlineContext);
	icb->text = (char *)block;
	block += tt.text_length;
	icb->advances = (unsigned *)block;
	block += tt.text_length * sizeof(unsigned);
	icb->tokens = (InlineToken *)block;
	block += tt.num_tokens * sizeof(InlineToken);
	icb->text_length = tt.text_length;
	icb->num_tokens = tt.num_tokens;
	icb->text_boxes = NULL;
	icb->selection_start = INLINE_START;
	icb->selection_end = INLINE_START;

	/* Scan the text a second time, this time writing tokens and segments
	 * into the allocated buffers. */
	itok_init(&tt, document, node, space_mode, max_chunk, 
		icb->text, icb->tokens);
	itok_tokenize(&tt);

	node->icb = icb;

	node->t.flags &= ~NFLAG_RECONSTRUCT_PARAGRAPH;
	node->t.flags |= NFLAG_REMEASURE_INLINE_TOKENS | NFLAG_UPDATE_TEXT_LAYERS;
	if (node->box != NULL)
		node->box->layout_flags &= ~BLFLAG_TEXT_VALID;
}

/* Makes a paragraph structure from the tokens of an inline context. */
void build_paragraph(Document *document, Node *node, Paragraph *p, 
	int hanging_indent)
{
	/* We must have an up-to-date ICB. */
	assertb((node->t.flags & (NFLAG_RECONSTRUCT_PARAGRAPH | 
		NFLAG_REMEASURE_INLINE_TOKENS)) == 0);
	InlineContext *icb = node->icb;

	WhiteSpaceMode mode = (WhiteSpaceMode)node->style.white_space_mode;

	const FontMetrics *metrics = NULL, *prev_metrics = NULL;
	int space_width = 0, space_stretch = 0, space_shrink = 0;
	int16_t font_id = INVALID_FONT_ID;

	for (unsigned i = 0; i < icb->num_tokens; ++i) {
		InlineToken *token = icb->tokens + i;
		
		/* Have we changed font? */
		if (token->type != TTT_CHILD) {
			int16_t new_font_id = token->child->style.text.font_id;
			if (new_font_id != font_id) {
				font_id = new_font_id;
				prev_metrics = metrics;
				metrics = get_font_metrics(document->system, font_id);
				/* Use metrics from whichever font has the smaller space width for
				 * glue connecting text in two different fonts. */
				if (prev_metrics == NULL || metrics->space_width < 
						prev_metrics->space_width) {
					space_width = metrics->space_width;
					space_shrink = metrics->space_shrink;
					space_stretch = metrics->space_stretch;
				}
			}
		} else {
			/* The size of a child token's box can change at any time, so we
			 * always update it before paragraph layout. */
			update_child_token_size(document, node, token);
		}

		/* Add any required glue and hyphenation penalties before before the 
		 * token. */
		if (mode == WSM_NORMAL) {
			if (i != 0) {
				if ((token->flags & ITF_MULTIPART_TAIL) == 0) {
					paragraph_append(p, PET_GLUE, space_width, 
						space_stretch, space_shrink);
				} else {
					paragraph_append(p, PET_PENALTY, 0, 0, 0, 2000);
				}
			} else if (hanging_indent != 0) {
				if (hanging_indent < 0)
					hanging_indent = (int)metrics->paragraph_indent_width;
				paragraph_append(p, PET_TEXT, (uint32_t)hanging_indent, 
					0, 0, 0, true, false);
			}
		}

		/* Add the token's box. */
		if (token->type != TTT_BREAK) {
			paragraph_append(p, PET_TEXT, round_signed(token->width), 
				0, 0, 0, false, true);
			token->flags |= ITF_HAS_PARAGRAPH_BOX;
		} else {
			token->flags &= ~ITF_HAS_PARAGRAPH_BOX;
		}

		/* Add a forced break at newline characters if we're preserving white
		 * space. */
		if (mode == WSM_PRESERVE && token->type == TTT_BREAK)
			paragraph_append(p, PET_PENALTY, 0, 0, 0, PENALTY_MIN);
	}

	/* Add finishing glue. */
	paragraph_append(p, PET_GLUE, 0, INT16_MAX, 0); /* Infinitely expanding zero-width space. */
	paragraph_append(p, PET_PENALTY, 0, 0, 0, PENALTY_MIN); /* Forced break. */
}

/* Calculates horizontal pixel spaces between tokens in a paragraph line. The
 * result for a line containing N words is N+1 spaces, each saying how much
 * space should go on the LHS of the corresponding token, with a final space
 * associated with the RHS of the last token. */
static unsigned compute_token_spaces(Justification justification, 
	const Paragraph *p, const ParagraphLine *line, float *out_spaces)
{
	/* If we're using a ragged edge, let spaces be their natural width provided
	 * we aren't squashing the line. */
	float adjustment_ratio = (justification == JUSTIFY_FLUSH || 
		line->adjustment_ratio < 0.0f) ? line->adjustment_ratio : 0.0f;

	/* Add a space to the result for each box that has the 'has_token' flag
	 * set. These correspond to tokens with the ITF_HAS_PARAGRAPH_BOX flag. */
	unsigned num_spaces = 0;
	float space = 0.0f;
	for (unsigned i = line->a; i != line->b; ++i) {
		ParagraphElement e = p->elements[i];
		if (e.type == PET_TEXT) {
			if (e.has_token) {
				out_spaces[num_spaces++] = space;
				space = 0.0f;
			}
			if (e.empty)
				space += e.width; 
		} else {
			unsigned m = adjustment_ratio < 0 ? e.shrink : e.stretch;
			space += e.width + (float)m * adjustment_ratio;
		}
	}
	return num_spaces;	
}

/* Helper to create a text box which positions a run of tokens.  */
static Box *create_multi_token_box(Document *document, Node *node, 
	InlineContext *icb, unsigned start_token, unsigned end_token,
	unsigned icb_start, unsigned icb_end)
{
	assertb(start_token != end_token);

	Box *box = build_text_box(document, node, icb->text + icb_start, 
		icb_end - icb_start);
	box->token_start = start_token;
	box->token_end = end_token;
	box->t.flags |= BOXFLAG_SELECTION_ANCHOR;

	/* Add the box to the ICB's text box owner chain. */
	box->owner_next = icb->text_boxes;
	icb->text_boxes = box;

	/* Assign the box to each token in the run and mark the first as 
	 * "positioned". */
	float width = 0.0f;
	
	InlineToken *token = icb->tokens + start_token, 
		*end = icb->tokens + end_token;
	token->flags |= ITF_POSITIONED;
	for (;;) {
		width += token->width;
		token->text_box = box;
		if (++token == end)
			break;
		token->flags &= ~ITF_POSITIONED;
	}
	return box;
}

/* Helper to set the margins on a box inside an inline context. */
static void set_text_box_spacing(Box *box, float space_before)
{
	box->axes[AXIS_H].mode_margin_lower = DMODE_ABSOLUTE;
	box->axes[AXIS_H].mode_margin_upper = ADEF_UNDEFINED;
	box->axes[AXIS_H].margin_lower = space_before;
	box->axes[AXIS_H].margin_upper = 0.0f;
}

/* Makes boxes to position contiguous runs of tokens in a line, applying spacing 
 * computed by paragraph layout as padding on the boxes. */
static unsigned build_line_text_boxes(Document *document, Node *node, 
	InlineContext *icb, Box *line_box, unsigned token_index, 
	const Paragraph *paragraph, const ParagraphLine *line, 
	Justification justification)
{
	/* Get the space before each token on the line. */
	float token_spaces[NUM_STATIC_PARAGRAPH_ELEMENTS + 1];
	unsigned num_line_tokens = compute_token_spaces(justification, 
		paragraph, line, token_spaces);
	assertb(token_index + num_line_tokens <= icb->num_tokens);

	unsigned run_start = token_index;
	float box_width = 0.0f, box_height = 0.0f, space = 0.0f;
	InlineToken *last_token = NULL;
	for (unsigned i = 0; ; token_index++) {
		/* Scan until we've seen all the box tokens in the line. */
		InlineToken *token = NULL;
		bool last = true;
		if (token_index != icb->num_tokens) {
			token = icb->tokens + token_index;
			last = (i == num_line_tokens);
		}

		/* Create a box to position the run of tokens terminating before the
		 * current token, if required. */
		bool terminator = last || token->type == TTT_CHILD || 
			(token->flags & ITF_MULTIPART_TAIL) == 0;
		if (run_start != token_index && terminator) {
			unsigned icb_start = icb->tokens[run_start].start;
			unsigned icb_end = last_token != NULL ? last_token->end : 0;
			Box *box;
			if (last_token != NULL && last_token->type == TTT_CHILD) {
				box = last_token->child->box;
				box->token_start = token_index - 1;
				box->token_end = token_index;
				box->t.flags |= BOXFLAG_SELECTION_ANCHOR;
			} else {
				box = create_multi_token_box(document, node, icb, 
					run_start, token_index, icb_start, icb_end);
				set_ideal_size(document, box, AXIS_H, DMODE_ABSOLUTE, box_width);
				set_ideal_size(document, box, AXIS_V, DMODE_ABSOLUTE, box_height);
			}
			set_text_box_spacing(box, space);
			append_child(document, line_box, box);
			run_start = token_index;
			box_width = 0.0f;
			box_height = 0.0f;
			space = 0.0f;
		}

		/* Stop if we've seen the token (or absence of one) after having seen
		 * num_line_tokens tokens that have ITF_HAS_PARAGRAPH_BOX set. */
		if (last)
			break;

		/* If the token has ITF_HAS_PARAGRAPH_BOX set, it is responsible for
		 * generating the i'th token space. */
		if ((token->flags & ITF_HAS_PARAGRAPH_BOX) != 0) {
			box_width += token->width;
			box_height = std::max(box_height, token->height);
			space += token_spaces[i++];
		} else {
			token->flags &= ~ITF_POSITIONED;
			token->text_box = NULL;
			if (run_start == token_index)
				run_start++;
		}
		last_token = token;
	}
	return token_index;
}

/* Given a computed paragraph layout, makes the required number of line boxes,
 * puts the boxes for all inline tokens into the proper line box, and applies 
 * computed spacing to each token box. */
static void construct_boxes_from_paragraph(Document *document, Node *node, 
	Justification justification, const Paragraph *paragraph, 
	const ParagraphLine *lines, unsigned num_lines, float leading, 
	float line_height)
{
	/* We must have an up-to-date ICB. */
	assertb((node->t.flags & (NFLAG_RECONSTRUCT_PARAGRAPH | 
		NFLAG_REMEASURE_INLINE_TOKENS)) == 0);
	InlineContext *icb = node->icb;

	/* Destroy any existing text box chain. */
	if (icb->text_boxes != NULL) {
		destroy_owner_chain(document, icb->text_boxes, false);
		icb->text_boxes = NULL;
	}

	/* Create line boxes and divide our child boxes between the line boxes 
	 * using the computed breakpoints. */
	Box *container = node->box;
	destroy_sibling_chain(document, container->t.first.box, false);
	unsigned token_index = 0;
	for (unsigned i = 0; i < num_lines; ++i) {
		/* Make a line box to contain the words. */
		Box *line_box = build_line_box(document, node, justification);
		set_box_debug_string(line_box, "line %u", i);
		append_child(document, container, line_box);

		/* Add leading. */
		if (i != 0) {
			line_box->axes[AXIS_V].mode_margin_lower = DMODE_ABSOLUTE;
			line_box->axes[AXIS_V].margin_lower = leading;
		}

		/* Make boxes to position the line's tokens. */
		token_index = build_line_text_boxes(document, node, icb, line_box,
			token_index, paragraph, lines + i, justification);

		/* Set an explicit height on empty lines to prevent them from 
		 * collapsing. */
		if (line_box->t.first == NULL) {
			set_ideal_size(document, line_box, AXIS_V, DMODE_ABSOLUTE, 
				line_height);
		}
	}

	node->t.flags |= NFLAG_UPDATE_TEXT_LAYERS;
}

/* Reconstructs the text boxes inside an inline container box. */
void update_inline_boxes(Document *document, Box *box, float width)
{
	Node *node = box->t.counterpart.node;
	
	int line_width = round_signed(width);
	
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
	if ((document->flags & DOCFLAG_DEBUG_PARAGRAPHS) != 0) {
		dump_paragraph(document, &paragraph);
		dump_paragraph_lines(document, lines, num_lines);
	}

	/* Create a vertical box for each line and put the word boxes inside 
	 * them. */
	construct_boxes_from_paragraph(document, node, justification, &paragraph, lines,
		num_lines, (float)leading, (float)metrics->height);

	/* Deallocate the paragraph and any heap buffer used for to store lines. */
	paragraph_clear(&paragraph);
	if (lines != line_buffer)
		delete [] lines;

	/* No need to do paragraph layout again unless the container's width 
	 * changes. */
	box->layout_flags |= BLFLAG_TEXT_VALID;
	if ((node->t.flags & NFLAG_IN_SELECTION_CHAIN) != 0)
		node->t.flags |= NFLAG_UPDATE_SELECTION_LAYERS;
}

static const unsigned MAX_CONTEXT_TEXT_LAYERS = 16;
static const unsigned BUCKET_HASH_CAPACITY = 2 * MAX_CONTEXT_TEXT_LAYERS;

/* A temporary structure that stores the character count of a font style key
 * in an inline context. */
struct TextStyleBucket {
	TextStyle style;
	unsigned num_characters;
	TextStyleBucket *font_chain;
	unsigned layer_index;
	unsigned palette_index;
};

/* Helper to find an insertion index for a font style key in a text style 
 * bucket hash. Assumes the hash can never be full. */
static unsigned find_text_style_bucket(const TextStyleBucket * const *table, 
	uint32_t key)
{
	unsigned index = key % BUCKET_HASH_CAPACITY;
	while (table[index] != NULL && table[index]->style.key != key)
		index = (index + 1) % BUCKET_HASH_CAPACITY;
	return index;
}

/* Creates a stack of text layers for all the characters in an inline context. 
 * There will be one text layer for each distinct (font, colour) combination
 * used in the context. */
VisualLayer *build_text_layer_stack(Document *document, Node *node)
{
	/* We must have an up-to-date ICB. */
	assertb((node->t.flags & (NFLAG_RECONSTRUCT_PARAGRAPH | 
		NFLAG_REMEASURE_INLINE_TOKENS)) == 0);
	const InlineContext *icb = node->icb;
	
	/* Count the number of characters for each distinct text style key in the
	 * context, producing a list of layers that we need to create. */
	TextStyleBucket *bucket_hash[BUCKET_HASH_CAPACITY] = { 0 };
	TextStyleBucket	buckets[MAX_CONTEXT_TEXT_LAYERS] = { 0 };
	TextStyleBucket *bucket = NULL;
	unsigned num_buckets = 0;
	for (unsigned i = 0; i < icb->num_tokens; ++i) {
		const InlineToken *token = icb->tokens + i;
		TextStyleSegment ss = token_first_style_segment(document, node, token);
		while (ss.segment.child != NULL) {
			/* If we've changed font or colour, get or create the bucket for the 
			 * new (font, colour) combination. */
			uint32_t key = ss.style.key;
			if (bucket == NULL || key != bucket->style.key) {
				unsigned index = find_text_style_bucket(bucket_hash, key);
				bucket = bucket_hash[index];
				if (bucket == NULL) {
					/* Create a new bucket for this style. */
					ensure(num_buckets != MAX_CONTEXT_TEXT_LAYERS);
					bucket = buckets + num_buckets;
					bucket_hash[index] = bucket;
					bucket->style = ss.style;
					bucket->num_characters = 0;
					num_buckets++;
				}
			}
			/* Add the segment's characters to the bucket's count. */
			bucket->num_characters += ss.segment.end - ss.segment.start;
			ss = token_next_style_segment(document, node, token, &ss);
		}
	}
	if (num_buckets == 0)
		return NULL;

	/* Depending on the system configuration, we either make a layer for each
	 * style bucket with a single-entry palette, or we group the buckets by
	 * font ID and make a layer for each font ID with a multicolour palette. */
	VisualLayer *layers[MAX_CONTEXT_TEXT_LAYERS];
	unsigned num_layers;
	if ((document->system->flags & SYSFLAG_TEXT_LAYER_PALETTES) != 0) {
		/* Chain the buckets sharing a font ID together, and make a layer for
		 * each font. The layers contain a colour palette with an entry for each 
		 * bucket that uses the font. */
		TextStyleBucket *font_id_chains[MAX_CACHED_FONTS] = { 0 };
		for (unsigned i = 0; i < num_buckets; ++i) {
			TextStyleBucket *bucket = buckets + i;
			int font_id = bucket->style.font_id;
			bucket->font_chain = font_id_chains[font_id];
			font_id_chains[font_id] = bucket;
		}

		/* Count the characters and colours for each distinct font ID. */
		unsigned color_counts[MAX_CACHED_FONTS];
		unsigned character_counts[MAX_CACHED_FONTS];
		num_layers = 0;
		for (unsigned i = 0; i < MAX_CACHED_FONTS; ++i) {
			TextStyleBucket *bucket = font_id_chains[i];
			if (bucket == NULL)
				continue;
			font_id_chains[num_layers] = bucket;
			unsigned num_colors = 0, num_characters = 0;
			do {
				num_characters += bucket->num_characters;
				bucket->layer_index = num_layers;
				bucket->palette_index = num_colors & TLF_COLOR_INDEX_MASK;
				num_colors++;
				bucket = bucket->font_chain;
			} while (bucket != NULL);
			character_counts[num_layers] = num_characters;
			color_counts[num_layers] = num_colors;
			num_layers++;
		}

		/* Make a layer for each font ID. */
		for (unsigned i = 0; i < num_layers; ++i) {
			unsigned length = character_counts[i];
			unsigned num_colors = color_counts[i];
			unsigned extra_bytes = length * TEXT_LAYER_BYTES_PER_CHAR + 
				num_colors * sizeof(uint32_t);
			VisualLayer *layer = create_layer(document, node, VLT_TEXT, extra_bytes);
			TextStyleBucket *bucket = font_id_chains[i];
			layer->text.key = bucket->style.font_id;
			layer->text.flags = bucket->style.flags;
			layer->text.font_id = bucket->style.font_id;
			layer->text.length = length;
			layer->text.num_colors = num_colors;
			layer->depth_offset = 1; /* Above line box selection layers. */
			layers[i] = layer;
			/* Add a palette entry for each bucket in the chain. */
			uint32_t *palette = (uint32_t *)get_text_layer_palette(layer);
			do {
				palette[bucket->palette_index] = blend32(bucket->style.color, 
					bucket->style.tint);
				bucket = bucket->font_chain;
			} while (bucket != NULL);
		}
	} else {
		/* Make a layer for each bucket. Each layer has a single-entry 
		 * palette. */
		for (unsigned i = 0; i < num_buckets; ++i) {
			TextStyleBucket *bucket = buckets + i;
			unsigned extra_bytes = bucket->num_characters * 
				TEXT_LAYER_BYTES_PER_CHAR + 1 * sizeof(uint32_t);
			VisualLayer *layer = create_layer(document, node, VLT_TEXT, extra_bytes);
			layer->text.key = bucket->style.key;
			layer->text.flags = bucket->style.flags;
			layer->text.font_id = bucket->style.font_id;
			layer->text.length = bucket->num_characters;
			layer->text.num_colors = 1;
			layer->depth_offset = 1; /* Above line box selection layers. */
			uint32_t *palette = (uint32_t *)get_text_layer_palette(layer);
			palette[0] = blend32(bucket->style.color, bucket->style.tint);
			layers[i] = layer;
			bucket->layer_index = i;
			bucket->palette_index = 0;
		}
		num_layers = num_buckets;
	}

	/* Iterate over the segments again, copying the text of each into the
	 * appropriate layer and calculating positions. */
	const Box *text_box = NULL, *last_text_box = NULL;
	unsigned write_positions[MAX_CONTEXT_TEXT_LAYERS] = { 0 };
	int x = 0, y = 0;
	bucket = buckets;
	VisualLayer *layer = layers[bucket->layer_index];
	unsigned character_flags = TLF_TOKEN_HEAD | TLF_LINE_HEAD | TLF_STYLE_HEAD;
	for (unsigned i = 0; i < icb->num_tokens; ++i) {
		const InlineToken *token = icb->tokens + i;

		/* A text box defines a position for a horizontal run of tokens. */
		if ((token->flags & ITF_POSITIONED) != 0) {
			text_box = token->text_box;
			x = round_signed(content_edge_lower(text_box, AXIS_H));
			y = round_signed(content_edge_lower(text_box, AXIS_V));
			character_flags = TLF_TOKEN_HEAD;
			if (last_text_box == NULL || text_box->t.parent != last_text_box->t.parent)
				character_flags |= TLF_LINE_HEAD;
			last_text_box = text_box;
		}
		
		TextStyleSegment ss = token_first_style_segment(document, node, token);
		while (ss.segment.child != NULL) {
			/* Switch to the layer for the segment's style key. */
			uint32_t key = ss.style.key;
			if (key != bucket->style.key) {
				unsigned index = find_text_style_bucket(bucket_hash, key);
				bucket = bucket_hash[index];
				layer = layers[bucket->layer_index];
				character_flags |= TLF_STYLE_HEAD;
			}

			/* Copy the segment's text into the layer. */
			char *layer_text = (char *)get_text_layer_text(layer);
			unsigned written = write_positions[bucket->layer_index];
			const char *seg_text = icb->text + ss.segment.start;
			unsigned seg_length = ss.segment.end - ss.segment.start;
			memcpy(layer_text + written, seg_text, seg_length);

			/* Set character flags. */
			uint16_t *flags = (uint16_t *)get_text_layer_flags(layer);
			character_flags |= TLF_SEGMENT_HEAD;
			for (unsigned j = 0; j < seg_length; ++j) {
				flags[written + j] = uint16_t(character_flags | bucket->palette_index);
				character_flags = 0;
			}

			/* Calculate (x, y) positions for each character. */
			int *positions = (int *)get_text_layer_positions(layer) + 2 * written;
			for (unsigned j = 0; j < seg_length; ++j) {
				positions[2 * j + 0] = x;
				positions[2 * j + 1] = y;
				x += icb->advances[ss.segment.start + j];
			}

			/* Advance the write position within the layer. */
			write_positions[bucket->layer_index] += seg_length;
			ss = token_next_style_segment(document, node, token, &ss);
		}
	}

	/* Chain the layers together.*/
	for (unsigned i = 1; i < num_layers; ++i)
		layers[i - 1]->next[VLCHAIN_NODE] = layers[i];
	layers[num_layers - 1]->next[VLCHAIN_NODE] = NULL;
	return layers[0];
}

unsigned read_inline_text(const Document *document, const Node *node,
	InternalAddress start, InternalAddress end, char *buffer)
{
	assertb(node->layout == LAYOUT_INLINE_CONTAINER);
	assertb(node->icb != NULL);

	document;

	start = expand_internal_address(node, start);
	end = expand_internal_address(node, end);

	WhiteSpaceMode space_mode = (WhiteSpaceMode)node->style.white_space_mode;
	const InlineContext *icb = node->icb;
	unsigned read = 0;
	for (unsigned i = start.token; i <= end.token; ++i) {
		const InlineToken *token = icb->tokens + i;

		/* Calculate range within the ICB's text buffer to extract for this
		 * token. */
		unsigned icb_start = token->start;
		unsigned icb_end = token->end;
		if (i == start.token) {
			if (start.offset == IA_END)
				icb_start = icb_end;
			else
				icb_start += start.offset;
		}
		if (i == end.token && end.offset != IA_END) {
			icb_end = icb_start + end.offset;
		}
		if (icb_start == icb_end)
			continue;

		/* Add a space before the word, unless we have preserved white space, in
		 * which case we expect there to be space and break tokens. */
		if (space_mode == WSM_NORMAL && token->type == TTT_WORD &&  
			(token->flags & ITF_MULTIPART_TAIL) == 0 && i != start.token) {
			if (buffer != NULL)
				buffer[read] = ' ';
			read++;
		}

		/* Copy out the token's text. */
		unsigned length = icb_end - icb_start;
		if (buffer != NULL)
			memcpy(buffer + read, icb->text + icb_start, length);
		read += length;
	}
	return read;
}

} // namespace stkr
