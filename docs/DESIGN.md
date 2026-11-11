# ESP32-HID-Serial 技术设计文档

| 项 | 内容 |
|---|---|
| 版本 | v1.0（协议版本 `proto = 1`） |
| 目标硬件 | ESP32-DevKitC-32U（ESP32-WROOM-32U，4 MB Flash，无 PSRAM，外接 2.4 GHz 天线） |
| 框架 | ESP-IDF v5.3.x，NimBLE，esp_hid，esp_http_server，cJSON，mdns |
| 用途 | 把开发板变成一个可编程的 BLE HID 设备（键盘 / 鼠标 / 绝对坐标指针 / 媒体键 / 手柄），通过 USB 串口或 Wi-Fi（HTTP / MCP）接收指令，模拟用户操作，用于自动化测试和 AI Agent 驱动的 UI 操作 |

---

## 1. 背景与约束

1. **原版 ESP32 没有 USB 控制器**。板上 Micro-USB 经 CP2102 转成 UART，只能当串口用，**不能做 USB HID**，所以 HID 只能走蓝牙。
2. 选 **BLE HID（HOGP）**，不用经典蓝牙 HID。原因：
   - NimBLE 比 Bluedroid 省 50–80 KB RAM。
   - 和 Wi-Fi、HTTP 同时跑时内存更宽裕。
   - Windows 8+、macOS、Linux、Android、iOS 都原生支持。
3. Wi-Fi 和 BLE 共用一个 2.4 GHz 射频，靠软件分时共存（`CONFIG_ESP_COEX_SW_COEXIST_ENABLE`）。Wi-Fi 必须保持默认的 modem-sleep，**不能设成 `WIFI_PS_NONE`**。
4. 以后迁移到 ESP32-S3（带原生 USB OTG）时，只需要替换 HID 输出层（`hid_send()`），协议、命令处理、配置、MCP 都不用改（见 §10）。

## 2. 总体架构

```
        ┌──────────── PC / 测试主机 ────────────┐
        │  host.py / 测试框架     MCP 客户端(Agent) │
        └──────┬──────────────────────┬─────────┘
          USB(CP2102)             Wi-Fi (HTTP)
               │                      │
┌──────────────▼──────────────────────▼───────────────────────────┐
│ ESP32                                                            │
│  uart_proto.c  JSON Lines 分帧      http_srv.c  /mcp /api/* /    │
│        └──────────────┬──────────────────┘                       │
│                  rpc.c  统一命令分发 (sys/wifi/ble/cfg/hid)       │
│        ┌──────────────┼───────────────┬───────────────┐          │
│   wifi_mgr.c      config.c (NVS)   hid_dev.c 队列+worker          │
│   STA/重连/mDNS                        │                         │
│                                hid_actions.c 动作→报告            │
│                                       │ hid_send()               │
│                         esp_hid (HOGP) + hid_ble.c (GAP/安全)    │
└───────────────────────────────────────┬──────────────────────────┘
                                   BLE HID
                                        ▼
                                  被测电脑 / 手机
```

设计原则：
- **单一命令实现**：串口、HTTP `/api/rpc` 和 MCP `tools/call` 都调用 `rpc_call()`，同一条命令在三个入口的行为完全一致。
- **HID 串行化**：所有 `hid.*` 动作进入同一个队列，由单个 worker 任务按顺序执行，避免多个来源同时发送导致按键交错。
- **安全默认**：HID 状态有自动释放机制；MCP 只开放 HID 操作和状态查询，不开放网络和配置命令。

## 3. 源码结构

