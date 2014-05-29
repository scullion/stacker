#include "text_template.h"

#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cassert>

#include "url_cache.h"

#pragma warning(disable: 4505) // unreferenced local function has been removed

namespace sstl {

using namespace urlcache;

const unsigned MAX_INCLUSION_DEPTH = 16;

enum Opcode {
	TAOP_INSERT_TEXT,
	TAOP_INSERT_URL,
	TAOP_INSERT_VARIABLE,

	TAOP_PUSH,
	TAOP_PUSH_LITERAL,
	TAOP_POP,
	TAOP_POP_INSERT,
	
	TAOP_FILTER_ABS,
	TAOP_FILTER_STRIP,
	TAOP_FILTER_PRETTY,
	TAOP_FILTER_PRECISION,
	TAOP_FILTER_ENCODE,
	TAOP_FILTER_DECODE,
	TAOP_FILTER_BYTES,
	
	TAOP_AND,
	TAOP_OR,
	TAOP_NOT,

	TAOP_NEGATE,
	
	TAOP_ADD,
	TAOP_SUBTRACT,
	TAOP_MULTIPLY,
	TAOP_DIVIDE,
	TAOP_MODULO,

	TAOP_CONCATENATE,
	
	TAOP_EQUAL,
	TAOP_NOT_EQUAL,
	TAOP_LESS,
	TAOP_LESS_EQUAL,
	TAOP_GREATER,
	TAOP_GREATER_EQUAL,

	TAOP_BRANCH,
	TAOP_BRANCH_IF_FALSE
};

struct TemplateOperation {
	Opcode opcode;
	union {
		TemplateVariable literal;
		struct { unsigned offset, length; } text;
		struct { uint64_t key; } lookup;
		struct { urlcache::UrlKey key; } url;
		struct { unsigned target; } branch;
		struct { int precision; } pretty;
		struct { int precision; } precision;
		struct { char delimiter; unsigned count; } bytes;
	};
};

enum Token {
	TTOK_INVALID = -1,
	
	TTOK_EOS,
	TTOK_TEXT,
	TTOK_OPEN_EXPR,
	TTOK_CLOSE_EXPR,
	TTOK_OPEN_DIRECTIVE,
	TTOK_CLOSE_DIRECTIVE,
	TTOK_OPEN_PAREN,
	TTOK_CLOSE_PAREN,
	TTOK_COMMA,
	TTOK_IDENTIFIER,
	TTOK_INTEGER,
	TTOK_FLOAT,
	TTOK_STRING,
	TTOK_EQUAL,
	TTOK_NOT_EQUAL,
	TTOK_LESS,
	TTOK_LESS_EQUAL,
	TTOK_GREATER,
	TTOK_GREATER_EQUAL,
	TTOK_PLUS,
	TTOK_MINUS,
	TTOK_TIMES,
	TTOK_DIVIDE,
	TTOK_MODULO,
	TTOK_PIPE,
	TTOK_CONCATENATE,

	TTOK_KEYWORD_FIRST,
	TTOK_IF = TTOK_KEYWORD_FIRST,
	TTOK_ENDIF,
	TTOK_ELSE,
	TTOK_ELSEIF,
	TTOK_INCLUDE,
	TTOK_AND,
	TTOK_OR,
	TTOK_NOT,

	TTOK_ABS,
	TTOK_STRIP,
	TTOK_PRETTY,
	TTOK_PRECISION,
	TTOK_ENCODE,
	TTOK_DECODE,
	TTOK_BYTES,

	TTOK_KEYWORD_LAST,
	
