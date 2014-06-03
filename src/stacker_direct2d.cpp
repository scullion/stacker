#if defined(STACKER_DIRECT2D)

#include <cwchar>

#include <algorithm>
#include <numeric>

#include "stacker_direct2d.h"
#include "stacker_platform.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>

#define STBI_HEADER_FILE_ONLY
#include "stb_image.c"

#include "url_cache.h"
#include "stacker_shared.h"
#include "stacker_system.h"
#include "stacker_document.h"
#include "stacker_node.h"
#include "stacker_view.h"
#include "stacker_util.h"
#include "stacker_view.h"
#include "stacker_layer.h"

namespace stkr {

using namespace urlcache;

struct NetworkImage {
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	ID2D1Bitmap *d2d_bitmap;
	uint32_t tint;
	unsigned use_count;
};

const unsigned RENDER_CACHE_CAPACITY = 64;

struct TextRunCacheEntry {
	uint64_t key;
	uint16_t *glyph_indices;
	float *glyph_advances;
	unsigned num_glyphs;
	unsigned width;
	unsigned height;
	unsigned last_used;
};

struct BackEnd {
	ID2D1Factory *d2d_factory;
	IDWriteFactory *dw_factory;
	ID2D1DCRenderTarget *d2d_rt;
	HWND rt_hwnd;
	RECT rt_bounds;
	UrlCache *url_cache;
	int image_notify_id;
	TextRunCacheEntry run_cache[RENDER_CACHE_CAPACITY];
	unsigned num_run_cache_entries;
	unsigned run_cache_clock;
};

struct BackEndFont {
	IDWriteFont *font;
	IDWriteFontFace *face;
	float em_size;
	float ascent;
	float cell_height;
};

extern const char * const DEFAULT_FONT_FACE        = "Segoe UI";
extern const unsigned     DEFAULT_FONT_SIZE        = 16 * 96 / 72;
extern const unsigned     DEFAULT_FONT_FLAGS       = 0;
extern const char * const DEFAULT_FIXED_FONT_FACE  = "Consolas";
extern const unsigned     DEFAULT_FIXED_FONT_SIZE  = 16 * 96 / 72;
extern const unsigned     DEFAULT_FIXED_FONT_FLAGS = 0;
extern const char * const DEBUG_LABEL_FONT_FACE    = "Consolas";
extern const unsigned     DEBUG_LABEL_FONT_SIZE    = 10 * 96 / 72;
extern const unsigned     DEBUG_LABEL_FONT_FLAGS   = 0;

static void d2d_trc_init(BackEnd *back_end)
{
	memset(back_end->run_cache, 0, sizeof(back_end->run_cache));
	back_end->num_run_cache_entries = 0;
	back_end->run_cache_clock = 0;
}

static void d2d_check(HRESULT hr, const char *op = "Direct2D call")
{
	if (SUCCEEDED(hr))
		return;
	char buf[512];
	sprintf(buf, "%s failed with HRESULT %.8Xh.", op, hr);
	MessageBoxA(NULL, buf, "Direct2D Error", MB_ICONINFORMATION | MB_OK);
	abort();
}

void *platform_match_font(BackEnd *back_end, const LogicalFont *info)
{
	bool match_default = info->face[0] == '\0';
	const char *src = match_default ? DEFAULT_FONT_FACE : info->face;
	
	unsigned flags = match_default ? DEFAULT_FONT_FLAGS : info->flags;
	wchar_t face_name[MAX_FONT_FACE_LENGTH + 1];
	mbsrtowcs(face_name, &src, strlen(src) + 1, NULL);

	int size = match_default ? DEFAULT_FONT_SIZE : info->font_size;
	if (size <= 0)
		return NULL;
	
	DWRITE_FONT_WEIGHT weight = (flags & STYLE_BOLD) ? 
		DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
	DWRITE_FONT_STYLE style = (flags & STYLE_ITALIC) ?
		DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

	IDWriteFontCollection *collection = NULL;
	IDWriteFontFamily *family = NULL;
	IDWriteFont *font = NULL;
	IDWriteFontFace *face = NULL;

	HRESULT hr = back_end->dw_factory->GetSystemFontCollection(&collection);
	d2d_check(hr, "GetSystemFontCollection");

	BOOL font_exists;
	unsigned family_index;
	hr = collection->FindFamilyName(face_name, &family_index, &font_exists);
	d2d_check(hr, "FindFamilyName");
	if (!font_exists)
		return NULL;

	hr = collection->GetFontFamily(family_index, &family);
	d2d_check(hr, "GetFontFamily");

	hr = family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, &font);
	d2d_check(hr, "GetFirstMatchingFont");

