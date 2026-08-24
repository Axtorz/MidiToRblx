#include "midi_file.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <queue>
#include <utility>

namespace midi {
namespace {

constexpr std::uint64_t kMaxCompatibilityGapUs = 10'000'000;

enum class RawKind : std::uint8_t {
    Marker,
    NoteOn,
    NoteOff,
    ControlChange,
    ProgramChange,
    PitchBend,
    Aftertouch,
    PolyAftertouch,
    SysEx,
    Unknown,
    Tempo,
};

struct RawEvent {
    std::uint64_t tick = 0;
    std::uint32_t value = 0;
    RawKind kind = RawKind::Marker;
    std::uint8_t channel = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    std::uint8_t status = 0;
};

bool ReadExact(std::istream& input, void* destination, std::size_t size) {
    input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return input.good() || input.gcount() == static_cast<std::streamsize>(size);
}

bool ReadU16(std::istream& input, std::uint16_t& value) {
    std::array<unsigned char, 2> bytes{};
    if (!ReadExact(input, bytes.data(), bytes.size())) {
        return false;
    }
    value = static_cast<std::uint16_t>((bytes[0] << 8U) | bytes[1]);
    return true;
}

bool ReadU32(std::istream& input, std::uint32_t& value) {
    std::array<unsigned char, 4> bytes{};
    if (!ReadExact(input, bytes.data(), bytes.size())) {
        return false;
    }
    value = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
            (static_cast<std::uint32_t>(bytes[1]) << 16U) |
            (static_cast<std::uint32_t>(bytes[2]) << 8U) |
            static_cast<std::uint32_t>(bytes[3]);
    return true;
}

class TrackReader {
public:
    TrackReader(std::istream& input, std::uint64_t length)
        : input_(input), remaining_(length) {}

    [[nodiscard]] std::uint64_t remaining() const { return remaining_; }

    bool ReadByte(std::uint8_t& value) {
        if (remaining_ == 0) {
            return false;
        }
        char byte = 0;
        input_.read(&byte, 1);
        if (!input_) {
            return false;
        }
        value = static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
        --remaining_;
        return true;
    }

    bool ReadVlq(std::uint32_t& value) {
        value = 0;
        for (int index = 0; index < 4; ++index) {
            std::uint8_t byte = 0;
            if (!ReadByte(byte)) {
                return false;
            }
            value = (value << 7U) | (byte & 0x7FU);
            if ((byte & 0x80U) == 0) {
                return true;
            }
        }
        return false;
    }

