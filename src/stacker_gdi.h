#pragma once

#define NOMINMAX
#include <Windows.h>

namespace stkr {

void gdi_draw_view(struct View *view, HDC hdc, const RECT *dest);

} // namespace stkr
