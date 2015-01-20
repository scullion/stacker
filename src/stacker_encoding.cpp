#include "stacker_encoding.h"
#include "stacker_util.h"

namespace stkr {

extern const unsigned BYTES_PER_CODE_UNIT[NUM_ENCODINGS] = { 1, 1, 1, 2, 4 };
extern const unsigned ENCODING_BYTE_SHIFTS[NUM_ENCODINGS] = { 0, 0, 0, 1, 2 };
extern const unsigned ENCODING_LENGTH_MASKS[NUM_ENCODINGS] = { 0, 0, 7, 4, 0 };

bool unicode_isalpha(uint32_t ch)
{
	return ch >= 'A' && ch <= 'Z' || ch >= 'a' && ch <= 'z';
}

bool unicode_isdigit(uint32_t ch)
{
	return ch >= '0' && ch <= '9';
}

bool unicode_isalnum(uint32_t ch)
{
	return unicode_isalpha(ch) || unicode_isdigit(ch);
}

bool unicode_isident(uint32_t ch)
{
	return unicode_isalnum(ch) || ch == '_' || ch == '-' || 
		(ch >= 0xA0 && ch <= 0x10FFFF);
}

bool unicode_isidentfirst(uint32_t ch)
{
	return unicode_isalpha(ch) || ch == '_' || 
		(ch >= 0xA0 && ch <= 0x10FFFF);
}

/* True if a Unicode code point represents an ASCII white space character. */
bool unicode_isspace(uint32_t ch)
{
	static const unsigned MIN = '\t';
	static const unsigned MAX = ' ';
	static const unsigned MASK = 
		 (1u << (' '  - MIN)) + 
		 (1u << ('\t' - MIN)) + 
		 (1u << ('\r' - MIN)) + 
		 (1u << ('\n' - MIN)) + 
		 (1u << ('\v' - MIN));
	ch -= MIN;
	return ch <= MAX - MIN && ((1 << ch) & MASK) != 0;
}

/* True if a Unicode code point is a character that dividesa word into parts
 * for the purposes of text breaking. */
bool unicode_is_multipart_delimiter(uint32_t ch)
{
	return ch == '-';
}

/* Copies as much of 's' into 'buffer' as will fit. The buffer_size parameter
 * indicates the maximum number of code units that may be written to the buffer,
 * including the terminator. The result is guaranteed to be null terminated. 
 * Returns the number of code units copied, excluding the terminator. */
unsigned strcpy_encoding(const void *s, unsigned length, void *buffer, 
	unsigned buffer_size, TextEncoding encoding)
{
	assertb(buffer_size != 0);
	unsigned chars_to_copy = length < buffer_size ? length : buffer_size - 1;
	unsigned bytes_to_copy = chars_to_copy << ENCODING_BYTE_SHIFTS[encoding];
	memcpy(buffer, s, bytes_to_copy);
	encode_null((char *)buffer + bytes_to_copy, encoding);
	return chars_to_copy;
}

/* Decodes a single UTF-8 sequence, returning the number of bytes decoded. 
 * Invalid sequences decode to the replacement character. */
unsigned utf8_decode(const char *s, const char *end, uint32_t *code_point)
{
	static const unsigned BASE[4] = { 0x80, 0x800, 0x10000, 0x110000 };

	/* End of stream? */
	if (s == end) {
		*code_point = END_OF_STREAM;
		return 0;
	}

	/* A 7-bit character? */
	unsigned ch0 = (unsigned char)s[0];
	if (ch0 < 0x80) {
		*code_point = ch0;
		return 1;
	}

	/* Check for an invalid continuation or too-long encoding. */
	if (ch0 < 0xC0 || ch0 >= 0xF8) {
		*code_point = ch0;
		return 1;
	}

	/* Is there enough space for a well formed sequence? */
	unsigned count = ch0 >= 0xF0 ? 4 : (ch0 >= 0xE0 ? 3 : 2);
	if (s + count > end) {
		*code_point = UNICODE_REPLACEMENT;
		return end - s;
	}

	/* Decode the continuations. */
	unsigned cp = ch0 & ((0x80u >> count) - 1u);
	unsigned advance = 1;
	do {
		unsigned ch = (unsigned char)s[advance++];
		if ((ch & 0xC0) != 0x80) {
			cp = UNICODE_REPLACEMENT;
			break;
		}
		cp = (cp << 6) + (ch & 0x3F);
	} while (advance != count);
	if (cp < BASE[count - 2] && cp >= BASE[count - 1])
		cp = UNICODE_REPLACEMENT;
	*code_point = cp;
	return advance;
}

/* Encodes a code point as UTF-8, returning the number of bytes encoded. */
unsigned utf8_encode(char *s, uint32_t code_point)
{
	if (code_point < 0x80) {
		s[0] = char(code_point);
		return 1;
	} else if (code_point < 0x800) {
		s[0] = char(0xC0 + (code_point >> 6));
		s[1] = char(0x80 + (code_point & 0x3F));
		return 2;
	} else if (code_point < 0x10000) {
		s[0] = char(0xE0 + (code_point >> 12));
		s[1] = char(0x80 + (code_point >> 6) & 0x3F);
		s[2] = char(0x80 + (code_point & 0x3F));
		return 3;	
	} else {
		s[0] = char(0xF0 + (code_point >> 18));
		s[1] = char(0x80 + (code_point >> 12) & 0x3F);
		s[2] = char(0x80 + (code_point >> 6) & 0x3F);
		s[3] = char(0x80 + (code_point & 0x3F));
		return 4;
	}
}

/* Returns the number of bytes required to encode a code point as UTF-8. */
unsigned utf8_encoded_length(uint32_t code_point)
{
	if (code_point <    0x80) return 1;
	if (code_point <   0x800) return 2;
	if (code_point < 0x10000) return 3;
	return 4;
}

/* Returns the number of code points that will result from decoding a UTF-8
 * string using utf8_decode(). The count excludes the null terminator. */
unsigned utf8_count(const char *s, unsigned length)
{
	const char *end = s + length;
	unsigned count = 0;
	while (*s != 0) {
		uint32_t code_point;
		s += utf8_decode(s, end, &code_point);
		count++;
	}
	return count;
}

/* Decodes a single UTF-16 sequence, returning the number of words decoded. */
unsigned utf16_decode(const uint16_t *s, uint32_t *code_point)
{
	/* A BMP character? */
	uint32_t high = s[0] - 0xD800;
	if (high >= 0x800) {
		*code_point = s[0];
		return 1;
	}

	/* Check that the first and second words are in the high and low surrogate
	 * ranges respectively. */
	uint32_t low = s[1] - 0xDC00;
	if (high >= 0x400 || low >= 0x400) {
		*code_point = UNICODE_REPLACEMENT;
		return 1;
	}

	/* A valid surrogate pair. */
	*code_point = 0x10000 + (high >> 10) + low;
	return 2;
}

/* Encodes a code point as UTF-16, returning the number of words encoded. */
unsigned utf16_encode(uint16_t *s, uint32_t code_point)
{
	if (code_point < 0x10000) {
		s[0] = uint16_t(code_point);
		return 1;
	} else {
		code_point -= 0x10000;
		s[0] = uint16_t(0xD800 + (code_point >> 10));
		s[1] = uint16_t(0xDC00 + (code_point & 0x3FF));
		return 2;
	}
}

/* Returns the number of words required to encode a code point as UTF-16. */
unsigned utf16_encoded_length(uint32_t code_point)
{
	return (code_point < 0x10000) ? 1 : 2;
}

/* Returns the highest Unicode code point representable in a text encoding. */
unsigned highest_encodable_code_point(TextEncoding encoding)
{
	switch (encoding) {
		case ENCODING_ASCII:
			return 0x7F;
		case ENCODING_LATIN1:
			return 0xFF;
		case ENCODING_UTF8:
		case ENCODING_UTF16:
		case ENCODING_UTF32:
			return 0x10FFFF;
	}
	assertb(false);
	return 0;
}

/* Returns the number of code units required to represent a code point. */
unsigned encoded_length(uint32_t code_point, unsigned mask)
{
	unsigned length = 1;
	length += (mask >> 0) & (code_point >= 0x80);
	length += (mask >> 1) & (code_point >= 0x800);
	length += (mask >> 2) & (code_point >= 0x10000);
	return length;
}

/* Returns the number of characters required to represent a code point as a
 * Unicode short identifier. */
static unsigned short_identifier_length(uint32_t code_point)
{
	return code_point <= 0xFFFF ? 6 : 10;
}

/* Writes a Unicode short identifier ("U+XXXX" or "U+XXXXXXXX") to an ASCII or 
 * UTF-8 string, returning the number of characters written. */
static unsigned write_short_identifier(uint32_t code_point, char *output)
{
	static const char HEX_DIGITS[16 + 1] = "0123456789ABCDEF";

	unsigned digits = code_point >= 0x10000 ? 8 : 4;
	unsigned length = 0;
	output[length++] = 'U'; 
	output[length++] = '+';
	do {
		digits--;
		output[length++] = HEX_DIGITS[(code_point >> (4 * digits)) & 0xF];
	} while (digits != 0);
	return length;
}

/* Encodes a UTF-8 string in ASCII or Latin-1. Code points that cannot be 
 * represented are replaced by short identifier sequences ("U+XXXX"). */
static unsigned utf8_to_bytes(const char *s, unsigned length, 
	char *output, TextEncoding encoding)
{
	uint32_t code_point;
	uint32_t highest = highest_encodable_code_point(encoding);
	unsigned out_length = 0;
	const char *end = s + length;
	if (output == NULL) {
		while (s != end) {
			s += utf8_decode(s, end, &code_point);
			out_length += (code_point <= highest) ? 1 :
				short_identifier_length(code_point);
		}
	} else {
		while (s != end) {
			s += utf8_decode(s, end, &code_point);
			if (code_point <= highest) {
				output[out_length++] = char(code_point);
			} else {
				out_length += write_short_identifier(code_point, 
					output + out_length);
			}
		}
		output[out_length] = 0;
	}
	return out_length;
}

/* Encodes a UTF-8 string as UTF-16. The output is null terminated. Returns the 
 * number of code points written, excluding the terminator. */
static unsigned utf8_to_utf16(const char *s, unsigned length, 
	uint16_t *output)
{
	uint32_t code_point;
	const char *end = s + length;
	unsigned out_length = 0;
	if (output == NULL) {
		while (s != end) {
			s += utf8_decode(s, end, &code_point);
			out_length += utf16_encoded_length(code_point);
		}
	} else {
		while (s != end) {
			s += utf8_decode(s, end, &code_point);
			out_length += utf16_encode(output + out_length, code_point);
		}
		output[out_length] = 0;
	}
	return out_length;
}

/* Encodes a UTF-8 string as UTF-32. The output is null terminated. Returns the 
 * number of code points written, excluding the terminator. */
static unsigned utf8_to_utf32(const char *s, unsigned length, 
	uint32_t *output)
{
	unsigned out_length;
	if (output == NULL) {
		out_length = utf8_count(s, length);
	} else {
		out_length = 0;
		const char *end = s + length;
		while (s != end) {
			uint32_t code_point;
			s += utf8_decode(s, end, &code_point);
			output[out_length++] = code_point;
		}
		output[out_length] = 0;
	}
	return out_length;
}

/* Converts a UTF-8 encoded string to another encoding. The input need not be
 * null terminated, but output is guaranteed to be. Returns the length of
 * the encoded string, which does not include the terminator. A NULL output
 * may be passed to determine the encoded length of the string. */
unsigned utf8_transcode(const char *s, unsigned length, 
	void *output, TextEncoding encoding)
{
	switch (encoding) {
		case ENCODING_ASCII:
		case ENCODING_LATIN1:
			return utf8_to_bytes(s, length, (char *)output, encoding);
		case ENCODING_UTF8:
			if (output != NULL) {
				memcpy(output, s, length);
				((char *)output)[length] = 0;
			}
			return length;
		case ENCODING_UTF16:
			return utf8_to_utf16(s, length, (uint16_t *)output);
		case ENCODING_UTF32:
			return utf8_to_utf32(s, length, (uint32_t *)output);
	}
	assertb(false);
	return 0;
}

/* Transcodes a UTF-8 string into a new heap buffer, for which the caller 
 * assumes responsibility. */
unsigned utf8_transcode_heap(const char *s, unsigned length, 
	void **output, TextEncoding encoding)
{
	unsigned encoded_length = utf8_transcode(s, length, NULL, encoding);
	unsigned bytes_required = (encoded_length + 1) * BYTES_PER_CODE_UNIT[encoding];
	*output = new char[bytes_required];
	return utf8_transcode(s, length, *output, encoding);
}

/* Writes a double newline sequence to 'buffer' in the specified encoding,
 * returning the number of code units written. The buffer may be NULL. */
unsigned encode_paragraph_break(void *buffer, TextEncoding encoding)
{
	switch (encoding) {
		case ENCODING_ASCII:
		case ENCODING_LATIN1:
		case ENCODING_UTF8:
			if (buffer != NULL) {
				((char *)buffer)[0] = '\n';
				((char *)buffer)[1] = '\n';
			}
			return 2;
		case ENCODING_UTF16:
			if (buffer != NULL) {
				((uint16_t *)buffer)[0] = '\n';
				((uint16_t *)buffer)[1] = '\n';
			}
			return 2;
		case ENCODING_UTF32:
			if (buffer != NULL) {
				((uint32_t *)buffer)[0] = '\n';
				((uint32_t *)buffer)[1] = '\n';
			}
			return 2;
	}
	assertb(false);
	return 0;
}

/* Writes a null terminator to 'buffer' in the specified encoding and returns
 * the number of code units written (which is always one). */
unsigned encode_null(void *buffer, TextEncoding encoding)
{
	if (buffer != NULL) {
		switch (encoding) {
			case ENCODING_ASCII:
			case ENCODING_LATIN1:
			case ENCODING_UTF8:
				*(char *)buffer = 0;
				break;
			case ENCODING_UTF16:
				*(uint16_t *)buffer = 0;
				break;
			case ENCODING_UTF32:
				*(uint32_t *)buffer = 0;
				break;
		}
	}
	return 1;
}

} // namespace stkr

