#pragma once

#include "stacker_attribute.h"
#include "stacker_rule.h"
#include "stacker_inline.h"
#include "stacker_diagnostics.h"
#include "stacker_quadtree.h"
#include "stacker_message.h"
#include "url_cache.h"

namespace stkr {

struct Box;

struct Document {
	struct System *system;

	Node *root;
	unsigned flags;
	unsigned layout_clock;
	unsigned change_clock; 
	unsigned change_clock_at_layout;
	unsigned root_dims[2];

	/* Rules. */
	RuleTable rules;
	unsigned global_rule_table_revision;
	unsigned rule_revision_at_layout;

	/* Styling. */
	uint32_t selected_text_color;
	uint32_t selected_text_fill_color;

	/* Box quadtree. */
	Grid grid;
	unsigned box_query_stamp;
	
	/* Node hit testing. */
	Node *hit_chain_head;
	Node *hit_chain_tail;
	unsigned hit_clock;
	Node *hit_node;
	Node *mouse_down_node;
	CursorType cursor;

	/* Mouse selection. */
	CaretAddress selection_start;
	CaretAddress selection_end;
	Node *selection_chain_head;
	Node *selection_chain_tail;
	Box *debug_start_anchor;
	Box *debug_end_anchor;
	float mouse_down_x;
	float mouse_down_y;
	float mouse_last_x;
	float mouse_last_y;
	unsigned mouse_modifiers;
	const View *selection_view;

	/* Message queue. */
	MessageQueue message_queue;

	/* Navigation state. */
	urlcache::UrlHandle url_handle;
	NavigationState navigation_state;

	/* Markup storage. */
	char *source;
	unsigned source_length;
	unsigned source_capacity;

	/* Diagnostics. */
	DumpCallback dump;
	void *dump_data;
};

bool needs_update(const Document *document);
void document_notify_box_destroy(Document *document, Box *box);
void document_notify_node_destroy(Document *document, Node *node);
void document_notify_node_changed(Document *document, Node *node);
void impose_root_constraints(Document *d);
void document_dump(const Document *document, const char *fmt, ...);
bool document_handle_message(Document *document, Message *message);
void document_handle_mouse_event(Document *document, View *view, 
	MessageType type, float doc_x, float doc_y, unsigned flags);
void document_handle_keyboard_event(Document *document, View *view, 
	MessageType type, unsigned key_code, unsigned flags);
Node *process_hit_stack(Document *document, Box **hit_stack,
	unsigned hit_count, float x, float y);
void clear_selection(Document *document);
void set_cursor(Document *document, CursorType new_cursor);
void enqueue_message(Document *document, const Message *message);
unsigned document_fetch_notify_callback(urlcache::UrlHandle handle, 
	urlcache::UrlNotification type, urlcache::UrlKey key, 
	System *system, Document *document, urlcache::UrlFetchState fetch_state);
void document_store_source(Document *document, const char *source, 
	unsigned length);

} // namespace stkr

