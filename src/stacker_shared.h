#pragma once

#include <cstdlib>
#include <cstdio>

namespace stkr {

#pragma warning(disable: 4505) // unreferenced local function has been removed

#if defined(NDEBUG)
	#pragma warning(disable: 4100) // unreferenced formal parameter
	#pragma warning(disable: 4189) // local variable is initialized but not referenced
	#pragma warning(disable: 4530) // C++ exception handler used but unwind semantics disabled
#endif

#if defined(_MSC_VER) && !defined(snprintf)
	#define snprintf _snprintf
#endif

// #define ensure(p) ((p) ? (void)0 : abort())
#define ensure(p) if (!(p)) { __asm { int 3 }; exit(1); }
#pragma warning(disable: 4127) // conditional expression is constant
#if defined(NDEBUG)
	#define assertb(p)
#else
	#define assertb(p) if (!(p)) { __asm { int 3 }; }
#endif

#define docmsgp(flag, fmt, ...) if (get_flags(document) & flag) document_dump(document, (fmt), __VA_ARGS__);
#define dmsg(fmt, ...) docmsgp(-1, fmt, __VA_ARGS__)
#define lmsg(fmt, ...) docmsgp(DOCFLAG_DEBUG_LAYOUT, fmt, __VA_ARGS__);

} // namespace stkr
