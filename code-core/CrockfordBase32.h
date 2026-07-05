// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vishwakarma::crockford_base32 {

inline constexpr size_t kEncodedUInt64Length = 13;
inline constexpr size_t kEncodedUInt64LengthWithNull = kEncodedUInt64Length + 1;
inline constexpr char kAlphabet[33] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
inline constexpr char kMaxUInt64Text[kEncodedUInt64LengthWithNull] = "FZZZZZZZZZZZZ";

namespace detail {

inline constexpr uint8_t kInvalidDigit = 0xFFu;

constexpr std::array<uint8_t, 256> MakeDecodeTable() noexcept {
    std::array<uint8_t, 256> table{};

    for (uint8_t& value : table) {
        value = kInvalidDigit;
    }

    for (uint8_t digit = 0; digit <= 9; ++digit) {
        table[static_cast<size_t>('0' + digit)] = digit;
    }

    table[static_cast<size_t>('A')] = 10;
    table[static_cast<size_t>('a')] = 10;
    table[static_cast<size_t>('B')] = 11;
    table[static_cast<size_t>('b')] = 11;
    table[static_cast<size_t>('C')] = 12;
    table[static_cast<size_t>('c')] = 12;
    table[static_cast<size_t>('D')] = 13;
    table[static_cast<size_t>('d')] = 13;
    table[static_cast<size_t>('E')] = 14;
    table[static_cast<size_t>('e')] = 14;
    table[static_cast<size_t>('F')] = 15;
    table[static_cast<size_t>('f')] = 15;
    table[static_cast<size_t>('G')] = 16;
    table[static_cast<size_t>('g')] = 16;
    table[static_cast<size_t>('H')] = 17;
    table[static_cast<size_t>('h')] = 17;
    table[static_cast<size_t>('J')] = 18;
    table[static_cast<size_t>('j')] = 18;
    table[static_cast<size_t>('K')] = 19;
    table[static_cast<size_t>('k')] = 19;
    table[static_cast<size_t>('M')] = 20;
    table[static_cast<size_t>('m')] = 20;
    table[static_cast<size_t>('N')] = 21;
    table[static_cast<size_t>('n')] = 21;
    table[static_cast<size_t>('P')] = 22;
    table[static_cast<size_t>('p')] = 22;
    table[static_cast<size_t>('Q')] = 23;
    table[static_cast<size_t>('q')] = 23;
    table[static_cast<size_t>('R')] = 24;
    table[static_cast<size_t>('r')] = 24;
    table[static_cast<size_t>('S')] = 25;
    table[static_cast<size_t>('s')] = 25;
    table[static_cast<size_t>('T')] = 26;
    table[static_cast<size_t>('t')] = 26;
    table[static_cast<size_t>('V')] = 27;
    table[static_cast<size_t>('v')] = 27;
    table[static_cast<size_t>('W')] = 28;
    table[static_cast<size_t>('w')] = 28;
    table[static_cast<size_t>('X')] = 29;
    table[static_cast<size_t>('x')] = 29;
    table[static_cast<size_t>('Y')] = 30;
    table[static_cast<size_t>('y')] = 30;
    table[static_cast<size_t>('Z')] = 31;
    table[static_cast<size_t>('z')] = 31;

    table[static_cast<size_t>('I')] = 1;
    table[static_cast<size_t>('i')] = 1;
    table[static_cast<size_t>('L')] = 1;
    table[static_cast<size_t>('l')] = 1;
    table[static_cast<size_t>('O')] = 0;
    table[static_cast<size_t>('o')] = 0;

    return table;
}

inline constexpr std::array<uint8_t, 256> kDecodeTable = MakeDecodeTable();

[[nodiscard]] inline constexpr uint8_t DecodeByte(char c) noexcept {
    return kDecodeTable[static_cast<size_t>(static_cast<unsigned char>(c))];
}

} // namespace detail

[[nodiscard]] inline constexpr uint8_t DecodeCharacter(char c) noexcept {
    return detail::DecodeByte(c);
}

[[nodiscard]] inline constexpr bool IsValidCharacter(char c) noexcept {
    return DecodeCharacter(c) != detail::kInvalidDigit;
}

