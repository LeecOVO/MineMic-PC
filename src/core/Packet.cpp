#include "core/Packet.h"
#include <algorithm>
#include <stdexcept>

namespace minemic {
namespace {
void put16(std::uint8_t* p, std::uint16_t n) { p[0] = n >> 8; p[1] = n & 255; }
void put32(std::uint8_t* p, std::uint32_t n) {
    p[0] = n >> 24; p[1] = (n >> 16) & 255; p[2] = (n >> 8) & 255; p[3] = n & 255;
}
std::uint16_t get16(const std::uint8_t* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t get32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
bool validPayload(PacketType type, std::size_t size) {
    switch (type) {
    case PacketType::Hello: case PacketType::Welcome: return size == 16;
    case PacketType::Audio: return size == MaxPayload;
    case PacketType::Bye: case PacketType::Heartbeat: return size == 0;
    }
    return false;
}
}

std::vector<std::uint8_t> encodePacket(const Packet& packet) {
    if (!validPayload(packet.type, packet.payload.size())) throw std::invalid_argument("Invalid packet payload");
    std::vector<std::uint8_t> out(HeaderSize + packet.payload.size(), 0);
    std::copy_n("MMIC", 4, out.begin());
    out[4] = 2; out[5] = static_cast<std::uint8_t>(packet.type);
    put32(out.data() + 8, packet.session);
    put32(out.data() + 12, packet.sequence);
    put32(out.data() + 16, packet.timestamp);
    if (packet.type == PacketType::Audio) {
        put32(out.data() + 20, SampleRate);
        put16(out.data() + 24, FramesPerPacket);
        out[26] = 1; out[27] = 1; // Mono, signed PCM16 little endian.
    }
    put16(out.data() + 28, static_cast<std::uint16_t>(packet.payload.size()));
    std::copy(packet.payload.begin(), packet.payload.end(), out.begin() + HeaderSize);
    return out;
}

bool decodePacket(const std::uint8_t* b, std::size_t size, Packet& packet) {
    if (!b || size < HeaderSize || size > HeaderSize + MaxPayload) return false;
    if (!std::equal(b, b + 4, "MMIC") || b[4] != 2 || get16(b + 6) || get16(b + 30)) return false;
    const auto type = static_cast<PacketType>(b[5]);
    const auto length = get16(b + 28);
    if (size != HeaderSize + length || !validPayload(type, length)) return false;
    if (type == PacketType::Audio) {
        if (get32(b + 20) != SampleRate || get16(b + 24) != FramesPerPacket || b[26] != 1 || b[27] != 1) return false;
    } else if (get32(b + 20) || get16(b + 24) || b[26] || b[27]) return false;
    packet.type = type;
    packet.session = get32(b + 8); packet.sequence = get32(b + 12); packet.timestamp = get32(b + 16);
    packet.payload.assign(b + HeaderSize, b + size);
    return true;
}

std::array<std::int16_t, FramesPerPacket> decodePcm(const Packet& packet) {
    std::array<std::int16_t, FramesPerPacket> samples{};
    if (packet.type != PacketType::Audio || packet.payload.size() != MaxPayload) return samples;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto value = std::uint16_t(packet.payload[i * 2]) | (std::uint16_t(packet.payload[i * 2 + 1]) << 8);
        samples[i] = static_cast<std::int16_t>(value < 32768 ? int(value) : int(value) - 65536);
    }
    return samples;
}

std::int64_t sequenceDistance(std::uint32_t a, std::uint32_t b) {
    const auto d = a - b;
    return d <= 0x7fffffffU ? std::int64_t(d) : std::int64_t(d) - 0x100000000LL;
}
}
