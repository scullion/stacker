#include "stacker_attribute.h"

#include <cstddef>
#include <cstring>
#include <cfloat>

#include <algorithm>

#include "stacker_shared.h"
#include "stacker_util.h"
#include "stacker_attribute_buffer.h"

namespace stkr {

#pragma pack(push, 1)

/* Helper used to read attribute data located after the header in memory. */
union AttributeData {
	int16_t int16;
	int32_t int32;
	float float32;
	char string[1];
};

struct BufferEntry {
	Attribute header;
	AttributeData data;
};

#pragma pack(pop)

/* Constants used to form operator masks, used in validation. */
enum AttributeOperatorBit {
	AOP_BIT_SET            = 1 << AOP_SET,
	AOP_BIT_OVERRIDE       = 1 << AOP_OVERRIDE,
	AOP_BIT_ADD            = 1 << AOP_ADD,
	AOP_BIT_SUBTRACT       = 1 << AOP_SUBTRACT,
	AOP_BIT_MULTIPLY       = 1 << AOP_MULTIPLY,
	AOP_BIT_DIVIDE         = 1 << AOP_DIVIDE,

	AOP_BIT_ASSIGNMENT     = AOP_BIT_SET | AOP_BIT_OVERRIDE,
	AOP_BIT_ADDITIVE       = AOP_BIT_ADD | AOP_BIT_SUBTRACT,
	AOP_BIT_MULTIPLICATIVE = AOP_BIT_MULTIPLY | AOP_BIT_DIVIDE,
	AOP_BIT_ARITHMETIC     = AOP_BIT_ADDITIVE | AOP_BIT_MULTIPLICATIVE
};

extern const char * const STORAGE_STRINGS[NUM_ATTRIBUTE_TYPES] = 
	{ "none", "int16", "int32", "float32", "string" };

/* Returns the attribute operator corresponding to a token, or -1 if the token
 * is not an operator token. */
int token_to_attribute_operator(int name)
{
	switch (name) {
		case TOKEN_EQUALS:
			return AOP_SET;
		case TOKEN_COLON_EQUALS:
			return AOP_OVERRIDE;
		case TOKEN_PLUS_EQUALS:
			return AOP_ADD;
		case TOKEN_DASH_EQUALS:
			return AOP_SUBTRACT;
		case TOKEN_STAR_EQUALS:
			return AOP_MULTIPLY;
		case TOKEN_SLASH_EQUALS:
			return AOP_DIVIDE;
	}
	return -1;
}

/* Returns the storage type and mode set of an attribute given its name. */
AttributeSemantic attribute_semantic(int name)
{
	switch (name) {
		case TOKEN_WIDTH:
		case TOKEN_HEIGHT:
		case TOKEN_MIN_WIDTH:
		case TOKEN_MIN_HEIGHT:
		case TOKEN_MAX_WIDTH:
		case TOKEN_MAX_HEIGHT:
		case TOKEN_BACKGROUND_WIDTH:
		case TOKEN_BACKGROUND_HEIGHT:
		case TOKEN_BACKGROUND_OFFSET_X:
		case TOKEN_BACKGROUND_OFFSET_Y:
			return ASEM_DIMENSON;
		case TOKEN_PADDING:
		case TOKEN_PADDING_LEFT:
		case TOKEN_PADDING_RIGHT:
		case TOKEN_PADDING_TOP:
		case TOKEN_PADDING_BOTTOM:
		case TOKEN_MARGIN:
		case TOKEN_MARGIN_LEFT:
		case TOKEN_MARGIN_RIGHT:
		case TOKEN_MARGIN_TOP:
		case TOKEN_MARGIN_BOTTOM:
		case TOKEN_LEADING:
		case TOKEN_INDENT:
			return ASEM_ABSOLUTE_DIMENSION;
		case TOKEN_URL:
			return ASEM_URL;
		case TOKEN_ARRANGE:
		case TOKEN_ALIGN:
		case TOKEN_BACKGROUND_HORIZONTAL_ALIGNMENT:
		case TOKEN_BACKGROUND_VERTICAL_ALIGNMENT:
			return ASEM_ALIGNMENT;
		case TOKEN_JUSTIFY:
			return ASEM_JUSTIFICATION;
		case TOKEN_FONT:
		case TOKEN_MATCH:
			return ASEM_STRING;
		case TOKEN_CLASS:
			return ASEM_STRING_SET;
		case TOKEN_FONT_SIZE:
		case TOKEN_BORDER_WIDTH:
			return ASEM_ABSOLUTE_DIMENSION;
		case TOKEN_COLOR:
		case TOKEN_BACKGROUND_COLOR:
		case TOKEN_BORDER_COLOR:
		case TOKEN_SELECTION_COLOR:
		case TOKEN_SELECTION_FILL_COLOR:
		case TOKEN_TINT:
			return ASEM_COLOR;
		case TOKEN_GLOBAL:
		case TOKEN_BOLD:
		case TOKEN_ITALIC:
		case TOKEN_UNDERLINE:
		case TOKEN_ENABLED:
		case TOKEN_CLIP_LEFT:
		case TOKEN_CLIP_RIGHT:
		case TOKEN_CLIP_TOP:
		case TOKEN_CLIP_BOTTOM:
			return ASEM_FLAG;
		case TOKEN_BACKGROUND:
			return ASEM_BACKGROUND;
		case TOKEN_LAYOUT:
			return ASEM_LAYOUT;
		case TOKEN_CLIP:
			return ASEM_EDGES;
		case TOKEN_WHITE_SPACE:
			return ASEM_WHITE_SPACE;
		case TOKEN_WRAP:
			return ASEM_WRAP_MODE;
		case TOKEN_BACKGROUND_SIZE:
			return ASEM_BACKGROUND_SIZE;
		case TOKEN_BACKGROUND_PLACEMENT:
		case TOKEN_CLIP_BOX:
			return ASEM_BOUNDING_BOX;
		case TOKEN_CURSOR:
			return ASEM_CURSOR;
	}
	return ASEM_INVALID;
}

/* Returns a mask of the storage types permitted for a (semantic, mode) 
 * combination. */
static unsigned storage_mask(AttributeSemantic semantic, int mode)
{
	switch (semantic) {
		case ASEM_DIMENSON:
		case ASEM_ABSOLUTE_DIMENSION:
			return STORAGE_BIT_NUMERIC;
		case ASEM_REAL:
			return STORAGE_BIT_FLOAT32;
		case ASEM_STRING:
		case ASEM_STRING_SET:
		case ASEM_URL:
			return mode != ADEF_UNDEFINED ? STORAGE_BIT_STRING : 
				STORAGE_BIT_NONE;
		case ASEM_BACKGROUND:
			if (mode == BGMODE_URL)
				return STORAGE_BIT_STRING;
			if (mode == BGMODE_COLOR)
				return STORAGE_BIT_INT32;
			return STORAGE_BIT_NONE;
		case ASEM_COLOR:
			return STORAGE_BIT_INT32;
		case ASEM_FLAG:
		case ASEM_ALIGNMENT:
		case ASEM_JUSTIFICATION:
		case ASEM_LAYOUT:
		case ASEM_WHITE_SPACE:
		case ASEM_WRAP_MODE:
		case ASEM_BACKGROUND_SIZE:
		case ASEM_BOUNDING_BOX:
		case ASEM_CURSOR:
		case ASEM_EDGES:
			return STORAGE_BIT_NONE;
	}
	ensure(false);
	return STORAGE_NONE;
}

/* Given an attribute value token, returns a number specifying any special 
 * interpretation that should be used during the value's validation and 
 * conversion. For example, a number might be a percentage, or a string might be 
 * a URL. */
ValueSemantic value_semantic(int type_token)
{
	switch (type_token) {
		case TOKEN_INTEGER:
		case TOKEN_STRING: 
		case TOKEN_FLOAT:
			return VSEM_NONE;
		case TOKEN_BOOLEAN:
			return VSEM_BOOLEAN;
		case TOKEN_PERCENTAGE:
			return VSEM_PERCENTAGE;
		case TOKEN_COLOR_LITERAL:
			return VSEM_COLOR;
		case TOKEN_URL_LITERAL:
			return VSEM_URL;
		default:
			if (is_enum_token(type_token))
				return VSEM_TOKEN;
			break;
	}
	return VSEM_INVALID;
}

/* Returns a mask of operators that can be applied to a particular kind of
 * attribute. */
static unsigned supported_operators(AttributeSemantic semantic)
{
	switch (semantic) {
		case ASEM_DIMENSON:
		case ASEM_ABSOLUTE_DIMENSION:
		case ASEM_REAL:
			return AOP_BIT_ASSIGNMENT | AOP_BIT_ARITHMETIC;
		case ASEM_STRING:
		case ASEM_URL:
			return AOP_BIT_ASSIGNMENT | AOP_BIT_ADD;
		case ASEM_EDGES:
		case ASEM_STRING_SET:
			return AOP_BIT_ASSIGNMENT | AOP_BIT_ADDITIVE;
		case ASEM_COLOR:
			return AOP_BIT_ASSIGNMENT | AOP_BIT_MULTIPLY;
		default:
			break;
	}
	return AOP_BIT_ASSIGNMENT;
}

const BufferEntry ATTR_ZERO             = { TOKEN_INVALID, STORAGE_INT32, ADEF_DEFINED, false, AOP_SET, 0 };
const BufferEntry ATTR_EMPTY_STRING_SET = { TOKEN_INVALID, STORAGE_STRING, ADEF_DEFINED, false, AOP_SET, 1, '\0' };
const BufferEntry ATTR_EMPTY_EDGE_SET   = { TOKEN_INVALID, STORAGE_NONE, EDGE_FLAG_NONE, false, AOP_SET, 0 };

/* Returns a L.H.S. attribute to use when no SET is present in the expression
 * for a particular attribute. This is defined for attributes with set semantics
 * and numbers that have a natural zero value. For example, if the user puts
 * 'class += "abc"' on a node but there are no other assignments to "class", 
 * it's reasonable to compute the class as {"abc"} instead of considering its
 * value to be undefined. */
 const Attribute *attribute_default_value(int name)
{
	AttributeSemantic semantic = attribute_semantic(name);
	switch (semantic) {
		case ASEM_DIMENSON:
		case ASEM_ABSOLUTE_DIMENSION:
			switch (name) {
				case TOKEN_PADDING:
				case TOKEN_PADDING_LEFT:
				case TOKEN_PADDING_RIGHT:
				case TOKEN_PADDING_TOP:
				case TOKEN_PADDING_BOTTOM:
				case TOKEN_MARGIN:
				case TOKEN_MARGIN_LEFT:
				case TOKEN_MARGIN_RIGHT:
				case TOKEN_MARGIN_TOP:
				case TOKEN_MARGIN_BOTTOM:
					return &ATTR_ZERO.header;
			}
			break;
		case ASEM_EDGES:
			return &ATTR_EMPTY_EDGE_SET.header;
		case ASEM_STRING_SET:
			return &ATTR_EMPTY_STRING_SET.header;
	}
	return NULL;
}

AttributeAssignment make_assignment(Token name, int value, 
	ValueSemantic vs, AttributeOperator op)
{
	AttributeAssignment assignment;
	assignment.name = name;
	assignment.op = op;
	variant_set_integer(&assignment.value, value, vs);
	return assignment;
}

AttributeAssignment make_assignment(Token name, unsigned value, 
	ValueSemantic vs, AttributeOperator op)
{
	return make_assignment(name, (int)value, vs, op);
}

AttributeAssignment make_assignment(Token name, float value, 
	ValueSemantic vs, AttributeOperator op)
{
	AttributeAssignment assignment;
	assignment.name = name;
	assignment.op = op;
	variant_set_float(&assignment.value, value, vs);
	return assignment;
}

AttributeAssignment make_assignment(Token name, const char *value,
	ValueSemantic vs, AttributeOperator op)
{
	AttributeAssignment assignment;
	assignment.name = name;
	assignment.op = op;
	variant_set_string(&assignment.value, value, vs);
	return assignment;
}

/* Parses a string of space- or comma-delimited tokens into the attribute
 * storage form for string sets: zero or more null-terminated strings 
 * concatenated end to end, terminated by an extra null at the end. Returns 
 * a negative error code if the string has inconsistent delimiters. */
int parse_string_list(const char *s, int length, char *buffer, 
	unsigned buffer_size)
{
	int result_length = 0, elements = 0;
	char delimiter = 0;
	for (int i = 0; i != length; ) {
		while (i != length && isspace(s[i]))
			i++;
		if (i == length)
			break;
		if (elements != 0 && ((delimiter != 0) || s[i] == ',')) {
			if ((delimiter != 0) != (s[i] == ',')) {
				if (elements > 1)
					return -1;
				delimiter = s[i];
			}
			do { ++i; } while (i != length && isspace(s[i]));
		}
		int start = i;
		while (i != length && !isspace(s[i]) && s[i] != ',')
			++i;
		if (i != start) {
			unsigned token_length = i - start;
			if (buffer != NULL && result_length + token_length + 1 < buffer_size) {
				memcpy(buffer + result_length, s + start, token_length);
				buffer[result_length + token_length] = '\0';
			}
			result_length += token_length + 1;
		}
		elements++;
	}
	if (buffer != NULL) {
		if ((unsigned)result_length >= buffer_size)
			buffer[buffer_size - 1] = '\0';
		else
			buffer[result_length] = '\0';
	}
	return result_length;
}

static bool string_set_contains(const char *s, unsigned length, const char *p)
{
	const char *end = s + length;
	while (s != end) {
		if (0 == strcmp(p, s))
			return true;
		s += 1 + strlen(s);
	}
	return false;
}

/* Eliminates duplicates from a string set, returning the size delta. */
static int string_set_unique(char *s, unsigned length)
{
	char *d = s;
	while (*s != '\0') {
		unsigned item_length = 1 + strlen(s);
		length -= item_length;
		if (!string_set_contains(s + item_length, length, s)) {
			memmove(d, s, item_length);
			d += item_length;
		}
		s += item_length;
	}
	*d = '\0';
	return d - s;
}

/* Deletes entries in A that are part of B. A and B are null-terminated lists
 * of null-terminated strings. Returns the (zero or negative) adjustment in
 * the number of characters in A. */
static int string_set_difference(char *a, const char *b, unsigned length_b)
{
	unsigned length_p;
	const char *p;
	for (p = a; *p != '\0'; p += 1 + length_p) {
		length_p = strlen(p);
		if (!string_set_contains(b, length_b, p)) {
			strcpy(a, p);
			a += length_p + 1;
		}
	}
	*a = '\0';
	return a - p;
}

static const unsigned VALIDATION_BUFFER_SIZE = 1024;

/* Temporary container for the result of validation. */
struct ValidationResult {
	AttributeSemantic semantic;
	AttributeStorage storage;
	char buffer[VALIDATION_BUFFER_SIZE];
	AttributeData *data;
	int size;
	int capacity;
	int terminators;
};

static void vresult_init(ValidationResult *vr)
{
	vr->storage = STORAGE_NONE;
	vr->capacity = 0;
	vr->size = 0;
	vr->data = (AttributeData *)vr->buffer;
	vr->terminators = 0;
}

static void vresult_set_static(ValidationResult *vr, const char *s, unsigned length)
{
	vr->storage = STORAGE_STRING;
	vr->data = (AttributeData *)s;
	vr->size = (int)length;
	vr->capacity = 0;
	vr->terminators = 1;
}

static char *vresult_allocate(ValidationResult *vr, unsigned capacity)
{
	vr->storage = STORAGE_STRING;
	if (capacity > VALIDATION_BUFFER_SIZE) {
		vr->data = (AttributeData *)(new char[capacity]);
		vr->capacity = (int)capacity;
	} else {
		vr->data = (AttributeData *)vr->buffer;
		vr->capacity = -int(capacity);
	}
	return vr->data->string;
}

static void vresult_free(ValidationResult *vr)
{
	if (vr->capacity > 0)
		delete [] (char *)vr->data;
}

/* Performs validation checks common to all attributes. A return value <= 0 
 * indicates that validation has succeded or failed in the pre-check. */
static int initialize_validation(int name, ValueSemantic vs,
	AttributeOperator op, ValidationResult *result)
{
	vs;

	/* Make sure the token is an attribute name. */
	AttributeSemantic as = attribute_semantic(name);
	if (as == ASEM_INVALID)
		return STKR_NO_SUCH_ATTRIBUTE;
	result->semantic = as;

	/* A valid operation for this kind of attribute? */
	if (((1 << op) & supported_operators(as)) == 0)
		return STKR_INVALID_OPERATION;

	return ADEF_DEFINED;
}

/* Determines whether an integer (value, semantic) pair can be assigned to an
 * attribute with the specified semantic. If it can, the mode the attribute
 * will be switched into is returned. Otherwise, a validation error code is
 * returned. */
static int validate_integer(int name, ValueSemantic vs, int value, 
	AttributeOperator op, ValidationResult *result)
{
	int rc = initialize_validation(name, vs, op, result);
	if (rc < ADEF_DEFINED)
		return rc;

	/* Every attribute can be undefined. */
	if (vs == VSEM_TOKEN && value == TOKEN_UNDEFINED)
		return ADEF_UNDEFINED;

	/* What mode will this (value, semantic) pair switch the attribute into? */
	int mode = STKR_TYPE_MISMATCH;
	AttributeSemantic as = result->semantic;
	switch (as) {
		case ASEM_DIMENSON:
			if (vs == VSEM_PERCENTAGE) {
				mode = unsigned(value) > 100 ? 
					STKR_OUT_OF_BOUNDS : DMODE_FRACTIONAL;
			}
			/*  Fall through. */
		case ASEM_ABSOLUTE_DIMENSION:
			if (vs == VSEM_TOKEN && value == TOKEN_AUTO)
				mode = DMODE_AUTO;
			else if (vs == VSEM_NONE)
				mode = DMODE_ABSOLUTE;
			break;
		case ASEM_COLOR:
			if (vs == VSEM_COLOR || vs == VSEM_NONE)
				mode = ADEF_DEFINED;
			break;
		case ASEM_FLAG:
			if (vs == VSEM_BOOLEAN) {
				if (value == 0) 
					mode = FLAGMODE_FALSE;
				else if (value == 1) 
					mode = FLAGMODE_TRUE;
				else
					mode = STKR_OUT_OF_BOUNDS;
			}
			break;
		case ASEM_ALIGNMENT:
			if (vs == VSEM_TOKEN) {
				mode = ALIGN_START + (value - TOKEN_START);
				if (mode < ALIGN_START || mode >= ALIGN_SENTINEL)
					mode = STKR_TYPE_MISMATCH;
			}
			break;
		case ASEM_JUSTIFICATION:
			if (vs == VSEM_TOKEN) {
				switch (value) {
					case TOKEN_LEFT:
						mode = JUSTIFY_LEFT;
						break;
					case TOKEN_RIGHT:
						mode = JUSTIFY_RIGHT;
						break;
					case TOKEN_CENTER:
						mode = JUSTIFY_CENTER;
						break;
					case TOKEN_FLUSH:
						mode = JUSTIFY_FLUSH;
						break;
					default:
						mode = STKR_TYPE_MISMATCH;
						break;
				}
			}
			break;
		case ASEM_LAYOUT:
			if (vs == VSEM_TOKEN) {
				if (value == TOKEN_NONE) {
					mode = LCTX_NO_LAYOUT;
				} else {
					mode = LCTX_BLOCK + (value - TOKEN_BLOCK);
					if (mode < LCTX_BLOCK || mode >= LCTX_SENTINEL)
						mode = STKR_TYPE_MISMATCH;
				}
			}
			break;
		case ASEM_EDGES:
			if (vs == VSEM_TOKEN) {
				switch (value) {
					case TOKEN_NONE:
						mode = EDGE_FLAG_NONE;
						break;
					case TOKEN_ALL:
						mode = EDGE_FLAG_ALL;
						break;
					case TOKEN_HORIZONTAL:
						mode = EDGE_FLAG_HORIZONTAL;
						break;
					case TOKEN_VERTICAL:
						mode = EDGE_FLAG_VERTICAL;
						break;
					case TOKEN_LEFT:
						mode = EDGE_FLAG_LEFT;
						break;
					case TOKEN_RIGHT:
						mode = EDGE_FLAG_RIGHT;
						break;
					case TOKEN_TOP:
						mode = EDGE_FLAG_TOP;
						break;
					case TOKEN_BOTTOM:
						mode = EDGE_FLAG_BOTTOM;
						break;
					default:
						mode = STKR_TYPE_MISMATCH;
						break;
				}
			} else if (vs == VSEM_EDGES) {
				mode = (value == (value & EDGE_FLAG_ALL)) ? 
					value : STKR_OUT_OF_BOUNDS;
			}
			break;
		case ASEM_WHITE_SPACE:
			if (vs == VSEM_TOKEN) {
				if (value == TOKEN_NORMAL) 
					mode = WSM_NORMAL;
				else if (value == TOKEN_PRESERVE)
					mode = WSM_PRESERVE;
				else
					mode = STKR_TYPE_MISMATCH;
			}
			break;
		case ASEM_WRAP_MODE:
			if (vs == VSEM_TOKEN) {
				if (value == TOKEN_WORD_WRAP) 
					mode = WRAPMODE_WORD;
				else if (value == TOKEN_CHARACTER_WRAP)
					mode = WRAPMODE_CHARACTER;
				else
					mode = STKR_TYPE_MISMATCH;
			}
			break;
		case ASEM_BOUNDING_BOX:
			if (vs == VSEM_TOKEN) {
				if (value == TOKEN_AUTO || value == TOKEN_NONE) {
					mode = BBOX_PADDING;
				} else {
					mode = BBOX_CONTENT + (value - TOKEN_CONTENT_BOX);
					if (mode < BBOX_CONTENT || mode >= BBOX_SENTINEL)
						mode = STKR_TYPE_MISMATCH;
				}
			}
			break;
		case ASEM_BACKGROUND_SIZE:
			if (vs == VSEM_TOKEN) {
				if (value == TOKEN_AUTO || value == TOKEN_NONE) {
					mode = VLPM_STANDARD;
				} else {
					mode = VLPM_FIT + (value - TOKEN_FIT);
					if (mode < VLPM_FIT || mode >= VLPM_SENTINEL)
						mode = STKR_TYPE_MISMATCH;
				}
			}
			break;
		case ASEM_BACKGROUND:
			if (vs == VSEM_TOKEN) {
				if (value == TOKEN_NONE) {
					mode = ADEF_UNDEFINED;
				} else {
					mode = BGMODE_PANE_FIRST + (value - TOKEN_FLAT);
					if (mode < BGMODE_PANE_FIRST || mode > BGMODE_PANE_LAST)
						mode = STKR_TYPE_MISMATCH;
					}
			} else if (vs == VSEM_COLOR) {
				mode = BGMODE_COLOR;
			}
			break;
		case ASEM_CURSOR:
			if (vs == VSEM_TOKEN) {
				if (value == TOKEN_DEFAULT || value == TOKEN_AUTO || 
					value == TOKEN_NONE) {
					mode = CT_DEFAULT;
				} else {
					mode = CT_HAND + (value - TOKEN_CURSOR_HAND);
					if (mode < CT_HAND || mode >= CT_SENTINEL)
						mode = STKR_TYPE_MISMATCH;
				}
			}
			break;
	}
	if (mode < 0)
		return mode;

	/* Choose the smallest storage type that can represent the value without
	 * loss of information, or, failing that, the widest permitted type. */
	unsigned permitted_types = storage_mask(as, mode);
	if ((permitted_types & STORAGE_BIT_INT16) != 0 &&
		value >= SHRT_MIN && value <= SHRT_MAX) {
		result->storage = STORAGE_INT16;
	} else if ((permitted_types & STORAGE_BIT_INT32) != 0) {
		result->storage = STORAGE_INT32;
	} else if ((permitted_types & STORAGE_BIT_FLOAT32) != 0) {
		result->storage = STORAGE_FLOAT32;
	} else if ((permitted_types & STORAGE_BIT_INT16) != 0) {
		result->storage = STORAGE_INT16;
	} else if ((permitted_types & STORAGE_BIT_NONE) != 0) {
		result->storage = STORAGE_NONE;
	} else {
		assertb(false);
		return 0;
	}

	/* Convert the value to its storage type. */
	switch (result->storage) {
		case STORAGE_NONE:
			result->size = 0;
			break;
		case STORAGE_INT16:
			if (value < SHRT_MIN || value > SHRT_MAX)
				return STKR_OUT_OF_BOUNDS;
			if (as == ASEM_DIMENSON && mode == DMODE_FRACTIONAL) {
				ensure(vs == VSEM_PERCENTAGE);
				value = int((uint32_t)value * INT16_MAX / 100u);
			}
			result->data->int16 = (int16_t)value;
			result->size = sizeof(int16_t);
			break;
		case STORAGE_INT32:
			if (as == ASEM_DIMENSON && mode == DMODE_FRACTIONAL) {
				ensure(vs == VSEM_PERCENTAGE);
				value = int((uint64_t)value * INT32_MAX / 100ull);
			} 
			result->data->int32 = value;
			result->size = sizeof(int32_t);
			break;
		case STORAGE_FLOAT32:
			result->data->float32 = (float)value;
			result->size = sizeof(float);
			break;
		default:
			assertb(false);
			return 0;
	}

	return mode;
}

static int validate_float(int name, ValueSemantic vs, float value, 
	AttributeOperator op, ValidationResult *result)
{
	int rc = initialize_validation(name, vs, op, result);
	if (rc < ADEF_DEFINED)
		return rc;

	/* Determine the mode. */
	int mode = STKR_TYPE_MISMATCH;
	AttributeSemantic as = result->semantic;
	switch (as) {
		case ASEM_DIMENSON:
			if (vs == VSEM_PERCENTAGE) {
				float tolerance = 100.0f * FLT_EPSILON;
				mode = (value < -tolerance || value > 100.0f + tolerance) ? 
					STKR_OUT_OF_BOUNDS : DMODE_FRACTIONAL;
				break;
			}
			/*  Fall through. */
		case ASEM_ABSOLUTE_DIMENSION:
			if (vs == VSEM_NONE)
				mode = DMODE_ABSOLUTE;
			break;
	}
	if (mode < 0)
		return mode;

	/* Choose the widest numeric type permitted. */
	unsigned permitted_types = storage_mask(as, mode);
	if ((permitted_types & STORAGE_BIT_FLOAT32) != 0) {
		result->storage = STORAGE_FLOAT32;
	} else if ((permitted_types & STORAGE_BIT_INT32) != 0) {
		result->storage = STORAGE_INT32;
	} else if ((permitted_types & STORAGE_BIT_INT16) != 0) {
		result->storage = STORAGE_INT16;
	} else if ((permitted_types & STORAGE_BIT_NONE) != 0) {
		result->storage = STORAGE_NONE;
	} else {
		assertb(false);
		return 0;
	}

	/* Convert the value to its storage type. */
	if (vs == VSEM_PERCENTAGE)
		value *= 1.0f / 100.0f;
	switch (result->storage) {
		case STORAGE_NONE:
			result->size = 0;
			break;
		case STORAGE_INT16:
			if (value < float(SHRT_MIN) || value > float(SHRT_MAX))
				return STKR_OUT_OF_BOUNDS;
			if (as == ASEM_DIMENSON && mode == DMODE_FRACTIONAL) {
				result->data->int16 = (int16_t)round_signed(value * float(INT16_MAX));
			} else {
				result->data->int16 = (int16_t)round_signed(value);
			}
			result->size = sizeof(int16_t);
			break;
		case STORAGE_INT32:
			if (as == ASEM_DIMENSON && mode == DMODE_FRACTIONAL) {
				result->data->int32 = round_signed(value * float(INT32_MAX));
			} else {
				result->data->int32 = round_signed(value);
			}
			result->size = sizeof(int32_t);
			break;
		case STORAGE_FLOAT32:
			result->data->float32 = value;
			result->size = sizeof(float);
			break;
		default:
			assertb(false);
			return 0;
	}

	return mode;
}

/* Determines whether an string (value, semantic) pair can be assigned to an
 * attribute with the specified semantic. If it can, the mode the attribute
 * will be switched into is returned. Otherwise, a validation error code is
 * returned. */
static int validate_string(int name, ValueSemantic vs, 
	const char *value, int length, AttributeOperator op, 
	ValidationResult *result)
{
	int rc = initialize_validation(name, vs, op, result);
	if (rc < ADEF_DEFINED)
		return rc;

	if (length < 0)
		length = (int)strlen(value);
	vresult_set_static(result, value, length);

	/* Determine the new mode. */
	int mode = STKR_TYPE_MISMATCH;
	AttributeSemantic as = result->semantic;
	switch (as) {
		case ASEM_STRING:
			if (vs == VSEM_NONE)
				mode = ADEF_DEFINED;
			break;
		case ASEM_STRING_SET:
			if (vs == VSEM_NONE || vs == VSEM_LIST)
				mode = ADEF_DEFINED;
			break;
		case ASEM_URL:
			if (vs == VSEM_NONE || vs == VSEM_URL)
				mode = ADEF_DEFINED;
			break;
		case ASEM_BACKGROUND:
			if (vs == VSEM_URL)
				mode = BGMODE_URL;
			break;
	}
	if (mode < 0)
		return mode;

	/* Choose the storage type. */
	unsigned permitted_types = storage_mask(as, mode);
	if ((permitted_types & STORAGE_BIT_STRING) != 0) {
		result->storage = STORAGE_STRING;
	} else if ((permitted_types & STORAGE_BIT_NONE) != 0) {
		result->storage = STORAGE_NONE;
	} else {
		assertb(false);
		return 0;
	}

	/* Convert the value to storage form if required. */
	if (as == ASEM_STRING_SET) {
		rc = parse_string_list(value, length, result->buffer, 
			sizeof(result->buffer));
		if (rc < 0)
			return rc;
		if (rc >= sizeof(result->buffer)) {
			rc = parse_string_list(value, length, 
				vresult_allocate(result, rc + 1), rc + 1);
		} else {
			result->data = (AttributeData *)result->buffer;
		}
		result->size = rc;
		result->terminators = 1;
		if (rc >= 0)
			result->size += string_set_unique(result->data->string, rc);
		mode = rc < 0 ? rc : ADEF_DEFINED;
	}

	return mode;
}

static BufferEntry *abuf_allocate_replace(
	AttributeBuffer *abuf, 
	int name,
	int mode, 
	AttributeStorage storage_type, 
	AttributeOperator op, 
	unsigned required_size);

static int abuf_fold(
	AttributeBuffer *abuf, 
	BufferEntry *ea, 
	AttributeStorage type_b, 
	int mode_b,
	AttributeOperator op_b,
	const AttributeData *data_b, 
	unsigned size_b,
	BufferEntry **out_folded);

/*
 * Attribute Buffers
 */

 
inline BufferEntry *abuf_next_entry(BufferEntry *entry)
{
	return (BufferEntry *)((char *)entry + sizeof(Attribute) + 
		entry->header.size);
}

inline BufferEntry *abuf_end(AttributeBuffer *abuf)
{
	return (BufferEntry *)(abuf->buffer + abuf->size);
}
 
/* Returns the number of attributes in the range [start, end). */
static unsigned count_attributes_between(const BufferEntry *start, 
	const BufferEntry *end)
{
	unsigned count = 0;
	while (start != end) {
		start = abuf_next_entry((BufferEntry *)start);
		count++;
	}
	return count;
}

static void abuf_reallocate(AttributeBuffer *abuf, unsigned new_size)
{
	if (int(new_size) > abs(abuf->capacity)) {
		char *block = new char[new_size];
		if (abuf->buffer != NULL) {
			memcpy(block, abuf->buffer, abuf->size);
			if (abuf->capacity > 0) 
				delete [] abuf->buffer;
		}
		abuf->buffer = block;
		abuf->capacity = int(new_size);
	}
	abuf->size = int(new_size);
}

/* Allocates a new entry at the end of the buffer. */
static BufferEntry *abuf_create_attribute(AttributeBuffer *abuf, int name,
	int mode, AttributeStorage storage, AttributeOperator op, 
	unsigned data_size)
{
	unsigned offset = abuf->size;
	unsigned new_size = abuf->size + sizeof(Attribute) + data_size;
	abuf_reallocate(abuf, new_size);
	BufferEntry *entry = (BufferEntry *)((char *)abuf->buffer + offset);
	entry->header.name = (uint8_t)name;
	entry->header.mode = mode;
	entry->header.type = storage;
	entry->header.folded = false;
	entry->header.op = op;
	entry->header.size = check16(data_size);
	abuf->num_attributes++;
	return entry;
}

/* Deletes a single buffer entry. */
static void abuf_remove_one(AttributeBuffer *abuf, Attribute *attribute)
{
	unsigned attr_size = sizeof(Attribute) + attribute->size;
	char *buf_end = (char *)abuf->buffer + abuf->size;
	char *attr_end = (char *)attribute + attr_size;
	unsigned suffix = buf_end - attr_end;
	memmove(attribute, attr_end, suffix);
	abuf->size -= int(attr_size);
	abuf->num_attributes--;
}

/* Removes all entries for a name. */
static void abuf_remove_all(AttributeBuffer *abuf, int name)
{
	BufferEntry *entry = (BufferEntry *)abuf->buffer;
	BufferEntry *end = (BufferEntry *)(abuf->buffer + abuf->size);
	unsigned bytes_removed = 0;
	while (entry != end) {
		unsigned entry_size = sizeof(Attribute) + entry->header.size;
		unsigned gap = 0;
		if (entry->header.name == name) {
			gap = entry_size;
			abuf->num_attributes--;
		}
		memmove((char *)entry - bytes_removed, entry, entry_size);
		entry = (BufferEntry *)((char *)entry + entry_size);
		bytes_removed += gap;
	}
	abuf->size -= bytes_removed;
}

/* Allocates or reallocates storage for a single buffer entry. Any existing
 * entries for the attribute are removed. */
static BufferEntry *abuf_allocate_replace(AttributeBuffer *abuf, int name,
	int mode, AttributeStorage storage_type, AttributeOperator op, 
	unsigned required_size)
{
	/* FIXME (TJM): reuse memory. */
	abuf_remove_all(abuf, name);
	return abuf_create_attribute(abuf, name, mode, storage_type,
		op, required_size);
}

/* Resizes a buffer entry. Existing entry data is preserved (but will be 
 * truncated if the new size is smaller than the old). */
static BufferEntry *abuf_resize_entry(AttributeBuffer *abuf, 
	BufferEntry *entry, unsigned data_size)
{
	unsigned new_size = abuf->size + (data_size - entry->header.size);
	unsigned start_offset = (char *)entry - (char *)abuf->buffer;
	unsigned old_end_offset = start_offset + sizeof(Attribute) + entry->header.size;
	unsigned new_end_offset = start_offset + sizeof(Attribute) + data_size;
	char *old_end = abuf->buffer + old_end_offset;
	if (int(new_size) > abs(abuf->capacity)) {
		char *new_buffer = new char[new_size];
		char *new_end = new_buffer + new_end_offset;
		unsigned copy_size = std::min(old_end_offset, new_end_offset);
		memcpy(new_buffer, abuf->buffer, copy_size);
		memcpy(new_end, old_end, abuf->size - old_end_offset);
		entry = (BufferEntry *)(new_buffer + start_offset);
		if (abuf->capacity > 0)
			delete [] abuf->buffer;
		abuf->buffer = new_buffer;
		abuf->capacity = int(new_size);
	} else {
		char *new_end = (char *)abuf->buffer + new_end_offset;
		memmove(new_end, old_end, abuf->size - old_end_offset);
	}
	abuf->size = int(new_size);
	entry->header.size = check16(data_size);
	return entry;
}

void abuf_init(AttributeBuffer *abuf, void *storage, unsigned storage_size)
{
	abuf->buffer = storage_size != 0 ? (char *)storage : NULL;
	abuf->size = 0;
	abuf->capacity = -int(storage_size);
	abuf->num_attributes = 0;
}

void abuf_clear(AttributeBuffer *abuf)
{
	if (abuf->capacity > 0) {
		delete [] abuf->buffer;
		abuf->buffer = NULL;
		abuf->capacity = 0;
	}
	abuf->size = 0;
	abuf->num_attributes = 0;
}

const Attribute *abuf_first(const AttributeBuffer *abuf)
{
	return abuf->size != 0 ? (const Attribute *)(abuf->buffer) : NULL;
}

const Attribute *abuf_next(const AttributeBuffer *abuf, 
	const Attribute *attribute)
{
	if (attribute != NULL) {
		const BufferEntry *end = abuf_end((AttributeBuffer *)abuf);
		const BufferEntry *next = abuf_next_entry((BufferEntry *)attribute);
		attribute = (next != end) ? (const Attribute *)next : NULL;
	}
	return attribute;
}

/* Appends an entry to the end of the buffer. */
Attribute *abuf_append(AttributeBuffer *abuf, const Attribute *attribute)
{
	unsigned attr_size = sizeof(Attribute) + attribute->size;
	abuf_reallocate(abuf, abuf->size + attr_size);
	BufferEntry *dest = (BufferEntry *)(abuf->buffer + abuf->size - attr_size);
	memcpy(dest, attribute, attr_size);
	abuf->num_attributes++;
	return &dest->header;
}

/* Adds an entry to the start of the buffer. */
Attribute *abuf_prepend(AttributeBuffer *abuf, const Attribute *attribute)
{
	unsigned attr_size = sizeof(Attribute) + attribute->size;
	abuf_reallocate(abuf, abuf->size + attr_size);
	memmove(abuf->buffer + attr_size, abuf->buffer, abuf->size - attr_size);
	memcpy(abuf->buffer, attribute, attr_size);
	abuf->num_attributes++;
	return (Attribute *)abuf_first(abuf);
}

/* Overwrites one attribute with another in place. */
Attribute *abuf_replace(AttributeBuffer *abuf, Attribute *a, 
	const Attribute *b)
{
	BufferEntry *ea = (BufferEntry *)a;
	if (ea->header.size != b->size)
		ea = abuf_resize_entry(abuf, ea, b->size);
	memcpy(ea, b, sizeof(Attribute) + b->size);
	return &ea->header;
}

/* Replaces the range of attributes [start, end) with the attributes from a
 * source buffer. */
void abuf_replace_range(AttributeBuffer *abuf, const Attribute *start, 
	const Attribute *end, const AttributeBuffer *source)
{
	if (start == NULL)
		start = (const Attribute *)abuf_first(abuf);
	if (end == NULL)
		end = (const Attribute *)abuf_end(abuf);

	unsigned old_start = 0;
	unsigned new_range_size = 0;
	if (source != NULL) {
		new_range_size = source->size;
		abuf->num_attributes += source->num_attributes;
	}

	if (start != NULL && end != NULL) {
		old_start = (char *)start - abuf->buffer;
		unsigned old_end = (char *)end - abuf->buffer;
		unsigned old_range_size = old_end - old_start;
		unsigned old_size = abuf->size;
		abuf->num_attributes -= count_attributes_between(
			(const BufferEntry *)start, 
			(const BufferEntry *)end);
		unsigned new_end = old_start + new_range_size;
		unsigned new_size = abuf->size + new_range_size - old_range_size;

		abuf_reallocate(abuf, new_size);
		memmove(abuf->buffer + new_end, abuf->buffer + old_end, 
			old_size - old_end);
	} else {
		abuf_reallocate(abuf, new_range_size);
	}
	if (new_range_size != 0)
		memcpy(abuf->buffer + old_start, source->buffer, new_range_size);
	
}

/* Casts a numeric attribute to an integer. */
static int attribute_as_int(AttributeStorage storage, const AttributeData *data)
{
	switch (storage) {
		case STORAGE_INT16:
			return (int)data->int16;
		case STORAGE_INT32:
			return data->int32;
		case STORAGE_FLOAT32:
			return round_signed(data->float32);
	}
	assertb(false);
	return 0;
}

/* Casts a numeric attribute to a float. */
static float attribute_as_float(AttributeStorage storage, 
	const AttributeData *data)
{
	switch (storage) {
		case STORAGE_INT16:
			return (float)data->int16;
		case STORAGE_INT32:
			return (float)data->int32;
		case STORAGE_FLOAT32:
			return (float)data->float32;
	}
	assertb(false);
	return 0.0f;
}

/* Attempts to replace operation A with an operation that does the same thing
 * as A followed by B. Returns a pointer to the modified A if folding occurred,
 * otherwise returns NULL. */
static int abuf_fold(
	AttributeBuffer *abuf, 
	BufferEntry *ea, 
	AttributeStorage type_b, 
	int mode_b,
	AttributeOperator op_b,
	const AttributeData *data_b, 
	unsigned size_b,
	BufferEntry **out_folded)
{
	/* Assignments just replace the existing attribute with a SET. */
	AttributeOperator op_a = (AttributeOperator)ea->header.op;
	if (op_b <= AOP_OVERRIDE) {
		if (ea->header.size != size_b)
			ea = abuf_resize_entry(abuf, ea, size_b);
		ea->header.mode = mode_b;
		ea->header.folded = true;
		ea->header.op = op_b;
		ea->header.size = size_b;
		ea->header.type = type_b;
		memcpy(&ea->data, data_b, size_b);
		if (out_folded != NULL)
			*out_folded = ea;
		return true;
	}

	/* Arithmetic with an undefined RHS is a no-op. */
	if (mode_b == ADEF_UNDEFINED) {
		if (out_folded != NULL)
			*out_folded = ea;
		return false;
	}

	/* The type of the result is the wider of the operand types. */
	AttributeSemantic as = attribute_semantic(ea->header.name);
	AttributeStorage type_a = (AttributeStorage)ea->header.type;
	AttributeStorage type_ab = type_a > type_b ? type_a : type_b;

	/* Determine the operation to execute and the operation represented by the
	 * result. */
	AttributeOperator op = op_b, result_op = op_a;
	if (op_a <= AOP_OVERRIDE) {
		/* A is a set. The result is the same kind set. */
		result_op = op_a;
	} else {
		/* Neither op is a set. The result is a modifier. */
		if (op_a == op_b) {
			/* The operators are the same. We can always fold, but if the 
			 * operator is non-associative, we have to invert it so that the
			 * folded operation has the same effect as applying A followed
			 * by B, e.g. -(a - b) => -(a + b) == - a - b. */
			if (op == AOP_SUBTRACT || op == AOP_DIVIDE)
				op = (AttributeOperator)(int(op) + 1);
		} else {
			/* Different operators can be folded only if they are closed over
			 * the result type. This is true of the arithmetic operators but not 
			 * of set-difference. For example, the sequence "+ 4 - 3" can be 
			 * folded into "+1", but the sequence "union {x} diff {y}" cannot be 
			 * represented as a single set union or difference. */
			if (as == ASEM_STRING_SET)
				return STKR_CANNOT_FOLD;
		}
	}

	/* Perform the operation. */
	bool changed = false;
	if (type_ab == STORAGE_NONE) {
		assertb(as == ASEM_EDGES);
		int new_mode = ea->header.mode;
		switch (op) {
			case AOP_ADD:
				new_mode |= mode_b;
				break;
			case AOP_SUBTRACT:
				new_mode &= ~mode_b;
				break;
		}
		if ((new_mode & EDGE_FLAG_ALL) != 0)
			new_mode &= ~EDGE_FLAG_NONE;
		if (new_mode != ea->header.mode) {
			ea->header.mode = new_mode;
			changed = true;
		}
	} else if (type_ab == STORAGE_INT16 || type_ab == STORAGE_INT32) {
		int va = attribute_as_int(type_a, &ea->data);
		int vb = attribute_as_int(type_b, data_b);
		int result;
		switch (op_b) {
			case AOP_ADD:
				result = va + vb;
				break;
			case AOP_SUBTRACT:
				result = va - vb;
				break;
			case AOP_MULTIPLY:
				result = va * vb;
				break;
			case AOP_DIVIDE:
				result = vb != 0 ? va / vb : 0;
				break;
			default:
				assertb(false);
				return NULL;
		}
		if (type_a != type_ab) {
			unsigned data_size = (type_ab == STORAGE_INT16) ? 
				sizeof(int16_t) : sizeof(int32_t);
			ea = abuf_resize_entry(abuf, ea, data_size);
			ea->header.type = type_ab;
			changed = true;
		}
		if (type_ab == STORAGE_INT16) {
			if (as == ASEM_DIMENSON && (ea->header.mode == DMODE_FRACTIONAL || 
				mode_b == DMODE_FRACTIONAL))
				result >>= 16;
			int16_t converted = saturate16(result);
			if (converted != ea->data.int16) {
				ea->data.int16 = converted;
				changed = true;
			}
		} else {
			if (ea->data.int32 != result) {
				ea->data.int32 = result;
				changed = true;
			}
		}
	} else if (type_ab == STORAGE_FLOAT32) {
		float va = attribute_as_float(type_a, &ea->data);
		float vb = attribute_as_float(type_b, data_b);
		float result;
		switch (op_b) {
			case AOP_ADD:
				result = va + vb;
				break;
			case AOP_SUBTRACT:
				result = va - vb;
				break;
			case AOP_MULTIPLY:
				result = va * vb;
				break;
			case AOP_DIVIDE:
				result = vb != 0.0f ? va / vb : 0.0f;
				break;
			default:
				assertb(false);
				return NULL;
		}
		if (type_a != STORAGE_FLOAT32) {
			ea = abuf_resize_entry(abuf, ea, sizeof(float));
			ea->header.type = STORAGE_FLOAT32;
			changed = true;
		}
		if (result != ea->data.float32) {
			ea->data.float32 = result;
			changed = true;
		}
	} else if (type_ab == STORAGE_STRING) {
		if (type_a == STORAGE_NONE) {
			ea = abuf_resize_entry(abuf, ea, size_b);
			ea->header.mode = mode_b;
			ea->header.folded = true;
			ea->header.op = op_b;
			ea->header.size = size_b;
			ea->header.type = type_b;
			memcpy(&ea->data, data_b, size_b);
			changed = true;
		} else {
			unsigned length_a = ea->header.size - 1;
			unsigned length_b = size_b - 1;
			unsigned length_ab;
			ea->header.type = STORAGE_STRING;
			char *p = ea->data.string, *q = (char *)data_b->string;
			if (op == AOP_ADD) {
				length_ab = length_a + length_b;
				if (ea->header.size != length_ab + 1) {
					ea = abuf_resize_entry(abuf, ea, length_ab + 1);
					p = ea->data.string;
				}
				memcpy(p + length_a, q, length_b + 1);
				q = p + length_a;
			} else {
				length_ab = length_a;
			}
			if (as == ASEM_STRING_SET) {
				unsigned length_q = length_b;
				if (op == AOP_ADD) {
					std::swap(p, q);
					length_q = length_a;
				}
				length_ab += string_set_difference(p, q, length_q);
				if (ea->header.size != length_ab + 1)
					ea = abuf_resize_entry(abuf, ea, length_ab + 1);
			}
			changed = length_ab != length_a;
		} 
	}

	ea->header.folded = true;
	ea->header.op = result_op;
	if (out_folded != NULL)
		*out_folded = ea;
	return changed;
}

int abuf_fold(AttributeBuffer *abuf, Attribute *a, const Attribute *b, 
	Attribute **out_folded)
{
	return abuf_fold(
		abuf, 
		(BufferEntry *)a, 
		(AttributeStorage)b->type, 
		b->mode, 
		(AttributeOperator)b->op, 
		&((const BufferEntry *)b)->data, 
		b->size, 
		(BufferEntry **)out_folded);
}

/* Returns an attribute's current mode, or 'defmode' if the attribute is
 * undefined. */
int abuf_read_mode(const Attribute *attribute, int defmode)
{
	return attribute != NULL && attribute->mode != ADEF_UNDEFINED ? 
		attribute->mode : defmode;
}

/* Reads an attribute as an integer. */
int abuf_read_integer(const Attribute *attribute, int32_t *result, 
	int32_t defval)
{
	if (attribute == NULL || attribute->mode == ADEF_UNDEFINED) {
		*result = defval;
		return ADEF_UNDEFINED;
	}
	const BufferEntry *entry = (const BufferEntry *)attribute;
	switch (attribute->type) {
		case STORAGE_NONE:
			*result = defval;
			break;
		case STORAGE_INT16:
			*result = entry->data.int16;
			break;
		case STORAGE_INT32:
			*result = entry->data.int32;
			break;
		case STORAGE_FLOAT32:
			*result = round_signed(entry->data.float32);
			break;
		default:
			assertb(false);
			return ADEF_UNDEFINED;
	}
	return attribute->mode;
}

/* Reads an attribute as a float. */
int abuf_read_float(const Attribute *attribute, float *result, float defval)
{
	if (attribute == NULL || attribute->mode == ADEF_UNDEFINED) {
		*result = defval;
		return ADEF_UNDEFINED;
	}
	AttributeSemantic as = attribute_semantic(attribute->name);
	const BufferEntry *entry = (const BufferEntry *)attribute;
	switch (attribute->type) {
		case STORAGE_NONE:
			*result = defval;
			break;
		case STORAGE_INT16:
			*result = float(entry->data.int16);
			if (as == ASEM_DIMENSON && attribute->mode == DMODE_FRACTIONAL)
				*result *= 1.0f / float(INT16_MAX);
			break;
		case STORAGE_INT32:
			*result = float(entry->data.int32);
			if (as == ASEM_DIMENSON && attribute->mode == DMODE_FRACTIONAL)
				*result *= 1.0f / float(INT32_MAX);
			break;
		case STORAGE_FLOAT32:
			*result = entry->data.float32;
			break;
		default:
			assertb(false);
			return ADEF_UNDEFINED;
	}
	return attribute->mode;
}

/* Reads an attribute value as a string, returning a pointer to the data inside
 * the buffer via 'result'. The result string is guaranteed to be null 
 * terminated. The result pointer is invalidated by any mutation of the 
 * attribute buffer. */
int abuf_read_string(const Attribute *attribute, const char **result, 
	uint32_t *length, const char *defval)
{
	if (attribute == NULL || attribute->mode == ADEF_UNDEFINED)
		goto missing;
	switch (attribute->type) {
		case STORAGE_STRING:
			*result = ((const BufferEntry *)attribute)->data.string;
			if (length != NULL)
				*length = attribute->size - 1;
			break;
		default:
			assertb(false);
			goto missing;
	}
	return attribute->mode;

missing:

	*result = defval;
	if (length != NULL)
		*length = defval != NULL ? strlen(defval) : 0;
	return ADEF_UNDEFINED;
}

/* Reads an attribute value as a string, copying the result to 'buffer', which
 * is guaranteed to be null terminated. */
int abuf_read_string(const Attribute *attribute, char *buffer,
	uint32_t buffer_size, uint32_t *out_length, const char *defval, 
	StringSetRepresentation ssr)
{
	const char *data = NULL;
	uint32_t length = 0;
	int mode = abuf_read_string(attribute, &data, &length, defval);
	if (buffer != NULL && buffer_size != 0) {
		if (length + 1 > buffer_size)
			length = buffer_size - 1;
		memcpy(buffer, data, length);
		buffer[length] = '\0';

		/* If this is a set, format a set literal of the requested type. */
		if (attribute_semantic(attribute->name) == ASEM_STRING_SET &&
			ssr != SSR_INTERNAL) {
			char delimiter = (ssr == SSR_COMMA_DELIMITED) ? ',' : ' ';
			for (unsigned i = 0; i < length; ++i)
				if (buffer[i] == '\0')
					buffer[i] = delimiter;
			if (length != 0)
				buffer[--length] = '\0';
		}
	}
	if (out_length != NULL)
		*out_length = length;
	return mode;
}

/* Copies the result of attribute validation into an attribute buffer entry,
 * returning true if the stored value was changed. */
static int store_validated_attribute(AttributeBuffer *abuf, int name, int mode,
	AttributeOperator op, const ValidationResult *vr, bool fold)
{
	/* Do nothing if validation failed. */
	if (mode < 0)
		return mode;

	/* Validated strings are not necessarily null terminated. */
	unsigned stored_size = vr->size + vr->terminators;

	/* A size query? */
	if (abuf == NULL)
		return sizeof(Attribute) + stored_size;

	BufferEntry *entry;
	if (fold) {
		/* Try to fold with any existing entries. */
		entry = (BufferEntry *)abuf->buffer;
		BufferEntry *end = (BufferEntry *)(abuf->buffer + abuf->size);
		while (entry != end) {
			if (entry->header.name == name) {
				int rc = abuf_fold(abuf, entry, vr->storage, mode, 
					op, vr->data, stored_size, NULL);
				if (rc >= 0)
					return rc;
			}
			entry = abuf_next_entry(entry);
		}

		/* Folding wasn't possible. Create a new attribute. */
		entry = abuf_create_attribute(abuf, name, mode, vr->storage, op, 
			stored_size);
	} else {
		/* Reallocate memory for the attribute. */
		entry = abuf_allocate_replace(abuf, name, mode, vr->storage, 
			op, stored_size);
		/* Is the new value different from the old? */
		if (mode == (int)entry->header.mode &&
			op == (AttributeOperator)entry->header.op &&
			entry->header.size == stored_size &&
			(0 == memcmp(&entry->data, vr->data, vr->size))) {
			return false;
		}
	}

	/* Copy the validated data into the attribute and pad with the specified
	 * number of zero bytes. */
	memcpy(&entry->data, vr->data, vr->size);
	memset(&entry->data.string[vr->size], 0, vr->terminators);
	return true;
}

static int abuf_handle_shorthand_integer(AttributeBuffer *abuf, int name, 
	ValueSemantic vs, int value, AttributeOperator op, bool fold);
static int abuf_handle_shorthand_float(AttributeBuffer *abuf, int name, 
	ValueSemantic vs, float value, AttributeOperator op, bool fold);

int abuf_set_integer(AttributeBuffer *abuf, int name, ValueSemantic vs, 
	int value, AttributeOperator op, bool fold)
{
	/* Handle shorthand attributes like 'pad'. */
	int rc = abuf_handle_shorthand_integer(abuf, name, vs, value, op, fold);
	if (rc >= 0)
		return rc;

	/* Validate the value and determine the new mode. */
	ValidationResult vr;
	vresult_init(&vr);
	int new_mode = validate_integer(name, vs, value, op, &vr);

	/* Store the validated value. */
	int result = store_validated_attribute(abuf, name, new_mode, op, &vr, fold);
	vresult_free(&vr);
	return result;
}

int abuf_set_float(AttributeBuffer *abuf, int name, ValueSemantic vs, 
	float value, AttributeOperator op, bool fold)
{
	/* Handle shorthand atributes. */
	int rc = abuf_handle_shorthand_float(abuf, name, vs, value, op, fold);
	if (rc >= 0)
		return rc;

	/* Validate the value and determine the new mode. */
	ValidationResult vr;
	vresult_init(&vr);
	int new_mode = validate_float(name, vs, value, op, &vr);

	/* Store the validated value. */
	int result = store_validated_attribute(abuf, name, new_mode, op, &vr, fold);
	vresult_free(&vr);
	return result;
}

int abuf_set_string(AttributeBuffer *abuf, int name, ValueSemantic vs, 
	const char *value, int length, AttributeOperator op, bool fold)
{
	/* Validate the value and determine the new mode. */
	ValidationResult vr;
	vresult_init(&vr);
	int new_mode = validate_string(name, vs, value, length, op, &vr);

	/* Store the validated value. */
	int result = store_validated_attribute(abuf, name, new_mode, op, &vr, fold);
	vresult_free(&vr);
	return result;
}

/* Calls the appropriate function to store a variant into an attribute buffer. */
int abuf_set(AttributeBuffer *abuf, Token name, const Variant *value, 
	AttributeOperator op, bool fold)
{
	switch (value->type) {
		case VTYPE_INTEGER:
			/* Note that tokens representing enum values go through this path.
			 * Their integer value is the token itself. */
			return abuf_set_integer(abuf, name, value->semantic, 
				value->integer, op, fold);
		case VTYPE_FLOAT:
			return abuf_set_float(abuf, name, value->semantic, 
				value->real, op, fold);
		case VTYPE_STRING:
			return abuf_set_string(abuf, name, value->semantic, 
				value->string.data, value->string.length, op, fold);
	}
	assertb(false);
	return -1;
}

/* Handles the storage of shorthand attributes like 'pad', which set other
 * attributes but have no storage of their own. */
int abuf_handle_shorthand_integer(AttributeBuffer *abuf, int name, 
	ValueSemantic vs, int value, AttributeOperator op, bool fold)
{
	int rc = -1;
	if (name == TOKEN_PADDING) {
		rc  = abuf_set_integer(abuf, TOKEN_PADDING_LEFT,   vs, value, op, fold);
		rc += abuf_set_integer(abuf, TOKEN_PADDING_RIGHT,  vs, value, op, fold);
		rc += abuf_set_integer(abuf, TOKEN_PADDING_TOP,    vs, value, op, fold);
		rc += abuf_set_integer(abuf, TOKEN_PADDING_BOTTOM, vs, value, op, fold);
	} else if (name == TOKEN_MARGIN) {
		rc  = abuf_set_integer(abuf, TOKEN_MARGIN_LEFT,   vs, value, op, fold);
		rc += abuf_set_integer(abuf, TOKEN_MARGIN_RIGHT,  vs, value, op, fold);
		rc += abuf_set_integer(abuf, TOKEN_MARGIN_TOP,    vs, value, op, fold);
		rc += abuf_set_integer(abuf, TOKEN_MARGIN_BOTTOM, vs, value, op, fold);
	} else if (name >= TOKEN_CLIP_LEFT && name <= TOKEN_CLIP_BOTTOM) {
		int edges = EDGE_FLAG_LEFT << (name - TOKEN_CLIP_LEFT);
		op = (value == FLAGMODE_TRUE) ? AOP_ADD : AOP_SUBTRACT;
		rc = abuf_set_integer(abuf, TOKEN_CLIP, VSEM_EDGES, edges, op, true);
	}
	return rc;
}

/* Handles the storage of shorthand attributes like 'pad', which set other
 * attributes but have no storage of their own. */
int abuf_handle_shorthand_float(AttributeBuffer *abuf, int name, 
	ValueSemantic vs, float value, AttributeOperator op, bool fold)
{
	int rc = -1;
	if (name == TOKEN_PADDING) {
		rc  = abuf_set_float(abuf, TOKEN_PADDING_LEFT,   vs, value, op, fold);
		rc += abuf_set_float(abuf, TOKEN_PADDING_RIGHT,  vs, value, op, fold);
		rc += abuf_set_float(abuf, TOKEN_PADDING_TOP,    vs, value, op, fold);
		rc += abuf_set_float(abuf, TOKEN_PADDING_BOTTOM, vs, value, op, fold);
	} else if (name == TOKEN_MARGIN) {
		rc  = abuf_set_float(abuf, TOKEN_MARGIN_LEFT,   vs, value, op, fold);
		rc += abuf_set_float(abuf, TOKEN_MARGIN_RIGHT,  vs, value, op, fold);
		rc += abuf_set_float(abuf, TOKEN_MARGIN_TOP,    vs, value, op, fold);
		rc += abuf_set_float(abuf, TOKEN_MARGIN_BOTTOM, vs, value, op, fold);
	}
	return rc;
}

/* Generates a string representation of an attribute value for use in a 
 * diagnostic message. */
int attribute_value_string(char *buffer, unsigned buffer_size, 
	const Attribute *attribute)
{
	char value_buffer[1024];
	const char *data = (const char *)(attribute + 1);
	int length = 0;

	AttributeSemantic as = attribute_semantic(attribute->name);
	switch (attribute->type) {
		case STORAGE_NONE:
			length = snprintf(buffer, buffer_size, "none/%u", attribute->mode); 
			break;
		case STORAGE_STRING:
			{
				unsigned value_length = 0;
				abuf_read_string(attribute, value_buffer, sizeof(value_buffer), 
					&value_length, NULL, SSR_COMMA_DELIMITED);
				if (as == ASEM_STRING_SET) {
					length = snprintf(buffer, buffer_size, "{%s}/%u", 
						value_buffer, attribute->mode);
				} else {
					length = snprintf(buffer, buffer_size, "%s/%u", 
						value_buffer, attribute->mode);
				}
			}
			break;
		case STORAGE_INT16:
		case STORAGE_INT32:
			{
				int32_t value = (attribute->type == STORAGE_INT16) ? 
					*(int16_t *)data : *(int32_t *)data;
				if (as == ASEM_DIMENSON && attribute->mode == DMODE_FRACTIONAL) {
					uint32_t divisor = (attribute->type == STORAGE_INT16) ? 
						INT16_MAX : INT32_MAX;
					float percentage = 100.0f * (float)value / (float)divisor;
					length = snprintf(buffer, buffer_size, "%.1f%%/%u", percentage, attribute->mode);
				} else {
					length = snprintf(buffer, buffer_size, "%d/%u", value, attribute->mode);
				}
			}
			break;
		case STORAGE_FLOAT32:
			length = snprintf(buffer, buffer_size, "%.2f/%u", *(float *)data, attribute->mode);
			break;
		default:
			length = snprintf(buffer, buffer_size, "corrupt");
	}
	if (length < 0 || length == (int)buffer_size)
		length = buffer_size - 1;
	buffer[length] = '\0';
	return length;
}

} // namespace stkr
