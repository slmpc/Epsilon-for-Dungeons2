#include "payload/feature/config/ConfigManager.h"

#include "common/Text.h"
#include "payload/Payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>          // SHGetKnownFolderPath

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace epsilon::feature {
namespace fs = std::filesystem;

// 日志辅助在 epsilon::payload 里(见 Payload.h)。只引这三个, 不用 using namespace。
using epsilon::payload::logInfo;
using epsilon::payload::logWarn;
using epsilon::payload::logError;

namespace {

// 配置根目录的环境变量覆盖开关, 便于测试与多开。
constexpr wchar_t envOverrideName[] = L"EPSILON_CONFIG_DIR";

// 配置目录名(相对用户目录)。
const fs::path relativeConfigPath = fs::path(L".epsilon") / L"ext" / L"dungeons2";

// 各子项名。
constexpr wchar_t activeConfigFileName[] = L"active-config.txt";
constexpr wchar_t configsFolderName[]    = L"configs";
constexpr wchar_t modulesFolderName[]    = L"modules";
constexpr wchar_t jsonExtension[]        = L".json";

// 取用户目录。优先 USERPROFILE, 失败退到 SHGetKnownFolderPath。
fs::path userHomeDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf);

    PWSTR known = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &known)) && known) {
        fs::path p(known);
        ::CoTaskMemFree(known);
        return p;
    }
    if (known) ::CoTaskMemFree(known);
    return {};
}

// 原子写文件: 先写 <path>.tmp, 再 MoveFileEx 覆盖目标。
// 直接覆盖写会在进程被强杀时留下半截 json, 用户的整份配置就没了。
bool writeFileAtomic(fs::path const& path, std::string const& content, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = fmt("创建目录失败: {}", ec.message());
        return false;
    }

    fs::path tmp = path;
    tmp += L".tmp";

    {
        // 二进制写: json 里的 \n 不该被 CRT 翻译成 \r\n。
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "无法打开临时文件";
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            error = "写入临时文件失败";
            out.close();
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return false;
        }
    }

    // MoveFileExW 而不是 fs::rename: 后者在目标已存在时的行为随实现而变,
    // 且不替换只读文件。
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = ::GetLastError();
        error = fmt("替换目标文件失败 Win32={}", err);
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

bool readFileAll(fs::path const& path, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "无法打开文件";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// 目录名 -> 是否可用作配置名。委托给公开校验, 保证"新建时能通过"与
// "列表里认得出"用的是同一套规则。
bool isUsableConfigName(std::string const& name) {
    return ConfigManager::isValidConfigName(name);
}

} // namespace

// ===========================================================================
//  单例
// ===========================================================================
ConfigManager& ConfigManager::instance() {
    static ConfigManager mgr;
    return mgr;
}

// ===========================================================================
//  路径
// ===========================================================================
fs::path ConfigManager::configDirLocked() const {
    // 1) 环境变量覆盖
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ::GetEnvironmentVariableW(envOverrideName, buf,
                                              static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return fs::path(buf);
    }

    // 2) 默认: <用户目录>\.epsilon\ext\dungeons2
    const fs::path home = userHomeDir();
    if (home.empty()) {
        // 拿不到用户目录时退到进程当前目录 —— 总比完全不落盘好。
        return fs::current_path() / L"epsilon-config";
    }
    return home / relativeConfigPath;
}

fs::path ConfigManager::configDir() const {
    std::lock_guard lk(mu_);
    return configDirLocked();
}

fs::path ConfigManager::configsDir() const {
    std::lock_guard lk(mu_);
    return configDirLocked() / configsFolderName;
}

fs::path ConfigManager::modulesDirFor(std::string_view configName) const {
    return configDirLocked() / configsFolderName / toUtf16(configName) / modulesFolderName;
}

fs::path ConfigManager::moduleFileFor(std::string_view configName,
                                      std::string_view moduleName) const {
    fs::path p = modulesDirFor(configName);
    p /= toUtf16(moduleName);
    p += jsonExtension;
    return p;
}

fs::path ConfigManager::activeConfigDir() const {
    std::lock_guard lk(mu_);
    return configDirLocked() / configsFolderName / toUtf16(activeConfig_);
}