	hr = font->CreateFontFace(&face);
	d2d_check(hr, "CreateFontFace");

	BackEndFont *bef = new BackEndFont();
	bef->font = font;
	bef->face = face;
	bef->em_size = (float)size;

	DWRITE_FONT_METRICS metrics;
	bef->face->GetMetrics(&metrics);
	
	float pixels_per_design_unit = float(size) / float(metrics.designUnitsPerEm);
	bef->cell_height =	float(metrics.ascent + metrics.descent) * pixels_per_design_unit;
	bef->ascent = float(metrics.ascent) * pixels_per_design_unit;

	family->Release();
	collection->Release();

	return (void *)bef;
}

void platform_release_font(BackEnd *back_end, void *handle)
{
	back_end;
	if (handle != NULL) {
		BackEndFont *bef = (BackEndFont *)handle;
		if (bef->face != NULL)
			bef->face->Release();
		if (bef->font != NULL)
			bef->font->Release();
		delete bef;
	}
}

static void d2d_trc_clear_entry(TextRunCacheEntry *entry)
{
	if (entry->key != 0ULL) {
		delete [] entry->glyph_indices;
		delete [] entry->glyph_advances;
		entry->glyph_indices = NULL;
		entry->glyph_advances = NULL;
		entry->key = 0ULL;
	}
}

static void d2d_trc_clear(BackEnd *back_end)
{
	for (unsigned i = 0; i < RENDER_CACHE_CAPACITY; ++i)
		d2d_trc_clear_entry(back_end->run_cache + i);
	back_end->num_run_cache_entries = 0;
	back_end->run_cache_clock = 0;
}

