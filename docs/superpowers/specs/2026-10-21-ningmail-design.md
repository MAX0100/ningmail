# NingMail 设计规格

| 项目 | 内容 |
|---|---|
| 产品名 | NingMail |
| 仓库 | <https://github.com/MAX0100/ningmail> |
| 文档状态 | 待审阅 |
| 日期 | 2026-10-21 |
| 目标平台 | 银河麒麟桌面 V10 SP1 2303/2403、V10 SP3；ARM64（飞腾/鲲鹏）、x86_64（兆芯/海光） |
| 技术栈 | Qt 6.8 LTS + QML + C++20 |
| 应用许可证 | MIT；第三方依赖遵循各自许可证 |

---

## 1. 背景与目标

NingMail 是面向信创麒麟桌面的原生邮件客户端。目标不是做一个“能收发”的最小工具，而是在国产 CPU、集成显卡和 8GB 内存的整机上，提供接近现代商业邮件客户端的外观、响应速度和日常可用性。

### 1.1 目标

- 在麒麟 V10 桌面、ARM64 与 x86_64 上稳定运行。
- 三栏布局、会话聚合、离线可读、全文搜索、附件、草稿/发件箱、日历、通讯录。
- 中文输入法、系统托盘、通知、`mailto:`、高分屏缩放正常。
- 不依赖系统 WebKit，不引入 Chromium 运行时。
- 邮件 HTML 默认不执行 JavaScript，默认屏蔽远程内容。
- 万级邮箱下保持可用的启动、滚动和搜索性能。

### 1.2 成功标准

| 指标 | 目标 |
|---|---|
| 冷启动（FT-2000/4，8GB，SSD） | < 2.5s |
| 热启动 | < 1.5s |
| 首屏邮件列表 | < 300ms |
| 列表滚动 | 60fps 目标，低端 GPU/软件渲染 ≥ 30fps |
| 搜索 1 万封 | < 300ms |
| 搜索 10 万封 | < 800ms |
| 空闲内存 | < 200MB |
| 阅读 HTML 邮件时内存 | < 300MB |
| 安装包 | 100–130MB（含 Qt、ICU、OpenSSL） |
| 首次同步 1 万封头部 | < 5 分钟（正常网络） |
| 连续使用 30 天 | 无邮件丢失、无数据库损坏 |

### 1.3 非目标

- 不做移动端、Web 端、服务器端。
- 不做插件系统。
- V1.0 不支持龙芯 LoongArch（目标架构只有 ARM64 与 x86_64）。
- V1.0 不做 Exchange/EWS、OAuth2、S/MIME、国密、iMIP 会议邀请、邮件规则/过滤器；这些进入 V1.1。
- V1.0 不以 Wayland 为第一目标；X11 为主，Wayland 做尽力兼容。
- 不做遥测；崩溃报告默认关闭，需用户显式开启。

---

## 2. 范围

### 2.1 V1.0 范围

- 多账号：IMAP、SMTP、POP3（POP3 仅基础收取，无 IDLE）。
- 三栏主界面、统一收件箱、会话聚合、未读/旗标/文件夹管理。
- 本地 SQLite 缓存、离线阅读、FTS5 全文搜索。
- HTML 邮件阅读（litehtml）、纯文本邮件、附件收发、CID 内嵌图片（阅读）。
- 写信：基础富文本、附件、签名、自动保存草稿、持久化发件箱、发送撤销。
- 通知、托盘、`mailto:`、单实例。
- 日历与通讯录：CalDAV/CardDAV 同步，月/周/日视图，事件编辑，联系人自动补全。
- 亮色/暗色主题、125%/150%/200% 缩放、fcitx5/ibus/搜狗输入法。
- deb 打包，ARM64 与 x86_64 双架构。

### 2.2 V1.1 范围

- Exchange：on-prem EWS（SOAP）与 Exchange Online（Microsoft Graph）适配。
- OAuth2：Gmail、Outlook/Exchange Online。
- iMIP 会议邀请与回复。
- S/MIME 签名/加密；国密 SM2/SM3/SM4 扩展。
- 邮件规则/过滤器、快捷回复模板。
- 写信内嵌图片、拼写检查、读屏（Orca）改进。
- 可选的渲染进程沙箱。

### 2.3 范围决策记录

- **Exchange 放在 V1.1。** EWS/Graph 的认证、日历、联系人、附件和错误模型都与 IMAP 差异很大，放进 V1.0 会让首个可交付版本增加约 6–8 周。V1.0 先通过标准 IMAP/SMTP + CalDAV/CardDAV 覆盖大多数国产邮箱和自建邮件系统。
- **OAuth2 放在 V1.1。** V1.0 支持密码/授权码登录，覆盖 QQ、163、126、腾讯企业邮、阿里企业邮、Coremail 等国内常见方式；Gmail/Outlook 的 OAuth2 在 V1.1 补齐。
- **日历/通讯录留在 V1.0。** 使用 CalDAV/CardDAV，不依赖 Exchange。
- **渲染沙箱放在 V1.1。** V1.0 单进程多线程，但渲染接口按可拆进程设计。

---

## 3. 技术选型

### 3.1 结论

- UI：**Qt 6.8 LTS + QML（Qt Quick / Qt Quick Controls 2 自绘设计系统）**。
- 核心：**C++20**，CMake + Ninja。
- 邮件协议：**libetpan**（IMAP/SMTP/POP3/MIME）。
- HTML 渲染：**litehtml + Gumbo**，不执行 JavaScript。
- 存储：**SQLite + FTS5 + ICU**。
- 日历：**libical + CalDAV**；通讯录：**CardDAV + 自研 vCard 子集解析器**。
- 加密/TLS：**OpenSSL 3**。
- 凭据：**libsecret**（Secret Service）；不可用时仅保留会话内密码，不落盘。
- 打包：**deb**，自带 Qt 运行时与必要第三方库。

### 3.2 备选方案对比

