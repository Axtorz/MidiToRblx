#include "debug_monitor.hpp"

#include "MidiToRblx.h"
#include "Resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr UINT_PTR kRenderTimer = 1;
constexpr std::size_t kMaximumStoredEvents = 50'000;
constexpr std::size_t kMaximumBatchEvents = 2'000;
constexpr std::uint64_t kTrimBatchEvents = 1'000;
constexpr wchar_t kHeader[] =
    L"TIME            IN  PORT STATUS  DATA1 DATA2 CH NOTE           EVENT                       OUTPUT                          DELAY";
constexpr wchar_t kSeparator[] =
    L""; //ptetre si ca rend bien

enum FilterBits : std::uint32_t {
    FilterNoteOn = 1U << 0U,
    FilterNoteOff = 1U << 1U,
    FilterControlChange = 1U << 2U,
    FilterProgramChange = 1U << 3U,
    FilterPitchBend = 1U << 4U,
    FilterAftertouch = 1U << 5U,
    FilterPolyAftertouch = 1U << 6U,
    FilterSustain = 1U << 7U,
    FilterSysEx = 1U << 8U,
    FilterOutput = 1U << 10U,
    FilterWarnings = 1U << 11U,
    FilterErrors = 1U << 12U,
};

constexpr std::uint32_t kAllFilters =
    FilterNoteOn | FilterNoteOff | FilterControlChange | FilterProgramChange |
    FilterPitchBend | FilterAftertouch | FilterPolyAftertouch | FilterSustain |
    FilterSysEx | FilterOutput | FilterWarnings | FilterErrors;

struct StoredEvent {
    std::uint64_t sequence = 0;
    MonitorEvent event;
};

struct RenderLine {
    int color = 1;
    std::wstring text;
};

std::wstring HexByte(std::uint8_t value) {
    std::wostringstream text;
    text << std::uppercase << std::hex << std::setfill(L'0') << std::setw(2)
         << static_cast<unsigned int>(value);
    return text.str();
}

std::wstring NoteName(std::uint8_t note) {
    static constexpr std::array<const wchar_t*, 12> names{
        L"C", L"C#", L"D", L"D#", L"E", L"F",
        L"F#", L"G", L"G#", L"A", L"A#", L"B"};
    return std::wstring(names[note % 12U]) +
           std::to_wstring(static_cast<int>(note / 12U) - 1);
}

std::wstring EventDescription(const MonitorEvent& event) {
    if (!event.message.empty()) {
        return event.message;
    }
    switch (event.type) {
        case MonitorEventType::NoteOn:
            return L"Note On velocity=" + std::to_wstring(event.data2);
        case MonitorEventType::NoteOff:
            return L"Note Off velocity=" + std::to_wstring(event.data2);
        case MonitorEventType::ControlChange:
            return L"Control Change controller=" + std::to_wstring(event.data1) +
                   L" value=" + std::to_wstring(event.data2);
        case MonitorEventType::ProgramChange:
            return L"Program Change program=" + std::to_wstring(event.data1);
        case MonitorEventType::PitchBend: {
            const int value = static_cast<int>(event.data1) |
                              (static_cast<int>(event.data2) << 7);
            return L"Pitch Bend value=" + std::to_wstring(value) +
                   L" signed=" + std::to_wstring(value - 8192);
        }
        case MonitorEventType::Aftertouch:
            return L"Aftertouch pressure=" + std::to_wstring(event.data1);
        case MonitorEventType::PolyAftertouch:
            return L"Poly Aftertouch pressure=" + std::to_wstring(event.data2);
        case MonitorEventType::Sustain:
            return std::wstring(L"Sustain ") + (event.data2 >= 64U ? L"ON" : L"OFF") +
                   L" value=" + std::to_wstring(event.data2);
        case MonitorEventType::SysEx: {
            std::wstring text = L"SysEx length=" + std::to_wstring(event.payloadLength);
            if (event.payloadPreviewLength != 0) {
                text += L" data=";
                for (std::size_t index = 0; index < event.payloadPreviewLength; ++index) {
                    if (index != 0) {
                        text += L' ';
                    }
                    text += HexByte(event.payloadPreview[index]);
                }
                if (event.payloadPreviewLength < event.payloadLength) {
                    text += L" ...";
                }
            }
            return text;
        }
        case MonitorEventType::Unknown:
            return L"Unknown Event";
        case MonitorEventType::Information:
            return L"Information";
    }
    return L"Unknown Event";
}