| 文件 | 职责 |
|---|---|
| `main/app_main.c` | 启动顺序：NVS 配置 → 串口 → BLE HID → Wi-Fi → HTTP → 发出 `sys.ready` 事件 |
| `main/config.c/.h` | 配置结构体、默认值、NVS 读写（单个 blob 带版本号），扁平 key（`wifi.ssid`）与 JSON 互转，标记需要重启的字段 |
| `main/uart_proto.c/.h` | UART0 JSON Lines 收发、分帧状态机、事件上报、`#` 日志行、可选的串口鉴权 |
| `main/rpc.c/.h` | 命令分发、错误码、MCP 工具目录（名称、描述、JSON Schema） |
| `main/wifi_mgr.c/.h` | STA 连接、DHCP 或静态 IP、指数退避重连、扫描、mDNS |
| `main/http_srv.c/.h` | 配置网页、`/api/config`、`/api/rpc`、`/mcp`（Streamable HTTP，返回 JSON），Bearer Token 鉴权 |
| `main/hid_dev.c/.h` | 初始化 esp_hid、动作队列（深度 32）、worker 任务、LED 输出报告事件 |
| `main/hid_actions.c/.h` | 各个 `hid.*` 方法：维护按键状态、生成报告、批处理、自动释放 |
| `main/hid_report.c/.h` | 按配置拼接的复合 HID Report Map |
| `main/hid_ble.c/.h` | NimBLE 控制器初始化、广播、配对和绑定、连接跟踪、覆盖 PnP ID |
| `main/keymap.c/.h` | 按键名 → HID Usage、ASCII → Usage（US 布局）、媒体键表 |
| `main/web/index.html` | 内嵌在固件里的配置页面 |
| `tools/host.py` | Python 主机端 SDK 和命令行工具 |
| `partitions.csv` | 双 OTA 分区表（每个 app 分区 1.875 MB） |
| `sdkconfig.defaults` | NimBLE、仅 BLE、HID 服务、共存、日志等级等默认配置 |

## 4. 启动流程与任务

```
app_main
 ├─ config_init()          nvs_flash_init + 读取配置 blob（失败用默认值）
 ├─ uart_proto_init()      UART0 @ uart.baud，接管 ESP_LOG（转成 '#' 行）
 ├─ hid_dev_init()         释放经典蓝牙内存 → 控制器(BLE) → nimble → esp_hidd_dev_init
 │                         → 设置 GAP 名称/外观、DIS(PnP/型号) → 绑定存储 → host 任务
 ├─ wifi_mgr_init()        netif + wifi STA + mDNS，已配置 SSID 就开始连接
 ├─ http_srv_start()
 └─ emit sys.ready
```

| 任务 | 栈 | 优先级 | 说明 |
|---|---|---|---|
| uart_rx | 6 KB | 10 | 读串口，分帧，同步执行 rpc |
| hid_worker | 4 KB | 9 | 串行执行 HID 动作，每 200 ms 检查一次自动释放 |
| nimble_host | 默认 | – | NimBLE 主机 |
| ble_hidd_events | 4 KB | – | esp_hid 事件循环 |
| httpd | 8 KB | 默认 | HTTP 和 MCP |

## 5. 串口协议

### 5.1 物理层与分帧
- UART0（CP2102），默认 **115200 8N1**（与 ESP-IDF 默认控制台波特率一致，串口监视器开箱即用），无流控；可通过 `uart.baud` 改为 230400/460800/921600 等，改后需重启。
- **JSON Lines**：一行一条紧凑 JSON，以 `\n` 结尾，接收端兼容 `\r\n`。
- JSON 字符串里的换行必须转义成 `\n`，所以原始字节 0x0A 只会出现在帧尾。要输入多行文本，直接在 `text` 字段里写转义字符就可以。
- 接收端状态机（`uart_proto.c: feed_byte`）：
  - **帧头校验**：缓冲区为空时，只接受 `{`（命令）或 `#`（注释或日志）作为首字符，其他字节一律丢弃。这能过滤掉开机时 ROM 输出的乱码。
  - **帧尾**：收到 `\n` 就提交。去掉末尾的 `\r` 后，首字符是 `{` 才解析。
  - **超长**：超过 2048 字节就进入丢弃模式，直到下一个 `\n`，然后返回 `-32600`。
  - **半帧超时**：两个字节之间间隔超过 500 ms，清空缓冲区。
  - **重同步**：出现任何错误都等到下一个 `\n` 再恢复。主机连上后先发一个 `\n`，把设备端可能残留的半帧冲掉。
- 设备输出：
  - JSON 帧以 `{` 开头。
  - 日志行以 `#` 开头，只在 `sys.log` 开启时输出。ESP_LOG 也被重定向成 `#` 行。
- 未采用的方案：
  - 用 `\r\n` 作分隔符：没有收益，常用工具兼容性还更差。
  - 括号深度分帧：可以支持格式化的多行 JSON，但重同步差。
  - COBS/SLIP 加 CRC：预留给以后的二进制通道（比如串口 OTA）。

