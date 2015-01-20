#pragma once

#include "stacker_platform.h"
#include "stacker_attribute.h"
#include "stacker_rule.h"
#include "stacker_inline2.h"
#include "stacker_diagnostics.h"
#include "stacker_quadtree.h"
#include "stacker_message.h"
#include "stacker_layout.h"
#include "url_cache.h"

namespace stkr {

struct Box;

const int INVALID_VIEW_ID = -1;

/* The top-level progress of a document update. */
enum DocumentUpdateStage {
	DUS_PRE_LAYOUT,  // Update nodes before layout.
	DUS_LAYOUT,      // Compute box layout.
	DUS_POST_LAYOUT, // Update nodes after layout.
	DUS_COMPLETE     // Done.
};

/* The progress in updating the current node in the pre-layout pass. */
enum NodePreLayoutUpdateStage {
	NUS_UPDATE,           // Main update. Do preorder and postorder operations as specified by the iterator.
	NUS_COMPLETE          // Done.
};

const unsigned INCREMENTAL_UPDATE_SCRATCH_BYTES = 512;

/* The state of an incremental document update. */
struct IncrementalUpdateState {
	DocumentUpdateStage stage;
	NodePreLayoutUpdateStage pre_layout_stage;
	TimerValue start_time;
	uintptr_t timeout;
	TreeIterator iterator;
	IncrementalLayoutState layout_state;
	uint8_t scratch_buffer[INCREMENTAL_UPDATE_SCRATCH_BYTES];
};

struct Document {
	struct System *system;

	Node *root;
	unsigned flags;
	unsigned update_clock;
	unsigned change_clock; 
	unsigned change_clock_at_update;
	unsigned root_dims[2];

	/* Views. */
	View *views;
	unsigned available_view_ids;

	/* Box free list. */
	Box *free_boxes;

	/* Rules. */
	RuleTable rules;
	unsigned global_rule_table_revision;
	unsigned rule_revision_at_update;

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

	/* Incremental update state. */
	IncrementalUpdateState *update;

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
bool check_interrupt(const Document *document);
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
int allocate_view_id(Document *document);
void deallocate_view_id(Document *document, int id);
void add_to_view_list(Document *document, View *view);
void remove_from_view_list(Document *document, View *view);

} // namespace stkr


