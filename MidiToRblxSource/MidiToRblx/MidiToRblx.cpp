#include "framework.h"
#include "MidiToRblx.h"

#include "debug_monitor.hpp"
#include "midi_file.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <objidl.h>
#include <gdiplus.h>
#include <sstream>
#include <string>
#include <thread>
#include <uxtheme.h>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowTitle[] = L"MidiToRblx";
constexpr UINT_PTR kProgressTimer = 1;
constexpr int kDeleteHotkeyId = 1;
constexpr UINT kLoadFinishedMessage = WM_APP + 1;
constexpr UINT kPlaybackFinishedMessage = WM_APP + 2;
constexpr float kWoodTextureZoom = 0.35F;
constexpr float kMetalTextureZoom = 0.50F;
constexpr float kButtonCornerRadius = 6.0F;
constexpr BYTE kButtonHoverAlpha = 52;
constexpr wchar_t kButtonHoverProperty[] = L"MidiToRblx.TexturedButtonHover";

class EmbeddedTexture {
public:
    EmbeddedTexture() = default;
    EmbeddedTexture(const EmbeddedTexture&) = delete;
    EmbeddedTexture& operator=(const EmbeddedTexture&) = delete;

    ~EmbeddedTexture() {
        bitmap_.reset();
        if (stream_ != nullptr) {
            stream_->Release();
        }
    }

    bool Load(HINSTANCE instance, int resourceId) {
        const HRSRC resource =
            FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (resource == nullptr) {
            return false;
        }
        const DWORD size = SizeofResource(instance, resource);
        const HGLOBAL loaded = LoadResource(instance, resource);
        const void* source = loaded == nullptr ? nullptr : LockResource(loaded);
        if (size == 0 || source == nullptr) {
            return false;
        }
        const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
        if (memory == nullptr) {
            return false;
        }
        void* destination = GlobalLock(memory);
        if (destination == nullptr) {
            GlobalFree(memory);
            return false;
        }
        std::memcpy(destination, source, size);
        GlobalUnlock(memory);

        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
            GlobalFree(memory);
            return false;
        }
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(stream, FALSE);
        if (bitmap->GetLastStatus() != Gdiplus::Ok || bitmap->GetWidth() == 0 ||
            bitmap->GetHeight() == 0) {
            bitmap.reset();
            stream->Release();
            return false;
        }
        stream_ = stream;
        bitmap_ = std::move(bitmap);
        return true;
    }

    Gdiplus::Bitmap* Get() const {
        return bitmap_.get();
    }

private:
    IStream* stream_ = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> bitmap_;
};

class TexturedUi {
public:
    explicit TexturedUi(HINSTANCE instance) {
        woodLoaded_ = wood_.Load(instance, IDR_WOOD_TEXTURE);
        metalLoaded_ = metal_.Load(instance, IDR_METAL_TEXTURE);
    }

    bool IsReady() const {
        return woodLoaded_ && metalLoaded_;
    }

