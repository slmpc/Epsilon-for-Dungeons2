#pragma once

#include "payload/feature/module/ModuleManager.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::feature {

class ConfigManager {
public:
    // 配置格式版本, 写进每个模块文件, 供将来迁移判断。
    static constexpr int configVersion = 1;

    // 默认配置名。空配置集时自动创建它。
    static constexpr std::string_view defaultConfigName = "default";

    [[nodiscard]] static ConfigManager& instance();

    ConfigManager() = default;
    ~ConfigManager() = default;
    ConfigManager(ConfigManager const&) = delete;
    ConfigManager& operator=(ConfigManager const&) = delete;

    // ---------------------------------------------------------------- 路径
    // 配置根目录。解析顺序: 环境变量 EPSILON_CONFIG_DIR > <用户目录>\.epsilon\ext\dungeons2
    [[nodiscard]] std::filesystem::path configDir() const;
    [[nodiscard]] std::filesystem::path configsDir() const;
    [[nodiscard]] std::filesystem::path activeConfigDir() const;
    [[nodiscard]] std::filesystem::path activeModulesDir() const;

    // ---------------------------------------------------------------- 生命周期
    // 建目录、读 active-config、加载当前配置。可重复调用(等价于 reload)。
    // 返回是否成功; 失败时配置仍可用, 只是全部停在默认值。
    bool initialize();

    // 从磁盘重新加载当前配置(先丢弃内存里的状态)。
    bool reload();

    // 把当前配置应用到已注册的模块上(与 reload 是同一条加载路径, 只是语义更
    // 贴合"initialize 决定当前配置是谁, 应用是下一步")。
    void applyToModules();

    // 把内存状态写回磁盘。任何时候都可安全调用, 包括尚未 initialize 时
    // (那时会先补齐目录结构)。
    bool save();

    // 只在真的有模块被改动过时才写盘。宿主应当周期性调用(注入体没有可靠的
    // "退出前保存"时机)。
    bool saveIfDirty();

    // ---------------------------------------------------------------- 配置集
    [[nodiscard]] std::string activeConfigName() const;
    bool setActiveConfig(std::string_view name);

    [[nodiscard]] std::vector<std::string> listConfigs() const;
    // 新建一个配置(不切换)。名字非法或已存在返回 false。
    bool createConfig(std::string_view name);
    // 删除一个配置目录。不允许删掉最后一个, 也不允许删当前生效的那个
    // (前者会让程序无配置可用, 后者会让 active-config.txt 指向不存在的目录)。
    bool deleteConfig(std::string_view name);
    // ---------------------------------------------------------------- 复位
    // 把所有模块恢复默认值并落盘。
    bool resetAll();

    // ---------------------------------------------------------------- 校验
    // 配置名是否合法(用于目录名, 必须挡住路径穿越)。
    [[nodiscard]] static bool isValidConfigName(std::string_view name);

    // ---------------------------------------------------------------- 诊断
    // 最近一次失败原因, 供 status 命令/UI 显示。
    [[nodiscard]] std::string lastError() const;

private:
    // 内部实现都不加锁, 由公开接口统一持锁。带 Locked 后缀的成员要求调用方已持锁。
    bool initializeLocked();
    bool saveLocked();
    void loadActiveLocked();
    bool writeActiveConfigNameLocked() const;
    std::optional<std::string> readActiveConfigNameLocked() const;
    // 不加锁版本, 供已持锁的成员复用(直接调公开版会自锁)。
    [[nodiscard]] std::vector<std::string> listConfigsLocked() const;

    [[nodiscard]] std::filesystem::path configDirLocked() const;
    [[nodiscard]] std::filesystem::path modulesDirFor(std::string_view configName) const;
    [[nodiscard]] std::filesystem::path moduleFileFor(std::string_view configName,
                                                      std::string_view moduleName) const;

    bool ensureDirectoriesLocked();
    void setLastErrorLocked(std::string_view msg) const;

    mutable std::mutex mu_;
    std::string        activeConfig_;
    bool               initialized_ = false;
    mutable std::string lastError_;
};

} // namespace epsilon::feature
