#include "payload/feature/module/ModuleManager.h"

#include "common/Text.h"
#include "payload/Payload.h"

#include <cctype>

namespace epsilon::feature {

// 日志辅助在 epsilon::payload 命名空间里(见 Payload.h)。只引这三个函数,
// 不用 using namespace —— 那会把 payload 的全部符号灌进来。
using epsilon::payload::logInfo;
using epsilon::payload::logWarn;
using epsilon::payload::logError;

ModuleManager& ModuleManager::instance() {
    static ModuleManager mgr;
    return mgr;
}

std::string ModuleManager::normalizeKey(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        // 空格/下划线/连字符与大小写一律归一到同一个键, 手改配置不至于找不到。
        if (c == ' ' || c == '_' || c == '-') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// ===========================================================================
//  注册
// ===========================================================================
bool ModuleManager::registerModule(std::unique_ptr<Module> module) {
    if (!module) return false;

    const std::string key = normalizeKey(module->name());
    if (key.empty()) return false;

    if (byName_.contains(key)) {
        logWarn(fmt("功能模块 [{}] 已存在, 忽略重复注册", module->name()));
        return false;
    }

    Module* raw = module.get();
    byName_.emplace(key, raw);
    modules_.push_back(raw);
    owned_.push_back(std::move(module));
    return true;
}

// ===========================================================================
//  查询
// ===========================================================================
Module* ModuleManager::find(std::string_view name) const {
    auto it = byName_.find(normalizeKey(name));
    return it == byName_.end() ? nullptr : it->second;
}

bool ModuleManager::has(std::string_view name) const {
    return byName_.contains(normalizeKey(name));
}

std::vector<Module*> ModuleManager::modulesIn(Category category) const {
    std::vector<Module*> out;
    for (auto* m : modules_) {
        if (m && m->category() == category) out.push_back(m);
    }
    return out;
}

// ===========================================================================
//  批量操作
// ===========================================================================
size_t ModuleManager::enabledCount() const noexcept {
    size_t n = 0;
    for (auto* m : modules_) {
        if (m && m->isEnabled()) ++n;
    }
    return n;
}

void ModuleManager::disableAll() {
    // 遍历副本: onDisable 里可能去碰注册表, 直接遍历 modules_ 会让迭代器失效。
    const auto snapshot = modules_;
    for (auto* m : snapshot) {
        if (m && m->isEnabled()) m->setEnabled(false);
    }
}

// ===========================================================================
//  事件分发
// ===========================================================================
bool ModuleManager::dispatchKeyEvent(int32_t vk, bool pressed) {
    if (vk < 0) return false;

    // 第一步: 收集本次事件涉及的模块, 并判断是否有模块因此被启用。
    // ★ 必须先收集再改状态 —— 遍历中改 enabled 会让后面的模块看到已变化的
    // 世界, Toggle/Hold 的语义就乱了。
    std::vector<Module*> affected;
    bool anyEnabling = false;

    for (auto* m : modules_) {
        if (!m || m->keyBind() != vk) continue;

        if (m->bindMode() == Module::BindMode::toggle) {
            if (pressed) {
                if (!m->isEnabled()) anyEnabling = true;
                affected.push_back(m);
            }
        } else {   // hold
            if (pressed && !m->isEnabled()) {
                anyEnabling = true;
                affected.push_back(m);
            } else if (!pressed && m->isEnabled()) {
                affected.push_back(m);
            }
        }
    }

    if (affected.empty()) return false;

    // 第二步: 应用状态变更。
    // affected 里所有成员的方向一致(Toggle 只在按下时进入, Hold 要么全按下
    // 要么全抬起), 所以一个标志就够。
    const bool enabling = anyEnabling;
    for (auto* m : affected) {
        if (m->bindMode() == Module::BindMode::toggle) {
            m->toggle();
        } else {   // hold
            m->setEnabled(pressed);
        }

        // 按键切换也走通知: 用热键开关模块时同样需要看到反馈。
        logInfo(fmt("模块 [{}] 已{}", m->name(), enabling ? "启用" : "禁用"));
    }

    // 第三步: 让模块有机会自行消费这次按键。放在状态变更之后, 模块看到的
    // 就是自己最新的启用状态。
    // 先问受影响模块, 未消费再问全部已启用模块 —— 一个没绑键但需要观察按键
    // 的模块(如宏录制)也能收到事件。
    // 返回值的语义见头文件: 只要"有模块绑定了这个键并因此动作了"就算已处理。
    bool consumed = false;
    for (auto* m : affected) {
        if (m->onKeyEvent(vk, pressed)) consumed = true;
    }
    if (!consumed) {
        for (auto* m : modules_) {
            if (m && m->isEnabled() && m->onKeyEvent(vk, pressed)) consumed = true;
        }
    }
    // affected 非空即说明确实有模块绑了(或正在响应)这个键, 算已处理。
    return consumed || !affected.empty();
}

void ModuleManager::onFrame() {
    for (auto* m : modules_) {
        // 只驱动已启用的模块, 免得每份实现各自重判 isEnabled() 还漏掉。
        if (m && m->isEnabled()) m->onFrame();
    }
}

// ===========================================================================
//  配置
// ===========================================================================
nlohmann::json ModuleManager::toJson() const {
    nlohmann::json out = nlohmann::json::object();
    for (auto* m : modules_) {
        if (!m) continue;
        out[m->name()] = m->toJson();
    }
    return out;
}

void ModuleManager::fromJson(nlohmann::json const& in) {
    if (!in.is_object()) return;

    // 只应用**已注册**的模块: 配置里有、代码里没有的条目直接跳过。
    for (auto it = in.begin(); it != in.end(); ++it) {
        auto* m = find(it.key());
        if (!m) continue;
        if (!it.value().is_object()) continue;
        try {
            m->fromJson(it.value());
        } catch (std::exception const& e) {
            // 单个模块读失败不能连累其它模块, 更不能把异常放出去。
            logWarn(fmt("模块 [{}] 配置加载失败: {}", m->name(), e.what()));
        } catch (...) {
            logWarn(fmt("模块 [{}] 配置加载失败(未知错误)", m->name()));
        }
    }
}

bool ModuleManager::anyDirty() const noexcept {
    for (auto* m : modules_) {
        if (m && m->isDirty()) return true;
    }
    return false;
}

void ModuleManager::markAllClean() noexcept {
    for (auto* m : modules_) {
        if (m) m->markClean();
    }
}

} // namespace epsilon::feature
