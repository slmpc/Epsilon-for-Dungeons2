// ============================================================================
//  ModuleManager.h — 模块注册表与事件分发
//
//  对应 Open-Epsilon 的 common/managers/ModuleManager.java。职责:
//    * 拥有全部模块(唯一所有权)
//    * 按名字/分类查询
//    * 把按键事件按 Toggle/Hold 语义分发给对应模块
//    * 把每帧回调转发给已启用的模块
//
//  与 Java 版的重要差别:
//
//    1. **没有 EventBus**。Java 版靠 @EventHandler 订阅按键/鼠标事件, 因为
//       Minecraft 提供了现成的事件总线。本项目没有, 所以改成"宿主喂事件":
//       KeyHook 从帧钩子/窗口过程拿到按键后调 dispatchKeyEvent。框架不知道
//       事件从哪来, 也不知道游戏是否在前台 —— 这些判断留给宿主。
//
//    2. **不持有游戏状态**。Java 版的 nullCheck() 会去问 Minecraft.getInstance()。
//       这里由宿主决定何时不该分发(比如游戏窗口失焦时), 框架只做分发。
//
//  这是**纯框架**: 注册表初始为空, 一个具体功能模块都没有。
// ============================================================================
#pragma once

#include "payload/feature/module/Module.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace epsilon::feature {

class ModuleManager {
public:
    // ---------------------------------------------------------------- 单例
    // 与项目其余部分保持一致用函数内静态单例(见 runtime.cpp 的 engine())。
    // 不用全局对象: 注入体的加载/卸载时机不可控, 全局对象的构造析构顺序
    // 在 DllMain 线程里很容易出问题。
    [[nodiscard]] static ModuleManager& instance();

    ModuleManager() = default;
    ~ModuleManager() = default;
    ModuleManager(ModuleManager const&) = delete;
    ModuleManager& operator=(ModuleManager const&) = delete;

    // ---------------------------------------------------------------- 注册
    // 接管模块所有权。
    //
    // 重复名字会被拒绝并返回 false —— 静默覆盖会让"我注册了两个同名模块却只有
    // 一个生效"变成极难查的问题。
    bool registerModule(std::unique_ptr<Module> module);

    // 便捷包装: 原地构造并注册。返回裸指针(所有权在管理器手里)。
    // 用法:
    //     auto& myModule = ModuleManager::instance().add<MyModule>();
    template <typename T, typename... Args>
    T& add(Args&&... args) {
        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = owned.get();
        registerModule(std::move(owned));
        return *raw;
    }

    [[nodiscard]] size_t moduleCount() const noexcept { return modules_.size(); }
    [[nodiscard]] bool   empty() const noexcept { return modules_.empty(); }

    // ---------------------------------------------------------------- 查询
    [[nodiscard]] Module* find(std::string_view name) const;
    [[nodiscard]] bool    has(std::string_view name) const;

    [[nodiscard]] std::vector<Module*> const& modules() const noexcept { return modules_; }
    [[nodiscard]] std::vector<Module*> modulesIn(Category category) const;

    // ---------------------------------------------------------------- 批量操作
    [[nodiscard]] size_t enabledCount() const noexcept;
    // 全部关掉。不触发 reset()(不动键位/设置), 只切开关。
    void disableAll();

    // ---------------------------------------------------------------- 事件分发
    // 按键事件。vk 是 Windows 虚拟键码, pressed = true 为按下, false 为抬起。
    // 返回 true 表示这次按键已被功能模块处理, 宿主通常不应再把它交给游戏 ——
    // 否则"用 F5 开关模块"会同时触发游戏里绑在 F5 的功能。
    //
    // 三种"已处理"的情形:
    //   * 某模块绑定到这个键并因此切换了开关(Toggle/Hold)
    //   * 某模块的 onKeyEvent 明确消费了它
    // 未绑定任何模块, 且没有模块消费 -> 返回 false。
    //
    // Toggle 语义: 只在按下时生效一次。
    // Hold   语义: 按下启用、抬起禁用。
    bool dispatchKeyEvent(int32_t vk, bool pressed);

    // 每帧回调。只转发给已启用的模块。
    void onFrame();

    // ---------------------------------------------------------------- 配置
    // 全部模块 <-> json。供 ConfigManager 使用。
    [[nodiscard]] nlohmann::json toJson() const;
    void fromJson(nlohmann::json const& in);

    // 任一模块自上次 markAllClean 后有改动。
    [[nodiscard]] bool anyDirty() const noexcept;
    void markAllClean() noexcept;

private:
    // 按 registration 顺序保存, 保证遍历顺序稳定 —— unordered_map 的遍历
    // 顺序会让"设置界面里模块顺序每次启动都在变"。
    std::vector<Module*> modules_;
    std::vector<std::unique_ptr<Module>> owned_;
    // 名字 -> 模块。名字做小写规范化, 便于大小写不敏感查询。
    std::unordered_map<std::string, Module*> byName_;

    [[nodiscard]] static std::string normalizeKey(std::string_view name);
};

} // namespace epsilon::feature