std::wstring NarrowToWide(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

int ColorFor(const MonitorEvent& event) {
    if (event.severity == MonitorSeverity::Error) {
        return 7;
    }
    if (event.severity == MonitorSeverity::Warning) {
        return 6;
    }
    switch (event.type) {
        case MonitorEventType::NoteOn:
            return 2;
        case MonitorEventType::NoteOff:
            return 3;
        case MonitorEventType::ControlChange:
        case MonitorEventType::Sustain:
            return 4;
        case MonitorEventType::SysEx:
            return 5;
        case MonitorEventType::Unknown:
            return 1;
        case MonitorEventType::ProgramChange:
        case MonitorEventType::PitchBend:
        case MonitorEventType::Aftertouch:
        case MonitorEventType::PolyAftertouch:
            return 9;
        case MonitorEventType::Information:
            return 1;
    }
    return 1;
}

std::uint32_t FilterFor(MonitorEventType type) {
    switch (type) {
        case MonitorEventType::NoteOn:
            return FilterNoteOn;
        case MonitorEventType::NoteOff:
            return FilterNoteOff;
        case MonitorEventType::ControlChange:
            return FilterControlChange;
        case MonitorEventType::ProgramChange:
            return FilterProgramChange;
        case MonitorEventType::PitchBend:
            return FilterPitchBend;
        case MonitorEventType::Aftertouch:
            return FilterAftertouch;
        case MonitorEventType::PolyAftertouch:
            return FilterPolyAftertouch;
        case MonitorEventType::Sustain:
            return FilterSustain;
        case MonitorEventType::SysEx:
            return FilterSysEx;
        case MonitorEventType::Unknown:
            return 0;
        case MonitorEventType::Information:
            return 0;
    }
    return 0;
}

bool PassesFilters(const MonitorEvent& event, std::uint32_t filters) {
    if (event.severity == MonitorSeverity::Warning &&
        (filters & FilterWarnings) == 0) {
        return false;
    }
    if (event.severity == MonitorSeverity::Error &&
        (filters & FilterErrors) == 0) {
        return false;
    }
    const std::uint32_t required = FilterFor(event.type);
    if (required != 0 && (filters & required) == 0) {
        return false;
    }
    return event.output.empty() || (filters & FilterOutput) != 0;
}

std::wstring FormatTimestamp(
    std::chrono::system_clock::time_point wallOrigin,
    std::chrono::steady_clock::time_point steadyOrigin,
    std::chrono::steady_clock::time_point timestamp) {
    const auto wallTime = wallOrigin + (timestamp - steadyOrigin);
    const auto totalMicroseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            wallTime.time_since_epoch());
    const std::time_t seconds = static_cast<std::time_t>(totalMicroseconds.count() / 1'000'000);
    const long long fraction = totalMicroseconds.count() % 1'000'000;
    std::tm local{};
    localtime_s(&local, &seconds);
    std::wostringstream text;
    text << std::setfill(L'0') << std::setw(2) << local.tm_hour << L':'
         << std::setw(2) << local.tm_min << L':' << std::setw(2) << local.tm_sec
         << L'.' << std::setw(6) << fraction;
    return text.str();
}

std::wstring FormatLine(const MonitorEvent& event,
                        std::chrono::system_clock::time_point wallOrigin,
                        std::chrono::steady_clock::time_point steadyOrigin) {
    const std::wstring time = FormatTimestamp(wallOrigin, steadyOrigin, event.timestamp);
    const wchar_t* source = event.source == MonitorSource::Live
                                ? L"IN"
                                : (event.source == MonitorSource::File ? L"FILE" : L"APP");
    const std::wstring port = event.source == MonitorSource::Live
                                  ? std::to_wstring(event.deviceId)
                                  : L"-";
    const std::wstring status = event.source == MonitorSource::Application
                                    ? L"--"
                                    : HexByte(event.status);
    const std::wstring data1 = event.hasData1 ? HexByte(event.data1) : L"--";
    const std::wstring data2 = event.hasData2 ? HexByte(event.data2) : L"--";
    const bool channelMessage = event.status >= 0x80U && event.status <= 0xEFU;
    const std::wstring channel = channelMessage
                                     ? std::to_wstring((event.status & 0x0FU) + 1U)
                                     : L"--";
    const bool noteEvent = event.type == MonitorEventType::NoteOn ||
                           event.type == MonitorEventType::NoteOff ||
                           event.type == MonitorEventType::PolyAftertouch;
    const std::wstring note = noteEvent ? NoteName(event.data1) : L"-";
    const std::wstring description = EventDescription(event);
    const std::wstring output = NarrowToWide(event.output);
    std::wostringstream delay;
    if (event.counted) {
        if (event.latencyMicroseconds >= 1000.0) {
            delay << std::fixed << std::setprecision(3)
                  << event.latencyMicroseconds / 1000.0 << L" ms";
        } else {
            delay << std::fixed << std::setprecision(1)
                  << event.latencyMicroseconds << L" us";
        }
    } else {
        delay << L"-";
    }
    std::wostringstream line;
    line << std::left << std::setw(16) << time
         << std::setw(5) << source
         << std::setw(6) << port
         << std::setw(7) << status
         << std::setw(6) << data1
         << std::setw(6) << data2
         << std::setw(3) << channel
         << std::setw(6) << note
         << std::setw(36) << description
         << std::setw(32) << output
         << delay.str();
    return line.str();
}

