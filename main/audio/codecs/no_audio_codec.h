#ifndef _NO_AUDIO_CODEC_H
#define _NO_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2s_pdm.h>
#include <mutex>

class NoAudioCodec : public AudioCodec {
protected:
    std::mutex data_if_mutex_;
    // 复用 I2S 32-bit 转换缓冲区，避免每帧播放时反复在堆上分配释放造成抖动
    std::vector<int32_t> write_buffer_;

    // ---- 软件回采（AEC 参考信号）----
    // 本板无硬件回采（喇叭只接 I2S TX、麦单独走 I2S RX），AEC 拿不到“喇叭放了什么”。
    // 解法：把即将写给 I2S 的 PCM 存一份到环形缓冲，读麦时交织进第二通道，
    // 让 AFE 把它当作 input_format 里的 R（reference）。抄自官方 esp32-s3-box-lite。
    std::vector<int16_t> ref_buffer_;
    // 写入/读取的累计计数（不是下标）。
    //
    // 为什么用累计计数而不是下标：数组索引 = 计数 & (SIZE-1)，
    // 天然回绕，不需要 memmove 搬家。
    //
    // 为何用 uint32_t：配网模式下 Read 不消费，两个计数会一直同步增长
    // （Push 会把 read 推着走），若用 int 约 37 小时就会进入负区间，
    // 而对负数做位与是实现定义行为。uint32_t 的模运算回绕是良性的：
    //   “未读数 = write - read” 即使跨回绕也算得对。
    //
    // 历史（重要，别改回线性缓冲）：
    // 初版是「下标 + 写满就 memmove 把有效窗口搬到头部」的线性缓冲。
    // 正常对话时 Read 一直在消费，缓冲不会满，看不出问题。
    // 但配网模式下 AFE 不跑（EnableWakeWordDetection(false)），Read 一次都不调用，
    // ref_read_cnt_ 恒不动 → 缓冲永远处于满状态 → **每写入一个样本都 memmove 约 2KB**。
    // 一帧 960 个参考样本 × 2KB ≈ 2.15MB 搬运，实测 185ms/帧，
    // 而一帧音频只有 60ms，于是「进入配网模式」这句话播得一顿一顿。
    // 环形缓冲把这个路径降为 O(1)，无任何搬运。
    uint32_t ref_write_cnt_ = 0;
    uint32_t ref_read_cnt_ = 0;
    // ref_buffer_ 固定容量，必须是 2 的幂（下面用位与做索引）。
    static constexpr int kRefBufferSize = 2048;
    // ref_buffer_ 专属锁。不能复用 data_if_mutex_：Write 里的 i2s_channel_write
    // 是阻塞调用，与 Read 共用一把锁会让采集被播放阻塞，导致麦数据丢失。
    std::mutex ref_mutex_;
    // 跨帧保留的重采样相位（见 AppendReference）。
    // 播放 24000Hz、麦克风 16000Hz，不重采样会让参考与回声错位 1.5 倍。
    double ref_resample_pos_ = 0.0;

    // I2S 32-bit 定点值转 int16 的右移位量。
    // 本板麦是 24-bit 左对齐在 32-bit slot，必须用 16；默认 12 是原实现值，
    // 它会把信号放大 16 倍，播放时削掉 91% 样本成方波，导致唤醒完全失效。
    // 保留 12 作默认是为了不影响其它共用本基类的板子，需修正的板子在子类里改成 16。
    int input_shift_bits_ = 12;

    // Read 里攒参考值用的临时缓冲，复用以避免每帧一次堆分配。
    std::vector<int16_t> ref_scratch_;

    // 把一批待播放的 PCM（已乘音量）压成麦克风采样率的参考信号。
    void AppendReference(const int32_t* src, int n);

    // 往参考环形缓冲塞入一个样本，带溢出保护。
    void PushReferenceSample(int16_t sample);

    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

public:
    virtual ~NoAudioCodec();
};

class NoAudioCodecDuplex : public NoAudioCodec {
public:
    NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
};

class NoAudioCodecSimplex : public NoAudioCodec {
public:
    NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din);
    NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din, i2s_std_slot_mask_t mic_slot_mask);
};

class NoAudioCodecSimplexAec : public NoAudioCodecSimplex {
public:
    // 专为"单麦 + 无硬件回采 + 想开 AEC"的板子准备。
    // 独立成一个类而不是给 NoAudioCodecSimplex 加默认参数，
    // 是为了不影响其它已经在用 NoAudioCodecSimplex 的板子（它们没有回采需求）。
    NoAudioCodecSimplexAec(int input_sample_rate, int output_sample_rate,
                           gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                           gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din);
};

class NoAudioCodecSimplexPdm : public NoAudioCodec {
public:
    NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck,  gpio_num_t mic_din);
    NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck,  gpio_num_t mic_din);
    int Read(int16_t* dest, int samples);
};

#endif // _NO_AUDIO_CODEC_H
