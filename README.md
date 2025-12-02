# mi_ete 项目骨架

## 目录规划
- `server/`：mi_server（Windows Server 2008 R2）账号验证、消息转发、面板端口。
- `client/windows/`：mi_client Windows 端，KCP 隧道、白盒 AES、聊天/图片/视频处理与撤回。
- `client/android/`：Android 占位目录，后续 Jetpack Compose/Gradle 实现。
- `shared/`：跨端共享的 KCP 封装、白盒 AES 接口、通用数据结构与工具（含媒体乱序落盘）。
- `scripts/`：CI/本地辅助脚本。
- `configs/`：示例配置（YAML/ENV）。
- `docs/`：文档与设计说明。

## 构建示例
- 服务器：`cmake -S . -B build -G "Ninja" -DBUILD_SERVER=ON -DBUILD_CLIENT_WINDOWS=OFF`，然后 `cmake --build build --config Release`。
- Windows 客户端：`cmake -S . -B build -G "Ninja" -DBUILD_SERVER=OFF -DBUILD_CLIENT_WINDOWS=ON`。
- 测试：`ctest --test-dir build --output-on-failure`。
- CI：`.github/workflows/ci.yml` 在 Windows 跑全量构建与测试，可作为 Tag 前的默认验证。

## 当前消息类型
- `0x01` 认证请求（AuthRequest），响应 `0x11`（AuthResponse，成功返回 sessionId 并登记 Peer）。
- `0x02` 数据包（DataPacket，含 sessionId + targetSessionId + payload），路由将按目标会话转发，回显类型 `0x12`。
- `0x03` 媒体分片（MediaChunk，含 mediaId/文件名/分片信息/payload），转发类型 `0x23`。
- `0x04` 媒体控制（MediaControl，action=1 撤回），转发类型 `0x24`。
- `0x05` 聊天消息（ChatMessage，含 messageId + payload），转发类型 `0x25`。
- `0x06` 聊天控制（ChatControl，action=1 撤回），转发类型 `0x26`。
- `0x13` 错误响应（ErrorResponse，包含 code + message），用于解析失败、未注册会话、目标缺失等场景。
- 服务端命令行：`mi_server.exe --config configs/server.yaml`，`--once` 单次轮询，`--ticks N` 轮询 N 次后退出。
- 客户端占位：`mi_client.exe` 默认经 KCP 认证后发送 Chat/Media；可通过 `--mode chat|data|both`/`MI_MODE` 切换为 DataPacket 回显或双发模式，便于端到端回显校验。
- 白盒 AES 密钥可通过环境变量 `MI_AES_KEY_PART0..N` 注入（十六进制分片），构造 `WhiteboxKeyInfo` 时会自动读取。
- 服务器认证用户：`configs/server.yaml` 可设置 `users: user1:pass1,user2:pass2`，或通过环境变量 `MI_USERS` 传入同格式，留空则默认允许非空账号。

## 客户端参数
- `--server <host:port>` 目标服务器。
- `--user <username>`、`--password <password>`，亦可用环境变量 `MI_USER/MI_PASS`。
- `--message <text>`（默认 `secure_payload`），或环境变量 `MI_MESSAGE`。
- `--target <sessionId>` 指定目标会话（缺省回显自己）。
- `--timeout-ms <ms>` 认证与回显等待超时。
- 媒体发送：`--media-path <file>` 发送图片/视频文件，`--media-chunk <bytes>` 指定分片大小（默认 1200），`--revoke-after` 在成功接收后自动发送撤回指令。
- 发送模式：`--mode chat|data|both` 或环境变量 `MI_MODE`，控制是否发送聊天、数据包或两者；重试参数 `--retries`/`--retry-delay-ms` 控制断开后回连次数。
- 聊天落盘：出/入站消息使用 `ChatHistoryStore` 按 session 动态密钥乱序落盘，撤回会删除本地记录。
- Qt UI（默认）：启用 `-DBUILD_CLIENT_QT=ON`（默认 ON，缺少 Qt 时自动跳过）生成 `mi_client_qt_ui.dll`，`mi_client.exe` 默认加载该 DLL 启动界面（`--cli`/`--no-ui` 可回退 CLI）；界面直接调用内置 `ClientRunner` 逻辑（非子进程），可配置服务器/账号/模式/媒体、启动/停止并查看实时日志；CLI 版本 `mi_client` 仍用于自动化测试。

