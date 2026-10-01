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
    // 函数内静态单例: 注入体的加载/卸载时机不可控, 全局对象的构造析构顺序
    // 在 DllMain 线程里很容易出问题。
    [[nodiscard]] static ModuleManager& instance();

    ModuleManager() = default;
    ~ModuleManager() = default;
    ModuleManager(ModuleManager const&) = delete;
    ModuleManager& operator=(ModuleManager const&) = delete;

    // ---------------------------------------------------------------- 注册
    // 接管模块所有权。重复名字返回 false 且不覆盖 —— 静默覆盖会让"注册了两个
    // 同名模块却只有一个生效"变成极难查的问题。
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
    // 全部关掉。只切开关, 不触发 reset()(不动键位/设置)。
    void disableAll();

    // ---------------------------------------------------------------- 事件分发
    // 按键事件。vk 是 Windows 虚拟键码, pressed = true 为按下, false 为抬起。
    // 返回 true 表示这次按键已被功能模块处理, 宿主通常不应再把它交给游戏 ——
    // 否则"用 F5 开关模块"会同时触发游戏里绑在 F5 的功能。
    //
    // 返回 true 的两种情形: ① 有模块绑定这个键并因此切换了开关 ② 有模块的
    // onKeyEvent 明确消费了它。未绑定且无人消费 -> false。
    //
    // Toggle 语义: 只在按下时生效一次。Hold 语义: 按下启用、抬起禁用。
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
    // 按注册顺序保存, 保证遍历顺序稳定(unordered_map 的顺序会让面板里模块
    // 顺序每次启动都在变)。
    std::vector<Module*> modules_;
    std::vector<std::unique_ptr<Module>> owned_;
    // 名字(小写规范化) -> 模块, 便于大小写不敏感查询。
    std::unordered_map<std::string, Module*> byName_;

    [[nodiscard]] static std::string normalizeKey(std::string_view name);
};

} // namespace epsilon::feature