    bool Skip(std::uint64_t count) {
        if (count > remaining_) {
            return false;
        }
        constexpr std::streamoff kMaxSeek =
            static_cast<std::streamoff>(std::numeric_limits<std::streamoff>::max());
        while (count != 0) {
            const auto part = static_cast<std::streamoff>(
                std::min<std::uint64_t>(count, static_cast<std::uint64_t>(kMaxSeek)));
            input_.seekg(part, std::ios::cur);
            if (!input_) {
                return false;
            }
            remaining_ -= static_cast<std::uint64_t>(part);
            count -= static_cast<std::uint64_t>(part);
        }
        return true;
    }

private:
    std::istream& input_;
    std::uint64_t remaining_;
};

bool IsCancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

bool AddTicks(std::uint64_t& tick, std::uint32_t delta) {
    if (tick > std::numeric_limits<std::uint64_t>::max() - delta) {
        return false;
    }
    tick += delta;
    return true;
}

bool ParseTrack(std::istream& input,
                std::uint32_t length,
                std::vector<RawEvent>& events,
                std::uint64_t& endTick,
                std::wstring& error,
                const std::atomic_bool* cancel) {
    TrackReader reader(input, length);
    std::uint64_t tick = 0;
    std::uint8_t runningStatus = 0;
    std::uint64_t eventCounter = 0;

    while (reader.remaining() != 0) {
        if ((eventCounter++ & 0xFFFU) == 0 && IsCancelled(cancel)) {
            error = L"Loading was cancelled.";
            return false;
        }

        std::uint32_t delta = 0;
        if (!reader.ReadVlq(delta) || !AddTicks(tick, delta)) {
            error = L"The MIDI track contains an invalid delta time.";
            return false;
        }
        endTick = std::max(endTick, tick);

        std::uint8_t first = 0;
        if (!reader.ReadByte(first)) {
            error = L"The MIDI track ends in the middle of an event.";
            return false;
        }

        std::uint8_t status = 0;
        bool hasFirstDataByte = false;
        std::uint8_t firstDataByte = 0;
        if ((first & 0x80U) != 0) {
            status = first;
            if (status >= 0x80U && status <= 0xEFU) {
                runningStatus = status;
            } else if (status < 0xF8U) {
                runningStatus = 0;
            }
        } else {
            if (runningStatus == 0) {
                error = L"The MIDI track uses running status before a channel status byte.";
                return false;
            }
            status = runningStatus;
            hasFirstDataByte = true;
            firstDataByte = first;
        }

        if (status == 0xFFU) {
            std::uint8_t metaType = 0;
            std::uint32_t metaLength = 0;
            if (!reader.ReadByte(metaType) || !reader.ReadVlq(metaLength) ||
                metaLength > reader.remaining()) {
                error = L"The MIDI file contains a truncated meta event.";
                return false;
            }

            if (metaType == 0x51U && metaLength == 3) {
                std::uint8_t a = 0;
                std::uint8_t b = 0;
                std::uint8_t c = 0;
                if (!reader.ReadByte(a) || !reader.ReadByte(b) || !reader.ReadByte(c)) {
                    error = L"The MIDI file contains a truncated tempo event.";
                    return false;
                }
                const std::uint32_t tempo =
                    (static_cast<std::uint32_t>(a) << 16U) |
                    (static_cast<std::uint32_t>(b) << 8U) |
                    static_cast<std::uint32_t>(c);
                if (tempo != 0) {
                    events.push_back({tick, tempo, RawKind::Tempo, 0, 0, 0});
                } else {
                    events.push_back({tick, 0, RawKind::Marker, 0, 0, 0});
                }
            } else {
                if (!reader.Skip(metaLength)) {
                    error = L"The MIDI file contains a truncated meta event.";
                    return false;
                }
                if (metaType != 0x2FU) {
                    events.push_back({tick, 0, RawKind::Marker, 0, 0, 0});
                }
            }
            continue;
        }

        if (status == 0xF0U || status == 0xF7U) {
            std::uint32_t sysexLength = 0;
            if (!reader.ReadVlq(sysexLength) || !reader.Skip(sysexLength)) {
                error = L"The MIDI file contains a truncated SysEx event.";
                return false;
            }
            events.push_back({tick, sysexLength, RawKind::SysEx, 0, 0, 0, status});
            continue;
        }

        if (status >= 0x80U && status <= 0xEFU) {
            const std::uint8_t category = status & 0xF0U;
            const int dataLength = (category == 0xC0U || category == 0xD0U) ? 1 : 2;
            std::uint8_t data1 = 0;
            std::uint8_t data2 = 0;
            if (hasFirstDataByte) {
                data1 = firstDataByte;
            } else if (!reader.ReadByte(data1)) {
                error = L"The MIDI file contains a truncated channel event.";
                return false;
            }
            if (dataLength == 2 && !reader.ReadByte(data2)) {
                error = L"The MIDI file contains a truncated channel event.";
                return false;
            }
            if ((data1 & 0x80U) != 0 || (dataLength == 2 && (data2 & 0x80U) != 0)) {
                error = L"The MIDI file contains an invalid channel data byte.";
                return false;
            }

            RawKind kind = RawKind::Marker;
            if (category == 0x80U) {
                kind = RawKind::NoteOff;
            } else if (category == 0x90U) {
                kind = RawKind::NoteOn;
            } else if (category == 0xA0U) {
                kind = RawKind::PolyAftertouch;
            } else if (category == 0xB0U) {
                kind = RawKind::ControlChange;
            } else if (category == 0xC0U) {
                kind = RawKind::ProgramChange;
            } else if (category == 0xD0U) {
                kind = RawKind::Aftertouch;
            } else if (category == 0xE0U) {
                kind = RawKind::PitchBend;
            } else {
                kind = RawKind::Unknown;
            }
            events.push_back({tick, 0, kind, static_cast<std::uint8_t>(status & 0x0FU),
                              data1, data2, status});
            continue;
        }

        int systemDataLength = 0;
        switch (status) {
            case 0xF1U:
            case 0xF3U:
                systemDataLength = 1;
                break;
            case 0xF2U:
                systemDataLength = 2;
                break;
            default:
                systemDataLength = 0;
                break;
        }
        if (!reader.Skip(static_cast<std::uint64_t>(systemDataLength))) {
            error = L"The MIDI file contains a truncated system event.";
            return false;
        }
    }

    return true;
}

long double TickDeltaToMicroseconds(std::uint64_t ticks,
                                    std::uint16_t division,
                                    std::uint32_t tempo) {
    if ((division & 0x8000U) == 0) {
        return static_cast<long double>(ticks) * static_cast<long double>(tempo) /
               static_cast<long double>(division);
    }

    const auto signedFrames = static_cast<std::int8_t>(division >> 8U);
    const std::uint8_t ticksPerFrame = static_cast<std::uint8_t>(division & 0xFFU);
    if (signedFrames == 0 || ticksPerFrame == 0) {
        return 0;
    }
    const int frameCode = -static_cast<int>(signedFrames);
    const long double framesPerSecond =
        frameCode == 29 ? (30'000.0L / 1'001.0L) : static_cast<long double>(frameCode);
    return static_cast<long double>(ticks) * 1'000'000.0L /
           (framesPerSecond * static_cast<long double>(ticksPerFrame));
}

std::uint64_t ClampMicroseconds(long double value) {
    if (value <= 0) {
        return 0;
    }
    const long double maximum =
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    if (value >= maximum) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(value + 0.5L);
}

}

bool LoadFile(const std::filesystem::path& path,
              Song& song,
              std::wstring& error,
              const std::atomic_bool* cancel) {
    song = {};
    error.clear();

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"The selected MIDI file could not be opened.";
        return false;
    }