### 5.2 消息格式（JSON-RPC 2.0 精简版，省略 `jsonrpc` 字段）
```
请求   {"id":1,"method":"hid.key","params":{"keys":["CTRL","C"]}}
通知   {"method":"hid.mouse_move","params":{"dx":5,"dy":0}}      // 不带 id，不回复
成功   {"id":1,"result":{...}}                                   // 可附带 "warn":{"code":3002,...}
失败   {"id":1,"error":{"code":-32602,"message":"..."}}
事件   {"event":"ble.connected","data":{...}}
```
- 回复顺序和请求顺序一致，主机可以连续发送多条请求，用 `id` 匹配回复。
- 如果设置了 `uart.auth_token`，第一条必须是 `sys.auth {"token":...}`，否则返回 4001。

### 5.3 错误码
| code | 含义 |
|---|---|
| -32700 / -32600 / -32601 / -32602 / -32603 | 解析错误 / 请求无效（含超长帧） / 方法不存在 / 参数错误 / 内部错误 |
| 1001 | BLE 主机未连接 |
| 1002 | HID 队列已满 |
| 1003 | 超时（保留） |
| 2001 / 2002 | Wi-Fi 未连接 / 认证失败（保留，状态通过事件上报） |
| 3001 | 配置写入失败 |
| 3002 | 警告：需要重启才能生效（放在 `warn` 字段里） |
| 4001 | 未授权 |

### 5.4 方法
| 方法 | 参数 | 说明 |
|---|---|---|
| `sys.ping` | – | `{pong, uptime_ms}` |
| `sys.info` | – | 固件版本、协议版本、芯片、BT MAC、可用堆、最小堆 |
| `sys.reboot` | `delay_ms?` | 先松开所有按键，再重启 |
| `sys.factory_reset` | `confirm:true` | 清空配置和绑定后重启 |
| `sys.log` | `level` | none / error / warn / info / debug |
| `sys.auth` | `token` | 串口鉴权 |
| `wifi.set` | `ssid, pass?, ip_mode?(dhcp\|static), ip?, mask?, gw?, dns?, hostname?` | 保存后立即重连 |
| `wifi.connect` / `wifi.disconnect` / `wifi.forget` | – | |
| `wifi.status` | – | state / ssid / ip / rssi / mdns / mac |
| `wifi.scan` | – | 阻塞扫描，最多返回 20 个 AP |
| `ble.status` | – | state / peer / bonded / name |
| `ble.set_identity` | `name, manufacturer, model, serial, vid_src(1=SIG,2=USB), vid, pid, ver` | 需要重启，**主机端也要删除旧配对** |
| `ble.set_profile` | `keyboard, mouse, abs_pointer, media, gamepad` | 改变 Report Map，需要重启并重新配对 |
| `ble.advertise` | `enable` | 开始或停止广播 |
| `ble.unpair` | – | 断开连接并清空所有绑定 |
| `cfg.get` / `cfg.export` | `keys?[]` | 密码类字段显示为 `***` |
| `cfg.set` / `cfg.import` | 扁平 key-value | 未知 key 或类型错误时整批不生效 |
| `hid.key` | `keys[], hold_ms?=30` | 组合键：修饰键先按下，最后松开 |
| `hid.key_down` / `hid.key_up` | `keys[]`（`key_up` 传空数组表示全部松开） | |
| `hid.type` | `text, interval_ms?` | **异步**：先返回 `{accepted}`，完成后发 `hid.done` 事件，结果里有 typed 和 skipped 数量 |
| `hid.mouse_move` | `dx, dy` | 相对移动，16 位，超出范围会自动拆成多帧 |
| `hid.mouse_abs` | `x, y ∈ [0,1]` | 绝对坐标 |
| `hid.click` | `button?, count?, x?, y?` | 带 x、y 时走绝对指针 |
| `hid.button` | `button, state(down\|up)` | 用于拖拽 |
| `hid.scroll` | `v, h` | 自动拆分成 ±127 一帧 |
| `hid.media` | `key` | VOLUME_UP / DOWN、MUTE、PLAY_PAUSE、NEXT、PREV 等，也可以写 `0x..` |
| `hid.gamepad` | `buttons[], hat, lx, ly, rx, ry, lt, rt, hold_ms` | 需要启用 gamepad 配置 |
| `hid.release_all` | – | **不排队**，立即执行 |
| `hid.batch` | `steps[]`（每步是 `{method, params}` 或 `{delay_ms}`），最多 64 步 | **异步**；任意一步失败就松开所有按键并终止 |

### 5.5 事件
`sys.ready`、`wifi.state`、`ble.connected`、`ble.disconnected`、`ble.paired`、`hid.led`（Num/Caps/Scroll 灯状态）、`hid.done`。

