# ESP32-HID-Serial API 参考

本文档适用于协议版本 `proto = 1`，对应固件 v1.x。设计原理见 [DESIGN.md](DESIGN.md)。

- [1. 通用约定](#1-通用约定)
- [2. 传输方式](#2-传输方式)：串口、HTTP `/api/rpc`、HTTP `/api/config`、MCP `/mcp`
- [3. sys.*](#3-sys-系统)
- [4. wifi.*](#4-wifi-网络)
- [5. ble.*](#5-ble-蓝牙)
- [6. cfg.*](#6-cfg-配置)
- [7. hid.*](#7-hid-输入控制)
- [8. 事件](#8-事件)
- [9. 错误码](#9-错误码)
- [10. 按键名与媒体键表](#10-按键名与媒体键表)

---

## 1. 通用约定

### 1.1 消息结构
各传输方式共用同一套方法和参数，只是外层封装不同（见第 2 节）。下面的示例都按**串口格式**书写：`→` 表示主机发出，`←` 表示设备返回。

| 类型 | 格式 |
|---|---|
| 请求 | `{"id":<int\|string>,"method":"<name>","params":{...}}`，`params` 可以省略 |
| 通知 | 同请求，但**不带 `id`**。设备执行但不回复，适合高频鼠标移动 |
| 成功 | `{"id":..,"result":{...}}` |
| 成功+警告 | `{"id":..,"result":{...},"warn":{"code":3002,"message":"reboot required"}}` |
| 失败 | `{"id":..,"error":{"code":<int>,"message":"<text>"}}` |
| 事件 | `{"event":"<name>","data":{...}}`，设备主动发送 |

### 1.2 参数类型
- 数值类参数（如 `vid`）既可以写成 JSON 数字，也可以写成十六进制字符串 `"0x2225"`。
- 坐标 `x`、`y` 是**相对屏幕的比例**，取值 0.0–1.0，(0,0) 是左上角。
- 时间单位都是毫秒。
- 字符串统一用 UTF-8。

### 1.3 同步与异步
- 大部分方法是**同步**的：动作执行完才返回 `result`。
- `hid.type` 和 `hid.batch` 是**异步**的：先返回 `{"accepted":true}`，执行完后在**串口**上发 `hid.done` 事件。

---

## 2. 传输方式

### 2.1 USB 串口（JSON Lines）
| 项 | 值 |
|---|---|
| 端口 | CP2102 虚拟串口（Linux 上是 `/dev/ttyUSB*`，Windows 上是 `COMx`） |
| 参数 | **115200 8N1**，无流控（可通过 `uart.baud` 修改，重启后生效） |
| 分帧 | 一行一条紧凑 JSON，以 `\n` 结尾，也兼容 `\r\n` |
| 最大帧长 | 2048 字节 |
| 设备日志 | 以 `#` 开头的行，主机应忽略 |

主机打开串口后，建议按下面的顺序开始通信：
1. 先发一个空行 `\n`，清掉设备端可能残留的半帧。
2. 等待 `sys.ready` 事件；如果设备之前已经启动，就直接发 `sys.ping` 确认。
3. 把 DTR 和 RTS 置为无效，否则板子可能被复位。

```
← {"event":"sys.ready","data":{"fw":"1.0.0","proto":1,"reset_reason":"poweron"}}
→ {"id":1,"method":"sys.ping"}
← {"id":1,"result":{"pong":true,"uptime_ms":5321}}
```

**串口鉴权**：只有设置了 `uart.auth_token` 才需要。设置后，第一条命令必须是 `sys.auth`。
```
→ {"id":1,"method":"sys.info"}
← {"id":1,"error":{"code":4001,"message":"unauthorized"}}
→ {"id":2,"method":"sys.auth","params":{"token":"s3cret"}}
← {"id":2,"result":true}
```

### 2.2 HTTP `POST /api/rpc`
可以调用全部方法，权限和串口相同。
- 请求体：`{"method":"...","params":{...}}`，不需要 `id`。
- 设置了 `http.token` 时，请求头要带 `Authorization: Bearer <token>`。
- HTTP 状态码固定是 200，业务错误放在响应体的 `error` 字段里；只有鉴权失败返回 401。
- 需要重启才能生效的修改，响应里会带 `"reboot_required": true`（串口返回的是 `warn` 字段）。
- 异步方法只返回 `{"accepted":true}`，完成事件只在串口上输出。

```bash
curl -s http://hid-agent.local/api/rpc \
  -H 'Content-Type: application/json' -H 'Authorization: Bearer s3cret' \
  -d '{"method":"hid.key","params":{"keys":["CTRL","C"]}}'
```
```json
{"result":{"ok":true}}
```
错误示例：
```json
{"error":{"code":1001,"message":"BLE host not connected"}}
```
鉴权失败（HTTP 401）：
```json
{"error":"unauthorized"}
```

### 2.3 HTTP `/api/config`
直接读写扁平格式的配置，相当于 `cfg.get` 和 `cfg.set`。

**GET** `/api/config` 返回全部配置，密码类字段显示为 `***`：
```json
{"wifi.ssid":"Lab","wifi.pass":"***","wifi.static":false,"wifi.ip":"","wifi.mask":"","wifi.gw":"","wifi.dns":"",
 "wifi.hostname":"hid-agent","ble.name":"HID-Agent","ble.manufacturer":"HID-Agent","ble.model":"ESP32-HID-Serial",
 "ble.serial":"0001","ble.vid_src":2,"ble.vid":8741,"ble.pid":12806,"ble.ver":256,
 "hid.keyboard":true,"hid.mouse":true,"hid.abs_pointer":true,"hid.media":true,"hid.gamepad":false,
 "hid.default_interval_ms":8,"hid.auto_release_ms":5000,
 "http.enable":true,"http.port":80,"http.token":"","mcp.enable":true,
 "uart.baud":115200,"uart.auth_token":""}
```

**POST** `/api/config` 只需要提交要改的 key：
```bash
curl -s -X POST http://hid-agent.local/api/config -d '{"hid.default_interval_ms":12,"ble.name":"TestKB"}'
```
```json
{"ok":true,"reboot_required":true}
```
出错时返回 HTTP 400，`error` 是第一个出错的 key，整批修改都不会生效：
```json
{"ok":false,"error":"hid.foo","reboot_required":false}
```

### 2.4 MCP `POST /mcp`
采用 MCP Streamable HTTP，标准 JSON-RPC 2.0，响应类型是 `application/json`，不使用 SSE。
- 支持批量请求（数组）。
- 通知返回 HTTP 202。
- `GET /mcp` 返回 405。
- 鉴权方式同 `/api/rpc`。
- **只开放这些工具**：`hid_*`、`ble_status`、`sys_info`、`sys_ping`。
- 工具名就是方法名把 `.` 换成 `_`，例如 `hid.mouse_abs` 对应 `hid_mouse_abs`。

**initialize**
```json
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"demo","version":"1"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-03-26","capabilities":{"tools":{}},
   "serverInfo":{"name":"esp32-hid-serial","version":"1.0.0"},
   "instructions":"Controls a computer through a Bluetooth HID keyboard/mouse. ..."}}
→ {"jsonrpc":"2.0","method":"notifications/initialized"}
← HTTP 202（无响应体）
```

**tools/list**（节选）
```json
→ {"jsonrpc":"2.0","id":2,"method":"tools/list"}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[
    {"name":"hid_key","description":"Press and release a key combination, ...",
     "inputSchema":{"type":"object","properties":{"keys":{"type":"array","items":{"type":"string"}},"hold_ms":{"type":"integer"}},"required":["keys"]}},
    {"name":"hid_click","description":"Click a mouse button, optionally at absolute position x,y (0.0-1.0).","inputSchema":{...}},
    ...]}}
```
`hid_gamepad` 只在启用了 gamepad 配置时才会出现在列表里。

**tools/call 成功**
```json
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"hid_click","arguments":{"button":"left","x":0.5,"y":0.5}}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"{\"ok\":true}"}],"isError":false}}
```
**tools/call 执行失败**：作为工具结果返回，让 Agent 能看到错误内容。
```json
← {"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"error 1001: BLE host not connected"}],"isError":true}}
```
**未知工具，或调用了不对 MCP 开放的工具**：返回协议层错误。
```json
→ {"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"wifi_set","arguments":{}}}
← {"jsonrpc":"2.0","id":5,"error":{"code":-32602,"message":"not exposed via MCP: wifi.set"}}
```

---

## 3. sys.* 系统

### sys.ping
检查设备是否在线。
```
→ {"id":1,"method":"sys.ping"}
← {"id":1,"result":{"pong":true,"uptime_ms":123456}}
```

### sys.info
返回设备信息。
```
→ {"id":2,"method":"sys.info"}
← {"id":2,"result":{"fw":"1.0.0","proto":1,"chip":"ESP32 rev3.1 cores=2","bt_mac":"24:6F:28:AA:BB:CE","heap_free":98234,"heap_min":81020}}
```
| 字段 | 说明 |
|---|---|
| `fw` | 固件版本（git describe 或 PROJECT_VER） |
| `proto` | 协议版本，客户端用它做兼容判断 |
| `bt_mac` | BLE 地址，被测机上看到的就是这个地址 |
| `heap_free` / `heap_min` | 当前剩余堆 / 开机以来的最低值（字节） |

### sys.reboot
| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `delay_ms` | int | 300 | 延迟多久重启 |

重启前会先松开所有按键。设备重启完成后会发出 `sys.ready` 事件。
```
→ {"id":3,"method":"sys.reboot","params":{"delay_ms":500}}
← {"id":3,"result":{"ok":true}}
← {"event":"sys.ready","data":{"fw":"1.0.0","proto":1,"reset_reason":"software"}}
```

### sys.factory_reset
清空全部配置和蓝牙配对信息，然后重启。
```
→ {"id":4,"method":"sys.factory_reset","params":{"confirm":true}}
← {"id":4,"result":{"ok":true}}
→ {"id":5,"method":"sys.factory_reset"}
← {"id":5,"error":{"code":-32602,"message":"confirm:true required"}}
```

### sys.log
设置设备日志级别。开启后，日志以 `#` 开头的行输出到串口。
| 参数 | 取值 |
|---|---|
| `level` | `none`（默认）\| `error` \| `warn` \| `info` \| `debug` |
```
→ {"id":6,"method":"sys.log","params":{"level":"info"}}
← {"id":6,"result":{"ok":true}}
← #I (52310) wifi:connected with Lab, aid = 3, channel 6
```

### sys.auth
仅串口使用，见 2.1 节。

---

## 4. wifi.* 网络

### wifi.set
保存 Wi-Fi 配置并立即重连。
| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `ssid` | string ≤32 | ✅ | |
| `pass` | string ≤64 | | 省略或空串表示开放网络 |
| `ip_mode` | `dhcp` \| `static` | | 默认 dhcp |
| `ip` / `mask` / `gw` / `dns` | string | 用 static 时必填 | 点分十进制 |
| `hostname` | string ≤31 | | DHCP 主机名和 mDNS 名称，**重启后生效** |

```
→ {"id":10,"method":"wifi.set","params":{"ssid":"Lab","pass":"secret"}}
← {"id":10,"result":{"ok":true}}
← {"event":"wifi.state","data":{"state":"connecting"}}
← {"event":"wifi.state","data":{"state":"connected","ip":"192.168.1.50"}}
```
静态 IP：
```
→ {"id":11,"method":"wifi.set","params":{"ssid":"Lab","pass":"secret","ip_mode":"static","ip":"192.168.1.50","mask":"255.255.255.0","gw":"192.168.1.1","dns":"192.168.1.1"}}
← {"id":11,"result":{"ok":true}}
```
修改 hostname 时会附带重启警告：
```
→ {"id":12,"method":"wifi.set","params":{"ssid":"Lab","pass":"secret","hostname":"bench-01"}}
← {"id":12,"result":{"ok":true},"warn":{"code":3002,"message":"reboot required"}}
```
错误：
```
→ {"id":13,"method":"wifi.set","params":{"pass":"x"}}
← {"id":13,"error":{"code":-32602,"message":"ssid required"}}
```

### wifi.status
查询当前 Wi-Fi 状态。
```
→ {"id":14,"method":"wifi.status"}
← {"id":14,"result":{"state":"connected","ssid":"Lab","ip":"192.168.1.50","rssi":-52,"mdns":"hid-agent.local","mac":"24:6F:28:AA:BB:CC"}}
```
`state` 的取值：`idle`、`connecting`、`connected`、`failed`、`disabled`。未连接时没有 `rssi` 字段，`ip` 为空串。

### wifi.scan
阻塞扫描约 2–3 秒，最多返回 20 个结果。
```
→ {"id":15,"method":"wifi.scan"}
← {"id":15,"result":{"aps":[{"ssid":"Lab","rssi":-48,"ch":6,"auth":"wpa2"},{"ssid":"Guest","rssi":-71,"ch":11,"auth":"open"}]}}
```
`auth` 的取值：`open`、`wep`、`wpa`、`wpa2`、`wpa/wpa2`、`wpa2-ent`、`wpa3`、`wpa2/wpa3`、`wapi`、`owe`、`other`。

### wifi.connect / wifi.disconnect / wifi.forget
```
→ {"id":16,"method":"wifi.connect"}
← {"id":16,"result":{"ok":true}}            // 没配置过 SSID 时：{"code":-32602,"message":"no ssid configured"}
→ {"id":17,"method":"wifi.disconnect"}
← {"id":17,"result":{"ok":true}}
← {"event":"wifi.state","data":{"state":"idle"}}
→ {"id":18,"method":"wifi.forget"}           // 断开连接并清除 ssid 和密码
← {"id":18,"result":{"ok":true}}
```

---

## 5. ble.* 蓝牙

### ble.status
```
→ {"id":20,"method":"ble.status"}
← {"id":20,"result":{"state":"connected","peer":"A4:C3:F0:11:22:33","bonded":1,"name":"HID-Agent"}}
```
`state` 的取值：`connected` 或 `advertising`（未连接时）。`bonded` 是已保存的配对主机数量。

### ble.set_identity
修改蓝牙设备身份，**重启后生效**。修改后需要在被测机上**删除旧配对**，再重新配对。
| 参数 | 类型 | 说明 |
|---|---|---|
| `name` | string ≤29 | 蓝牙设备名 |
| `manufacturer` / `model` / `serial` | string ≤31 | DIS 服务里的厂商、型号、序列号 |
| `vid_src` | 1 \| 2 | 1 = Bluetooth SIG，2 = USB-IF |
| `vid` / `pid` / `ver` | uint16 | PnP ID，可以写 `"0x2225"` 这种字符串 |

```
→ {"id":21,"method":"ble.set_identity","params":{"name":"TestKB","manufacturer":"Acme","vid":"0x2225","pid":"0x3206"}}
← {"id":21,"result":{"ok":true},"warn":{"code":3002,"message":"reboot required"}}
→ {"id":22,"method":"ble.set_identity","params":{"vid":"0x12345"}}
← {"id":22,"error":{"code":-32602,"message":"invalid: ble.vid"}}
```

### ble.set_profile
选择 HID 复合设备包含哪些功能。会改变 Report Map，**需要重启并重新配对**。
| 参数 | 默认 |
|---|---|
| `keyboard` | true |
| `mouse` | true |
| `abs_pointer` | true |
| `media` | true |
| `gamepad` | false |
```
→ {"id":23,"method":"ble.set_profile","params":{"gamepad":true}}
← {"id":23,"result":{"ok":true},"warn":{"code":3002,"message":"reboot required"}}
```

### ble.advertise
开启或停止蓝牙广播。
```
→ {"id":24,"method":"ble.advertise","params":{"enable":false}}
← {"id":24,"result":{"ok":true}}
```

### ble.unpair
断开当前连接并清除所有配对信息。设备会重新开始广播。
```
→ {"id":25,"method":"ble.unpair"}
← {"id":25,"result":{"ok":true}}
← {"event":"ble.disconnected","data":{"peer":"A4:C3:F0:11:22:33","reason":534}}
```

---

## 6. cfg.* 配置

各配置项的含义、默认值以及是否需要重启，见 DESIGN.md 第 9 节。

### cfg.get / cfg.export
```
→ {"id":30,"method":"cfg.get","params":{"keys":["http.port","wifi.pass","uart.baud"]}}
← {"id":30,"result":{"http.port":80,"wifi.pass":"***","uart.baud":115200}}
→ {"id":31,"method":"cfg.export"}
← {"id":31,"result":{ ...全部配置，同 2.3 节... }}
```
不存在的 key 会被直接忽略，不报错。

### cfg.set / cfg.import
提交扁平的 key-value。只要有一个 key 不认识或类型不对，**整批都不会生效**。密码类字段传 `"***"` 表示保持原值，这样导出的配置可以原样导回。
```
→ {"id":32,"method":"cfg.set","params":{"http.token":"s3cret","hid.auto_release_ms":3000}}
← {"id":32,"result":{"ok":true}}
→ {"id":33,"method":"cfg.set","params":{"uart.baud":460800}}
← {"id":33,"result":{"ok":true},"warn":{"code":3002,"message":"reboot required"}}
→ {"id":34,"method":"cfg.set","params":{"http.port":"abc"}}
← {"id":34,"error":{"code":-32602,"message":"invalid key/value: http.port"}}
```

---

## 7. hid.* 输入控制

所有 `hid.*` 方法都有以下共同规则：
- BLE 主机未连接时返回 `1001`。
- 动作在设备端排队，按顺序执行，队列深度 32，满了返回 `1002`。
- 任何按键或按钮按住超过 `hid.auto_release_ms`（默认 5000 ms）没有松开，设备会自动全部松开。

### hid.key：组合键（按下后松开）
| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `keys` | string[] | 必填 | 最多 6 个普通键，修饰键数量不限，按键名见第 10 节 |
| `hold_ms` | int | 30 | 按住多久，最长 5000 |
```
→ {"id":40,"method":"hid.key","params":{"keys":["CTRL","SHIFT","ESC"]}}
← {"id":40,"result":{"ok":true}}
→ {"id":41,"method":"hid.key","params":{"keys":["GUI","R"],"hold_ms":50}}
← {"id":41,"result":{"ok":true}}
→ {"id":42,"method":"hid.key","params":{"keys":["CTL","C"]}}
← {"id":42,"error":{"code":-32602,"message":"invalid key: CTL"}}
```

### hid.key_down / hid.key_up：只按下 / 只松开
`hid.key_up` 传空数组或不带参数，表示松开全部按键。
```
→ {"id":43,"method":"hid.key_down","params":{"keys":["SHIFT"]}}
← {"id":43,"result":{"ok":true}}
→ {"id":44,"method":"hid.click","params":{"x":0.8,"y":0.6}}      // Shift + 点击
← {"id":44,"result":{"ok":true}}
→ {"id":45,"method":"hid.key_up","params":{"keys":[]}}
← {"id":45,"result":{"ok":true}}
```

### hid.type：输入文本（异步）
| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `text` | string ≤1024 字节 | 必填 | ASCII 字符，按 **US 键盘布局**输入。`\n` 对应回车，`\t` 对应 Tab |
| `interval_ms` | int | `hid.default_interval_ms`（8） | 按下和松开的间隔，取值 2–1000 |

非 ASCII 字符（如中文）不会输入，会计入 `skipped`。
```
→ {"id":46,"method":"hid.type","params":{"text":"Hello, World!\n"}}
← {"id":46,"result":{"accepted":true}}
← {"event":"hid.done","data":{"id":46,"result":{"typed":14,"skipped":0}}}
→ {"id":47,"method":"hid.type","params":{"text":"abc中文"}}
← {"id":47,"result":{"accepted":true}}
← {"event":"hid.done","data":{"id":47,"result":{"typed":3,"skipped":6}}}
```
> `skipped` 统计的是字节数，一个中文字符的 UTF-8 编码占 3 个字节，所以 2 个中文字符计为 6。

### hid.mouse_move：相对移动
| 参数 | 类型 | 说明 |
|---|---|---|
| `dx`, `dy` | int（±100000） | 移动距离（像素）。超过 ±32767 会自动拆成多帧发送 |

实际移动距离受系统“提高指针精确度”（鼠标加速）影响，需要精确定位时请用 `hid.mouse_abs`。
```
→ {"id":50,"method":"hid.mouse_move","params":{"dx":120,"dy":-40}}
← {"id":50,"result":{"ok":true}}
→ {"method":"hid.mouse_move","params":{"dx":5,"dy":0}}           // 通知形式，不回复
```

### hid.mouse_abs：绝对定位
| 参数 | 类型 | 说明 |
|---|---|---|
| `x`, `y` | number，取值 0.0–1.0 | 相对主屏幕的比例 |
```
→ {"id":51,"method":"hid.mouse_abs","params":{"x":0.5,"y":0.5}}
← {"id":51,"result":{"ok":true}}
→ {"id":52,"method":"hid.mouse_abs","params":{"x":1.2,"y":0.5}}
← {"id":52,"error":{"code":-32602,"message":"x,y must be numbers in 0.0..1.0"}}
```
关闭了 `abs_pointer` 配置时返回 `-32602 "abs_pointer profile disabled"`。

### hid.click：点击
| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `button` | `left` \| `right` \| `middle` \| `back` \| `forward` | left | |
| `count` | int 1–5 | 1 | 2 表示双击 |
| `x`, `y` | number 0–1 | – | 带上时先移动到该绝对位置再点击 |
```
→ {"id":53,"method":"hid.click","params":{"x":0.25,"y":0.1}}
← {"id":53,"result":{"ok":true}}
→ {"id":54,"method":"hid.click","params":{"button":"right"}}
← {"id":54,"result":{"ok":true}}
→ {"id":55,"method":"hid.click","params":{"count":2,"x":0.5,"y":0.5}}
← {"id":55,"result":{"ok":true}}
```

### hid.button：按下 / 松开鼠标键（拖拽）
| 参数 | 说明 |
|---|---|
| `button` | 同 `hid.click` |
| `state` | `down` 或 `up` |

拖拽示例：
```
→ {"id":56,"method":"hid.mouse_abs","params":{"x":0.2,"y":0.3}}
← {"id":56,"result":{"ok":true}}
→ {"id":57,"method":"hid.button","params":{"button":"left","state":"down"}}
← {"id":57,"result":{"ok":true}}
→ {"id":58,"method":"hid.mouse_move","params":{"dx":300,"dy":0}}
← {"id":58,"result":{"ok":true}}
→ {"id":59,"method":"hid.button","params":{"button":"left","state":"up"}}
← {"id":59,"result":{"ok":true}}
```

### hid.scroll：滚轮
| 参数 | 类型 | 说明 |
|---|---|---|
| `v` | int ±1000 | 垂直滚动格数，正数向上 |
| `h` | int ±1000 | 水平滚动格数，正数向右 |
```
→ {"id":60,"method":"hid.scroll","params":{"v":-5}}
← {"id":60,"result":{"ok":true}}
```

### hid.media：媒体键
| 参数 | 说明 |
|---|---|
| `key` | 名称见 10.3 节，也可以直接写 Consumer Usage，如 `"0xE9"` |
| `hold_ms` | 默认 30 |
```
→ {"id":61,"method":"hid.media","params":{"key":"VOLUME_UP"}}
← {"id":61,"result":{"ok":true}}
→ {"id":62,"method":"hid.media","params":{"key":"LOUDER"}}
← {"id":62,"error":{"code":-32602,"message":"unknown media key"}}
```

### hid.gamepad：手柄
需要先启用 gamepad 配置（`ble.set_profile {"gamepad":true}`），否则返回 `-32602`。设备按参数设置手柄状态，保持 `hold_ms` 后回到中立状态。
| 参数 | 类型 | 说明 |
|---|---|---|
| `buttons` | string[] | `A B X Y LB RB BACK START LS RS HOME`，或 `B1`–`B16` |
| `hat` | string | `up` `up_right` `right` `down_right` `down` `down_left` `left` `up_left`，省略表示居中 |
| `lx` `ly` `rx` `ry` | int ±127 | 摇杆 |
| `lt` `rt` | int 0–255 | 扳机 |
| `hold_ms` | int | 默认 50 |
```
→ {"id":63,"method":"hid.gamepad","params":{"buttons":["A"],"lx":127,"hold_ms":200}}
← {"id":63,"result":{"ok":true}}
```

### hid.release_all：全部松开
**不进入队列，立即执行**，用于紧急停止。
```
→ {"id":64,"method":"hid.release_all"}
← {"id":64,"result":{"ok":true}}
```

### hid.batch：批量时序（异步）
在设备端按顺序执行多个步骤。时间精度由设备控制，不受串口或网络延迟影响。
| 参数 | 说明 |
|---|---|
| `steps` | 数组，最多 64 项。每项是 `{"method":"hid.xxx","params":{...}}` 或 `{"delay_ms":n}`（n ≤ 10000）。不能嵌套 `hid.batch` |

任意一步失败，后续步骤都不执行，并自动松开所有按键。
```
→ {"id":70,"method":"hid.batch","params":{"steps":[
     {"method":"hid.key","params":{"keys":["GUI","R"]}},
     {"delay_ms":400},
     {"method":"hid.type","params":{"text":"notepad\n"}},
     {"delay_ms":1000},
     {"method":"hid.type","params":{"text":"hello from esp32"}}
  ]}}
← {"id":70,"result":{"accepted":true}}
← {"event":"hid.done","data":{"id":70,"result":{"steps":5}}}
```
失败示例：
```
← {"event":"hid.done","data":{"id":71,"code":-32602,"message":"step 2: invalid key: FOO"}}
```

---

## 8. 事件

| 事件 | data | 触发时机 |
|---|---|---|
| `sys.ready` | `{"fw","proto","reset_reason"}` | 启动完成。`reset_reason` 取值：poweron、software、panic、watchdog、brownout、deepsleep、other |
| `wifi.state` | `{"state","ip"?}` | Wi-Fi 状态变化 |
| `ble.connected` | `{"peer"}` | 主机连上 |
| `ble.paired` | `{"peer"}` | 链路加密完成（配对或回连） |
| `ble.disconnected` | `{"peer","reason"}` | 断开。`reason` 是 NimBLE 错误码（如 531 表示远端断开，534 表示本端断开） |
| `hid.led` | `{"num","caps","scroll"}` | 主机键盘指示灯变化（例如按了 CapsLock） |
| `hid.done` | `{"id","result"}` 或 `{"id","code","message"}` | 异步任务完成。HTTP 和 MCP 发起的任务 `id` 为 0 |

示例：
```
← {"event":"ble.connected","data":{"peer":"A4:C3:F0:11:22:33"}}
← {"event":"ble.paired","data":{"peer":"A4:C3:F0:11:22:33"}}
← {"event":"hid.led","data":{"num":true,"caps":false,"scroll":false}}
```

---

## 9. 错误码

| code | 名称 | 典型 message | 处理建议 |
|---|---|---|---|
| -32700 | 解析错误 | `parse error` | 检查 JSON 格式。这种回复里 `id` 为 null |
| -32600 | 请求无效 | `invalid request` / `frame too long` | 缺少 method，或单帧超过 2048 字节 |
| -32601 | 方法不存在 | `method not found: x` | |
| -32602 | 参数错误 | `invalid key: CTL` | 按 message 修正参数 |
| -32603 | 内部错误 | `oom` | 重试；持续出现时检查 `sys.info.heap_min` |
| 1001 | BLE 未连接 | `BLE host not connected` | 等 `ble.connected` 事件后再发 |
| 1002 | 队列满 | `HID queue full` | 降低发送频率，或改用 `hid.batch` |
| 1003 | 超时 | – | 保留 |
| 2001 | Wi-Fi 未连接 | – | 保留 |
| 2002 | Wi-Fi 认证失败 | – | 保留，目前通过 `wifi.state=failed` 事件体现 |
| 3001 | 配置写入失败 | – | NVS 异常，可以尝试 `sys.factory_reset` |
| 3002 | 需要重启（警告） | `reboot required` | 放在 `warn` 字段，执行 `sys.reboot` 后生效 |
| 4001 | 未授权 | `unauthorized` / `bad token` | 先发 `sys.auth`（串口）或带上 Bearer Token（HTTP） |

---

## 10. 按键名与媒体键表

按键名不区分大小写。

### 10.1 修饰键
| 名称 | 说明 |
|---|---|
| `CTRL` `LCTRL` / `RCTRL` | Ctrl |
| `SHIFT` `LSHIFT` / `RSHIFT` | Shift |
| `ALT` `LALT` / `RALT` | Alt（RALT 相当于 AltGr） |
| `GUI` `WIN` `CMD` `LGUI` / `RGUI` | Windows 键或 Command 键 |

### 10.2 普通键
| 类别 | 名称 |
|---|---|
| 字母和数字 | `A`–`Z`、`0`–`9` |
| 功能键 | `F1`–`F24` |
| 编辑 | `ENTER`(`RETURN`) `ESC`(`ESCAPE`) `BACKSPACE` `TAB` `SPACE` `DELETE`(`DEL`) `INSERT` |
| 导航 | `UP` `DOWN` `LEFT` `RIGHT` `HOME` `END` `PGUP`(`PAGEUP`) `PGDN`(`PAGEDOWN`) |
| 符号 | `MINUS` `EQUAL` `LBRACKET` `RBRACKET` `BACKSLASH` `SEMICOLON` `QUOTE` `GRAVE` `COMMA` `DOT` `SLASH` |
| 其他 | `CAPSLOCK` `NUMLOCK` `SCROLLLOCK` `PRTSC`(`PRINTSCREEN`) `PAUSE` `MENU`(`APP`) |
| 原始码 | `"0x04"`–`"0xFF"`，即 HID Keyboard Usage。0xE0–0xE7 会被当作修饰键 |

### 10.3 媒体键（`hid.media`）
| 名称 | Usage | 名称 | Usage |
|---|---|---|---|
| `VOLUME_UP` | 0xE9 | `VOLUME_DOWN` | 0xEA |
| `MUTE` | 0xE2 | `PLAY_PAUSE` | 0xCD |
| `NEXT` | 0xB5 | `PREV` | 0xB6 |
| `STOP` | 0xB7 | `BRIGHTNESS_UP` / `BRIGHTNESS_DOWN` | 0x6F / 0x70 |
| `HOME` | 0x223 | `BACK` | 0x224 |
| `SEARCH` | 0x221 | `CALCULATOR` | 0x192 |
| `MAIL` | 0x18A | `BROWSER` | 0x196 |

---

## 附录：Python 快速示例

```python
from tools.host import HidAgent   # 也可以直接把 tools/host.py 复制到你的项目里

dev = HidAgent("/dev/ttyUSB0", 115200,
               on_event=lambda n, d: print("event", n, d))
print(dev.call("sys.info"))
dev.key("GUI", "R")
dev.type("notepad\n")
dev.click(0.5, 0.5)
dev.call("hid.batch", {"steps": [{"method": "hid.key", "params": {"keys": ["CTRL", "A"]}},
                                 {"delay_ms": 100},
                                 {"method": "hid.key", "params": {"keys": ["DELETE"]}}]})
dev.close()
```
