#include "stacker_util.h"
#include "stacker_attribute.h"

#include <cstring>
#include <climits>

namespace stkr {

extern const float INFINITE_RECTANGLE[4] = 
	{ -FLT_MAX, FLT_MAX, -FLT_MAX, FLT_MAX };

/* Aligns an interval with respect to another interval. */
void align_1d(Alignment alignment, float dim, float offset,
	float a0, float a1, float *b0, float *b1)
{
	if (alignment <= ALIGN_START) {
		*b0 = a0 + offset;
		*b1 = *b0 + dim;
	} else if (alignment == ALIGN_END) {
		*b1 = a1 + offset;
		*b0 = *b1 - dim;
	} else if (alignment == ALIGN_MIDDLE) {
		float mid = 0.5f * (a1 + a0);
		*b0 = mid - 0.5f * dim;
		*b1 = *b0 + dim;
	}
}

/* Construct a rectangle aligned with respect to another rectangle at an 
 * offset. */
void align_rectangle(Alignment align_h, Alignment align_v,
	float width, float height, float offset_x, float offset_y,
	const float *bounds, float *result)
{
	align_1d(align_h, width, offset_x, rleft(bounds), rright(bounds), 
		sidep(result, AXIS_H, 0), sidep(result, AXIS_H, 1));
	align_1d(align_v, height, offset_y, rtop(bounds), rbottom(bounds), 
		sidep(result, AXIS_V, 0), sidep(result, AXIS_V, 1));
}

/* Calculate the absolute value of a dimension specified relative to a 
 * container box. */
float relative_dimension(DimensionMode mode, float specified, 
	float container, float value_if_undefined)
{
	if (mode <= DMODE_AUTO)
		return value_if_undefined;
	if (mode == DMODE_ABSOLUTE)
		return specified;
	if (mode == DMODE_FRACTIONAL)
		container *= specified;
	return container;
}

/* The (unsigned) distance from X to whichever of A or B is nearerer, or
 * zero if X lies between A and B. */
float band_distance(float x, float a, float b)
{
	if (x < a) return a - x;
	if (x > b) return x - b;
	return 0.0f;
}

/* Calculates the distance metric used to score selection anchor candidate
 * rectangles. */
float rectangle_selection_distance(float x, float y, float bx0, float bx1, 
	float by0, float by1)
{
	float d = 0.0f;
	if (x < bx0)
		d += bx0 - x;
	else if (x > bx1)
		d += x - bx1;
	if (y < by0)
		d += 1e5f * (by0 - y);
	else if (y > by1) 
		d += 1e5f * (y - by1);
	return d;
}


/* MurmurHash3 by Austin Appleby. */
unsigned murmur3_32(const void *key, int len, unsigned seed)
{
	const int nblocks = len / 4;
	unsigned h1 = seed, c1 = 0xcc9e2d51, c2 = 0x1b873593;
	const unsigned *blocks = (const unsigned *)key + nblocks;
	for (int i = -nblocks; i; i++) {
		unsigned k1 = blocks[i];
		k1 *= c1; k1 = (k1 << 15) | (k1 >> (32 - 15)); k1 *= c2;
		h1 ^= k1; h1 = (h1 << 15) | (h1 >> (32 - 15)); h1 = h1 * 5 + 0xe6546b64;
	}
	const unsigned char *tail = (const unsigned char*)key + nblocks * 4;
	unsigned k1 = 0;
	switch (len & 3) {
		case 3: k1 ^= tail[2] << 16;
		case 2: k1 ^= tail[1] << 8;
		case 1: k1 ^= tail[0];
			k1 *= c1; k1 = (k1 << 15) | (k1 >> (32 - 15)); k1 *= c2; h1 ^= k1;
	}
	h1 ^= len;
	h1 ^= h1 >> 16; 
	h1 *= 0x85ebca6b; 
	h1 ^= h1 >> 13; 
	h1 *= 0xc2b2ae35; 
	h1 ^= h1 >> 16;
	return h1;
}

/* MurmurHash3 by Austin Appleby. */
uint64_t murmur3_64(const void *key, const int len, unsigned seed)
{
	const int nblocks = len / 16;
	unsigned h1 = seed, h2 = seed, h3 = seed, h4 = seed;
	unsigned c1 = 0x239b961b; 
	unsigned c2 = 0xab0e9789;
	unsigned c3 = 0x38b34ae5; 
	unsigned c4 = 0xa1e38b93;

#define rotl32(x, r) ((x << r) | (x >> (32 - r)))

	const unsigned *blocks = (const unsigned *)key + nblocks * 4;
	for (int i = -nblocks; i; i++) {
		unsigned k1 = blocks[i * 4 + 0];
		unsigned k2 = blocks[i * 4 + 1];
		unsigned k3 = blocks[i * 4 + 2];
		unsigned k4 = blocks[i * 4 + 3];
		k1 *= c1; k1  = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
		h1 = rotl32(h1,19); h1 += h2; h1 = h1 * 5 + 0x561ccd1b;
		k2 *= c2; k2  = rotl32(k2, 16); k2 *= c3; h2 ^= k2;
		h2 = rotl32(h2,17); h2 += h3; h2 = h2 * 5 + 0x0bcaa747;
		k3 *= c3; k3  = rotl32(k3, 17); k3 *= c4; h3 ^= k3;
		h3 = rotl32(h3,15); h3 += h4; h3 = h3 * 5 + 0x96cd1c35;
		k4 *= c4; k4  = rotl32(k4, 18); k4 *= c1; h4 ^= k4;
		h4 = rotl32(h4,13); h4 += h1; h4 = h4 * 5 + 0x32ac3b17;
	}

	const unsigned char *tail = (const unsigned char *)key + nblocks * 16;
	unsigned k1 = 0, k2 = 0, k3 = 0, k4 = 0;
	switch (len & 15) {
	case 15: k4 ^= tail[14] << 16;
	case 14: k4 ^= tail[13] << 8;
	case 13: k4 ^= tail[12] << 0; 
		k4 *= c4; k4  = rotl32(k4,18); k4 *= c1; h4 ^= k4;
	case 12: k3 ^= tail[11] << 24;
	case 11: k3 ^= tail[10] << 16;
	case 10: k3 ^= tail[ 9] << 8;
	case  9: k3 ^= tail[ 8] << 0; 
		k3 *= c3; k3  = rotl32(k3, 17); k3 *= c4; h3 ^= k3;
	case  8: k2 ^= tail[ 7] << 24;
	case  7: k2 ^= tail[ 6] << 16;
	case  6: k2 ^= tail[ 5] << 8;
	case  5: k2 ^= tail[ 4] << 0;
		k2 *= c2; k2  = rotl32(k2,16); k2 *= c3; h2 ^= k2;
	case  4: k1 ^= tail[ 3] << 24;
	case  3: k1 ^= tail[ 2] << 16;
	case  2: k1 ^= tail[ 1] << 8;
	case  1: k1 ^= tail[ 0] << 0;
		k1 *= c1; k1  = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
	}

#undef rotl32

	h1 ^= len; h2 ^= len; h3 ^= len; h4 ^= len;
	h1 += h2; h1 += h3; h1 += h4;
	h2 += h1; h3 += h1; h4 += h1;

#define fmix(h) h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; \
	h *= 0xc2b2ae35; h ^= h >> 16;

	fmix(h1);
	fmix(h2);
	fmix(h3);
	fmix(h4);

#undef fmix

	h1 += h2; h1 += h3; h1 += h4;
	h2 += h1;

	return ((uint64_t)h1 << 32) | h2;
}

uint64_t murmur3_64_cstr(const char *key, unsigned seed)
{
	return murmur3_64(key, strlen(key), seed);
}

const char *random_word(uintptr_t seed)
{
	static const char * const DICTIONARY[] = {
		"angle",      "ant",       "apple",     "arch",      "arm",
		"army",		  "baby",	   "bag",		"ball",		 "band",
		"basin",	  "basket",	   "bath",		"bed",		 "bee",
		"bell",		  "berry",	   "bird",		"blade",	 "board",
		"boat",		  "bone",	   "book",		"boot",		 "bottle",
		"box",		  "boy",	   "brain",		"brake",	 "branch",
		"brick",	  "bridge",	   "brush",		"bucket",	 "bulb",
		"button",	  "cake",	   "camera",	"card",		 "cart",
		"carriage",	  "cat",	   "chain",		"cheese",	 "chest",
		"chin",		  "church",	   "circle",	"clock",	 "cloud",
		"coat",		  "collar",	   "comb",		"cord",		 "cow",
		"cup",		  "curtain",   "cushion",	"dog",		 "door",
		"drain",	  "drawer",	   "dress",		"drop",		 "ear",
		"egg",		  "engine",	   "eye",		"face",		 "farm",
		"feather",	  "finger",	   "fish",		"flag",		 "floor",
		"fly",		  "foot",	   "fork",		"fowl",		 "frame",
		"garden",	  "girl",	   "glove",		"goat",		 "gun",
		"hair",		  "hammer",	   "hand",		"hat",		 "head",
		"heart",	  "hook",	   "horn",		"horse",	 "hospital",
		"house",	  "island",	   "jewel",		"kettle",	 "key",
		"knee",		  "knife",	   "knot",		"leaf",		 "leg",
		"library",	  "line",	   "lip",		"lock",		 "map",
		"match",	  "monkey",	   "moon",		"mouth",	 "muscle",
		"nail",		  "neck",	   "needle",	"nerve",	 "net",
		"nose",		  "nut",	   "office",	"orange",	 "oven",
		"parcel",	  "pen",	   "pencil",	"picture",	 "pig",
		"pin",		  "pipe",	   "plane",		"plate",	 "plow",
		"pocket",	  "pot",	   "potato",	"prison",	 "pump",
		"rail",		  "rat",	   "receipt",	"ring",		 "rod",
		"roof",		  "root",	   "sail",		"school",	 "scissors",
		"screw",	  "seed",	   "sheep",		"shelf",	 "ship",
		"shirt",	  "shoe",	   "skin",		"skirt",	 "snake",
		"sock",		  "spade",	   "sponge",	"spoon",	 "spring",
		"square",	  "stamp",	   "star",		"station",	 "stem",
		"stick",	  "stocking",  "stomach",	"store",	 "street",
		"sun",		  "table",	   "tail",		"thread",	 "throat",
		"thumb",	  "ticket",	   "toe",		"tongue",	 "tooth",
		"town",		  "train",	   "tray",		"tree",		 "trousers",
		"umbrella",	  "wall",	   "watch",		"wheel",	 "whip",
		"whistle",	  "window",	   "wing",		"wire",		 "worm"
	};
	static const unsigned NUM_WORDS = sizeof(DICTIONARY) / sizeof(DICTIONARY[0]);
	return DICTIONARY[murmur3_32(&seed, sizeof(seed)) % NUM_WORDS];
}


/*
 * Doubly-linked List
 */

struct Link { void *prev, *next; };
#define item_prev(item, offset) ((Link *)(((char *)(item) + offset)))->prev
#define item_next(item, offset) ((Link *)(((char *)(item) + offset)))->next

void list_insert_before(void **head, void **tail, void *item, void *next, 
	unsigned offset)
{
	void *prev;
	if (next != NULL) {
		prev = item_prev(item, offset);
		item_prev(next, offset) = item;
	} else {
		prev = *tail;
		*tail = item;
	}
	if (prev != NULL)
		item_next(prev, offset) = item;
	else
		*head = item;
	item_prev(item, offset) = prev;
	item_next(item, offset) = next;
}

void list_remove(void **head, void **tail, void *item, unsigned offset)
{
	void *prev = item_prev(item, offset);
	void *next = item_next(item, offset);
	if (prev != NULL)
		item_next(prev, offset) = next;
	else
		*head = next;
	if (next != NULL)
		item_prev(next, offset) = prev;
	else
		*tail = prev;
	item_prev(item, offset) = NULL;
	item_next(item, offset) = NULL;
}

/*
 * Tree Utilities
 */

#define item_parent(item, offset) *(void **)((char *)(item) + (offset))

/* Determines the first tree ancestor common to A and B. The result is NULL
 * if the nodes are not part of the same tree. */
const void *lowest_common_ancestor(const void *a, const void *b,
	const void **below_a, const void **below_b, unsigned parent_offset)
{
	static const unsigned MAX_TREE_DEPTH = 64;

	const void *pa[MAX_TREE_DEPTH], *pb[MAX_TREE_DEPTH];
	unsigned da = 0, db = 0;
	while (a != NULL) {
		ensure(da != MAX_TREE_DEPTH);
		pa[da++] = a;
		a = item_parent(a, parent_offset);
	}
	while (b != NULL) {
		ensure(db != MAX_TREE_DEPTH);
		pb[db++] = b;
		b = item_parent(b, parent_offset);
	}
	const void *ancestor = NULL;
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

} // namespace stkr
