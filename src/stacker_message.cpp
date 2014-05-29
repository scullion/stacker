#include "stacker_message.h"

#include "stacker_document.h"
#include "stacker_node.h"
#include "stacker_util.h"

namespace stkr {

bool is_mouse_message(MessageType type)
{
	return type == MSG_MOUSE_MOVE ||
	       type == MSG_MOUSE_LEFT_DOWN ||
	       type == MSG_MOUSE_LEFT_UP ||
	       type == MSG_MOUSE_RIGHT_DOWN ||
	       type == MSG_MOUSE_RIGHT_UP;
}

bool is_keyboard_message(MessageType type)
{
	return type == MSG_KEY_DOWN || type == MSG_KEY_UP;
}

void init_message_queue(MessageQueue *queue, unsigned capacity)
{
	assertb(capacity != 0);
	capacity = next_power_of_two(capacity);
	queue->messages = new Message[capacity];
	queue->capacity = capacity;
	queue->head = 0;
	queue->tail = 0;
}

void deinit_message_queue(MessageQueue *queue)
{
	delete [] queue->messages;
}

void clear_message_queue(MessageQueue *queue)
{
	queue->head = 0;
	queue->tail = 0;
}

void enqueue_message(MessageQueue *queue, const Message *message)
{
	unsigned mask = queue->capacity - 1;
	unsigned next = (queue->tail + 1) & mask;
	if (next == queue->head) {
		unsigned new_capacity = std::max(2 * queue->capacity, 
			DEFAULT_MESSAGE_QUEUE_CAPACITY);
		Message *messages = new Message[new_capacity], *m = messages;
		do {
			*m++ = queue->messages[queue->head];
			queue->head = (queue->head + 1) & mask;
		} while (queue->head != queue->tail);
		queue->messages = messages;
		queue->head = 0;
		queue->tail = queue->capacity;
		queue->capacity = new_capacity;
		next = (queue->tail + 1) & (new_capacity - 1);
	}
	queue->messages[queue->tail] = *message;
	queue->tail = next;
}

const Message *dequeue_message(MessageQueue *queue)
{
	if (queue->head == queue->tail)
		return NULL;
	const Message *message = queue->messages + queue->head;
	queue->head = (queue->head + 1) & (queue->capacity - 1);
	return message;
}

/* If a node contains the mouse and defines a cursor, update the document 
 * cursor. */
static bool maybe_set_cursor(Document *document, Node *node)
{
	if ((node->flags & (NFLAG_MOUSE_OVER | NFLAG_MOUSE_OVER_CHILD)) == 0)
		return false;
	int node_cursor = read_mode(node, TOKEN_CURSOR);
	if (node_cursor != ADEF_UNDEFINED) {
		set_cursor(document, (CursorType)node_cursor);
		return true;
	}
	return false;
}

/* Updates a node's interaction flags in response to a box-hit message for the
 * node or one of its children. */
static bool handle_node_hit(Document *document, Node *node, 
	const Message *message)
{
	/* If the node isn't yet in the hit set, update its mouse flags. */
	if (node->mouse_hit_stamp != document->hit_clock) {
		/* Update flags. A node may receive many box-hit messages in a tick,
		 * but we only the first will ever have HITFLAG_TOPMOST set, so the
		 * mouse flags can be fully determined from the first message each 
		 * tick.*/
		set_node_flags(document, node, NFLAG_MOUSE_INSIDE, true);
		set_node_flags(document, node, NFLAG_MOUSE_OVER | NFLAG_MOUSE_OVER_CHILD, false);
		if ((message->flags & HITFLAG_TOPMOST) != 0) {
			set_node_flags(document, node, NFLAG_MOUSE_OVER_CHILD, true);
			if (message->hit.hit_node == node)
				set_node_flags(document, node, NFLAG_MOUSE_OVER, true);
		}
		node->mouse_hit_stamp = document->hit_clock;
		
		/* Add the node to the hit chain. */
		if ((node->flags & NFLAG_IN_HIT_CHAIN) == 0) {
			list_insert_before(
				(void **)&document->hit_chain_head, 
				(void **)&document->hit_chain_tail,
				node, NULL, offsetof(Node, hit_prev));
			set_node_flags(document, node, NFLAG_IN_HIT_CHAIN, true);
		}
	}

	/* If this node defines the cursor, consume the message. */
	if ((message->flags & MFLAG_HANDLED) == 0 &&
		maybe_set_cursor(document, node))
		return true;

	return false;
}

/* Updates a node's mouse flags in response to a notification that there were
 * no hits in its tree this tick. */
static bool handle_node_unhit(Document *document, Node *node, 
	const Message *message)
{
	message;
	/* Remove the node from the hit chain. */
	if ((node->flags & NFLAG_IN_HIT_CHAIN) != 0) {
		list_remove(
			(void **)&document->hit_chain_head, 
			(void **)&document->hit_chain_tail,
			node, offsetof(Node, hit_prev));
	}
	set_node_flags(document, node, NFLAG_MOUSE_OVER | NFLAG_MOUSE_OVER_CHILD | 
		NFLAG_MOUSE_INSIDE | NFLAG_IN_HIT_CHAIN, false);
	return false; /* Always propagate up. */
}

/* Default node message handler. */
static bool handle_node_message(Document *document, Node *node, 
	const Message *message)
{
	switch (message->type) {
		case MSG_NODE_HIT:
			return handle_node_hit(document, node, message);
		case MSG_NODE_UNHIT:
			return handle_node_unhit(document, node, message);
	}
	return false;
}

static void notify_activated(Document *document, Node *node)
{
	Message message;
	message.type = MSG_NODE_ACTIVATED;
	message.flags = 0;
	message.activation.node = node;
	enqueue_message(document, &message);
}

/* Hyperlink message handler. */
static bool handle_hyperlink_message(Document *document, Node *node, 
	const Message *message)
{
	if (message->type == MSG_NODE_HIT || message->type == MSG_NODE_UNHIT) {
		handle_node_message(document, node, message);
		bool highlight = is_enabled(node) && 
			(node->flags & NFLAG_MOUSE_OVER_CHILD) != 0;
		set_interaction_state(document, node, NFLAG_INTERACTION_HIGHLIGHTED,
			highlight);
		return true;
	} else if (message->type == MSG_MOUSE_LEFT_DOWN) {
		if (is_enabled(node))
			notify_activated(document, node); 
		return true;
	}
	return false;
}


/* Sends a message to a node and its parents. Returns true if the mesage was
 * handled. */
bool send_message(Document *document, Node *node, Message *message)
{
	message->flags &= ~MFLAG_HANDLED;
	message->flags |= MFLAG_PROPAGATE;
	while (node != NULL && (message->flags & MFLAG_PROPAGATE) != 0) {
		bool handled = false;
		switch (node->type) {
			case LNODE_HYPERLINK:
				handled = handle_hyperlink_message(document, node, message);
				break;
		}
		if (handled || handle_node_message(document, node, message))
			message->flags |= MFLAG_HANDLED;
		node = node->parent;
	}
	if ((message->flags & MFLAG_PROPAGATE) != 0 && 
		document_handle_message(document, message))
		message->flags |= MFLAG_HANDLED;
	if ((message->flags & MFLAG_PROPAGATE) != 0)
		enqueue_message(document, message);
	return (message->flags & MFLAG_HANDLED) != 0;
}

} // namespace stkr
