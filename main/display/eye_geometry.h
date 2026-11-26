#pragma once

#include <cmath>
#include <cstdlib>  // std::abs(int)：整型取绝对值（<cmath> 只保证浮点重载）

// 与 LVGL 无关的扫描行几何，供 OLED 绘制和主机测试共用。
namespace eye_geometry {

// 圆角矩形按行光栅化：中间为直边，顶底用半径 r 的圆弧。
inline int RoundedRectHalfWidth(int width, int height, int row, int radius) {
    if (width < 2 || height <= 0 || row < 0 || row >= height) return 0;

    const int half_width = width / 2;
    int r = radius;
    if (r < 0) r = 0;
    if (r > half_width) r = half_width;   // 宽度不够时退化为胶囊
    if (r > height / 2) r = height / 2;   // 上下圆角不能重叠

    // 行中心到最近水平边的距离。用行中心而非行索引，保证上下对称。
    const float top_distance = static_cast<float>(row) + 0.5f;
    const float bottom_distance = static_cast<float>(height) - (static_cast<float>(row) + 0.5f);
    const float dy = top_distance < bottom_distance ? top_distance : bottom_distance;

    // 直边区：直接整宽返回。
    if (r == 0 || dy >= static_cast<float>(r)) return half_width;

    // 圆角区：直边半宽 + 半径 r 的圆弧外凸量
    const float offset = static_cast<float>(r) - dy;
    const float bulge = std::sqrt(static_cast<float>(r) * r - offset * offset);
    const int half = static_cast<int>(std::lround((half_width - r) + bulge));
    return half < half_width ? half : half_width;
}

// 眼睛圆角半径：统一取高度的 35%。
//
// 为何随高度缩放而不写死：眨眼/情绪切换时框高在 1~36px 之间变化，
// 写死半径会让矮眼睛（如眨眼 4px 高）的圆角超过半高、退化为胶囊，
// 形状跳变。随高度缩放后形状连续变化。
inline int EyeCornerRadius(int width, int height) {
    if (width < 2 || height <= 0) return 0;
    int r = static_cast<int>(std::lround(static_cast<float>(height) * 0.35f));
    if (r < 1) r = 1;
    if (r > width / 2) r = width / 2;
    if (r > height / 2) r = height / 2;
    return r;
}

// 主眼型的每行半宽（圆角矩形）：绘制侧统一入口，避免调用处重复算半径。
inline int EyeHalfWidth(int width, int height, int row) {
    return RoundedRectHalfWidth(width, height, row, EyeCornerRadius(width, height));
}

// 半径取半高；宽高相等时为正圆，眨眼压低时保留横向宽度。
inline int CircleHalfWidth(int width, int height, int row) {
    if (width < 2 || height <= 0) return 0;
    // 半径取半高：宽==高时即正圆；宽>高时仍能得到上下完全对称的圆
    return RoundedRectHalfWidth(width, height, row, height / 2);
}

// 超椭圆 |x/rx|^n + |y/ry|^n = 1 的每行半宽。
// n=2 是椭圆；正常眼高 n=2.4 使整个顶部/底部自然变平，
// 而不是在椭圆外沿追加一条横线。小眼高连续回到 n=2，避免眨眼时突变。
//
// 已不再用于主眼型（被上面的 RoundedRectHalfWidth 取代）。
// 保留原因：仅作为对比与回退手段（万一以后想回到椭圆风格），
// 生产绘制路径不应再调用它——主机测试会断言绘制走 CircleHalfWidth（圆形）。
inline int HalfWidth(int width, int height, int row) {
    if (width < 2 || height <= 0 || row < 0 || row >= height) return 0;

    const float rx = width / 2.0f;
    const float blend = height <= 12 ? 0.0f
                       : height >= 24 ? 1.0f : (height - 12) / 12.0f;
    const float exponent = 2.0f + 0.4f * blend;
    // 整数镜像距离后再归一化，上下对称行得到完全相同的输入。
    const int distance = std::abs(2 * row + 1 - height);
    const float ny = static_cast<float>(distance) / height;
    const float s = exponent == 2.0f ? 1.0f - ny * ny
                                    : 1.0f - std::pow(ny, exponent);
    if (s <= 0.0f) return 0;
    const float fraction = exponent == 2.0f ? std::sqrt(s)
                                           : std::pow(s, 1.0f / exponent);
    const int half = static_cast<int>(std::lround(rx * fraction));
    // 光栅取整不能超出对象：LVGL 无效区域以对象边界为准，越界会造成残影。
    return half < width / 2 ? half : width / 2;
}

static constexpr int kArcSamples = 160;

// 预计算弧线中心线上的采样点。
//
// 为何要单独拆出来：ArcSpan 需要知道每个采样点的位置，如果每列都重算一遍
// cos/sin，40 列就会重复 40 倍的三角运算（实测约 12880 次 cos/sin 每帧）。
// laughing 是每 tick 都重绘的热路径，拆出后三角运算只做 160 次，快约 40 倍。
// 返回实际写入的点数（kArcSamples+1 个）。
inline int ArcCenterline(float rx, float ry, int width, int height, int thick,
                         bool arc_down, float* out_x, float* out_y, int max_points) {
    if (out_x == nullptr || out_y == nullptr) return 0;
    if (rx <= 0.0f || ry <= 0.0f || thick <= 0 || max_points < 2) return 0;

    const float cx = width / 2.0f;
    const float r = thick / 2.0f;
    // ∪ 的圆心贴框顶（弧向下探），∩ 的圆心贴框底（弧向上探）。
    const float cy = arc_down ? r : (height - r);
    const float kPi = 3.14159265f;

    int n = 0;
    for (int i = 0; i <= kArcSamples && n < max_points; ++i, ++n) {
        const float u = static_cast<float>(i) / kArcSamples;
        // ∪ 用 180°→360°，∩ 用 0°→180°；统一 y = cy - ry*sin(theta)。
        const float theta = arc_down ? (kPi + kPi * u) : (kPi * u);
        out_x[n] = cx + rx * std::cos(theta);
        out_y[n] = cy - ry * std::sin(theta);
    }
    return n;
}

// 用已算好的中心线采样点，求某一列上圆头粗弧的上下边界。
inline bool ArcSpanFromPoints(const float* xs, const float* ys, int count,
                              int height, int thick, int col,
                              int* out_top, int* out_bottom) {
    if (xs == nullptr || ys == nullptr || out_top == nullptr || out_bottom == nullptr) return false;
    if (count <= 0 || height < 3 || thick <= 0 || col < 0) return false;

    const float r = thick / 2.0f;
    int top = height;
    int bottom = -1;
    // 每个中心线采样点扫一个半径 r 的圆，并集即圆头粗弧。
    for (int i = 0; i < count; ++i) {
        const float dx = (col + 0.5f) - xs[i];
        if (dx * dx >= r * r) continue;
        const float dy = std::sqrt(r * r - dx * dx);
        const int t = static_cast<int>(std::ceil(ys[i] - dy));
        const int b = static_cast<int>(std::floor(ys[i] + dy));
        if (t < top) top = t;
        if (b > bottom) bottom = b;
    }
    if (bottom < 0) return false;

    // 夹到对象内：越界像素若不夹住，LVGL 会在对象边界外留下残影。
    if (top < 0) top = 0;
    if (bottom > height - 1) bottom = height - 1;
    if (bottom < top) return false;

    *out_top = top;
    *out_bottom = bottom;
    return true;
}

// 单列便捷入口（内部重算中心线）。适合测试或偶尔绘制；
// 热路径请用 ArcCenterline + ArcSpanFromPoints 复用采样点。
inline bool ArcSpan(int width, int height, int col,
                    float rx, float ry, int thick, bool arc_down,
                    int* out_top, int* out_bottom) {
    float xs[kArcSamples + 1];
    float ys[kArcSamples + 1];
    const int n = ArcCenterline(rx, ry, width, height, thick, arc_down, xs, ys, kArcSamples + 1);
    return ArcSpanFromPoints(xs, ys, n, height, thick, col, out_top, out_bottom);
}

// 笑眼弧的参数推导：由框尺寸反推 arc 半径，保证粗弧刚好填满框且不越界。
//   框宽 w → rx = (w - thick)/2（两端各占 r 半径）
//   框高 h → ry = h - thick（∪ 从顶部圆心下探到底边）
inline float SmileArcRx(int width, int thick) {
    const float v = (width - thick) / 2.0f;
    return v > 1.0f ? v : 1.0f;
}
inline float SmileArcRy(int height, int thick) {
    const float v = static_cast<float>(height - thick);
    return v > 1.0f ? v : 1.0f;
}

// 眼泪水滴的每行半宽。
//
// 为何单独抽成函数：泪滴是「哭」的决定性符号（单色屏上靠眼睛形状
// 无法区分哭与难过），必须能单独验证它的像素形状，而不是只能看截图。
//
// 形状：上尖下圆的泪滴（不是方块）。
//   v = (row + 0.5) / height，归一化到 0..1
//   v < split  → 半宽按 sqrt 张开（顶部收成尖角）
//   v >= split → 按圆弧收底（底部圆润）
//
// split 随高度自适应为 1 - 2/H：总是把最后两行留给圆底。
// 为何不写固定 0.75：H=5 时 0.75 换算到末行只有 0.9，圆的弧长太短，
// 最后一行仍是最宽值 —— 泪滴会被截断成一根小柱子，看不出是水珠。
inline int TearHalfWidth(int row, int height, int max_half) {
    if (height <= 0 || max_half <= 0 || row < 0 || row >= height) return 0;

    float split = 1.0f - 2.0f / static_cast<float>(height);
    if (split < 0.5f) split = 0.5f;
    if (split > 0.9f) split = 0.9f;

    const float v = static_cast<float>(row) + 0.5f;
    const float norm = v / static_cast<float>(height);
    float factor;
    if (norm < split) {
        factor = std::sqrt(norm / split);
    } else {
        const float u = (norm - split) / (1.0f - split);
        factor = std::sqrt(1.0f - u * u);
    }
    const int half = static_cast<int>(std::lround(max_half * factor));
    if (half < 0) return 0;
    return half > max_half ? max_half : half;
}

// 按列对圆角矩形施加线性位移；两眼镜像使悲伤表情的外眼角下垂。
inline bool TiltedRoundedRectSpan(int width, int height, int col, int shear,
                                  bool mirror, int radius, int* out_top,
                                  int* out_bottom) {
    if (out_top == nullptr || out_bottom == nullptr) return false;
    if (width < 4 || height < 3 || col < 0 || col >= width) return false;

    int r = radius;
    if (r < 0) r = 0;
    if (r > width / 2) r = width / 2;
    if (r > height / 2) r = height / 2;

    // 本列距最近左右边缘的距离（列中心计）。
    const float left_distance = static_cast<float>(col) + 0.5f;
    const float right_distance = static_cast<float>(width) - (static_cast<float>(col) + 0.5f);
    const float dx = left_distance < right_distance ? left_distance : right_distance;

    int top = 0;
    int bottom = height - 1;
    if (r > 0 && dx < static_cast<float>(r)) {
        // 圆角区：上下各收缩 corner 像素。
        const float offset = static_cast<float>(r) - dx;
        const float inside = std::sqrt(static_cast<float>(r) * r - offset * offset);
        const int corner = static_cast<int>(std::lround(static_cast<float>(r) - inside));
        top = corner;
        bottom = height - 1 - corner;
    }

    // 倾斜：左眼（mirror=false）外侧希望最低 → shift 为正（屏幕 y 向下）。
    const float center = width / 2.0f;
    const float t = (static_cast<float>(col) + 0.5f - center) / center;  // [-1, 1]
    const float dir = mirror ? 1.0f : -1.0f;
    const int shift = static_cast<int>(
        std::lround(static_cast<float>(shear) * dir * t));
    top += shift;
    bottom += shift;

    // 夹到对象内：剪切后两端会超出上下边；越界像素若不夹住，
    // LVGL 会在对象外的无效区域留下残影。
    if (top < 0) top = 0;
    if (bottom > height - 1) bottom = height - 1;
    if (bottom < top) return false;

    *out_top = top;
    *out_bottom = bottom;
    return true;
}

inline bool TiltedOvalSpan(int width, int height, int col, int shear, bool mirror,
                           int* out_top, int* out_bottom) {
    if (out_top == nullptr || out_bottom == nullptr) return false;
    if (width < 4 || height < 3 || col < 0 || col >= width) return false;

    const float rx = width / 2.0f;
    const float t = (col + 0.5f - rx) / rx;  // 列中心归一化到 [-1, 1]
    const float s = 1.0f - t * t;
    if (s <= 0.0f) return false;  // 超出椭圆宽度（两端无像素）

    // 左眼（mirror=false）外侧是 t=-1，希望那里最低 → shift 为正（屏幕 y 向下）。
    const float dir = mirror ? 1.0f : -1.0f;
    const float shift = static_cast<float>(shear) * dir * t;

    const float cy = height / 2.0f;
    const float hh = cy * std::sqrt(s);
    int top = static_cast<int>(std::lround(cy - hh + shift));
    int bottom = static_cast<int>(std::lround(cy + hh + shift));

    // 夹到对象内：剪切后两端会超出上下边；越界像素若不夹住，
    // LVGL 会在对象外的无效区域留下残影。
    if (top < 0) top = 0;
    if (bottom > height - 1) bottom = height - 1;
    if (bottom < top) return false;

    *out_top = top;
    *out_bottom = bottom;
    return true;
}

}  // namespace eye_geometry