static TextRunCacheEntry *d2d_trc_find(BackEnd *back_end, const char *text, 
	unsigned length, BackEndFont *bef)
{
	if (length == 0)
		return NULL;

	/* Look for an entry for this string, keeping track of the LRU entry 
	 * encountered in the probe chain. */
	uint64_t key = murmur3_64(text, length, (unsigned)bef);
	unsigned index = key % RENDER_CACHE_CAPACITY;
	TextRunCacheEntry *entry = NULL, *lru_entry = NULL;
	unsigned now = back_end->run_cache_clock++;
	for (;; index = (index + 1) % RENDER_CACHE_CAPACITY) {
		entry = back_end->run_cache + index;
		if (entry->key == key) {
			entry->last_used = now;
			return entry;
		}
		if (entry->key == 0ULL)
			break;
		if (lru_entry == NULL || entry->last_used < lru_entry->last_used)
			lru_entry = entry;
	}

	/* If the cache is at max-occupancy, replace the LRU entry. If we don't
	 * have one, don't insert a new entry. */
	if (back_end->num_run_cache_entries * 2 >= RENDER_CACHE_CAPACITY) {
		if (lru_entry != NULL) {
			entry = lru_entry;
		} else {
			/* The cache is full but we hit an empty slot. Bump out another
			 * entry at random. */
			do {
				index = (index + 1) % RENDER_CACHE_CAPACITY;
				lru_entry = back_end->run_cache + index;
			} while (lru_entry->key == 0ULL);
			d2d_trc_clear_entry(lru_entry);
		}
	} else {
		back_end->num_run_cache_entries++;
	}

	/* If this entry has been used before, delete the existing data. */
	d2d_trc_clear_entry(entry);

	/* Convert the text to UTF-16. */
	const char *src = text;
	wchar_t *text_utf16 = new wchar_t[length + 1];
	mbsrtowcs(text_utf16, &src, length, NULL);
	text_utf16[length] = L'\0';

	IDWriteTextAnalyzer *analyzer;
	HRESULT hr = back_end->dw_factory->CreateTextAnalyzer(&analyzer);
	d2d_check(hr, "CreateTextAnalyzer");

	DWRITE_SCRIPT_ANALYSIS script_analysis; 
	script_analysis.script = 0;
	script_analysis.shapes = DWRITE_SCRIPT_SHAPES_DEFAULT;

	DWRITE_SHAPING_TEXT_PROPERTIES *text_properties = 
		new DWRITE_SHAPING_TEXT_PROPERTIES[length];
	uint16_t *clusters = new uint16_t[length];
	uint32_t capacity = 3 * length / 2 + 16;
	uint32_t num_glyphs;
	DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_properties = NULL;
	uint16_t *glyph_indices = NULL;
	do {
		if (glyph_indices != NULL) {
			delete [] glyph_indices;
			delete [] glyph_properties;
		}
		glyph_indices = new uint16_t[capacity];
		glyph_properties = new DWRITE_SHAPING_GLYPH_PROPERTIES[capacity];
		hr = analyzer->GetGlyphs(
			text_utf16, length,
			bef->face, 
			FALSE, FALSE,
			&script_analysis, L"", 
			NULL, NULL, NULL, 0, 
			capacity, 
			clusters, 
			text_properties, 
			glyph_indices,
			glyph_properties,
			&num_glyphs);
		if (hr == S_OK)
			break;
		capacity *= 2;
	} while (hr == E_NOT_SUFFICIENT_BUFFER);
	d2d_check(hr, "GetGlyphs");

	float *glyph_advances = new float[num_glyphs];
	DWRITE_GLYPH_OFFSET *glyph_offsets = new DWRITE_GLYPH_OFFSET[num_glyphs];
	hr = analyzer->GetGlyphPlacements(
		text_utf16, 
		clusters, 
		text_properties, 
		length,
		glyph_indices,
		glyph_properties,
		num_glyphs,
		bef->face,
		bef->em_size,
		FALSE, FALSE,
		&script_analysis,
		L"", NULL, NULL, 0,
		glyph_advances, 
		glyph_offsets
	);
	d2d_check(hr, "GetGlyphPlacements");
	delete [] clusters;
	delete [] text_properties;
	delete [] glyph_properties;
	delete [] glyph_offsets;
	delete [] text_utf16;

	analyzer->Release();

	entry->width = round_signed(std::accumulate(glyph_advances, 
		glyph_advances + num_glyphs, 0.0f));
	entry->height = round_signed(bef->cell_height);

	entry->glyph_advances = glyph_advances;
	entry->glyph_indices = glyph_indices;
	entry->num_glyphs = num_glyphs;
	entry->last_used = back_end->run_cache_clock++;
	entry->key = key;

	return entry;
}

void platform_measure_text(BackEnd *back_end, void *font_handle, 
	const char *text, unsigned length, unsigned *width, unsigned *height, 
	unsigned *character_widths)
{
	const TextRunCacheEntry *rce = d2d_trc_find(back_end, text, length, 
		(BackEndFont *)font_handle);
	if (rce == NULL)
		return;
	if (width != NULL)
		*width = rce->width;
	if (height != NULL)
		*height = rce->height;
	if (character_widths != NULL) {
		for (unsigned i = 0; i < length; ++i) {
			character_widths[i] = unsigned(rce->glyph_advances[i] + 0.5f);
		}
	}
}

void platform_font_metrics(BackEnd *back_end, void *font_handle, 
	FontMetrics *result)
{
	back_end;
	const BackEndFont *bef = (BackEndFont *)font_handle;
	result->height = round_signed(bef->cell_height);
	result->em_width = result->height;
}

