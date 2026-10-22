# 构建与单文件发布

## 工具链

- CMake 3.24+，Ninja，C++17。
- Qt 6.5+ 的 qtbase（Core / Gui / Widgets / Network）。
- Windows：Visual Studio 2022 C++ x64 开发环境、Windows SDK。当前 WASAPI 后端使用 MSVC/WRL，未支持 MinGW。
- macOS：Xcode Command Line Tools、Qt macOS 包。音频后端使用系统 AudioToolbox/CoreAudio，无需 Qt Multimedia。
- macOS 26/27 等新 SDK 请使用 Qt 6.10.3 或兼容更新版本；旧 Qt 包可能仍引用已移除的 AGL framework。
- Linux：可运行核心/网络测试；桌面界面可以编译，音频输出尚未实现。

## 普通开发构建

```sh
cmake --preset desktop -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.3/macos
cmake --build --preset desktop
ctest --preset desktop
./build/desktop/MineMic.app/Contents/MacOS/MineMic
```

Windows 请在 VS 2022 的 x64 Developer PowerShell 中：

```powershell
./scripts/build-windows.ps1 -QtPrefix C:/Qt/6.10.3/msvc2022_64
```

该模式生成开发用 `dist/portable/` 目录，附带 Qt DLL。目标机器需要官方 MSVC Redistributable。它不叫单文件版。脚本不会覆盖已经存在的产物目录。

GitHub Actions 配置 `.github/workflows/build.yml` 提供 Linux 核心测试与 Windows 开发包构建。

## Windows 真正单 EXE

单文件要求：静态构建 qtbase、静态 CRT、静态平台插件；仅打包或压缩动态 Qt 不能达到这一目标。构建脚本会校验 Qt6::Core 的库类型，拒绝误用动态 Qt。

1. 从 [Qt 官方源代码存档](https://download.qt.io/archive/qt/) 获取并校验 `qtbase-everywhere-src-6.10.3`，解压到例如 `C:/src/qtbase-everywhere-src-6.10.3`。
2. 在 VS 2022 x64 Developer PowerShell 中构建静态 Qt：

   ```powershell
   ./scripts/build-static-qt.ps1 -QtBaseSource C:/src/qtbase-everywhere-src-6.10.3
   ```

   默认采用 Qt 的开源构建配置；持商业授权的开发者可传 `-QtLicense commercial`。脚本只构建 qtbase，不引入 Qt Quick、WebEngine、Multimedia；关闭 OpenGL、ICU、OpenSSL。

3. 用静态 Qt 构建应用：

   ```powershell
   ./scripts/build-windows.ps1 -QtPrefix ./out/qt-static -SingleFile -BuildDirectory ./build/windows-static -OutputDirectory ./dist-static
   ```

4. 成功后产物为 `dist-static/single-file/MineMic.exe`；导入列表保存在外侧的 `dist-static/MineMic-imports.txt`。若仍依赖 Qt 或 MSVC 编译器运行库 DLL，脚本会报错。
5. 在没有 Qt、没有开发工具的干净 Windows 机器上验证启动、声音、托盘、退出。检查导入表只能证明没有直接导入列出的外部运行库，不能替代干净机器运行和系统音频设备验证。

“单文件”指应用发布物，不表示不调用 Windows 自带 DLL，不表示单 EXE 能免安装创建系统音频驱动。第三方虚拟声卡的安装包与授权分开处理，仓库没有将其内嵌或捆绑。

## Qt 许可

Qt 有开源及商业许可选项。静态发布前应按实际选用的许可准备相应声明、Qt 源码及必要的可重链接材料，或使用适用的商业授权。本项目原创代码采用 [MIT License](../LICENSE)；Qt 的许可与分发要求仍独立适用。

参考：[Qt licensing](https://doc.qt.io/qt-6/licensing.html)、[Qt 开源许可 FAQ](https://www.qt.io/faq/qt-open-source-licensing)。