## 6. BLE HID 设计

### 6.1 复合 Report Map（`hid_report.c`）
| Report ID | 功能 | 输入报告格式 | 默认 |
|---|---|---|---|
| 1 | 键盘（含 LED 输出报告） | `mod, 0, k1..k6`（8 B） | 开 |
| 2 | 相对鼠标 | `btn(5bit), X(int16), Y(int16), wheel(int8), pan(int8)`（7 B） | 开 |
| 3 | Consumer（媒体键） | `usage(uint16)`（2 B） | 开 |
| 4 | 绝对指针 | `btn(3bit), X(0..32767), Y(0..32767)`（5 B） | 开 |
| 5 | Gamepad | `16 btn, hat(4bit, 8 为居中), LX LY RX RY(int8), LT RT(uint8)`（9 B） | 关 |

- 各段按 `hid.*` 配置开关拼接起来，整个设备只有一个 HID Service（`map_index = 0`）。
- **绝对指针**用的是带绝对 X/Y 的 Mouse Collection，Windows、macOS、Linux 都会把它当作“绝对定位鼠标”。它不受鼠标加速影响，是自动化测试点击的首选方式。
- Gamepad 默认关闭，因为有些主机看到同时带键盘和手柄的设备会识别异常。

### 6.2 设备身份
- GAP 名称和外观：`ble.name`。外观按优先级取：开了键盘就是 Keyboard（0x03C1），否则开了手柄就是 Gamepad（0x03C4），否则是 Mouse（0x03C2）。
- DIS：制造商名、序列号由 esp_hid 设置，型号由 `hid_ble.c` 设置。
- **PnP ID**：esp_hid 把 Vendor ID Source 写死成 USB（0x02）。`hid_ble_start()` 会在 GATT 注册前调用 `ble_svc_dis_pnp_id_set()`，用配置里的 `vid_src`、`vid`、`pid`、`ver` 覆盖。
- 默认 PnP ID：VID Source = 0x02（USB-IF），**VID 0x2225，PID 0x3206**，版本 0x0100。目标是让主机上识别到的 VID/PID 和现有 USB 设备 `HID\VID_2225&PID_3206&MI_00\7&16DD86B3&0&0000` 一致。
- 注意：同样的 VID/PID 走 BLE 时，Windows 生成的硬件 ID 格式不同，例如 `HID\{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&022225_PID&3206_REV&0100...`。`MI_00`（USB 接口号）和 `7&16DD86B3&0&0000`（实例 ID）是 Windows 按总线和接口分配的，BLE 设备无法伪造。按 VID/PID 匹配设备的上层逻辑（如 hidapi 枚举、SetupAPI 按 `VID_2225&PID_3206` 子串过滤）可以正常工作；按完整 USB 实例路径匹配的逻辑不行。完全一致只能在 ESP32-S3 的 USB HID 上做到（MI_00 的前提是复合设备的第 0 个接口）。

### 6.3 广播、安全与连接
- 广播包：Flags、外观、HID 服务 UUID 0x1812。名称放在扫描响应里，避免超过 31 字节。广播间隔 30–50 ms，永久广播。断开连接或广播结束后自动重新开始。
- 安全：Just Works（`NO_IO`），开启 Bonding 和 LE Secure Connections，交换 ENC 和 ID 密钥，绑定信息保存在 NVS（`CONFIG_BT_NIMBLE_NVS_PERSIST`）。
- 连接成功后，设备主动调用 `ble_gap_security_initiate()`。如果主机丢失了绑定（REPEAT_PAIRING），设备删除旧绑定并重新配对。
- 最多 1 个连接（`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`）。

### 6.4 动作执行模型
- `hid_dev_submit()`：
  - 校验方法名和 BLE 连接状态。
  - 复制参数，封装成 job 放进队列（满了返回 1002）。
  - 同步方法用信号量等待 worker 执行完；所有动作的耗时都有上限（`hold_ms ≤ 5 s`，文本 ≤ 1024 字节，批处理 ≤ 64 步）。