    std::array<char, 4> chunkId{};
    std::uint32_t headerLength = 0;
    if (!ReadExact(input, chunkId.data(), chunkId.size()) ||
        chunkId != std::array<char, 4>{'M', 'T', 'h', 'd'} ||
        !ReadU32(input, headerLength) || headerLength < 6) {
        error = L"The selected file is not a valid Standard MIDI File.";
        return false;
    }

    std::uint16_t division = 0;
    if (!ReadU16(input, song.format) || !ReadU16(input, song.trackCount) ||
        !ReadU16(input, division)) {
        error = L"The MIDI header is truncated.";
        return false;
    }
    if (song.format > 2 || song.trackCount == 0 || division == 0 ||
        ((division & 0x8000U) != 0 && (division & 0xFFU) == 0)) {
        error = L"The MIDI header contains an unsupported format or time division.";
        return false;
    }
    if (headerLength > 6) {
        input.seekg(static_cast<std::streamoff>(headerLength - 6), std::ios::cur);
        if (!input) {
            error = L"The MIDI header is truncated.";
            return false;
        }
    }

    std::vector<std::vector<RawEvent>> tracks;
    tracks.reserve(song.trackCount);
    std::uint64_t finalTick = 0;

    while (tracks.size() < song.trackCount) {
        if (IsCancelled(cancel)) {
            error = L"Loading was cancelled.";
            return false;
        }
        std::uint32_t chunkLength = 0;
        if (!ReadExact(input, chunkId.data(), chunkId.size()) || !ReadU32(input, chunkLength)) {
            error = L"The MIDI file ends before all declared tracks were found.";
            return false;
        }
        if (chunkId != std::array<char, 4>{'M', 'T', 'r', 'k'}) {
            input.seekg(static_cast<std::streamoff>(chunkLength), std::ios::cur);
            if (!input) {
                error = L"A MIDI chunk is truncated.";
                return false;
            }
            continue;
        }

        tracks.emplace_back();
        auto& track = tracks.back();

        track.reserve(std::min<std::uint32_t>(chunkLength / 4U, 1'000'000U));
        std::uint64_t trackEndTick = 0;
        if (!ParseTrack(input, chunkLength, track, trackEndTick, error, cancel)) {
            return false;
        }
        finalTick = std::max(finalTick, trackEndTick);
    }

