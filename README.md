# MineMic-PC

MineMic 的独立电脑端：C++17 + Qt 6 Widgets，Windows 优先，手机端在相邻 `MineMic` Kotlin 项目中。

## 当前功能

- PC 显示 **6 位数字配对码**，手机在同一局域网输入配对码连接。
- 可手动输入 PC IPv4（默认 48721），按产品要求不另输配对码。
- 首台成功连接的手机独占 PC；拒绝其他手机，不允许抢占。
- 断网、TCP 中断、心跳超时只标记异常，保留所有权和配对码；原手机凭 token 重连。
- 手机/PC 手动断开时释放连接，立刻更新为不同的新配对码，旧码失效。
- Qt Widgets 中文界面、系统托盘、音量、静音、电平、有界音频缓冲。
- Windows WASAPI 输出；macOS Core Audio 输出；Linux 音频后端待实现。
- TCP 控制 + UDP PCM 音频，DNS-SD/mDNS 自动发现，无云服务器、账户或 JVM。

## 将手机作为系统麦克风

1. PC 先独立安装一套虚拟音频线/虚拟声卡。仓库不打包第三方驱动，也没有 MineMic 自有驱动。
2. MineMic 选择虚拟声卡的**播放端**，例如 `CABLE Input`，点击“开始接收”。
3. 手机使用同一局域网，输入 PC 的 6 位码；也可手动填 PC IPv4。
4. 会议、游戏或录音软件选择配对的**录音端**，例如 `CABLE Output`。
5. PC 默认 TCP/UDP 48721；自动发现还需要 mDNS UDP 5353。程序不会自动修改防火墙。

选择实体扬声器只用于试听，不等于系统虚拟麦克风验收。音频和控制目前未加密，适用于可信局域网。手机进程退出后，PC 不自动释放占用，可在 PC 手动断开。

## 开发与发布

需要 CMake 3.24+、Ninja、C++17、Qt 6.5+ qtbase；本机验证使用 Qt 6.10.3。Windows 当前使用 MSVC 2022。

```sh
cmake --preset desktop -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.3/macos
cmake --build --preset desktop
ctest --preset desktop
```

仅运行无 Qt 的核心测试：使用 `core-tests` 预设。设置 `-DMINEMIC_DEVICE_TESTS=ON` 可增加真实输出设备的静音测试。

Windows 单 EXE 路径、静态 Qt/CRT 及许可说明见 [BUILDING.md](docs/BUILDING.md)。构建选项会拒绝动态 Qt，脚本检查 Qt/编译器运行库导入。**尚未在 Windows 生成和验收单文件 EXE**；单文件也不免除虚拟声卡的系统安装。

测试发送器（在手机完成真实设备验收前验证传输）：

```sh
python3 tools/send_test_audio.py 192.168.1.100 --code 038215 --seconds 10
# 手动 IP：省略 --code
```

## 文档

- [协议与独占规则](docs/PROTOCOL.md)
- [实际验证结果](docs/VALIDATION.md)
- [后续真机验收](docs/ROADMAP.md)

目录：`src/core` 协议与缓冲，`src/network` 控制/音频/发现，`src/audio` 原生后端，`src/ui` 桌面界面，`tests` 回归测试，`tools` 参考发送器，`scripts` 发布构建。

缓冲固定上限，隐藏窗口不刷新电平，设备回调不动态分配；当前使用短时互斥锁，不宣称硬实时。Windows 内存、CPU、体积和端到端延迟尚未实测。项目自身许可证尚未指定，Qt 和第三方驱动分别按其许可分发。
