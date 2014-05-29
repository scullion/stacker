#if defined(STACKER_WIN32)

#include "stacker_platform.h"

#define NOMINMAX
#include <Windows.h>

namespace stkr {

void platform_copy_to_clipboard(BackEnd *back_end, const char *text, unsigned length)
{
	back_end;

	HGLOBAL block_handle = GlobalAlloc(GMEM_MOVEABLE, length);
	memcpy(GlobalLock(block_handle), text, length);
	GlobalUnlock(block_handle);
	OpenClipboard(0);
	EmptyClipboard();
	SetClipboardData(CF_TEXT, block_handle);
	CloseClipboard();
}

} // namespace stkr

#endif // defined(STACKER_WIN32)