	NUM_KEYWORDS = TTOK_KEYWORD_LAST - TTOK_KEYWORD_FIRST
};

struct CompilerState {
	const char *input;
	unsigned pos;
	unsigned length;
	int token;
	unsigned token_start;
	unsigned token_length;
	union {
		int integer;
		double real;
		uint64_t key;
	} token_value;
	bool in_directive;
	bool in_argument_list;
	bool first_argument;
	bool trim;
	unsigned text_length;
};

const unsigned RENDER_STACK_SIZE = 32;

struct RenderState {
	char *buffer;
	unsigned buffer_size;
	unsigned required;
	unsigned written;
	TemplateVariable stack[RENDER_STACK_SIZE];
	unsigned stack_top;
	unsigned pos;
	bool buffer_owned;
};

const char * const KEYWORD_STRINGS[NUM_KEYWORDS] = {
	"if",
	"endif",
	"else",
	"elseif",
	"include",
	"and",
	"or",
	"not",
	"abs",
	"strip",
	"pretty",
	"precision",
	"encode",
	"decode",
	"bytes"
};

TemplateVariable TVAR_NULL = { TVT_NULL, 0 };

/*
 * Helpers
 */

inline int round_signed(double n)
{
	return n >= 0.0 ? int(n + 0.5) : int(n - 0.5);
}

/* MurmurHash3 by Austin Appleby. */
static uint64_t murmur3_64(const void *key, const int len, unsigned seed = 0)
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

/* Finds the keyword token for a string. */
static Token match_keyword(const char *s, unsigned slen)
{
	for (unsigned i = 0; i < NUM_KEYWORDS; ++i) {
		if (slen == strlen(KEYWORD_STRINGS[i]) && 
			0 == memcmp(KEYWORD_STRINGS[i], s, slen))
			return (Token)(TTOK_KEYWORD_FIRST + i);
	}
	return TTOK_IDENTIFIER;
}

/* Returns the 1-based line number for a given offset in a string. */
static unsigned determine_line(const char *s, unsigned slen, unsigned offset,
	unsigned *offset_in_line = 0)
{
	unsigned i, line_number = 1, line_start = 0;
	for (i = 0; i < offset && i < slen; ++i) {
		if (s[i] == '\n') {
			line_start = i + 1;
			line_number++;
		}
	}
	if (offset_in_line != NULL)
		*offset_in_line = i - line_start;
	return line_number;
}

inline bool is_binary_arithmetic(Opcode opcode)
{
	return opcode >= TAOP_ADD && opcode <= TAOP_MODULO;
}

inline bool is_binary_logical(Opcode opcode)
{
	return opcode >= TAOP_AND && opcode <= TAOP_OR;
}

inline bool is_relational(Opcode opcode)
{
	return opcode >= TAOP_EQUAL && opcode <= TAOP_GREATER;
}

inline bool is_binary_op(Opcode opcode)
{
	return is_binary_arithmetic(opcode) || is_binary_logical(opcode) ||
		is_relational(opcode) || opcode == TAOP_CONCATENATE;
}

inline bool is_unary_op(Opcode opcode)
{
	return opcode == TAOP_NOT || opcode == TAOP_NEGATE || 
		(opcode >= TAOP_FILTER_ABS && opcode <= TAOP_FILTER_BYTES);
}

/*
 * Template Variables
 */

/* Copies a template variable. */
static TemplateVariable tv_dup(const TemplateVariable *tv)
{
	TemplateVariable result = *tv;
	if (result.type == TVT_STRING) {
		unsigned capacity = tv->string.length + 1;
		result.string.data = new char[capacity];
		memcpy(result.string.data, tv->string.data, capacity);
		result.string.capacity = capacity;
	}
	return result;
}

/* Deallocates string data. */
static void tv_clear_string(TemplateVariable *tv)
{
	if (tv->type == TVT_STRING) {
		if (tv->string.data != NULL)
			delete [] tv->string.data;
		tv->string.data = NULL;
		tv->string.capacity = 0;
		tv->string.length = 0;
	}
}

/* True if two variables share the same string buffer. */
static bool tv_is_alias(const TemplateVariable *a, const TemplateVariable *b)
{
	return a->type == TVT_STRING && b->type == TVT_STRING && 
		a->string.data == b->string.data;
}

/* Frees memory used by a template variable. */
static void tv_deinit(TemplateVariable *tv)
{
	tv_clear_string(tv);
}

/* Sets a template variable to a null value. */
static void tv_set_null(TemplateVariable *tv)
{
	tv_clear_string(tv);
	tv->type = TVT_NULL;
	tv->integer.value = 0;
}

/* Sets a template variable to a string value. The variable takes ownership
 * of the buffer, which must be null terminated. */
static void tv_set_string_buffer(TemplateVariable *tv, 
	char *buffer, unsigned length)
{
	tv_clear_string(tv);
	tv->type = TVT_STRING;
	tv->string.data = buffer;
	tv->string.length = length;
	tv->string.capacity = length;
}

/* Reallocates a string variable's buffer to at least the specified length and
 * clears the string. */
static void tv_allocate_string(TemplateVariable *tv, unsigned length)
{
	if (tv->type != TVT_STRING) {
		tv->type = TVT_STRING;
		tv->string.data = NULL;
		tv->string.capacity = 0;
	}
	if (length + 1 > tv->string.capacity) {
		if (tv->string.data != NULL)
			delete [] tv->string.data;
		tv->string.capacity = (tv->string.capacity == 0) ? length + 1 :
			((2 * (length + 1)) + 15) & unsigned(-16);
		tv->string.data = new char[tv->string.capacity];
	}
	tv->string.length = 0;
}

/* Sets a template variable to a string value, copying the source string. */
static void tv_set_string(TemplateVariable *tv, const char *s, unsigned slen)
{
	tv_allocate_string(tv, slen);
	memcpy(tv->string.data, s, slen);
	tv->string.data[slen] = '\0';
	tv->string.length = slen;
}

static void tv_set_integer(TemplateVariable *tv, int value)
{
	tv_clear_string(tv);
	tv->type = TVT_INTEGER;
	tv->integer.value = value;
}

static void tv_set_boolean(TemplateVariable *tv, bool value)
{
	tv_clear_string(tv);
	tv->type = TVT_BOOLEAN;
	tv->integer.value = value;
}

static void tv_set_float(TemplateVariable *tv, double value, 
	int precision = FLOAT_DISPLAY_PRECISION)
{
	tv_clear_string(tv);
	tv->type = TVT_FLOAT;
	tv->real.value = value;
	tv->real.precision = precision;
}

static int tv_result_precision(const TemplateVariable *a, 
	const TemplateVariable *b)
{
	if (a->type == TVT_FLOAT) {
		if (b->type == TVT_FLOAT)
			return std::min(a->real.precision, b->real.precision);
		return a->real.precision;
	} else if (b->type == TVT_FLOAT) {
		return b->real.precision;
	}
	return -1;
}

static void tv_set_precision(TemplateVariable *tv, int precision)
{
	if (tv->type == TVT_FLOAT)
		tv->real.precision = precision;
}

static bool tv_is_true(const TemplateVariable *tv)
{
	if (tv->type == TVT_NULL) {
		return false;
	} else if (tv->type == TVT_INTEGER || tv->type == TVT_BOOLEAN) {
		return tv->integer.value != 0;
	} else if (tv->type == TVT_FLOAT) {
		return tv->real.value != 0.0;
	} else if (tv->type == TVT_STRING) {
		if (tv->string.length == 0)
			return false;
		char *end = NULL;
		double value = strtod(tv->string.data, &end);
		if (end == tv->string.data + tv->string.length && 
			fabs(value) < DBL_EPSILON)
			return false;
		return true;
	}
	return false;
}

/* The result of a comparison between two template variables. */
enum {
	TVCMP_LESS      = -1,
	TVCMP_EQUAL     =  0,
	TVCMP_GREATER   =  1,
	TVCMP_UNDEFINED = INT_MAX
};

const double DEFAULT_COMPARE_EPSILON = 1e-4;

/* Performs a three way comparison between a pair of template variables. */
static int tv_compare(const TemplateVariable *a, const TemplateVariable *b,
	double epsilon = DEFAULT_COMPARE_EPSILON)
{
	if (a->type != b->type)
		return TVCMP_UNDEFINED;
	if (a->type == TVT_INTEGER || a->type == TVT_BOOLEAN) {
		if (a->integer.value < b->integer.value) return TVCMP_LESS;
		if (a->integer.value > b->integer.value) return TVCMP_GREATER;
		return TVCMP_EQUAL;
	}
	if (a->type == TVT_FLOAT) {
		double delta = a->real.value - b->real.value;
		if (fabs(delta) < epsilon) return TVCMP_EQUAL;
		return delta < 0.0 ? TVCMP_LESS : TVCMP_GREATER;
	}
	if (a->type == TVT_STRING)
		return strcmp(a->string.data, b->string.data);
	return TVCMP_UNDEFINED;
}

/* Performs a relational operation, returning the result as a boolean or null
 * template variable. */
static TemplateVariable tv_relational(const TemplateVariable *a, 
	const TemplateVariable *b, Opcode opcode)
{
	/* Perform the comparison. If the result is undefined, return a null. */
	int cr = tv_compare(a, b);
	TemplateVariable vr = { TVT_NULL, 0 };
	if (cr == TVCMP_UNDEFINED)
		return vr;

	/* Convert the three-way comparison result into a boolean. */
	vr.type = TVT_BOOLEAN;
	switch (opcode) {
		case TAOP_LESS:          vr.integer.value = (cr  < 0); break;
		case TAOP_LESS_EQUAL:    vr.integer.value = (cr <= 0); break;
		case TAOP_GREATER:       vr.integer.value = (cr  > 0); break;
		case TAOP_GREATER_EQUAL: vr.integer.value = (cr >= 0); break;		
		case TAOP_EQUAL:         vr.integer.value = (cr == 0); break;
		case TAOP_NOT_EQUAL:     vr.integer.value = (cr != 0); break;
	}
	return vr;
}

/* Negates a variable in place. */
static void tv_negate(TemplateVariable *tv)
{
	if (tv->type == TVT_INTEGER)
		tv->integer.value = -tv->integer.value;
	else if (tv->type == TVT_FLOAT)
		tv->real.value = -tv->real.value;
}

/* Sets a template variable to its absolute value. */
static void tv_abs(TemplateVariable *tv)
{
	if (tv->type == TVT_INTEGER)
		tv->integer.value = abs(tv->integer.value);
	else if (tv->type == TVT_FLOAT)
		tv->real.value = fabs(tv->real.value);
}

/* Attempts to interpret a template variable as an integer. Returns true if 
 * successful. */
static bool tv_integer_value(const TemplateVariable *tv, int *result)
{
	if (tv->type == TVT_INTEGER || tv->type == TVT_BOOLEAN) {
		*result = tv->integer.value;
		return true;
	} else if (tv->type == TVT_FLOAT) {
		*result = round_signed(tv->real.value);
		return true;
	} else if (tv->type == TVT_STRING) {
		const char *end = NULL;
		int value = strtol(tv->string.data, (char **)&end, 10);
		if (end == tv->string.data + tv->string.length) {
			*result = value;
			return true;
		}
	}
	return false;
}

/* Attempts to interpret a template variable as a double. Returns true if 
 * successful. */
static bool tv_float_value(const TemplateVariable *tv, double *result)
{
	if (tv->type == TVT_FLOAT) {
		*result = tv->real.value;
		return true;
	}
	if (tv->type == TVT_INTEGER || tv->type == TVT_BOOLEAN) {
		*result = (double)tv->integer.value;
		return true;
	}
	if (tv->type == TVT_STRING) {
		char *terminator = NULL;
		double value = strtod(tv->string.data, &terminator);
		if (*terminator == '\0' || isspace(*terminator)) {
			*result = value;
			return true;
		}
	}
	return false;
}

/* Performs a logical "not" on a template variable in place. */
static void tv_not(TemplateVariable *tv)
{
	if (tv->type == TVT_BOOLEAN || tv->type == TVT_INTEGER) {
		tv->type = TVT_BOOLEAN;
		tv->integer.value = (tv->integer.value == 0);
	} else {
		tv_set_boolean(tv, !tv_is_true(tv));
	}
}

/* Performs a binary arithmetic operation on two template variables. */
static TemplateVariable tv_arithmetic(const TemplateVariable *a, 
	const TemplateVariable *b, Opcode op)
{
	/* Promote both operands to floats. */
	double va, vb;
	TemplateVariable vr = { TVT_NULL, 0 };
	if (!tv_float_value(a, &va) || !tv_float_value(b, &vb))
		return vr;

	/* Calculate the result. */
	double result = 0.0;
	switch (op) {
		case TAOP_ADD:
			result = va + vb;
			break;
		case TAOP_SUBTRACT:
			result = va - vb;
			break;
		case TAOP_MULTIPLY:
			result = va * vb;
			break;
		case TAOP_DIVIDE:
			result = fabs(vb) > DBL_EPSILON ? va / vb : 0.0;
			break;
		case TAOP_MODULO:
			result = fmod(va, vb);
			break;
		default:
			assert(false);
			break;
	}

	/* If both operands are integers, the result is an integer. */
	if ((a->type == TVT_INTEGER || a->type == TVT_BOOLEAN) && 
	    (b->type == TVT_INTEGER || b->type == TVT_BOOLEAN)) {
		vr.type = TVT_INTEGER;
		vr.integer.value = round_signed(result);
	} else {
		vr.type = TVT_FLOAT;
		vr.real.value = result;
		vr.real.precision = tv_result_precision(a, b);
	}
	return vr;
}

/* Performs a binary logical operation on two template variables. */
static TemplateVariable tv_logical(const TemplateVariable *a, 
	const TemplateVariable *b, Opcode op)
{
	bool va = tv_is_true(a);
	bool vb = tv_is_true(b);
	bool result = false;
	switch (op) {
		case TAOP_AND:
			result = va && vb;
			break;
		case TAOP_OR:
			result = va || vb;
			break;
		default:
			assert(false);
			break;
	}
	TemplateVariable vr = { TVT_BOOLEAN, result };
	return vr;
}

/* Converts a variable to an integer in place. Returns false if no coversion
 * is possible. */
static bool tv_cast_integer(TemplateVariable *tv)
{
	if (tv_integer_value(tv, &tv->integer.value)) {
		tv->type = TVT_INTEGER;
		return true;
	}
	return false;
}

/* Converts a variable to an float in place. Returns false if no coversion
 * is possible. */
static bool tv_cast_float(TemplateVariable *tv)
{
	if (tv_float_value(tv, &tv->real.value)) {
		tv->type = TVT_FLOAT;
		tv->real.precision = -1;
		return true;
	}
	return false;
}

/* Converts a variable to a string in place. Always succeeds. */
static bool tv_cast_string(TemplateVariable *tv)
{
	if (tv->type == TVT_STRING)
		return true;
	if (tv->type == TVT_NULL) {
		tv_set_string(tv, "", 0);
	} else if (tv->type == TVT_INTEGER) {
		char buf[64];
		int length = sprintf(buf, "%d", tv->integer.value);
		tv_set_string(tv, buf, length);
	} else if (tv->type == TVT_BOOLEAN) {
		const char *s = tv->integer.value ? "true" : "false";
		tv_set_string(tv, s, strlen(s));
	} else if (tv->type == TVT_FLOAT) {
		char buf[64];
		int length = 0;
		if (tv->real.precision < 0) 
			length = sprintf(buf, "%f", tv->real.value);
		else
			length = sprintf(buf, "%.*f", tv->real.precision, tv->real.value);
		tv_set_string(tv, buf, length);
	}
	return true;
}

/* Converts two template variables to strings an concatenates them. */
static TemplateVariable tv_concatenate(const TemplateVariable *a, 
	const TemplateVariable *b)
{
	/* Convert the operands to strings if required. */
	TemplateVariable sa = *a, sb = *b;
	tv_cast_string(&sa);
	tv_cast_string(&sb);

	/* Concatenate the strings. */
	TemplateVariable result = { TVT_STRING, 0 };
	unsigned length = sa.string.length + sb.string.length;
	result.string.data = new char[length + 1];
	result.string.capacity = length + 1;
	result.string.length = length;
	memcpy(result.string.data, sa.string.data, sa.string.length);
	memcpy(result.string.data + sa.string.length, sb.string.data, 
		sb.string.length + 1);

	/* Free any temporary data we used. */
	if (!tv_is_alias(a, &sa))
		tv_clear_string(&sa);
	if (!tv_is_alias(b, &sb))
		tv_clear_string(&sb);
	return result;
}

/* Removes the white space from both ends of a variable. */
static void tv_strip(TemplateVariable *tv)
{
	if (tv->type != TVT_STRING || tv->string.length == 0)
		return;
	const char *p, *q;
	for (p = tv->string.data; isspace(*p); ++p);
	for (q = tv->string.data + tv->string.length; q > p && isspace(q[-1]); --q);
	memmove(tv->string.data, p, q - p);
	tv->string.length = q - p;
	tv->string.data[tv->string.length] = '\0';
}

/* Formats a number, inserting commas between thousand-groups. */
static void tv_pretty(TemplateVariable *tv, int precision)
{
	double value = 0.0;
	if (!tv_float_value(tv, &value))
		return;

	/* If precision was not specified as a parameter, use the value's 
	 * precision. */
	if (precision < 0) {
		if (tv->type == TVT_INTEGER)
			precision = 0;
		else if (tv->type == TVT_FLOAT)
			precision = tv->real.precision;
	}

	/* Print the number. */
	char buffer[256];
	unsigned length;
	if (precision < 0)
		length = sprintf(buffer, "%f", value);
	else
		length = sprintf(buffer, "%.*f", precision, value);

	/* Calculate the number of delimiters to insert and, from that, the length 
	 * of the output. */
	unsigned sign = (buffer[0] == '-');
	unsigned whole_part = strcspn(buffer, ".e");
	unsigned whole_digits = whole_part - sign;
	unsigned num_delimiters = (whole_digits - 1) / 3;
	unsigned out_length = length + num_delimiters;

	/* Calculate the number of digits in the first group, which is considered
	 * to include the sign if present. */
	unsigned group = whole_digits % 3;
	if (group == 0) 
		group = 3;
	group += sign;

	/* Copy the whole part into the output string, inserting delimiters. */
	tv_allocate_string(tv, out_length);
	unsigned i = 0, j = 0;
	if (i != whole_part) {
		for (;;) {
			tv->string.data[j++] = buffer[i++];
			if (i == whole_part)
				break;
			if (--group == 0) {
				tv->string.data[j++] = ',';
				group = 3;
			}
		}
	}

	/* Copy everything after the whole part verbatim, including the 
	 * terminator. */
	memcpy(tv->string.data + j, buffer + whole_part, length - whole_part + 1);
	tv->string.length = out_length;
}

/* URL encodes a variable in place. */
static void tv_url_encode(TemplateVariable *tv)
{
	char buffer[1024];
	unsigned encoded_length;
	tv_cast_string(tv);
	char *encoded = urlcache::url_encode(tv->string.data, tv->string.length, 
		buffer, sizeof(buffer), &encoded_length, URLPARSE_HEAP);
	if (encoded == buffer) {
		tv_set_string(tv, buffer, encoded_length);
	} else {
		tv_set_string_buffer(tv, encoded, encoded_length); 
	}
}

/* Performs URL decoding on a template variable in place. */
static void tv_url_decode(TemplateVariable *tv)
{
	tv_cast_string(tv);
	tv->string.length = urlcache::url_decode(tv->string.data, 
		tv->string.length + 1, URLPARSE_DECODE_PLUS_TO_SPACE) - 1;
}

/* Converts a template variable in place to a comma delimited list of decimal
 * byte values, least significant byte first. */
static void tv_byte_list(TemplateVariable *tv, unsigned count, char delimiter)
{
	unsigned value = 0;
	if (!tv_integer_value(tv, (int *)&value)) {
		tv_set_null(tv);
		return;
	}
	char buffer[512];
	if (count > 4)
		count = 4;
	unsigned length = 0;
	for (unsigned i = 0; i < count; ++i) {
		if (i != 0) {
			buffer[length++] = delimiter;
			buffer[length++] = ' ';
		}
		length += sprintf(buffer + length, "%u", (value & 0xFF));
		value >>= 8;
	}
	tv_set_string(tv, buffer, length);
}

/* Performs a binary operation on two template variables. */
static TemplateVariable tv_binary_op(const TemplateVariable *a, 
	const TemplateVariable *b, Opcode opcode)
{
	if (is_binary_arithmetic(opcode))
		return tv_arithmetic(a, b, opcode);
	if (is_binary_logical(opcode))
		return tv_logical(a, b, opcode);
	if (is_relational(opcode))
		return tv_relational(a, b, opcode);
	if (opcode == TAOP_CONCATENATE)
		return tv_concatenate(a, b);
	assert(false);
	return TemplateVariable();
}

/* Performs a unary operation on a template variable in place. */
static void tv_unary_op(TemplateVariable *tv, const TemplateOperation *op)
{
	switch (op->opcode) {
		case TAOP_NOT:
			tv_not(tv);
			break;
		case TAOP_NEGATE:
			tv_negate(tv);
			break;
		case TAOP_FILTER_ABS:
			tv_abs(tv);
			break;
		case TAOP_FILTER_STRIP:
			tv_strip(tv);
			break;
		case TAOP_FILTER_PRECISION:
			tv_set_precision(tv, op->precision.precision);
			break;
		case TAOP_FILTER_PRETTY:
			tv_pretty(tv, op->pretty.precision);
			break;
		case TAOP_FILTER_ENCODE:
			tv_url_encode(tv);
			break;
		case TAOP_FILTER_DECODE:
			tv_url_decode(tv);
			break;
		case TAOP_FILTER_BYTES:
			tv_byte_list(tv, op->bytes.count, op->bytes.delimiter);
			break;
		default:
			assert(false);
	}
} 

/*
 * TemplateProcessor
 */

unsigned TemplateProcessor::notify_callback(UrlHandle handle, 
	UrlNotification type, UrlKey key, TemplateProcessor *processor, 
	CompiledTemplate *ct, UrlFetchState fetch_state)
{
	fetch_state; key;
	if (type == URL_NOTIFY_EVICT) {
		/* Release the handle's reference to the CT. */
		processor->destroy(ct);
		processor->cache->destroy_handle(handle);
	} else if (type == URL_QUERY_EVICT) {
		return ct->memory_usage();
	}
	return 0;
}

TemplateProcessor::TemplateProcessor(UrlCache *cache) :
	cache(cache),
	notify_sink_id(INVALID_NOTIFY_SINK_ID),
	inclusion_depth(0)
{
	if (cache != NULL) {
		notify_sink_id = cache->add_notify_sink(
			(NotifyCallback)&notify_callback, this);
	}
}

TemplateProcessor::~TemplateProcessor()
{
	for (unsigned i = 0; i < VSCOPE_COUNT; ++i)
		clear_scope((Scope)i);
	if (cache != NULL)
		cache->remove_notify_sink(notify_sink_id);
}

CompiledTemplate *TemplateProcessor::create(const char *source, 
	int source_length)
{
	if (source_length < 0)
		source_length = (int)strlen(source);
	CompiledTemplate *ct = new CompiledTemplate(this);
	ct->compile(source, source_length);
	return ct;
}

CompiledTemplate *TemplateProcessor::create_from_url(const char *url, 
	int length)
{
	if (cache == NULL)
		return NULL;
	/* FIXME (TJM): URL_FLAG_KEEP_URL for debugging here. */
	UrlHandle handle = cache->create_handle(url, length, URLP_NORMAL,
		DEFAULT_TTL_SECS, NULL, notify_sink_id, URL_FLAG_REUSE_SINK_HANDLE | URL_FLAG_KEEP_URL);
	return create_from_url_internal(handle);
}

CompiledTemplate * TemplateProcessor::create_from_url(UrlKey key)
{
	if (cache == NULL)
		return NULL;
	UrlHandle handle = cache->create_handle(key, URLP_NORMAL,
		DEFAULT_TTL_SECS, NULL, notify_sink_id, URL_FLAG_REUSE_SINK_HANDLE | URL_FLAG_KEEP_URL);
	return create_from_url_internal(handle);
}

CompiledTemplate * TemplateProcessor::create_from_url_internal(UrlHandle handle)
{
	/* Do we already have a compiled template? */
	CompiledTemplate *ct = (CompiledTemplate *)cache->user_data(handle);
	if (ct != NULL) {
		ct->use_count++;
		return ct;
	}

	/* If data is available for the URL, make a new compiled template and
	 * store it in the handle. */
	const char *url_data;
	unsigned data_size;
	url_data = (const char *)cache->lock(handle, &data_size);
	if (url_data != NULL) {
		ct = new CompiledTemplate(this);
		ct->compile(url_data, data_size);
		cache->set_user_data(handle, ct);
		ct->use_count++; /* The handle itself takes a reference. */
		cache->unlock(handle);
	} else {
		cache->destroy_handle(handle);
	}
	return ct;
}

void TemplateProcessor::destroy(CompiledTemplate *ct)
{
	if (ct == NULL)
		return;
	assert(ct->use_count > 0);
	if (--ct->use_count == 0)
		delete ct;
}

CompiledTemplate *TemplateProcessor::copy(const CompiledTemplate *ct)
{
	CompiledTemplate *result = const_cast<CompiledTemplate *>(ct);
	result->use_count++;
	return result;
}

void TemplateProcessor::clear_scope(Scope scope)
{
	VariableTable *table = tables + scope;
	VariableTable::iterator iter = table->begin();
	for (iter = table->begin(); iter != table->end(); ++iter)
		tv_clear_string(&iter->second);
	table->clear();
}

TemplateVariable *TemplateProcessor::get_or_create_variable(
	Scope scope, const char *name)
{
	uint64_t key = murmur3_64(name, strlen(name));
	return &tables[scope][key];
}

void TemplateProcessor::set_string(Scope scope, const char *name, 
	const char *value, int length)
{
	TemplateVariable *tv = get_or_create_variable(scope, name);
	if (value != NULL)
		tv_set_string(tv, value, length < 0 ? strlen(value) : length);
	else
		tv_set_null(tv);
}

void TemplateProcessor::set_integer(Scope scope, const char *name, int value)
{
	TemplateVariable *tv = get_or_create_variable(scope, name);
	tv_set_integer(tv, value);
}

void TemplateProcessor::set_float(Scope scope, const char *name, double value,
	int precision)
{
	TemplateVariable *tv = get_or_create_variable(scope, name);
	tv_set_float(tv, value, precision);
}

void TemplateProcessor::set_boolean(Scope scope, const char *name, bool value)
{
	TemplateVariable *tv = get_or_create_variable(scope, name);
	tv_set_boolean(tv, value);
}

UrlCache *TemplateProcessor::url_cache(void) const
{
	return cache;
}

const TemplateVariable *TemplateProcessor::look_up(uint64_t key, unsigned chain)
{
	for (unsigned i = VSCOPE_COUNT - 1; i + 1 != 0; --i) {
		if ((chain & (1 << i)) == 0)
			continue;
		const VariableTable *table = tables + i;
		VariableTable::const_iterator iter = table->find(key);
		if (iter != table->end())
			return &iter->second;
	}
	return NULL;
}

/*
 * CompiledTemplate
 */

int CompiledTemplate::set_error(CompilerState *cs, int code, ...)
{
	va_list args;
	va_start(args, code);
	int rc = format_error(code, args, cs, NULL);
	va_end(args);
	return rc;
}


int CompiledTemplate::set_error(RenderState *rs, int code, ...) const
{
	/* TE_NOT_READY and TE_TRUNCATED are special in that rendering continues 
	 * even after they occur. Thus, we don't want to format an error message and 
	 * clear the program. */
	if (code == TE_NOT_READY || code == TE_TRUNCATED)
		return code;
	va_list args;
	va_start(args, code);
	int rc = const_cast<CompiledTemplate *>(this)->format_error(
		code, args, NULL, rs);
	va_end(args);
	return rc;
}

int CompiledTemplate::format_error(int code, va_list args, 
	CompilerState *cs, RenderState *rs)
{
	if (code == TE_OK)
		return code;

	/* Only keep the first error. */
	if (cs != NULL && compiler_error < 0)
		return compiler_error;

	const char *fmt = NULL;
	switch (code) {
		case TE_ERROR:
			fmt = "template ill-formed";
			break;
		case TE_UNEXPECTED_TOKEN:
			fmt = "expected %s";
			break;
		case TE_UNTERMINATED_DIRECTIVE:
			fmt = "unterminated directive";
			break;
		case TE_UNTERMINATED_STRING:
			fmt = "unterminated string literal";
			break;
		case TE_INVALID_INPUT:
			fmt = "invalid input";
			break;
		case TE_TOO_MANY_CLAUSES:
			fmt = "conditional structure exceeds maximum number of clauses";
			break;
		case TE_CYCLIC_INCLUDE:
			fmt = "reciprocal inclusion detected";
			break;
		case TE_STACK_UNDERFLOW:
			fmt = "stack underflow";
			break;
		case TE_STACK_OVERFLOW:
			fmt = "stack overflow";
			break;
		case TE_MISSING_PAREN:
			fmt = "missing closing parenthesis";
			break;
		case TE_INVALID_ARGUMENT:
			fmt = "invalid argument for filter \"%s\"; expected %s";
			break;
		case TE_OUT_OF_BOUNDS:
			fmt = "argument for %s filter %s out of bounds";
			break;
	}

	char buf[1024];
	int length = 0;

	/* If this is a compiler message, add a line number/context prefix. */
	if (cs != NULL) {	
		const char *context = cs->input + cs->token_start;
		unsigned context_length = cs->length - cs->token_start;
		if (context_length > 16)
			context_length = 16;
		unsigned offset_in_line, line_number = determine_line(cs->input, 
			cs->length, cs->pos, &offset_in_line);
		length = sprintf(buf, "template compiler: "
			"line %u at offset %u near \"%.*s\": ", 
			line_number, offset_in_line, context_length, context);
	} else if (rs != NULL) {
		length = sprintf(buf, "template program: ");
	}

	/* Format a message based on the error code. */
	length += vsnprintf(buf + length, sizeof(buf) - length, fmt, args);
	if (length < 0 || length >= sizeof(buf))
		length = sizeof(buf) - 1;
	buf[length] = '\0';

	/* If this is a compiler error, delete any half-constructed program and
	 * replace the heap with the error string. If it's a rendering error, 
	 * write the error string to the render output buffer. */
	if (cs != NULL) {
		reallocate_heap(length + 1);
		memcpy(heap, buf, length + 1);
		compiler_error = (char)code;
		cs->token = code;
	} else if (rs != NULL) {
		reallocate_render_buffer(rs, length + 1);
		memcpy(rs->buffer, buf, length);
		rs->written = length;
		rs->required = length;
	}
	return code;
}

/* Reads one token from the input, returning the token, or a negative error
 * code in case of failure. Continues to return TTOK_EOS when the end of the
 * input has been reached. */
int CompiledTemplate::next_token(CompilerState *cs)
{
	if (cs->pos == cs->length) {
		cs->token = TTOK_EOS;
		return TTOK_EOS;
	}
	if (cs->in_directive) {
		char ch = cs->input[cs->pos];
		char ch2 = cs->pos + 1 != cs->length ? cs->input[cs->pos + 1] : '\0';
		char ch3 = cs->pos + 2 != cs->length ? cs->input[cs->pos + 2] : '\0';
		cs->token_start = cs->pos;
		if (ch == '{' && (ch2 == '{' || ch2 == '%')) {
			cs->token = (ch2 == '%') ? TTOK_OPEN_DIRECTIVE : TTOK_OPEN_EXPR;
			cs->pos += 2;
			cs->trim = false;
			if (ch3 == '+' || ch3 == '-') {
				cs->trim = (ch3 == '-');
				cs->pos++;
			}
		} else if ((ch2 == '}' && (ch == '%' || ch == '}')) ||
			((ch == '+' || ch == '-') && (ch2 == '%' || ch2 == '}') && ch3 == '}')) {
			cs->trim = false;
			if (ch == '+' || ch == '-') {
				cs->trim = (ch == '-');
				cs->pos++;
				ch = ch2;
			}
			cs->pos += 2;
			cs->token = (ch == '%') ? TTOK_CLOSE_DIRECTIVE : TTOK_CLOSE_EXPR;
			cs->in_directive = false;
			return cs->token;
		} else if (ch == '"') {
			if (++cs->pos == cs->length)
				return set_error(cs, TE_UNTERMINATED_STRING);
			cs->token_start = cs->pos;
			while (cs->pos != cs->length && cs->input[cs->pos] != '"')
				++cs->pos;
			if (cs->pos == cs->length)
				return set_error(cs, TE_UNTERMINATED_STRING);
			cs->token_length = cs->pos - cs->token_start;
			++cs->pos;
			cs->token = TTOK_STRING;
		} else if (ch == '(') {
			cs->token = TTOK_OPEN_PAREN;
			cs->pos++;
		} else if (ch == ')') {
			cs->token = TTOK_CLOSE_PAREN;
			cs->pos++;
		} else if (ch == ',') {
			cs->token = TTOK_COMMA;
			cs->pos++;
		} else if (ch == '=' && ch2 == '=') {
			cs->token = TTOK_EQUAL;
			cs->pos += 2;
		} else if (ch == '!' && ch2 == '=') {
			cs->token = TTOK_NOT_EQUAL;
			cs->pos += 2;
		} else if (ch == '<' || ch == '>') {
			cs->token = (ch == '<') ? TTOK_LESS : TTOK_GREATER;
			cs->pos++;
			if (ch2 == '=') {
				cs->token++;
				cs->pos++;
			}
		} else if (ch == '+') {
			cs->token = TTOK_PLUS;
			cs->pos++;
		} else if (ch == '-' && !isdigit(ch2)) {
			cs->token = TTOK_MINUS;
			cs->pos++;
		} else if (ch == '*') {
			cs->token = TTOK_TIMES;
			cs->pos++;
		} else if (ch == '/') {
			cs->token = TTOK_DIVIDE;
			cs->pos++;
		} else if (ch == '%') {
			cs->token = TTOK_MODULO;
			cs->pos++;
		} else if (ch == '|') {
			cs->token = TTOK_PIPE;
			cs->pos++;
		} else if (ch == '.' && ch2 == '.') {
			cs->token = TTOK_CONCATENATE;
			cs->pos += 2;
		} else if (isdigit(ch) || (ch == '-' && isdigit(ch2))) {
			char buf[64], *end = NULL;
			bool is_float = false;
			cs->token_length = 0;
			do {
				buf[cs->token_length++] = ch;
				if (cs->token_length == sizeof(buf))
					return set_error(cs, TE_INVALID_INPUT);
				if (++cs->pos == cs->length)
					break;
				ch = cs->input[cs->pos];
				is_float |= (ch == '.' || ch == 'e');
			} while (isdigit(ch) || ch == '.' || ch == 'e');
			buf[cs->token_length] = '\0';
			if (is_float) {
				cs->token = TTOK_FLOAT;
				cs->token_value.real = strtod(buf, &end);
			} else {
				cs->token = TTOK_INTEGER;
				cs->token_value.integer = strtol(buf, &end, 10);
			}
			if (*end != '\0')
				return set_error(cs, TE_INVALID_INPUT);
		} else if (isalpha(ch) || ch == '_') {
			while (++cs->pos != cs->length) {
				ch = cs->input[cs->pos];
				if (!(isalnum(ch) || ch == '_' || ch == '-'))
					break;
			}
			cs->token_length = cs->pos - cs->token_start;
			cs->token_value.key = murmur3_64(cs->input + cs->token_start, cs->token_length);
			cs->token = match_keyword(cs->input + cs->token_start, cs->token_length);
		} else {
			return set_error(cs, TE_INVALID_INPUT);
		}
		while (cs->pos != cs->length && isspace(cs->input[cs->pos]))
			++cs->pos;
	} else {
		for (cs->token_start = cs->pos; cs->pos != cs->length; ++cs->pos) {
			if (cs->input[cs->pos] != '{' || cs->pos + 1 == cs->length)
				continue;
			char ch = cs->input[cs->pos + 1];
			if (ch == '{' || ch == '%') {
				cs->in_directive = true;
				if (cs->pos != cs->token_start)
					break;
				return next_token(cs);
			}
		}
		cs->token_length = cs->pos - cs->token_start;
		cs->token = cs->token_length != 0 ? TTOK_TEXT : TTOK_EOS;
	}
	return cs->token;
}

CompiledTemplate::CompiledTemplate(TemplateProcessor *processor) :
	processor(processor),
	heap(NULL),
	heap_size(0),
	program(NULL),
	num_operations(0),
	compiler_error((char)TE_OK),
	use_count(1)
{ }

CompiledTemplate::~CompiledTemplate()
{
	assert(use_count == 0);
	clear_heap();
}

TemplateOperation *CompiledTemplate::add_operation(Opcode opcode)
{
	TemplateOperation *op = NULL;
	if (program != NULL) {
		op = program + num_operations;
		op->opcode = opcode;
	}
	num_operations++;
	return op;
}

int CompiledTemplate::parse_conditional_directive(CompilerState *cs)
{
	static const unsigned MAX_CLAUSES = 256;

	/* Read the clauses of the conditional until we hit an "endif". */
	TemplateOperation *escape_branches[MAX_CLAUSES], *condition_branch = NULL;
	unsigned num_clauses = 0;
	int rc = TE_OK;
	for (;;) {
		/* Consume the keyword. */
		int keyword = cs->token;
		next_token(cs);
		
		/* The first clause must be an if. Others must be else, elseif or 
		 * endif. */
		if (keyword == TTOK_IF) {
			/* Only the first clause may begin with "if". */
			if (num_clauses != 0) {
				return set_error(cs, TE_UNEXPECTED_TOKEN, 
					"{% endif %} or {% else[if] %}");
			}
		} else if (keyword == TTOK_ELSE || keyword == TTOK_ELSEIF) {
			/* The first clause must begin with "if". */
			if (num_clauses == 0) {
				return set_error(cs, TE_UNEXPECTED_TOKEN, 
					"{% if <expression> %}");
			}
		} else if (keyword != TTOK_ENDIF) {
			return set_error(cs, TE_UNEXPECTED_TOKEN, "conditional directive");
		}

		/* Parse the condition. */
		bool has_condition = (keyword == TTOK_IF || keyword == TTOK_ELSEIF);
		if (has_condition && (rc = parse_expression(cs)) < 0)
			return rc;

		/* Consume "%}". */
		if (cs->token != TTOK_CLOSE_DIRECTIVE)
			return set_error(cs, TE_UNEXPECTED_TOKEN, "%}");
		next_token(cs);

		/* If this is "endif", we're done. */
		if (keyword == TTOK_ENDIF)
			break;

		/* If this is a conditional section, push a conditional branch that 
		 * jumps over ops from the text we're about to parse if the expression
		 * is false. */
		condition_branch = has_condition ? 
			add_operation(TAOP_BRANCH_IF_FALSE) : NULL;

		/* Parse the text inside the section. */
		if ((rc = parse_text(cs)) < 0)
			return rc;
		
		/* If this clause is not the last, generate an unconditional "escape" 
		 * branch that jumps over all subsequent clauses. We don't know 
		 * the target of such jumps yet, so we keep them in a list and patch in 
		 * the targets after parsing the whole construction. */
		if (cs->token != TTOK_ENDIF) {
			TemplateOperation *escape = add_operation(TAOP_BRANCH);
			if (escape != NULL) {
				escape->branch.target = unsigned(-1); /* Not yet known. */
				escape_branches[num_clauses] = escape;
			}
		}

		/* Target the conditional branch that begins this section at the first
		 * operation of the next section. */
		if (condition_branch != NULL)
			condition_branch->branch.target = num_operations;

		if (++num_clauses == MAX_CLAUSES)
			return set_error(cs, TE_TOO_MANY_CLAUSES);
	}

	/* Target escape branches at first op after the end of the whole 
	 * construction. */
	if (program != NULL) {
		for (unsigned i = 0; i < num_clauses - 1; ++i)
			escape_branches[i]->branch.target = num_operations;
	}
	return TE_OK;
}

/* Parses {% include <url> %}. */
int CompiledTemplate::parse_include_directive(CompilerState *cs)
{
	int token = next_token(cs); /* Skip the "include" keyword. */
	if (token != TTOK_STRING)
		return set_error(cs, TE_UNEXPECTED_TOKEN, "URL string");

	/* Add an insert-url OP. */
	TemplateOperation *op = add_operation(TAOP_INSERT_URL);
	if (op != NULL) {
		UrlCache *cache = processor->url_cache();
		op->url.key = cache->request(cs->input + cs->token_start, 
			cs->token_length);
	}
	
	/* Consume the closer. */
	if (next_token(cs) != TTOK_CLOSE_DIRECTIVE)
		return set_error(cs, TE_UNEXPECTED_TOKEN, "%}");
	next_token(cs);

	return TE_OK;
}

/* Tries to advance to the text argument in an argument list and verify that the
 * token encountered is of the requested type. */
bool CompiledTemplate::match_argument(CompilerState *cs, Token token, 
	bool required, const char *filter_name, const char *parameter)
{
	if (compiler_error != TE_OK)
		return false;
	if (!cs->in_argument_list || cs->token == TTOK_CLOSE_PAREN) {
		if (required)
			set_error(cs, TE_INVALID_ARGUMENT, filter_name, parameter);
		return false;
	}
	if (!cs->first_argument) {
		if (cs->token != TTOK_COMMA) {
			set_error(cs, TE_UNEXPECTED_TOKEN, ",");
			return false;
		}
		next_token(cs); /* Consume ','. */
	}
	if (cs->token != token) {
		set_error(cs, TE_INVALID_ARGUMENT, filter_name, parameter);
		return false;
	}
	cs->first_argument = false;
	return true;
}

bool CompiledTemplate::match_integer_argument(CompilerState *cs,  
	bool required, int *out_value, 
	const char *filter_name, const char *parameter,
	int min_value, int max_value)
{
	if (!match_argument(cs, TTOK_INTEGER, required, filter_name, parameter))
		return false;
	if (cs->token_value.integer < min_value || 
	    cs->token_value.integer > max_value) {
		set_error(cs, TE_OUT_OF_BOUNDS, parameter, filter_name);
		return false;
	}
	if (out_value != NULL)
		*out_value = cs->token_value.integer;
	next_token(cs);
	return true;
}

bool CompiledTemplate::match_string_argument(CompilerState *cs,  
	bool required, const char **out_text, unsigned *out_length, 
	const char *filter_name, const char *parameter,
	unsigned min_length, unsigned max_length)
{
	if (!match_argument(cs, TTOK_STRING, required, filter_name, parameter))
		return false;
	if (cs->token_length < min_length || cs->token_length > max_length) {
		set_error(cs, TE_OUT_OF_BOUNDS, parameter, filter_name);
		return false;
	}
	if (out_text != NULL)
		*out_text = cs->input + cs->token_start;
	if (out_length != NULL)
		*out_length = cs->token_length;
	next_token(cs);
	return true;
}


/* Parses a filter specification, which somewhat resembles a function call. */
int CompiledTemplate::parse_filter_specification(CompilerState *cs)
{
	TemplateOperation op;
	switch (cs->token) {
		case TTOK_ABS:
			op.opcode = TAOP_FILTER_ABS;
			break;
		case TTOK_STRIP:
			op.opcode = TAOP_FILTER_STRIP;
			break;
		case TTOK_PRETTY:
			op.opcode = TAOP_FILTER_PRETTY;
			op.pretty.precision = -1; /* Auto. */
			break;
		case TTOK_PRECISION:
			op.opcode = TAOP_FILTER_PRECISION;
			op.precision.precision = -1; /* Auto. */
			break;
		case TTOK_ENCODE:
			op.opcode = TAOP_FILTER_ENCODE;
			break;
		case TTOK_DECODE:
			op.opcode = TAOP_FILTER_DECODE;
			break;
		case TTOK_BYTES:
			op.opcode = TAOP_FILTER_BYTES;
			op.bytes.delimiter = ',';
			op.bytes.count = 4;
			break;
		default:
			return set_error(cs, TE_UNEXPECTED_TOKEN, "filter name");
	}

	/* Read the optional argument list. */
	cs->in_argument_list = false;
	cs->first_argument = true;
	if (next_token(cs) == TTOK_OPEN_PAREN) {
		cs->in_argument_list = true;
		next_token(cs); /* Consume "(". */
	}

	/* Match the arguments. */
	if (op.opcode == TAOP_FILTER_PRETTY) {
		match_integer_argument(cs, false, &op.pretty.precision,
			"pretty", "integer precision");
	} else if (op.opcode == TAOP_FILTER_PRECISION) {
		match_integer_argument(cs, false, &op.precision.precision, 
			"precision", "integer precision");
	} else if (op.opcode == TAOP_FILTER_BYTES) {
		const char *delimiter = NULL;
		match_integer_argument(cs, false, (int *)&op.bytes.count, 
			"bytes", "byte count");
		if (match_string_argument(cs, false, &delimiter, NULL, 
			"bytes", "delimiter", 1, 1))
			op.bytes.delimiter = *delimiter;
	}
	if (compiler_error != TE_OK)
		return compiler_error;

	/* Consume ")". */
	if (cs->in_argument_list) {
		if (cs->token != TTOK_CLOSE_PAREN)
			return set_error(cs, TE_UNEXPECTED_TOKEN, ")");
		next_token(cs);
	}
	if (program != NULL)
		program[num_operations] = op;
	num_operations++;
	return TE_OK;
}

/* Parses a primary expression: an identifier, literal, or parenthesized
 * subexpression. */
int CompiledTemplate::parse_primary_expression(CompilerState *cs)
{
	int rc = TE_OK;
	if (cs->token == TTOK_IDENTIFIER) {
		TemplateOperation *op = add_operation(TAOP_PUSH);
		if (op != NULL)
			op->lookup.key = cs->token_value.key;
		next_token(cs);
	} else if (cs->token == TTOK_STRING || cs->token == TTOK_INTEGER ||
		cs->token == TTOK_FLOAT) {
		TemplateOperation *op = add_operation(TAOP_PUSH_LITERAL);
		if (op != NULL) {
			switch (cs->token) {
				case TTOK_STRING:
					tv_set_string(&op->literal, cs->input + cs->token_start, 
						cs->token_length);
					break;
				case TTOK_INTEGER:
					tv_set_integer(&op->literal, cs->token_value.integer);
					break;
				case TTOK_FLOAT:
					tv_set_float(&op->literal, cs->token_value.real);
					break;
			}
		}
		next_token(cs);
	} else if (cs->token == TTOK_OPEN_PAREN) {
		next_token(cs); /* Consume "(". */
		if ((rc = parse_expression(cs)) < 0)
			return rc;
		if (cs->token != TTOK_CLOSE_PAREN)
			return set_error(cs, TE_MISSING_PAREN);
		next_token(cs); /* Consume ")". */
	} else {
		return set_error(cs, TE_UNEXPECTED_TOKEN, "identifier or literal");
	}
	return rc;
}

/* Parses a expression in one of the right-associated unary operators. */
int CompiledTemplate::parse_unary_expression(CompilerState *cs)
{
	int op_token = cs->token, rc = TE_OK;
	if (op_token == TTOK_NOT || op_token == TTOK_MINUS) {
		next_token(cs); /* Consume the operator. */
		if ((rc = parse_unary_expression(cs)) < 0)
			return rc;
		add_operation((op_token == TTOK_NOT) ? TAOP_NOT : TAOP_NEGATE);
	} else {
		rc = parse_primary_expression(cs);
	}
	return rc;
}

/* Parses a multiplication, division or modulo expression. */
int CompiledTemplate::parse_multiplicative_expression(CompilerState *cs)
{
	int rc = parse_unary_expression(cs);
	if (rc < 0)
		return rc;
	for (;;) {
		Opcode opcode;
		if (cs->token == TTOK_TIMES )
			opcode = TAOP_MULTIPLY;
		else if (cs->token == TTOK_DIVIDE)
			opcode = TAOP_DIVIDE;
		else if (cs->token == TTOK_MODULO)
			opcode = TAOP_MODULO;
		else 
			break;
		next_token(cs);
		if ((rc = parse_unary_expression(cs)) < 0)
			return rc;
		add_operation(opcode);
	}
	return TE_OK;
}

/* Parses an addition or subtraction expression. */
int CompiledTemplate::parse_additive_expression(CompilerState *cs)
{
	int rc = parse_multiplicative_expression(cs);
	if (rc < 0)
		return rc;
	for (;;) {
		Opcode opcode;
		if (cs->token == TTOK_PLUS)
			opcode = TAOP_ADD;
		else if (cs->token == TTOK_MINUS) 
			opcode = TAOP_SUBTRACT;
		else 
			break;
		next_token(cs);
		if ((rc = parse_multiplicative_expression(cs)) < 0)
			return rc;
		add_operation(opcode);
	}
	return TE_OK;
}

/* Parses a string concatenation expression. */
int CompiledTemplate::parse_concatenative_expression(CompilerState *cs)
{
	int rc = parse_additive_expression(cs);
	if (rc < 0)
		return rc;
	while (cs->token == TTOK_CONCATENATE) {
		next_token(cs);
		if ((rc = parse_additive_expression(cs)) < 0)
			return rc;
		add_operation(TAOP_CONCATENATE);
	}
	return TE_OK;
}

/* Parses a filter chain expression. */
int CompiledTemplate::parse_pipe_expression(CompilerState *cs)
{
	int rc = parse_concatenative_expression(cs);
	if (rc < 0)
		return rc;
	while (cs->token == TTOK_PIPE) {
		next_token(cs);
		if ((rc = parse_filter_specification(cs)) < 0)
			return rc;
	}
	return TE_OK;
}

/* Parses a comparison expression. */
int CompiledTemplate::parse_relational_expression(CompilerState *cs)
{
	int rc = parse_pipe_expression(cs);
	if (rc < 0)
		return rc;
	for (;;) {
		Opcode opcode;
		if (cs->token == TTOK_EQUAL)
			opcode = TAOP_EQUAL;
		else if (cs->token == TTOK_NOT_EQUAL)
			opcode = TAOP_NOT_EQUAL;
		else if (cs->token == TTOK_LESS)
			opcode = TAOP_LESS;
		else if (cs->token == TTOK_LESS_EQUAL)
			opcode = TAOP_LESS_EQUAL;
		else if (cs->token == TTOK_GREATER)
			opcode = TAOP_GREATER;
		else if (cs->token == TTOK_GREATER_EQUAL)
			opcode = TAOP_GREATER_EQUAL;
		else
			break;
		next_token(cs);
		if ((rc = parse_concatenative_expression(cs)) < 0)
			return rc;
		add_operation(opcode);
	}
	return TE_OK;
}

/* Parses a logical and expression. */
int CompiledTemplate::parse_and_expression(CompilerState *cs)
{
	int rc = parse_relational_expression(cs);
	if (rc < 0)
		return rc;
	while (cs->token == TTOK_AND) {
		next_token(cs);
		if ((rc = parse_relational_expression(cs)) < 0)
			return rc;
		add_operation(TAOP_AND);
	}
	return TE_OK;
}

/* Parses a logical or expression. */
int CompiledTemplate::parse_or_expression(CompilerState *cs)
{
	int rc = parse_and_expression(cs);
	if (rc < 0)
		return rc;
	while (cs->token == TTOK_OR) {
		next_token(cs);
		if ((rc = parse_and_expression(cs)) < 0)
			return rc;
		add_operation(TAOP_OR);
	}
	return TE_OK;
}

/* Parses a Python style left-associative ternary expression. */
int CompiledTemplate::parse_expression(CompilerState *cs)
{
	int rc = parse_or_expression(cs);
	if (rc < 0)
		return rc;
	while (cs->token == TTOK_IF) {
		/* Consume if <condition>. */
		next_token(cs);
		if ((rc = parse_or_expression(cs)) < 0)
			return rc;
		/* If <condition> is false, hop over the escape branch to the code
		 * that pops expr-if-true and optionally pushes expr-if-false.  */
		TemplateOperation *condition = add_operation(TAOP_BRANCH_IF_FALSE);
		if (condition != NULL)
			condition->branch.target = num_operations + 1;
		/* The escape branch, taken if the condition is true, jumps to the end
		 * of the construction, leaving expr-if-true on the stack. */
		TemplateOperation *escape = add_operation(TAOP_BRANCH);
		/* Pop expr-if-true and push the optional expr-if-false. */
		add_operation(TAOP_POP); 
		if (cs->token == TTOK_ELSE) {
			next_token(cs); /* Consume "else". */
			if ((rc = parse_or_expression(cs)) < 0)
				return rc;
		} else {
			/* If there's no expr-if-false, the expression evaluates to null. */
			TemplateOperation *push_null = add_operation(TAOP_PUSH_LITERAL);
			if (push_null != NULL)
				push_null->literal = TVAR_NULL;
		}
		/* Target the escape branch at the end of the construction. */
		if (escape != NULL)
			escape->branch.target = num_operations;
	}

	return TE_OK;
}


/* logical-OR-expression ? expression : conditional-expression */

int CompiledTemplate::parse_text(CompilerState *cs)
{
	int rc = TE_OK;
	for (;;) {
		if (cs->token == TTOK_TEXT) {
			/* Read the trim modifiers either side of the text token. */
			unsigned start = cs->token_start, length = cs->token_length;
			bool trim_left = cs->trim, trim_right;
			next_token(cs);
			trim_right = cs->trim;
			/* Trim the input text range as mandated by the flags. */
			if (trim_left) {
				while (length != 0 && isspace(cs->input[start])) {
					start++;
					length--;
				}
			}
			if (trim_right) {
				while (length != 0 && isspace(cs->input[start + length - 1]))
					length--;
			}
			/* If any text remains, add a text op. */
			if (length != 0) {
				TemplateOperation *op = add_operation(TAOP_INSERT_TEXT);
				if (op != NULL) {
					op->text.offset = cs->text_length;
					op->text.length = length;
					memcpy(heap + cs->text_length, cs->input + start, length);
				}
				cs->text_length += length;
			}
		} else if (cs->token == TTOK_OPEN_EXPR) {
			next_token(cs); /* Consume "{{". */
			if ((rc = parse_expression(cs)) < 0)
				return rc;
			if (cs->token != TTOK_CLOSE_EXPR)
				return set_error(cs, TE_UNEXPECTED_TOKEN, "}}");
			next_token(cs); /* Consume "}}". */
			add_operation(TAOP_POP_INSERT);
		} else if (cs->token == TTOK_OPEN_DIRECTIVE) {
			int token = next_token(cs);
			if (token == TTOK_INCLUDE) {
				if ((rc = parse_include_directive(cs)) < 0)
					break;
			} else if (token == TTOK_IF) {
				if ((rc = parse_conditional_directive(cs)) < 0)
					break;
			} else if (token == TTOK_ELSE || token == TTOK_ELSEIF || 
				token == TTOK_ENDIF) {
				break;
			} else {
				rc = set_error(cs, TE_UNEXPECTED_TOKEN, "directive keyword");
				break;
			}
		} else {
			break;
		}
	}
	return rc;
}

int CompiledTemplate::parse_template(CompilerState *cs)
{
	int rc = parse_text(cs);
	if (rc < 0)
		return rc;
	if (cs->token != TTOK_EOS)
		return set_error(cs, TE_UNEXPECTED_TOKEN, "end of input");
	return TE_OK;
}

void CompiledTemplate::clear_program(void)
{
	if (program != NULL) {
		for (unsigned i = 0; i < num_operations; ++i)
			if (program[i].opcode == TAOP_PUSH_LITERAL)
				tv_deinit(&program[i].literal);
	}
	program = NULL;
	num_operations = 0;
}

void CompiledTemplate::clear_heap(void)
{
	clear_program();
	if (heap != NULL) {
		delete [] heap;
		heap = NULL;
	}
	heap_size = 0;
}

void CompiledTemplate::reallocate_heap(unsigned size)
{
	clear_heap();
	if (size != 0) {
		heap = new char[size];
		heap_size = size;
	}
}

void CompiledTemplate::reset_compiler(CompilerState *cs, 
	const char *input, unsigned length)
{
	compiler_error = TE_OK;
	cs->input = input;
	cs->length = length;
	cs->pos = 0;
	cs->in_directive = false;
	cs->trim = false;
	cs->token_start = 0;
	cs->token_length = 0;
	cs->text_length = 0;
	next_token(cs);
}

int CompiledTemplate::compile(const char *input, unsigned length)
{
	/* Parse once without generating any operations or heap data, to calculate
	 * the program length and heap size. */
	clear_heap();
	CompilerState cs;
	reset_compiler(&cs, input, length);
	int rc = parse_template(&cs);
	if (rc < 0)
		return rc;

	/* Allocate a buffer for the program and text heap. */
	unsigned required_size = num_operations * sizeof(TemplateOperation) +
		cs.text_length * sizeof(char);
	reallocate_heap(required_size);
	program = (TemplateOperation *)(heap + cs.text_length);

	/* Parse a second time, generating operations and copying text into the
	 * heap. */
	num_operations = 0;
	reset_compiler(&cs, input, length);
	rc = parse_template(&cs);
	if (rc < 0) {
		clear_heap();
		return rc;
	}

	return TE_OK;
}

/* Adds text to the render buffer. If the buffer too small to accommodate
 * the data leaving at least one byte remaining for a final null, the data is
 * trunacted. */
int CompiledTemplate::append(RenderState *rs, const void *data, 
	unsigned size) const
{
	rs->required += size;
	int rc = TE_OK;
	if (rs->buffer_size != 0) {
		if (rs->written + size + 1 > rs->buffer_size) {
			size = rs->buffer_size - rs->written - 1;
			rc = TE_TRUNCATED;
		}
		if (data != NULL)
			memcpy(rs->buffer + rs->written, data, size);
		rs->written += size;
	} else {
		rc = TE_TRUNCATED;
	}
	return rc;
}

/* Converts a template variable to text and adds it to the render buffer. */
int CompiledTemplate::append_variable(RenderState *rs, 
	const TemplateVariable *tv) const
{
	TemplateVariable vs = *tv;
	tv_cast_string(&vs);
	int rc = append(rs, vs.string.data, vs.string.length);
	if (vs.string.data != tv->string.data)
		tv_clear_string(&vs);
	return rc;
}

/* Renders another template directly into the render output of this one. */
int CompiledTemplate::append_template(RenderState *rs, 
	const CompiledTemplate *ct, unsigned chain) const
{
	unsigned required = 0;
	int rc = TE_OK;
	if (rs->required + 1 >= rs->buffer_size) {
		rc = ct->render(NULL, &required, NULL, 0, chain);
	} else {
		unsigned available = rs->buffer_size - rs->written - 1;
		rc = ct->render(NULL, &required, rs->buffer + rs->written,
			available, chain);
		if (required <= available) {
			rs->written += required;
		} else {
			rs->written += available;
		}
	}
	rs->required += required;
	return rc;
}

/* Pushes a variable onto the render stack. */
int CompiledTemplate::push(RenderState *rs, const TemplateVariable *tv) const
{
	if (rs->stack_top == RENDER_STACK_SIZE)
		return set_error(rs, TE_STACK_OVERFLOW);
	rs->stack[rs->stack_top++] = tv_dup(tv);
	return TE_OK;
}

/* Pops the variable from the top of the render stack and appends it to the
 * render buffer. */
int CompiledTemplate::pop_insert(RenderState *rs) const
{
	if (rs->stack_top == 0)
		return set_error(rs, TE_STACK_UNDERFLOW);
	return append_variable(rs, &rs->stack[--rs->stack_top]);
}

/* Attempts to retrieve the text from a URL and append it to the render 
 * buffer. */
int CompiledTemplate::insert_url(RenderState *rs, UrlKey url_key,
	unsigned chain) const
{
	if (++processor->inclusion_depth == MAX_INCLUSION_DEPTH)
		return set_error(rs, TE_CYCLIC_INCLUDE);
	CompiledTemplate *ct = processor->create_from_url(url_key);
	--processor->inclusion_depth;
	if (ct == NULL)
		return set_error(rs, TE_NOT_READY);
	int rc = append_template(rs, ct, chain);
	processor->destroy(ct);
	return rc;
}

/* Attempts to find a variable using this template's lookup chain. */
int CompiledTemplate::look_up(unsigned chain, uint64_t key, 
	const TemplateVariable **tv) const
{
	*tv = processor->look_up(key, chain);
	if (*tv == NULL) {
		*tv = &TVAR_NULL;
		return TE_NOT_READY;
	}
	return TE_OK;
}

/* Executes a single program operation. */
int CompiledTemplate::execute_op(RenderState *rs, const TemplateOperation *op,
	unsigned chain) const
{
	int rc = TE_OK, rc2;
	const TemplateVariable *tv = NULL;
	unsigned next = rs->pos + 1;
	if (op->opcode == TAOP_PUSH) {
		rc = look_up(chain, op->lookup.key, &tv);
		if ((rc2 = push(rs, tv)) < 0)
			rc = rc2;
	} else if (op->opcode == TAOP_PUSH_LITERAL) {
		rc = push(rs, &op->literal);
	} else if (op->opcode == TAOP_POP) {
		if (rs->stack_top == 0)
			return set_error(rs, TE_STACK_UNDERFLOW);
		tv_deinit(rs->stack + rs->stack_top - 1);
		rs->stack_top--;
	} else if (op->opcode == TAOP_POP_INSERT) {
		rc = pop_insert(rs);
	} else if (is_binary_op(op->opcode)) {
		if (rs->stack_top < 2)
			return set_error(rs, TE_STACK_UNDERFLOW);
		TemplateVariable *a = rs->stack + rs->stack_top - 2;
		TemplateVariable *b = rs->stack + rs->stack_top - 1;
		TemplateVariable result = tv_binary_op(a, b, op->opcode);
		tv_deinit(a);
		tv_deinit(b);
		rs->stack[rs->stack_top - 2] = result;
		rs->stack_top--;
	} else if (is_unary_op(op->opcode)) {
		if (rs->stack_top == 0)
			return set_error(rs, TE_STACK_UNDERFLOW);
		tv_unary_op(rs->stack + rs->stack_top - 1, op);
	} else if (op->opcode == TAOP_INSERT_TEXT) {
		rc = append(rs, heap + op->text.offset, op->text.length);
	} else if (op->opcode == TAOP_INSERT_VARIABLE) {
		rc = look_up(chain, op->lookup.key, &tv);
		if (rc >= 0)
			rc = append_variable(rs, tv);
	} else if (op->opcode == TAOP_INSERT_URL) {
		rc = insert_url(rs, op->url.key, chain);
	} else if (op->opcode == TAOP_BRANCH) {
		next = op->branch.target;
	} else if (op->opcode == TAOP_BRANCH_IF_FALSE) {
		if (rs->stack_top == 0)
			return set_error(rs, TE_STACK_UNDERFLOW);
		if (!tv_is_true(rs->stack + rs->stack_top - 1))
			next = op->branch.target;
		rs->stack_top--;
	}
	rs->pos = next;
	return rc;
}

/* Moves the instruction pointer to the first instruction and clears the
 * stack. */
void CompiledTemplate::rewind(RenderState *rs) const
{
	rs->stack_top = 0;
	rs->pos = 0;
	rs->required = 0;
	rs->written = 0;
}

/* Executes the template program. */
int CompiledTemplate::execute(RenderState *rs, unsigned chain) const
{
	/* Execute until an error other than TE_INSUFFICIENT_BUFFER or TE_NOT_READY
	 * is encountered. */
	rewind(rs);
	int rc = TE_OK;
	while (rs->pos != num_operations) {
		int op_rc = execute_op(rs, program + rs->pos, chain);
		if (op_rc <= TE_ERROR)
			return op_rc;
		if (rc == TE_OK)
			rc = op_rc;
	}
	return rc;
}

void CompiledTemplate::init_render_state(RenderState *rs, 
	char *buffer, unsigned buffer_size) const
{
	rs->buffer = buffer;
	rs->buffer_size = buffer_size;
	rs->buffer_owned = false;
	rewind(rs);
}

void CompiledTemplate::reallocate_render_buffer(RenderState *rs, 
	unsigned size) const
{
	if (size < rs->buffer_size)
		return;
	if (rs->buffer_owned) {
		delete [] rs->buffer;
		rs->buffer_size = 0;
	}
	rs->buffer = new char[size];
	rs->buffer_size = size;
	rs->buffer_owned = true;
}

int CompiledTemplate::render(char **out_text, unsigned *out_length, 
	char *buffer, unsigned buffer_size, unsigned chain) const
{
	RenderState rs;
	init_render_state(&rs, buffer, buffer_size);
	int rc = compiler_error;
	if (rc != TE_OK) {
		if (out_text != NULL)
			reallocate_render_buffer(&rs, heap_size);
		append(&rs, heap, heap_size - 1);
	} else {
		rc = execute(&rs, chain);
		if (rc == TE_TRUNCATED && out_text != NULL) {
			reallocate_render_buffer(&rs, rs.required + 1);
			rc = execute(&rs, chain);
		}
	}
	if (rs.buffer_size != 0)
		rs.buffer[rs.written++] = '\0';
	if (out_text != NULL)
		*out_text = rs.buffer;
	if (out_length != 0)
		*out_length = rs.required;
	return rc;
}

TemplateErrorCode CompiledTemplate::error_code(void) const
{
	return (TemplateErrorCode)compiler_error;
}

const char *CompiledTemplate::error_message(void) const
{
	return (compiler_error == TE_OK) ? NULL : heap;
}

unsigned CompiledTemplate::memory_usage(void) const
{
	return sizeof(CompiledTemplate) + heap_size;
}

/*
 * Unit Tests
 */

#define TEXT_TEMPLATE_UNIT_TESTS
#if defined(TEXT_TEMPLATE_UNIT_TESTS)

#include <Windows.h>

void unit_test(FILE *os)
{
	if (os == NULL)
		os = stdout;

	static const char * const TEST_INPUTS[] = {
		"This should all be verbatim: }} } }} {} }{ %} }} {!.",

		"This shouldn't compile. {{",

		"This shouldn't compile. {% mavis",

		"This shouldn't compile. {% if else",

		"This shouldn't compile. {% if var }}",

		"This shouldn't compile. {% if abc %}Terminate me!",

		"Here's some plain text.",

		"Heres's a nice juicy apple: {{ var_apple }} and a pear: {{ var_pear }}.",

		"Variabes that don\'t exist: [{{ bogus }}], [{{ road_beers }}].",

		"What about a nice integer? {{ var_two }} "
		"Or perhaps you'd prefer a float? {{ var_pi }} "
		"Too many digits, you say? We exist only to please: {{ var_pi_p2 }}.",

		"White space control: "
		"|      {{- \"<- both sides trimmed ->\" -}}      |, "
		"|      {{- \"<- left trim only ->\" +}}      |, "
		"|      {{+ \"<- right trim only ->\" -}}      |",

		"Booleans: {{ var_true }}/{{ var_false }}.",

		"{% if var_true %}This should display.{% else %}This shouldn't display.{% endif %}",

		"{% if var_false %}This shouldn't display.{% else %}This should display.{% endif %}",

		"{% if 10 < 5 %}This shouldn't display.{% elseif 1 == 0 %}This shouldn't display.{% else %}This should display.{% endif %}",

		"{% if var_false %}This shouldn't display: {{ var_pear }}{% elseif var_false %}This shouldn't display: {{ var_apple }}.{% elseif var_true %}This should display: {{ var_orange }}.{% endif %}",
	
		"I though it would be nice to include an {% include \"stkr://apple\" %}, or perhaps an {% include \"stkr://orange\" %}.",

		"Including a template from another template: [{% include \"stkr://test8\" %}].",

		"Literals: {{ \"a string\" }}, {{ -42 }}, {{ 732.0 }}.",

		"Literal arithmetic: "
			"3 * 4 + 7.0 / 2 = {{ 3 * 4 + 7.0 / 2 }}, "
			"8 % 3 = {{ 8 % 3 }}, "
			"(3 + 5) * 2 = {{ (3 + 5) * 2 }}.",

		"Logical expressions: "
			"1 and 0: {{ 1 and 0 }}, "
			"1 or 0: {{ 1 or 0 }}, "
			"not 1: {{ not 1 }}, "
			"1 and not 0: {{ 1 and not 0 }}.",

		"Relational expressions: "
			"3 == 4: {{ 3 == 4 }}, "
			"3 != 4: {{ 3 != 4 }}, "
			"3 < 4: {{ 3 < 4 }}, "
			"4 < 3: {{ 4 < 3 }}, "
			"3 < 3: {{ 3 < 3 }}, "
			"3 <= 3: {{ 3 <= 3 }}",
		
		"String relational expressions: "
			"\"a\" == \"b\": {{ \"a\" == \"b\" }}, "
			"\"a\" != \"b\" {{ \"a\" != \"b\" }}, "
			"\"a\" == \"a\" {{ \"a\" == \"a\" }}, "
			"\"ab\" < \"cde\": {{ \"ab\" < \"cde\" }}, "
			"\"ab\" > \"cde\": {{ \"ab\" > \"cde\" }}",

		"Ternary operator: "
			"\"toad\" if 1 else \"moon\" = {{ \"toad\" if 1 else \"moon\" }}, "
			"\"toad\" if 0 else \"moon\" = {{ \"toad\" if 0 else \"moon\" }}, "
			"\"toad\" if 1 = {{ \"toad\" if 1 }}, "
			"\"toad\" if 0 = {{ \"toad\" if 0 }}",

		"Concatenation: {{ \"ABC\" .. \"DEF\"..(3)..(5.0) }}",

		"Filter expressions: "
			"Pretty: 0: {{ 0 | precision(0) | pretty }}, 1: {{ 1 | pretty(3) }}, 123: {{ 123 | pretty }}, 12345: {{ 12345 | pretty }}, 3456789.123: {{ 3456789.123 | pretty(3) }}, -312.12: {{ -312.12 | pretty }}, "
			"{{ -3.0 | abs | precision(6) }}, "
			"{{ \"http://example.org/please encode me/!@#\" | encode }}, "
			"{{ \"http://example.org/please%20decode+me/\" | decode }}, "
			"A byte list: rgb({{ 13 * 37 * 44 | bytes(4) }}), "
			"{{ (\"  a string   \" | strip)..\" another string\" }}"
			
	};
	static const unsigned NUM_TEST_INPUTS = sizeof(TEST_INPUTS) / 
		sizeof(TEST_INPUTS[0]);

	UrlCache *cache = new UrlCache();
	TemplateProcessor processor(cache);

	/* Define some variables. */
	processor.clear_scope(VSCOPE_GLOBAL);
	processor.clear_scope(VSCOPE_LOCAL);
	processor.set_string(VSCOPE_GLOBAL, "var_apple", "APPLE");
	processor.set_string(VSCOPE_GLOBAL, "var_orange", "ORANGE");
	processor.set_string(VSCOPE_GLOBAL, "var_pear", "PEAR");
	processor.set_string(VSCOPE_LOCAL, "var_pear", "PEAR_LOCAL");
	processor.set_integer(VSCOPE_GLOBAL, "var_two", 2);
	processor.set_float(VSCOPE_GLOBAL, "var_pi", 3.141592);
	processor.set_float(VSCOPE_GLOBAL, "var_pi_p2", 3.141592, 2);
	processor.set_boolean(VSCOPE_GLOBAL, "var_true", true);
	processor.set_boolean(VSCOPE_GLOBAL, "var_false", false);

	/* Add some test data to the URL cache for inclusion. */
	cache->insert("stkr://apple", -1, "INCLUDED_APPLE", strlen("INCLUDED_APPLE"));
	cache->insert("stkr://orange", -1, "INCLUDED_ORANGE", strlen("INCLUDED_ORANGE"));
	for (unsigned i = 0; i < NUM_TEST_INPUTS; ++i) {
		char url[128];
		sprintf(url, "stkr://test%u", i);
		cache->insert(url, -1, TEST_INPUTS[i], 
			strlen(TEST_INPUTS[i]), MIMETYPE_TEMPLATE);
	}


	CompiledTemplate *templates[NUM_TEST_INPUTS];
	int render_codes[NUM_TEST_INPUTS];
	memset(render_codes, TE_OK, sizeof(render_codes));
	char *buffer;
	unsigned length;
	for (unsigned i = NUM_TEST_INPUTS - 1; i + 1 != 0; --i) {
		/* Compile the template. */
		CompiledTemplate *ct = processor.create(TEST_INPUTS[i], -1);
		if (ct->error_code() != TE_OK) {
			fprintf(os, "%03d: Compilation failed. "
				"error_code: %d, error_string: %s\n", 
				i, ct->error_code(), ct->error_message());
		} else {
			fprintf(os, "%03d: Compilation successful.\n", i);
		}
		templates[i] = ct;

		int rc = ct->render(&buffer, &length);
		fprintf(os, "\trender() returned: %d\n", rc);
		fprintf(os, "\tRendered text (%u bytes): %s\n", length, buffer);
		render_codes[i] = rc;
	}

	for (unsigned pass = 0; pass < 3; ++pass) {
		for (unsigned i = NUM_TEST_INPUTS - 1; i + 1 != 0; --i) {
			CompiledTemplate *ct = templates[i];

			/* Try to render the template. */
			int rc = ct->render(&buffer, &length);
			if (rc != render_codes[i]) {
				fprintf(os, "\trender() returned: %d\n", rc);
				fprintf(os, "\tRendered text (%u bytes): %s\n", length, buffer);
				render_codes[i] = rc;
			}
		}
		Sleep(1000);
		cache->update();
	}

	for (unsigned i = 0; i < NUM_TEST_INPUTS; ++i)
		processor.destroy(templates[i]);
}

#endif // defined(TEXT_TEMPLATE_UNIT_TESTS)

} // namespace text_template
