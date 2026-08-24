#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace midi {

enum class EventKind : std::uint8_t {
    NoteOn,
    NoteOff,
    ControlChange,
    ProgramChange,
    PitchBend,
    Aftertouch,
    PolyAftertouch,
    SysEx,
    Unknown,
};

struct Event {
    std::uint64_t playbackMicroseconds = 0;
    EventKind kind = EventKind::NoteOn;
    std::uint8_t channel = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    std::uint32_t value = 0;
};

struct Song {
    std::vector<Event> events;
    std::uint64_t durationMicroseconds = 0;
    std::uint64_t playbackDurationMicroseconds = 0;
    std::uint16_t format = 0;
    std::uint16_t trackCount = 0;
};

bool LoadFile(const std::filesystem::path& path,
              Song& song,
              std::wstring& error,
              const std::atomic_bool* cancel = nullptr);

}