// Writes exactly 13 Crockford Base32 digits. The caller owns buffer sizing.
inline void EncodeUInt64ToChars(uint64_t value, char* out) noexcept {
    out[0] = kAlphabet[static_cast<size_t>((value >> 60) & 0x0Fu)];
    out[1] = kAlphabet[static_cast<size_t>((value >> 55) & 0x1Fu)];
    out[2] = kAlphabet[static_cast<size_t>((value >> 50) & 0x1Fu)];
    out[3] = kAlphabet[static_cast<size_t>((value >> 45) & 0x1Fu)];
    out[4] = kAlphabet[static_cast<size_t>((value >> 40) & 0x1Fu)];
    out[5] = kAlphabet[static_cast<size_t>((value >> 35) & 0x1Fu)];
    out[6] = kAlphabet[static_cast<size_t>((value >> 30) & 0x1Fu)];
    out[7] = kAlphabet[static_cast<size_t>((value >> 25) & 0x1Fu)];
    out[8] = kAlphabet[static_cast<size_t>((value >> 20) & 0x1Fu)];
    out[9] = kAlphabet[static_cast<size_t>((value >> 15) & 0x1Fu)];
    out[10] = kAlphabet[static_cast<size_t>((value >> 10) & 0x1Fu)];
    out[11] = kAlphabet[static_cast<size_t>((value >> 5) & 0x1Fu)];
    out[12] = kAlphabet[static_cast<size_t>(value & 0x1Fu)];
}

// Writes 13 digits plus a trailing '\0'. The caller owns buffer sizing.
inline void EncodeUInt64ToCString(uint64_t value, char* out) noexcept {
    EncodeUInt64ToChars(value, out);
    out[kEncodedUInt64Length] = '\0';
}

[[nodiscard]] inline std::array<char, kEncodedUInt64Length> EncodeUInt64ToArray(uint64_t value) noexcept {
    std::array<char, kEncodedUInt64Length> result{};
    EncodeUInt64ToChars(value, result.data());
    return result;
}

[[nodiscard]] inline std::string EncodeUInt64(uint64_t value) {
    std::string result(kEncodedUInt64Length, '\0');
    EncodeUInt64ToChars(value, result.data());
    return result;
}

// Decodes exactly 13 characters. Lowercase, I/L as 1, and O as 0 are accepted.
[[nodiscard]] inline bool TryDecodeUInt64FromChars(const char* text, uint64_t& value) noexcept {
    const uint32_t d0 = detail::DecodeByte(text[0]);
    const uint32_t d1 = detail::DecodeByte(text[1]);
    const uint32_t d2 = detail::DecodeByte(text[2]);
    const uint32_t d3 = detail::DecodeByte(text[3]);
    const uint32_t d4 = detail::DecodeByte(text[4]);
    const uint32_t d5 = detail::DecodeByte(text[5]);
    const uint32_t d6 = detail::DecodeByte(text[6]);
    const uint32_t d7 = detail::DecodeByte(text[7]);
    const uint32_t d8 = detail::DecodeByte(text[8]);
    const uint32_t d9 = detail::DecodeByte(text[9]);
    const uint32_t d10 = detail::DecodeByte(text[10]);
    const uint32_t d11 = detail::DecodeByte(text[11]);
    const uint32_t d12 = detail::DecodeByte(text[12]);

    const uint32_t invalidBits =
        (d0 | d1 | d2 | d3 | d4 | d5 | d6 | d7 | d8 | d9 | d10 | d11 | d12) & 0xE0u;

    // Thirteen Base32 digits carry 65 bits; uint64_t permits only 0..F in digit 0.
    if (invalidBits != 0 || (d0 & 0xF0u) != 0) {
        return false;
    }

    value =
        (static_cast<uint64_t>(d0) << 60) |
        (static_cast<uint64_t>(d1) << 55) |
        (static_cast<uint64_t>(d2) << 50) |
        (static_cast<uint64_t>(d3) << 45) |
        (static_cast<uint64_t>(d4) << 40) |
        (static_cast<uint64_t>(d5) << 35) |
        (static_cast<uint64_t>(d6) << 30) |
        (static_cast<uint64_t>(d7) << 25) |
        (static_cast<uint64_t>(d8) << 20) |
        (static_cast<uint64_t>(d9) << 15) |
        (static_cast<uint64_t>(d10) << 10) |
        (static_cast<uint64_t>(d11) << 5) |
        static_cast<uint64_t>(d12);

    return true;
}

[[nodiscard]] inline bool TryDecodeUInt64(std::string_view text, uint64_t& value) noexcept {
    if (text.size() != kEncodedUInt64Length) {
        return false;
    }

    return TryDecodeUInt64FromChars(text.data(), value);
}

[[nodiscard]] inline uint64_t DecodeUInt64(std::string_view text) {
    uint64_t value = 0;
    if (!TryDecodeUInt64(text, value)) {
        throw std::invalid_argument("Invalid Crockford Base32 uint64 text.");
    }

    return value;
}

inline void EncodeUInt64Batch(const uint64_t* values, size_t count, char* out) noexcept {
    for (size_t i = 0; i < count; ++i) {
        EncodeUInt64ToChars(values[i], out + (i * kEncodedUInt64Length));
    }
}

[[nodiscard]] inline bool TryDecodeUInt64Batch(const char* text, size_t count, uint64_t* values) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (!TryDecodeUInt64FromChars(text + (i * kEncodedUInt64Length), values[i])) {
            return false;
        }
    }

    return true;
}

} // namespace vishwakarma::crockford_base32