    tracks.push_back({RawEvent{finalTick, 0, RawKind::Marker, 0, 0, 0}});

    struct Cursor {
        std::uint64_t tick = 0;
        std::size_t track = 0;
        std::size_t index = 0;
    };
    const auto later = [](const Cursor& left, const Cursor& right) {
        if (left.tick != right.tick) {
            return left.tick > right.tick;
        }
        if (left.track != right.track) {
            return left.track > right.track;
        }
        return left.index > right.index;
    };
    std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> queue(later);
    std::size_t usefulCount = 0;
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        if (!tracks[trackIndex].empty()) {
            queue.push({tracks[trackIndex][0].tick, trackIndex, 0});
        }
        for (const RawEvent& event : tracks[trackIndex]) {
            if (event.kind != RawKind::Marker && event.kind != RawKind::Tempo) {
                ++usefulCount;
            }
        }
    }
    song.events.reserve(usefulCount);

    std::uint64_t previousTick = 0;
    std::uint32_t tempo = 500'000;
    long double durationUs = 0;
    long double playbackUs = 0;
    std::uint64_t mergeCounter = 0;

    while (!queue.empty()) {
        if ((mergeCounter++ & 0xFFFU) == 0 && IsCancelled(cancel)) {
            error = L"Loading was cancelled.";
            song = {};
            return false;
        }
        const Cursor cursor = queue.top();
        queue.pop();
        const RawEvent& raw = tracks[cursor.track][cursor.index];
        const std::uint64_t deltaTicks = raw.tick - previousTick;
        const long double deltaUs = TickDeltaToMicroseconds(deltaTicks, division, tempo);
        durationUs += deltaUs;
        playbackUs += std::min<long double>(deltaUs, kMaxCompatibilityGapUs);
        previousTick = raw.tick;

        if (raw.kind == RawKind::Tempo && (division & 0x8000U) == 0 && raw.value != 0) {
            tempo = raw.value;
        } else if (raw.kind != RawKind::Marker) {
            Event output;
            output.playbackMicroseconds = ClampMicroseconds(playbackUs);
            output.channel = raw.channel;
            output.status = raw.status;
            output.data1 = raw.data1;
            output.data2 = raw.data2;
            output.value = raw.value;
            switch (raw.kind) {
                case RawKind::NoteOn:
                    output.kind = EventKind::NoteOn;
                    break;
                case RawKind::NoteOff:
                    output.kind = EventKind::NoteOff;
                    break;
                case RawKind::ControlChange:
                    output.kind = EventKind::ControlChange;
                    break;
                case RawKind::ProgramChange:
                    output.kind = EventKind::ProgramChange;
                    break;
                case RawKind::PitchBend:
                    output.kind = EventKind::PitchBend;
                    break;
                case RawKind::Aftertouch:
                    output.kind = EventKind::Aftertouch;
                    break;
                case RawKind::PolyAftertouch:
                    output.kind = EventKind::PolyAftertouch;
                    break;
                case RawKind::SysEx:
                    output.kind = EventKind::SysEx;
                    break;
                case RawKind::Unknown:
                    output.kind = EventKind::Unknown;
                    break;
                case RawKind::Tempo:
                case RawKind::Marker:
                    output.kind = EventKind::Unknown;
                    break;
            }
            song.events.push_back(output);
        }

        const std::size_t nextIndex = cursor.index + 1;
        if (nextIndex < tracks[cursor.track].size()) {
            queue.push({tracks[cursor.track][nextIndex].tick, cursor.track, nextIndex});
        }
    }

    song.durationMicroseconds = ClampMicroseconds(durationUs);
    song.playbackDurationMicroseconds = ClampMicroseconds(playbackUs);
    return true;
}

}