| 方案 | 视觉/动效 | 资源占用 | HTML 邮件 | 麒麟集成 | 开发效率 | 结论 |
|---|---|---|---|---|---|---|
| Qt 6 + QML + C++ | 高 | 最好，100–200MB | 中，litehtml 兜底 | 最好，UKUI 同为 Qt | 中 | **采用** |
| Electron + React | 最高 | 差，300–500MB | 最好 | 中，需锁定版本/GPU 参数 | 最高 | 不采用 |
| Flutter | 高 | 中，150–250MB | 差，需 WebView 或低还原度方案 | 中，输入法/托盘/通知弱 | 中 | 不采用 |
| Avalonia + .NET | 中高 | 中 | 差，缺成熟 HTML 引擎 | 中 | 中高 | 不采用 |
| Qt + QtWebEngine | 最高 | 中高，+100MB 以上 | 最好 | 中，ARM64/GPU 风险 | 中 | 不采用 |
| Tauri 2 / Wails / Neutralino | 高 | 小 | 依赖系统 WebView | 差，WebKitGTK 版本不达标 | 高 | 不采用 |

### 3.3 关键理由

- 麒麟桌面 UKUI 本身基于 Qt，输入法、字体、DPI、托盘、通知的兼容路径最短。
- QML 适合做三栏布局、动效、HiDPI 和自绘控件；性能是原生绘制，不依赖浏览器。
- 不依赖系统 WebKitGTK，绕开 Tauri 2 在麒麟上的核心障碍。
- 不引入 Chromium，安装包、内存和低端 GPU 兼容性可控。
- ARM64 与 x86_64 共用一套代码，交叉编译和 CI 成熟。

### 3.4 不采用 Qt Widgets 作为主框架

Qt Widgets 在复杂表格和传统桌面表单上更成熟，但现代视觉、动效和高分屏适配成本更高。NingMail 的主界面、列表、阅读窗和设置全部用 QML；写信使用 QML `TextArea` + `QTextDocument` 扩展，不把 QWidget 作为界面组件（托盘回退可能链接 QtWidgets，见 8.1）

### 3.5 许可证

- 应用代码：MIT。
- Qt 6.8：LGPLv3，动态链接；随包提供许可证、版权声明和可重链接目标文件。若未来改为静态链接，需购买 Qt 商业许可。
- libetpan：BSD-3-Clause。
- litehtml：BSD-3-Clause。
- Gumbo：Apache-2.0。
- libical：MPL-2.0 / LGPL-2.1。
- OpenSSL：Apache-2.0。
- SQLite：Public Domain。
- ICU：Unicode License。
- libsecret：LGPL-2.1。

---

## 4. 总体架构

### 4.1 运行模型

**单进程、多线程，渲染接口预留进程拆分能力。**

```text
┌────────────────────────────────────────────────────────────┐
│ QML UI                                                     │
│ 三栏布局 / 会话列表 / 阅读器 / 写信 / 日历 / 通讯录 / 设置   │
│ 设计系统、动效、HiDPI、输入法、可访问性                      │
├────────────────────────────────────────────────────────────┤
│ app/core                                                   │
│ AccountManager / SyncScheduler / Outbox / Store / Search    │
│ NotificationService / Settings / SingleInstance             │
├──────────────────┬──────────────────┬──────────────────────┤
│ libs/mail        │ libs/render      │ libs/calendar        │
│ libetpan         │ litehtml         │ libical              │
│ MIME / SMTP      │ 清洗 / 分块渲染   │ CalDAV / CardDAV     │
│ 字符集 / 解析     │ 文本选择 / 链接   │ vCard                │
├──────────────────┴──────────────────┴──────────────────────┤
│ libs/exchange（V1.1）  EWS / Graph                          │
├────────────────────────────────────────────────────────────┤
│ platform/kylin  keyring / 托盘 SNI / 通知 / mailto / 自启动  │
├────────────────────────────────────────────────────────────┤
│ Qt 6.8 / OpenSSL 3 / SQLite(FTS5,ICU) / ICU / libsecret     │
└────────────────────────────────────────────────────────────┘
```

### 4.2 模块职责

| 模块 | 职责 | 不做什么 |
|---|---|---|
| `app/ui` | QML 界面、ViewModel、命令分发、主题 | 不直接访问网络、不写数据库 |
| `app/core` | 账号、同步调度、发件箱、存储门面、搜索、通知 | 不依赖 QML |
| `libs/mail` | IMAP/SMTP/POP3、MIME、字符集、RFC 2047、附件元数据 | 不依赖 UI |
| `libs/render` | HTML 清洗、litehtml 布局/绘制、分块、文本 run、链接区域 | 不依赖同步逻辑 |
| `libs/calendar` | iCalendar、CalDAV、CardDAV、vCard | 不依赖 UI |
| `libs/exchange` | EWS/Graph 适配（V1.1），实现统一后端接口 | 不影响 V1.0 |
| `platform/kylin` | keyring、托盘、通知、`mailto:`、单实例、自启动 | 不含业务逻辑 |

依赖方向：`app/ui → app/core → libs/*`，`platform/kylin` 只被 `app/core` 调用。

### 4.3 线程模型

| 线程/池 | 数量 | 职责 |
|---|---|---|
| GUI 线程 | 1 | QML 渲染、ViewModel、轻量 UI 查询 |
| 账号命令线程 | 每账号 1 | 文件夹列表、批量头部同步、标志同步、正文/附件拉取 |
| IDLE 线程 | 每账号 1 | INBOX IDLE，收到变更后通知命令线程 |
| MIME/解析池 | 2–4 | MIME 解析、字符集解码、HTML 清洗、会话聚合 |
| 存储写线程 | 1 | SQLite 单写者、事务、FTS 更新 |
| 搜索线程 | 1 | FTS5 查询、结果分页、片段高亮 |
| 渲染池 | 2 | litehtml 布局与分块绘制 |
| 发件线程 | 1 | SMTP 发送、Sent 追加、重试 |
| 日历/通讯录同步 | 1–2（共享） | CalDAV/CardDAV 增量同步 |

- 账号默认上限 5 个，可配置到 10 个；线程按需创建、空闲回收。默认 5 个账号时活跃线程通常少于 25，10 个账号的极端情况不超过 40。
- 所有网络和解析操作不得在 GUI 线程执行。
- SQLite 使用 WAL；GUI 线程只读连接，写操作全部投递到存储写线程。
- 渲染文档对象只属于一个渲染任务，不跨线程共享；分块结果通过队列回到 UI。

### 4.4 数据流

**同步：**

```text
IDLE/定时触发
  → 账号命令线程拉取 UID/FLAGS/ENVELOPE
  → MIME 解析池解码头部、计算 thread_id
  → 存储写线程事务写入 messages + search_docs + FTS
  → Store 发出 messagesChanged(folderId)
  → UI 列表模型增量刷新
```

**阅读：**

