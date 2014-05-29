#pragma once

#include <cstdio>

namespace urlcache { 
	class UrlCache; 
	typedef void *UrlHandle;
	typedef unsigned long long UrlKey; 
}

namespace stkr {

struct BackEnd;
struct LogicalFont;
struct FontMetrics;

/*
 * Font Handling
 */
void *platform_match_font(BackEnd *back_end, const LogicalFont *info);
void platform_release_font(BackEnd *back_end, void *handle);
void platform_measure_text(BackEnd *back_end, void *font_handle, 
	const char *text, unsigned length, unsigned *width, unsigned *height, 
	unsigned *character_widths);
void platform_font_metrics(BackEnd *back_end, void *font_handle, 
	FontMetrics *result);

/*
 * Network Images
 */
urlcache::UrlHandle platform_create_network_image(BackEnd *back_end, 
	urlcache::UrlCache *cache, const char *url);
urlcache::UrlHandle platform_create_network_image(BackEnd *back_end, 
	urlcache::UrlCache *cache, urlcache::UrlKey key);
void platform_destroy_network_image(BackEnd *back_end, 
	urlcache::UrlCache *cache, urlcache::UrlHandle image_handle);
bool platform_get_network_image_info(BackEnd *back_end, 
	urlcache::UrlCache *cache, urlcache::UrlHandle image_handle, 
	unsigned *width = 0, unsigned *height = 0);
void *platform_get_network_image_data(BackEnd *back_end, 
	urlcache::UrlCache *cache, urlcache::UrlHandle image_handle);
void platform_test_network_image(FILE *os);

/*
 * GUI
 */
void platform_copy_to_clipboard(BackEnd *back_end, const char *text, 
	unsigned length);

} // namespace stkr

