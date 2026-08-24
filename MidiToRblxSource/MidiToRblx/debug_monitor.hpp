#pragma once

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class MonitorSource : std::uint8_t {
    Live,
    File,
    Application,
};

enum class MonitorEventType : std::uint8_t {
    NoteOn,
    NoteOff,
    ControlChange,
    ProgramChange,
    PitchBend,
    Aftertouch,
    PolyAftertouch,
    Sustain,
    SysEx,
    Unknown,
    Information,
};

enum class MonitorSeverity : std::uint8_t {
    Normal,
    Warning,
    Error,
};

struct MonitorEvent {
    std::chrono::steady_clock::time_point timestamp = std::chrono::steady_clock::now();
    MonitorSource source = MonitorSource::Application;
    MonitorEventType type = MonitorEventType::Information;
    MonitorSeverity severity = MonitorSeverity::Normal;
    std::uint32_t deviceId = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    bool hasData1 = false;
    bool hasData2 = false;
    std::uint32_t payloadLength = 0;
    std::array<std::uint8_t, 16> payloadPreview{};
    std::size_t payloadPreviewLength = 0;
    std::string output;
    double latencyMicroseconds = 0.0;
    bool counted = true;
    std::wstring message;
};

MonitorEventType ClassifyMonitorEvent(std::uint8_t status,
                                      std::uint8_t data1,
                                      std::uint8_t data2);

class DebugMonitor {
public:
    explicit DebugMonitor(HINSTANCE instance);
    ~DebugMonitor();
    DebugMonitor(const DebugMonitor&) = delete;
    DebugMonitor& operator=(const DebugMonitor&) = delete;

    void Open(HWND owner);
    void Close();
    bool ProcessDialogMessage(MSG& message);
    void Record(MonitorEvent event);
    void RecordInformation(std::wstring message);
    void RecordWarning(std::wstring message);
    void RecordError(std::wstring message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