- 异步方法（`hid.type`、`hid.batch`）立即返回，完成后发出 `hid.done {id, result}` 或 `hid.done {id, code, message}` 事件。
- 键盘 6KRO：组合键先单独发一帧修饰键，再发普通键，松开顺序相反。这样能兼容某些对按键顺序敏感的主机。
- `hid.type` 按 **US 键盘布局**把 ASCII 字符映射成按键。非 ASCII 字符计入 `skipped`，不会输入。中文请用输入法或剪贴板的方式（例如先在主机上设置好剪贴板，再发 `CTRL+V`）。
- **防卡键**：有按键或按钮处于按下状态超过 `hid.auto_release_ms`（默认 5000 ms）时，worker 在空闲检查时自动 release_all。BLE 断开时清空内部按键状态。`sys.reboot` 前也会先松开所有按键。

## 7. Wi-Fi 设计
- 只用 STA 模式。凭据保存在应用配置里（`esp_wifi_set_storage(RAM)`），不依赖 Wi-Fi 驱动自己的 NVS 存储。
- 支持 DHCP 或静态 IP（ip、mask、gw、dns）。主机名同时用于 DHCP 和 mDNS（`<hostname>.local`），并注册 `_http._tcp` 服务。
- 重连采用指数退避：1, 2, 4 … 最长 60 s。认证失败连续 3 次后状态变为 `failed`，但仍会继续重试。
- 状态变化通过 `wifi.state` 事件上报。
- 后续可选：连不上网时开启 SoftAP 加配置页，作为兜底配网方式。

## 8. HTTP 与 MCP

### 8.1 路由
| 路由 | 方法 | 说明 |
|---|---|---|
| `/` | GET | 配置页面（内嵌 HTML，显示状态，编辑配置，保存，重启） |
| `/api/config` | GET / POST | 读写扁平配置，返回 `reboot_required` 标志 |
| `/api/rpc` | POST | `{"method","params"}`，可以调用全部方法（权限和串口相同） |
| `/mcp` | POST | MCP Streamable HTTP，支持单条和批量 JSON-RPC，返回 `application/json` |
| `/mcp` | GET | 405（不提供服务端推送的 SSE 流） |

- 设置了 `http.token` 时，所有接口（`/` 页面本身除外）都要求请求头带 `Authorization: Bearer <token>`。
- 局域网内默认用 HTTP。HTTPS 会多占约 40 KB RAM，暂不开启。

### 8.2 MCP 实现
- 支持的方法：`initialize`（回显客户端的 protocolVersion，默认 `2025-03-26`，能力声明为 `tools`）、`ping`、`tools/list`、`tools/call`。通知（如 `notifications/initialized`）返回 202。
- 工具名 = 方法名把 `.` 换成 `_`，例如 `hid_click`、`hid_mouse_abs`、`ble_status`。
- `tools/call` 的结果以 `content[0].text`（结果 JSON 文本）返回。执行出错时设置 `isError: true`，让 Agent 能看到错误并自行纠正。
- **权限隔离**：从 MCP 进来的调用只能用 `hid.*`、`ble.status`、`sys.info`、`sys.ping`，在 `rpc_call` 入口统一拦截。这样可以避免 Agent 修改网络或配置导致设备失联。
- 只支持 stdio 的客户端可以用 `npx mcp-remote http://hid-agent.local/mcp --header "Authorization: Bearer xxx"` 桥接。

## 9. 配置项（NVS 命名空间 `appcfg`，key `cfg`，blob v1）
| key | 默认 | 重启生效 |
|---|---|---|
| `wifi.ssid` / `wifi.pass` / `wifi.static` / `wifi.ip` / `wifi.mask` / `wifi.gw` / `wifi.dns` | 空 / 空 / false | 否（会立即重连） |
| `wifi.hostname` | hid-agent | 是 |
| `ble.name` / `ble.manufacturer` / `ble.model` / `ble.serial` | HID-Agent / HID-Agent / ESP32-HID-Serial / 0001 | 是 |
| `ble.vid_src` / `ble.vid` / `ble.pid` / `ble.ver` | 2 / 0x2225 / 0x3206 / 0x0100 | 是 |
| `hid.keyboard` / `hid.mouse` / `hid.abs_pointer` / `hid.media` / `hid.gamepad` | 开 / 开 / 开 / 开 / 关 | 是，并需要重新配对 |
| `hid.default_interval_ms` / `hid.auto_release_ms` | 8 / 5000 | 否 |
| `http.enable` / `http.port` / `http.token` / `mcp.enable` | 开 / 80 / 空 / 开 | 端口和开关需要重启 |
| `uart.baud` / `uart.auth_token` | 115200 / 空 | 波特率需要重启 |

