# MineMic-PC

MineMic 的独立电脑端，采用 C++17 + Qt Widgets，通过局域网接收 [移动端](https://github.com/LeecOVO/MineMic) 的麦克风声音。Windows 优先，支持 macOS 音频输出；Linux 音频后端尚未实现。无需云服务器、账户或 JVM。

已发布版本：**0.0.1-ea.1 · Early Access**。当前开发版本：**0.1.0-dev.1**。项目处于初始开发阶段，版本规则见 [VERSIONING.md](docs/VERSIONING.md)。

## 使用

1. 手机与电脑连接同一局域网。
2. PC 选择音频输出设备，点击“开始接收”。
3. 手机输入 PC 显示的 **6 位配对码**；也可手动填写 PC 的 IPv4 地址，默认端口为 48721。
4. 首台手机独占连接，其他手机无法抢占。网络中断后保留占用，原手机可自动重连。
5. 手机或 PC 主动断开后释放占用，并更新配对码。

作为系统麦克风使用时，需单独安装虚拟声卡：MineMic 输出到其播放端（例如 `CABLE Input`），会议、游戏或录音软件使用对应的录音端（例如 `CABLE Output`）。选择实体扬声器可以试听声音。

Windows 提供单文件版与压缩版。单文件版运行 `MineMic.exe`；压缩版需完整解压并保留依赖文件。两种分发均不包含虚拟声卡驱动。

## 功能与连接

- 输出设备选择、音量、静音、电平及连接统计。
- 系统托盘运行，关闭主窗口后可继续接收。
- Windows WASAPI、macOS Core Audio 原生音频输出。
- 开发版支持按需采集 Windows 外放参考，供手机执行可选回声抑制；可选择实际使用的扬声器，算法在手机运行。
- TCP 控制与 UDP PCM 音频使用 48721 端口，自动发现使用 mDNS UDP 5353。程序不会自动修改防火墙。

传输尚未加密，请在可信局域网使用。手机进程退出后，PC 仍保留占用，可在 PC 手动断开。Linux 音频、不同设备兼容性和开发版回声参考仍需继续测试。

## 构建与测试

需要 CMake 3.24+、Ninja、C++17 和 Qt 6.5+ qtbase。Windows 使用 Visual Studio 2022 C++ x64 工具链。

```sh
cmake --preset desktop -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.3/macos
cmake --build --preset desktop
ctest --preset desktop
```

无需 Qt 的核心测试：

```sh
cmake --preset core-tests
cmake --build --preset core-tests
ctest --preset core-tests
```

设置 `-DMINEMIC_DEVICE_TESTS=ON` 可增加输出设备静音测试。音频测试发送器：

```sh
python3 tools/send_test_audio.py 192.168.1.100 --code 038215 --seconds 10
# 手动 IP：省略 --code
```

Windows 打包、静态 Qt/CRT 和单 EXE 构建见 [BUILDING.md](docs/BUILDING.md)。协议和兼容性见 [PROTOCOL.md](docs/PROTOCOL.md)。

## 项目结构

- `src/core`：协议与有界缓冲。
- `src/network`：会话、传输和发现。
- `src/audio`：原生音频后端。
- `src/ui`：桌面界面。
- `tests`、`tools`、`scripts`：测试、参考发送器和构建工具。

## 许可证

原创代码采用 [MIT License](LICENSE)，版权所有 © 2026 LeeC。Qt 和第三方虚拟声卡分别遵循其许可证，分发要求见 [BUILDING.md](docs/BUILDING.md#qt-许可)。