## 媒体乱序落盘与撤回
- `mi::shared::storage::DisorderedFileStore` 将图片/视频按块乱序+掩码落盘，支持常见扩展名（png/jpg/gif/webp/mp4/mov/mkv/avi/heic 等），默认输出 `.mids` 加密文件。
- 保存时可传入动态密钥（会与根密钥一起派生），读取时校验密钥摘要与数据摘要；`Revoke` 会覆盖并删除本地文件以实现撤回。
- 示例用法：`DisorderedFileStore store(L"cache", rootKey); auto saved = store.Save(L"photo.png", bytes, sessionKey); store.Load(saved.id, sessionKey, restored);`
- 聊天记录：`mi::shared::storage::ChatHistoryStore` 基于乱序落盘保存每条消息，记录 sessionId/时间戳，`Append` 返回 id，`Load` 使用动态密钥校验解密，`Revoke` 可擦除。

## TCP 封装示例
- `mi::shared::net::TcpTunnelClient/Server` 将 TCP 流量封装到 KCP 会话，消息类型使用 0x30/0x31/0x32，便于旁路转发。
- 测试用例 `mi_shared_tcp_tunnel_tests` 会启动本地 TCP Echo、KCP 双端、隧道 client/server，并验证端到端回环。
- 创建隧道：`TcpTunnelServer server(kcpServer, {peerClient, sessionId, L"127.0.0.1", targetPort}); server.Start();`，`TcpTunnelClient client(kcpClient, {L"127.0.0.1", 0, peerServer, sessionId}); client.Start();`。

## 服务端配置补充
- `configs/server.yaml` 新增 KCP 参数：`kcp_mtu`、`kcp_send_window`、`kcp_recv_window`、`kcp_idle_timeout_ms`（会话回收）和 `kcp_peer_rebind_ms`（端点漂移重绑节流）。
- `listen_host/port` 控制 KCP 监听，`panel_host/port` 预留管理面板地址，`poll_sleep_ms` 控制主循环休眠。
- 环境变量可覆盖 KCP 参数：`MI_KCP_MTU`、`MI_KCP_INTERVAL_MS`、`MI_KCP_SEND_WINDOW`、`MI_KCP_RECV_WINDOW`、`MI_KCP_IDLE_TIMEOUT_MS`、`MI_KCP_PEER_REBIND_MS`；CRC 配置 `MI_KCP_CRC_ENABLE`、`MI_KCP_CRC_DROP_LOG`、`MI_KCP_CRC_MAX_FRAME`；账号列表可用 `MI_USERS` 设置（`user:pass,user2:pass2`）。
- 面板：内置轻量 HTTP 监听，访问 `http://<panel_host>:<panel_port>/` 返回 JSON（当前会话数、监听端口、会话列表、KCP CRC 统计/回收计数），用于健康检查/告警集成；如配置 `panel_token` 或环境变量 `MI_PANEL_TOKEN`，需在请求头携带 `x-panel-token: <token>`。
- KcpChannel 可选启用 UDP 帧 CRC32 校验（`enableCrc32`，默认关闭，需双方一致），可调 `maxFrameSize`，Panel JSON 在开启时会标示 CRC 状态和累计计数。

## 白盒 AES
- 采用表驱动白盒 AES-128 CTR：轮级 T-box（含 MixColumns）与随机掩码嵌入，派生掩码由密钥分片驱动；最终回合使用 SBox/ShiftRows/AddRoundKey。
- 密钥来源：`WhiteboxKeyInfo::keyParts` + 环境变量分片 `MI_AES_KEY_PART*`，经扰动派生出会话密钥与 CTR 初始计数器；可用 `MixKey(base, dynamic)` 将会话/媒体动态分量混入，生成一次性密钥。
- 接口：`Encrypt`/`Decrypt`（CTR 对称），可直接用于文本、媒体分片等场景；`MixKey` 用于动态密钥。

## 安全基础类型
- `client/secure_types.hpp` 覆盖 `int8/uint8/int16/uint16/int32/uint32/int64/uint64/short/long/size_t/char/wchar_t/bool/float/double/string` 等跨平台常用类型，运行时以随机排列+掩码存储。
- `shared/secure/obfuscated_value.hpp` 提供通用 `ObfuscatedValue<T>` 与 `ObfuscatedUint32/Uint64`，用于服务端/共享层计数器与会话号的动态掩码存储，测试覆盖 `mi_shared_secure_tests`。

## CI
- GitHub Actions `build-and-test` 跑 Windows 矩阵（server/client/all），产物与 ctest 日志上传；`windows-coverage` 使用 MinGW+gcovr 生成覆盖率（XML/HTML）作为工件。

保持函数单一职责、Unicode 输出，非代码文件 UTF-8 编码。提交遵循 Conventional Commits，CI 默认全量构建。
