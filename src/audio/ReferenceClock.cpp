#include "audio/AudioReference.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#endif
namespace minemic {
std::uint64_t referenceClockMicros() {
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(ticks.QuadPart / frequency.QuadPart) * 1000000 +
           static_cast<std::uint64_t>(ticks.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
}