```text
用户选中邮件
  → UI 查询本地 header/body 状态
  → 正文缺失：命令线程拉取 BODY[]
  → 解析/清洗
  → RenderService 布局
  → 按滚动请求 tile
  → QML 显示 tile + 文本 run + 链接区域
```

**搜索：**

```text
Ctrl+K 输入
  → 搜索线程 FTS5 查询
  → 返回 message_pk 列表 + 高亮片段
  → 结果模型分页加载
```

**发送：**

```text
写信窗口
  → 本地草稿/发件箱事务
  → 5 秒撤销窗口
  → SMTP 线程发送
  → 成功：写 Sent；失败：指数退避重试
```

### 4.5 错误处理

统一错误类型：

| 类型 | 示例 | UI 行为 |
|---|---|---|
| `AuthError` | 密码错误、授权码失效 | 暂停账号，提示重新登录 |
| `NetworkError` | DNS、连接超时、断网 | 自动重试，显示离线状态 |
| `TlsError` | 证书过期、主机名不匹配 | 显示证书详情，不提供“忽略”按钮 |
| `ProtocolError` | 服务器返回异常、UIDVALIDITY 变化 | 重建该文件夹缓存，记录日志 |
| `StorageError` | 磁盘满、数据库损坏 | 停写，提示导出诊断包 |
| `QuotaError` | 邮箱已满、附件超限 | 提示清理或减小附件 |

- 每个文件夹独立同步状态；单文件夹失败不阻塞其他文件夹。
- 同步失败指数退避：30s、2min、10min、30min，上限 30min；用户手动刷新立即重试。
- 发件箱持久化；重启后恢复未完成发送，使用 Message-ID 去重，避免重复发送。
- 日志默认脱敏：不记录密码、OAuth token、完整正文；诊断包需用户确认后导出。

### 4.6 可演进性

- `libs/render` 通过 `RenderService` 接口暴露：`render(messageId, width, scale)`、`tile(handle, index)`、`cancel(handle)`。
- V1.0 是进程内线程实现；V1.1 可替换为独立进程 + 共享内存/IPC，UI 层不改。
- `libs/mail` 通过 `MailBackend` 接口暴露；IMAP/SMTP 与未来的 EWS/Graph 实现同一接口。

---

## 5. 数据与同步

### 5.1 数据模型

SQLite 单库，按 `account_id` 隔离；不使用每账号独立数据库，保证统一收件箱和全局搜索。

| 表 | 关键字段 | 说明 |
|---|---|---|
| `accounts` | `id, display_name, email, imap_*, smtp_*, auth_type, username, credential_ref, signature_html, sort_order` | 账号配置；密码/token 不落库 |
| `folders` | `id, account_id, full_name, role, delimiter, uidvalidity, uidnext, highestmodseq, last_sync_at, sync_state, unread_count, total_count` | IMAP 文件夹与同步游标 |
| `messages` | `id, account_id, folder_id, uid, rfc_message_id, thread_id, subject, subject_norm, from_name, from_addr, to_addrs, cc_addrs, bcc_addrs, reply_to, sent_at, received_at, size, flags, keywords_json, has_attachments, snippet, body_state` | 头部与状态；`body_state` 为 `none/fetching/ready/error` |
| `bodies` | `message_pk, content_type, charset, html_raw, text_plain, fetched_at, size` | 正文，按需加载；渲染时清洗，可缓存清洗结果及清洗器版本 |
| `attachments` | `id, message_pk, filename, mime_type, size, content_id, is_inline, storage_key, sha256, state` | 附件元数据；文件在内容寻址目录 |
| `threads` | `id, account_id, subject_norm, last_at, message_count, unread_count` | 会话聚合 |
| `search_docs` | `message_pk, subject, from_text, to_text, body_text, attachment_names` | FTS5 外部内容表 |
| `contacts` | `id, account_id, display_name, email, org, title, phones_json, note, avatar_path, uid, rev` | CardDAV 联系人 |
| `calendars` | `id, account_id, name, url, color, ctag, sync_token` | CalDAV 日历 |
| `events` | `id, account_id, calendar_id, uid, summary, description, location, start_at, end_at, all_day, rrule, attendees_json, alarm_json, etag, rev` | 事件 |
| `drafts` | `id, account_id, subject, body_html, body_text, recipients_json, attachments_json, updated_at` | 本地草稿 |
| `outbox` | `id, account_id, draft_id, recipients_json, subject, body_html, body_text, attachments_json, state, attempts, next_retry_at, last_error, rfc_message_id, created_at` | 持久化发件箱 |
| `sync_state` | `account_id, scope, key, value` | 通用同步游标 |
| `schema_version` | `version` | 迁移版本 |

**索引：**

- `messages(folder_id, sent_at DESC, id DESC)`：列表键集分页。
- `messages(account_id, thread_id, sent_at)`：会话视图。
- `messages(folder_id, flags)`：未读/旗标过滤。
- `messages(rfc_message_id)`：去重与引用。
- `messages(account_id, uid, folder_id)` 唯一：IMAP UID 定位。
- `attachments(message_pk)`、`events(calendar_id, start_at)`。

**存储约定：**

- 时间统一存 UTC 毫秒；显示时用 `QLocale` 和系统时区。
- 标志用位掩码：`\Seen`、`\Flagged`、`\Answered`、`\Draft`、`\Deleted`；自定义关键字存 JSON。
- 正文压缩后存 `bodies`（zlib），附件存内容寻址文件（`sha256` 前两位分目录）。
- 原始 MIME 默认不长期保存；需要“重新解析”时从服务器重新拉取。可选的 `raw` 缓存上限 200MB，按 LRU 淘汰。

### 5.2 中文全文搜索

- FTS5 使用 **ICU tokenizer**（`SQLITE_ENABLE_ICU`），对中文分词、英文按词切分；不使用默认 `unicode61` 作为主方案。
- `search_docs` 为普通表，`messages_fts` 为 external-content FTS5 表；`messages`/`bodies` 变更时在同一事务内更新。
- 索引字段：主题、发件人、收件人、正文纯文本、附件文件名。
- 查询支持：短语、前缀、账号/文件夹/日期/未读/附件过滤；排序默认 `bm25`，可按时间切换。
- 高亮片段由 `snippet()` 生成，UI 只渲染纯文本，不注入 HTML。
- 首次同步时后台建索引；索引未完成时，搜索回退到 `LIKE` 仅搜主题/发件人，并提示“正在建立索引”。
- 附件内容不做全文索引（V1.0）；只索引文件名。

