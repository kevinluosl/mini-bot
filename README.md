# mini-bot

**一台用 ESP32-S3 驱动的小机器人，一个把 LLaVA-OneVision-2.5 接进真实世界的实验。**

mini-bot 想回答两个问题：ESP32 能否作为一台桌面机器人的基础硬件，长期稳定地连接云端「大脑」？给机器人一只摄像头之后，[LLaVA-OneVision-2.5](https://github.com/EvolvingLMMs-Lab/LLaVA-OneVision-2.5) 能在真实互动里看懂多少，又会在哪些场景出错？这不是成品机器人，而是一个持续试验语音、视觉和动作如何配合的项目。

```text
麦克风 / 触摸 / 红外 ── ESP32-S3 ── 云端机器人「大脑」
扬声器 / OLED / 舵机 ────┘                 │
摄像头 ────────── ESP32-S3 ── 视觉网关 / LLaVA-OV-2.5
```

## 重点实验：LLaVA-OV-2.5 视觉

OV-2.5 不在 ESP32 上运行。固件在开启观察后持续采集 JPEG，以约每秒一帧的频率送给视觉网关。**持续观察不等于持续解说**：只有提问才请求回答，结束观察后停止上传并关闭会话。

| 能力 | 实验方式 |
| --- | --- |
| **看当前画面** | `self.camera.look_current`：判断画面里是否有人，简述活动和可能的情绪；也可以传入具体问题，例如「我手里拿着什么？」 |
| **回看最近片段** | `self.camera.look_past`：根据最近约一分钟的画面总结人在做什么，而不是用一张最新图片猜测过去。 |
| **从看见到互动** | 测试服务把观察结果变成一句自然的口语，可配合眼睛表情和头部动作；回看支持无人时静默。 |

我们重点观察：人物和活动有没有看错，回看能否把前后动作分清，遮挡、逆光、快速运动时是否会乱猜，以及从提问到开口要等多久。情绪描述只是基于画面的推测，不当作事实。

| 接入项 | 状态 |
| --- | --- |
| OV-2.5 视觉网关地址与接口说明 | 待补充。固件的 `CONFIG_MINI_BOT_OV_BASE_URL` 默认留空，不会自动向外部网关上传画面。 |
| OV-2.5 本地部署 | 待补充。 |

## 机器人还能做什么

- **语音对话**：本地中文唤醒，连接云端完成对话与语音播报。麦克风和扬声器使用独立的 I²S 通道，支持设备端回声消除。
- **头部动作和表情**：双舵机控制左右转头和俯仰；同一个动作工具控制点头、摇头、张望等动作，并同步 OLED 眼睛表情。
- **近距离互动**：摸头和左右红外可以触发不同的回应。多路同时出现时只执行一路，避免抢动作。
- **联网与调试**：支持 Wi-Fi 配网、测试服务激活、固件更新，以及串口下的舵机和摄像头调试。

## 硬件

| 部件 | 选型 |
| --- | --- |
| 主控 | ESP32-S3-CAM 核心开发板 N16R8（16 MB Flash / 8 MB PSRAM） |
| 摄像头 | ESP32-S3 相机模块，**OV2640、160° 广角、75 mm 加长排线** |
| 屏幕 | 1.3 英寸 SH1106 128×64 I²C 单色 OLED |
| 音频 | I²S 麦克风、I²S 功放和扬声器 |
| 动作与传感器 | 两路舵机、电容触摸模块、左右红外模块 |
| 外壳 | 参考瓦楞机器人方案的 3D 打印外壳 |

接线以 `main/boards/` 下的板级 `config.h` 为准。舵机用独立电源，与主控共地；传感器信号使用 3.3 V 逻辑。

## 构建固件

安装 ESP-IDF 6.0.1 或以上版本，并使用其专用 Python 环境。首次构建由 ESP-IDF Component Manager 按 `dependencies.lock` 获取第三方依赖。

```bash
git clone https://github.com/kevinluosl/mini-bot.git
cd mini-bot
source "$HOME/esp/esp-idf/export.sh"  # 按实际安装路径调整
python3 scripts/build.py mini-bot --name mini-bot \
  --language zh-CN --wake-word wn9_xiaomingtongxue_tts2
python3 -B -m unittest discover -s scripts/tests
python3 -B verify_startup.py
```

固件和唤醒资源在 `build/` 下，烧录地址以 `build/flash_args` 为准。构建脚本不会自动烧录；`verify_startup.py` 只做静态检查，不代替上板测试。

本仓库包含完整的固件项目源码（`main/`、`components/`）、分区配置、构建脚本和测试。为实现上述实验目标，我们重新实现了机器人侧的视觉、互动与云端连接逻辑，也重写了配套测试服务；**本仓库不包含服务端源码**。

## 测试服务与开发者群

语音测试服务的固定地址是 **`http://43.142.28.216:8765/`**。此地址是测试服务入口，不是 OV-2.5 视觉接口，也不代表已获授权。请扫码加入 **LLaVA-OV-2.5 开发者交流群**，由群内提供个人使用的授权地址和授权码。请勿提交或公开授权信息。

<img src="docs/images/llava-ov25-group-qr.png" alt="LLaVA-OV-2.5 开发者交流群二维码" width="280">

> 这是联网实验项目。开启观察会向配置的服务器发送摄像头图像；请征得画面中人员同意，避免拍摄敏感内容，不用时停止观察。

## 致谢

- 感谢 [小智语音机器人（78/xiaozhi-esp32）](https://github.com/78/xiaozhi-esp32)。语音唤醒、联网和对话链路参考了小智的实现；直接使用的上游代码保留原有版权与许可。
- 感谢 [瓦楞机器人](https://wcnucfj0ewmg.feishu.cn/docx/OpqodJBo4oROIfxO8tUcxvzvn5f)。机器人的 3D 打印外壳与硬件选型参考了它的方案。
- 感谢 [LLaVA-OneVision-2.5](https://github.com/EvolvingLMMs-Lab/LLaVA-OneVision-2.5) 团队开源视觉模型。

## 许可证

本仓库采用 [MIT License](LICENSE)。第三方组件按各自的许可证使用。
