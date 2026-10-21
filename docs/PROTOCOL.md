# MineMic 局域网协议 v2

v2 替代初版 v1。控制使用 TCP，音频使用 UDP，默认均为 IPv4 48721。双方控制消息采用 UTF-8 的单行 JSON，所有字段的值均为字符串。每条不超过 1024 字节，后接 `\n`。每条必须包含 `app=MineMic`、`v=2`。

## 发现和连接

PC 通过 DNS-SD/mDNS 广告 `_minemic._tcp.local.`，TXT 为 `v=2`，SRV 指向控制端口，A 为可用 IPv4。Android 使用系统 NSD，iOS 使用 Bonjour 指定服务浏览，因此手机不自行广播 UDP，也不扫描整个子网。自动发现受路由器客户端隔离、组播限制和系统局域网权限影响；失败时可手动输入 PC 地址。

手机配对码模式：发现候选 PC 后依次连接其 TCP 控制端口，发送：

```json
{"app":"MineMic","v":"2","type":"connect","mode":"code","code":"038215","clientId":"32位随机十六进制","nonce":"32位随机十六进制","name":"手机名称"}
```

手动模式：连接用户输入的 PC IPv4、默认端口 48721，同样发送 `connect`，但 `mode=manual`、`code=""`。**按当前产品要求，手动 IP 不需要配对码**。

PC 在网络线程内按顺序原子处理请求。首个有效连接立即独占，返回：

```json
{"app":"MineMic","v":"2","type":"welcome","nonce":"原请求nonce","session":"非零uint32十进制","key":"16字节音频HMAC密钥的32位hex","token":"16字节重连凭据的32位hex","pcName":"电脑名称","port":"48721"}
```

手机检查 nonce、版本、session 范围、key/token 长度。相同 clientId、nonce、来源 IP 的连接重试会获得同一会话，处理首次回复丢失。其他客户端返回 `type=error, reason=busy`。错误码不占用 PC；错误配对码返回 `wrong_code`；连接请求全局限速 12 次/秒；最多 8 个控制 socket，非当前所有者的 socket 5 秒后关闭。

## 独占、重连和手动断开

- 首次连接后，直到用户手动断开，PC 不接受其他手机。超时、TCP 中断、音频缺包、麦克风失败均不释放所有权，也不换码。
- 手机每秒发送 `type=ping`，PC 返回 `type=pong`。3 秒没有活跃数据时 PC 显示异常并清空旧音频，但仍锁定原手机。
- 原手机可新建 TCP socket，发送 `type=resume, clientId, token, nonce`。PC 校验凭据后返回新的 welcome；token 保持不变，音频 session/key 更新，旧音频失效。
- 手机主动断开使用 TCP `type=disconnect, clientId, token`。PC 返回 `type=disconnected` 并释放所有权，立即生成与上一组不同的 6 位数字码（保留前导零）。
- PC 的“断开当前手机”“停止接收”或主动退出同样释放并换码；在线手机收到 `type=disconnected`。
- UDP BYE、其他手机的断开请求、错误 token 均不能释放锁。
- 手机没有收到断开确认时保留凭据并提示重试，或让用户在 PC 上断开。PC 已释放旧连接时返回 `not_owner`，手机可安全结束旧会话。
- 当前凭据在手机应用进程内保留（Android ViewModel 覆盖配置变化），未持久化到磁盘。手机进程被系统结束后，PC 仍保留占用，用户需要在 PC 上手动断开。PC 进程重启会生成新的配对码，不跨重启保存锁。

## 音频数据报

每个 UDP 包：**32 字节头 + 960 字节 PCM + 32 字节 HMAC-SHA256**，总计 1024 字节。HMAC 使用 welcome 中的随机 key，对 `header || payload` 计算，附原始 32 字节。不是用 6 位码直接计算音频 HMAC。

| 偏移 | 字节 | 字段 |
|---|---|---|
| 0 | 4 | ASCII `MMIC` |
| 4 | 1 | version=2 |
| 5 | 1 | AUDIO=3（其他控制操作走 TCP） |
| 6 | 2 | flags=0 |
| 8 | 4 | session：当前 welcome 的非零 uint32 |
| 12 | 4 | sequence：每个采集帧递增，uint32 回绕 |
| 16 | 4 | timestamp：48 kHz 采样帧时钟，uint32 回绕 |
| 20 | 4 | sampleRate=48000 |
| 24 | 2 | frameCount=480 |
| 26 | 1 | channels=1 |
| 27 | 1 | format=1（有符号 PCM16LE） |
| 28 | 2 | payloadBytes=960 |
| 30 | 2 | reserved=0 |
| 32 | 960 | 单声道 PCM16 小端数据 |
| 992 | 32 | HMAC-SHA256 tag |

头部多字节数字均为大端。每包 10 ms，每秒 100 包。手机按采集时钟编号；丢弃积压采集帧后序号继续前进，不补发陈旧音频。静音仍发送同格式零样本。

PC 校验 TCP 所有者 IP、音频 session、HMAC 和来源 UDP 端口。会话第一份合法音频绑定 UDP 端口；重连时重置绑定。保留 64 包重放窗口，时间戳差必须等于序号差乘 480。播放乱序窗口为 16 包，40 ms 预缓冲，最大 160 ms，缺包补静音、溢出丢旧数据。

## 使用范围

这是可信局域网协议。TCP 配对与凭据、UDP 音频目前均未加密。6 位码是发现后选择 PC 的方式，手动 IP 也允许首次连接，因此不提供不可信网络下的身份认证保证。后续加密需要独立设计和审查。

参考实现：手机 `connection/Wire.kt`、`MicController.kt`；PC `Receiver.*`；Python `tools/send_test_audio.py`。修改契约时应同步三者并运行互通测试。
