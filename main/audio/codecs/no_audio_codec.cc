#include "no_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#define TAG "NoAudioCodec"

NoAudioCodec::~NoAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}

NoAudioCodecDuplex::NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}


NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // Create a new channel for MIC
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din, i2s_std_slot_mask_t mic_slot_mask){
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = spk_slot_mask,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // Create a new channel for MIC
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.slot_cfg.slot_mask = mic_slot_mask;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

int NoAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (write_buffer_.size() < static_cast<size_t>(samples)) {
        write_buffer_.resize(samples);
    }

    // 使用整数运算代替 pow() 浮点计算：output_volume_ (0-100) -> volume_factor (0-65536)
    int32_t vol = output_volume_;
    int32_t volume_factor = (vol * vol * 65536) / 10000;
    for (int i = 0; i < samples; i++) {
        int64_t temp = int64_t(data[i]) * volume_factor;
        if (temp > INT32_MAX) {
            write_buffer_[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
            write_buffer_[i] = INT32_MIN;
        } else {
            write_buffer_[i] = static_cast<int32_t>(temp);
        }
    }

    size_t bytes_written = 0;
    // 避免驱动异常时占住播放/电源锁，也不要因一次 TX 写入失败让整板重启。
    constexpr uint32_t kWriteTimeoutMs = 250;
    const auto err = i2s_channel_write(tx_handle_, write_buffer_.data(), samples * sizeof(int32_t),
                                       &bytes_written, kWriteTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S TX 写入失败 err=%s written=%u requested=%u", esp_err_to_name(err),
                 static_cast<unsigned>(bytes_written / sizeof(int32_t)), static_cast<unsigned>(samples));
    }

    // 把参考信号存进环形缓冲。
    //
    // 关键：参考必须取 write_buffer_（已乘音量）而不是入参 data。
    // AEC 要减掉的是“喇叭实际发出的声音”，若拿未乘音量的原始 PCM 当参考，
    // 幅度对不上，AEC 反而会把残余回声当成新人声，效果比不开还差。
    if (input_reference_) {
        AppendReference(write_buffer_.data(), bytes_written / sizeof(int32_t));
    }
    return bytes_written / sizeof(int32_t);
}

void NoAudioCodec::AppendReference(const int32_t* src, int n) {
    // src 是 Q16 定点（data * volume_factor），>>16 拿回 int16 幅度。
    // 必须用已乘音量的值：AEC 要减的是喇叭实际发出的声音。
    const int in_rate = output_sample_rate_ > 0 ? output_sample_rate_ : input_sample_rate_;
    const int out_rate = input_sample_rate_ > 0 ? input_sample_rate_ : in_rate;

    if (n <= 0) {
        return;
    }

    // 整个重采样 + 入队过程持锁：Write（播放任务）与 Read（采集任务）并发访问。
    std::lock_guard<std::mutex> lock(ref_mutex_);

    if (in_rate == out_rate) {
        for (int i = 0; i < n; i++) {
            PushReferenceSample(static_cast<int16_t>(src[i] >> 16));
        }
        return;
    }

    // 采样率不同时重采样：本板播放 24k、麦 16k。AFE 要求同采样率的交织数据，
    // 不重采样会让参考被拉慢 1.5 倍、与回声彻底错位（还引入伪影）。
    // 用线性插值而非抽样：抽样丢高频，参考波形偏离大，残留回声会干扰唤醒。
    const double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    double pos = ref_resample_pos_;
    // 要求 pos < n-1 才能取到 src[i0+1] 做插值，末尾留一个样本给下一帧衔接
    while (pos < static_cast<double>(n - 1)) {
        const int i0 = static_cast<int>(pos);
        const double frac = pos - static_cast<double>(i0);
        const double a = static_cast<double>(src[i0] >> 16);
        const double b = static_cast<double>(src[i0 + 1] >> 16);
        PushReferenceSample(static_cast<int16_t>(a + (b - a) * frac));
        pos += step;
    }
    // 负数表示下一帧开头还需要补这么多才能对齐输出点，这是正常的跨帧衔接。
    ref_resample_pos_ = pos - static_cast<double>(n);
}

