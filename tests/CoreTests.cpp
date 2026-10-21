#include "core/AudioBuffer.h"
#include "core/Packet.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void check(bool pass, const char* message) {
    if (!pass) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::array<std::int16_t, minemic::FramesPerPacket> tone(std::int16_t value) {
    std::array<std::int16_t, minemic::FramesPerPacket> result; result.fill(value); return result;
}
}
int main() {
    using namespace minemic;
    Packet audio; audio.type = PacketType::Audio; audio.session = 0x12345678;
    audio.sequence = 0xfedcba98; audio.timestamp = 480; audio.payload.resize(MaxPayload, 0);
    audio.payload[0] = 0; audio.payload[1] = 128; // -32768
    audio.payload[2] = 255; audio.payload[3] = 127; // 32767
    auto bytes = encodePacket(audio);
    Packet decoded;
    check(decodePacket(bytes.data(), bytes.size(), decoded), "valid audio round trip");
    check(decoded.session == audio.session && decoded.sequence == audio.sequence, "network endian fields");
    auto pcm = decodePcm(decoded);
    check(pcm[0] == -32768 && pcm[1] == 32767, "signed little endian PCM");
    check(!decodePacket(bytes.data(), bytes.size() - 1, decoded), "truncated payload rejected");
    bytes.push_back(0); check(!decodePacket(bytes.data(), bytes.size(), decoded), "trailing bytes rejected"); bytes.pop_back();
    bytes[27] = 2; check(!decodePacket(bytes.data(), bytes.size(), decoded), "unsupported codec rejected"); bytes[27] = 1;
    bytes[6] = 1; check(!decodePacket(bytes.data(), bytes.size(), decoded), "unknown flags rejected"); bytes[6] = 0;
    bytes[5] = 99; check(!decodePacket(bytes.data(), bytes.size(), decoded), "unknown packet type rejected");
    check(!decodePacket(nullptr, 1000, decoded), "null input rejected");
    for (std::size_t length = 0; length < HeaderSize; ++length)
        check(!decodePacket(bytes.data(), length, decoded), "short headers rejected");

    AudioBuffer buffer;
    std::array<std::int16_t, FramesPerPacket> out;
    buffer.push(100, tone(100)); buffer.read(out.data(), out.size());
    check(out[0] == 0, "pre-roll produces silence");
    buffer.push(102, tone(102)); buffer.push(101, tone(101)); buffer.push(103, tone(103));
    check(!buffer.push(103, tone(999)), "duplicate rejected");
    for (int expected = 100; expected <= 103; ++expected) {
        buffer.read(out.data(), out.size()); check(out[0] == expected && out.back() == expected, "reorder preserves samples");
    }
    check(!buffer.push(100, tone(100)), "late packet rejected");
    buffer.read(out.data(), out.size()); check(out[0] == 0, "underflow produces silence");
    for (unsigned i = 200; i < 204; ++i) buffer.push(i, tone(321));
    buffer.read(out.data(), out.size()); check(out[0] == 321, "starved stream re-primes");

    buffer.reset();
    for (unsigned i : {0U, 1U, 3U, 4U}) buffer.push(i, tone(500));
    buffer.read(out.data(), out.size()); buffer.read(out.data(), out.size()); buffer.read(out.data(), out.size());
    check(out[0] == 0 && buffer.stats().missing == 1, "gap becomes exactly one silent packet");
    buffer.read(out.data(), out.size()); check(out[0] == 500, "audio resumes after gap");

    buffer.reset();
    const auto last = std::numeric_limits<std::uint32_t>::max();
    for (unsigned i = 0; i < 8; ++i) buffer.push(last - 3 + i, tone(static_cast<std::int16_t>(i + 1)));
    for (int expected = 1; expected <= 8; ++expected) {
        buffer.read(out.data(), out.size()); check(out[0] == expected, "sequence wraps without slot collision");
    }
    check(sequenceDistance(0, last) == 1 && sequenceDistance(last, 0) == -1, "modular sequence comparison");

    buffer.reset();
    for (unsigned i = 0; i < 100000; ++i) buffer.push(i, tone(20000));
    check(buffer.stats().queuedFrames <= 16 * FramesPerPacket, "buffer remains bounded under long overload");
    buffer.reset();
    for (unsigned i = 0; i < 4; ++i) buffer.push(i, tone(20000));
    buffer.setGain(2.0f); buffer.read(out.data(), out.size()); check(out[0] == 32767, "gain clips safely");
    buffer.setMuted(true); buffer.read(out.data(), out.size()); check(out[0] == 0, "mute consumes but silences audio");
    buffer.setMuted(false); buffer.setGain(1.0f);
    std::int16_t partial[137]; buffer.read(partial, 137); check(partial[0] == 20000, "partial device reads supported");
    check(buffer.takePeak() == 32767, "peak is bounded and retained");
    std::cout << "Core protocol and audio buffer tests passed\n";
}
