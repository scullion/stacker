#include "stacker_document.h"

#include "stacker_system.h"
#include "stacker_message.h"
#include "stacker_node.h"
#include "stacker_box.h"
#include "stacker_layout.h"
#include "stacker_rule.h"
#include "stacker_quadtree.h"
#include "stacker_inline.h"
#include "stacker_diagnostics.h"
#include "stacker_inline.h"
#include "stacker_util.h"
#include "stacker_message.h"
#include "stacker_view.h"
#include "stacker_platform.h"

#include "url_cache.h"

namespace stkr {

using namespace urlcache;

Node *get_root(Document *document)
{
	return document->root;
}

const Node *get_root(const Document *document)
{
	return document->root;
}

unsigned get_hit_clock(const Document *document)
{
	return document->hit_clock;
}

unsigned get_layout_clock(const Document *document)
{
	return document->layout_clock;
}

unsigned get_flags(const Document *document)
{
	return document->flags;
}

const Box *get_selection_start_anchor(const Document *document)
{
	return document->debug_start_anchor;
}

const Box *get_selection_end_anchor(const Document *document)
{
	return document->debug_end_anchor;
}

CaretAddress get_selection_start(const Document *document)
{
	return document->selection_start;
}

CaretAddress get_selection_end(const Document *document)
{
	return document->selection_end;
}

void document_dump(const Document *document, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	document->dump(document->dump_data, fmt, args);
	va_end(args);
}

/* Adds a message to the document's external message queue. */
void enqueue_message(Document *document, const Message *message)
{
	if ((document->flags & DOCFLAG_EXTERNAL_MESSAGES) != 0)
		enqueue_message(&document->message_queue, message);
}

/* Returns the next external message for a document, or NULL if the message
 * queue is empty. */
const Message *dequeue_message(Document *document)
{
	return dequeue_message(&document->message_queue);
}

/* Resolves an anchor box and a mouse position into a tree position at which
 * to start or end a mouse selection. */
static CaretAddress resolve_selection_anchor(Document *document, 
	Box *anchor, float x, float y, bool upwards)
{
	static const float LINE_HIT_MARGIN = 128.0f;

	upwards;

	/* Fail if the anchor is not associated with a node. */
	CaretAddress address = { NULL, 0, 0 };
	Node *node = (Node *)find_context_node(document, anchor->owner);
	if (node == NULL)
		return address;

	/* Get the anchor bounds. */
	float ax0, ax1, ay0, ay1;
	outer_rectangle(anchor, &ax0, &ax1, &ay0, &ay1);
	
	if (node->layout == LAYOUT_INLINE_CONTAINER) {
		/* If 'y' is inside a vertical band surrounding the anchor box, find the
		 * caret position in the anchor token closest to 'x'. If 'y' is outside 
		 * the band, ignore 'x' and select from the beginning or end of the line
		 * containing the anchor. */
		if (y >= ay0 - LINE_HIT_MARGIN && y <= ay1 + LINE_HIT_MARGIN) {
			address = caret_position(document, anchor, x);
		} else {
			const InlineContext *icb = node->inline_context;
			Box *line_box = anchor->parent;
			bool selecting_from_above = (y <= ay1);
			address.node = node;
			if (selecting_from_above) {
				/* Select from the beginning of the line. */
				Box *first_box = line_box->first_child;
				if (first_box != NULL) {
					address.ia.token = first_box->token_start;
					address.ia.offset = 0;
				}
			} else {
				/* Select from the end of the line. */
				Box *last_box = line_box->last_child;
				if (last_box != NULL && last_box->token_end != 0) {
					const InlineToken *token = icb->tokens + 
						last_box->token_end - 1;
					TextSegment s = token_first_segment(node, token);
					while (s.child != NULL)
						s = token_next_segment(node, token, &s);
					address.ia.token = last_box->token_end - 1;
					address.ia.offset = token->end - token->start;
				}
			}
		}
	} else {
		address = caret_position(document, anchor, x);
	}
	return address;
}

static void clear_selection_chain(Document *document)
{
	Node *next;
	for (Node *node = document->selection_chain_head; 
		node != NULL; node = next) {
		next = node->selection_next;
		node->selection_prev = NULL;
		node->selection_next = NULL;
		node->flags &= ~NFLAG_IN_SELECTION_CHAIN;
		node->flags |= NFLAG_UPDATE_SELECTION_LAYERS | NFLAG_UPDATE_TEXT_LAYERS;
	}
	document->selection_chain_head = NULL;
	document->selection_chain_tail = NULL;
}

static void build_selection_chain(Document *document, CaretAddress start,
	CaretAddress end)
{
	CaretWalker walker;
	Node *node = cwalk_first(document, &walker, start, end, 
		NFLAG_IN_SELECTION_CHAIN);
	while (node != NULL) {
	 	set_node_flags(document, node, NFLAG_IN_SELECTION_CHAIN | 
			NFLAG_UPDATE_TEXT_LAYERS | NFLAG_UPDATE_SELECTION_LAYERS, true);
		list_insert_before(
			(void **)&document->selection_chain_head,
			(void **)&document->selection_chain_tail,
			node, NULL, offsetof(Node, selection_prev));
		if (node->layout == LAYOUT_INLINE_CONTAINER) {
			InlineContext *icb = node->inline_context;
			icb->selection_start = closest_internal_address(document, node, walker.start, ARW_TIES_TO_END);
			icb->selection_end = closest_internal_address(document, node, walker.end, ARW_TIES_TO_START);
		}
		node = cwalk_next(document, &walker);
	}
}

/* Rebuilds the document's list of selected nodes. */
static void update_selection_chain(Document *document)
{
	CaretAddress start = document->selection_start;
	CaretAddress end = document->selection_end;
	clear_selection_chain(document);
	if (start.node != NULL && end.node != NULL && !caret_equal(start, end)) {
		build_selection_chain(document, start, end);
		document->flags |= DOCFLAG_HAS_SELECTION;
	} else {
		document->flags &= ~DOCFLAG_HAS_SELECTION;
	}
	document->flags &= ~DOCFLAG_UPDATE_SELECTION_CHAIN;
}

static void clear_mouse_selection(Document *document)
{
	document->selection_start.node = NULL;
	document->selection_end.node = NULL;
	document->debug_start_anchor = NULL;
	document->debug_end_anchor = NULL;
	document->flags |= DOCFLAG_UPDATE_SELECTION_CHAIN;
	document->flags &= ~DOCFLAG_HAS_SELECTION;
}

void clear_selection(Document *document)
{
	clear_mouse_selection(document);
	document->flags &= ~DOCFLAG_SELECTING;
}

/* Reads selected text from all nodes in the selection chain into a buffer. */
static unsigned read_selection_chain_text(const Document *document, char *buffer)
{
	unsigned length = 0;
	for (const Node *node = document->selection_chain_head; node != NULL; 
		node = node->selection_next) {
		if (node->layout != LAYOUT_INLINE_CONTAINER)
			continue;
		const InlineContext *icb = node->inline_context;
		if (length != 0) {
			if (buffer != NULL) {
				buffer[length] = '\n';
				buffer[length + 1] = '\n';
			}
			length += 2;
		}
		length += read_inline_text(document, node, icb->selection_start, 
			icb->selection_end, buffer != NULL ? buffer + length : NULL);
	}
	return length;
}

/* If the document has a selection, returns a heap allocated buffer containing
 * the selected text. If not, returns NULL. If a buffer is returned, it is 
 * guaranteed to be null terminated. The reported size does not include the
 * terminator. */
static char *get_selected_text(Document *document, unsigned *out_size = NULL)
{
	if ((document->flags & DOCFLAG_UPDATE_SELECTION_CHAIN) != 0)
		update_selection_chain(document);
	if (out_size != NULL)
		*out_size = 0;
	if ((document->flags & DOCFLAG_HAS_SELECTION) == 0)
		return NULL;
	unsigned bytes_required = read_selection_chain_text(document, NULL);
	if (bytes_required == 0)
		return NULL;
	char *buffer = new char[bytes_required + 1];
	read_selection_chain_text(document, buffer);
	buffer[bytes_required] = '\0';
	if (out_size != NULL)
		*out_size = bytes_required;
	return buffer;
}

/* Recalculates the mouse selection with a new end position. */
static void update_mouse_selection(Document *document, float x1, float y1,
	const View *view)
{
	document->flags |= DOCFLAG_UPDATE_SELECTION_CHAIN;
	document->change_clock++;

	document->mouse_last_x = x1;
	document->mouse_last_y = y1;
	document->selection_view = view;

	float x0 = document->mouse_down_x;
	float y0 = document->mouse_down_y;
	float bound_y0 = rtop(view->bounds);
	float bound_y1 = rbottom(view->bounds);
	float bound_x0 = rleft(view->bounds);
	float bound_x1 = rright(view->bounds);
	if (y1 < y0) std::swap(bound_y0, bound_y1);

	Box *start_box = grid_query_anchor(document, x0, bound_x0, bound_x1, y0, bound_y1);
	Box *end_box = grid_query_anchor(document, x1, bound_x0, bound_x1, y1, bound_y0);
	// dmsg("update_mouse_selection(): x0: %.2f, y0: %.2f, y1: %.2f\n", x0, y0, y1);
	if (start_box == NULL || end_box == NULL) {
		clear_mouse_selection(document);
		return;
	}
		
	CaretAddress start = resolve_selection_anchor(document, start_box, x0, y0, y1 < y0);
	CaretAddress end = resolve_selection_anchor(document, end_box, x1, y1, y0 < y1);
	if (start.node != NULL && end.node != NULL) {
		document->selection_start = start;
		document->selection_end = end;
	} else {
		document->selection_start.node = NULL;
		document->selection_end.node = NULL;
	}
	document->debug_start_anchor = start_box;
	document->debug_end_anchor = end_box;
}

static void begin_mouse_selection(Document *document, float x, float y, 
	const View *view, unsigned modifiers)
{
	document->flags |= DOCFLAG_SELECTING;
	document->mouse_down_x = x;
	document->mouse_down_y = y;
	document->mouse_modifiers = modifiers;
	update_mouse_selection(document, x, y, view);
}

static void end_mouse_selection(Document *document, float x, float y)
{
	if ((document->flags & DOCFLAG_SELECTING) == 0)
		return;
	update_mouse_selection(document, x, y, document->selection_view);
	document->flags &= ~DOCFLAG_SELECTING;
}

static void refresh_mouse_selection(Document  *document)
{
	if ((document->flags & DOCFLAG_SELECTING) != 0) {
		update_mouse_selection(document, document->mouse_last_x,
			document->mouse_last_y, document->selection_view);
	}
}

static void debug_selection_hit(Document *document, const Message *message)
{
	const View *view = message->mouse.view;

	if ((message->flags & KMF_SHIFT) != 0) {
		update_mouse_selection(document, document->mouse_last_x, document->mouse_last_y, message->mouse.view);
		return;
	}

	if ((message->flags & KMF_CTRL) != 0) {
		update_selection_chain(document);
		return;
	}

	bool upwards = (message->flags & KMF_ALT) != 0;
	float bound_x0 = rleft(view->bounds);
	float bound_x1 = rright(view->bounds);
	float bound_y = upwards ? rtop(view->bounds) : rbottom(view->bounds);
	Box *anchor = grid_query_anchor(document, 
		message->mouse.x, 
		bound_x0, 
		bound_x1, 
		message->mouse.y,
		bound_y);
	dmsg("Hit anchor box: %s.\n", get_box_debug_string(anchor));
}

/* Updates the selection when a box is destroyed. */
void document_notify_box_destroy(Document *document, Box *box)
{
	if (box == document->debug_start_anchor)
		document->debug_start_anchor = NULL;
	if (box == document->debug_end_anchor)
		document->debug_end_anchor = NULL;
}

/* Updates the selection when a node is destroyed. */
static void selection_handle_node_destroy(Document *document, Node *node)
{
	if ((node->flags & NFLAG_IN_SELECTION_CHAIN) == 0)
		return;
	list_remove(
		(void **)&document->selection_chain_head, 
		(void **)&document->selection_chain_tail,
		node, offsetof(Node, selection_prev));	
	CaretAddress *start = &document->selection_start;
	CaretAddress *end = &document->selection_end;
	if (node == start->node)
		*start = caret_start(document, document->selection_chain_head);
	if (node == end->node)
		*end = caret_end(document, document->selection_chain_tail);

}

/* Updates hit testing state when a node is destroyed. */
static void hit_handle_node_destroy(Document *document, Node *node)
{
	if ((node->flags & NFLAG_IN_HIT_CHAIN) != 0) {
		list_remove(
			(void **)&document->hit_chain_head, 
			(void **)&document->hit_chain_tail,
			node, offsetof(Node, hit_prev));
		if (node == document->hit_node)
			document->hit_node = NULL;
		if (node == document->mouse_down_node)
			document->mouse_down_node = NULL;
	}
}

/* A node has been destroyed. */
void document_notify_node_destroy(Document *document, Node *node)
{
	selection_handle_node_destroy(document, node);
	hit_handle_node_destroy(document, node);
}

/* Updates the selection when a node moves in the tree or has children added
 * or removed. */
void document_notify_node_changed(Document *document, Node *node)
{
	if ((node->flags & NFLAG_IN_SELECTION_CHAIN) != 0)
		document->flags |= DOCFLAG_UPDATE_SELECTION_CHAIN;
}

/* Returns the document's active cursor. */
CursorType get_cursor(const Document *document)
{
	return document->cursor;
}

/* Sets the document's active cursor. */
void set_cursor(Document *document, CursorType new_cursor)
{
	if (new_cursor == document->cursor)
		return;
	document->cursor = new_cursor;

	Message message;
	message.type = MSG_CURSOR_CHANGED;
	message.flags = 0;
	message.cursor.cursor = new_cursor;
	enqueue_message(document, &message);
}

/* Keeps track of the node under the mouse. */
static void update_hit_node(Document *document, const Message *message)
{
	Node *node = message->hit.hit_node;
	bool use_default_cursor = false;
	if (message->type == MSG_NODE_HIT) {
		if (node == NULL || (node->flags & NFLAG_MOUSE_OVER) != 0) {
			document->hit_node = node;
			use_default_cursor = node == NULL || 
				(message->flags & MFLAG_HANDLED) == 0;
		}
	} else if (message->type == MSG_NODE_UNHIT) {
		if (document->hit_node == node) {
			document->hit_node = NULL;
			use_default_cursor = true;
		}
	}
	if (use_default_cursor) {
		CursorType ct = (document->hit_node != NULL && 
			(document->flags & DOCFLAG_ENABLE_SELECTION) != 0 &&
			document->hit_node->layout == LAYOUT_INLINE) ? 
			CT_CARET : CT_DEFAULT;
		set_cursor(document, ct);
	}
}

static bool document_handle_mouse_message(Document *document, Message *message)
{
	if (message->type == MSG_MOUSE_LEFT_DOWN) {
		if ((document->flags & DOCFLAG_ENABLE_SELECTION) != 0)
			begin_mouse_selection(document, message->mouse.x, message->mouse.y,
				message->mouse.view, message->flags);
		return true;
	} else if (message->type == MSG_MOUSE_LEFT_UP) {
		if ((document->flags & DOCFLAG_SELECTING) != 0)
			end_mouse_selection(document, message->mouse.x, message->mouse.y);
		return true;
	} else if (message->type == MSG_MOUSE_RIGHT_DOWN) {
		if ((document->flags & DOCFLAG_DEBUG_SELECTION) != 0)
			debug_selection_hit(document, message);
		return true;
	} else if (message->type == MSG_MOUSE_MOVE) {
		if ((document->flags & DOCFLAG_SELECTING) != 0)
			update_mouse_selection(document, message->mouse.x, message->mouse.y,
				message->mouse.view);
		return true;
	}
	return false;
}

/* Root message handler. */
bool document_handle_message(Document *document, Message *message)
{
	if (is_mouse_message(message->type)) {
		message->flags &= ~MFLAG_PROPAGATE;
		if ((message->flags & MFLAG_HANDLED) == 0)
			return document_handle_mouse_message(document, message);
	} else if (message->type == MSG_KEY_DOWN) {
		int ch = message->keyboard.code;
		if (ch < 128)
			ch = tolower(ch);
		if ((message->flags & KMF_CTRL) != 0) {
			if (ch == int('c') || ch == int('x')) {
				unsigned text_size = 0;
				char *text = get_selected_text(document, &text_size);
				if (text != NULL) {
					platform_copy_to_clipboard(document->system->back_end, 
						text, text_size);
					dmsg("%u bytes copied to the clipboard: --[%.*s]--.\n", text_size, text_size, text);
					delete [] text;
				} else {
					dmsg("No text to copy.\n");
				}
			}
		}
		message->flags &= ~MFLAG_PROPAGATE;
		return true;
	} else if (message->type == MSG_NODE_HIT || 
		message->type == MSG_NODE_UNHIT) {
		update_hit_node(document, message);
	}
	return false;
}

/* Sets the dimensions of the document's current root node to those specified in 
 * the root constraints. */
void impose_root_constraints(Document *d)
{
	Node *root = d->root;
	if ((d->flags & DOCFLAG_CONSTRAIN_WIDTH) != 0)
		set_outer_dimension(d, root, AXIS_H, d->root_dims[AXIS_H]);
	if ((d->flags & DOCFLAG_CONSTRAIN_HEIGHT) != 0)
		set_outer_dimension(d, root, AXIS_V, d->root_dims[AXIS_V]);
}

void set_document_flags(Document *document, unsigned mask, bool value)
{
	unsigned old_flags = document->flags;
	document->flags = set_or_clear(old_flags, mask, value);
	unsigned changed = old_flags ^ document->flags;
	document->change_clock += (changed != 0);
	if ((changed & DOCFLAG_ENABLE_SELECTION) != 0)
		clear_selection(document);
}

float get_root_dimension(const Document *document, Axis axis)
{
	if (document->root->box == NULL)
		return 0.0f;
	return outer_dim(document->root->box, axis);
}

void set_root_dimension(Document *document, Axis axis, unsigned dimension)
{
	document->root_dims[axis] = dimension;
	document->change_clock++;
}

void set_layout_dump_callback(Document *document, DumpCallback layout_dump, 
	void *layout_dump_data)
{
	document->dump = layout_dump != NULL ? layout_dump : &dump_discard;
	document->dump_data = layout_dump_data;
}

/* Determines whether document or global rule tables have changed since the
 * last layout. */
static bool check_rule_tables(const Document *document)
{
	const System *system = document->system;
	return (document->flags & DOCFLAG_RULE_TABLE_CHANGED) != 0 ||
		document->global_rule_table_revision != system->rule_table_revision;
}

/* True if a document's nodes need to be visited to check for things that
 * need to be updated. */
bool needs_update(const Document *document)
{
	if (check_rule_tables(document))
		return true;
	if (document->change_clock != document->change_clock_at_layout)
		return true;
	if (document->rule_revision_at_layout != 
		document->system->rule_revision_counter)
		return true;
	return false;
}

/* Traverses the node tree, updating node state and layout that is invalid. */
void update_document(Document *document)
{
	System *system = document->system;
	Node *root = document->root;

	/* Rebuild the list of selected nodes if required. */
	if ((document->flags & DOCFLAG_UPDATE_SELECTION_CHAIN) != 0) {
		update_selection_chain(document);
		document->flags &= ~DOCFLAG_UPDATE_SELECTION_CHAIN;
	}

	/* Do we need to visit nodes? */
	if (!needs_update(document))
		return;
	document->layout_clock++;
	
	bool rule_tables_changed = check_rule_tables(document);
	update_nodes_pre_layout(document, root, 0, rule_tables_changed);
	layout(document, root->box);
	update_nodes_post_layout(document, root);
	update_box_clip(document, root->box, INFINITE_RECTANGLE, 0);
	refresh_mouse_selection(document);

	document->change_clock_at_layout = document->change_clock;
	document->rule_revision_at_layout = system->rule_revision_counter;
	document->global_rule_table_revision = system->rule_table_revision;
	document->flags &= ~DOCFLAG_RULE_TABLE_CHANGED;
}

static void clear_document(Document *document)
{
	clear_message_queue(&document->message_queue);
	clear_selection(document);
	document->hit_chain_head = NULL;
	document->hit_chain_tail = NULL;
	document->selection_chain_head = NULL;
	document->selection_chain_tail = NULL;
	document->selection_start.node = NULL;
	document->selection_end.node = NULL;
	document->debug_start_anchor = NULL;
	document->debug_end_anchor = NULL;
	clear_rule_table(&document->rules);
	if (document->root != NULL)
		destroy_node(document, document->root, true);
	document->source_length = 0;
}

Document *create_document(System *system, unsigned flags)
{
	Document *document = new Document();
	document->system = system;
	document->root = NULL;
	document->dump = &dump_discard;
	document->dump_data = NULL;
	document->box_query_stamp = 1;
	document->layout_clock = 0;
	document->change_clock = 0;
	document->change_clock_at_layout = unsigned(-1);
	document->free_boxes = NULL;
	document->hit_clock = 0;
	document->flags = flags;
	document->root_dims[AXIS_H] = 0;
	document->root_dims[AXIS_V] = 0;
	document->hit_node = NULL;
	document->mouse_down_node = NULL;
	document->cursor = CT_DEFAULT;
	document->navigation_state = DOCNAV_IDLE;
	document->url_handle = urlcache::INVALID_URL_HANDLE;
	document->source = NULL;
	document->source_length = 0;
	document->source_capacity = 0;
	grid_init(&document->grid);
	
	unsigned mq_capacity = (flags & DOCFLAG_EXTERNAL_MESSAGES) != 0 ? 
		DEFAULT_MESSAGE_QUEUE_CAPACITY : 0;
	init_message_queue(&document->message_queue, mq_capacity);

	reset_document(document);
	return document;
}

static void clear_box_free_list(Document *document)
{
	while (document->free_boxes != NULL) {
		Box *box = document->free_boxes;
		document->free_boxes = box->next_sibling;
		delete box;
	}
}

void destroy_document(Document *document)
{
	clear_box_free_list(document);
	clear_document(document);
	deinit_message_queue(&document->message_queue);
	grid_deinit(&document->grid);
	if (document->url_handle != urlcache::INVALID_URL_HANDLE)
		document->system->url_cache->destroy_handle(document->url_handle);
	delete [] document->source;
	delete document;
}

void reset_document(Document *document)
{
	clear_document(document);
	int rc = create_node(&document->root, document, LNODE_VBOX, TOKEN_DOCUMENT);
	ensure(rc >= 0);
	document->layout_clock++;
	document->change_clock++;
	document->rule_revision_at_layout = document->system->
		rule_revision_counter - 1;
}

/* Walks the hit chain looking for nodes that were not hit this tick and sends
 * each a message to that effect. */
static void prune_hit_chain(Document *document)
{
	Node *next;
	for (Node *node = document->hit_chain_head; node != NULL; node = next) {
		next = node->hit_next;
		if (node->mouse_hit_stamp == document->hit_clock)
			continue;
		Message message;
		message.type = MSG_NODE_UNHIT;
		message.flags = 0;
		message.hit.hit_node = node;
		message.hit.hit_box = NULL;
		send_message(document, node, &message);
	}
}

/* Processes a depth-sorted list of boxes that were found to be under the
 * mouse, sending a node-hit message to the node of each, starting with the
 * topmost. */
Node *process_hit_stack(Document *document, Box **hit_stack,
	unsigned hit_count, float x, float y)
{
	y;

	Message message;
	message.type = MSG_NODE_HIT;

	Node *hit_node = NULL;
	document->hit_clock++;
	for (unsigned i = hit_count - 1; i + 1 != 0; --i) {
		Box *box = hit_stack[i];
		box->mouse_hit_stamp = document->hit_clock;
		
		CaretAddress address = caret_position(document, box, x);
		if (address.node == NULL)
			continue;
		Node *node = (Node *)node_at_caret(address);
		if (hit_node == NULL)
			hit_node = node;

		message.hit.hit_node = node;
		message.hit.hit_box = box;
		message.flags = (i + 1 == hit_count) ? HITFLAG_TOPMOST : 0;
		send_message(document, node, &message);
	}
	prune_hit_chain(document);

	return hit_node;
}

void document_handle_mouse_event(Document *document, View *view, 
	MessageType type, float doc_x, float doc_y, unsigned flags)
{
	static const unsigned MAX_HIT_BOXES = 16;

	Node *hit_node = NULL;
	if ((document->flags & DOCFLAG_SELECTING) == 0) {
		/* Query the grid for the stack of boxes under the pointer. */
		Box *hit_stack[MAX_HIT_BOXES];
		unsigned hit_count = grid_query_point(document, hit_stack, 
			MAX_HIT_BOXES, doc_x, doc_y);
		depth_sort_boxes((const Box **)hit_stack, hit_count);
		hit_node = process_hit_stack(document, hit_stack, hit_count, 
			doc_x, doc_y);
	} else {
		prune_hit_chain(document);
	}

	/* Guarantee that button down/up messages are issued in pairs. */
	Node *target = hit_node;
	if (type == MSG_MOUSE_LEFT_DOWN || type == MSG_MOUSE_RIGHT_DOWN) {
		document->mouse_down_node = hit_node;
	} else if (type == MSG_MOUSE_LEFT_UP || type == MSG_MOUSE_RIGHT_UP) {
		target = document->mouse_down_node;
		document->mouse_down_node = NULL;
	}

	/* If a node was hit, send the raw mouse message to it. */
	Message message;
	message.type = type;
	message.flags = flags;
	message.mouse.x = doc_x;
	message.mouse.y = doc_y;
	message.mouse.view = view;
	send_message(document, target, &message);
}

void document_handle_keyboard_event(Document *document, View *view, 
	MessageType type, unsigned key_code, unsigned flags)
{
	Message message;
	message.type = type;
	message.flags = flags;
	message.keyboard.view = view;
	message.keyboard.code = key_code;
	send_message(document, NULL, &message);
}

/* Updates the document's navigation state and sends a notification message. */
static void set_navigation_state(Document *document, NavigationState state)
{
	/* Has the state changed? */
	NavigationState old_state = document->navigation_state;
	if (state == old_state)
		return;
	document->navigation_state = state;

	/* Send a notification. */
	Message message;
	message.type = MSG_NAVIGATE;
	message.flags = 0;
	message.navigation.old_state = old_state;
	message.navigation.new_state = state;
	enqueue_message(document, &message);
}

/* Queries the state of the URL handle being used to fetch the document content,
 * updating the document if the data is available. */
static NavigationState poll_url_handle(Document *document)
{
	System *system = document->system;
	UrlCache *cache = system->url_cache;
	UrlHandle handle = document->url_handle;
	if (handle == INVALID_URL_HANDLE) {
		set_navigation_state(document, DOCNAV_IDLE);
		return DOCNAV_IDLE;
	}
	unsigned data_size;
	const void *data = cache->lock(handle, &data_size);
	if (data != NULL) {
		reset_document(document);
		int rc = parse(system, document, get_root(document), 
			(const char *)data, data_size);
		if (rc == STKR_OK) {
			set_navigation_state(document, DOCNAV_SUCCESS);
		} else {
			set_navigation_state(document, DOCNAV_PARSE_ERROR);
		}
		cache->unlock(handle);
	}
	return document->navigation_state;
}

/* URL cache callback for document fetch. */
unsigned document_fetch_notify_callback(UrlHandle handle, 
	UrlNotification type, UrlKey key, System *system, Document *document, 
	UrlFetchState fetch_state)
{
	key; handle; system;
	if (type == URL_NOTIFY_FETCH) {
		if (fetch_state == URL_FETCH_SUCCESSFUL || 
			fetch_state == URL_FETCH_DISK) {
			poll_url_handle(document);
		} else if (fetch_state == URL_FETCH_FAILED) {
			set_navigation_state(document, DOCNAV_FAILED);
		}
	} 
	return 0;
}

/* Returns the document's URL, or NULL if none is set. The semantics w.r.t. the
 * buffer are the same as those of parse_url().  */
ParsedUrl *get_url(const Document *document, void *buffer, unsigned buffer_size)
{
	UrlCache *cache = document->system->url_cache;
	if (cache == NULL || document->url_handle == INVALID_URL_HANDLE)
		return NULL;
	return cache->url(document->url_handle, buffer, buffer_size);
}

/* Sets the URL at which the document's content is considered to reside. This
 * does not initiate any network operation. */
int set_url(Document *document, const char *url)
{
	System *system = document->system;
	UrlCache *cache = system->url_cache;
	if (cache == NULL)
		return STKR_ERROR;
	
	/* If the URL is changing, make a new notification handle. */
	UrlKey key = cache->key(url);
	if (key != cache->key(document->url_handle)) {
		cache->destroy_handle(document->url_handle);
		document->url_handle = cache->create_handle(
			url, -1, URLP_NO_FETCH, DEFAULT_TTL_SECS,
			document, 0, system->document_notify_id,
			URL_FLAG_KEEP_URL);
	}

	set_navigation_state(document, DOCNAV_IDLE);
	return STKR_OK;
}

/* Attempts to load the document from a URL. */
int navigate(Document *document, const char *url, UrlFetchPriority priority)
{
	System *system = document->system;
	UrlCache *cache = system->url_cache;

	/* Set the target URL. */
	int rc = set_url(document, url);
	if (rc != STKR_OK)
		return rc;

	if (document->url_handle != INVALID_URL_HANDLE) {
		/* Request the URL. */
		cache->request(document->url_handle, priority);
		/* Poll the handle, since the data might be available immidiately. */
		poll_url_handle(document);
	}
	return document->navigation_state;
}

/* Returns the status any attempt to fetch content from a URL that is underway
 * for this document. */
NavigationState get_navigation_state(const Document *document)
{
	return document->navigation_state;
}

/* Returns the document's copy of the source text most recenly parsed into it,
 * or NULL if no source is available. */
const char *get_source(const Document *document, unsigned *out_size)
{
	if ((document->flags & DOCFLAG_KEEP_SOURCE) == 0) {
		if (out_size != NULL)
			*out_size = 0;
		return NULL;
	}
	if (out_size != NULL)
		*out_size = document->source_length;
	return document->source;
}

/* Stores a copy of markup being parsed into the document if the document
 * is configured to do so. */
void document_store_source(Document *document, const char *source, 
	unsigned length)
{
	if ((document->flags & DOCFLAG_KEEP_SOURCE) == 0 || 
		document->source_length != 0)
		return;
	if (document->source_capacity < length) {
		delete [] document->source;
		document->source = new char[length];
		document->source_capacity = length;
	}
	memcpy(document->source, source, length);
	document->source_length = length;
}

} // namespace stkr
