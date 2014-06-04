#pragma once

#include "stacker_attribute.h"
#include "stacker_util.h"

namespace stkr {

/*
 * Attribute Buffers
 */

#pragma pack(push, 1)

struct Attribute {
	uint8_t name;
	uint8_t type : 3;
	uint8_t mode : 5;
	uint32_t folded: 1;
	uint32_t op : 3;
	uint32_t size : 28;
};

#pragma pack(pop)

struct AttributeBuffer {
	char *buffer;
	int size;
	int capacity; 
	unsigned num_attributes;
};

extern const char * const STORAGE_STRINGS[NUM_ATTRIBUTE_TYPES];

int token_to_attribute_operator(int name);
AttributeSemantic attribute_semantic(int name);
ValueSemantic value_semantic(int type_token);
bool is_inheritable(int name);
bool is_auto_mode(int name, int mode);

int parse_string_list(const char *s, int length, char *buffer, 
	unsigned buffer_size);

void abuf_init(AttributeBuffer *abuf, void *storage = 0, 
	unsigned storage_size = 0);
void abuf_clear(AttributeBuffer *abuf);
Attribute *abuf_to_stand_alone(AttributeBuffer *abuf);
void abuf_free_stand_alone(Attribute *attribute);
const Attribute *abuf_first(const AttributeBuffer *abuf);
const Attribute *abuf_next(const AttributeBuffer *abuf, 
	const Attribute *attribute);
Attribute *abuf_append(AttributeBuffer *abuf, const Attribute *attribute);
Attribute *abuf_prepend(AttributeBuffer *abuf, const Attribute *attribute);
Attribute *abuf_append_integer(AttributeBuffer *abuf, int name, 
	ValueSemantic vs, int value, AttributeOperator op = AOP_SET);
Attribute *abuf_append_float(AttributeBuffer *abuf, int name, 
	ValueSemantic vs, float value, AttributeOperator op = AOP_SET);
Attribute *abuf_append_string(AttributeBuffer *abuf, int name, 
	ValueSemantic vs, const char *value, int length, 
	AttributeOperator op = AOP_SET);
Attribute *abuf_replace(AttributeBuffer *abuf, Attribute *a, const Attribute *b);
void abuf_replace_range(AttributeBuffer *abuf, const Attribute *start, 
	const Attribute *end, const AttributeBuffer *source = 0);
int abuf_fold(AttributeBuffer *abuf, Attribute *a, const Attribute *b, 
	Attribute **out_folded = 0);
int abuf_set_integer(AttributeBuffer *abuf, int name, ValueSemantic vs, 
	int value, AttributeOperator op = AOP_SET, bool fold = false);
int abuf_set_float(AttributeBuffer *abuf, int name, ValueSemantic vs, 
	float value, AttributeOperator op = AOP_SET, bool fold = false);
int abuf_set_string(AttributeBuffer *abuf, int name, ValueSemantic vs,
	const char *value, int length = -1, AttributeOperator op = AOP_SET, 
	bool fold = false);
int abuf_set(AttributeBuffer *abuf, Token name, const Variant *value, 
	AttributeOperator op = AOP_SET, bool fold = false);
int abuf_read_mode(const Attribute *attribute, int defmode = ADEF_UNDEFINED);
int abuf_read_integer(const Attribute *attribute, int32_t *result, 
	int32_t defval = 0);
int abuf_evaluate_integer(const Attribute *attribute, int32_t *result, 
	int32_t lhs, int32_t defval = 0);
int abuf_read_float(const Attribute *attribute, float *result, 
	float defval = 0.0f);
int abuf_evaluate_float(const Attribute *attribute, float *result, 
	float lhs, float defval = 0.0f);
int abuf_read_string(const Attribute *attribute, const char **result, 
	uint32_t *length = 0, const char *defval = 0);
int abuf_read_string(const Attribute *attribute, char *buffer, 
	uint32_t buffer_size, uint32_t *out_length, const char *defval = 0, 
	StringSetRepresentation ssr = SSR_INTERNAL);
int attribute_value_string(char *buffer, unsigned buffer_size, 
	const Attribute *attribute);

/*
 * Attribute Masks
 */

const unsigned ATTRIBUTE_MASK_WORDS = (NUM_ATTRIBUTE_TOKENS + 31) / 32;

inline bool amask_test(const uint32_t *mask, int name)
{
	unsigned index = name - TOKEN_ATTRIBUTE_FIRST;
	assertb(index < NUM_ATTRIBUTE_TOKENS);
	return (mask[index >> 5] >> (index & 0x1F)) & 1;
}

inline void amask_or(uint32_t *mask, int name, bool value = true)
{
	unsigned index = name - TOKEN_ATTRIBUTE_FIRST;
	assertb(index < NUM_ATTRIBUTE_TOKENS);
	mask[index >> 5] |= (unsigned(value) << (index & 0x1F));
}

inline bool amask_is_subset(const uint32_t *a, const uint32_t *b)
{
	uint32_t diff = 0;
	for (unsigned i = 0; i < ATTRIBUTE_MASK_WORDS; ++i)
		diff |= b[i] & ~a[i];
	return (diff == 0);
}

inline void amask_union(uint32_t *a, const uint32_t *b)
{
	for (unsigned i = 0; i < ATTRIBUTE_MASK_WORDS; ++i)
		a[i] |= b[i];
}

} // namespace stkr

