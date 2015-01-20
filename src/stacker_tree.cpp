#include "stacker_tree.h"
#include "stacker_util.h"

#include <cstddef>

namespace stkr {

/* Recursively checks that a tree is well-formed. */
void tree_check(const Tree *tree)
{
	const Tree *parent = tree->parent.tree;
	const Tree *next = tree->next.tree;
	const Tree *prev = tree->prev.tree;
	if (parent == NULL) {
		ensure(next == NULL);
		ensure(prev == NULL);
	} else {
		if (next != NULL) {
			ensure(next->prev.tree == tree);
			ensure(next->parent.tree == parent);
		}
		if (prev != NULL) {
			ensure(prev->next.tree == tree);
			ensure(prev->parent.tree == parent);
		}
	}
	const Tree *child = tree->first.tree;
	while (child != NULL) {
		ensure(child->parent.tree == tree);
		tree_check(child);
		if (child->next.tree == NULL)
			break;
		child = child->next.tree;
	}
	ensure(child == tree->last.tree);
}

void tree_init(Tree *tree, unsigned flags)
{
	tree->parent.tree = NULL;
	tree->prev.tree = NULL;
	tree->next.tree = NULL;
	tree->first.tree = NULL;
	tree->last.tree = NULL;
	tree->counterpart.tree = NULL;
	tree->flags = flags;
}

/* Removes a 'child' from its sibling chain. */
void tree_remove_from_parent(Tree *parent, Tree *child)
{
	assertb(child->parent.tree == parent);
	list_remove(
		(void **)&parent->first.tree, 
		(void **)&parent->last.tree, 
		child, offsetof(Tree, prev));
	child->parent.tree = NULL;
}

/* Removes all immediate children of a tree. */
void tree_remove_children(Tree *parent)
{
	for (Tree *child = parent->first.tree; child != NULL; 
		child = child->next.tree)
		child->parent.tree = NULL;
	parent->first.tree = NULL;
	parent->last.tree = NULL;
}

/* Removes 'child' from the tree. */
void tree_remove(Tree *child)
{
	if (child->parent.tree != NULL)
		tree_remove_from_parent(child->parent.tree, child);
}

/* Inserts a child before the specified node in a sibling chain. If 'before' is
 * NULL, the child becomes the last child. */
void tree_insert_child_before(Tree *parent, Tree *child, Tree *before)
{
	assertb(child->parent.tree == NULL || child->parent.tree == parent);
	assertb(before == NULL || before->parent.tree == parent);
	list_insert_before(
		(void **)&parent->first.tree, 
		(void **)&parent->last.tree, 
		child, before, offsetof(Tree, prev));
	child->parent.tree = parent;
}

/* Removes 'child' and all its next siblings from the sibling chain of their
 * shared parent. Does not clear parent pointers. */
void tree_detach_siblings(Tree *child)
{
	assertb(child != NULL);
	Tree *prev = child->prev.tree;
	Tree *parent = child->parent.tree;
	if (prev != NULL)
		prev->next.tree = NULL;
	if (parent != NULL) {
		parent->last.tree = prev;
		if (prev == NULL)
			parent->first.tree = NULL;
	}
}

/* Returns the number of immediate children of a node. */
unsigned tree_count_children(const Tree *parent)
{
	unsigned num_children = 0;
	for (const Tree *child = parent->first.tree; child != NULL; 
		child = child->next.tree)
		num_children++;
	return num_children;
}

/* Yields nodes under 'root' in tree order, not traversing into children. */
const Tree *tree_next_up(const Tree *root, const Tree *tree)
{
	assertb(tree != NULL);
	assertb(tree_is_child_or_self(tree, root));
	while (tree != root) {
		if (tree->next.tree != NULL)
			return tree->next.tree;
		tree = tree->parent.tree;
	}
	return NULL;
}

/* Yields nodes under 'root' in tree order. */
const Tree *tree_next(const Tree *root, const Tree *tree)
{
	assertb(tree != NULL);
	assertb(tree_is_child_or_self(tree, root));
	if (tree->first.tree != NULL)
		return tree->first.tree;
	return tree_next_up(root, tree);
}

/* True if child is in the subtree of parent. False if child == parent. */
bool tree_is_child(const Tree *child, const Tree *parent)
{
	assertb(parent != NULL);
	assertb(child != NULL);
	do {
		if (child->parent.tree == parent)
			return true;
		child = child->parent.tree;
	} while (child != NULL);
	return false;
}

/* True if child is in the subtree of parent or child == parent. */
bool tree_is_child_or_self(const Tree *child, const Tree *parent)
{
	assertb(parent != NULL)
	assertb(child != NULL);
	do {
		if (child == parent)
			return true;
		child = child->parent.tree;
	} while (child != NULL);
	return false;
}

/* Sets the capacity of tree iterator's stacks. The capacity is a number of
 * bytes, and should be a multiple of the iterator's frame size. */
static void tree_iterator_reallocate(TreeIterator *ti, unsigned new_capacity)
{
	uint8_t *new_stack = new uint8_t[new_capacity];
	if (ti->frame >= ti->stack) {
		memcpy(new_stack, ti->stack, ti->capacity);
		if (ti->using_heap)
			delete [] ti->stack;
	}
	ti->frame = new_stack + (ti->frame - ti->stack);
	ti->stack = new_stack;
	ti->capacity = new_capacity;
	ti->using_heap = true;
}

/* Deallocates tree iterator memory. */
void tree_iterator_deinit(TreeIterator *ti)
{
	if (ti->using_heap)
		delete [] ti->stack; 
}

/* Initializes a tree iterator. */
void tree_iterator_init(TreeIterator *ti)
{
	memset(ti, 0, sizeof(TreeIterator));
}

/* Supplies a temporary buffer that the iterator may use until it is 
 * deinitialized, or until the buffer is invalidated by a subsequent cal to
 * this function. */
void tree_iterator_set_buffer(TreeIterator *ti, uint8_t *buffer, 
	unsigned buffer_size)
{
	if (ti->using_heap)
		return;
	ti->capacity = buffer_size;
	ti->stack = buffer;
}

/* Prepares a tree iterator for callback based preorder and/or postorder 
 * traversal. */
unsigned tree_iterator_begin(
	TreeIterator *ti, 
	const Document *document, 
	const Tree *first, 
	const Tree *last,
	unsigned frame_size)
{
	ti->document = document;
	ti->first = first;
	ti->last = last; 
	ti->node = first;
	ti->frame = ti->stack - frame_size;
	ti->frame_size = frame_size;
	if (frame_size > ti->capacity)
		tree_iterator_reallocate(ti, TREE_ITERATOR_INITIAL_CAPACITY * frame_size);
	if (first != NULL) {
		if (first->first.tree != NULL)
			ti->flags = TIF_VISIT_PREORDER;
		else
			ti->flags = TIF_VISIT_LEAF;
	} else {
		ti->flags = 0;
	}
	return ti->flags;
}

/* Adds a frame to the top of the iterator stack. Returns the down frame and
 * zero-initializes the up frame. */
void *tree_iterator_push(TreeIterator *ti)
{
	ti->frame += ti->frame_size;
	if (ti->frame + ti->frame_size > ti->stack + ti->capacity)
		tree_iterator_reallocate(ti, 3 * ti->capacity / 2);
	memset(ti->frame, 0, ti->frame_size);
	return ti->frame;
}

void *tree_iterator_pop(TreeIterator *ti)
{
	void *frame = NULL;
	if (ti->frame >= ti->stack) {
		frame = ti->frame;
		ti->frame -= ti->frame_size;
	}
	return frame;
}

/* Returns a stack frame by index, zero representing the top of the stack. 
 * Returns NULL if the index is out of bounds. */
void *tree_iterator_peek(TreeIterator *ti, unsigned n)
{
	void *frame = ti->frame - ti->frame_size * n;
	return (frame >= ti->stack) ? frame : NULL;
}

/* True if the children of the current node of a tree iterator should be 
 * visited. */
bool tree_iterator_should_step_into(unsigned flags, TreeIteratorMode mode)
{
	switch (mode) {
		case TIMODE_DOWN:
			if (flags == TIF_VISIT_POSTORDER)
				return true;
		case TIMODE_DEFAULT:
			return flags == TIF_VISIT_PREORDER;
	}
	return false;
}

TreeIteratorStep tree_iterator_query_step(const TreeIterator *ti, 
	TreeIteratorMode mode)
{
	if (tree_iterator_should_step_into(ti->flags, mode))
		return TISTEP_DOWN;
	if (ti->node == ti->last)
		return TISTEP_NONE;
	return (ti->node->next.tree) != NULL ? TISTEP_RIGHT : TISTEP_UP;
}

unsigned tree_iterator_flags(const Tree *tree, TreeIteratorStep step)
{
	unsigned flags = 0;
	switch (step) {
		case TISTEP_DOWN:
		case TISTEP_RIGHT:
			flags = TIF_VISIT_PREORDER;
			if (tree == NULL || tree->first.tree == NULL)
				flags |= TIF_VISIT_POSTORDER;
			break;
		case TISTEP_UP:
			flags = TIF_VISIT_POSTORDER;
			break;
		case TISTEP_NONE:
			flags = TIF_END;
			break;
	}
	return flags;
}

/* Advances a tree iterator to the next node and returns a mask indicating
 * whether the node should be visited preorder, postorder or both. Returns zero
 * when there are no more nodes. */
unsigned tree_iterator_step(TreeIterator *ti, TreeIteratorMode mode)
{
	if (ti->node == NULL)
		return TIF_END;
	unsigned flags = TIF_VISIT_PREORDER;
	if (tree_iterator_should_step_into(ti->flags, mode)) {
		ti->node = ti->node->first.tree;
	} else {
		const Tree *next = NULL;
		if (ti->node != ti->last) {
			if (ti->node->next.tree != NULL) {
				next = ti->node->next.tree;
			} else {
				next = ti->node->parent.tree;
				flags = TIF_VISIT_POSTORDER;
			}
		}
		ti->node = next;
		if (next == NULL) {
			ti->flags = TIF_END;
			return TIF_END;
		}
	}
	if (ti->node->first.tree == NULL)
		flags |= TIF_VISIT_POSTORDER;
	ti->flags = (uint8_t)flags;
	return flags;
}

/* Moves the iterator to an arbitrary node. */
unsigned tree_iterator_jump(TreeIterator *ti, const Tree *target, 
	unsigned flags)
{
	ti->node = target;
	ti->flags = (uint8_t)flags;
	return flags;
}

/* Convenience to jump to a node as if we were re-encountering it in while
 * traversing down or right. */
unsigned tree_iterator_revisit(TreeIterator *ti, const Tree *target)
{
	unsigned flags = tree_iterator_flags(target, TISTEP_DOWN);
	return tree_iterator_jump(ti, target, flags);
}

/* Determines the first tree ancestor common to A and B. The result is NULL
 * if the nodes are not part of the same tree. */
const Tree *lowest_common_ancestor(const Tree *a, const Tree *b,
	const Tree **below_a, const Tree **below_b)
{
	static const unsigned MAX_TREE_DEPTH = 64;

	const Tree *pa[MAX_TREE_DEPTH], *pb[MAX_TREE_DEPTH];
	unsigned da = 0, db = 0;
	while (a != NULL) {
		ensure(da != MAX_TREE_DEPTH);
		pa[da++] = a;
		a = a->parent.tree;
	}
	while (b != NULL) {
		ensure(db != MAX_TREE_DEPTH);
		pb[db++] = b;
		b = b->parent.tree;
	}
	const Tree *ancestor = NULL;
	do {
		if (pa[--da] != pb[--db])
			break;
		ancestor = pa[da];
	} while (da != 0 && db != 0);
	if (below_a != NULL)
		*below_a = (ancestor != a) ? pa[da] : NULL;
	if (below_b != NULL)
		*below_b = (ancestor != b) ? pb[db] : NULL;
	return ancestor;
}

/* True if A is before B in the tree. */
bool tree_before(const Tree *a, const Tree *b)
{
	const Tree *ba, *bb;
	const Tree *ancestor = lowest_common_ancestor(a, b, &ba, &bb);
	ensure(ancestor != NULL); /* Undefined if A and B are not in the same tree. */
	if (ancestor == b) return false; /* A is a child of B or A == B. */
	if (ancestor == a) return true;  /* B is a child of A. */
	while (ba != NULL) {
		if (ba == bb)
			return true;
		ba = ba->next.tree;
	}
	return false;
}

} // namespace stkr