    void Prepare(HWND dialog) {
        for (const int id : kButtonIds) {
            HWND button = GetDlgItem(dialog, id);
            if (button == nullptr) {
                continue;
            }
            const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
            SetWindowLongPtrW(button, GWL_STYLE,
                              (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) |
                                  BS_OWNERDRAW);
            SetWindowTheme(button, L"", L"");
            SetWindowPos(button, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
            SetWindowSubclass(button, &TexturedUi::TexturedButtonProcedure, 4, 0);
        }
        EnumChildWindows(dialog, &TexturedUi::PrepareControl,
                         reinterpret_cast<LPARAM>(this));
        InvalidateRect(dialog, nullptr, TRUE);
    }

    void DrawBackground(HWND dialog, HDC deviceContext) const {
        RECT client{};
        GetClientRect(dialog, &client);
        Gdiplus::Graphics graphics(deviceContext);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const Gdiplus::Rect bounds(client.left, client.top,
                                   client.right - client.left,
                                   client.bottom - client.top);
        DrawSurface(graphics, bounds);
        graphics.Flush(Gdiplus::FlushIntentionSync);
        for (HWND groupBox : groupBoxes_) {
            DrawGroupBox(dialog, deviceContext, groupBox);
        }
    }

    bool DrawControl(const DRAWITEMSTRUCT* item) const {
        if (item == nullptr) {
            return false;
        }
        if (item->CtlType != ODT_BUTTON ||
            !IsTexturedButton(static_cast<int>(item->CtlID))) {
            return false;
        }
        const int pixelWidth = item->rcItem.right - item->rcItem.left;
        const int pixelHeight = item->rcItem.bottom - item->rcItem.top;
        if (pixelWidth <= 1 || pixelHeight <= 1) {
            return true;
        }
        HDC drawingContext = item->hDC;
        HDC memoryContext = CreateCompatibleDC(item->hDC);
        HBITMAP memoryBitmap = nullptr;
        HGDIOBJ previousBitmap = nullptr;
        RECT drawingBounds = item->rcItem;
        if (memoryContext != nullptr) {
            memoryBitmap = CreateCompatibleBitmap(item->hDC, pixelWidth, pixelHeight);
            if (memoryBitmap != nullptr) {
                previousBitmap = SelectObject(memoryContext, memoryBitmap);
                if (previousBitmap != nullptr && previousBitmap != HGDI_ERROR) {
                    drawingContext = memoryContext;
                    drawingBounds = RECT{0, 0, pixelWidth, pixelHeight};
                } else {
                    DeleteObject(memoryBitmap);
                    memoryBitmap = nullptr;
                    DeleteDC(memoryContext);
                    memoryContext = nullptr;
                }
            } else {
                DeleteDC(memoryContext);
                memoryContext = nullptr;
            }
        }
        const float width = static_cast<float>(pixelWidth);
        const float height = static_cast<float>(pixelHeight);
        DrawControlBackground(item->hwndItem, drawingContext);
        const float dpiScale = static_cast<float>(GetDpiForWindow(item->hwndItem)) / 96.0F;
        const float radius = std::min(kButtonCornerRadius * dpiScale, height * 0.45F);
        const bool hovered =
            GetPropW(item->hwndItem, kButtonHoverProperty) != nullptr;
        const Gdiplus::RectF shapeRect(
            static_cast<float>(drawingBounds.left) + 0.75F,
            static_cast<float>(drawingBounds.top) + 0.75F,
            width - 1.5F, height - 1.5F);
        Gdiplus::GraphicsPath path;
        AddRoundedRectangle(path, shapeRect, radius);

        {
            Gdiplus::Graphics graphics(drawingContext);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            const Gdiplus::GraphicsState state = graphics.Save();
            graphics.SetClip(&path);
            const Gdiplus::Rect textureBounds(
                drawingBounds.left, drawingBounds.top,
                drawingBounds.right - drawingBounds.left,
                drawingBounds.bottom - drawingBounds.top);
            if (metal_.Get() != nullptr) {
                DrawTexture(graphics, *metal_.Get(), textureBounds, kMetalTextureZoom);
            } else {
                Gdiplus::SolidBrush fallback(Gdiplus::Color(255, 190, 194, 196));
                graphics.FillPath(&fallback, &path);
            }
            if (hovered && (item->itemState & ODS_DISABLED) == 0) {
                Gdiplus::SolidBrush hoverOverlay(
                    Gdiplus::Color(kButtonHoverAlpha, 35, 145, 235));
                graphics.FillPath(&hoverOverlay, &path);
            }
            if ((item->itemState & ODS_SELECTED) != 0) {
                Gdiplus::SolidBrush pressed(Gdiplus::Color(70, 0, 0, 0));
                graphics.FillPath(&pressed, &path);
            }
            if ((item->itemState & ODS_DISABLED) != 0) {
                Gdiplus::SolidBrush disabled(Gdiplus::Color(110, 238, 238, 238));
                graphics.FillPath(&disabled, &path);
            }
            graphics.Restore(state);
            const Gdiplus::Color borderColor =
                (item->itemState & ODS_FOCUS) != 0
                    ? Gdiplus::Color(255, 225, 225, 225)
                    : hovered && (item->itemState & ODS_DISABLED) == 0
                          ? Gdiplus::Color(255, 92, 176, 235)
                    : Gdiplus::Color(255, 105, 108, 110);
            Gdiplus::Pen border(borderColor, 1.5F * dpiScale);
            border.SetAlignment(Gdiplus::PenAlignmentInset);
            graphics.DrawPath(&border, &path);
        }

        std::array<wchar_t, 256> text{};
        GetWindowTextW(item->hwndItem, text.data(), static_cast<int>(text.size()));
        RECT textBounds = drawingBounds;
        if ((item->itemState & ODS_SELECTED) != 0) {
            const int offset = std::max(1, static_cast<int>(std::lround(dpiScale)));
            OffsetRect(&textBounds, offset, offset);
        }
        const int previousMode = SetBkMode(drawingContext, TRANSPARENT);
        const COLORREF previousColor = SetTextColor(
            drawingContext,
            (item->itemState & ODS_DISABLED) != 0 ? RGB(105, 105, 105)
                                                  : RGB(24, 27, 29));
        const HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
        const HGDIOBJ previousFont =
            font == nullptr ? nullptr : SelectObject(drawingContext, font);
        DrawTextW(drawingContext, text.data(), -1, &textBounds,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (previousFont != nullptr) {
            SelectObject(drawingContext, previousFont);
        }
        SetTextColor(drawingContext, previousColor);
        SetBkMode(drawingContext, previousMode);
        if ((item->itemState & ODS_FOCUS) != 0 &&
            (item->itemState & ODS_NOFOCUSRECT) == 0) {
            RECT focus = drawingBounds;
            InflateRect(&focus, -4, -4);
            DrawFocusRect(drawingContext, &focus);
        }
        if (memoryContext != nullptr) {
            BitBlt(item->hDC, item->rcItem.left, item->rcItem.top,
                   pixelWidth, pixelHeight, memoryContext, 0, 0, SRCCOPY);
            SelectObject(memoryContext, previousBitmap);
            DeleteObject(memoryBitmap);
            DeleteDC(memoryContext);
        }
        return true;
    }

    static HBRUSH TransparentControlBrush(HDC deviceContext) {
        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, RGB(247, 239, 226));
        return reinterpret_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
    }

private:
    static constexpr std::array<int, 9> kButtonIds{
        IDC_REFRESH_INPUTS,
        IDC_CONNECT_INPUT,
        IDC_STOP_INPUT,
        IDC_SELECT_FILE,
        IDC_PLAY_FILE,
        IDC_PAUSE_FILE,
        IDC_STOP_FILE,
        MONITOR_BUTTON,
        CREDITS_BUTTON,
    };

    static bool IsTexturedButton(int id) {
        return std::find(kButtonIds.begin(), kButtonIds.end(), id) !=
               kButtonIds.end();
    }

    static LRESULT CALLBACK TexturedButtonProcedure(HWND control, UINT message,
                                                    WPARAM wParam, LPARAM lParam,
                                                    UINT_PTR, DWORD_PTR) {
        if (message == WM_ERASEBKGND) {
            return TRUE;
        }
        if (message == WM_MOUSEMOVE &&
            GetPropW(control, kButtonHoverProperty) == nullptr) {
            TRACKMOUSEEVENT tracking{
                sizeof(tracking),
                TME_LEAVE,
                control,
                0,
            };
            if (TrackMouseEvent(&tracking)) {
                SetPropW(control, kButtonHoverProperty,
                         reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1)));
                InvalidateRect(control, nullptr, FALSE);
            }
        }
        if (message == WM_MOUSELEAVE) {
            if (RemovePropW(control, kButtonHoverProperty) != nullptr) {
                InvalidateRect(control, nullptr, FALSE);
            }
        }
        if (message == WM_NCDESTROY) {
            RemovePropW(control, kButtonHoverProperty);
            RemoveWindowSubclass(control,
                                 &TexturedUi::TexturedButtonProcedure, 4);
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    void DrawSurface(Gdiplus::Graphics& graphics,
                     const Gdiplus::Rect& bounds) const {
        if (wood_.Get() != nullptr) {
            DrawTexture(graphics, *wood_.Get(), bounds, kWoodTextureZoom);
        } else {
            Gdiplus::SolidBrush fallback(Gdiplus::Color(255, 93, 43, 22));
            graphics.FillRectangle(&fallback, bounds);
        }
    }

    void DrawControlBackground(HWND control, HDC deviceContext) const {
        HWND parent = GetParent(control);
        if (parent == nullptr) {
            return;
        }
        RECT parentClient{};
        GetClientRect(parent, &parentClient);
        POINT origin{};
        MapWindowPoints(control, parent, &origin, 1);
        Gdiplus::Graphics graphics(deviceContext);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const Gdiplus::GraphicsState state = graphics.Save();
        graphics.TranslateTransform(static_cast<Gdiplus::REAL>(-origin.x),
                                    static_cast<Gdiplus::REAL>(-origin.y));
        const Gdiplus::Rect bounds(parentClient.left, parentClient.top,
                                   parentClient.right - parentClient.left,
                                   parentClient.bottom - parentClient.top);
        DrawSurface(graphics, bounds);
        graphics.Restore(state);
    }

    static void DrawGroupBox(HWND dialog, HDC deviceContext, HWND groupBox) {
        RECT bounds{};
        GetWindowRect(groupBox, &bounds);
        MapWindowPoints(HWND_DESKTOP, dialog,
                        reinterpret_cast<POINT*>(&bounds), 2);
        std::array<wchar_t, 256> text{};
        GetWindowTextW(groupBox, text.data(), static_cast<int>(text.size()));
        const HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(groupBox, WM_GETFONT, 0, 0));
        const HGDIOBJ previousFont =
            font == nullptr ? nullptr : SelectObject(deviceContext, font);
        SIZE textSize{};
        GetTextExtentPoint32W(deviceContext, text.data(),
                              static_cast<int>(wcslen(text.data())), &textSize);
        const float dpiScale = static_cast<float>(GetDpiForWindow(groupBox)) / 96.0F;
        const float left = static_cast<float>(bounds.left) + 0.75F;
        const float right = static_cast<float>(bounds.right) - 0.75F;
        const float top = static_cast<float>(bounds.top) +
                          std::max(4.0F, textSize.cy * 0.5F);
        const float bottom = static_cast<float>(bounds.bottom) - 0.75F;
        const float textLeft = left + 8.0F * dpiScale;
        const float gapLeft = textLeft - 4.0F * dpiScale;
        const float gapRight = textLeft + textSize.cx + 4.0F * dpiScale;
        {
            Gdiplus::Graphics graphics(deviceContext);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen border(Gdiplus::Color(235, 238, 238, 238),
                                std::max(1.0F, dpiScale));
            graphics.DrawLine(&border, left, top, std::max(left, gapLeft), top);
            graphics.DrawLine(&border, std::min(right, gapRight), top, right, top);
            graphics.DrawLine(&border, right, top, right, bottom);
            graphics.DrawLine(&border, right, bottom, left, bottom);
            graphics.DrawLine(&border, left, bottom, left, top);
        }
        RECT textBounds{
            static_cast<LONG>(std::lround(textLeft)),
            bounds.top,
            bounds.right,
            bounds.top + textSize.cy,
        };
        const int previousMode = SetBkMode(deviceContext, TRANSPARENT);
        const COLORREF previousColor = SetTextColor(deviceContext, RGB(247, 239, 226));
        DrawTextW(deviceContext, text.data(), -1, &textBounds,
                  DT_LEFT | DT_TOP | DT_SINGLELINE);
        SetTextColor(deviceContext, previousColor);
        SetBkMode(deviceContext, previousMode);
        if (previousFont != nullptr) {
            SelectObject(deviceContext, previousFont);
        }
    }

