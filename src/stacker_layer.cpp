#include "stacker_layer.h"

#include <cmath>

#include <algorithm>

#include "stacker.h"
#include "stacker_shared.h"
#include "stacker_util.h"
#include "stacker_node.h"
#include "stacker_document.h"
#include "stacker_system.h"
#include "stacker_box.h"
#include "stacker_layout.h"
#include "stacker_platform.h"

#include "url_cache.h"

namespace stkr {

using namespace urlcache;

/* Returns a pointer to the text array located after a text layer in memory. */
const char *get_text_layer_text(const VisualLayer *layer)
{
	return (const char *)(layer + 1);
}

/* Returns a text layer's array of character flags. */
const uint16_t *get_text_layer_flags(const VisualLayer *layer)
{
	return (const uint16_t *)(get_text_layer_text(layer) + layer->text.length);
}

/* Returns a text layer's colour palette. */
const uint32_t *get_text_layer_palette(const VisualLayer *layer)
{
	return (const uint32_t *)(get_text_layer_flags(layer) + layer->text.length);
}

/* Returns a pointer to the array of struct { int x, y; } positions located
 * after a text layer in memory. */
const int *get_text_layer_positions(const VisualLayer *layer)
{
	return (const int *)(get_text_layer_palette(layer) + layer->text.num_colors);
}

/* Default-initializes a LayerPosition structure. */
void initialize_layer_position(LayerPosition *lp)
{
	for (unsigned axis = 0; axis < 2; ++axis) {
		lp->placement = BBOX_PADDING;
		lp->positioning_mode = VLPM_STANDARD;
		lp->alignment[axis] = (Alignment)ADEF_UNDEFINED;
		lp->dims[axis] = 0.0f;
		lp->offsets[axis] = 0.0f;
		lp->mode_offset[axis] = ADEF_UNDEFINED;
		lp->mode_size[axis] = ADEF_UNDEFINED;
	}
}

/* Calculates the document-space rectangle of a layer. */
void compute_layer_position(const Box *box, const LayerPosition *lp, 
	float *r, float natural_width, float natural_height, 
	bool use_natural)
{
	/* Get the positioning reference box. */
	float ref[4], box_dims[2], specified_dims[2], offsets[2], defaults[2];	
	bounding_box_rectangle(box, (BoundingBox)lp->placement, ref);
	
	/* Resolve fractional offsets and sizes relative to the container. */
	if (use_natural) {
		defaults[AXIS_H] = natural_width;
		defaults[AXIS_V] = natural_height;
	} else {
		defaults[AXIS_H] = rdim(ref, AXIS_H);
		defaults[AXIS_V] = rdim(ref, AXIS_V);
	}
	for (unsigned axis = 0; axis < 2; ++axis) {
		box_dims[axis] = rdim(ref, (Axis)axis);
		specified_dims[axis] = relative_dimension(
			(DimensionMode)lp->mode_size[axis], 
			lp->dims[axis], box_dims[axis], defaults[axis]);
		offsets[axis] = relative_dimension(
			(DimensionMode)lp->mode_offset[axis], 
			lp->offsets[axis], box_dims[axis], 0.0f);
	}

	/* Apply non-standard positioning modes if the image has a natural size. */
	if (use_natural && (lp->positioning_mode == VLPM_FIT || lp->positioning_mode == VLPM_FILL)) {
		float scale_x = natural_width != 0.0f ? 
			rdim(ref, AXIS_H) / natural_width : 0.0f;
		float scale_y = natural_height != 0.0f ? 
			rdim(ref, AXIS_V) / natural_height : 0.0f;
		bool x_greater = fabsf(scale_x - 1.0f) > fabsf(scale_y - 1.0f);
		bool use_greater = (lp->positioning_mode == VLPM_FIT);
		float scale = (x_greater == use_greater) ? scale_x : scale_y;
		specified_dims[AXIS_H] = natural_width * scale;
		specified_dims[AXIS_V] = natural_height * scale;
	}

	/* Construct the rectangle. */
	for (unsigned axis = 0; axis < 2; ++axis) {
		align_1d((Alignment)lp->alignment[axis], 
			specified_dims[axis], offsets[axis], 
			side(ref, (Axis)axis, 0), 
			side(ref, (Axis)axis, 1),
			sidep(r, (Axis)axis, 0), 
			sidep(r, (Axis)axis, 1));
	}
}

VisualLayer *create_layer(Document *document, const Node *node, 
	VisualLayerType type, unsigned extra)
{
	document; node; 

	VisualLayer *layer = (VisualLayer *)(new char[sizeof(VisualLayer) + extra]);
	layer->type = type;
	layer->next[VLCHAIN_BOX] = NULL;
	layer->next[VLCHAIN_NODE] = NULL;
	layer->flags = 0;
	layer->depth_offset = 0;
	layer->key = LKEY_INVALID;
	if (type == VLT_IMAGE) {
		layer->image.notify_handle = INVALID_URL_HANDLE;
		layer->image.image_handle = INVALID_URL_HANDLE;
		initialize_layer_position(&layer->image.position);
		layer->image.tint = 0xFFFFFFFF;
	} else if (type == VLT_PANE) {
		layer->pane.pane_type = PANE_LAST;
		layer->pane.border_color = 0;
		layer->pane.border_width = 0;
		layer->pane.fill_color = 0;
		initialize_layer_position(&layer->pane.position);
	} else if (type == VLT_TEXT) {
		layer->text.key = 0;
		layer->text.font_id = INVALID_FONT_ID;
		layer->text.flags = 0;
		layer->text.length = 0;
		layer->text.num_colors = 0;
	}
	return layer;
}

void destroy_layer(Document *document, VisualLayer *layer)
{
	assertb((layer->flags & (VLFLAG_IN_BOX_CHAIN | VLFLAG_IN_NODE_CHAIN)) == 0);
	if (layer->type == VLT_IMAGE)
		clear_image_layer_url(document, layer);
	delete [] (char *)layer;
}

void release_layer(Document *document, VisualLayer *layer)
{
	if ((layer->flags & (VLFLAG_IN_BOX_CHAIN | VLFLAG_IN_NODE_CHAIN)) == 0)
		destroy_layer(document, layer);
}

/* Destroys layers in a layer chain than are not in use by another chain. */
void release_layer_chain(Document *document, 
	VisualLayerChain chain, VisualLayer *head)
{
	for (VisualLayer *layer = head, *next; layer != NULL; layer = next) {
		next = layer->next[chain];
		layer->flags &= ~(1 << chain);
		release_layer(document, layer);
	}
}

VisualLayer *layer_chain_lower_bound(VisualLayerChain chain, 
	VisualLayer *head, LayerKey key)
{
	VisualLayer *next = head, *prev = NULL;
	while (next != NULL && next->key < key) {
		prev = next;
		next = next->next[chain];
	}
	return prev;
}

VisualLayer *layer_chain_find(VisualLayerChain chain, 
	VisualLayer *head, LayerKey key)
{
	while (head != NULL && head->key != key)
		head = head->next[chain];
	return head;
}

bool layer_chain_contains(VisualLayerChain chain, 
	const VisualLayer *head, const VisualLayer *layer)
{
	while (head != NULL) {
		if (head == layer)
			return true;
		head = head->next[chain];
	}
	return false;
}

/* Inserts a chain of layers into another chain at the beginning of the 
 * equal range of keys matching 'key'. */
void layer_chain_insert(VisualLayerChain chain, VisualLayer **head, 
	VisualLayer *insert_head, LayerKey key)
{
	assertb(insert_head != NULL);
	assertb(!layer_chain_contains(chain, *head, insert_head));

	/* Find the end of the chain being inserted. */
	VisualLayer *insert_tail = insert_head;
	for (;; insert_tail = insert_tail->next[chain]) {
		insert_tail->key = key;
		insert_tail->flags |= (1 << chain);
		if (insert_tail->next[chain] == NULL)
			break;
	}

	/* Insert the chain. */
	VisualLayer *prev = layer_chain_lower_bound(chain, *head, key);
	if (prev != NULL) {
		insert_tail->next[chain] = prev->next[chain];
		prev->next[chain] = insert_head;
	} else {
		insert_tail->next[chain] = *head;
		*head = insert_head;
	}
}

/* Replaces all entries in a layer chain with 'key' with another layer chain,
 * returning the chain of elements replaced, or NULL if no elements matched 
 * 'key'. */
VisualLayer *layer_chain_replace(VisualLayerChain chain, 
	VisualLayer **head, LayerKey key, VisualLayer *insert_head)
{
	/* Find the insertion position. */
	VisualLayer *replace_prev = layer_chain_lower_bound(chain, *head, key);
	VisualLayer *replace_head = (replace_prev != NULL) ? 
		replace_prev->next[chain] : *head;

	/* replace_tail is the last entry matching 'key'. If there are no matching
	 * entries, it is equal to replace_prev. */
	VisualLayer *replace_tail = replace_prev;
	if (replace_head != NULL && replace_head->key == key) {
		VisualLayer *next = replace_head;
		do {
			next->flags &= ~(1 << chain);
			replace_tail = next;
			next = replace_tail->next[chain];
		} while (next != NULL && next->key == key);
	} else {
		replace_head = NULL;
	}

	/* Remove the chain from replace_prev->next to replace_tail. */
	if (replace_tail != replace_prev) {
		if (replace_prev != NULL) {
			replace_head = replace_prev->next[chain];
			replace_prev->next[chain] = replace_tail->next[chain];
		} else {
			replace_head = *head;
			*head = replace_tail->next[chain];
		}
		replace_tail->next[chain] = NULL;
	}

	if (insert_head != NULL) {
		/* Find the end of the insertion chain and set the key in each entry. */
		VisualLayer *insert_tail = insert_head;
		for (;; insert_tail = insert_tail->next[chain]) {
			insert_tail->key = key;
			insert_tail->flags |= (1 << chain);
			if (insert_tail->next[chain] == NULL)
				break;
		}

		/* Insert the chain from 'insert_head' to 'insert_tail' after 
		 * 'replace_prev'. */
		if (replace_prev != NULL) {
			insert_tail->next[chain] = replace_prev->next[chain];
			replace_prev->next[chain] = insert_head;
		} else {
			insert_tail->next[chain] = *head;
			*head = insert_head;
		}
	}

	return replace_head;
}

/* Removes a layer from a chain, returning true if it was present. */
bool layer_chain_remove(VisualLayerChain chain, VisualLayer **head, 
	VisualLayer *layer)
{
	if (*head == NULL)
		return false;
	if (layer == *head) {
		*head = layer->next[chain];
	} else {
		VisualLayer *prev = *head;
		while (prev->next[chain] != layer) {
			prev = prev->next[chain];
			if (prev == NULL)
				return false;
		}
		prev->next[chain] = layer->next[chain];
	}
	layer->next[chain] = NULL;
	layer->flags &= ~(1 << chain);
	return true;
}

/* Duplicates the links in chain A into chain B. */
VisualLayer *layer_chain_mirror(VisualLayer *head, VisualLayerChain a, 
	VisualLayerChain b)
{
	VisualLayer *layer = head;
	while (layer != NULL) {
		layer->next[b] = layer->next[a];
		layer->flags |= (1 << b);
		layer = layer->next[a];
	}
	return head;
}

/* Returns the number of distinct keys in a layer chain. */
unsigned layer_chain_count_keys(VisualLayerChain chain, const VisualLayer *head)
{
	unsigned count = 0;
	LayerKey last_key = LKEY_INVALID;
	while (head != NULL) {
		count += (head->key != last_key);
		last_key = (LayerKey)head->key;
		head = head->next[chain];
	}
	return count;
}

/* Callback for image layer notification handles. */
unsigned image_layer_notify_callback(UrlHandle handle, 
	UrlNotification type, UrlKey key, System *system, Node *node, 
	UrlFetchState fetch_state)
{
	handle; key; fetch_state; system;

	if (type == URL_NOTIFY_FETCH) {
		VisualLayer *background = layer_chain_find(VLCHAIN_NODE, 
			node->layers, LKEY_BACKGROUND);
		VisualLayer *content = layer_chain_find(VLCHAIN_NODE, 
			node->layers, LKEY_CONTENT);
		if (background != NULL && background->type == VLT_IMAGE)
			poll_network_image(node->document, node, background);
		if (content != NULL && content->type == VLT_IMAGE)
			poll_network_image(node->document, node, content);
	}
	return 0;
}

/* Flags a node for box rebuild if it is waiting for a network image that has
 * become available. */
void poll_network_image(Document *document, Node *node, 
	VisualLayer *layer)
{
	if ((layer->flags & VLFLAG_IMAGE_AVAILABLE) != 0)
		return;
	BackEnd *back_end = document->system->back_end;
	UrlCache *cache = document->system->url_cache;
	ImageLayer *il = &layer->image;
	if (platform_get_network_image_data(back_end, cache, il->image_handle) != NULL) {
		layer->flags |= VLFLAG_IMAGE_AVAILABLE;
		set_node_flags(document, node, NFLAG_REBUILD_BOXES, true);
	}
}

/* Clears the image URL associated with an image layer. Returns true if the
 * image changed. */
bool clear_image_layer_url(Document *document, VisualLayer *layer)
{
	BackEnd *back_end = document->system->back_end;
	UrlCache *cache = document->system->url_cache;
	if (cache == NULL)
		return false;
	ImageLayer *il = &layer->image;
	if (il->notify_handle != INVALID_URL_HANDLE) {
		cache->destroy_handle(il->notify_handle);
		platform_destroy_network_image(back_end, cache, il->image_handle);
		il->notify_handle = INVALID_URL_HANDLE;
		il->image_handle = INVALID_URL_HANDLE;
		return true;
	}
	return false;
}


/* Changes the image URL associated with an image layer. */
void set_image_layer_url(Document *document, Node *node, 
	VisualLayer *layer, const char *url)
{
	BackEnd *back_end = document->system->back_end;
	UrlCache *cache = document->system->url_cache;
	if (cache == NULL)
		return;
	ImageLayer *il = &layer->image;
	bool image_changed = false;
	if (url != NULL) {
		/* Replace the notify handle with one for the new URL. */
		UrlHandle notify_handle = cache->create_handle(url, -1,
			urlcache::URLP_NORMAL, urlcache::DEFAULT_TTL_SECS, node, 0,
			document->system->image_layer_notify_id, 
			urlcache::URL_FLAG_REUSE_DATA_HANDLE);
		image_changed = il->notify_handle != notify_handle;
		if (image_changed) {
			/* Replace the notify handle with the one for the new URL. */
			cache->destroy_handle(il->notify_handle);
			il->notify_handle = notify_handle;
			/* Recreate the network image handle. */
			UrlKey key = cache->key(notify_handle);
			platform_destroy_network_image(back_end, cache, il->image_handle);
			il->image_handle = platform_create_network_image(back_end, cache, key);
			/* We'll want to update boxes when the new image becomes 
			 * available. */
		}
	} else {
		image_changed = clear_image_layer_url(document, layer);
	}
	if (image_changed) {
		layer->flags &= ~VLFLAG_IMAGE_AVAILABLE;
		set_node_flags(document, node, NFLAG_REBUILD_BOXES, true);
		document->change_clock++;
		if (il->image_handle != INVALID_URL_HANDLE)
			poll_network_image(document, node, layer);
	}
}

/* Sets any undefined dimensions of 'box' to the natural size of the image
 * in 'layer'. */
void set_box_dimensions_from_layer(Document *document, Box *box, VisualLayer *layer)
{
	BackEnd *back_end = document->system->back_end;
	UrlCache *cache = document->system->url_cache;
	if (layer->type != VLT_IMAGE || cache == NULL)
		return;
	UrlHandle image_handle = layer->image.image_handle;
	unsigned image_dims[2];
	if (!platform_get_network_image_info(back_end, cache, image_handle, 
		&image_dims[0],  &image_dims[1]))
		return;
	for (unsigned axis = 0; axis < 2; ++axis) {
		if (box->axes[axis].mode_dim <= DMODE_AUTO) {
			set_ideal_size(document, box, (Axis)axis, DMODE_ABSOLUTE,
				(float)image_dims[axis]);
		}
	}
}

/* Sets undefined box dimensions to the natural size of the node's background
 * or content image. */
void set_box_dimensions_from_image(Document *document, Node *node, Box *box)
{
	VisualLayer *background = layer_chain_find(VLCHAIN_NODE, node->layers, LKEY_BACKGROUND);
	VisualLayer *image = layer_chain_find(VLCHAIN_NODE, node->layers, LKEY_CONTENT);
	if (background != NULL && background->type == VLT_IMAGE && node->first_child == NULL)
		set_box_dimensions_from_layer(document, box, background);
	if (image != NULL && image->type == VLT_IMAGE)
		set_box_dimensions_from_layer(document, box, image);
}


} // namespace stkr