fs::path ConfigManager::activeModulesDir() const {
    std::lock_guard lk(mu_);
    return modulesDirFor(activeConfig_);
}

// ===========================================================================
//  校验
// ===========================================================================
bool ConfigManager::isValidConfigName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == "..") return false;

    // ★ 配置名会直接当目录名用, 所以必须白名单式校验 —— 漏一个字符就等于
    // 让配置名能写到配置根之外(路径穿越)。
    bool hasNonDot = false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        // 控制字符
        if (u < 0x20 || u == 0x7F) return false;
        // Win32 文件名非法字符
        switch (c) {
            case '\\': case '/': case ':': case '*':
            case '?':  case '"': case '<': case '>': case '|':
                return false;
            default: break;
        }
        if (c != '.') hasNonDot = true;
    }
    // 至少得有非点字符, 否则 "." / "..." 这类名字会落到当前目录或上两级。
    if (!hasNonDot) return false;
    if (name.find("..") != std::string_view::npos) return false;
    // 结尾不能是点或空格 —— Win32 会静默裁掉, 之后按名字就找不回来了。
    if (name.back() == '.' || name.back() == ' ') return false;
    return true;
}

std::string ConfigManager::lastError() const {
    std::lock_guard lk(mu_);
    return lastError_;
}

void ConfigManager::setLastErrorLocked(std::string_view msg) const {
    lastError_.assign(msg);
}

// ===========================================================================
//  生命周期
// ===========================================================================
bool ConfigManager::ensureDirectoriesLocked() {
    std::error_code ec;
    const fs::path root = configDirLocked();
    fs::create_directories(root / configsFolderName, ec);
    if (ec) {
        setLastErrorLocked(fmt("创建配置目录失败: {}", ec.message()));
        return false;
    }
    fs::create_directories(modulesDirFor(activeConfig_), ec);
    if (ec) {
        setLastErrorLocked(fmt("创建模块目录失败: {}", ec.message()));
        return false;
    }
    return true;
}

std::optional<std::string> ConfigManager::readActiveConfigNameLocked() const {
    const fs::path file = configDirLocked() / activeConfigFileName;
    std::string content;
    std::string err;
    if (!readFileAll(file, content, err)) return std::nullopt;

    // 去掉首尾空白与 BOM —— 用文本编辑器改过这个文件的话很容易带上这些,
    // 不清掉会导致按名字找目录失败。
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }
    while (!content.empty() && (content.back() == '\r' || content.back() == '\n' ||
                                content.back() == ' '  || content.back() == '\t')) {
        content.pop_back();
    }
    size_t b = 0;
    while (b < content.size() && (content[b] == ' ' || content[b] == '\t')) ++b;
    content.erase(0, b);

    if (content.empty() || !isValidConfigName(content)) return std::nullopt;
    return content;
}

bool ConfigManager::writeActiveConfigNameLocked() const {
    const fs::path file = configDirLocked() / activeConfigFileName;
    std::string err;
    if (!writeFileAtomic(file, activeConfig_, err)) {
        setLastErrorLocked(fmt("写入 active-config.txt 失败: {}", err));
        return false;
    }
    return true;
}

bool ConfigManager::initialize() {
    std::lock_guard lk(mu_);
    return initializeLocked();
}

bool ConfigManager::initializeLocked() {
    // 决定当前生效的配置名。
    if (auto stored = readActiveConfigNameLocked()) {
        activeConfig_ = *stored;
    } else {
        // 没有 active-config.txt 时: 已有配置目录就取第一个, 否则用默认名。
        activeConfig_.clear();
        std::error_code ec;
        const fs::path cfgRoot = configDirLocked() / configsFolderName;
        if (fs::exists(cfgRoot, ec)) {
            for (auto const& entry : fs::directory_iterator(cfgRoot, ec)) {
                if (!entry.is_directory()) continue;
                const std::string name = toUtf8(entry.path().filename().wstring());
                if (!isUsableConfigName(name)) continue;
                activeConfig_ = name;
                break;
            }
        }
        if (activeConfig_.empty()) activeConfig_ = std::string(defaultConfigName);
    }

    if (!ensureDirectoriesLocked()) return false;
    if (!writeActiveConfigNameLocked()) return false;

    loadActiveLocked();
    initialized_ = true;

    logInfo(fmt("配置目录: {}", toUtf8(configDirLocked().wstring())));
    logInfo(fmt("当前配置: {}", activeConfig_));
    return true;
}

