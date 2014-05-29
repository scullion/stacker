#if defined(STACKER_GDI)

#include "stacker_gdi.h"
#include "stacker_platform.h"

#define NOMINMAX
#include <Windows.h>

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

namespace stkr {

static void gdi_draw_line(View *view, HDC hdc, const LineCommandData *data)
{
	view;

	int x0 = round_signed(data->x0);
	int y0 = round_signed(data->y0);
	int x1 = round_signed(data->x1);
	int y1 = round_signed(data->y1);
	int width = std::max(1, round_signed(data->width));

	HANDLE pen_handle = CreatePen(PS_SOLID, width, data->color);
	HANDLE old_pen_handle = SelectObject(hdc, pen_handle);
	MoveToEx(hdc, x0, y0, NULL);
	LineTo(hdc, x1, y1);
	SelectObject(hdc, old_pen_handle);
}

static void gdi_draw_rectangle(View *view, HDC hdc, const RectangleCommandData *data)
{
	view;

	int x0 = round_signed(side(data->bounds, AXIS_H, 0));
	int x1 = round_signed(side(data->bounds, AXIS_H, 1));
	int y0 = round_signed(side(data->bounds, AXIS_V, 0));
	int y1 = round_signed(side(data->bounds, AXIS_V, 1));
	int border_width = round_signed(data->border_width);

	HANDLE pen = NULL, old_pen = NULL;
	HANDLE brush = NULL, old_brush = NULL;

	if ((data->fill_color & 0xFF000000) != 0) {
		COLORREF gdi_color = data->fill_color & 0x00FFFFFF; // No alpha support.
		brush = CreateSolidBrush(gdi_color);
		old_brush = SelectObject(hdc, brush);
	} else {
		SelectObject(hdc, GetStockObject(NULL_BRUSH));
	}

	if ((data->border_color & 0xFF000000) != 0 && border_width) {
		COLORREF gdi_color = data->border_color & 0x00FFFFFF; // No alpha support.
		pen = CreatePen(PS_SOLID, border_width, gdi_color);
		old_pen = SelectObject(hdc, pen);
	} else {
		SelectObject(hdc, GetStockObject(NULL_PEN));
	}

	Rectangle(hdc, x0, y0, x1, y1);
	
	if (old_pen != NULL)
		SelectObject(hdc, old_pen);
	if (old_brush != NULL)
		SelectObject(hdc, old_brush);
	if (pen != NULL)
		DeleteObject(pen);
	if (brush != NULL)
		DeleteObject(brush);
}

static void gdi_draw_text(View *view, HDC hdc, const TextCommandData *d)
{
	System *system = view->document->system;
	HANDLE font_handle = (HANDLE)get_font_handle(system, d->font_id);
	SelectObject(hdc, font_handle);
	SetTextAlign(hdc, TA_LEFT | TA_TOP);
	COLORREF gdi_color = d->color & 0x00FFFFFF;
	SetTextColor(hdc, gdi_color);
	SetBkMode(hdc, TRANSPARENT);
	for (unsigned i = 0; i < d->length; ++i) {
		TextOut(hdc, d->positions[2 * i + 0], d->positions[2 * i + 1], 
			d->text + i, 1);
	}
	//ExtTextOutA(hdc, 0, 0, ETO_PDY, NULL, d->text, d->length, d->positions);
}

static void gdi_draw_image(View *view, HDC hdc, const ImageCommandData *data)
{
	view;
	
	int dest_x0 = round_signed(side(data->bounds, AXIS_H, 0));
	int dest_x1 = round_signed(side(data->bounds, AXIS_H, 1));
	int dest_y0 = round_signed(side(data->bounds, AXIS_V, 0));
	int dest_y1 = round_signed(side(data->bounds, AXIS_V, 1));
	int dest_width = dest_x1 - dest_x0;
	int dest_height = dest_y1 - dest_y0;

	HDC memdc_handle = CreateCompatibleDC(NULL);
	SelectObject(memdc_handle, (HBITMAP)data->system_image);
	BitBlt(hdc, dest_x0, dest_y0, dest_width, dest_height, 
		memdc_handle, 0, 0, SRCCOPY);
	DeleteDC(memdc_handle);
}

void gdi_draw_view(View *view, HDC hdc, const RECT *dest)
{
	ViewCommandIterator iterator;
	const void *data;
	DrawCommand command = view_first_command(view, &iterator, &data);

	/* Clip to the destination rectangle. */
	IntersectClipRect(hdc, dest->left, dest->top, dest->right, dest->bottom);
	
	/* Coordinates in view commands are document coordinates. This offset
	 * maps (view->x0, view->y0) to (dest->left, dest->top). */
	POINT old_origin;
	int offset_x = -round_signed(view->x0) + dest->left;
	int offset_y = -round_signed(view->y0) + dest->top;
	SetViewportOrgEx(hdc, offset_x, offset_y, &old_origin);

	/* Process the command list. */
	while (command != DCMD_END) {
		switch (command) {
			case DCMD_LINE:
				gdi_draw_line(view, hdc, (const LineCommandData *)data);
				break;
			case DCMD_RECTANGLE:
				gdi_draw_rectangle(view, hdc, (const RectangleCommandData *)data);
				break;
			case DCMD_TEXT:
				gdi_draw_text(view, hdc, (const TextCommandData *)data);
				break;
			case DCMD_IMAGE:
				gdi_draw_image(view, hdc, (const ImageCommandData *)data);
				break;
		}
		command = view_next_command(&iterator, &data);
	}

	SetViewportOrgEx(hdc, old_origin.x, old_origin.y, NULL);
}

} // namespace stkr

#endif // defined(STACKER_GDI)
