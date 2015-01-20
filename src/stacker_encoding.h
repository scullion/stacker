#pragma once

#include <cstdint>

#include "stacker.h"

namespace stkr {

bool unicode_isalpha(uint32_t ch);
bool unicode_isdigit(uint32_t ch);
bool unicode_isalnum(uint32_t ch);
bool unicode_isident(uint32_t ch);
bool unicode_isidentfirst(uint32_t ch);
bool unicode_is_multipart_delimiter(uint32_t ch);
bool unicode_isspace(uint32_t ch);

unsigned strcpy_encoding(const void *s, unsigned length, void *buffer, 
	unsigned buffer_size, TextEncoding encoding);

unsigned utf8_decode(const char *s, const char *end, uint32_t *code_point);
unsigned utf8_encode(char *s, uint32_t code_point);
unsigned utf8_encoded_length(uint32_t code_point);
unsigned utf8_count(const char *s);
unsigned utf8_transcode(const char *s, unsigned length, 
	void *output, TextEncoding encoding);
unsigned utf8_transcode_heap(const char *s, unsigned length, 
	void **output, TextEncoding encoding);

unsigned utf16_decode(const uint16_t *s, uint32_t *code_point);
unsigned utf16_encode(uint16_t *s, uint32_t code_point);
unsigned utf16_encoded_length(uint32_t code_point);

unsigned encoded_length(uint32_t code_point, unsigned mask);
unsigned highest_encodable_code_point(TextEncoding encoding);
unsigned encode_paragraph_break(void *buffer, TextEncoding encoding);
unsigned encode_null(void *buffer, TextEncoding encoding);

const uint32_t UNICODE_REPLACEMENT = 0xFFFD;     // U+FFFD REPLACEMENT CHARACTER
const uint32_t UNICODE_BOM         = 0xFEFF;     // U+FEFF BYTE ORDER MARK
const uint32_t END_OF_STREAM       = 0xFFFFFFFF; // Not a character.

extern const unsigned BYTES_PER_CODE_UNIT[NUM_ENCODINGS];
extern const unsigned ENCODING_BYTE_SHIFTS[NUM_ENCODINGS];

/* Masks representing text encodings for use with encoded_length(). */
extern const unsigned ENCODING_LENGTH_MASKS[NUM_ENCODINGS];

} // namespace stkr
