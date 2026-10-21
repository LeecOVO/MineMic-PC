#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace minemic {
constexpr std::uint32_t SampleRate = 48000;
constexpr std::size_t FramesPerPacket = 480;
constexpr std::size_t HeaderSize = 32;
constexpr std::size_t TagSize = 32;
constexpr std::size_t MaxPayload = FramesPerPacket * 2;
constexpr std::size_t MaxDatagram = HeaderSize + MaxPayload + TagSize;
enum class PacketType : std::uint8_t { Hello = 1, Welcome = 2, Audio = 3, Bye = 4, Heartbeat = 5 };

struct Packet {
    PacketType type = PacketType::Hello;
    std::uint32_t session = 0;
    std::uint32_t sequence = 0;
    std::uint32_t timestamp = 0; // Capture clock in 48 kHz frames, modulo 2^32.
    std::vector<std::uint8_t> payload;
};

// Encodes/decodes the authenticated body only. The network layer appends/checks HMAC.
std::vector<std::uint8_t> encodePacket(const Packet& packet);
bool decodePacket(const std::uint8_t* bytes, std::size_t size, Packet& packet);
std::array<std::int16_t, FramesPerPacket> decodePcm(const Packet& packet);
std::int64_t sequenceDistance(std::uint32_t a, std::uint32_t b);
} // namespace minemic