static void d2d_update_rt(BackEnd *be, HWND hwnd, HDC hdc, 
	const RECT *dest)
{
	hwnd;
	if (be->d2d_rt == NULL || hwnd != be->rt_hwnd || *dest != be->rt_bounds) {
		if (be->d2d_rt != NULL) {
			be->d2d_rt->Release();
			be->d2d_rt = NULL;
		}

		D2D1_RENDER_TARGET_PROPERTIES rtp;
		rtp.type = D2D1_RENDER_TARGET_TYPE_HARDWARE;
		rtp.usage = D2D1_RENDER_TARGET_USAGE_NONE;
		rtp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
		rtp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
		rtp.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
		rtp.dpiX = 96.0f;
		rtp.dpiY = 96.0f;

		HRESULT hr = be->d2d_factory->CreateDCRenderTarget(&rtp, &be->d2d_rt);
		d2d_check(hr, "CreateDCRenderTarget");
		be->d2d_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

		be->rt_hwnd = hwnd;
		be->rt_bounds = *dest;
	}
	HRESULT hr = be->d2d_rt->BindDC(hdc, dest);
	d2d_check(hr, "BindDC");
}

static D2D1_COLOR_F d2d_convert_color(uint32_t color)
{
	D2D1_COLOR_F cf;
	cf.r  = (1.0f / 255.0f) * float((color >>  0) & 0xFF);
	cf.g  = (1.0f / 255.0f) * float((color >>  8) & 0xFF);
	cf.b  = (1.0f / 255.0f) * float((color >> 16) & 0xFF);
	cf.a  = (1.0f / 255.0f) * float((color >> 24) & 0xFF);
	return cf;
}

static void d2d_draw_rectangle(BackEnd *be, const RectangleCommandData *data)
{
	ID2D1SolidColorBrush *fill_brush = NULL;
	ID2D1SolidColorBrush *border_brush = NULL;
	HRESULT hr;

	if ((data->fill_color & 0xFF000000) != 0) {
		hr = be->d2d_rt->CreateSolidColorBrush(
			d2d_convert_color(data->fill_color), &fill_brush); 
		d2d_check(hr, "CreateSolidColorBrush");
	}
	if ((data->border_color & 0xFF000000) != 0 && data->border_width != 0.0f) {
		hr = be->d2d_rt->CreateSolidColorBrush(
			d2d_convert_color(data->border_color), &border_brush); 
		d2d_check(hr, "CreateSolidColorBrush");
	}

	D2D1_RECT_F bounds;
	bounds.left = side(data->bounds, AXIS_H, 0);
	bounds.right = side(data->bounds, AXIS_H, 1);
	bounds.top = side(data->bounds, AXIS_V, 0);
	bounds.bottom = side(data->bounds, AXIS_V, 1);
	
	if (fill_brush != NULL)
		be->d2d_rt->FillRectangle(&bounds, fill_brush);
	if (border_brush != NULL)
		be->d2d_rt->DrawRectangle(&bounds, border_brush, data->border_width);

	if (fill_brush != NULL)
		fill_brush->Release();
	if (border_brush != NULL)
		border_brush->Release();
}


static void d2d_draw_text_run(BackEnd *be, const TextCommandData *d, 
	unsigned start, unsigned length, BackEndFont *font, int x, int y,
	ID2D1SolidColorBrush *brush)
{
	static const unsigned ADVANCE_BUFFER_SIZE = 256;

	TextRunCacheEntry *rce = d2d_trc_find(be, d->text + start, length, font);
	if (rce == NULL)
		return;
		
	/* Build an array of glyph advances from horizontal positions in the text 
	 * layer. */
	float static_advances[ADVANCE_BUFFER_SIZE];
	float *advances = length > ADVANCE_BUFFER_SIZE ? 
		new float[length] : static_advances;
	int last_x0 = x;
	for (unsigned i = 1; i < length; ++i) {
		int char_x0 = d->positions[2 * (start + i) + 0];
		int width = char_x0 - last_x0;
		advances[i - 1] = (float)width;
		last_x0 = char_x0;
	}
	advances[length - 1] = 0.0f;

	/* Draw the glyph run. */
	DWRITE_GLYPH_RUN glyph_run;
	glyph_run.fontFace = font->face;
	glyph_run.fontEmSize = font->em_size;
	glyph_run.glyphCount = length;
	glyph_run.glyphIndices = rce->glyph_indices;
	glyph_run.glyphAdvances = advances;
	glyph_run.glyphOffsets = NULL;
	glyph_run.isSideways = FALSE;
	glyph_run.bidiLevel = 0;
	D2D1_POINT_2F pos = D2D1::Point2F((float)x, (float)y + font->ascent);
	be->d2d_rt->DrawGlyphRun(pos, &glyph_run, brush);

	if (advances != static_advances)
		delete [] advances;
}

