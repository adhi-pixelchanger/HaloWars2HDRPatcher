// StatusIcon.h - the status indicator, drawn at run time.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace ui {

enum class Status {
    Idle,
    Busy,
    Success,
    Failure,
    Warning,
};

bool StartGraphics();
void StopGraphics();

void DrawStatus(HDC dc, const RECT& rect, Status status);

}  // namespace ui
