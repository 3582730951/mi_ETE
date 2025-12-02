# Repository Guidelines

## 项目结构与模块组织
- 仓库根目录为 core，按约定创建子目录：core/server（Windows Server 2008 R2 账号校验与转发面板）、core/client/windows（桌面端 KCP + 白盒 AES）、core/client/android（占位，后续 Jetpack Compose/Gradle）、core/shared（公共类型、KCP/AES 封装）、scripts（CI/本地脚本）、docs 和 assets。目录尚未存在时按此命名新增。
- server 仅负责账号验证、消息路由与面板端口，所有业务数据保持端到端加密；客户端完成聊天、图片/视频处理与撤回，文件保存需乱序加密以避免“文件落地”。

## 构建、测试与开发命令
- 生成构建目录：`cmake -S core/server -B core/build/server -G "Ninja" -DCMAKE_BUILD_TYPE=Release`；Windows 客户端同理：`cmake -S core/client/windows -B core/build/client-win -G "Ninja" -DCMAKE_BUILD_TYPE=Release`。
- 编译：`cmake --build core/build/server --config Release`；`cmake --build core/build/client-win --config Release`。
- 运行示例：`core/build/server/mi_server.exe --config server.yaml`；`core/build/client-win/mi_client.exe --server <ip>:<port>`（将 TCP 封装为 KCP）。
- 测试（存在 CTest 时）：`ctest --test-dir core/build/server --output-on-failure`；Android 端后续使用 `./gradlew test`。
- 自动化测试脚本：`powershell -File core/scripts/run_tests.ps1` 将按已存在的 build-ci 目录批量执行 ctest。

## 代码风格与命名约定
- 遵循 Microsoft Coding Guidelines：4 空格缩进，左大括号换行，类型/类用 PascalCase，函数与变量用 camelCase，常量全大写加下划线。
- 一个函数只做一件事；通用逻辑放入 core/shared；输出日志统一 Unicode，非代码文件 UTF-8；提交前运行 clang-format / eslint / prettier（如适用）。

## 测试规范
- C++ 首选 gtest/catch2，Java 用 JUnit5；测试文件命名 `*_test.(cc|cpp|java)`，用例命名 `Function_ShouldBehavior_WhenCondition`。
- 重点覆盖加密/解密、KCP 握手与重连、文件乱序加密还原、撤回逻辑；目标覆盖率 ≥80%，新增协议字段必须有回归测试。
- 集成测试需验证端到端登录、消息往返、图片/视频收发与恢复，不得打印明文密钥或内容。

## 安全与配置提示
- 配置项集中在 `configs/*.yaml` 或 `.env`，本地开发可使用 `.env.local`，严禁将真实密钥、测试账号提交；白盒 AES 密钥分片/混淆后通过环境变量注入，如 `MI_AES_KEY_PART*`。
- KCP 库与白盒 AES 实现优先使用静态链接，避免运行时缺失；网络参数（MTU/窗口/超时）放在配置文件，便于 CI/生产一致。
- 日志：Release 默认 info，禁止输出明文 payload；崩溃 dump 在收集前先做数据脱敏；所有输出使用 Unicode。
- 性能：优先零拷贝 buffer、对象池，避免不必要的字符串拷贝；数据结构要紧凑，对齐策略与字节序处理在 shared 层统一。
- 依赖管理：CMake 使用 FetchContent 或 vcpkg 锁定版本；前端/Android 依赖使用 lockfile，CI 缓存依赖但每次 major 升级需更新文档。

## 提交与 PR 流程
- 使用 Conventional Commits，例如 `feat(client): add kcp reconnect`、`fix(server): adjust auth timeout`；消息保持英文精简。
- 远程仓库：`https://github.com/3582730951/mi_ETE/`。提交前确保本地构建与测试通过，再 `git push origin main`。
- 推送范围限制：仅同步 core 目录内的内容（server/client/shared 等），勿推送仓库根目录的临时文件或无关项。
- PR 需写明背景、变更、测试结果、影响范围；UI/交互改动附截图或录屏；CI（GitHub Actions）默认全量构建 server/client，可提供参数选择模块。CI 通过后更新版本号/Tag。
- 安全：不得提交明文密钥、证书或测试账号；配置用环境变量或加密文件；日志与 crash dump 不得包含敏感明文。