    void DrawStaticControl(HWND control, HDC deviceContext) const {
        DrawControlBackground(control, deviceContext);
        RECT bounds{};
        GetClientRect(control, &bounds);
        const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
        const LONG_PTR type = style & SS_TYPEMASK;
        if (type == SS_ETCHEDHORZ) {
            const float middle = (bounds.bottom - bounds.top) * 0.5F;
            Gdiplus::Graphics graphics(deviceContext);
            Gdiplus::Pen shadow(Gdiplus::Color(185, 70, 70, 70), 1.0F);
            Gdiplus::Pen light(Gdiplus::Color(225, 238, 238, 238), 1.0F);
            graphics.DrawLine(&shadow, static_cast<float>(bounds.left), middle,
                              static_cast<float>(bounds.right), middle);
            graphics.DrawLine(&light, static_cast<float>(bounds.left), middle + 1.0F,
                              static_cast<float>(bounds.right), middle + 1.0F);
            return;
        }
        std::array<wchar_t, 1024> text{};
        GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
        UINT format = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
        if (type == SS_CENTER) {
            format |= DT_CENTER;
        } else if (type == SS_RIGHT) {
            format |= DT_RIGHT;
        } else {
            format |= DT_LEFT;
        }
        if ((style & SS_ENDELLIPSIS) != 0) {
            format |= DT_END_ELLIPSIS;
        }
        const HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(control, WM_GETFONT, 0, 0));
        const HGDIOBJ previousFont =
            font == nullptr ? nullptr : SelectObject(deviceContext, font);
        const int previousMode = SetBkMode(deviceContext, TRANSPARENT);
        const COLORREF previousColor =
            SetTextColor(deviceContext, RGB(247, 239, 226));
        DrawTextW(deviceContext, text.data(), -1, &bounds, format);
        SetTextColor(deviceContext, previousColor);
        SetBkMode(deviceContext, previousMode);
        if (previousFont != nullptr) {
            SelectObject(deviceContext, previousFont);
        }
    }

    static LRESULT CALLBACK StaticControlProcedure(HWND control, UINT message,
                                                   WPARAM wParam, LPARAM lParam,
                                                   UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<TexturedUi*>(data);
        if (message == WM_ERASEBKGND) {
            return TRUE;
        }
        if (message == WM_PAINT && self != nullptr) {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(control, &paint);
            self->DrawStaticControl(control, deviceContext);
            EndPaint(control, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT && self != nullptr) {
            self->DrawStaticControl(control, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control,
                                 &TexturedUi::StaticControlProcedure, 5);
        }
        const LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        if (message == WM_SETTEXT || message == WM_SETFONT ||
            message == WM_ENABLE) {
            RedrawWindow(control, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        }
        return result;
    }

    void DrawSysLink(HWND control, HDC deviceContext) const {
        DrawControlBackground(control, deviceContext);
        RECT bounds{};
        GetClientRect(control, &bounds);
        const HFONT baseFont = reinterpret_cast<HFONT>(
            SendMessageW(control, WM_GETFONT, 0, 0));
        LOGFONTW description{};
        HFONT linkFont = nullptr;
        if (baseFont != nullptr &&
            GetObjectW(baseFont, sizeof(description), &description) == sizeof(description)) {
            description.lfUnderline = TRUE;
            linkFont = CreateFontIndirectW(&description);
        }
        const HFONT selectedFont = linkFont != nullptr ? linkFont : baseFont;
        const HGDIOBJ previousFont = selectedFont == nullptr
                                             ? nullptr
                                             : SelectObject(deviceContext, selectedFont);
        const int previousMode = SetBkMode(deviceContext, TRANSPARENT);
        const COLORREF previousColor = SetTextColor(deviceContext, RGB(92, 183, 255));
        DrawTextW(deviceContext, L"MidiToRblx by Axtorz", -1, &bounds,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (GetFocus() == control) {
            RECT focus = bounds;
            InflateRect(&focus, -1, -1);
            DrawFocusRect(deviceContext, &focus);
        }
        SetTextColor(deviceContext, previousColor);
        SetBkMode(deviceContext, previousMode);
        if (previousFont != nullptr) {
            SelectObject(deviceContext, previousFont);
        }
        if (linkFont != nullptr) {
            DeleteObject(linkFont);
        }
    }

    static LRESULT CALLBACK SysLinkProcedure(HWND control, UINT message,
                                             WPARAM wParam, LPARAM lParam,
                                             UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<TexturedUi*>(data);
        if (message == WM_ERASEBKGND && self != nullptr) {
            self->DrawControlBackground(control, reinterpret_cast<HDC>(wParam));
            return TRUE;
        }
        if (message == WM_PAINT && self != nullptr) {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(control, &paint);
            self->DrawSysLink(control, deviceContext);
            EndPaint(control, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT && self != nullptr) {
            self->DrawSysLink(control, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, &TexturedUi::SysLinkProcedure, 1);
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    void DrawCheckBox(HWND control, HDC deviceContext) const {
        DrawControlBackground(control, deviceContext);
        RECT client{};
        GetClientRect(control, &client);
        const UINT dpi = GetDpiForWindow(control);
        const int boxSize = std::max(11, MulDiv(13, dpi, 96));
        const int boxTop = client.top +
                           std::max(0, (static_cast<int>(client.bottom) - boxSize) / 2);
        RECT box{client.left, boxTop, client.left + boxSize, boxTop + boxSize};
        const LRESULT state = SendMessageW(control, BM_GETCHECK, 0, 0);
        {
            Gdiplus::Graphics graphics(deviceContext);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const Gdiplus::RectF square(
                static_cast<float>(box.left) + 0.5F,
                static_cast<float>(box.top) + 0.5F,
                static_cast<float>(box.right - box.left) - 1.0F,
                static_cast<float>(box.bottom - box.top) - 1.0F);
            Gdiplus::SolidBrush fill(IsWindowEnabled(control)
                                         ? Gdiplus::Color(255, 245, 245, 245)
                                         : Gdiplus::Color(255, 195, 195, 195));
            graphics.FillRectangle(&fill, square);
            Gdiplus::Pen border(Gdiplus::Color(255, 92, 95, 97), 1.0F);
            graphics.DrawRectangle(&border, square);
            if (state == BST_CHECKED) {
                Gdiplus::Pen check(Gdiplus::Color(255, 32, 35, 37),
                                   std::max(1.5F, dpi / 72.0F));
                check.SetStartCap(Gdiplus::LineCapRound);
                check.SetEndCap(Gdiplus::LineCapRound);
                const std::array<Gdiplus::PointF, 3> points{
                    Gdiplus::PointF(square.X + square.Width * 0.20F,
                                    square.Y + square.Height * 0.53F),
                    Gdiplus::PointF(square.X + square.Width * 0.43F,
                                    square.Y + square.Height * 0.75F),
                    Gdiplus::PointF(square.X + square.Width * 0.82F,
                                    square.Y + square.Height * 0.26F),
                };
                graphics.DrawLines(&check, points.data(),
                                   static_cast<INT>(points.size()));
            } else if (state == BST_INDETERMINATE) {
                Gdiplus::SolidBrush mark(Gdiplus::Color(255, 85, 88, 90));
                const float inset = std::max(2.0F, square.Width * 0.27F);
                graphics.FillRectangle(&mark,
                                       square.X + inset,
                                       square.Y + inset,
                                       square.Width - inset * 2.0F,
                                       square.Height - inset * 2.0F);
            }
        }
        std::array<wchar_t, 256> text{};
        GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
        RECT textBounds = client;
        textBounds.left = box.right + std::max(3, MulDiv(4, dpi, 96));
        const HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(control, WM_GETFONT, 0, 0));
        const HGDIOBJ previousFont =
            font == nullptr ? nullptr : SelectObject(deviceContext, font);
        const int previousMode = SetBkMode(deviceContext, TRANSPARENT);
        const COLORREF previousColor = SetTextColor(
            deviceContext,
            IsWindowEnabled(control) ? RGB(247, 239, 226) : RGB(165, 160, 153));
        DrawTextW(deviceContext, text.data(), -1, &textBounds,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (GetFocus() == control) {
            RECT focus = textBounds;
            SIZE textSize{};
            GetTextExtentPoint32W(deviceContext, text.data(),
                                  static_cast<int>(wcslen(text.data())), &textSize);
            focus.right = std::min(focus.right, focus.left + textSize.cx + 2);
            InflateRect(&focus, 1, -1);
            DrawFocusRect(deviceContext, &focus);
        }
        SetTextColor(deviceContext, previousColor);
        SetBkMode(deviceContext, previousMode);
        if (previousFont != nullptr) {
            SelectObject(deviceContext, previousFont);
        }
    }

    static LRESULT CALLBACK CheckBoxProcedure(HWND control, UINT message,
                                              WPARAM wParam, LPARAM lParam,
                                              UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<TexturedUi*>(data);
        if (message == WM_ERASEBKGND) {
            return TRUE;
        }
        if (message == WM_PAINT && self != nullptr) {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(control, &paint);
            self->DrawCheckBox(control, deviceContext);
            EndPaint(control, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT && self != nullptr) {
            self->DrawCheckBox(control, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, &TexturedUi::CheckBoxProcedure, 3);
        }
        const LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        if (message == BM_SETCHECK || message == WM_LBUTTONUP ||
            message == WM_KEYUP || message == WM_SETFOCUS ||
            message == WM_KILLFOCUS || message == WM_ENABLE) {
            InvalidateRect(control, nullptr, FALSE);
        }
        return result;
    }

    static BOOL CALLBACK PrepareControl(HWND control, LPARAM data) {
        auto* self = reinterpret_cast<TexturedUi*>(data);
        std::array<wchar_t, 32> className{};
        GetClassNameW(control, className.data(), static_cast<int>(className.size()));
        const bool sysLink = _wcsicmp(className.data(), L"SysLink") == 0;
        const bool staticControl = _wcsicmp(className.data(), L"Static") == 0;
        bool transparent = staticControl;
        if (staticControl && self != nullptr) {
            const LONG_PTR extendedStyle = GetWindowLongPtrW(control, GWL_EXSTYLE);
            SetWindowLongPtrW(control, GWL_EXSTYLE,
                              extendedStyle &
                                  ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
            SetWindowSubclass(control, &TexturedUi::StaticControlProcedure, 5,
                              reinterpret_cast<DWORD_PTR>(self));
            transparent = false;
        }
        if (sysLink) {
            const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
            SetWindowLongPtrW(control, GWL_STYLE, style | LWS_TRANSPARENT);
            const LONG_PTR extendedStyle = GetWindowLongPtrW(control, GWL_EXSTYLE);
            SetWindowLongPtrW(control, GWL_EXSTYLE,
                              extendedStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
            SetWindowPos(control, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
            SetWindowSubclass(control, &TexturedUi::SysLinkProcedure, 1,
                              reinterpret_cast<DWORD_PTR>(self));
        }
        if (_wcsicmp(className.data(), L"Button") == 0) {
            const LONG_PTR type = GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK;
            const bool checkBox = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
                                  type == BS_3STATE || type == BS_AUTO3STATE;
            if (type == BS_GROUPBOX && self != nullptr) {
                self->groupBoxes_.push_back(control);
                ShowWindow(control, SW_HIDE);
                transparent = false;
            } else if (checkBox && self != nullptr) {
                SetWindowTheme(control, L"", L"");
                const LONG_PTR extendedStyle =
                    GetWindowLongPtrW(control, GWL_EXSTYLE);
                SetWindowLongPtrW(
                    control, GWL_EXSTYLE,
                    extendedStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
                SetWindowSubclass(control, &TexturedUi::CheckBoxProcedure, 3,
                                  reinterpret_cast<DWORD_PTR>(self));
                transparent = false;
            } else {
                transparent = type == BS_RADIOBUTTON ||
                              type == BS_AUTORADIOBUTTON;
            }
            if (transparent) {
                SetWindowTheme(control, L"", L"");
            }
        }
        if (transparent) {
            const LONG_PTR extendedStyle = GetWindowLongPtrW(control, GWL_EXSTYLE);
            SetWindowLongPtrW(control, GWL_EXSTYLE,
                              extendedStyle | WS_EX_TRANSPARENT);
        }
        return TRUE;
    }

    static void AddRoundedRectangle(Gdiplus::GraphicsPath& path,
                                    const Gdiplus::RectF& rectangle,
                                    float radius) {
        const float diameter = std::min(radius * 2.0F,
                                        std::min(rectangle.Width, rectangle.Height));
        if (diameter <= 0.0F) {
            path.AddRectangle(rectangle);
            return;
        }
        path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0F, 90.0F);
        path.AddArc(rectangle.GetRight() - diameter, rectangle.Y,
                    diameter, diameter, 270.0F, 90.0F);
        path.AddArc(rectangle.GetRight() - diameter,
                    rectangle.GetBottom() - diameter,
                    diameter, diameter, 0.0F, 90.0F);
        path.AddArc(rectangle.X, rectangle.GetBottom() - diameter,
                    diameter, diameter, 90.0F, 90.0F);
        path.CloseFigure();
    }

    static void DrawTexture(Gdiplus::Graphics& graphics,
                            Gdiplus::Bitmap& bitmap,
                            const Gdiplus::Rect& bounds,
                            float zoom) {
        const float safeZoom = std::max(0.01F, zoom);
        const int width = std::max(
            1, static_cast<int>(std::lround(bitmap.GetWidth() * safeZoom)));
        const int height = std::max(
            1, static_cast<int>(std::lround(bitmap.GetHeight() * safeZoom)));
        int startX = bounds.X + (bounds.Width - width) / 2;
        int startY = bounds.Y + (bounds.Height - height) / 2;
        while (startX > bounds.X) {
            startX -= width;
        }
        while (startY > bounds.Y) {
            startY -= height;
        }
        for (int y = startY; y < bounds.GetBottom(); y += height) {
            for (int x = startX; x < bounds.GetRight(); x += width) {
                graphics.DrawImage(&bitmap, Gdiplus::Rect(x, y, width, height),
                                   0, 0, static_cast<int>(bitmap.GetWidth()),
                                   static_cast<int>(bitmap.GetHeight()),
                                   Gdiplus::UnitPixel);
            }
        }
    }

    EmbeddedTexture wood_;
    EmbeddedTexture metal_;
    bool woodLoaded_ = false;
    bool metalLoaded_ = false;
    std::vector<HWND> groupBoxes_;
};

struct EmitResult {
    std::string output;
    UINT submittedInputs = 0;
    DWORD error = ERROR_SUCCESS;
};

std::wstring WinMmError(MMRESULT result) {
    std::array<wchar_t, MAXERRORLENGTH> text{};
    if (midiInGetErrorTextW(result, text.data(), static_cast<UINT>(text.size())) == MMSYSERR_NOERROR) {
        return text.data();
    }
    return L"Windows MIDI error " + std::to_wstring(result) + L".";
}

std::wstring Win32Error(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"No additional Win32 error information is available.";
    }
    std::array<wchar_t, 512> buffer{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    std::wstring message = length == 0 ? L"Unknown error." : std::wstring(buffer.data(), length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return L"Win32 error " + std::to_wstring(error) + L": " + message;
}

class KeyEmitter {
public:
    static EmitResult Send(std::uint8_t identifier, std::uint8_t value) {
        static constexpr std::array<WORD, 12> kBase12ScanCodes{
            0x52,
            0x4F,
            0x50,
            0x51,
            0x4B,
            0x4C,
            0x4D,
            0x47,
            0x48,
            0x49,
            0x4A,
            0x4E,
        };
        static constexpr WORD kMultiplyScanCode = 0x37;

        const auto digits = protocol::Encode(identifier, value);

        std::array<INPUT, 10> inputs{};
        FillKeyPair(inputs.data(), kMultiplyScanCode);
        for (std::size_t index = 0; index < digits.size(); ++index) {
            FillKeyPair(inputs.data() + 2 + index * 2, kBase12ScanCodes[digits[index]]);
        }

        SetLastError(ERROR_SUCCESS);
        EmitResult result;
        result.output = protocol::Format(identifier, value);
        result.submittedInputs =
            SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        if (result.submittedInputs != inputs.size()) {
            result.error = GetLastError();
            if (result.error == ERROR_SUCCESS) {
                result.error = ERROR_WRITE_FAULT;
            }
        }
        return result;
    }

    static EmitResult SendEvent(midi::EventKind kind, std::uint8_t data1,
                                std::uint8_t data2) {
        switch (kind) {
            case midi::EventKind::NoteOn:
                return Send(data1, data2);
            case midi::EventKind::NoteOff:
                return Send(data1, 0);
            case midi::EventKind::ControlChange:
                if (data1 == 64) {
                    return Send(143, data2);
                }
                break;
            case midi::EventKind::ProgramChange:
            case midi::EventKind::PitchBend:
            case midi::EventKind::Aftertouch:
            case midi::EventKind::PolyAftertouch:
            case midi::EventKind::SysEx:
            case midi::EventKind::Unknown:
                break;
        }
        return {};
    }

private:
    static void FillKeyPair(INPUT* output, WORD scanCode) {
        output[0].type = INPUT_KEYBOARD;
        output[0].ki.wScan = scanCode;
        output[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        output[1] = output[0];
        output[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    }
};

class MidiInput {
public:
    explicit MidiInput(DebugMonitor& monitor) : monitor_(monitor) {}
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    ~MidiInput() { Stop(); }

    bool Start(UINT deviceId, std::wstring& error) {
        Stop();
        HMIDIIN opened = nullptr;
        MMRESULT result = midiInOpen(&opened, deviceId,
                                     reinterpret_cast<DWORD_PTR>(&MidiInput::Callback),
                                     reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR) {
            error = WinMmError(result);
            return false;
        }
        handle_ = opened;
        deviceId_ = deviceId;
        for (LongBuffer& buffer : longBuffers_) {
            buffer.header = {};
            buffer.header.lpData = buffer.data.data();
            buffer.header.dwBufferLength = static_cast<DWORD>(buffer.data.size());
            result = midiInPrepareHeader(handle_, &buffer.header, sizeof(buffer.header));
            if (result != MMSYSERR_NOERROR) {
                error = WinMmError(result);
                ReleaseHandle();
                return false;
            }
            buffer.prepared = true;
            result = midiInAddBuffer(handle_, &buffer.header, sizeof(buffer.header));
            if (result != MMSYSERR_NOERROR) {
                error = WinMmError(result);
                ReleaseHandle();
                return false;
            }
        }
        active_.store(true, std::memory_order_release);
        result = midiInStart(handle_);
        if (result != MMSYSERR_NOERROR) {
            active_.store(false, std::memory_order_release);
            error = WinMmError(result);
            ReleaseHandle();
            return false;
        }
        return true;
    }

    void Stop() {
        active_.store(false, std::memory_order_release);
        ReleaseHandle();
    }

    [[nodiscard]] bool IsActive() const {
        return active_.load(std::memory_order_acquire);
    }

private:
    struct LongBuffer {
        MIDIHDR header{};
        std::array<char, 4096> data{};
        bool prepared = false;
    };

    static void CALLBACK Callback(HMIDIIN, UINT message, DWORD_PTR instance,
                                  DWORD_PTR parameter1, DWORD_PTR) {
        if (instance == 0) {
            return;
        }
        auto* self = reinterpret_cast<MidiInput*>(instance);
        if (!self->active_.load(std::memory_order_acquire)) {
            return;
        }
        if (message == MIM_DATA || message == MIM_ERROR) {
            self->ProcessShortMessage(static_cast<std::uint32_t>(parameter1),
                                      message == MIM_ERROR);
        } else if (message == MIM_LONGDATA || message == MIM_LONGERROR) {
            self->ProcessLongMessage(reinterpret_cast<MIDIHDR*>(parameter1),
                                     message == MIM_LONGERROR);
        }
    }

    static int DataLength(std::uint8_t status) {
        if (status >= 0x80U && status <= 0xEFU) {
            const std::uint8_t category = status & 0xF0U;
            return category == 0xC0U || category == 0xD0U ? 1 : 2;
        }
        switch (status) {
            case 0xF1U:
            case 0xF3U:
                return 1;
            case 0xF2U:
                return 2;
            default:
                return 0;
        }
    }

    void ProcessShortMessage(std::uint32_t packed, bool invalid) {
        const auto started = std::chrono::steady_clock::now();
        const std::uint8_t status = static_cast<std::uint8_t>(packed & 0xFFU);
        const std::uint8_t data1 = static_cast<std::uint8_t>((packed >> 8U) & 0x7FU);
        const std::uint8_t data2 = static_cast<std::uint8_t>((packed >> 16U) & 0x7FU);
        if (status >= 0xF1U && status != 0xF7U) {
            return;
        }
        const int length = DataLength(status);
        EmitResult emitted;
        if (!invalid) {
            switch (status & 0xF0U) {
                case 0x80U:
                    emitted = KeyEmitter::SendEvent(midi::EventKind::NoteOff, data1, data2);
                    break;
                case 0x90U:
                    emitted = KeyEmitter::SendEvent(midi::EventKind::NoteOn, data1, data2);
                    break;
                case 0xB0U:
                    emitted = KeyEmitter::SendEvent(midi::EventKind::ControlChange, data1, data2);
                    break;
                default:
                    break;
            }
        }
        const auto completed = std::chrono::steady_clock::now();
        MonitorEvent event;
        event.timestamp = started;
        event.source = MonitorSource::Live;
        event.type = ClassifyMonitorEvent(status, data1, data2);
        event.severity = invalid ? MonitorSeverity::Error : MonitorSeverity::Normal;
        event.deviceId = deviceId_;
        event.status = status;
        event.data1 = data1;
        event.data2 = data2;
        event.hasData1 = length >= 1;
        event.hasData2 = length >= 2;
        event.output = std::move(emitted.output);
        event.latencyMicroseconds =
            std::chrono::duration<double, std::micro>(completed - started).count();
        if (invalid) {
            event.message = L"Invalid short MIDI message";
        }
        monitor_.Record(std::move(event));
        if (emitted.error != ERROR_SUCCESS) {
            monitor_.RecordError(L"SendInput failed after " +
                                 std::to_wstring(emitted.submittedInputs) +
                                 L" of 10 input actions. Windows error " +
                                 std::to_wstring(emitted.error) + L".");
        }
    }

    void ProcessLongMessage(MIDIHDR* header, bool invalid) {
        const auto started = std::chrono::steady_clock::now();
        if (header != nullptr && header->dwBytesRecorded != 0) {
            MonitorEvent event;
            event.timestamp = started;
            event.source = MonitorSource::Live;
            event.type = MonitorEventType::SysEx;
            event.severity = invalid ? MonitorSeverity::Error : MonitorSeverity::Normal;
            event.deviceId = deviceId_;
            event.status = static_cast<std::uint8_t>(
                static_cast<unsigned char>(header->lpData[0]));
            event.payloadLength = header->dwBytesRecorded;
            event.payloadPreviewLength = std::min<std::size_t>(
                event.payloadPreview.size(), header->dwBytesRecorded);
            for (std::size_t index = 0; index < event.payloadPreviewLength; ++index) {
                event.payloadPreview[index] = static_cast<std::uint8_t>(
                    static_cast<unsigned char>(header->lpData[index]));
            }
            event.latencyMicroseconds = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - started).count();
            if (invalid) {
                event.message = L"Invalid or incomplete SysEx message";
            }
            monitor_.Record(std::move(event));
        }
        if (header != nullptr && active_.load(std::memory_order_acquire) &&
            handle_ != nullptr) {
            header->dwBytesRecorded = 0;
            const MMRESULT result = midiInAddBuffer(handle_, header, sizeof(MIDIHDR));
            if (result != MMSYSERR_NOERROR) {
                monitor_.RecordError(L"Could not recycle a SysEx input buffer: " +
                                     WinMmError(result));
            }
        }
    }

    void ReleaseHandle() {
        if (handle_ == nullptr) {
            return;
        }
        midiInStop(handle_);
        midiInReset(handle_);
        for (LongBuffer& buffer : longBuffers_) {
            if (buffer.prepared) {
                midiInUnprepareHeader(handle_, &buffer.header, sizeof(buffer.header));
                buffer.prepared = false;
            }
        }
        midiInClose(handle_);
        handle_ = nullptr;
    }

    DebugMonitor& monitor_;
    HMIDIIN handle_ = nullptr;
    UINT deviceId_ = 0;
    std::array<LongBuffer, 4> longBuffers_{};
    std::atomic_bool active_{false};
};

class Application {
public:
    explicit Application(HINSTANCE instance)
        : instance_(instance), texturedUi_(instance), monitor_(instance), midiInput_(monitor_) {}
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    ~Application() { Shutdown(); }

    int Run() {
        SetLastError(ERROR_SUCCESS);
        HWND created = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_MAIN_DIALOG),
                                          nullptr, &Application::DialogProcedure,
                                          reinterpret_cast<LPARAM>(this));
        if (created == nullptr) {
            const std::wstring message =
                L"MidiToRblx could not create its main dialog.\n\n" + Win32Error(GetLastError());
            MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
            return 1;
        }
        ShowWindow(created, SW_SHOW);
        UpdateWindow(created);
        MSG message{};
        while (true) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                return result == 0 ? static_cast<int>(message.wParam) : 1;
            }
            if (monitor_.ProcessDialogMessage(message) ||
                IsDialogMessageW(window_, &message) != FALSE) {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

private:
    struct LoadResult {
        std::shared_ptr<midi::Song> song;
        std::filesystem::path path;
        std::wstring error;
    };

    static INT_PTR CALLBACK DialogProcedure(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
        Application* app = reinterpret_cast<Application*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_INITDIALOG) {
            app = reinterpret_cast<Application*>(lParam);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            return app->OnInitDialog();
        }
        return app != nullptr ? app->HandleMessage(message, wParam, lParam)
                              : FALSE;
    }

    INT_PTR HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_COMMAND:
                OnCommand(LOWORD(wParam), HIWORD(wParam));
                return TRUE;
            case WM_NOTIFY:
                return OnNotify(reinterpret_cast<const NMHDR*>(lParam)) ? TRUE : FALSE;
            case WM_ERASEBKGND:
                texturedUi_.DrawBackground(window_, reinterpret_cast<HDC>(wParam));
                return TRUE;
            case WM_DRAWITEM:
                return texturedUi_.DrawControl(
                           reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))
                           ? TRUE
                           : FALSE;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
                return reinterpret_cast<INT_PTR>(
                    TexturedUi::TransparentControlBrush(reinterpret_cast<HDC>(wParam)));
            case WM_TIMER:
                if (wParam == kProgressTimer) {
                    UpdateProgress();
                }
                return TRUE;
            case WM_HOTKEY:
                if (wParam == kDeleteHotkeyId) {
                    TogglePlayback();
                }
                return TRUE;
            case kLoadFinishedMessage:
                FinishLoading();
                return TRUE;
            case kPlaybackFinishedMessage:
                FinishPlaybackUi();
                return TRUE;
            case WM_CLOSE:
                Shutdown();
                DestroyWindow(window_);
                window_ = nullptr;
                PostQuitMessage(0);
                return TRUE;
            default:
                return FALSE;
        }
    }

    INT_PTR OnInitDialog() {
        inputDevices_ = GetDlgItem(window_, IDC_INPUT_DEVICES);
        refreshInputs_ = GetDlgItem(window_, IDC_REFRESH_INPUTS);
        connectInput_ = GetDlgItem(window_, IDC_CONNECT_INPUT);
        stopInput_ = GetDlgItem(window_, IDC_STOP_INPUT);
        selectFile_ = GetDlgItem(window_, IDC_SELECT_FILE);
        selectedFile_ = GetDlgItem(window_, IDC_SELECTED_FILE);
        playFile_ = GetDlgItem(window_, IDC_PLAY_FILE);
        pauseFile_ = GetDlgItem(window_, IDC_PAUSE_FILE);
        stopFile_ = GetDlgItem(window_, IDC_STOP_FILE);
        durationLabel_ = GetDlgItem(window_, IDC_FILE_DURATION);
        progress_ = GetDlgItem(window_, IDC_FILE_PROGRESS);
        statusLabel_ = GetDlgItem(window_, IDC_STATUS_LABEL);

        texturedUi_.Prepare(window_);
        if (!texturedUi_.IsReady()) {
            MessageBoxW(window_, L"The embedded interface textures could not be loaded.",
                        kWindowTitle, MB_OK | MB_ICONWARNING);
        }

        const HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
        EnableWindow(stopInput_, FALSE);
        EnableWindow(playFile_, FALSE);
        EnableWindow(pauseFile_, FALSE);
        EnableWindow(stopFile_, FALSE);

        RECT windowBounds{};
        if (GetWindowRect(window_, &windowBounds)) {
            const int width = windowBounds.right - windowBounds.left;
            const int height = windowBounds.bottom - windowBounds.top;
            const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
            const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
            SetWindowPos(window_, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        }

        RefreshInputs();
        SetTimer(window_, kProgressTimer, 100, nullptr);
        hotkeyRegistered_ = RegisterHotKey(window_, kDeleteHotkeyId, MOD_NOREPEAT, VK_DELETE) != FALSE;
        if (!hotkeyRegistered_) {
            SetStatus(L"Ready (the global Delete hotkey is unavailable)." );
            monitor_.RecordWarning(L"The global Delete hotkey is unavailable.");
        } else {
            SetStatus(L"Ready. Delete globally toggles file playback." );
        }
        monitor_.RecordInformation(L"MidiToRblx initialized.");
        return TRUE;
    }

    bool OnNotify(const NMHDR* notification) {
        if (notification == nullptr || notification->idFrom != IDC_SYSLINK1 ||
            (notification->code != NM_CLICK && notification->code != NM_RETURN)) {
            return false;
        }
        const auto* link = reinterpret_cast<const NMLINK*>(notification);
        const INT_PTR result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(window_, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            const std::wstring message =
                L"Windows could not open the MidiToRblx GitHub page. Shell error " +
                std::to_wstring(result) + L".";
            monitor_.RecordWarning(message);
            MessageBoxW(window_, message.c_str(), kWindowTitle, MB_OK | MB_ICONWARNING);
        } else {
            monitor_.RecordInformation(L"Opened https://github.com/Axtorz/MidiToRblx.");
        }
        return true;
    }

    void OnCommand(int id, int notification) {
        if (notification != BN_CLICKED && id != IDC_INPUT_DEVICES) {
            return;
        }
        switch (id) {
            case IDC_REFRESH_INPUTS:
                RefreshInputs();
                break;
            case IDC_CONNECT_INPUT:
                ConnectInput();
                break;
            case IDC_STOP_INPUT:
                StopInput();
                break;
            case IDC_SELECT_FILE:
                SelectFile();
                break;
            case IDC_PLAY_FILE:
                if (IsPlaybackPaused()) {
                    ResumePlayback();
                } else {
                    StartPlayback();
                }
                break;
            case IDC_PAUSE_FILE:
                PausePlayback();
                break;
            case IDC_STOP_FILE:
                StopPlayback();
                break;
            case MONITOR_BUTTON:
                monitor_.Open(window_);
                break;
            default:
                break;
        }
    }

    void RefreshInputs() {
        if (midiInput_.IsActive()) {
            return;
        }
        SendMessageW(inputDevices_, CB_RESETCONTENT, 0, 0);
        inputDeviceIds_.clear();
        const UINT count = midiInGetNumDevs();
        for (UINT device = 0; device < count; ++device) {
            MIDIINCAPSW capabilities{};
            if (midiInGetDevCapsW(device, &capabilities, sizeof(capabilities)) == MMSYSERR_NOERROR) {
                SendMessageW(inputDevices_, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(capabilities.szPname));
                inputDeviceIds_.push_back(device);
            }
        }
        if (inputDeviceIds_.empty()) {
            SendMessageW(inputDevices_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(L"(No MIDI input devices found)"));
            SendMessageW(inputDevices_, CB_SETCURSEL, 0, 0);
            EnableWindow(connectInput_, FALSE);
        } else {
            SendMessageW(inputDevices_, CB_SETCURSEL, 0, 0);
            EnableWindow(connectInput_, TRUE);
        }
    }

    void ConnectInput() {
        const LRESULT selection = SendMessageW(inputDevices_, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR || static_cast<std::size_t>(selection) >= inputDeviceIds_.size()) {
            return;
        }
        std::wstring error;
        const UINT deviceId = inputDeviceIds_[static_cast<std::size_t>(selection)];
        if (!midiInput_.Start(deviceId, error)) {
            monitor_.RecordError(L"Could not open MIDI input device " +
                                 std::to_wstring(deviceId) + L": " + error);
            MessageBoxW(window_, error.c_str(), L"Could not open MIDI input",
                        MB_OK | MB_ICONERROR);
            return;
        }
        EnableWindow(inputDevices_, FALSE);
        EnableWindow(refreshInputs_, FALSE);
        EnableWindow(connectInput_, FALSE);
        EnableWindow(stopInput_, TRUE);
        SetStatus(L"MIDI input connected. Events are sent to the foreground app.");
        monitor_.RecordInformation(L"Connected MIDI input device " +
                                   std::to_wstring(deviceId) + L".");
    }

    void StopInput() {
        const bool wasActive = midiInput_.IsActive();
        midiInput_.Stop();
        EnableWindow(inputDevices_, TRUE);
        EnableWindow(refreshInputs_, TRUE);
        EnableWindow(stopInput_, FALSE);
        EnableWindow(connectInput_, inputDeviceIds_.empty() ? FALSE : TRUE);
        SetStatus(L"MIDI input disconnected.");
        if (wasActive) {
            monitor_.RecordInformation(L"MIDI input disconnected.");
        }
    }

    void SelectFile() {
        std::array<wchar_t, 32'768> filename{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.hInstance = instance_;
        dialog.lpstrFilter = L"MIDI Files (*.mid;*.midi)\0*.mid;*.midi\0All Files (*.*)\0*.*\0\0";
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrTitle = L"Select a MIDI file";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                       OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) {
            return;
        }
        StartLoading(std::filesystem::path(filename.data()));
    }

    void StartLoading(const std::filesystem::path& path) {
        if (loading_) {
            return;
        }
        if (loadThread_.joinable()) {
            loadThread_.join();
        }
        loading_ = true;
        loadCancel_.store(false, std::memory_order_relaxed);
        EnableWindow(selectFile_, FALSE);
        EnableWindow(playFile_, FALSE);
        SetWindowTextW(selectedFile_, path.filename().c_str());
        SetStatus(L"Loading and indexing MIDI file...");
        monitor_.RecordInformation(L"Loading MIDI file: " + path.wstring());

        loadThread_ = std::thread([this, path] {
            auto result = std::make_unique<LoadResult>();
            result->path = path;
            try {
                result->song = std::make_shared<midi::Song>();
                if (!midi::LoadFile(path, *result->song, result->error, &loadCancel_)) {
                    result->song.reset();
                }
            } catch (const std::bad_alloc&) {
                result->error = L"There is not enough memory to load this MIDI file.";
                result->song.reset();
            } catch (...) {
                result->error = L"An unexpected error occurred while loading the MIDI file.";
                result->song.reset();
            }
            {
                std::lock_guard lock(loadResultMutex_);
                pendingLoadResult_ = std::move(result);
            }
            PostMessageW(window_, kLoadFinishedMessage, 0, 0);
        });
    }

    void FinishLoading() {
        if (loadThread_.joinable()) {
            loadThread_.join();
        }
        std::unique_ptr<LoadResult> result;
        {
            std::lock_guard lock(loadResultMutex_);
            result = std::move(pendingLoadResult_);
        }
        loading_ = false;
        EnableWindow(selectFile_, TRUE);
        if (!result) {
            return;
        }
        if (!result->song) {
            monitor_.RecordError(L"MIDI file parsing failed: " + result->error);
            EnableWindow(playFile_, loadedSong_ ? TRUE : FALSE);
            if (!shuttingDown_) {
                MessageBoxW(window_, result->error.c_str(), L"Could not load MIDI file",
                            MB_OK | MB_ICONERROR);
                SetStatus(L"MIDI file loading failed.");
                SetWindowTextW(selectedFile_, loadedPath_.empty()
                                                   ? L""
                                                   : loadedPath_.filename().c_str());
            }
            return;
        }

        loadedSong_ = std::move(result->song);
        loadedPath_ = std::move(result->path);
        SetWindowTextW(selectedFile_, loadedPath_.filename().c_str());
        SetDuration(0, loadedSong_->durationMicroseconds);
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        EnableWindow(playFile_, TRUE);
        EnableWindow(pauseFile_, FALSE);
        EnableWindow(stopFile_, FALSE);
        std::wostringstream status;
        status << L"Loaded " << loadedSong_->events.size() << L" MIDI events.";
        SetStatus(status.str());
        monitor_.RecordInformation(L"Loaded " + std::to_wstring(loadedSong_->events.size()) +
                                   L" MIDI events from " + loadedPath_.filename().wstring() + L".");
    }

    bool WaitForPlaybackTime(std::uint64_t microseconds) {
        std::unique_lock lock(playbackMutex_);
        while (true) {
            if (playbackStopRequested_) {
                return false;
            }
            if (playbackPaused_) {
                playbackCv_.wait(lock, [this] {
                    return playbackStopRequested_ || !playbackPaused_;
                });
                continue;
            }
            const auto target = playbackStart_ + playbackAccumulatedPause_ +
                                std::chrono::microseconds(microseconds);
            if (playbackCv_.wait_until(lock, target, [this] {
                    return playbackStopRequested_ || playbackPaused_;
                })) {
                continue;
            }
            return true;
        }
    }

    void ProcessPlaybackEvent(const midi::Event& midiEvent) {
        const auto started = std::chrono::steady_clock::now();
        EmitResult emitted =
            KeyEmitter::SendEvent(midiEvent.kind, midiEvent.data1, midiEvent.data2);
        const auto completed = std::chrono::steady_clock::now();
        MonitorEvent event;
        event.timestamp = started;
        event.source = MonitorSource::File;
        event.status = midiEvent.status;
        event.data1 = midiEvent.data1;
        event.data2 = midiEvent.data2;
        event.type = ClassifyMonitorEvent(event.status, event.data1, event.data2);
        if (midiEvent.kind == midi::EventKind::SysEx) {
            event.type = MonitorEventType::SysEx;
            event.payloadLength = midiEvent.value;
        } else if (midiEvent.kind == midi::EventKind::Unknown) {
            event.type = MonitorEventType::Unknown;
        }
        const std::uint8_t category = event.status & 0xF0U;
        if (event.status >= 0x80U && event.status <= 0xEFU) {
            event.hasData1 = true;
            event.hasData2 = category != 0xC0U && category != 0xD0U;
        }
        event.output = std::move(emitted.output);
        event.latencyMicroseconds =
            std::chrono::duration<double, std::micro>(completed - started).count();
        monitor_.Record(std::move(event));
        if (emitted.error != ERROR_SUCCESS) {
            monitor_.RecordError(L"SendInput failed during MIDI file playback after " +
                                 std::to_wstring(emitted.submittedInputs) +
                                 L" of 10 input actions. Windows error " +
                                 std::to_wstring(emitted.error) + L".");
        }
    }

    void StartPlayback() {
        if (!loadedSong_ || playbackUiActive_ || loading_) {
            return;
        }
        if (playbackThread_.joinable()) {
            playbackThread_.join();
        }
        {
            std::lock_guard lock(playbackMutex_);
            playbackRunning_ = true;
            playbackPaused_ = false;
            playbackStopRequested_ = false;
            playbackNaturalCompletion_ = false;
            playbackStart_ = std::chrono::steady_clock::now();
            playbackPauseStart_ = {};
            playbackAccumulatedPause_ = std::chrono::steady_clock::duration::zero();
        }
        playbackUiActive_ = true;
        BeginPlaybackTimerResolution();
        EnableWindow(selectFile_, FALSE);
        EnableWindow(playFile_, FALSE);
        EnableWindow(pauseFile_, TRUE);
        EnableWindow(stopFile_, TRUE);
        SetStatus(L"Playing. Focus Roblox; Delete pauses/resumes globally.");
        monitor_.RecordInformation(L"MIDI file playback started.");

        const std::shared_ptr<const midi::Song> song = loadedSong_;
        playbackThread_ = std::thread([this, song] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            bool completed = true;
            for (const midi::Event& event : song->events) {
                if (!WaitForPlaybackTime(event.playbackMicroseconds)) {
                    completed = false;
                    break;
                }
                ProcessPlaybackEvent(event);
            }
            if (completed && !WaitForPlaybackTime(song->playbackDurationMicroseconds)) {
                completed = false;
            }
            {
                std::lock_guard lock(playbackMutex_);
                playbackNaturalCompletion_ = completed && !playbackStopRequested_;
                playbackRunning_ = false;
                playbackPaused_ = false;
            }
            PostMessageW(window_, kPlaybackFinishedMessage, 0, 0);
        });
    }

    void PausePlayback() {
        std::lock_guard lock(playbackMutex_);
        if (!playbackRunning_ || playbackPaused_) {
            return;
        }
        playbackPaused_ = true;
        playbackPauseStart_ = std::chrono::steady_clock::now();
        playbackCv_.notify_all();
        EnableWindow(playFile_, TRUE);
        EnableWindow(pauseFile_, FALSE);
        SetStatus(L"Playback paused. Press Play or Delete to resume.");
        monitor_.RecordInformation(L"MIDI file playback paused.");
    }

    void ResumePlayback() {
        std::lock_guard lock(playbackMutex_);
        if (!playbackRunning_ || !playbackPaused_) {
            return;
        }
        playbackAccumulatedPause_ += std::chrono::steady_clock::now() - playbackPauseStart_;
        playbackPaused_ = false;
        playbackCv_.notify_all();
        EnableWindow(playFile_, FALSE);
        EnableWindow(pauseFile_, TRUE);
        SetStatus(L"Playing. Focus Roblox; Delete pauses/resumes globally.");
        monitor_.RecordInformation(L"MIDI file playback resumed.");
    }

    void StopPlayback() {
        {
            std::lock_guard lock(playbackMutex_);
            if (!playbackRunning_) {
                return;
            }
            playbackStopRequested_ = true;
            playbackPaused_ = false;
        }
        playbackCv_.notify_all();
        EnableWindow(playFile_, FALSE);
        EnableWindow(pauseFile_, FALSE);
        EnableWindow(stopFile_, FALSE);
        SetStatus(L"Stopping playback...");
        monitor_.RecordInformation(L"Stopping MIDI file playback.");
    }

    bool IsPlaybackPaused() const {
        std::lock_guard lock(playbackMutex_);
        return playbackPaused_;
    }

    void TogglePlayback() {
        if (loading_ || !loadedSong_) {
            return;
        }
        if (!playbackUiActive_) {
            StartPlayback();
        } else if (IsPlaybackPaused()) {
            ResumePlayback();
        } else {
            PausePlayback();
        }
    }

    void FinishPlaybackUi() {
        if (playbackThread_.joinable()) {
            playbackThread_.join();
        }
        bool natural = false;
        {
            std::lock_guard lock(playbackMutex_);
            natural = playbackNaturalCompletion_;
            playbackStopRequested_ = false;
        }
        playbackUiActive_ = false;
        EndPlaybackTimerResolution();
        EnableWindow(selectFile_, TRUE);
        EnableWindow(playFile_, loadedSong_ ? TRUE : FALSE);
        EnableWindow(pauseFile_, FALSE);
        EnableWindow(stopFile_, FALSE);
        SendMessageW(progress_, PBM_SETPOS, 0, 0);
        SetDuration(0, loadedSong_ ? loadedSong_->durationMicroseconds : 0);
        SetStatus(natural ? L"Playback finished." : L"Playback stopped.");
        monitor_.RecordInformation(natural ? L"MIDI file playback finished."
                                           : L"MIDI file playback stopped.");
    }

    std::uint64_t PlaybackPosition() const {
        std::lock_guard lock(playbackMutex_);
        if (!playbackRunning_) {
            return loadedSong_ ? loadedSong_->playbackDurationMicroseconds : 0;
        }
        const auto reference = playbackPaused_ ? playbackPauseStart_
                                               : std::chrono::steady_clock::now();
        const auto elapsed = reference - playbackStart_ - playbackAccumulatedPause_;
        if (elapsed <= std::chrono::steady_clock::duration::zero()) {
            return 0;
        }
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }

    void UpdateProgress() {
        if (!playbackUiActive_ || !loadedSong_) {
            return;
        }
        const std::uint64_t position = PlaybackPosition();
        SetDuration(position, loadedSong_->durationMicroseconds);
        int progress = 0;
        if (loadedSong_->durationMicroseconds != 0) {
            const long double ratio = static_cast<long double>(position) /
                                      static_cast<long double>(loadedSong_->durationMicroseconds);
            progress = static_cast<int>(std::clamp<long double>(ratio * 1000.0L, 0, 1000));
        }
        SendMessageW(progress_, PBM_SETPOS, progress, 0);
    }

    static std::wstring FormatTime(std::uint64_t microseconds) {
        const std::uint64_t totalSeconds = microseconds / 1'000'000U;
        const std::uint64_t minutes = totalSeconds / 60U;
        const std::uint64_t seconds = totalSeconds % 60U;
        std::wostringstream text;
        text << std::setfill(L'0') << std::setw(2) << minutes << L':'
             << std::setw(2) << seconds;
        return text.str();
    }

    void SetDuration(std::uint64_t position, std::uint64_t duration) const {
        const std::wstring text = FormatTime(position) + L"/" + FormatTime(duration);
        SetWindowTextW(durationLabel_, text.c_str());
    }

    void SetStatus(const std::wstring& text) const {
        SetWindowTextW(statusLabel_, text.c_str());
    }

    void BeginPlaybackTimerResolution() {
        if (timerResolutionActive_) {
            return;
        }
        TIMECAPS capabilities{};
        if (timeGetDevCaps(&capabilities, sizeof(capabilities)) != TIMERR_NOERROR) {
            return;
        }
        playbackTimerPeriod_ = std::min<UINT>(
            capabilities.wPeriodMax, std::max<UINT>(1, capabilities.wPeriodMin));
        timerResolutionActive_ =
            timeBeginPeriod(playbackTimerPeriod_) == TIMERR_NOERROR;
    }

    void EndPlaybackTimerResolution() {
        if (timerResolutionActive_) {
            timeEndPeriod(playbackTimerPeriod_);
            timerResolutionActive_ = false;
        }
    }

    void Shutdown() {
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        if (window_ != nullptr) {
            KillTimer(window_, kProgressTimer);
        }
        if (hotkeyRegistered_) {
            UnregisterHotKey(window_, kDeleteHotkeyId);
            hotkeyRegistered_ = false;
        }
        midiInput_.Stop();
        loadCancel_.store(true, std::memory_order_relaxed);
        if (loadThread_.joinable()) {
            loadThread_.join();
        }
        {
            std::lock_guard lock(playbackMutex_);
            playbackStopRequested_ = true;
            playbackPaused_ = false;
        }
        playbackCv_.notify_all();
        if (playbackThread_.joinable()) {
            playbackThread_.join();
        }
        EndPlaybackTimerResolution();
        monitor_.Close();
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    bool hotkeyRegistered_ = false;
    bool shuttingDown_ = false;
    bool timerResolutionActive_ = false;
    UINT playbackTimerPeriod_ = 1;

    HWND inputDevices_ = nullptr;
    HWND refreshInputs_ = nullptr;
    HWND connectInput_ = nullptr;
    HWND stopInput_ = nullptr;
    HWND selectFile_ = nullptr;
    HWND selectedFile_ = nullptr;
    HWND playFile_ = nullptr;
    HWND pauseFile_ = nullptr;
    HWND stopFile_ = nullptr;
    HWND durationLabel_ = nullptr;
    HWND progress_ = nullptr;
    HWND statusLabel_ = nullptr;

    TexturedUi texturedUi_;
    DebugMonitor monitor_;
    MidiInput midiInput_;
    std::vector<UINT> inputDeviceIds_;

    bool loading_ = false;
    std::atomic_bool loadCancel_{false};
    std::thread loadThread_;
    std::mutex loadResultMutex_;
    std::unique_ptr<LoadResult> pendingLoadResult_;
    std::shared_ptr<midi::Song> loadedSong_;
    std::filesystem::path loadedPath_;

    mutable std::mutex playbackMutex_;
    std::condition_variable playbackCv_;
    std::thread playbackThread_;
    bool playbackUiActive_ = false;
    bool playbackRunning_ = false;
    bool playbackPaused_ = false;
    bool playbackStopRequested_ = false;
    bool playbackNaturalCompletion_ = false;
    std::chrono::steady_clock::time_point playbackStart_{};
    std::chrono::steady_clock::time_point playbackPauseStart_{};
    std::chrono::steady_clock::duration playbackAccumulatedPause_{};
};

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC =
        ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LINK_CLASS;
    if (InitCommonControlsEx(&commonControls) == FALSE) {
        const std::wstring message =
            L"MidiToRblx could not initialize Windows common controls.\n\n" +
            Win32Error(GetLastError());
        MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    const HMODULE richEdit20 = LoadLibraryW(L"Riched20.dll");
    if (richEdit20 == nullptr) {
        const std::wstring message =
            L"MidiToRblx could not load the Windows RichEdit 2.0 component.\n\n" +
            Win32Error(GetLastError());
        MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    const HMODULE richEdit50 = LoadLibraryW(L"Msftedit.dll");
    if (richEdit50 == nullptr) {
        const std::wstring message =
            L"MidiToRblx could not load the Windows RichEdit 5.0 component.\n\n" +
            Win32Error(GetLastError());
        MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
        FreeLibrary(richEdit20);
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"MidiToRblx could not initialize Windows graphics.",
                    kWindowTitle, MB_OK | MB_ICONERROR);
        FreeLibrary(richEdit50);
        FreeLibrary(richEdit20);
        return 1;
    }

    int result = 1;
    {
        Application application(instance);
        result = application.Run();
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    FreeLibrary(richEdit50);
    FreeLibrary(richEdit20);
    return result;
}