void AppendRtfText(std::string& output, const std::wstring& text) {
    for (const wchar_t character : text) {
        if (character == L'\\' || character == L'{' || character == L'}') {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        } else if (character >= 0x20 && character <= 0x7E) {
            output.push_back(static_cast<char>(character));
        } else {
            const auto signedValue = static_cast<std::int16_t>(character);
            output += "\\u" + std::to_string(signedValue) + "?";
        }
    }
}

std::string BuildRtf(const std::vector<RenderLine>& lines,
                     const std::wstring& fontName) {
    std::string output;
    output.reserve(lines.size() * 180U + 512U);
    output += "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0\\fmodern ";
    AppendRtfText(output, fontName);
    output += ";}}{\\colortbl;"
              "\\red245\\green245\\blue245;"
              "\\red69\\green212\\blue131;"
              "\\red255\\green95\\blue109;"
              "\\red255\\green215\\blue95;"
              "\\red255\\green95\\blue215;"
              "\\red255\\green159\\blue67;"
              "\\red255\\green51\\blue51;"
              "\\red155\\green155\\blue155;"
              "\\red93\\green207\\blue255;}"
              "\\viewkind4\\uc1\\pard\\f0\\fs18 ";
    for (const RenderLine& line : lines) {
        output += "\\cf" + std::to_string(line.color) + " ";
        AppendRtfText(output, line.text);
        output += "\\line ";
    }
    output += '}';
    return output;
}

struct StreamState {
    const char* data = nullptr;
    std::size_t size = 0;
    std::size_t position = 0;
};

DWORD CALLBACK StreamCallback(DWORD_PTR cookie, LPBYTE buffer, LONG requested,
                              LONG* written) {
    auto* state = reinterpret_cast<StreamState*>(cookie);
    const std::size_t remaining = state->size - state->position;
    const std::size_t amount = std::min<std::size_t>(remaining,
                                                     static_cast<std::size_t>(requested));
    if (amount != 0) {
        std::memcpy(buffer, state->data + state->position, amount);
        state->position += amount;
    }
    *written = static_cast<LONG>(amount);
    return 0;
}

int CALLBACK FontEnumerationCallback(const LOGFONTW*, const TEXTMETRICW*,
                                     DWORD, LPARAM parameter) {
    *reinterpret_cast<bool*>(parameter) = true;
    return 0;
}

bool FontExists(const wchar_t* name) {
    HDC device = GetDC(nullptr);
    if (device == nullptr) {
        return false;
    }
    LOGFONTW font{};
    font.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(font.lfFaceName, name);
    bool found = false;
    EnumFontFamiliesExW(device, &font, FontEnumerationCallback,
                        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, device);
    return found;
}

std::wstring SelectFontName() {
    if (FontExists(L"Consolas")) {
        return L"Consolas";
    }
    if (FontExists(L"Cascadia Mono")) {
        return L"Cascadia Mono";
    }
    return L"Lucida Console";
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        output.data(), size, nullptr, nullptr);
    return output;
}

} 

MonitorEventType ClassifyMonitorEvent(std::uint8_t status,
                                      std::uint8_t data1,
                                      std::uint8_t data2) {
    if (status >= 0xF0U) {
        return status == 0xF0U || status == 0xF7U
                   ? MonitorEventType::SysEx
                   : MonitorEventType::Unknown;
    }
    switch (status & 0xF0U) {
        case 0x80U:
            return MonitorEventType::NoteOff;
        case 0x90U:
            return data2 == 0 ? MonitorEventType::NoteOff : MonitorEventType::NoteOn;
        case 0xA0U:
            return MonitorEventType::PolyAftertouch;
        case 0xB0U:
            return data1 == 64U ? MonitorEventType::Sustain
                                : MonitorEventType::ControlChange;
        case 0xC0U:
            return MonitorEventType::ProgramChange;
        case 0xD0U:
            return MonitorEventType::Aftertouch;
        case 0xE0U:
            return MonitorEventType::PitchBend;
        default:
            return MonitorEventType::Unknown;
    }
}

struct DebugMonitor::Impl {
    explicit Impl(HINSTANCE applicationInstance)
        : instance(applicationInstance),
          steadyOrigin(std::chrono::steady_clock::now()),
          wallOrigin(std::chrono::system_clock::now()),
          fontName(SelectFontName()),
          backgroundBrush(CreateSolidBrush(RGB(13, 13, 13))) {}