static void d2d_draw_text(BackEnd *be, View *view, const TextCommandData *d)
{
	if (d->length == 0)
		return;

	/* Make a brush for each palette entry. */
	static const unsigned NUM_STATIC_BRUSHES = 64;
	ID2D1SolidColorBrush *static_brushes[64], **brushes = static_brushes;
	if (d->num_colors > NUM_STATIC_BRUSHES)
		brushes = new ID2D1SolidColorBrush *[d->num_colors];
	for (unsigned i = 0; i < d->num_colors; ++i) {
		HRESULT hr = be->d2d_rt->CreateSolidColorBrush(
			d2d_convert_color(d->palette[i]), &brushes[i]);
		d2d_check(hr, "CreateSolidColorBrush");
	}

	/* Draw the text. */
	System *system = view->document->system;
	BackEndFont *font = (BackEndFont *)get_font_handle(system, d->font_id);
	int run_x0 = d->positions[0 + 0];
	int run_y0 = d->positions[0 + 1];
	int last_color_index = d->flags[0] & TLF_COLOR_INDEX_MASK;
	unsigned run_start = 0;
	for (unsigned i = 0; i < d->length; ++i) {
		int x = d->positions[2 * i + 0];
		int y = d->positions[2 * i + 1];
		unsigned flags = d->flags[i];
		int color_index = flags & TLF_COLOR_INDEX_MASK;
		if ((flags & (TLF_LINE_HEAD | TLF_STYLE_HEAD)) != 0 || 
			color_index != last_color_index) {
			unsigned run_length = i - run_start;
			d2d_draw_text_run(be, d, run_start, run_length, font, 
				run_x0, run_y0, brushes[last_color_index]);
			run_start = i;
			run_x0 = x;
			run_y0 = y;
			last_color_index = color_index;
		}
	}
	unsigned run_length = d->length - run_start;
	d2d_draw_text_run(be, d, run_start, run_length, font, 
		run_x0, run_y0, brushes[last_color_index]);

	for (unsigned i = 0; i < d->num_colors; ++i)
		brushes[i]->Release();
	if (brushes != static_brushes)
		delete [] brushes;
}

static ID2D1Bitmap *d2d_get_tinted_bitmap(BackEnd *back_end, 
	NetworkImage *ni, uint32_t tint)
{
	if (ni->pixels == NULL)
		return NULL;
	if (ni->d2d_bitmap != NULL && tint == ni->tint)
		return ni->d2d_bitmap;
	if (ni->d2d_bitmap != NULL) {
		ni->d2d_bitmap->Release();
		ni->d2d_bitmap = NULL;
	}

	uint32_t *tinted = (uint32_t *)ni->pixels;
	if (tint != 0xFFFFFFFF) {
		unsigned num_pixels = ni->width * ni->height;
		tinted = new uint32_t[num_pixels * 4];
		const uint32_t *source = (const uint32_t *)ni->pixels;
		uint32_t tint_premul = premultiply(tint);
		for (unsigned i = 0; i < num_pixels; ++i)
			tinted[i] = blend32(source[i], tint_premul);
	}

	D2D1_BITMAP_PROPERTIES props;
	props.dpiX = 0.0f;
	props.dpiY = 0.0f;
	props.pixelFormat.format = DXGI_FORMAT_R8G8B8A8_UNORM;
	props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
	HRESULT hr = back_end->d2d_rt->CreateBitmap(
		D2D1::SizeU(ni->width, ni->height),
		tinted, ni->width * 4, &props, &ni->d2d_bitmap);
	d2d_check(hr, "CreateBitmap");

	if (tinted != ni->pixels)
		delete [] tinted;
	ni->tint = tint;
	return ni->d2d_bitmap;
}

