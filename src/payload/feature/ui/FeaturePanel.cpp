// ============================================================================
//  FeaturePanel.cpp
// ============================================================================
#include "payload/feature/ui/FeaturePanel.h"

#include "common/Text.h"
#include "imgui.h"
#include "payload/Payload.h"
#include "payload/feature/GameContext.h"
#include "payload/feature/config/ConfigManager.h"
#include "payload/feature/module/ModuleManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>     // VK_ESCAPE

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace epsilon::feature {
namespace {

using epsilon::payload::logInfo;
using epsilon::payload::logWarn;

// 正在等待用户按键的改键目标。
//
// 早先这里只存了一个 KeybindSetting*, 而模块自身的"热键"那一行是临时构造的
// KeybindSetting —— 把它交给下一帧的按键回调就是一个悬垂指针。所以这里明确
// 区分两种目标: 模块热键, 或模块内部的键位设置。
struct CaptureTarget {
    enum class Kind { none, moduleKey, settingKey };
    Kind  kind = Kind::none;
    Module*         module = nullptr;    // Kind::moduleKey
    KeybindSetting* setting = nullptr;   // Kind::settingKey

    void clear() { kind = Kind::none; module = nullptr; setting = nullptr; }
    [[nodiscard]] bool active() const { return kind != Kind::none; }
    [[nodiscard]] bool is(Module const* m) const {
        return kind == Kind::moduleKey && module == m;
    }
    [[nodiscard]] bool is(KeybindSetting const* s) const {
        return kind == Kind::settingKey && setting == s;
    }
    void setModuleKey(Module* m) { kind = Kind::moduleKey; module = m; setting = nullptr; }
    void setSettingKey(KeybindSetting* s) { kind = Kind::settingKey; setting = s; module = nullptr; }
};
CaptureTarget gCapture;

// 字符串设置编辑用的暂存缓冲。ImGui 的 InputText 需要可写的 char*,
// 而我们的 StringSetting 内部是 std::string, 所以过一道缓冲。
// 用设置指针做键, 避免多个字符串设置互相串值。
struct StringEdit {
    StringSetting* setting = nullptr;
    char buf[256]{};
    bool active = false;
};
std::vector<StringEdit> gStringEdits;

StringEdit& stringEditFor(StringSetting& s) {
    for (auto& e : gStringEdits) {
        if (e.setting == &s) return e;
    }
    gStringEdits.push_back(StringEdit{});
    auto& e = gStringEdits.back();
    e.setting = &s;
    const std::string& v = s.value();
    const size_t n = v.size() < sizeof(e.buf) - 1 ? v.size() : sizeof(e.buf) - 1;
    std::memcpy(e.buf, v.data(), n);
    e.buf[n] = '\0';
    return e;
}

// --------------------------------------------------------------- 各类型控件
// 每个控件返回"用户是否改了值"。改了就顺手标脏, 让 ConfigManager 的增量保存
// 能带走这次改动。
void drawBool(BoolSetting& s) {
    bool v = s.value();
    if (ImGui::Checkbox(s.name().c_str(), &v)) {
        s.setValue(v);
    }
    if (!s.description().empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.description().c_str());
    }
}

void drawInt(IntSetting& s) {
    int v = static_cast<int>(s.value());
    const int lo = static_cast<int>(s.minValue());
    const int hi = static_cast<int>(s.maxValue());
    if (ImGui::SliderInt(s.name().c_str(), &v, lo, hi)) {
        s.setValue(v);
    }
    if (!s.description().empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.description().c_str());
    }
}

void drawDouble(DoubleSetting& s) {
    float v = static_cast<float>(s.value());
    const float lo = static_cast<float>(s.minValue());
    const float hi = static_cast<float>(s.maxValue());
    // 步长交给 ImGui 从类型精度推导; 自己在 label 里带上倍率提示更有用。
    if (ImGui::SliderFloat(s.name().c_str(), &v, lo, hi, "%.2f")) {
        s.setValue(static_cast<double>(v));
    }
    if (!s.description().empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.description().c_str());
    }
}

void drawString(StringSetting& s) {
    auto& e = stringEditFor(s);
    ImGui::SetNextItemWidth(-120.0f);
    if (ImGui::InputText(s.name().c_str(), e.buf, sizeof(e.buf))) {
        e.active = true;
    }
    // 只在失去焦点时提交: 每敲一个字符都写设置会让 onChanged 高频触发。
    if (e.active && ImGui::IsItemDeactivatedAfterEdit()) {
        s.setValue(e.buf);
        e.active = false;
    }
    if (!s.description().empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.description().c_str());
    }
}