### 5.3 IMAP 同步

- **首次同步只拉头部**：`ENVELOPE`、`FLAGS`、`INTERNALDATE`、`RFC822.SIZE`、`BODYSTRUCTURE`。
- **首屏优先**：每个文件夹先拉最近 500 封，UI 可用；其余在后台按 UID 从新到旧分批回填，每批 200–500 封。
- **增量同步**：优先 `CONDSTORE/QRESYNC`；不支持时用 `UIDNEXT` + 标志位比对；INBOX 使用 IDLE。
- **连接策略**：每个账号最多 2 条连接（1 条 IDLE + 1 条命令），避免触发服务器连接数限制；服务器限制更严时降为 1 条，IDLE 暂停。
- **文件夹**：`LIST` 获取；识别 `INBOX`、`Sent`、`Drafts`、`Trash`、`Junk`、`Archive` 角色；Gmail 标签按文件夹处理。
- **标志**：本地先行更新，失败回滚并提示；批量已读/旗标使用 `UID STORE`。
- **删除**：默认移动到 Trash；Trash 内删除才 `\Deleted + EXPUNGE`。可配置为直接删除。
- **UIDVALIDITY 变化**：清空该文件夹消息与索引，重新同步；保留本地草稿/发件箱。
- **POP3**：只支持收件箱、手动/定时收取、`UIDL` 去重；不支持文件夹、标志同步、IDLE。

### 5.4 会话聚合

- 优先使用 `References` / `In-Reply-To` 构建会话树；缺失时用规范化主题（去掉 `Re:`、`Fwd:`、`回复:`、`转发:` 前缀，折叠空白和大小写）。
- 会话内按时间排序；同一 `rfc_message_id` 去重。
- 不跨账号合并会话；统一收件箱只按时间合并列表。
- 会话未读数 = 会话内未读消息数；打开会话不自动全部已读，用户滚动/点击后标记。

### 5.5 发件箱与草稿

- 写信内容每 3 秒自动保存到 `drafts`；关闭窗口/切换账号立即保存。
- 点击发送后先写 `outbox`，状态 `pending`；5 秒撤销窗口内可取消。
- SMTP 成功：状态 `sent`，异步 `APPEND` 到 Sent；追加失败不回滚发送状态，进入重试队列。
- SMTP 失败：状态 `retry`，指数退避；认证失败转 `failed` 并提示。
- 重启恢复：`sending` 状态改为 `retry`；用 `rfc_message_id` 检查 Sent 是否已存在，避免重复发送。
- 附件在发送前校验大小、文件名编码（RFC 2231）和 MIME 类型。

### 5.6 一致性与维护

- SQLite：`journal_mode=WAL`、`synchronous=NORMAL`、`foreign_keys=ON`、`busy_timeout=5000`。
- 单写线程串行化所有写事务；消息插入、`search_docs`、FTS 更新在同一事务。
- 启动执行 `PRAGMA quick_check`；失败则提示备份并尝试恢复。
- 每周 `PRAGMA optimize`；FTS 索引可重建；数据库迁移前自动备份到 `backups/`。
- 迁移只向前，不自动降级；迁移失败回滚并保留旧库。
- 数据库文件权限 `0600`，目录 `0700`。

### 5.7 规模目标

| 规模 | 目标 |
|---|---|
| 单账号 1 万封 | 日常使用无感知卡顿 |
| 单账号 5 万封 | 列表/搜索可用，首次同步后台完成 |
| 总 10 万封、5 账号 | 搜索 < 800ms，内存 < 300MB |
| 单封正文 5MB | 阅读不阻塞 UI，渲染可分块 |
| 附件 100MB | 分块下载、进度、可取消 |

- 列表每页 100–200 条，使用 `(sent_at, id)` 键集分页，不用 `OFFSET`。
- QML 列表只持有当前页 ID 和可见项数据，不把全量邮件装入模型。
- 正文 LRU 缓存 50–100 封；附件不缓存到内存。

---

## 6. UI / UX

### 6.1 主窗口

- 原生标题栏，不做无边框窗口；避免麒麟窗口管理器下的拖动/缩放/最大化兼容问题。
- 三栏：左栏 220–280px（可折叠），中栏 320–420px，右栏自适应；分隔条可拖动，宽度记忆。
- 顶部工具栏：搜索、写信、回复、转发、归档、删除、刷新、布局切换。
- 底部状态栏：同步状态、离线提示、索引状态、当前账号。
- 最小窗口 1024×720；默认 1280×800。

### 6.2 邮件列表

- 按日期分组：今天、昨天、本周、更早。
- 每行：发件人、主题、摘要、时间、未读点、旗标、附件图标、会话数、标签色点。
- 多选：`Ctrl/Cmd` 点选、`Shift` 连选；批量已读、旗标、移动、删除。
- 快速过滤：全部/未读/旗标/有附件。
- 虚拟滚动；滚动时只更新可见行，避免整表重绘。
- 空状态、加载骨架、错误重试。

### 6.3 阅读窗

- 头部：主题、发件人、收件人/抄送（可展开）、时间、旗标、更多操作。
- 会话栈：同一会话按时间展开，最新在上；引用内容默认折叠。
- 正文：litehtml 分块渲染；远程图片拦截条；“在浏览器打开”；“显示图片”按发件人白名单。
- 附件条：文件名、大小、下载/打开/全部保存；不自动打开。
- 快捷操作：`R` 回复、`A` 全部回复、`F` 转发、`E` 归档、`Delete` 删除、`U` 未读。
- 文本选择：拖选、双击选词、`Ctrl+C`；跨块选择按文本 run 拼接；复杂排版退化为按行选择。

### 6.4 写信窗口

- 独立窗口，可多开；标题显示主题；关闭时若有未保存内容则提示。
- 收件人/抄送/密送：chip 输入、通讯录自动补全、最近联系人、地址校验。
- 富文本：粗体、斜体、下划线、删除线、无序/有序列表、引用、链接、清除格式；不做表格和复杂排版。
- 纯文本模式切换；切换时保留可转换的格式，提示可能丢失的内容。
- 附件：拖拽、选择文件、粘贴文件；显示总大小；超过服务器限制时提示。
- 签名：按账号配置 HTML/纯文本签名；插入位置在引用上方。
- 自动保存：3 秒防抖；状态显示“正在保存/已保存”。
- 发送：5 秒撤销；发送失败保留在发件箱并提示重试。
- 回复/转发：自动引用原文；附件不自动带入转发（可手动添加）。

