// ============================================================================
//  FeaturePanel.h — 功能模块的 ImGui 面板(Overlay 里的 "Modules" 页签)。
//
//  ★ 画布是**游戏进程内**的 ImGui, 字体只有内置的 ASCII 位图字体 —— 面板文案
//  一律纯 ASCII 英文, 中文会渲染成 '?'。日志与管道输出不受此限。
//  绘制结构、改键捕获与各控件细节见 docs/features/ui.md
// ============================================================================
#pragma once

#include <cstdint>

namespace epsilon::feature {

class FeaturePanel {
public:
    // 画出整个 Modules 页签的内容(不含 BeginTabItem, 由调用方管)。
    // 只在 ImGui 帧内调用。
    static void draw();

    // 处理按键捕获: 面板里点过"改键"之后, 下一个按下的键会被吃掉并绑定。
    // 返回 true 表示这次按键被面板消费了, 调用方不要再分发给模块。
    //
    // 必须在模块的按键分发**之前**调用 —— 否则改键时会把新键同时触发一次
    // 模块开关, 表现为"改完键模块自己开了"。
    static bool captureKey(int32_t vk, bool pressed);

    // 是否正在等待用户按键。
    [[nodiscard]] static bool isCapturing();
};

} // namespace epsilon::feature
