#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// 麦克风 RX 和功放 TX 分别使用独立的 I²S 控制器。
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE true
#define AUDIO_I2S_METHOD_SIMPLEX

// I²S 麦克风。
#define AUDIO_I2S_MIC_GPIO_WS GPIO_NUM_1
#define AUDIO_I2S_MIC_GPIO_SCK GPIO_NUM_2
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_40

// I²S 功放。
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_41
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_42
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_48

#define BOOT_BUTTON_GPIO GPIO_NUM_0

// OV2640 DVP 并口接线。
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_XCLK GPIO_NUM_15
#define CAMERA_PIN_SIOD GPIO_NUM_4
#define CAMERA_PIN_SIOC GPIO_NUM_5
#define CAMERA_PIN_D7 GPIO_NUM_16
#define CAMERA_PIN_D6 GPIO_NUM_17
#define CAMERA_PIN_D5 GPIO_NUM_18
#define CAMERA_PIN_D4 GPIO_NUM_12
#define CAMERA_PIN_D3 GPIO_NUM_10
#define CAMERA_PIN_D2 GPIO_NUM_8
#define CAMERA_PIN_D1 GPIO_NUM_9
#define CAMERA_PIN_D0 GPIO_NUM_11
#define CAMERA_PIN_VSYNC GPIO_NUM_6
#define CAMERA_PIN_HREF GPIO_NUM_7
#define CAMERA_PIN_PCLK GPIO_NUM_13
// 使用 16 MHz XCLK，避免较高 PCLK 下采集到不完整的 JPEG。
#define XCLK_FREQ_HZ 16000000

// SH1106 单色 OLED。GPIO21/47 不与摄像头数据线或启动绑带脚冲突。
#define DISPLAY_SDA_PIN GPIO_NUM_21
#define DISPLAY_SCL_PIN GPIO_NUM_47
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// 舵机独立供电、与主板共地；开机不输出 PWM，首次动作才启用。
#define SERVO_1_GPIO GPIO_NUM_38
#define SERVO_2_GPIO GPIO_NUM_39
#define SERVO_PWM_FREQ_HZ 50

// 限制 PWM 脉宽，避免超出机械行程引起堵转。
#define SERVO_MIN_PULSE_US 1000
#define SERVO_MAX_PULSE_US 2000

// 俯仰舵机安装方向相反，镜像脉宽但保留逻辑方向：大于 90° 为低头。
#define SERVO_2_INVERT 1

// 逻辑角度 90° 为中位；横向角度增大为右转。
#define SERVO_CENTER_ANGLE 90

#define SERVO_1_MAX_OFFSET_DEG 60

// 俯仰机构的上下行程不对称，抬头和低头分别限位。
#define SERVO_2_MAX_OFFSET_UP_DEG 60
#define SERVO_2_MAX_OFFSET_DOWN_DEG 40

// 通用接口使用较小的俯仰限位。
#define SERVO_2_MAX_OFFSET_DEG SERVO_2_MAX_OFFSET_DOWN_DEG

// 通用横轴范围。
#define SERVO_MAX_OFFSET_DEG SERVO_1_MAX_OFFSET_DEG
#define SERVO_MIN_ANGLE (SERVO_CENTER_ANGLE - SERVO_1_MAX_OFFSET_DEG)  // 30
#define SERVO_MAX_ANGLE (SERVO_CENTER_ANGLE + SERVO_1_MAX_OFFSET_DEG)  // 150

// GPIO3/46 为启动绑带脚，传感器必须使用 3.3 V 逻辑，不能影响启动电平。
#define TOUCH_GPIO GPIO_NUM_14            // TTP223 电容触摸，输出高 = 被触摸
#define INFRARED_RIGHT_GPIO GPIO_NUM_46   // 机器人右侧红外，触发时头往右转
#define INFRARED_LEFT_GPIO GPIO_NUM_3     // 机器人左侧红外，触发时头往左转

// 红外模块默认低有效；反相模块可设为 0。
#define INFRARED_ACTIVE_LOW 1

// 红外反应使用相对机械中位的固定幅度，不叠加上一次定向动作。
#define REACT_IR_TURN_DEG 45

#define REACT_IR_TILT_DEG 40

// 触摸时围绕中位交替轻摆。
#define REACT_TOUCH_SWING_X_DEG 20  // 左右 ±20°
#define REACT_TOUCH_SWING_Y_DEG 12  // 俯仰 ±12°
// 每拍最多转 2°，横轴反向 40° 约需 400 ms。
#define REACT_TOUCH_SWING_MS 460

// 7 位地址；有的模块用含读写位的 0x78 标注同一地址。
#define DISPLAY_I2C_ADDR 0x3C
// 400 kHz 让整屏刷新能跟上动画节拍。
#define DISPLAY_I2C_SCL_HZ (400 * 1000)

// 这块屏需要 SH1106 逐页寻址和列偏移，不能用 SSD1306 驱动代替。
#define DISPLAY_USE_SH1106 1

// 仅控制本地画面预览，不关闭摄像头或云端观察。
#define CAMERA_PREVIEW_ON_DISPLAY 0

// 在像素数据层反色；这块面板不响应反色命令。
#define DISPLAY_PIXEL_INVERT 1

#endif  // _BOARD_CONFIG_H_