### 6.5 日历与通讯录

- 日历：月/周/日视图；今天、前后翻页、新建事件；事件编辑（标题、时间、全天、地点、描述、提醒、重复）。
- 多日历：按账号/日历着色；勾选显示；只读日历。
- 提醒：系统通知；点击打开事件。
- 通讯录：列表/详情/搜索；分组；从邮件右键“添加到通讯录”；自动补全。
- CalDAV/CardDAV：手动配置 URL/账号；支持 `.well-known` 发现；同步失败独立提示。
- iMIP 邀请：V1.1。

### 6.6 设计系统

- 颜色、间距、圆角、字体、阴影全部 token 化；QML 单例 `Theme`。
- 亮色/暗色/跟随系统；暗色模式对 HTML 邮件做反色/降亮处理，保留图片原色。
- 字体：优先系统字体（麒麟默认中文字体），回退 Noto Sans CJK；不内置大体积中文字体。
- 8px 网格；圆角 6–8px；动效 150–200ms，`OutCubic`/`OutQuad`。
- 不使用重模糊、大面积半透明、复杂粒子；低端 GPU 上直接关闭动效。
- 尊重“减少动效”系统设置；提供“性能模式”（关闭动画/阴影）。

### 6.7 输入法与可访问性

- 所有文本输入使用 QML `TextInput`/`TextArea` 原生输入法路径，不拦截 `key` 事件处理预编辑。
- 测试矩阵：fcitx5、ibus、搜狗输入法麒麟版；中文、日文、emoji；候选框跟随光标。
- 键盘导航完整：Tab 顺序、焦点环、Esc 关闭、Enter 确认。
- 高对比度主题；字号可调（小/中/大）。
- V1.0 保证键盘和高对比度；Orca 读屏为 V1.1 改进项。

### 6.8 快捷键

| 快捷键 | 功能 |
|---|---|
| `C` | 写信 |
| `R` / `A` / `F` | 回复 / 全部回复 / 转发 |
| `J` / `K` | 下一条 / 上一条 |
| `Enter` | 打开 |
| `E` | 归档 |
| `Delete` | 删除 |
| `U` | 切换未读 |
| `S` | 旗标 |
| `Ctrl+K` | 搜索/命令面板 |
| `Ctrl+N` | 新窗口写信 |
| `Ctrl+R` | 刷新当前账号 |
| `Ctrl+,` | 设置 |

---

## 7. HTML 渲染与安全

### 7.1 渲染管线

```text
原始 MIME
  → 选择 text/html 或 text/plain
  → 字符集检测/解码（UTF-8、GB18030、GBK、ISO-8859、RFC 2047）
  → HTML 清洗（allowlist）
  → litehtml + Gumbo 解析/布局
  → 按宽度分块绘制（tile 高度 1024px）
  → 输出 QImage + TextRun[] + LinkRegion[]
  → QML 显示，滚动时按需请求相邻 tile
```

- 渲染宽度随窗口变化；宽度变化时防抖 150ms 重新布局。
- 典型邮件渲染 < 200ms；超过 200ms 或 DOM 节点 > 20,000 时进入简化视图。
- tile 缓存上限 64MB，LRU 淘汰；快速滚动取消过期请求。
- 链接区域和文本 run 与 tile 一起返回，UI 不解析 HTML。

### 7.2 HTML 清洗

- 允许标签：`html, head, style, body, div, span, p, br, hr, table, thead, tbody, tfoot, tr, td, th, ul, ol, li, dl, dt, dd, blockquote, pre, code, a, img, strong, b, em, i, u, s, font, center, h1-h6, sup, sub`。
- 移除：`script, iframe, frame, object, embed, form, input, button, textarea, select, meta, link, base, video, audio, canvas, svg`。
- 属性 allowlist：`href, src, alt, title, width, height, colspan, rowspan, align, valign, style, class, id, bgcolor, color, face, size`。
- CSS：允许内联样式和 `<style>`，但移除 `@import`、`url()`、`expression()`、`behavior`、`position:fixed`；限制 `z-index`。
- URL 协议：仅 `http`、`https`、`mailto`、`cid`；禁止 `javascript:`、`file:`；`data:` 只允许用于 `img src`，且解码后不超过 64KB，其他场景一律拒绝。
- 所有链接外部浏览器打开；悬停显示完整 URL；不自动加载远程图片。
- 清洗后的 HTML 只交给 litehtml，不注入 QML。

### 7.3 远程内容与隐私

- 默认阻止远程图片、样式表、字体、追踪像素。
- 顶部提示“已阻止远程内容”，提供“本次显示”“始终显示此发件人”。
- 白名单按发件人地址存储；可在设置中清除。
- 1×1 像素、透明 GIF 等追踪像素默认阻止；显示图片时也不自动请求已知追踪域名（可选规则）。
- 不加载外部 CSS/字体；只用系统字体。
- 不执行 JavaScript；不提供“启用脚本”选项。

### 7.4 文本选择与复制

- 渲染时记录每个文本 run：文本、文档坐标、字体、颜色、字号。
- 鼠标按下/拖动：命中测试文本 run，计算 anchor/focus 文本偏移；绘制半透明高亮。
- 双击选词、三击选段；`Ctrl+A` 全选；`Ctrl+C` 复制纯文本。
- 跨 tile 选择：按文档顺序拼接 run；复制时保留换行和段落。
- 复杂布局（绝对定位、重叠）退化为按行选择；提供“复制全文”。
- 链接点击优先于选择：按下在链接区域内则延迟到释放判断。

### 7.5 字符集与 MIME

- 头部 RFC 2047 编码词解码；支持 `=?UTF-8?B?`、`=?GB2312?Q?` 等。
- HTML `meta charset`、`Content-Type charset`、BOM 检测；无法确定时用 `uchardet` 思路的启发式检测，默认 UTF-8。
- 支持 `multipart/alternative`、`multipart/related`、`multipart/mixed`；嵌套深度上限 20。
- `message/rfc822` 作为附件处理，不递归展开。
- 文件名 RFC 2231/2047 解码；非法字符替换，保留扩展名。
- 纯文本邮件按 `text/plain` 显示，保留换行；URL 自动识别为可点击链接。

### 7.6 附件安全

