# mini-bot 服务器接口说明

本仓库只包含固件。本文说明如何连接配套服务器，以及怎样通过后台 HTTP API 管理自己的设备；不包含服务端源码或任何凭据。

- [在线接口说明](http://43.142.28.216:8765/api-reference)：完整接口、字段、权限、状态码和调用示例。自建服务器使用对应基址下的 `/api-reference`。
- `/admin`：设备控制台；也可在右上角账号菜单打开「接口说明」。
- `/docs`、`/openapi.json`：Swagger 和机器可读的接口目录；手动读取的 JSON 字段以在线说明为准。

> 测试服务当前支持 HTTP，链路不加密。请在可信网络或 SSH 隧道中发送密码、会话 Cookie、配对码和 Key。文中的地址、字段名不代表已有访问授权。

## 固件接入

固件的 OTA 基址由 `CONFIG_OTA_URL` 配置，测试服务使用 `/deepbot/ota/`。不要把外部 OV 视觉网关地址填为 OTA 地址。

| 阶段 | 接口 / 通道 | 说明 |
| --- | --- | --- |
| 检查版本与登记 | `GET` / `POST /deepbot/ota/` | 固件发送 `Device-Id`（小写、冒号分隔的 MAC）和 `Client-Id`（板端持久 UUID）。待绑定时返回 `activation`；已绑定时返回 `mqtt`。
| 账号绑定 | 控制台 `/admin/bind` | 用户输入 OLED 显示或扬声器播报的六位码。首次登记但未绑定的设备不出现在后台列表。一次性码有效期 10 分钟。
| 激活确认 | `POST /deepbot/ota/activate` | 固件核对激活状态：`202` / `pending` 表示仍待绑定，`200` / `ok` 表示激活完成。随后重新请求 OTA 取得 MQTT 配置。
| 控制与工具 | MQTT | 使用 OTA 返回的连接参数和主题。`hello` 建立会话，控制事件和 MCP 工具消息通过 MQTT 收发。
| 语音 | UDP | 使用握手协商的端点交换 Opus 音频，不走 HTTP；音频首包用于建立 UDP 对端。

设备解绑后会重新进入配对流程；如果错过在线通知，兼容固件会在下一次握手时处理补发。不要通过清除 NVS 来刷新配对码：清除 NVS 可能改变板端持久 UUID，需要服务端核验登记身份。板端 UUID 校验不是出厂硬件身份认证。请勿公开板端 UUID、激活响应、配对码或 MQTT 凭据。

## 后台常用 HTTP API

先向 `POST /api/auth/login` 提交 JSON `username` 和 `password`，后续携带响应设置的 `mini_bot_session` Cookie。首次登录或管理员重置密码后，须先通过 `POST /api/auth/password` 完成密码设置；否则业务接口会返回 `428`。浏览器写请求须同源。管理员也只能管理自己账号的设备。

| 方法与路径 | 用途 |
| --- | --- |
| `GET /api/auth/me` | 查询当前账号及是否需要改密 |
| `GET /api/devices` | 列出当前账号的设备（含已解绑记录），使用 `bound` 和 `online` 判断状态 |
| `POST /api/devices/bind` | 提交 JSON `code`（六位数字字符串），可选 `name` |
| `GET /api/devices/{device_id}/chats` | 读取近期聊天；原账号也可读已解绑设备的历史 |
| `GET /api/config?device_id=…` | 读取音色和提示词 |
| `POST /api/config` | 提交 `device_id`、要修改的 `tts_voice`、`system_prompt` 或 `role_prompt`；空音色跟随平台默认 |
| `GET /api/devices/{device_id}/tools` | 查询设备与云端工具目录，不执行工具 |
| `GET /api/devices/{device_id}/scheduled-tasks` | 列出定时任务和可用工具 |
| `POST /api/devices/{device_id}/unbind` | 解除绑定，保留配置与历史；跨账号换绑时会清除前任的聊天、记忆和任务 |
| `DELETE /api/devices/{device_id}` | 删除设备及其记录，不能恢复；不影响平台模型 Key |

只读示例：在已登录且已完成改密的同源控制台中调用：

```js
const response = await fetch('/api/devices');
if (!response.ok) throw new Error(`HTTP ${response.status}`);
const { devices } = await response.json();
```

平台模型、用户管理、永久记忆、云端工具及定时任务的完整调用约定见[在线接口说明](http://43.142.28.216:8765/api-reference)。OV 视觉实验使用独立网关，授权和连接方式以开发者群内说明为准。