bool ConfigManager::reload() {
    std::lock_guard lk(mu_);
    if (!ensureDirectoriesLocked()) return false;
    loadActiveLocked();
    return true;
}

void ConfigManager::applyToModules() {
    std::lock_guard lk(mu_);
    loadActiveLocked();
}

// 遍历当前配置的 modules 目录, 逐个交给对应模块反序列化。
void ConfigManager::loadActiveLocked() {
    auto& mgr = ModuleManager::instance();

    const fs::path dir = modulesDirFor(activeConfig_);
    std::error_code ec;

    // 先把所有模块复位到默认值, 否则"配置里删掉了某个模块的条目"时内存里会
    // 残留上一次加载的值。
    for (auto* m : mgr.modules()) {
        if (m) m->reset();
    }

    if (!fs::exists(dir, ec)) {
        // 配置首次创建时目录就是空的, 走默认值即可。
        mgr.markAllClean();
        return;
    }

    size_t loaded = 0;
    for (auto const& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != jsonExtension) continue;

        const std::string moduleName = toUtf8(entry.path().stem().wstring());
        auto* m = mgr.find(moduleName);
        if (!m) continue;      // 代码里没有这个模块: 忽略残留文件

        std::string content;
        std::string err;
        if (!readFileAll(entry.path(), content, err)) {
            logWarn(fmt("读取模块配置失败 [{}]: {}", moduleName, err));
            continue;
        }

        // 不抛异常的解析入口: 配置可能被手改坏, 这时应跳过这一个模块而不是
        // 让整次加载中断。
        nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
        if (parsed.is_discarded()) {
            logWarn(fmt("模块配置解析失败(已跳过) [{}]", moduleName));
            continue;
        }

        try {
            m->fromJson(parsed);
            ++loaded;
        } catch (std::exception const& e) {
            logWarn(fmt("应用模块配置失败 [{}]: {}", moduleName, e.what()));
        }
    }

    // 刚加载进来的值就是磁盘上的值, 不该被当成"待保存的改动"。
    mgr.markAllClean();
    logInfo(fmt("已加载 {} 个模块配置(配置: {})", loaded, activeConfig_));
}

// ===========================================================================
//  保存
// ===========================================================================
bool ConfigManager::save() {
    std::lock_guard lk(mu_);
    return saveLocked();
}

bool ConfigManager::saveIfDirty() {
    std::lock_guard lk(mu_);
    if (!ModuleManager::instance().anyDirty()) return true;
    return saveLocked();
}

bool ConfigManager::saveLocked() {
    auto& mgr = ModuleManager::instance();

    if (!ensureDirectoriesLocked()) return false;

    std::error_code ec;
    const fs::path dir = modulesDirFor(activeConfig_);
    fs::create_directories(dir, ec);
    if (ec) {
        setLastErrorLocked(fmt("创建模块目录失败: {}", ec.message()));
        return false;
    }

    bool allOk = true;
    for (auto* m : mgr.modules()) {
        if (!m) continue;

        const fs::path file = moduleFileFor(activeConfig_, m->name());

        nlohmann::json doc;
        try {
            doc = m->toJson();
        } catch (std::exception const& e) {
            logWarn(fmt("序列化模块失败 [{}]: {}", m->name(), e.what()));
            allOk = false;
            continue;
        }

        // 4 空格缩进 + 不转义非 ASCII: 配置是给人看的。
        const std::string text = doc.dump(4, ' ', false, nlohmann::json::error_handler_t::replace);

        std::string err;
        if (!writeFileAtomic(file, text, err)) {
            logWarn(fmt("写入模块配置失败 [{}]: {}", m->name(), err));
            setLastErrorLocked(fmt("写入模块配置失败 [{}]: {}", m->name(), err));
            allOk = false;
        }
    }

    if (allOk) {
        // 只有全部写成功才清脏标记, 否则写失败的那部分改动会永久丢失。
        mgr.markAllClean();
    }
    return allOk;
}

