#include "stacker_paragraph.h"

#include <cmath>
#include <cfloat>
#include <climits>

#include <algorithm>

#include "stacker.h"
#include "stacker_util.h"

namespace stkr {

extern const char * const PARAGRAPH_ELEMENT_TYPE_STRINGS[] = 
	{ "box", "glue", "penalty" };

void paragraph_init(Paragraph *paragraph, int line_width)
{
	paragraph->elements = paragraph->buffer;
	paragraph->capacity = NUM_STATIC_PARAGRAPH_ELEMENTS;
	paragraph->num_elements = 0;
	paragraph->line_width = line_width;
}

void paragraph_clear(Paragraph *paragraph)
{
	if (paragraph->elements != paragraph->buffer)
		delete [] paragraph->elements;
	paragraph->elements = paragraph->buffer;
	paragraph->capacity = NUM_STATIC_PARAGRAPH_ELEMENTS;
	paragraph->num_elements = 0;
}

static ParagraphElement *paragraph_grow(Paragraph *paragraph, unsigned count)
{
	if (paragraph->num_elements + count > paragraph->capacity) {
		paragraph->capacity = (paragraph->num_elements + count) * 3 / 2;
		ParagraphElement *ne = new ParagraphElement[paragraph->capacity];
		memcpy(ne, paragraph->elements, 
			paragraph->num_elements * sizeof(ParagraphElement));
		if (paragraph->elements != paragraph->buffer)
			delete [] paragraph->elements;
		paragraph->elements = ne;
	}
	return paragraph->elements + paragraph->num_elements;
}

void paragraph_append(Paragraph *paragraph, ParagraphElementType type, 
	uint32_t width, int stretch, int shrink, int penalty, 
	bool empty, bool has_token)
{
	ParagraphElement *e = paragraph_grow(paragraph, 1);
	e->type       = type;
	e->has_token  = has_token;
	e->empty      = empty;
	e->width      = saturate16(width);
	e->stretch    = saturate16(stretch);
	e->shrink     = saturate16(shrink);
	e->penalty    = saturate16(penalty);
	paragraph->num_elements++;
}

/* Disable initialization of large stack arrays which makes debug builds 
 * very slow. */
#pragma runtime_checks("", off)

/* Computes a list of places to break a paragraph into lines. This is a simple 
 * implementation of the Knuth-Plass optimal fit algorithm [1].
 * 
 * [1] Knuth, D.E. & Plass, M.F. (1981). Breaking Paragraphs into Lines. 
 *     Software - Practice and Experience, Vol. 11, 1119-1184.
 */
unsigned determine_breakpoints(const Paragraph *p, ParagraphLine **lines,
	ParagraphLine *line_buffer, unsigned line_buffer_elements)
{
	/* Negative line widths count as infinity. Note that this doesn't mean the
	 * result is always a single line, because the paragraph may contain forced
	 * breaks. */
	int w = p->line_width < 0 ? 10000 : p->line_width; 

	/* Places the paragraph could be broken. */
	struct Breakpoint {
		int b;
		int next_box;
		int predecessor;
		float debug_demerits;
		float total_demerits;
		unsigned unscaled_width;
		float adjustment_ratio;
	}; 
	Breakpoint breakpoint_buffer[NUM_STATIC_PARAGRAPH_ELEMENTS + 1];
	Breakpoint *breakpoints = breakpoint_buffer;
	unsigned num_breakpoints = 0;

	/* Conservatively allocate the breakpoint buffer. */
	if (p->num_elements + 1 > NUM_STATIC_PARAGRAPH_ELEMENTS)
		breakpoints = new Breakpoint[p->num_elements + 1];

	/* Candidate line starts. */
	struct ActiveBreakpoint {
		int offset;
		int width;
		int stretch;
		int shrink;
	} active[MAX_ACTIVE_BREAKPOINTS];
	unsigned num_active = 0;

	/* Breakpoints that will become active at the next box. */
	unsigned num_preactive = 0;

	/* Start with one pre-active breakpoint before the first element. */
	breakpoints[0].b = 0;
	breakpoints[0].next_box = -1;
	breakpoints[0].predecessor = -1;
	breakpoints[0].total_demerits = 0.0f;
	num_breakpoints = 1;
	num_preactive = 1;

	for (unsigned i = 0; i < p->num_elements; ++i) {
		ParagraphElement e = p->elements[i];

		/* Is this a feasible breakpoint? We can break at (i.e. before) 1) glue 
		 * immediately preceded by a box and 2) penalty items with a penalty
		 * other than positive infinity. */
		if ((e.type == PET_GLUE && i != 0 && p->elements[i - 1].type == PET_BOX) ||
			(e.type == PET_PENALTY && e.penalty != PENALTY_MAX)) {

			/* Penalty width counts if we break after a penalty. */
			int penalty = 0, penalty_width = 0;
			bool must_break = false;
			if (e.type == PET_PENALTY) {
				penalty_width = e.width;
				penalty = e.penalty;
				must_break = (penalty == PENALTY_MIN);
			}
			
			Breakpoint *b = breakpoints + num_breakpoints;
			b->b = (int)i;
			b->next_box = -1;
			b->total_demerits = FLT_MAX; /* Infinity. */
			for (unsigned j = 0; j < num_active; ++j) {
				const ActiveBreakpoint *ab = active + j;

				/* Compute the adjustment ratio 'r' according to whether the 
				 * ideal width of the line from a to b is less than or greater 
				 * than the desired line width. */
				int slack = w - ab->width - penalty_width;
				float r = FLT_MAX;
				if (slack != 0) {
					if (slack > 0 && ab->stretch > 0)
						r = (float)slack / (float)ab->stretch;
					else if (slack < 0 && ab->shrink > 0)
						r = (float)slack / (float)ab->shrink;
				} else {
					r = 0.0f; /* A perfect fit. */
				}

				/* Compute the demerits for the line. */
				float demerits = 1e12f, debug_line_demerits = demerits;
				if (r != FLT_MAX) {
					float badness = 100.0f * r * r * fabsf(r);
					demerits = 1.0f + badness;
					if (penalty >= 0)
						demerits += (float)penalty;
					demerits *= demerits;
					debug_line_demerits = demerits; // Exclude negative penalty adjustment.
					if (penalty < 0 && penalty != PENALTY_MIN)
						demerits -= float(penalty * penalty);
				}

				/* Is 'a' the best line start candidate we have discovered so
				 * far? */
				const Breakpoint *a = breakpoints + ab->offset;
				if (a->total_demerits + demerits < b->total_demerits ||
					(must_break && j == 0)) {
					b->predecessor = ab->offset;
					b->unscaled_width = ab->width;
					b->adjustment_ratio = r;
					b->total_demerits = a->total_demerits + demerits;
					b->debug_demerits = debug_line_demerits;
				}
			}

			/* If we made a new breakpoint 'b', add it to the pre-active list.
			 * It's not a candidate line start until we've skipped over the 
			 * sequence of glue and penalties following the break. */
			if (b->total_demerits != FLT_MAX || must_break) {
				/* Even though we never remove the last active breakpoint, 
				 * it's possible that the first active breakpoint has not
				 * yet been added. This happens, for example, in empty 
				 * paragraphs, and paragraphs containing only white space. 
				 * To honour the forced break we generate an empty line. */
				if (num_active == 0) {
					breakpoints[num_breakpoints] = breakpoints[0];
					breakpoints[num_breakpoints].debug_demerits = 0.0f;
					breakpoints[num_breakpoints].total_demerits = 0.0f;
					breakpoints[num_breakpoints].adjustment_ratio = 1.0f;
					breakpoints[num_breakpoints].unscaled_width = 0;
				}
				/* All forced breakpoints must be included in the solution. To
				 * ensure this, after seeing a forced break, we don't add new
				 * breakpoints to positions before the forced break, because to 
				 * do so would be to skip over the forced break. */
				if (must_break) {
					num_active = 0;
					num_preactive = 0;
				}
				/* Add the new breakpoint and make it pre-active. */
				num_breakpoints++;
				num_preactive++;
			}
		}

		/* Could this element begin a line? If so, it marks the end of the
		 * sequence of glue and penalties we're skipping after any pre-active 
		 * breakpoints. Those breakpoints become candidate line starts. */
		if (num_preactive != 0 && (e.type == PET_BOX || 
			(e.type == PET_PENALTY && e.penalty == PENALTY_MIN))) {
			do {
				ActiveBreakpoint *ab = active;
				if (num_active == MAX_ACTIVE_BREAKPOINTS) {
					for (unsigned j = 1; j < num_active; ++j) {
						if (breakpoints[active[j].offset].total_demerits > 
						    breakpoints[ab->offset].total_demerits) 
							ab = active + j;
					}
				} else {
					ab = active + num_active;
					num_active++;
				}
				ab->offset = num_breakpoints - num_preactive;
				ab->width = 0;
				ab->stretch = 0;
				ab->shrink = 0;
				breakpoints[ab->offset].next_box = (int)i;
				num_preactive--;
			} while (num_preactive != 0);
		}

		/* Update the width bounds of each candidate line, deactivating lines
		 * whose new minimal width exceeds the maximum line width. */
		if (e.type != PET_PENALTY && num_active != 0) {
			unsigned k = 0;
			for (unsigned j = 0; j < num_active; ++j) {
				active[k].offset  = active[j].offset;
				active[k].width   = active[j].width   + e.width;
				active[k].stretch = active[j].stretch + e.stretch;
				active[k].shrink  = active[j].shrink  + e.shrink;
				k += active[k].width - active[k].shrink <= w;
			}
			num_active = std::max(k, 1u);
		}
	}

	 /* Count the number of lines and allocate a heap line buffer if 
	  * required. */
	const Breakpoint *a, *b;
	unsigned num_lines = 0;
	for (b = breakpoints + num_breakpoints - 1; b->predecessor >= 0; 
		b = breakpoints + b->predecessor)
		num_lines++;
	if (num_lines > line_buffer_elements)
		line_buffer = new ParagraphLine[num_lines];
	*lines = line_buffer;

	/* Build the line objects from the breakpoint tree. The lowest-cost path 
	 * terminates at the final breakpoint, and we omit the breakpoint at 
	 * position 0. */
	line_buffer += num_lines - 1;
	for (b = breakpoints + num_breakpoints - 1; b->predecessor >= 0; b = a) {
		a = breakpoints + b->predecessor;
		line_buffer->a = a->next_box >= 0 ? a->next_box : a->b;
		line_buffer->b = b->b;
		line_buffer->demerits = b->total_demerits - a->total_demerits;
		line_buffer->line_demerits = b->debug_demerits;
		line_buffer->unscaled_width = b->unscaled_width;
		line_buffer->adjustment_ratio = b->adjustment_ratio;
		line_buffer--;
	}

	if (breakpoints != breakpoint_buffer)
		delete [] breakpoints;

	return num_lines;
}

} // namespace stkr