void drawKeybind(KeybindSetting& s) {
    const bool capturing = gCapture.is(&s);
    const std::string label = capturing
                                  ? std::string(s.name()) + ": press a key..."
                                  : std::string(s.name()) + ": " + s.displayValue();

    if (capturing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.1f, 1.0f));
    }
    if (ImGui::Button(label.c_str(), ImVec2(220, 0))) {
        if (capturing) {
            gCapture.clear();
        } else {
            gCapture.setSettingKey(&s);
        }
    }
    if (capturing) ImGui::PopStyleColor();

    // 右键清空绑定 —— 不上这个的话, 想把键位改回"未绑定"就只能改配置文件。
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        s.clear();
        if (capturing) gCapture.clear();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Left click: rebind\nRight click: unbind");
    }
}

// 模块自身热键那一行。直接操作 Module, 不经过临时 Setting 对象。
void drawModuleKeybind(Module& m) {
    const bool capturing = gCapture.is(&m);
    const std::string label = capturing
                                  ? std::string("Hotkey: press a key...")
                                  : "Hotkey: " + KeybindSetting::keyName(m.keyBind());

    if (capturing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.1f, 1.0f));
    }
    if (ImGui::Button(label.c_str(), ImVec2(220, 0))) {
        if (capturing) {
            gCapture.clear();
        } else {
            gCapture.setModuleKey(&m);
        }
    }
    if (capturing) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        m.setKeyBind(KeybindSetting::unbound);
        if (capturing) gCapture.clear();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Left click: rebind\nRight click: unbind");
    }
}

void drawEnum(EnumSetting& s) {
    const auto& choices = s.choices();
    if (choices.empty()) return;

    // 把候选名拼成 \0 分隔的串, 少一次 vector<char*> 的分配。
    std::string items;
    for (auto const& c : choices) {
        items += c;
        items.push_back('\0');
    }
    items.push_back('\0');

    int idx = s.choiceIndex();
    if (idx < 0) idx = 0;
    if (ImGui::Combo(s.name().c_str(), &idx, items.c_str())) {
        // Combo 只会给出 0..choices.size()-1 范围里的下标, 所以这里必然成功。
        // 显式 (void) 掉返回值而不是忽略警告 —— 将来若候选表与下标来源改动,
        // 这行会提醒得去看一眼 setChoiceByIndex 的契约。
        (void)s.setChoiceByIndex(idx);
    }
    if (!s.description().empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", s.description().c_str());
    }
}

// 按实际类型分派。用 typeName() 而不是 dynamic_cast 链 —— 前者是虚函数,
// 加第三方设置类型时不用改这里的分派逻辑。
void drawSetting(Setting& s) {
    // 依赖不满足的设置置灰, 但仍然显示: 隐藏掉会让用户以为设置丢了。
    if (!s.isAvailable()) {
        ImGui::BeginDisabled();
        ImGui::TextDisabled("%s (unavailable)", s.name().c_str());
        ImGui::EndDisabled();
        return;
    }

    const std::string_view t = s.typeName();
    if (t == "Bool")         { drawBool(static_cast<BoolSetting&>(s)); return; }
    if (t == "Int")          { drawInt(static_cast<IntSetting&>(s)); return; }
    if (t == "Double")       { drawDouble(static_cast<DoubleSetting&>(s)); return; }
    if (t == "String")       { drawString(static_cast<StringSetting&>(s)); return; }
    if (t == "Keybind")      { drawKeybind(static_cast<KeybindSetting&>(s)); return; }
    if (t == "Enum")         { drawEnum(static_cast<EnumSetting&>(s)); return; }
    ImGui::TextDisabled("%s (unsupported type: %.*s)", s.name().c_str(),
                        static_cast<int>(t.size()), t.data());
}

// --------------------------------------------------------------- 单个模块
void drawModule(Module& m) {
    ImGui::PushID(&m);

    // ---- 标题行: 开关 + 名字 ----
    bool enabled = m.isEnabled();
    if (ImGui::Checkbox("##enabled", &enabled)) {
        m.setEnabled(enabled);
    }
    ImGui::SameLine();

    if (ImGui::CollapsingHeader(m.name().c_str())) {
        if (!m.description().empty()) {
            ImGui::TextWrapped("%s", m.description().c_str());
        }
        const std::string info = m.info();
        if (!info.empty()) {
            // 这行是运行期真实读数 —— 用来一眼判断模块到底有没有生效。
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", info.c_str());
        }

        drawModuleKeybind(m);

        int mode = (m.bindMode() == Module::BindMode::hold) ? 1 : 0;
        const char* modes[] = {"Toggle", "Hold"};
        if (ImGui::Combo("Mode", &mode, modes, 2)) {
            m.setBindMode(mode == 1 ? Module::BindMode::hold : Module::BindMode::toggle);
        }

        bool hidden = m.isHidden();
        if (ImGui::Checkbox("Hidden in list", &hidden)) {
            m.setHidden(hidden);
        }

        ImGui::Separator();
        for (auto& s : m.settings()) {
            if (s) drawSetting(*s);
        }

        ImGui::Spacing();
        if (ImGui::Button("Reset to defaults")) {
            m.reset();
        }
    }

    ImGui::PopID();
}

} // namespace

