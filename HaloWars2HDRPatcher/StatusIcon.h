// StatusIcon.h - the status indicator, drawn at run time.
//
// No image resources: the glyph is composed from GDI+ primitives so it scales
// with the control and picks up the system DPI without a bitmap to go stale.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX   // StatusIcon.cpp uses std::min / std::max
#include <windows.h>

namespace ui {

enum class Status {
    Idle,      // grey ring
    Busy,      // amber ring with a dot
    Success,   // green disc with a tick
    Failure,   // red disc with a cross
    Warning,   // amber disc with a bar
};

bool StartGraphics();
void StopGraphics();

// Paints the indicator into the whole of rect, centred and square.
void DrawStatus(HDC dc, const RECT& rect, Status status);

}  // namespace ui