结构体变化时递增 `CFG_VERSION`，版本不匹配就回落到默认值。以后可以加迁移逻辑。

## 10. 资源预算与可移植性
| 项 | 估计 |
|---|---|
| RAM：Wi-Fi + lwIP | ~55 KB |
| RAM：NimBLE + esp_hid | ~40 KB |
| RAM：httpd + cJSON 峰值 | ~25 KB |
| RAM：业务（队列、缓冲、任务栈） | ~30 KB |
| 剩余堆 | 预计 > 80 KB（用 `sys.info.heap_min` 实测） |
| Flash：固件 | ~1.2–1.4 MB，放在 1.875 MB 的 OTA 分区里 |

**迁移到 ESP32-S3（USB HID）**：
- 新增 `hid_usb.c`，用 TinyUSB 实现 HID + CDC 复合设备。
- `hid_send()` 改成调用 `tud_hid_report()`。复用同一个 Report Map，Report ID 不变。
- 串口传输层改到 USB CDC，`uart_proto` 的分帧逻辑可以原样复用。

## 11. 已知限制与风险
| 风险 | 影响 | 应对 |
|---|---|---|
| Wi-Fi 和 BLE 共存 | HID 延迟有抖动（约 10–30 ms） | 只用串口时可以不配 Wi-Fi；对时序要求高的操作用 `hid.batch` 在设备端执行 |
| 主机缓存 GATT/HID 描述符 | 修改身份或配置后识别异常 | 文档要求：修改后先在主机删除配对，再执行 `ble.unpair`，然后重新配对 |
| CP2102 的 DTR/RTS 接在 EN/IO0 上 | 打开串口时板子可能复位 | host.py 打开串口时把 DTR/RTS 置为无效；主机等收到 `sys.ready` 再开始通信 |
| ROM 启动日志（74880 bps）和 IDF 引导日志 | 混入协议流 | 接收端只解析 `{` 开头的行；`CONFIG_LOG_DEFAULT_LEVEL_WARN` 降低日志量 |
| `hid.type` 依赖主机的键盘布局和输入法 | 非 US 布局或开着中文输入法时会输错 | 测试前确认主机为英文输入状态；中文用剪贴板方案 |
| BLE 只能有 1 个主机 | 不能同时控制多台机器 | 一块板对应一台被测机 |
| 未经 BLE 认证的 VID/PID | 合规问题 | 量产前换成合法持有的 VID/PID |
| `release_all` 可以从非 worker 任务调用 | 和正在执行的动作有短暂竞态 | 发送本身有互斥锁保护；最坏情况是多发一帧全 0 报告，可以接受 |

## 12. 验证计划
1. **构建**：`idf.py set-target esp32 && idf.py build`，没有警告升级为错误，app 大小在分区范围内。
2. **串口协议**（不需要连蓝牙主机）：
   - `sys.ping`、`sys.info`、`cfg.get` 正常返回。
   - 错误帧（非 JSON、超长、缺少 method）返回正确的错误码，并且之后的帧能正常解析。
   - 发送 `\r\n` 结尾的帧也能正常解析。
3. **Wi-Fi**：`wifi.set` 后收到 `wifi.state connected`；能访问 `http://hid-agent.local/`；断开 AP 后自动重连。
4. **BLE HID**：在 Windows 和 Android 上配对；分别验证 `hid.key ["GUI","R"]`、`hid.type`、`hid.click{x,y}`、`hid.scroll`、`hid.media`；按 Caps Lock 时收到 `hid.led` 事件。
5. **MCP**：
   - `curl -X POST /mcp -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'` 能列出工具。
   - 用 MCP Inspector 或 Claude Desktop（通过 mcp-remote）调用 `hid_click`。
6. **稳定性**：连续 1 小时每秒发 10 条随机 HID 命令，`heap_min` 稳定；断开 BLE 期间发送命令返回 1001，没有卡住的按键。

## 13. 参考
- ESP-IDF `examples/bluetooth/esp_hid_device`（NimBLE 版 HID 设备官方示例）
- ESP-IDF `components/esp_hid/src/nimble_hidd.c`（GATT 布局、DIS、PnP 实现）
- T-vK/ESP32-BLE-Keyboard、T-vK/ESP32-BLE-Mouse（键盘和鼠标 Report Map）
- lemmingDev/ESP32-BLE-Gamepad（手柄描述符）
- MCP 规范：Streamable HTTP transport（2025-03-26）
- USB HID Usage Tables 1.4