- 附件不自动打开；先保存到隔离目录（`~/.local/share/ningmail/quarantine`，`0700`）。
- MIME 类型以服务器声明 + 扩展名 + 文件头嗅探综合判断；不一致时提示。
- 可执行文件、脚本、`.desktop` 等高风险类型默认只提供“保存”，不提供“打开”。
- 可选 ClamAV 扫描（检测到 `clamd` 时启用）；扫描失败不阻止保存，但提示。
- 附件下载支持断点/取消；大附件分块写入，避免内存峰值。

### 7.7 凭据、TLS 与隐私

- 密码/授权码存 libsecret（Secret Service）；不可用时仅保留当前会话内存，退出即忘。
- OAuth2 token（V1.1）同样存 libsecret；刷新 token 不写日志。
- TLS 最低 1.2；严格校验证书链和主机名；支持导入企业 CA；不提供“忽略证书错误”。
- 支持系统代理和手动 HTTP/SOCKS 代理；libetpan 代理能力在 M0 验证，若不支持则通过本地 CONNECT 隧道或自研异步 IMAP 层补齐。
- 默认无遥测；崩溃报告 opt-in；诊断包脱敏。
- 日志保留 7 天，单文件 10MB，最多 5 个轮转。

---

## 8. 平台集成与打包

### 8.1 麒麟集成

| 能力 | 方案 | 备注 |
|---|---|---|
| 系统托盘 | StatusNotifierItem（DBus）；回退 QSystemTrayIcon/XEmbed（仅托盘链接 QtWidgets，不用于界面） | UKUI 兼容性 M0 验证 |
| 通知 | `org.freedesktop.Notifications` | 点击打开对应邮件 |
| 凭据 | libsecret / Secret Service | 不可用时仅会话内保存 |
| `mailto:` | `.desktop` 注册 `x-scheme-handler/mailto` | 单实例转发给已运行进程 |
| 单实例 | DBus `org.ningmail.NingMail`；回退 QLocalServer | 双击图标/邮件链接复用窗口 |
| 自启动 | 可选 `.desktop` 到 `~/.config/autostart` | 默认关闭 |
| 文件管理器 | 附件“在文件夹中显示” | 使用 `QDesktopServices` |
| 缩放 | Qt HighDPI 自动；125%/150%/200% 测试 | 不强制整数缩放 |
| 主题 | 跟随系统亮/暗；可手动覆盖 | 读取 Qt palette + 设置 |

### 8.2 构建基线

- **默认基线：麒麟 V10 SP1 2303/2403、V10 SP3（glibc 2.31）**。
- 构建环境：Debian 11 sysroot + GCC 11（backports/独立工具链）+ CMake 3.21+ + Ninja。
- Qt 6.8 LTS 从源码构建所需模块：`qtbase`、`qtdeclarative`、`qtsvg`、`qtnetworkauth`、`qttools`、`qttranslations`；**不构建 QtWebEngine**。
- 产物用 `objdump -T` / `readelf --version-info` 检查，确保不引用高于 2.31 的 GLIBC 符号。
- 若部署清单中存在 glibc 2.28 的存量麒麟机器，M0 评估 Qt 6.5 兼容构建；架构不变，单独出兼容包。
- 编译器警告：`-Wall -Wextra -Wpedantic -Werror`（第三方库除外）。

### 8.3 打包

```text
/opt/ningmail/
  bin/ningmail
  lib/            Qt、ICU、OpenSSL、libetpan、litehtml 等
  plugins/        Qt platform/imageformat/iconengines
  qml/            QML 模块
  translations/
/usr/bin/ningmail -> /opt/ningmail/bin/ningmail
/usr/share/applications/ningmail.desktop
/usr/share/icons/hicolor/.../ningmail.png
/usr/share/doc/ningmail/{copyright,LICENSE,...}
```

- deb 包名 `ningmail`；版本 `MAJOR.MINOR.PATCH`；架构 `amd64`/`arm64`。
- 系统依赖尽量少：`libc6`、`libstdc++6`、`libxcb-*`、`libgl1`、`fontconfig`、`libsecret-1-0`。
- 自带 Qt 运行时，避免与系统 Qt 5.15/6.x 冲突；启动脚本设置 `LD_LIBRARY_PATH` 和 `QT_PLUGIN_PATH`，不污染系统。
- 提供离线依赖包/安装说明，适配内网无外网环境。
- deb 使用 GPG 签名；发布附 SHA256。
- 麒麟软件商店兼容性认证纳入 V1.0 发布清单。

### 8.4 CI

- 矩阵：`amd64`、`arm64`。
- x86_64：GitHub Actions 托管 runner 或自建。
- ARM64：自建飞腾/鲲鹏 runner（推荐），或 QEMU 交叉构建 + 真机冒烟。
- 流水线：格式检查 → 静态分析 → 单元测试 → 构建 → 集成测试 → 打包 → 冒烟测试 → 产物归档。
- 缓存 Qt 构建产物和 ccache；ARM64 全量 Qt 构建只在 Qt 版本变更时执行。
- 每次 PR 跑 x86_64 全量；ARM64 每日/合并前跑。
- 冒烟测试在麒麟虚拟机/真机上执行：启动、建账号、收一封、发一封、搜索、托盘、通知、输入法。

### 8.5 更新

- 默认离线 deb 升级；提供 `apt` 源方式（企业内网）。
- 可选在线检查更新：HTTPS + 签名校验；不强制、不后台自动安装。
- 数据库迁移前自动备份；升级失败可回滚到上一 deb。

### 8.6 许可合规

- 随包附 `copyright` 和第三方许可证全文。
- Qt LGPLv3：动态链接；提供可重链接的目标文件/静态库说明；不修改 Qt 源码（如修改，需公开修改部分）。
- 上线前由法务确认 LGPLv3 义务和应用 MIT 的兼容性。
- 不使用 GPL-only 的邮件库；libetpan/litehtml/libical 的许可证需在选型锁定前复核。

---

## 9. 非功能指标

### 9.1 性能

| 场景 | 目标 | 测量方法 |
|---|---|---|
| 冷启动 | < 2.5s | 从进程启动到首帧可交互 |
| 首屏列表 | < 300ms | 本地 1 万封，缓存已建 |
| 滚动 | 60fps 目标，≥30fps 下限 | 1080p，FT-2000/4 集显 |
| 搜索 | 1 万 < 300ms；10 万 < 800ms | FTS5，含高亮 |
| 打开邮件 | 本地正文 < 150ms；需下载 < 2s（正常网络） | 典型 100KB HTML |
| 发送 | 点击到进入发件箱 < 100ms | 不等 SMTP |
| 内存 | 空闲 < 200MB；阅读 < 300MB | RSS |
| 安装包 | 100–130MB | deb 压缩后 |