// ===========================================================================
//  配置集
// ===========================================================================
std::string ConfigManager::activeConfigName() const {
    std::lock_guard lk(mu_);
    return activeConfig_;
}

std::vector<std::string> ConfigManager::listConfigs() const {
    std::lock_guard lk(mu_);
    return listConfigsLocked();
}

std::vector<std::string> ConfigManager::listConfigsLocked() const {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path root = configDirLocked() / configsFolderName;
    if (!fs::exists(root, ec)) return out;

    for (auto const& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const std::string name = toUtf8(entry.path().filename().wstring());
        if (isUsableConfigName(name)) out.push_back(name);
    }
    // 排序保证面板里顺序稳定 —— 目录遍历顺序在不同文件系统上不一样。
    std::sort(out.begin(), out.end());
    return out;
}

bool ConfigManager::setActiveConfig(std::string_view name) {
    std::lock_guard lk(mu_);

    if (!isValidConfigName(name)) {
        setLastErrorLocked("配置名不合法");
        return false;
    }

    const std::string wanted(name);
    if (wanted == activeConfig_) {
        loadActiveLocked();
        return true;
    }

    // 切换前先把当前配置存盘, 否则切回来时改动已经丢了。
    if (initialized_) {
        if (!saveLocked()) {
            logWarn("切换配置前保存失败, 仍继续切换");
        }
    }

    activeConfig_ = wanted;
    if (!ensureDirectoriesLocked()) return false;
    if (!writeActiveConfigNameLocked()) return false;

    loadActiveLocked();
    return true;
}

bool ConfigManager::createConfig(std::string_view name) {
    std::lock_guard lk(mu_);

    if (!isValidConfigName(name)) {
        setLastErrorLocked("配置名不合法");
        return false;
    }

    std::error_code ec;
    const fs::path dir = modulesDirFor(name);
    if (fs::exists(dir, ec)) {
        setLastErrorLocked("同名配置已存在");
        return false;
    }
    fs::create_directories(dir, ec);
    if (ec) {
        setLastErrorLocked(fmt("创建配置失败: {}", ec.message()));
        return false;
    }
    return true;
}

bool ConfigManager::deleteConfig(std::string_view name) {
    std::lock_guard lk(mu_);

    if (!isValidConfigName(name)) {
        setLastErrorLocked("配置名不合法");
        return false;
    }

    const std::string target(name);
    if (target == activeConfig_) {
        // 删当前配置会让 active-config.txt 指向不存在的目录, 下次启动得靠
        // fallback 逻辑猜 —— 直接拒绝。
        setLastErrorLocked("不能删除当前生效的配置, 请先切换到别的配置");
        return false;
    }

    auto configs = listConfigsLocked();
    if (configs.size() <= 1) {
        setLastErrorLocked("至少要保留一个配置");
        return false;
    }

    std::error_code ec;
    const fs::path dir = configDirLocked() / configsFolderName / toUtf16(target);
    const auto removed = fs::remove_all(dir, ec);
    if (ec || removed == 0) {
        setLastErrorLocked(fmt("删除配置失败: {}", ec ? ec.message() : "目录不存在"));
        return false;
    }
    return true;
}

// ===========================================================================
//  复位
// ===========================================================================
bool ConfigManager::resetAll() {
    std::lock_guard lk(mu_);

    // 先清空磁盘上这个配置的模块文件, 再让模块回默认值, 最后整体重写。
    // 不先清空的话, "某个模块被移除后残留的旧配置"会被重新写回去。
    std::error_code ec;
    const fs::path dir = modulesDirFor(activeConfig_);
    if (fs::exists(dir, ec)) {
        fs::remove_all(dir, ec);
    }

    for (auto* m : ModuleManager::instance().modules()) {
        if (m) m->reset();
    }
    return saveLocked();
}

} // namespace epsilon::feature
