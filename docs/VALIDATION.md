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

## 2026-10-22：0.1.0-dev.1 开发增量

已发布基线仍为 0.0.1-ea.1。本轮新增可选 Windows PCM 立体声参考与时钟/能力协商，手机执行 WebRTC AEC/NS。外放参考使用独立有界队列和工作线程，不在网络线程等待 COM 设备初始化/关闭；关闭/断网停止参考，继续保留原有独占规则。UI 新增可在接收中更改的实际扬声器选择，版本标为开发预览。

macOS Qt Release 编译通过；core-tests preset 1/1、desktop CTest 5/5（核心、网络/参考、Python 上传、Python 参考、UI）通过。参考测试使用可控源，验证了真实 TCP/UDP/HMAC/双声道拆包、仅所有者请求、关闭停止及断网保留占用；**不覆盖 Windows 实际 WASAPI 后端**。Android 四 ABI APK、iOS Simulator/iphoneos 无签名完整构建及手机 JVM 14 项互通/回退测试见相邻 MineMic/docs/AUDIO_ENHANCEMENT_VALIDATION.md。

Windows MSVC/硬件初始化、音频格式转换/设备时间戳、游戏帧时间、iPhone 长时锁屏和连续异步漂移补偿尚未验收。保持 dev 状态，尚未创建 Windows 分发包或推送远端。