### 9.2 可靠性

- 发件箱/草稿持久化；崩溃/断电后不丢已写内容。
- SQLite WAL + 事务；启动 `quick_check`；迁移备份。
- 网络断线自动重连；IDLE 断开后 1 分钟内恢复。
- 单文件夹同步失败不影响其他文件夹。
- 日志轮转，磁盘占用上限 50MB。

### 9.3 安全

- 不执行邮件中的 JavaScript；不加载远程内容（默认）。
- HTML 清洗 allowlist；链接协议限制；附件隔离。
- TLS 严格校验；不提供忽略证书错误。
- 凭据存 keyring；日志脱敏；诊断包需确认。
- MIME/HTML 解析器 fuzz；依赖漏洞扫描纳入 CI。
- 数据库/附件目录权限 `0600`/`0700`。

### 9.4 可维护性

- 模块边界清晰；`libs/*` 不依赖 QML；`app/core` 不依赖具体 UI。
- 核心逻辑单元测试覆盖率 ≥ 60%；MIME/清洗/搜索 ≥ 80%。
- 公共接口有 Doxygen 注释；复杂协议状态机有状态图。
- 代码格式：clang-format；静态分析：clang-tidy；QML：qmllint。
- 提交规范：Conventional Commits；PR 需通过 CI 和至少 1 人 review。

---

## 10. 测试策略

### 10.1 单元测试

- MIME 解析：嵌套 multipart、RFC 2047、RFC 2231、GB18030/GBK、损坏邮件。
- 会话聚合：References/In-Reply-To、缺失头、主题兜底、循环引用。
- HTML 清洗：脚本/iframe/表单/远程图片/危险 URL/样式注入。
- FTS5：中文分词、英文、混合、前缀、短语、过滤。
- 发件箱：重试、撤销、重启恢复、Sent 去重。
- 日历：RRULE、时区、全天事件、跨天事件。
- vCard：折行、转义、多值、编码。

### 10.2 集成测试

- Dovecot（IMAP/SMTP）容器：IDLE、断线重连、UIDVALIDITY 变化、EXPUNGE、CONDSTORE。
- GreenMail：基础协议兼容性。
- 5 万封合成邮箱：首次同步、增量同步、搜索、分页。
- CalDAV/CardDAV：Radicale 或 SabreDAV 容器。
- 代理：HTTP CONNECT/SOCKS5（如 libetpan 支持）。
- OAuth2 mock（V1.1）。

### 10.3 渲染金标准

- 30 封真实邮件（脱敏）：
  - 中文 GBK/GB18030 编码
  - 纯文本
  - 表格布局营销邮件
  - CID 内嵌图片
  - 远程图片/追踪像素
  - 深色模式
  - 超长邮件（> 1MB HTML）
  - 恶意 HTML（脚本、iframe、表单、`javascript:` 链接）
- 截图对比：像素差异阈值 + 人工复核；记录渲染耗时。
- 性能预算：典型邮件 < 200ms；超限进入简化视图。

### 10.4 UI 测试

- Qt Quick Test：模型、命令、快捷键、焦点。
- 手工用例：三栏拖拽、折叠、多选、拖拽附件、写信、撤销发送、托盘、通知、`mailto:`。
- 输入法：fcitx5/ibus/搜狗；中文预编辑、候选框、emoji。
- 缩放：125%/150%/200%；多显示器。
- 主题：亮/暗/高对比度。
- 可访问性：纯键盘完成收信、读信、回信。

### 10.5 性能与安全

- 启动、滚动、搜索、内存基准；每次合并前跑 x86_64，发布前跑 ARM64 真机。
- MIME/HTML fuzz（libFuzzer）；至少 24 小时无崩溃。
- 依赖扫描：Trivy/OSV；高危漏洞阻断发布。
- TLS 测试：自签 CA、过期证书、主机名不匹配、弱协议。
- 附件测试：伪装扩展名、超大文件、路径穿越文件名。

### 10.6 兼容性矩阵

| 维度 | 覆盖 |
|---|---|
| 系统 | 麒麟 V10 SP1 2303/2403、V10 SP3 |
| 架构 | ARM64（飞腾/鲲鹏）、x86_64（兆芯/海光） |
| 桌面 | UKUI 3.x，X11 为主；Wayland 尽力 |
| 输入法 | fcitx5、ibus、搜狗麒麟版 |
| 缩放 | 100%、125%、150%、200% |
| 主题 | 亮、暗、高对比度 |
| 网络 | 直连、HTTP 代理、SOCKS5、离线 |
| 邮件服务器 | Dovecot、Coremail、QQ、163、腾讯企业邮、Exchange IMAP |

---

## 11. 里程碑

### 11.1 V1.0

| 阶段 | 周期 | 交付 | 退出标准 |
|---|---|---|---|
| M0 技术验证 | 2 周 | litehtml 渲染 20–30 封真实邮件；QML 列表滚动；输入法/托盘/通知；双架构 deb Hello World；Qt 6.8 构建基线 | 关键风险有结论；go/no-go |
| M1 可用骨架 | 4–6 周 | 三栏界面、账号配置、IMAP/SMTP 基础收发、本地库、列表/阅读窗、纯文本写信 | 能稳定收发一封中文邮件 |
| M2 核心体验 | 4–6 周 | IDLE/增量同步、离线、FTS 搜索、附件、草稿/发件箱、通知、会话视图 | 1 万封邮箱日常可用 |
| M3 体验打磨 | 4 周 | 富文本写信、快捷键、设计系统、设置、深色模式、日历/通讯录 | 主要交互无阻塞缺陷 |
| M4 完整 V1 | 4 周 | 性能/安全加固、打包、麒麟兼容性测试、Beta | 成功标准达标；发布检查单通过 |

总计约 18–22 周，团队 4–5 人：

- 1 名 Qt/C++ 技术负责人
- 2 名 C++/QML 工程师
- 1 名 QA/构建/发布工程师
- 0.5 名 UX 设计
- 0.5 名安全/合规（兼）

### 11.2 V1.1

- Exchange（EWS/Graph）、OAuth2、iMIP、S/MIME、国密、规则/过滤器、内嵌图片、读屏改进。
- 预计 8–12 周，取决于 Exchange 认证方式和测试环境。

