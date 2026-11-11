# esp32-hid-serial

用 ESP32（ESP32-DevKitC-32U）把电脑命令转成 **BLE HID 键盘、鼠标、绝对指针、媒体键和手柄** 操作。可以通过以下三种方式发送命令：

- **USB 串口**（JSON Lines）
- **Wi-Fi HTTP**（`/api/rpc`）
- **MCP**（`/mcp`，可以直接接入 AI Agent）

详细设计见 [docs/DESIGN.md](docs/DESIGN.md)。

## 构建与烧录

```bash
. ~/esp/esp-idf/export.sh          # ESP-IDF v5.3.x
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

## 快速开始

```bash
pip install pyserial
python tools/host.py --port /dev/ttyUSB0 sys.info
python tools/host.py --port /dev/ttyUSB0 wifi.set '{"ssid":"Lab","pass":"secret"}'
# 在电脑蓝牙设置里配对 "HID-Agent"
python tools/host.py --port /dev/ttyUSB0 hid.key '{"keys":["GUI","R"]}'
python tools/host.py --port /dev/ttyUSB0 hid.click '{"x":0.5,"y":0.5}'
```

## MCP

```bash
curl -s http://hid-agent.local/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

如果客户端只支持 stdio，可以这样桥接：`npx mcp-remote http://hid-agent.local/mcp`

> 注意：修改 BLE 身份或 HID 配置后，需要先在电脑上删除旧配对，再执行 `ble.unpair` 并重启开发板。
