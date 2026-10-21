# 验证记录

日期：2026-10-21，Asia/Shanghai。主机为 Apple Silicon macOS，Apple Clang 21、Qt 6.10.3、CMake 4.4.4、Xcode 27。两端协议现为 v2；初版 v1 测试结果不作为当前版本验收。

## 当前开发版本

基础版本 0.0.1，完整版本 0.0.1-ea.1，首个可用 EA 版本，非稳定版。已统一 CMake、界面和 Windows 资源版本来源；macOS Release 编译及现有 5 项 CTest 再次通过。Windows 版本资源已修改，但未在本机执行 MSVC/RC 编译。手机音频修复详情在相邻手机仓库的 docs/AUDIO_PIPELINE.md；用户已确认修复后的真实 iPhone → Windows 卡顿验收通过；量化延迟和全部设备兼容性仍未验收。

## PC 已验证

- Release 编译通过。
- CTest 5/5：核心协议与有界缓冲、真实输出设备静音流、控制/音频回环、Python 互通、Qt 界面启动。
- 网络回归覆盖：6 位码、错误码不占用、首手机锁定、第二手机配对码/IP 均拒绝、首次回复重试、TCP/超时不释放、凭据恢复、UDP 音频重放/乱序、UDP BYE 不释放、手机断开/PC 断开均换码、旧码失效、DNS 压缩指针循环有界。
- 系统 `dns-sd` 原生 Bonjour 浏览发现 PC `_minemic._tcp` 服务成功。
- 单文件配置误用动态 Qt 时按预期报错。Windows 打包尚未执行。
- 窗口离屏渲染检查；截图只在被忽略的构建目录。

## 手机和互通已验证

- Android 调试 APK 构建通过。
- Kotlin iOS Simulator framework 链接通过；完整 Swift + Kotlin iOS Simulator 应用构建通过（未签名）。
- JVM 共 6 项测试通过，无失败/跳过：包括新协议单元测试，以及实际 Kotlin 会话控制器与真实 C++ 服务的配对、双手机竞争、网络故障、恢复、主动断开和旧码失效。
- iOS 链接有依赖库部署版本提示：Compose 的 ICU 数据对象最低 18.5，而原工程目标为 18.2；未据此下调依赖或提高项目目标，旧系统兼容性需后续验证。

## 仍待验收

Windows MSVC/WASAPI、干净系统单 EXE、第三方虚拟声卡路由、真实 Android/iPhone 录音、局域网权限恢复、锁屏/来电、持续运行、端到端延迟、Windows CPU/内存/体积。macOS 静音输出不等于真实手机系统麦克风成功。

`build*`、APK、framework、模拟器应用、日志和本机配置均不纳入 Git。工具链临时目录位于 `/tmp/minemic-pc-*`。没有远端推送；初始源代码以各自本地仓库提交供用户后续推送。