---

## 12. 风险与缓解

| 风险 | 影响 | 概率 | 缓解 |
|---|---|---|---|
| litehtml 对复杂邮件还原度不足 | 高 | 中高 | M0 用真实邮件验证；浏览器兜底；简化视图；金标准测试 |
| litehtml 文本选择/分块性能 | 中高 | 中 | text run 显示列表；tile 缓存；按行退化；M0 原型 |
| libetpan 代理/IDLE/服务器兼容 | 高 | 中 | M0 验证 Coremail/QQ/163；准备本地 CONNECT 隧道或自研异步 IMAP 备选 |
| Exchange 适配复杂度 | 高 | 高 | 放 V1.1；独立适配层；真实租户/on-prem 环境测试；预留 6–8 周 |
| 麒麟托盘/输入法/通知差异 | 中高 | 中高 | M0 真机验证；SNI 与 XEmbed 双路径；输入法矩阵 |
| Qt 6.8 在 glibc 2.31 构建/打包 | 中 | 中 | Debian 11 sysroot + GCC 11；CI 缓存；M0 验证；必要时 Qt 6.5 兼容包 |
| 低端 GPU/软件渲染掉帧 | 中 | 中 | 性能模式；关闭模糊/阴影；列表虚拟化；tile 渲染 |
| Qt LGPLv3 合规 | 中 | 低 | 动态链接；法务复核；保留可重链接目标文件 |
| 国产邮箱服务器差异 | 中 | 高 | 兼容性矩阵；真实账号测试；协议日志脱敏 |
| 数据库损坏/迁移失败 | 高 | 低 | WAL、事务、迁移备份、quick_check、重建索引 |
| 范围蔓延（Exchange/国密/规则） | 高 | 高 | 明确 V1.0/V1.1 边界；变更需重新估算 |

### 12.1 M0 go/no-go 标准

- litehtml 在 30 封真实邮件中，至少 24 封无需“浏览器打开”即可读（允许轻微排版差异）。
- QML 三栏列表在 FT-2000/4 上滚动 ≥ 30fps（1080p）。
- fcitx5/ibus/搜狗至少两种输入法中文输入正常。
- 托盘图标可见、通知可点击、`mailto:` 能唤起写信窗口。
- ARM64 与 x86_64 deb 均能在麒麟 V10 上启动。
- Qt 6.8 在 glibc 2.31 基线上构建通过。
- libetpan 能连接 Coremail/QQ/163 中至少两种，并支持 IDLE 或可接受的轮询替代。

任一项不达标，M0 结束前给出替代方案，不带着未验证的风险进入 M1。

---

## 13. 决策与假设

以下决策已在设计中固定；如业务方要求变更，需要重新评估工期：

1. **单进程多线程**，V1.1 再考虑渲染进程沙箱。
2. **litehtml 作为默认 HTML 渲染器**，不引入 QtWebEngine/Chromium。
3. **Qt 6.8 LTS + QML**，不混用 Qt Widgets 主框架。
4. **Exchange/OAuth2 放 V1.1**，V1.0 用 IMAP/SMTP + CalDAV/CardDAV。
5. **默认基线 glibc 2.31**；2.28 存量机通过兼容包解决，不改变主架构。
6. **V1.0 不做国密/S-MIME/规则/内嵌图片**。
7. **应用 MIT，Qt LGPLv3 动态链接**；上线前法务复核。
8. **不依赖系统 WebKit/Chromium**。

需要在 M0 验证的假设：

- 麒麟 V10 目标版本的 glibc/Qt 构建基线。
- UKUI 托盘 SNI 与通知行为。
- libetpan 在国产邮件服务器上的 IDLE/代理/字符集表现。
- litehtml 的真实邮件还原度与文本选择可行性。
- 飞腾/兆芯整机上的 QML 渲染性能。

---

## 14. 附录

### 14.1 依赖清单

| 依赖 | 用途 | 许可证 | 打包方式 |
|---|---|---|---|
| Qt 6.8 LTS | UI、网络、SQL、XML、DBus | LGPLv3 | 自带 |
| libetpan | IMAP/SMTP/POP3/MIME | BSD-3-Clause | 自带 |
| litehtml | HTML/CSS 渲染 | BSD-3-Clause | 自带 |
| Gumbo | HTML5 解析 | Apache-2.0 | 自带 |
| libical | iCalendar | MPL-2.0 / LGPL-2.1 | 自带 |
| SQLite | 本地存储/FTS5 | Public Domain | 自带 |
| ICU | 中文分词、字符集、时区 | Unicode License | 自带 |
| OpenSSL 3 | TLS/加密 | Apache-2.0 | 自带 |
| libsecret | 凭据存储 | LGPL-2.1 | 系统依赖 |
| Catch2 | 单元测试 | BSL-1.0 | 构建期 |
| CMake/Ninja | 构建 | BSD/Apache-2.0 | 构建期 |

### 14.2 目录结构

```text
ningmail/
  CMakeLists.txt
  cmake/
  app/
    main.cpp
    ui/                 QML、ViewModel、Theme
    core/               Account、Sync、Store、Search、Outbox、Notification
  libs/
    mail/               libetpan 封装、MIME、SMTP、字符集
    render/             litehtml、清洗、分块、text run
    calendar/           libical、CalDAV、CardDAV、vCard
    exchange/           V1.1 EWS/Graph
  platform/kylin/       keyring、tray、notification、mailto
  tests/
    unit/
    integration/
    golden/
  packaging/debian/
  translations/
  docs/
    superpowers/specs/
```

### 14.3 术语

- **UIDVALIDITY**：IMAP 文件夹唯一性版本；变化表示 UID 全部失效。
- **CONDSTORE/QRESYNC**：IMAP 增量同步扩展，减少标志和消息比对。
- **IDLE**：IMAP 服务器推送新邮件/变更。
- **CID**：`Content-ID`，HTML 邮件内嵌图片的引用方式。
- **FTS5**：SQLite 全文搜索模块。
- **RHI**：Qt 渲染硬件接口；无 GPU 时可回退软件渲染。
- **SNI**：StatusNotifierItem，Linux 桌面托盘协议。
- **EWS**：Exchange Web Services；Exchange Online 已转向 Graph，on-prem 仍可用。
- **CalDAV/CardDAV**：日历/通讯录同步协议。
- **iMIP**：通过邮件发送日历邀请的标准。
