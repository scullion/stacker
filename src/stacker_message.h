#pragma once

#include "stacker_attribute.h"

namespace stkr {

struct Document;
struct Node;
struct Box;
struct View;
enum NavigationState;

enum MessageType {
	/* Mouse messages. */
	MSG_MOUSE_MOVE,
	MSG_MOUSE_LEFT_DOWN,
	MSG_MOUSE_LEFT_UP,
	MSG_MOUSE_RIGHT_DOWN,
	MSG_MOUSE_RIGHT_UP,

	/* Keyboard messages. */
	MSG_KEY_DOWN,
	MSG_KEY_UP,

	/* Hit-test messages. */
	MSG_NODE_HIT,         // A node's box was hit.
	MSG_NODE_UNHIT,       // None of the boxes of a node or its children were hit this tick. 
	MSG_CURSOR_CHANGED,   // The document's cursor has changed.

	/* Notifications. */
	MSG_NODE_EXPANDED,    // Says that a node has changed size and, if possible, classifies
	                      // the change as an expansion (or contraction) up, down, left or right.
	MSG_NODE_ACTIVATED,   // An activatable node like a link or button has been clicked.
	MSG_NAVIGATE          // Document navigation state has changed.
};


enum MessageFlag {
	MFLAG_PROPAGATE = 1 << 30, // The message should be passed to parent nodes.
	MFLAG_HANDLED   = 1 << 31  // The message has been handled. Handled messages can still propagate.
};

enum MouseMessageFlag {
	MMF_CTRL   = 1 << 0,
	MMF_SHIFT  = 1 << 1,
	MMF_ALT    = 1 << 2
};

enum KeyboardMessageFlag {
	KMF_CTRL   = 1 << 0,
	KMF_SHIFT  = 1 << 1,
	KMF_ALT    = 1 << 2
};

enum HitMessageFlag {
	HITFLAG_TOPMOST = 1 << 0 // The hit box is the top of the hit stack.
};

struct MouseMessage { 
	float x, y;
	struct View *view;
};

struct KeyboardMessage {
	unsigned code;
	struct View *view;
};

struct HitMessage {
	Node *hit_node;
	Box *hit_box;
};

struct CursorMessage {
	CursorType cursor;
};

enum ExpansionMessageFlag {
	EMF_EXPANDED_LEFT  = 1 << 0,
	EMF_EXPANDED_RIGHT = 1 << 1,
	EMF_EXPANDED_UP    = 1 << 2,
	EMF_EXPANDED_DOWN  = 1 << 3
};

struct ExpansionMessage {
	Node *node;
};

struct ActivationMessage {
	Node *node;
};

struct NavigationMessage {
	NavigationState old_state;
	NavigationState new_state;
};

struct Message {
	MessageType type;
	unsigned flags;
	union {
		MouseMessage mouse;
		KeyboardMessage keyboard;
		HitMessage hit;
		CursorMessage cursor;
		ExpansionMessage expansion;
		ActivationMessage activation;
		NavigationMessage navigation;
	};
};

struct MessageQueue {
	Message *messages;
	unsigned head, tail;
	unsigned capacity;
};

const unsigned DEFAULT_MESSAGE_QUEUE_CAPACITY = 32;

bool is_mouse_message(MessageType type);
bool is_keyboard_message(MessageType type);
void init_message_queue(MessageQueue *queue, 
	unsigned capacity = DEFAULT_MESSAGE_QUEUE_CAPACITY);
void deinit_message_queue(MessageQueue *queue);
void clear_message_queue(MessageQueue *queue);
void enqueue_message(MessageQueue *queue, const Message *message);
const Message *dequeue_message(MessageQueue *queue);

} // namespace stkr
