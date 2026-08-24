#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace protocol {

constexpr std::array<std::uint8_t, 4> Encode(std::uint8_t identifier,
                                             std::uint8_t value) {
    return {
        static_cast<std::uint8_t>(identifier / 12U),
        static_cast<std::uint8_t>(identifier % 12U),
        static_cast<std::uint8_t>(value / 12U),
        static_cast<std::uint8_t>(value % 12U),
    };
}

inline std::string Format(std::uint8_t identifier, std::uint8_t value) {
    static constexpr std::array<char, 12> symbols{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '+'};
    const auto digits = Encode(identifier, value);
    std::string output{"*"};
    for (const std::uint8_t digit : digits) {
        output.push_back(symbols[digit]);
    }
    return output;
}

}