## 分支与发布
- 推荐使用 feature/*、fix/* 分支，提交后提 PR 合并 main，保持 main 随时可发布且可通过 CI。
- 发布前在 Release 配置下全量构建 server/client，并为版本创建 tag（如 v0.0.1），同步变更日志与版本号。
- GitHub Actions 完成后检查构建工件、日志与覆盖率报告，若有回归及时回滚或补丁修复。

## 待办与未完项
- KCP：已接入 ikcp，仍需完善握手/重连、超时与窗口调优。
- 路由：完善 session→目标客户端的查找与转发策略，未注册/异常时返回明确错误码；增加端到端回显/转发集成测试。
- 客户端：占位收发已可通过配置切换 chat/data/both 模式并支持重试，仍需引入真正的 UI、断线重连与多会话同步。
- 媒体：已支持 MediaChunk/MediaControl 协议与客户端乱序落盘，新增服务端多客户端端到端测试；后续补充大文件、断点续传与性能压测。
- 白盒 AES：已切换为表驱动+随机掩码+外编码的白盒 AES-128 CTR，支持动态 `MixKey`；仍需评估安全性并补充动态密钥协商与更严格测试。
- 面板/认证：面板现返回缓存的会话列表与 KCP CRC/回收统计（可选 token 校验），仍需接入真实账号源、UI/HTTP API 与安全审计。
- CI：完善 GitHub Actions，支持 server/client 可选构建、缓存依赖、覆盖率上报与制品上传；构建通过后自动打 Tag/更新版本号。
- CI：已添加 Windows 矩阵（server/client/all），仍需覆盖率上报与制品上传、按需缓存依赖。
  - 已上传 Windows 构建产物（exe/dll/pdb），覆盖率与缓存待补。
  - 已上传 ctest 日志/报告供快速排查。
  - 增加 CMake/vcpkg 缓存目录，减少重复依赖拉取。
- 文件/聊天全链路：客户端发送侧已支持乱序落盘，新增 ChatHistoryStore 加密记录，路由与撤回协议已接通，仍需与真实 UI/同步逻辑集成。
- TCP 封装：新增 TcpTunnel client/server 与测试覆盖本地 echo 场景，后续需与真实路由集成、补充握手与鉴权。
- Android：仅占位 README，缺少 Jetpack Compose/Gradle 骨架与构建脚本。
- 安全类型落地：共享层新增 `ObfuscatedValue`/服务端会话号混淆，仍需逐步替换剩余原生类型与跨端类型统一。
- 配置：KCP CRC 现可通过配置/env 开启（含最大帧长与日志开关），新增 config 测试覆盖 YAML 解析；证书支持 `cert_allow_self_signed`（或 env `MI_CERT_ALLOW_SELF_SIGNED`）控制自签允许策略。
- CI：新增 `windows-coverage` 任务使用 MinGW+gcovr 产出覆盖率工件，尚未接入报告平台。
- 自动化测试：shared 层新增会话订阅协议用例（session_list_tests），本地可运行 `ctest --test-dir core/build-ci/shared/tests --output-on-failure` 验证。
- 路由：会话订阅广播现包含 KCP 活跃检测（ActiveSessionIds 清理后移除会话并推送），后续可扩展状态持久化与错误码分级。
- 客户端 UI：默认构建 Qt 图形界面（mi_client_qt_ui.dll，mi_client.exe 动态加载；若无 Qt 则跳过），直接调用核心 ClientRunner 逻辑，无需子进程；界面采用双栏深色布局，后续需补充真实聊天/媒体展示与状态管理，订阅会话心跳每 4s 轮询保持在线状态。已消费协议回执/重试事件并在气泡中展示送达/已读/重试状态，表情盘替换为真实 emoji，媒体缩略图可基于 payload 生成预览（Qt6 Multimedia 可提取视频首帧，其他环境退回占位），收藏色支持可视化色块预览/点击应用，未读计数展示在会话列表。
- 客户端 UI 未完项：已接入 KCP 会话订阅（0x07/0x27）、错误分级提示/重试按钮、统计历史图表（HTTP /stats 拉取），支持富文本/表情、文件多选名展示；仍缺会话下线推送的动效、协议级表情/文件持久化、媒体撤回动效与主题/调色跨设备同步。
- 客户端核心未完项：多会话同步/重连仍需增强；媒体/聊天落盘后未提供格式支持列表与混淆校验报告；撤回/控制消息缺少端到端确认/超时重试；Windows 交付形态（单 exe + Qt UI dll）打包/签名已提供 package_win.ps1 + artifacts.zip/certs 打包，Android 占位 apk 已加入 CI 构建与打包，但仍需真实端到端校验和跨平台产物下载入口整合。
- 证书：CI 默认生成自签 CA/服务端/客户端证书并随 artifacts.zip 打包（密码 changeit），现支持从配置/环境以内存加载 PFX 并校验证书链+指纹（可配置允许/拒绝自签）；客户端经 RSA-OAEP 握手下发传输密钥，对 KCP 载荷做内存加密封装（证书不落地，支持指纹/链路校验）；非 Windows 平台在有 OpenSSL 时执行同等链路校验/解密，无 OpenSSL 则以指纹+对称封装降级，仍需补齐 Android/非 Windows 真正链路校验、证书轮换与双向校验。
