# MineMic 版本约定

遵循 [SemVer 2.0.0](https://semver.org/lang/zh-CN/)。当前基础版本是 **0.0.1**，完整版本是 **0.0.1-ea.1**，阶段为 **Early Access · 首个可用版本**。用户已确认卡顿修复验收，本版作为首个可用版本；后台、量化性能及全面兼容性仍需后续验收。

- `0.y.z`：初始开发阶段，公共接口仍可能变化，不承诺稳定。
- `-dev.N` 表示开发快照，`-ea.N` 表示可用的 EA 预发布；当前使用 `-ea.1`。对外分发有内容变化的新开发包时递增 `N`，不覆盖同一已发布版本。
- 构建号：Android `versionCode`、iOS `CFBundleVersion` 和 PC 文件资源中的第四段用于识别构建；它与 SemVer 基础版本分开管理。重新编译本身不代表应当升级主/次/修订号。
- EA 是访问/测试阶段说明，不是 SemVer 规定的固定等级；是否进入后续测试或正式阶段取决于验收，不因编译成功、打包成功或安装成功自动晋级。
- 稳定公共接口形成后才考虑 `1.0.0`。稳定期遵循不兼容公共接口修改升主版本、兼容新增升次版本、兼容修复升修订号；当前 `0.x` 阶段仍须记录兼容性变化。
- 公共兼容面包括手机与 PC 的配对/独占/恢复/断开行为、TCP 控制消息、UDP 音频格式及配置兼容性。协议版本 `v2` 与应用版本是两套编号，不随应用每次构建自动变化。

手机端版本唯一来源是根目录 `version.xcconfig`，供 Xcode 直接包含，Gradle 同时读取以生成 Android 版本和共享界面的版本常量。iOS 的系统展示字段只支持三段数字，所以 `CFBundleShortVersionString` 为 `0.0.1`；完整 `0.0.1-ea.1` 保存在 `MineMicVersion` 并显示在应用中。参见 [Apple 字段要求](https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundleshortversionstring)。

PC 的版本唯一来源是它自己仓库的 `CMakeLists.txt`，生成应用版本头文件和 Windows 版本资源。两个仓库独立管理；在同一轮联调分发中应核对基础版本和协议兼容性。
