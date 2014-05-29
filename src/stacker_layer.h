#pragma once

#include <cstdint>

#include "stacker_attribute.h"
#include "stacker_style.h"
#include "url_cache.h"

namespace stkr {

struct System;
struct Document;
struct Node;
struct Box;

enum VisualLayerType {
	VLT_NONE,
	VLT_PANE,
	VLT_IMAGE,
	VLT_TEXT
};

/* The two linked lists we use to organize layers. */
enum VisualLayerChain {
	VLCHAIN_NODE,
	VLCHAIN_BOX
};

enum VisualLayerFlag {
	VLFLAG_IN_NODE_CHAIN   = 1 << VLCHAIN_NODE, // Layer is in a node's layer stack.
	VLFLAG_IN_BOX_CHAIN    = 1 << VLCHAIN_BOX,  // Layer is in a box's layer stack.
	VLFLAG_IMAGE_AVAILABLE = 1 << 2             // Network image data is available.
};

/* Sort keys used to organize box and node layer stacks. */
enum LayerKey {
	LKEY_INVALID = -1,
	LKEY_BACKGROUND,
	LKEY_SELECTION,
	LKEY_CONTENT,
	LKEY_TEXT
};

/* How to position and scale a layer with respect to a box. */
struct LayerPosition {
	unsigned char placement;
	unsigned char positioning_mode;
	unsigned char alignment[2];
	unsigned char mode_offset[2];
	unsigned char mode_size[2];
	float offsets[2];
	float dims[2];
};

/* A pane is a filled box with a border. */
struct PaneLayer {
	LayerPosition position;
	PaneType pane_type;
	uint32_t fill_color;
	uint32_t border_color;
	float border_width;
};

/* Draws a scaled, tinted image. */
struct ImageLayer {
	LayerPosition position;
	urlcache::UrlHandle notify_handle;
	urlcache::UrlHandle image_handle;
	uint32_t tint;
};

/* TextLayer character flags. */
const unsigned TLF_LINE_HEAD        = 1 << 15;
const unsigned TLF_TOKEN_HEAD       = 1 << 14;
const unsigned TLF_SEGMENT_HEAD     = 1 << 13;
const unsigned TLF_STYLE_HEAD       = 1 << 12;
const unsigned TLF_COLOR_INDEX_MASK = (1 << 12) - 1;

const unsigned MAX_TEXT_LAYER_COLORS = 64;

/* A text layer is a list of glyph indices and corresponding (x, y) positions. */
struct TextLayer {
	uint32_t key;
	int16_t font_id;
	uint16_t flags;
	unsigned length;
	unsigned num_colors; /*
	char text[length];
	uint16_t flags[length];
	uint32_t palette[num_colors];
	struct { int x, y; } positions[length]; */
};

const unsigned TEXT_LAYER_BYTES_PER_CHAR = (sizeof(char) + sizeof(uint16_t) + 2 * sizeof(int));

/* Each box has a stack of layers which define its visual representation. */
struct VisualLayer {
	VisualLayerType type :  4;
	LayerKey key         :  4;
	int depth_offset     :  8;
	unsigned flags       : 12;
	VisualLayer *next[2];
	union {
		PaneLayer pane;
		ImageLayer image;
		TextLayer text;
	};
};

const char *get_text_layer_text(const VisualLayer *layer);
const uint16_t *get_text_layer_flags(const VisualLayer *layer);
const uint32_t *get_text_layer_palette(const VisualLayer *layer);
const int *get_text_layer_positions(const VisualLayer *layer);
VisualLayer *create_layer(Document *document, const Node *node, 
	VisualLayerType type, unsigned extra = 0);
void destroy_layer(Document *document, VisualLayer *layer);
void release_layer(Document *document, VisualLayer *layer);
void release_layer_chain(Document *document, VisualLayerChain chain, VisualLayer *head);
VisualLayer *layer_chain_lower_bound(VisualLayerChain chain, VisualLayer *head, LayerKey key);
void layer_chain_insert(VisualLayerChain chain, VisualLayer **head, VisualLayer *layer, LayerKey key);
VisualLayer *layer_chain_find(VisualLayerChain chain, VisualLayer *head, LayerKey key);
VisualLayer *layer_chain_replace(VisualLayerChain chain, VisualLayer **head, 
	LayerKey key, VisualLayer *insert_head = 0);
bool layer_chain_remove(VisualLayerChain chain, VisualLayer **head, VisualLayer *layer);
VisualLayer *layer_chain_mirror(VisualLayer *head, VisualLayerChain a, VisualLayerChain b);
unsigned layer_chain_count_keys(VisualLayerChain chain, const VisualLayer *head);

void poll_network_image(Document *document, Node *node, 
	VisualLayer *layer);
bool clear_image_layer_url(Document *document, VisualLayer *layer);
void set_image_layer_url(Document *document, Node *node, 
	VisualLayer *layer, const char *url);
void set_box_dimensions_from_layer(Document *document, Box *box, VisualLayer *layer);
void set_box_dimensions_from_image(Document *document, Node *node, Box *box);
unsigned image_layer_notify_callback(urlcache::UrlHandle handle, 
	urlcache::UrlNotification type, urlcache::UrlKey key, 
	System *system, Node *node, urlcache::UrlFetchState fetch_state);

void initialize_layer_position(LayerPosition *lp);
void compute_layer_position(const Box *box, const LayerPosition *lp, 
	float *r, float natural_width = 0.0f, float natural_height = 0.0f, 
	bool use_natural = false);

} // namespace stkr

