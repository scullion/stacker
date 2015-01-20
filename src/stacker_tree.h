#pragma once

#include <cstdint>

namespace stkr {

struct Tree;
struct Node;
struct Box;
struct Document;

union TreeLink {
	Tree *tree;
	Node *node;
	Box *box;
};

/* Header for nodes and boxes. */
struct Tree {
	TreeLink parent;
	TreeLink prev;
	TreeLink next;
	TreeLink first;
	TreeLink last;
	TreeLink counterpart;
	unsigned flags;
};

/* Flags common to nodes and boxes. TreeFlag and NodeFlag/BoxFlag must remain
 * disjoint. */
enum TreeFlag {
	TREEFLAG_IS_BOX              = 1 << 30, /* Box if set, node if clear. */
	TREEFLAG_IS_INLINE_CONTAINER = 1 << 31  /* Is an inline container node or box. */
};

void tree_check(const Tree *tree);
void tree_init(Tree *tree, unsigned flags);
void tree_remove(Tree *child);
void tree_remove_from_parent(Tree *parent, Tree *child);
void tree_remove_children(Tree *parent);
void tree_insert_child_before(Tree *parent, Tree *child, Tree *before);
void tree_detach_siblings(Tree *child);
unsigned tree_count_children(const Tree *parent);
const Tree *tree_next_up(const Tree *root, const Tree *node);
const Tree *tree_next(const Tree *root, const Tree *node);
bool tree_is_child(const Tree *child, const Tree *parent);
bool tree_is_child_or_self(const Tree *child, const Tree *parent);
const Tree *lowest_common_ancestor(const Tree *a, const Tree *b,
	const Tree **below_a, const Tree **below_b);
bool tree_before(const Tree *a, const Tree *b);

const unsigned TREE_ITERATOR_INITIAL_CAPACITY = 16;

const unsigned TIF_VISIT_PREORDER  = 1 << 0;
const unsigned TIF_VISIT_POSTORDER = 1 << 1;
const unsigned TIF_END             = 1 << 2;
const unsigned TIF_USER            = 1 << 3;
const unsigned TIF_VISIT_LEAF      = TIF_VISIT_PREORDER | TIF_VISIT_POSTORDER;

/* Incremental iterator for node and box trees. */
struct TreeIterator {
	const Document *document;
	const Tree *first;
	const Tree *last;
	const Tree *node;
	uint8_t flags;
	bool using_heap;
	uint8_t *stack;
	uint8_t *frame;
	unsigned frame_size;
	unsigned capacity;
};

/* Tells a tree iterator whether to descend into children. */
enum TreeIteratorMode {
	TIMODE_DEFAULT, // Descend if this subtree hasn't been visited.
	TIMODE_DOWN,    // Descend even if this subtree has already been visited.
	TIMODE_UP       // Don't descend.
};

enum TreeIteratorStep {
	TISTEP_NONE,
	TISTEP_DOWN,
	TISTEP_RIGHT,
	TISTEP_UP
};

void tree_iterator_init(TreeIterator *ti);
void tree_iterator_deinit(TreeIterator *ti);
unsigned tree_iterator_begin(
	TreeIterator *ti, 
	const Document *document, 
	const Tree *first, 
	const Tree *last,
	unsigned frame_size);
unsigned tree_iterator_step(TreeIterator *ti, 
	TreeIteratorMode mode = TIMODE_DEFAULT);
bool tree_iterator_should_step_into(unsigned flags, 
	TreeIteratorMode mode = TIMODE_DEFAULT);
TreeIteratorStep tree_iterator_query_step(const TreeIterator *ti, 
	TreeIteratorMode mode = TIMODE_DEFAULT);
unsigned tree_iterator_flags(const Tree *tree, TreeIteratorStep step);
unsigned tree_iterator_jump(TreeIterator *ti, const Tree *target, unsigned flags);
unsigned tree_iterator_revisit(TreeIterator *ti, const Tree *target);
void *tree_iterator_push(TreeIterator *ti);
void *tree_iterator_pop(TreeIterator *ti);
void *tree_iterator_peek(TreeIterator *ti, unsigned n = 0);
void tree_iterator_set_buffer(TreeIterator *ti, 
	uint8_t *buffer, unsigned buffer_size);

} // namespace stkr
