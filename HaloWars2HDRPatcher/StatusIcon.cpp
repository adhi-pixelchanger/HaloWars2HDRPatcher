// StatusIcon.cpp - drawing the status indicator with GDI+.

#include "StatusIcon.h"

#include <algorithm>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace ui {
namespace {

ULONG_PTR g_token = 0;

struct Palette {
    Color fill;
    Color edge;
    Color glyph;
};

Palette PaletteFor(Status status) {
    switch (status) {
        case Status::Success:
            return {Color(255, 30, 142, 62), Color(255, 22, 105, 46),
                    Color(255, 255, 255, 255)};
        case Status::Failure:
            return {Color(255, 196, 43, 43), Color(255, 146, 30, 30),
                    Color(255, 255, 255, 255)};
        case Status::Warning:
        case Status::Busy:
            return {Color(255, 214, 148, 26), Color(255, 160, 110, 18),
                    Color(255, 255, 255, 255)};
        case Status::Idle:
        default:
            return {Color(255, 236, 236, 236), Color(255, 168, 168, 168),
                    Color(255, 130, 130, 130)};
    }
}

void DrawTick(Graphics& g, const RectF& box, const Pen& pen) {
    const REAL x = box.X;
    const REAL y = box.Y;
    const REAL w = box.Width;
    const REAL h = box.Height;

    PointF points[3] = {
        PointF(x + w * 0.24f, y + h * 0.52f),
        PointF(x + w * 0.43f, y + h * 0.70f),
        PointF(x + w * 0.76f, y + h * 0.31f),
    };
    g.DrawLines(&pen, points, 3);
}

void DrawCross(Graphics& g, const RectF& box, const Pen& pen) {
    const REAL x = box.X;
    const REAL y = box.Y;
    const REAL w = box.Width;
    const REAL h = box.Height;

    g.DrawLine(&pen, PointF(x + w * 0.31f, y + h * 0.31f),
               PointF(x + w * 0.69f, y + h * 0.69f));
    g.DrawLine(&pen, PointF(x + w * 0.69f, y + h * 0.31f),
               PointF(x + w * 0.31f, y + h * 0.69f));
}

void DrawBar(Graphics& g, const RectF& box, const Pen& pen) {
    const REAL x = box.X;
    const REAL y = box.Y;
    const REAL w = box.Width;
    const REAL h = box.Height;

    g.DrawLine(&pen, PointF(x + w * 0.50f, y + h * 0.26f),
               PointF(x + w * 0.50f, y + h * 0.56f));
    g.DrawLine(&pen, PointF(x + w * 0.50f, y + h * 0.70f),
               PointF(x + w * 0.50f, y + h * 0.73f));
}

}  // namespace

bool StartGraphics() {
    GdiplusStartupInput input;
    return GdiplusStartup(&g_token, &input, nullptr) == Ok;
}

void StopGraphics() {
    if (g_token != 0) {
        GdiplusShutdown(g_token);
        g_token = 0;
    }
}

void DrawStatus(HDC dc, const RECT& rect, Status status) {
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);

    const REAL side = static_cast<REAL>(
        std::min(rect.right - rect.left, rect.bottom - rect.top));
    const REAL inset = side * 0.08f;
    const REAL diameter = side - inset * 2.0f;

    const REAL left = static_cast<REAL>(rect.left) +
                      (static_cast<REAL>(rect.right - rect.left) - side) / 2.0f + inset;
    const REAL top = static_cast<REAL>(rect.top) +
                     (static_cast<REAL>(rect.bottom - rect.top) - side) / 2.0f + inset;

    const RectF box(left, top, diameter, diameter);
    const Palette palette = PaletteFor(status);

    SolidBrush fill(palette.fill);
    graphics.FillEllipse(&fill, box);

    Pen edge(palette.edge, std::max(1.0f, diameter * 0.055f));
    graphics.DrawEllipse(&edge, box);

    Pen glyph(palette.glyph, std::max(1.5f, diameter * 0.12f));
    glyph.SetStartCap(LineCapRound);
    glyph.SetEndCap(LineCapRound);
    glyph.SetLineJoin(LineJoinRound);

    switch (status) {
        case Status::Success: DrawTick(graphics, box, glyph); break;
        case Status::Failure: DrawCross(graphics, box, glyph); break;
        case Status::Warning:
        case Status::Busy:    DrawBar(graphics, box, glyph); break;
        case Status::Idle:
        default: {
            // A hollow ring reads as "nothing has happened yet".
            SolidBrush hole(Color(255, 255, 255, 255));
            const REAL pad = diameter * 0.28f;
            graphics.FillEllipse(&hole, RectF(box.X + pad, box.Y + pad,
                                              box.Width - pad * 2.0f,
                                              box.Height - pad * 2.0f));
            break;
        }
    }
}

}  // namespace ui