static void d2d_draw_image(BackEnd *be, const ImageCommandData *data)
{
	NetworkImage *ni = (NetworkImage *)(data->system_image);
	ID2D1Bitmap *bitmap = d2d_get_tinted_bitmap(be, ni, data->tint);
	if (bitmap == NULL)
		return;
	D2D1_RECT_F dest;
	dest.left   = side(data->bounds, AXIS_H, 0);
	dest.right  = side(data->bounds, AXIS_H, 1);
	dest.top    = side(data->bounds, AXIS_V, 0);
	dest.bottom = side(data->bounds, AXIS_V, 1);
	be->d2d_rt->DrawBitmap(bitmap, &dest);
}

static void d2d_set_clip(BackEnd *be, View *view, const ClipCommandData *cd, 
	bool has_clip)
{
	view;
	D2D1_RECT_F clip_rect = D2D1::RectF(
		rleft(cd->clip), 
		rtop(cd->clip), 
		rright(cd->clip), 
		rbottom(cd->clip));
	if (has_clip)
		be->d2d_rt->PopAxisAlignedClip();
	be->d2d_rt->PushAxisAlignedClip(&clip_rect, 
		D2D1_ANTIALIAS_MODE_ALIASED);
}

void d2d_draw_view(BackEnd *be, View *view, HWND hwnd, HDC hdc, 
	const RECT *dest)
{
	ViewCommandIterator iterator;
	const void *data;
	DrawCommand command = view_first_command(view, &iterator, &data);

	d2d_update_rt(be, hwnd, hdc, dest);

	/* Coordinates in view commands are document coordinates. This offset
	 * maps (view->x0, view->y0) to (dest->left, dest->top). */
	static const float PIXEL_CENTER_ADJUST = 0.5f;
	float offset_x = -rleft(view->bounds) + (float)dest->left - PIXEL_CENTER_ADJUST;
	float offset_y = -rtop(view->bounds) + (float)dest->top - PIXEL_CENTER_ADJUST;
	D2D_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Translation(offset_x, offset_y);
	be->d2d_rt->BeginDraw();
	be->d2d_rt->SetTransform(&transform);

	/* Process the command list. */
	bool has_clip = false;
	while (command != DCMD_END) {
		switch (command) {
			case DCMD_SET_CLIP:
				d2d_set_clip(be, view, (const ClipCommandData *)data, has_clip);
				has_clip = true;
				break;
			case DCMD_RECTANGLE:
				d2d_draw_rectangle(be, (const RectangleCommandData *)data);
				break;
			case DCMD_TEXT:
				d2d_draw_text(be, view, (const TextCommandData *)data);
				break;
			case DCMD_IMAGE:
				d2d_draw_image(be, (const ImageCommandData *)data);
				break;
		}
		command = view_next_command(&iterator, &data);
	}
	if (has_clip)
		be->d2d_rt->PopAxisAlignedClip();
	HRESULT hr = be->d2d_rt->EndDraw();
	d2d_check(hr, "EndDraw");
}

/*
 * Network Images
 */

static unsigned image_url_notify_callback(UrlHandle handle, 
	UrlNotification type, UrlKey key, BackEnd *back_end, NetworkImage *ni, 
	UrlFetchState fetch_state)
{
	handle; back_end; key; fetch_state;

	if (ni != NULL) {
		if (type == URL_NOTIFY_EVICT) {
			fprintf(stdout, "Destroying ID2D1Bitmap for %llX.\n", key);
			if (ni->d2d_bitmap != NULL)
				ni->d2d_bitmap->Release();
			if (ni->pixels != NULL)
				stbi_image_free(ni->pixels);
			delete ni;
			back_end->url_cache->destroy_handle(handle);
		} else if (type == URL_QUERY_EVICT) {
			if (ni->use_count != 0)
				return PREVENT_EVICT;
			return ni->width * ni->height * 4;
		}
	}
	return 0;
}