    ~Impl() {
        Close();
        if (logFont != nullptr) {
            DeleteObject(logFont);
        }
        if (backgroundBrush != nullptr) {
            DeleteObject(backgroundBrush);
        }
        if (richEditLibrary != nullptr) {
            FreeLibrary(richEditLibrary);
        }
    }

    static INT_PTR CALLBACK DialogProcedure(HWND dialog, UINT message,
                                             WPARAM wParam, LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<Impl*>(lParam);
            self->window = dialog;
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return self->Initialize();
        }
        return self != nullptr ? self->HandleMessage(message, wParam, lParam) : FALSE;
    }

    INT_PTR Initialize() {
        header = GetDlgItem(window, IDC_MONITOR_HEADER);
        log = GetDlgItem(window, IDC_MONITOR_LOG);
        status = GetDlgItem(window, IDC_MONITOR_STATUS);
        const HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(header, EM_SETBKGNDCOLOR, 0, RGB(13, 13, 13));
        SendMessageW(header, EM_SETTARGETDEVICE, 0, 1);
        SendMessageW(log, EM_SETBKGNDCOLOR, 0, RGB(13, 13, 13));
        SendMessageW(log, EM_EXLIMITTEXT, 0, std::numeric_limits<LONG>::max());
        SendMessageW(log, EM_SETTARGETDEVICE, 0, 1);
        const int dpi = GetDpiForWindow(window);
        if (logFont != nullptr) {
            DeleteObject(logFont);
            logFont = nullptr;
        }
        logFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                              FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_MODERN, fontName.c_str());
        if (logFont != nullptr) {
            SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(logFont), TRUE);
            SendMessageW(log, WM_SETFONT, reinterpret_cast<WPARAM>(logFont), TRUE);
        }
        const std::vector<RenderLine> headerLines{{1, kHeader}, {8, kSeparator}};
        const std::string headerRtf = BuildRtf(headerLines, fontName);
        StreamState headerState{headerRtf.data(), headerRtf.size(), 0};
        EDITSTREAM headerStream{};
        headerStream.dwCookie = reinterpret_cast<DWORD_PTR>(&headerState);
        headerStream.pfnCallback = &StreamCallback;
        SendMessageW(header, EM_SETREADONLY, FALSE, 0);
        SendMessageW(header, EM_STREAMIN, SF_RTF,
                     reinterpret_cast<LPARAM>(&headerStream));
        SendMessageW(header, EM_SETREADONLY, TRUE, 0);
        SendMessageW(status, SB_SETBKCOLOR, 0, RGB(28, 28, 28));
        const std::array<std::pair<int, std::uint32_t>, 12> controls{{
            {IDC_FILTER_NOTE_ON, FilterNoteOn},
            {IDC_FILTER_NOTE_OFF, FilterNoteOff},
            {IDC_FILTER_CONTROL_CHANGE, FilterControlChange},
            {IDC_FILTER_PROGRAM_CHANGE, FilterProgramChange},
            {IDC_FILTER_PITCH_BEND, FilterPitchBend},
            {IDC_FILTER_AFTERTOUCH, FilterAftertouch},
            {IDC_FILTER_POLY_AFTERTOUCH, FilterPolyAftertouch},
            {IDC_FILTER_SUSTAIN, FilterSustain},
            {IDC_FILTER_SYSEX, FilterSysEx},
            {IDC_FILTER_OUTPUT, FilterOutput},
            {IDC_FILTER_WARNINGS, FilterWarnings},
            {IDC_FILTER_ERRORS, FilterErrors},
        }};
        for (const auto& [id, bit] : controls) {
            SetWindowTheme(GetDlgItem(window, id), L"", L"");
            CheckDlgButton(window, id, (filters & bit) != 0 ? BST_CHECKED : BST_UNCHECKED);
        }
        RECT headerBounds{};
        GetWindowRect(header, &headerBounds);
        POINT headerTopLeft{headerBounds.left, headerBounds.top};
        ScreenToClient(window, &headerTopLeft);
        headerTop = headerTopLeft.y;
        headerHeight = static_cast<int>(headerBounds.bottom - headerBounds.top);
        RECT logBounds{};
        GetWindowRect(log, &logBounds);
        POINT topLeft{logBounds.left, logBounds.top};
        ScreenToClient(window, &topLeft);
        logTop = topLeft.y;
        SetTimer(window, kRenderTimer, 50, nullptr);
        CenterWindow();
        Layout();
        rebuildPending = true;
        dirty = true;
        UpdateStatus(true);
        return TRUE;
    }

    INT_PTR HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_COMMAND:
                OnCommand(LOWORD(wParam), HIWORD(wParam));
                return TRUE;
            case WM_TIMER:
                if (wParam == kRenderTimer) {
                    if (rebuildPending || trimPending.exchange(false)) {
                        rebuildPending = false;
                        Rebuild();
                    } else {
                        RenderPending();
                    }
                    UpdateStatus(false);
                }
                return TRUE;
            case WM_SIZE:
                Layout();
                return TRUE;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
                limits->ptMinTrackSize.x = 1000;
                limits->ptMinTrackSize.y = 450;
                return TRUE;
            }
            case WM_DRAWITEM:
                if (wParam == IDC_MONITOR_STATUS) {
                    DrawStatus(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
                    return TRUE;
                }
                return FALSE;
            case WM_CTLCOLORDLG:
                return reinterpret_cast<INT_PTR>(backgroundBrush);
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                HDC device = reinterpret_cast<HDC>(wParam);
                SetTextColor(device, RGB(225, 225, 225));
                SetBkColor(device, RGB(13, 13, 13));
                return reinterpret_cast<INT_PTR>(backgroundBrush);
            }
            case WM_CLOSE:
                DestroyWindow(window);
                return TRUE;
            case WM_NCDESTROY:
                KillTimer(window, kRenderTimer);
                window = nullptr;
                header = nullptr;
                log = nullptr;
                status = nullptr;
                return TRUE;
            default:
                return FALSE;
        }
    }

    void Open(HWND ownerWindow) {
        if (window != nullptr && IsWindow(window)) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return;
        }
        if (richEditLibrary == nullptr) {
            richEditLibrary = LoadLibraryW(L"Msftedit.dll");
        }
        if (richEditLibrary == nullptr) {
            MessageBoxW(ownerWindow, L"The Windows RichEdit component could not be loaded.",
                        L"Debug Monitor", MB_OK | MB_ICONERROR);
            return;
        }
        owner = ownerWindow;
        HWND created = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_DEBUG_MONITOR),
                                          owner, &Impl::DialogProcedure,
                                          reinterpret_cast<LPARAM>(this));
        if (created == nullptr) {
            MessageBoxW(owner, L"The Debug Monitor window could not be created.",
                        L"Debug Monitor", MB_OK | MB_ICONERROR);
            return;
        }
        ShowWindow(created, SW_SHOW);
        UpdateWindow(created);
    }

    void Close() {
        if (window != nullptr && IsWindow(window)) {
            DestroyWindow(window);
        }
    }

    bool ProcessDialogMessage(MSG& message) const {
        return window != nullptr && IsWindow(window) &&
               IsDialogMessageW(window, &message) != FALSE;
    }

    void Record(MonitorEvent event) {
        std::lock_guard lock(mutex);
        if (event.counted) {
            ++totalEvents;
            latencyTotal += event.latencyMicroseconds;
            peakLatency = std::max(peakLatency, event.latencyMicroseconds);
        }
        if (events.size() >= kMaximumStoredEvents) {
            if (events.front().event.counted) {
                ++droppedEvents;
            }
            events.pop_front();
            ++evictedStoredEvents;
            if (evictedStoredEvents % kTrimBatchEvents == 0) {
                trimPending = true;
            }
        }
        events.push_back({nextSequence++, std::move(event)});
        dirty = true;
    }

    void RecordDiagnostic(std::wstring message, MonitorSeverity severity) {
        MonitorEvent event;
        event.source = MonitorSource::Application;
        event.type = MonitorEventType::Information;
        event.severity = severity;
        event.counted = false;
        event.message = std::move(message);
        Record(std::move(event));
    }

    void OnCommand(int id, int notification) {
        if (notification != BN_CLICKED) {
            return;
        }
        if (id == IDC_MONITOR_CLEAR) {
            {
                std::lock_guard lock(mutex);
                visibleStartSequence = nextSequence;
            }
            Rebuild();
            return;
        }
        if (id == IDC_MONITOR_SAVE) {
            Save();
            return;
        }
        const std::array<std::pair<int, std::uint32_t>, 12> controls{{
            {IDC_FILTER_NOTE_ON, FilterNoteOn},
            {IDC_FILTER_NOTE_OFF, FilterNoteOff},
            {IDC_FILTER_CONTROL_CHANGE, FilterControlChange},
            {IDC_FILTER_PROGRAM_CHANGE, FilterProgramChange},
            {IDC_FILTER_PITCH_BEND, FilterPitchBend},
            {IDC_FILTER_AFTERTOUCH, FilterAftertouch},
            {IDC_FILTER_POLY_AFTERTOUCH, FilterPolyAftertouch},
            {IDC_FILTER_SUSTAIN, FilterSustain},
            {IDC_FILTER_SYSEX, FilterSysEx},
            {IDC_FILTER_OUTPUT, FilterOutput},
            {IDC_FILTER_WARNINGS, FilterWarnings},
            {IDC_FILTER_ERRORS, FilterErrors},
        }};
        for (const auto& [controlId, bit] : controls) {
            if (id == controlId) {
                if (IsDlgButtonChecked(window, id) == BST_CHECKED) {
                    filters |= bit;
                } else {
                    filters &= ~bit;
                }
                Rebuild();
                return;
            }
        }
    }

    std::vector<StoredEvent> Snapshot(bool limited, bool& more,
                                      std::uint64_t& scannedThrough) {
        std::vector<StoredEvent> snapshot;
        std::lock_guard lock(mutex);
        more = false;
        scannedThrough = lastRenderedSequence;
        for (const StoredEvent& stored : events) {
            if (stored.sequence < visibleStartSequence ||
                stored.sequence <= lastRenderedSequence) {
                continue;
            }
            scannedThrough = stored.sequence;
            if (PassesFilters(stored.event, filters)) {
                snapshot.push_back(stored);
                if (limited && snapshot.size() >= kMaximumBatchEvents) {
                    more = stored.sequence + 1U < nextSequence;
                    break;
                }
            }
        }
        return snapshot;
    }

    std::vector<StoredEvent> FullSnapshot() {
        std::vector<StoredEvent> snapshot;
        std::lock_guard lock(mutex);
        snapshot.reserve(events.size());
        for (const StoredEvent& stored : events) {
            if (stored.sequence >= visibleStartSequence &&
                PassesFilters(stored.event, filters)) {
                snapshot.push_back(stored);
            }
        }
        lastRenderedSequence = nextSequence == 0 ? 0 : nextSequence - 1U;
        dirty = false;
        return snapshot;
    }

    std::vector<StoredEvent> VisibleSnapshot() {
        std::vector<StoredEvent> snapshot;
        std::lock_guard lock(mutex);
        snapshot.reserve(events.size());
        for (const StoredEvent& stored : events) {
            if (stored.sequence >= visibleStartSequence &&
                PassesFilters(stored.event, filters)) {
                snapshot.push_back(stored);
            }
        }
        return snapshot;
    }

    std::vector<RenderLine> MakeLines(const std::vector<StoredEvent>& snapshot) const {
        std::vector<RenderLine> lines;
        lines.reserve(snapshot.size());
        for (const StoredEvent& stored : snapshot) {
            lines.push_back({ColorFor(stored.event),
                             FormatLine(stored.event, wallOrigin, steadyOrigin)});
        }
        return lines;
    }

    void Stream(const std::vector<RenderLine>& lines, bool replace) {
        if (log == nullptr || (!replace && lines.empty())) {
            return;
        }
        const std::string rtf = BuildRtf(lines, fontName);
        StreamState state{rtf.data(), rtf.size(), 0};
        EDITSTREAM stream{};
        stream.dwCookie = reinterpret_cast<DWORD_PTR>(&state);
        stream.pfnCallback = &StreamCallback;
        SendMessageW(log, WM_SETREDRAW, FALSE, 0);
        SendMessageW(log, EM_SETREADONLY, FALSE, 0);
        if (replace) {
            SetWindowTextW(log, L"");
            SendMessageW(log, EM_SETSEL, 0, 0);
            SendMessageW(log, EM_STREAMIN, SF_RTF,
                         reinterpret_cast<LPARAM>(&stream));
        } else {
            SendMessageW(log, EM_SETSEL, static_cast<WPARAM>(-1),
                         static_cast<LPARAM>(-1));
            SendMessageW(log, EM_STREAMIN, SF_RTF | SFF_SELECTION,
                         reinterpret_cast<LPARAM>(&stream));
        }
        SendMessageW(log, EM_SETREADONLY, TRUE, 0);
        SendMessageW(log, EM_SETSEL, static_cast<WPARAM>(-1),
                     static_cast<LPARAM>(-1));
        SendMessageW(log, EM_SCROLLCARET, 0, 0);
        SendMessageW(log, WM_VSCROLL, SB_BOTTOM, 0);
        SendMessageW(log, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(log, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    }

    void Rebuild() {
        if (log == nullptr) {
            return;
        }
        const auto snapshot = FullSnapshot();
        Stream(MakeLines(snapshot), true);
    }

    void RenderPending() {
        if (!dirty.exchange(false) || log == nullptr) {
            return;
        }
        bool more = false;
        std::uint64_t scannedThrough = lastRenderedSequence;
        const auto snapshot = Snapshot(true, more, scannedThrough);
        lastRenderedSequence = scannedThrough;
        if (!snapshot.empty()) {
            Stream(MakeLines(snapshot), false);
        }
        if (more) {
            dirty = true;
        }
    }

    void UpdateStatus(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastStatusUpdate < std::chrono::milliseconds(250)) {
            return;
        }
        std::uint64_t total = 0;
        std::uint64_t dropped = 0;
        double average = 0;
        double peak = 0;
        {
            std::lock_guard lock(mutex);
            total = totalEvents;
            dropped = droppedEvents;
            average = totalEvents == 0 ? 0.0 : latencyTotal / static_cast<double>(totalEvents);
            peak = peakLatency;
        }
        const double elapsed = std::chrono::duration<double>(now - rateSampleTime).count();
        if (elapsed >= 0.5) {
            currentRate = static_cast<double>(total - rateSampleTotal) / elapsed;
            rateSampleTotal = total;
            rateSampleTime = now;
        }
        lastStatusUpdate = now;
        RECT bounds{};
        GetClientRect(window, &bounds);
        const int width = bounds.right - bounds.left;
        const int part = std::max(100, width / 5);
        std::array<int, 5> parts{part, part * 2, part * 3, part * 4, -1};
        SendMessageW(status, SB_SETPARTS, static_cast<WPARAM>(parts.size()),
                     reinterpret_cast<LPARAM>(parts.data()));
        statusValues = {
            L"Total events: " + std::to_wstring(total),
            L"Dropped events: " + std::to_wstring(dropped),
            L"Average latency: " + FormatLatency(average),
            L"Peak latency: " + FormatLatency(peak),
            L"Events/sec: " + FormatRate(currentRate),
        };
        for (std::size_t index = 0; index < statusValues.size(); ++index) {
            SendMessageW(status, SB_SETTEXTW,
                         static_cast<WPARAM>(index) | SBT_OWNERDRAW | SBT_NOBORDERS,
                         reinterpret_cast<LPARAM>(statusValues[index].c_str()));
        }
    }

    void DrawStatus(DRAWITEMSTRUCT* item) const {
        if (item == nullptr) {
            return;
        }
        HBRUSH brush = CreateSolidBrush(RGB(28, 28, 28));
        FillRect(item->hDC, &item->rcItem, brush);
        DeleteObject(brush);
        RECT textBounds = item->rcItem;
        textBounds.left += 8;
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, RGB(220, 220, 220));
        const auto* text = reinterpret_cast<const wchar_t*>(item->itemData);
        if (text != nullptr) {
            DrawTextW(item->hDC, text, -1, &textBounds,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    static std::wstring FormatLatency(double microseconds) {
        std::wostringstream text;
        if (microseconds >= 1000.0) {
            text << std::fixed << std::setprecision(3) << microseconds / 1000.0 << L" ms";
        } else {
            text << std::fixed << std::setprecision(1) << microseconds << L" us";
        }
        return text.str();
    }

    static std::wstring FormatRate(double rate) {
        std::wostringstream text;
        text << std::fixed << std::setprecision(1) << rate;
        return text.str();
    }

    void Layout() {
        if (window == nullptr || header == nullptr || log == nullptr || status == nullptr) {
            return;
        }
        RECT client{};
        GetClientRect(window, &client);
        RECT statusBounds{};
        GetWindowRect(status, &statusBounds);
        const int statusHeight = std::max(
            20, static_cast<int>(statusBounds.bottom - statusBounds.top));
        const int width = static_cast<int>(client.right - client.left);
        const int height = static_cast<int>(client.bottom - client.top);
        HWND clear = GetDlgItem(window, IDC_MONITOR_CLEAR);
        HWND save = GetDlgItem(window, IDC_MONITOR_SAVE);
        RECT clearBounds{};
        RECT saveBounds{};
        GetWindowRect(clear, &clearBounds);
        GetWindowRect(save, &saveBounds);
        const int clearWidth = static_cast<int>(clearBounds.right - clearBounds.left);
        const int clearHeight = static_cast<int>(clearBounds.bottom - clearBounds.top);
        const int saveWidth = static_cast<int>(saveBounds.right - saveBounds.left);
        const int saveHeight = static_cast<int>(saveBounds.bottom - saveBounds.top);
        SetWindowPos(save, nullptr, width - saveWidth - 10, 8, saveWidth, saveHeight,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        SetWindowPos(clear, nullptr, width - saveWidth - clearWidth - 18, 8,
                     clearWidth, clearHeight, SWP_NOACTIVATE | SWP_NOZORDER);
        SetWindowPos(header, nullptr, 8, headerTop, std::max(1, width - 16),
                     headerHeight, SWP_NOACTIVATE | SWP_NOZORDER);
        SetWindowPos(log, nullptr, 8, logTop, std::max(1, width - 16),
                     std::max(1, height - logTop - statusHeight - 4),
                     SWP_NOACTIVATE | SWP_NOZORDER);
        SetWindowPos(status, nullptr, 0, height - statusHeight, width, statusHeight,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        UpdateStatus(true);
    }

    void CenterWindow() const {
        RECT monitorBounds{};
        RECT windowBounds{};
        GetWindowRect(window, &windowBounds);
        HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO information{};
        information.cbSize = sizeof(information);
        if (!GetMonitorInfoW(monitor, &information)) {
            return;
        }
        monitorBounds = information.rcWork;
        const int width = windowBounds.right - windowBounds.left;
        const int height = windowBounds.bottom - windowBounds.top;
        const int x = monitorBounds.left +
                      (monitorBounds.right - monitorBounds.left - width) / 2;
        const int y = monitorBounds.top +
                      (monitorBounds.bottom - monitorBounds.top - height) / 2;
        SetWindowPos(window, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }

    std::wstring VisibleText() {
        const auto snapshot = VisibleSnapshot();
        std::wstring text = kHeader;
        text.reserve(snapshot.size() * 180U + 256U);
        text += L"\r\n";
        text += kSeparator;
        text += L"\r\n";
        for (const StoredEvent& stored : snapshot) {
            text += FormatLine(stored.event, wallOrigin, steadyOrigin);
            text += L"\r\n";
        }
        return text;
    }

    void Save() {
        std::array<wchar_t, 32'768> filename{};
        wcscpy_s(filename.data(), filename.size(), L"MidiToRblx-monitor.txt");
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window;
        dialog.hInstance = instance;
        dialog.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrDefExt = L"txt";
        dialog.lpstrTitle = L"Save Debug Monitor Log";
        dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT |
                       OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog)) {
            return;
        }
        const std::string utf8 = WideToUtf8(VisibleText());
        HANDLE file = CreateFileW(filename.data(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            MessageBoxW(window, L"The log file could not be created.",
                        L"Save Debug Monitor Log", MB_OK | MB_ICONERROR);
            return;
        }
        static constexpr std::array<unsigned char, 3> bom{0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        bool success = WriteFile(file, bom.data(), static_cast<DWORD>(bom.size()),
                                 &written, nullptr) != FALSE && written == bom.size();
        if (success && !utf8.empty()) {
            success = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()),
                                &written, nullptr) != FALSE && written == utf8.size();
        }
        CloseHandle(file);
        if (!success) {
            MessageBoxW(window, L"The complete log could not be written.",
                        L"Save Debug Monitor Log", MB_OK | MB_ICONERROR);
        }
    }

    HINSTANCE instance = nullptr;
    HMODULE richEditLibrary = nullptr;
    HWND owner = nullptr;
    HWND window = nullptr;
    HWND header = nullptr;
    HWND log = nullptr;
    HWND status = nullptr;
    HFONT logFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    int headerTop = 70;
    int headerHeight = 40;
    int logTop = 70;
    std::wstring fontName;
    std::chrono::steady_clock::time_point steadyOrigin;
    std::chrono::system_clock::time_point wallOrigin;
    std::mutex mutex;
    std::deque<StoredEvent> events;
    std::uint64_t nextSequence = 1;
    std::uint64_t visibleStartSequence = 1;
    std::uint64_t lastRenderedSequence = 0;
    std::uint64_t totalEvents = 0;
    std::uint64_t droppedEvents = 0;
    double latencyTotal = 0;
    double peakLatency = 0;
    std::atomic_bool dirty{false};
    std::atomic_bool trimPending{false};
    bool rebuildPending = false;
    std::uint64_t evictedStoredEvents = 0;
    std::uint32_t filters = kAllFilters;
    std::chrono::steady_clock::time_point lastStatusUpdate{};
    std::chrono::steady_clock::time_point rateSampleTime = std::chrono::steady_clock::now();
    std::uint64_t rateSampleTotal = 0;
    double currentRate = 0;
    std::array<std::wstring, 5> statusValues{};
};

DebugMonitor::DebugMonitor(HINSTANCE instance)
    : impl_(std::make_unique<Impl>(instance)) {}

DebugMonitor::~DebugMonitor() = default;

void DebugMonitor::Open(HWND owner) {
    impl_->Open(owner);
}

void DebugMonitor::Close() {
    impl_->Close();
}

bool DebugMonitor::ProcessDialogMessage(MSG& message) {
    return impl_->ProcessDialogMessage(message);
}

void DebugMonitor::Record(MonitorEvent event) {
    impl_->Record(std::move(event));
}

void DebugMonitor::RecordInformation(std::wstring message) {
    impl_->RecordDiagnostic(std::move(message), MonitorSeverity::Normal);
}

void DebugMonitor::RecordWarning(std::wstring message) {
    impl_->RecordDiagnostic(std::move(message), MonitorSeverity::Warning);
}

void DebugMonitor::RecordError(std::wstring message) {
    impl_->RecordDiagnostic(std::move(message), MonitorSeverity::Error);
}
