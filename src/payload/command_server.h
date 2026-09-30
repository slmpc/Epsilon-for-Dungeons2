// ============================================================================
//  command_server.h — 命令分发
//
//  两条输入源共用同一套命令实现:
//    * 注入器的命令行  → 经命名管道下发
//    * 注入体自己的控制台 → 读 stdin
//  命令的语义是"只读采集 + 一次性写值校验", 不做任何持续性篡改。
//  (分析结论 §6.4 明确建议: 走第 2 层"调用游戏自己的函数"而不是第 3 层硬改内存;
//   这里先提供第 1 层的只读采集与偏移重建, 是后续所有功能的地基。)
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mcd2::ue { class Engine; }

namespace mcd2::payload {

class CommandServer {
public:
    // engine 必须是一个长期存活的对象(由 runtime 持有)。
    explicit CommandServer(ue::Engine& engine) : eng_(engine) {}

    // 执行一条命令行, 输出经 payload::emit 系列写出。
    // 返回是否应该继续运行(false = 收到 quit/detach)。
    bool execute(std::string_view line);

    // 打印帮助。
    void print_help() const;

private:
    // ---- 元信息 ----
    void cmd_status();
    void cmd_rescan();
    void cmd_globals();

    // ---- 对象表 ----
    void cmd_objects(std::vector<std::string> const& args);
    void cmd_find(std::vector<std::string> const& args);
    void cmd_class(std::vector<std::string> const& args);

    // ---- 原始内存 ----
    void cmd_mem(std::vector<std::string> const& args);

    // ---- 属性/偏移 ----
    void cmd_props(std::vector<std::string> const& args);
    void cmd_get(std::vector<std::string> const& args);

    // ---- 世界 ----
    void cmd_world();
    void cmd_actors(std::vector<std::string> const& args);

    // ---- 帧钩子 ----
    void cmd_hooks();          // 显示状态
    void cmd_hook_install();   // 显式安装

    ue::Engine& eng_;
};

} // namespace mcd2::payload
