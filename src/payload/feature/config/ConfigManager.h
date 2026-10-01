// ============================================================================
//  ConfigManager.h — 配置持久化
//
//  对应 Open-Epsilon 的 common/managers/ConfigManager.java, 但按本项目的形态
//  做了大幅裁剪与一处重要的路径调整。
//
//  ── 文件布局 ──────────────────────────────────────────────────────────────
//     <配置根>/                       默认 ~/.epsilon/ext/dungeons2
//       active-config.txt             当前生效的配置名(纯文本单行)
//       configs/
//         default/
//           modules/
//             <模块名>.json           一个模块一个文件
//         <其它配置名>/
//           modules/...
//
//  为什么按模块拆文件而不是一个大 json:
//    模块是最小的独立功能单元, 拆开之后"某个模块配置写坏了"只影响它自己,
//    手改时也不用在几百行里找那一段。代价是加载要走一次目录遍历 —— 但配置
//    目录只有几十个文件, 这点开销相对注入流程可以忽略。
//
//  为什么默认落在 ~/.epsilon/ext/dungeons2:
//    与 Open-Epsilon 的 ~/.epsilon 保持同一父目录, 便于用户在一处管理。多出
//    的 ext/dungeons2 一层是因为 Open-Epsilon 自身的配置文件已经占了
//    ~/.epsilon 根目录(accounts.json / client-settings.json 等), 直接往同一个
//    目录里塞会互相干扰; 按 "扩展名/目标" 分层的写法也让同一台机器上可以同时
//    存在多个 Epsilon 目标的配置。
//
//  覆盖方式: 环境变量 EPSILON_CONFIG_DIR 指向别的目录即可(测试与多开用)。
//
//  ── 与 Java 版的差异 ────────────────────────────────────────────────────
//    * 去掉 zip 导入/导出。注入体跑在游戏进程里, 没有合适的 UI 触发这些操作,
//      留着只是一堆没有调用方的代码。
//    * 去掉账号/好友/根级客户端设置的持久化 —— 那些是 Open-Epsilon 特有的域
//      概念, 本项目没有对应物。
//    * 去掉旧版布局迁移(LegacyConfigMigrator): 本项目尚无历史版本需要兼容。
//    * 保留 active-config 与多配置切换, 因为这直接决定"用哪份配置启动"。
//
//  本类是纯框架: 它只认识 ModuleManager 里的模块, 不认识任何具体模块。
// ============================================================================
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
    // 返回是否成功。失败时配置仍可用, 只是全部停在默认值。
    bool initialize();

    // 从磁盘重新加载当前配置(先丢弃内存里的状态)。
    bool reload();

    // 把当前配置应用到已注册的模块上。
    // 与 reload 的区别只在语义: initialize 之后调用它更符合直觉(初始化只负责
    // 建目录与决定"当前配置是谁", 应用是下一步)。两者最终都走同一条加载路径。
    void applyToModules();

    // 把内存状态写回磁盘。任何时候都可安全调用, 包括尚未 initialize 时
    // (那时会先补齐目录结构)。
    bool save();

    // 只有在有模块被改动过时才写盘。宿主应当周期性调用这个(比如每帧或
    // 每几秒) —— 注入体没有可靠的"退出前保存"时机。
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
    // 内部实现都不加锁, 由公开接口统一持锁 —— 避免嵌套加锁。
    // 命名带 Locked 后缀的一律要求调用方已持有 mu_。
    bool initializeLocked();
    bool saveLocked();
    void loadActiveLocked();
    bool writeActiveConfigNameLocked() const;
    std::optional<std::string> readActiveConfigNameLocked() const;
    // 不加锁版本, 供已持锁的成员复用(listConfigs 的公开版会加锁, 直接调用会自锁)。
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
