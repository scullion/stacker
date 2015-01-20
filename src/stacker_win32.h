#pragma once

#define NOMINMAX
#include <Windows.h>

namespace stkr {

struct TimerValue { 
	LARGE_INTEGER time;
};

} // namespace stkr
