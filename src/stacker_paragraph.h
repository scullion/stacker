#pragma once

#include <cstdint>

namespace stkr {

const unsigned NUM_STATIC_PARAGRAPH_ELEMENTS = 256;
const unsigned MAX_ACTIVE_BREAKPOINTS        = 16;
const unsigned NUM_STATIC_BREAKPOINTS         = NUM_STATIC_PARAGRAPH_ELEMENTS + 1;

const int UNBOUNDED_LINE_WIDTH = -1;
const int16_t PENALTY_MIN = -2048;
const int16_t PENALTY_MAX =  2047;

enum ParagraphElementType { PET_BOX, PET_GLUE, PET_PENALTY };

struct ParagraphLine {
	unsigned a, b;
	float adjustment_ratio;
	unsigned unscaled_width;
	float demerits;
	float line_demerits;
};

struct ParagraphElement {
	int16_t width;
	int16_t stretch;
	int16_t shrink;
	int16_t penalty     : 12;
	uint16_t type       : 2;
	uint16_t empty      : 1;
	uint16_t has_token : 1; 
};

struct Paragraph {
	ParagraphElement buffer[NUM_STATIC_PARAGRAPH_ELEMENTS];
	ParagraphElement *elements;
	unsigned num_elements;
	unsigned capacity;
	int line_width;
};


void paragraph_init(Paragraph *paragraph, int line_width = UNBOUNDED_LINE_WIDTH);
void paragraph_clear(Paragraph *paragraph);
void paragraph_append(
	Paragraph *paragraph, 
	ParagraphElementType type, 
	uint32_t width, 
	int stretch = 0, 
	int shrink = 0,
	int penalty = 0, 
	bool empty = false, 
	bool has_token = false);
unsigned determine_breakpoints(const Paragraph *p, ParagraphLine **lines,
	ParagraphLine *line_buffer = 0, unsigned line_buffer_elements = 0);

extern const char * const PARAGRAPH_ELEMENT_TYPE_STRINGS[];

} // namespace stkr