// ===========================================================================
//  对外接口
// ===========================================================================
bool FeaturePanel::isCapturing() { return gCapture.active(); }

bool FeaturePanel::captureKey(int32_t vk, bool pressed) {
    if (!gCapture.active()) return false;
    // 只处理按下; 抬起事件直接吞掉, 免得漏到游戏里。
    if (!pressed) return true;

    if (vk == VK_ESCAPE) {
        gCapture.clear();          // Esc = 取消改键
        return true;
    }
    if (vk <= 0) return true;

    if (gCapture.kind == CaptureTarget::Kind::moduleKey) {
        // 模块可能已经消失(理论上不会, ModuleManager 持有全部所有权),
        // 仍然做一次判空, 免得 UI 与注册表状态不一致时崩在面板里。
        if (gCapture.module) gCapture.module->setKeyBind(vk);
    } else if (gCapture.kind == CaptureTarget::Kind::settingKey) {
        if (gCapture.setting) gCapture.setting->setValue(vk);
    }
    gCapture.clear();
    return true;
}

void FeaturePanel::draw() {
    auto& mgr = ModuleManager::instance();
    auto& ctx = game();

    // ---- 顶部: 目标状态 ----
    if (ctx.movementReady()) {
        const auto& t = ctx.movement->target();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "player: %s",
                           t.pawnClass.c_str());
        ImGui::Text("movement: %s", t.movementClassName.c_str());
        // 偏移来源: "反射" 说明运行时反射可用; "静态表(...)" 说明这个构建的
        // UStruct 布局解不出来, 走的是从二进制属性表读出的已知偏移。
        const std::string& src = ctx.movement->offsetSource();
        ImGui::Text("offsets : %s", src.empty() ? "?" : src.c_str());
        ImGui::Text("  JumpZ=+%#x  MaxWalk=+%#x",
                    static_cast<unsigned>(ctx.movement->offsets().jumpZVelocity),
                    static_cast<unsigned>(ctx.movement->offsets().maxWalkSpeed));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "player: not resolved");
        const std::string err = ctx.movement ? ctx.movement->lastError() : std::string("no engine");
        if (!err.empty()) ImGui::TextWrapped("%s", err.c_str());
    }

    ImGui::Text("modules: %d (%d enabled)", static_cast<int>(mgr.moduleCount()),
                static_cast<int>(mgr.enabledCount()));
    ImGui::SameLine();
    if (ImGui::Button("Disable all")) mgr.disableAll();
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        if (ctx.engine) ctx.movement->resolve(*ctx.engine, true);
    }
    ImGui::Separator();

    // ---- 按分类列出模块 ----
    if (mgr.empty()) {
        ImGui::TextWrapped("No modules registered yet. initModules() runs during "
                           "payload startup; if the engine was not ready at that "
                           "moment, run 'rescan' in the console.");
        return;
    }

    for (Category cat : allCategories()) {
        auto list = mgr.modulesIn(cat);
        if (list.empty()) continue;

        const std::string catLabel = std::string(categoryName(cat));
        if (ImGui::CollapsingHeader(catLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (auto* m : list) {
                if (!m || m->isHidden()) continue;
                drawModule(*m);
            }
        }
    }

    // ---- 配置动作 ----
    ImGui::Separator();
    auto& cfg = ConfigManager::instance();
    ImGui::Text("config: %s", cfg.activeConfigName().c_str());
    if (ImGui::Button("Save now")) {
        if (cfg.save()) {
            logInfo("[Feature] 配置已保存");
        } else {
            logWarn(fmt("[Feature] 配置保存失败: {}", cfg.lastError()));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        if (cfg.reload()) {
            logInfo("[Feature] 配置已重新加载");
        } else {
            logWarn(fmt("[Feature] 配置加载失败: {}", cfg.lastError()));
        }
    }

    // 面板上改过的值可能还没落盘。这里不主动写 —— 由 Runtime 周期调用
    // saveIfDirty(), 避免拖滑块时每帧都写磁盘。
    if (mgr.anyDirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(unsaved changes)");
    }
}

} // namespace epsilon::feature