void NoAudioCodec::PushReferenceSample(int16_t sample) {
    if (ref_buffer_.empty()) {
        // 惰性分配：未启用回采的板子不会白白占 4KB 内存。
        // 2048 样本 @16kHz 约 128ms，足够覆盖一次 I2S DMA 写入到被麦克风拾取的延迟。
        // 容量必须是 2 的幂：下面用 & (size-1) 做环形索引。
        ref_buffer_.resize(kRefBufferSize);
    }

    // 缓冲区写了没人读时（丢最旧的，保最新的），把读计数推进到只保留最近一整个缓冲。
    //
    // 为何必须丢旧的：AEC 依赖的是“此刻喇叭在放什么”，
    // 若保住最旧数据，参考永远是 128ms 前的旧音频，
    // 拿不相关的参考去减麦，反把用户人声当残响抹掉（初版写反过，导致播放时完全唤不醒）。
    if (ref_write_cnt_ - ref_read_cnt_ >= kRefBufferSize) {
        ref_read_cnt_ = ref_write_cnt_ - kRefBufferSize + 1;
    }

    // 环形写入：索引取模，自然回绕，**绝不搬运数据**。
    // 旧实现这里在写满后 memmove 整段有效窗口，
    // 消费者一旦停止（配网模式）就退化成每样本搬 2KB，是配网播报卡顿的根因。
    ref_buffer_[ref_write_cnt_ & (kRefBufferSize - 1)] = sample;
    ++ref_write_cnt_;
}

int NoAudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    constexpr uint32_t kReadTimeoutMs = 200;

    // samples 是“总 int16 数量”（见 AudioService::ReadAudioData 里的
    // data.resize(samples * input_channels())），所以实际要读的采样帧数是
    // samples / channels。以前恒为 1 时两者相等，开参考后就不再相等了。
    const int channels = input_channels_ < 1 ? 1 : input_channels_;
    const int frames = samples / channels;
    if (frames <= 0) {
        return 0;
    }

    std::vector<int32_t> bit32_buffer(frames);
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), frames * sizeof(int32_t), &bytes_read, kReadTimeoutMs) != ESP_OK) {
        return 0;
    }

    const int got = bytes_read / sizeof(int32_t);

    // 一次性把本帧需要的参考值取出来，而不是每样本加一次锁。
    //
    // 每帧 160 个样本，若逐个加锁就要加 160 次；批量取只需 1 次，
    // 临界区总长度也更短。AEC 对时延敏感，高频路径不该把时间花在锁竞争上。
    //
    // 注意：复用 ref_scratch_ 而不每帧新建 vector，且每个位置都必须显式赋值。
    // 不能留下“参考不足就不写”的空洞，否则会残留上一帧的旧值，
    // 把上帧音频当成本帧参考交给 AEC，反而引入伪影。
    if (channels > 1) {
        ref_scratch_.resize(got);
        std::lock_guard<std::mutex> lock(ref_mutex_);
        const bool has_ref = input_reference_;
        for (int i = 0; i < got; i++) {
            // 环形读：有未读数据就取，按取模索引读（不搬数据）。
            // 没数据就填 0，保证每个位置都显式赋值，不残留上帧旧值。
            // 用 != 而不是 < 判断：uint32 回绕时大小关系会失效，而“不等”始终成立。
            if (has_ref && ref_read_cnt_ != ref_write_cnt_) {
                ref_scratch_[i] = ref_buffer_[ref_read_cnt_ & (kRefBufferSize - 1)];
                ++ref_read_cnt_;
            } else {
                ref_scratch_[i] = 0;
            }
        }
        // 本轮已追平（无待读数据）时把两个计数归零。
        //
        // 这只是防止计数器无限增长溢出 int，不参与索引正确性：
        // 环形缓冲只看两个计数的**差值**，同时减去相同值不影响未读数。
        // 用差值判断而不是“任一计数到上限”才是安全的；
        // 归零时若还剩未读数据就会丢参考。
        if (ref_read_cnt_ == ref_write_cnt_) {
            ref_read_cnt_ = 0;
            ref_write_cnt_ = 0;
        }
    }

    for (int i = 0; i < got; i++) {
        // 用可配置的移位量：本板 24-bit 左对齐麦克风必须用 16，
        // 写死 12 会把信号放大 16 倍、在播放时把 91% 的样本削成方波（详见头文件注释）。
        // 仍然保留限幅，防止异常电平（如拍打麦克风）溢出成噪声。
        int32_t value = bit32_buffer[i] >> input_shift_bits_;
        const int16_t mic = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
        if (channels == 1) {
            dest[i] = mic;
        } else {
            // 交织输出 (mic, ref)，与 AFE 的 input_format="MR" 对应。
            // 顺序必须是 M 在前 R 在后：afe_config_init 按字符串逐字符
            // 决定每个通道的角色，弄反会把麦克风当参考、参考当麦克风。
            dest[i * channels] = mic;
            dest[i * channels + 1] = ref_scratch_[i];
        }
    }

    // 返回值是写进 dest 的 int16 总数，调用方据此算帧数
    return got * channels;
}

void NoAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    AudioCodec::EnableInput(enable);
}

// 专为“单麦 + 无硬件回采 + 要开 AEC”的板子准备。
// 独立成类而不是给 NoAudioCodecSimplex 加默认参数：后者被多个板子共用，
// 把 input_channels 改成 2 会让那些无回采需求的板子上行变双声道。
NoAudioCodecSimplexAec::NoAudioCodecSimplexAec(
    int input_sample_rate, int output_sample_rate,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
    gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
    : NoAudioCodecSimplex(input_sample_rate, output_sample_rate,
                         spk_bclk, spk_ws, spk_dout, mic_sck, mic_ws, mic_din) {
    // 父类只创建 I2S channel，不碰输入数据路径，所以在构造后改这两个字段是安全的。
    input_reference_ = true;
    // 1 路麦 + 1 路软件参考，AFE 据此拼出 input_format="MR" 并自动置 aec_init。
    input_channels_ = 2;
    // 24-bit 左对齐麦必须用 16（详见头文件注释）。
    input_shift_bits_ = 16;
}

void NoAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}

// Delegating constructor: calls the main constructor with default slot mask
NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_din)
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout, I2S_STD_SLOT_LEFT, mic_sck, mic_din) {
    // All initialization is handled by the delegated constructor
}

NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(1), I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    tx_chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    tx_chan_cfg.auto_clear_after_cb = true;
    tx_chan_cfg.auto_clear_before_cb = false;
    tx_chan_cfg.intr_priority = 0;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, NULL));


    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = spk_slot_mask,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));
#if SOC_I2S_SUPPORTS_PDM_RX
    // Create a new channel for MIC in PDM mode
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(0), I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle_));
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        /* The data bit-width of PDM mode is fixed to 16 */
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = mic_sck,
            .din = mic_din,

            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_rx_cfg));
#else
    ESP_LOGE(TAG, "PDM is not supported");
#endif
    ESP_LOGI(TAG, "Simplex channels created");
}

int NoAudioCodecSimplexPdm::Read(int16_t* dest, int samples) {
    size_t bytes_read;

    // PDM 解调后的数据位宽为 16 位，直接读取到目标缓冲区
    if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }

    samples = bytes_read / sizeof(int16_t);
    if (input_gain_ > 0) {
        int gain_factor = (int)input_gain_;
        for (int i = 0; i < samples; i++) {
            int32_t amplified = dest[i] * gain_factor;
            dest[i] = (amplified > INT16_MAX) ? INT16_MAX : (amplified < -INT16_MAX) ? -INT16_MAX : (int16_t)amplified;
        }
    }
    return samples;
}
