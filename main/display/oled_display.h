#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

class OledDisplay : public LvglDisplay {
// 成员改为 protected：板级子类（如 DeepbotEyesDisplay）需要在 SetupUI() 里隐藏默认控件
// 并叠加自定义画面。仅调整可见性，基类行为与布局完全不变，原 UI 可直接回退使用。
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* emotion_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();
    void SetupUI_128x32();

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
    virtual bool IsMonochrome() const override { return true; }
    void SetPowerSaveMode(bool on) override;
};

#endif  // OLED_DISPLAY_H
