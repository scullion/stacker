#if defined(STACKER_WIN32)

#include "stacker_platform.h"

#include <cstdint>

#define NOMINMAX
#include <Windows.h>

namespace stkr {

/*
 * Platform
 */

void platform_copy_to_clipboard(BackEnd *, const void *text, 
	unsigned length)
{
	unsigned buffer_size = (length + 1) * sizeof(wchar_t);
	HGLOBAL block_handle = GlobalAlloc(GMEM_MOVEABLE, buffer_size);
	memcpy(GlobalLock(block_handle), text, buffer_size);
	GlobalUnlock(block_handle);
	OpenClipboard(0);
	EmptyClipboard();
	SetClipboardData(CF_TEXT, block_handle); /* System takes ownership. */
	CloseClipboard();
}

/*
 * Timing
 */

TimerValue platform_query_timer(void)
{
	TimerValue now;
	QueryPerformanceCounter(&now.time);
	return now;
}

bool platform_check_timeout(TimerValue start, uintptr_t timeout)
{
	LARGE_INTEGER frequency, now;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&now);
	uint64_t delta = now.QuadPart - start.time.QuadPart;
	uint64_t timeout_ticks = timeout * frequency.QuadPart / uint64_t(1e6);
	return delta >= timeout_ticks;
}

} // namespace stkr

#endif // defined(STACKER_WIN32)
