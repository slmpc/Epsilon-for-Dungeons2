// ============================================================================
//  FeaturePanel.h — 功能模块的 ImGui 面板
//
//  在现有的 Data Overlay 里增加一个 "Modules" 页签, 用于:
//    * 按分类折叠显示全部模块
//    * 开关模块(带热键)
//    * 就地编辑每个模块的设置(按 Setting 的实际类型选控件)
//
//  重要限制: 这张面板的画布是**游戏进程内**的 ImGui, 字体是内置的 ASCII 位图
//  字体 —— 中文会渲染成 '?'。所以面板文案一律英文(与 Overlay.cpp 的既有约定
//  一致)。控制台/管道输出那边不受影响, 仍然可以写中文。
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