UrlHandle create_network_image_internal(BackEnd *back_end, UrlCache *cache, 
	UrlHandle handle)
{
	back_end;

	NetworkImage *image = (NetworkImage *)cache->user_data(handle);
	if (image == NULL) {
		image = new NetworkImage();
		image->pixels = NULL;
		image->width = 0;
		image->height = 0;
		image->d2d_bitmap = NULL;
		image->use_count = 0;
		cache->set_user_data(handle, image);
	}
	image->use_count++;
	return handle;
}

UrlHandle platform_create_network_image(BackEnd *back_end, 
	UrlCache *cache, const char *url)
{
	UrlHandle handle = cache->create_handle(url, -1, 
		URLP_NORMAL, DEFAULT_TTL_SECS,
		NULL, back_end->image_notify_id, 
		URL_FLAG_DISCARD | URL_FLAG_REUSE_SINK_HANDLE);
	return create_network_image_internal(back_end, cache, handle);
}

UrlHandle platform_create_network_image(BackEnd *back_end, 
	UrlCache *cache, UrlKey key)
{
	UrlHandle handle = cache->create_handle(key, 
		URLP_NORMAL, DEFAULT_TTL_SECS,
		NULL, back_end->image_notify_id, 
		URL_FLAG_DISCARD | URL_FLAG_REUSE_SINK_HANDLE);
	return create_network_image_internal(back_end, cache, handle);
}

void platform_destroy_network_image(BackEnd *back_end, 
	UrlCache *cache, UrlHandle image_handle)
{
	back_end;

	if (image_handle == INVALID_URL_HANDLE)
		return;
	cache->lock_cache();
	NetworkImage *image = (NetworkImage *)cache->user_data(image_handle);
	assertb(image != NULL && image->use_count != 0);
	--image->use_count;
	cache->unlock_cache();
}

static bool get_network_image_pixels(BackEnd *back_end, 
	UrlCache *cache, UrlHandle image_handle, NetworkImage *image)
{
	back_end;
	if (image_handle == INVALID_URL_HANDLE || image == NULL)
		return false;
	if (image->pixels != NULL)
		return true;
	unsigned data_size;
	const void *data = cache->lock(image_handle, &data_size);
	if (data == NULL)
		return false;
	int width, height;
	image->pixels = (uint32_t *)stbi_load_from_memory((const uint8_t *)data, 
		data_size, &width, &height, NULL, 4);
	for (int i = 0; i < width * height; ++i)
		image->pixels[i] = premultiply(image->pixels[i]);
	image->width = (unsigned)width;
	image->height = (unsigned)height;
	cache->unlock(image_handle);
	return image->pixels != NULL;
}

bool platform_get_network_image_info(BackEnd *back_end, UrlCache *cache, 
	UrlHandle image_handle, unsigned *width, unsigned *height)
{
	NetworkImage *image = (NetworkImage *)cache->user_data(image_handle);
	unsigned bitmap_width = 0;
	unsigned bitmap_height = 0;
	bool available = get_network_image_pixels(back_end, cache, 
		image_handle, image);
	if (available) {
		bitmap_width = image->width;
		bitmap_height = image->height;
	}
	if (width != NULL)
		*width = bitmap_width;
	if (height != NULL)
		*height = bitmap_height;
	return available;
}

void *platform_get_network_image_data(BackEnd *back_end, UrlCache *cache, 
	UrlHandle image_handle)
{
	back_end;
	NetworkImage *ni = (NetworkImage *)cache->user_data(image_handle);
	bool available = get_network_image_pixels(back_end, cache, 
		image_handle, ni);
	return available ? (void *)ni : NULL;
}

