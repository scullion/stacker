#pragma once

#include <cstdint>
#include <cctype>
#include <cfloat>
#include <cmath>

#include "stacker.h"
#include "stacker_shared.h"

namespace stkr {

inline unsigned set_or_clear(unsigned word, unsigned mask, bool value)
{
	return word ^ (mask & (-int(value) ^ word));
}

inline unsigned bitfield_read(unsigned w, unsigned shift, unsigned mask)
{
	return (w & mask) >> shift;
}

inline unsigned bitfield_write(unsigned w, unsigned shift, unsigned mask, 
	unsigned value)
{
	return (w & ~mask) | (value << shift);
}

/* Returns the index of the lowest set bit in 'word'. Returns zero if the word
 * is zero. */
inline unsigned lowest_set_bit(unsigned word)
{
	static const unsigned DEBRUIJN[32] = { 
		 0,  1, 28,  2, 29, 14, 24, 3, 
		30, 22, 20, 15, 25, 17,  4, 8, 
		31, 27, 13, 23, 21, 19, 16, 7, 
		26, 12, 18,  6, 11,  5, 10, 9
	};
	unsigned lsb = word & -int(word);
	return DEBRUIJN[(lsb * 0x077CB531u) >> 27];
}

/* Compares two wrapping time stamps. Valid only under the assumption that B
 * does not move ahead of A by more than 2^31. */
inline bool stamp_less(uint32_t a, uint32_t b)
{
	return int32_t(a - b) < 0;
}

inline int16_t saturate16(int n)
{
	if (n > INT16_MAX)
		n = INT16_MAX;
	else if (n < INT16_MIN)
		n = INT16_MIN;
	return (int16_t)n;
}

inline int16_t check16(int n)
{
	ensure(n >= INT16_MIN && n <= INT16_MAX);
	return int16_t(n);
}

inline uint16_t check16(unsigned n)
{
	ensure(n <= UINT16_MAX);
	return uint16_t(n);
}

inline uint32_t blend32(uint32_t a, uint32_t b)
{
	uint32_t cr = (((a >>  0) & 0xFF) * ((b >>  0) & 0xFF)) >>  8;
	uint32_t cg = (((a >>  8) & 0xFF) * ((b >>  8) & 0xFF)) >>  0;
	uint32_t cb = (((a >> 16) & 0xFF) * ((b >> 16) & 0xFF)) <<  8;
	uint32_t ca = (((a >> 24) & 0xFF) * ((b >> 24) & 0xFF)) << 16;
	return cr + (cg & 0xFF00) + (cb & 0xFF0000) + (ca & 0xFF000000);
}

inline uint32_t lerp32(uint32_t a, uint32_t b, uint32_t alpha)
{
	uint32_t a_rg = a & 0x00FF00FF;
	uint32_t b_rg = b & 0x00FF00FF;
	uint32_t a_ba = (a & 0xFF00FF00) >> 8;
	uint32_t b_ba = (b & 0xFF00FF00) >> 8;
	uint32_t c_rg = (a_rg * alpha + b_rg * (255 - alpha)) >> 8;
	uint32_t c_ba = (a_ba * alpha + b_ba * (255 - alpha));
	return (c_rg & 0x00FF00FF) | (c_ba & 0xFF00FF00);
}

inline uint32_t premultiply(uint32_t color)
{
	return blend32(color, 0xFF000000 | ((color >> 24) * 0x010101));
}

inline int round_signed(float n)
{
	return n >= 0.0f ? int(n + 0.5f) : int(n - 0.5f);
}

/* Rounds N up to the next power of two. */
inline unsigned next_power_of_two(unsigned n)
{
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	return n + 1;
}

/* Indexes a rectangle stored as an array of the form {x0, x1, y0, y1}. */
inline float side(const float *r, Axis axis_h, unsigned s)
{
	return r[2 * axis_h + s];
}

/* Indexes a rectangle stored as an array of the form {x0, x1, y0, y1}. */
inline float *sidep(float *r, Axis axis_h, unsigned s)
{
	return r + 2 * axis_h + s;
}

/* Returns the width or height of a rectangle store as an array of the form 
 * {x0, x1, y0, y1}. */
inline float rdim(const float *r, Axis axis)
{
	return side(r, axis, 1) - side(r, axis, 0);
}

/* Convenience accessors. */
inline float rleft  (const float *r) { return side(r, AXIS_H, 0); }
inline float rright (const float *r) { return side(r, AXIS_H, 1); }
inline float rtop   (const float *r) { return side(r, AXIS_V, 0); }
inline float rbottom(const float *r) { return side(r, AXIS_V, 1); }
inline float rwidth (const float *r) { return rdim(r, AXIS_H); }
inline float rheight(const float *r) { return rdim(r, AXIS_V); }

inline void rset(float *r, float x0, float x1, float y0, float y1)
{
	r[0] = x0; r[1] = x1; r[2] = y0; r[3] = y1;
}

/* Compares two rectangles using an absolute tolerance. */
inline bool requal(const float *a, const float *b, 
	float tolerance = FLT_EPSILON)
{
	return fabsf(a[0] - b[0]) <= tolerance && 
	       fabsf(a[1] - b[1]) <= tolerance && 
	       fabsf(a[2] - b[2]) <= tolerance && 
	       fabsf(a[3] - b[3]) <= tolerance;
}

inline float clip(float n, float a, float b)
{
	if (n <= a) return a;
	if (n >= b) return b;
	return n;
}

/* Tests for overlap between two half-open intervals. */
inline bool overlap(unsigned a0, unsigned a1, unsigned b0, unsigned b1)
{
	return a0 < b1 && b0 < a1;
}

inline bool rectangles_overlap(float ax0, float ax1, float ay0, float ay1, 
							   float bx0, float bx1, float by0, float by1)
{
 	return (ax0 <= bx1 && bx0 <= ax1) && (ay0 <= by1 && by0 <= ay1);
}

inline void rect_intersect(const float a[4], const float b[4], float *r)
{
	r[0] = a[0] >= b[0] ? a[0] : b[0];
	r[1] = a[1] <= b[1] ? a[1] : b[1];
	r[2] = a[2] >= b[2] ? a[2] : b[2];
	r[3] = a[3] <= b[3] ? a[3] : b[3];
}

inline void rect_union(const float a[4], const float b[4], float *r)
{
	r[0] = a[0] <= b[0] ? a[0] : b[0];
	r[1] = a[1] >= b[1] ? a[1] : b[1];
	r[2] = a[2] <= b[2] ? a[2] : b[2];
	r[3] = a[3] >= b[3] ? a[3] : b[3];
}

void align_1d(enum Alignment alignment, float dim, float offset,
	float a0, float a1, float *b0, float *b1);
void align_rectangle(enum Alignment align_h, enum Alignment align_v,
	float width, float height, float offset_x, float offset_y,
	const float *bounds, float *result);
float relative_dimension(enum DimensionMode mode, float specified, 
	float container, float value_if_undefined);
float band_distance(float x, float a, float b);
float rectangle_selection_distance(float x, float y, 
	float bx0, float bx1, float by0, float by1);

const char *random_word(uintptr_t seed);

void list_insert_before(void **head, void **tail, void *item, void *next, 
	unsigned offset);
void list_remove(void **head, void **tail, void *item, unsigned offset);

int32_t int_to_fixed(int32_t n, unsigned q);
int32_t round_float_to_fixed(float n, unsigned q);
int32_t fixed_multiply(int32_t a, int32_t b, unsigned q);
int32_t fixed_divide(int32_t a, int32_t b, unsigned q);
int32_t round_fixed(int32_t n, unsigned q);
int32_t round_fixed_to_int(int32_t n, unsigned q);
int32_t fixed_ceil_as_int(int32_t n, unsigned q);
double fixed_to_double(int32_t n, unsigned q);

extern const float INFINITE_RECTANGLE[4];

} // namespace stkr
