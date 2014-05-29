#pragma once

#define NOMINMAX
#include <Windows.h>

namespace urlcache { class UrlCache; }

namespace stkr {

struct View;
struct BackEnd;

BackEnd *d2d_init(urlcache::UrlCache *url_cache = 0);
void d2d_deinit(BackEnd *back_end);
void d2d_draw_view(BackEnd *back_end, View *view, HWND hwnd, HDC hdc, 
	const RECT *dest);

} // namespace stkr