void platform_test_network_image(FILE *os)
{
	static const char * const IMAGE_URLS[] = {
		"http://upload.wikimedia.org/wikipedia/commons/4/43/07._Camel_Profile%2C_near_Silverton%2C_NSW%2C_07.07.2007.jpg",
		"http://upload.wikimedia.org/wikipedia/commons/3/36/Eryops_-_National_Museum_of_Natural_History_-_IMG_1974.JPG",
		"http://en.wikipedia.org/wiki/File:Russet_potato_cultivar_with_sprouts.jpg"
	};
	static const unsigned NUM_IMAGE_URLS = sizeof(IMAGE_URLS) / sizeof(IMAGE_URLS[0]);
	static const unsigned POLL_INTERVAL_MSEC = 100;
	static const unsigned RUN_TIME_MSEC = 15 * 1000;

	UrlCache cache;
	BackEnd *back_end = d2d_init(&cache);

	UrlHandle images[NUM_IMAGE_URLS];
	void *image_data[NUM_IMAGE_URLS];
	for (unsigned i = 0; i < NUM_IMAGE_URLS; ++i) {
		images[i] = platform_create_network_image(back_end, &cache, IMAGE_URLS[i]);
		image_data[i] = NULL;
	}
	for (unsigned poll_count = 0; ; ++poll_count) {
		unsigned elapsed = poll_count * POLL_INTERVAL_MSEC;
		if (elapsed > RUN_TIME_MSEC)
			break;
		float elapsed_secs = float(elapsed) * 1e-3f;
		for (unsigned i = 0; i < NUM_IMAGE_URLS; ++i) {
			const char *url = IMAGE_URLS[i];
			UrlHandle image_handle = images[i];
			void *new_data = platform_get_network_image_data(back_end, 
				&cache, image_handle);
			if (new_data == image_data[i])
				continue;
			fprintf(os, "[%3.2f] Image data for %s changed to %xh.\n", 
				elapsed_secs, url, new_data);
			if (new_data != NULL) {
				BITMAP info;
				GetObject((HBITMAP)new_data, sizeof(info), &info);
				fprintf(os, "\tBitmap is %ux%u @ %u bits/pixel, total %uKB.\n", 
					info.bmWidth, info.bmHeight, info.bmBitsPixel,
					(info.bmHeight * info.bmWidthBytes) / 1024);
			}
			image_data[i] = new_data;
		}
		cache.update();
		Sleep(POLL_INTERVAL_MSEC);
	}
	d2d_deinit(back_end);
}

BackEnd *d2d_init(UrlCache *url_cache)
{
	BackEnd *be = new BackEnd();

	d2d_trc_init(be);

	D2D1_FACTORY_OPTIONS factory_options;
	factory_options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
	HRESULT hr = D2D1CreateFactory(
		D2D1_FACTORY_TYPE_SINGLE_THREADED,
		IID_ID2D1Factory,
        &factory_options, 
		(void **)&be->d2d_factory);
	d2d_check(hr, "D2D1CreateFactory");

	hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, 
		__uuidof(IDWriteFactory), (IUnknown **)&be->dw_factory);
	d2d_check(hr, "DWriteCreateFactory");

	be->d2d_rt = NULL;
	memset(&be->rt_bounds, 0, sizeof(RECT));

	be->url_cache = url_cache;
	be->image_notify_id = INVALID_NOTIFY_SINK_ID;
	if (url_cache != NULL) {
		be->image_notify_id = url_cache->add_notify_sink(
			(NotifyCallback)&image_url_notify_callback, be);
	}

	be->d2d_rt = NULL;
	be->rt_hwnd = NULL;
	memset(&be->rt_bounds, 0, sizeof(RECT));
	
	return be;
}

void d2d_deinit(BackEnd *be)
{
	d2d_trc_clear(be);
	if (be->image_notify_id != INVALID_NOTIFY_SINK_ID)
		be->url_cache->remove_notify_sink(be->image_notify_id);
	if (be->d2d_rt != NULL)
		be->d2d_rt->Release();
	if (be->dw_factory != NULL)
		be->dw_factory->Release();
	if (be->d2d_factory != NULL)
		be->d2d_factory->Release();
	delete be;
}

} // namespace stkr

#endif // defined(STACKER_DIRECT2D)

