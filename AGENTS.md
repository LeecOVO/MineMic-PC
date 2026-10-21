# MineMic-PC development context

- This is the independent PC repository for MineMic. The sibling `MineMic` project contains the Kotlin Android/iOS app; do not modify it as part of PC-only work unless requested.
- Priorities: Windows first, C++17 + Qt Widgets, low idle/streaming resource use, native desktop interaction, preferably a real single EXE. No JVM or embedded browser runtime. Python is a development/test tool only.
- Runtime Qt dependencies are qtbase Core/Gui/Widgets/Network. Native audio backends: Windows WASAPI (MSVC), macOS Core Audio. Linux audio is pending.
- `docs/PROTOCOL.md` is the mobile/PC wire contract. Update both implementation and contract when changing it; keep the Python test sender interoperable.
- The target microphone route requires an independently installed virtual audio cable: render into its playback endpoint, capture from its paired recording endpoint in the target application. This repository does not contain a custom driver. Never describe speaker playback alone as a working system microphone.
- `MINEMIC_SINGLE_FILE=ON` requires static Qt and static MSVC CRT. A dynamic Qt build or a ZIP is not a single-EXE release. Windows build scripts exist but must be run and validated on Windows before reporting Windows success.
- Preserve fixed buffer limits and bounded network work. Avoid allocations or Qt UI calls in device audio callbacks. The current short mutex is intentional, not a hard real-time guarantee.
- Checks: `cmake --preset core-tests`, `cmake --build --preset core-tests`, `ctest --preset core-tests`. With Qt configured: `cmake --preset desktop -DCMAKE_PREFIX_PATH=...`, build/test the `desktop` preset. The optional Python interoperability test is discovered automatically.
- Read `docs/VALIDATION.md` for actual evidence and `docs/ROADMAP.md` for remaining device acceptance. Do not infer phone, Windows, performance, or driver validation from macOS/loopback tests.
