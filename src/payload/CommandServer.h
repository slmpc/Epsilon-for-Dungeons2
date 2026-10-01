// ============================================================================
//  commandServer.h — 命令分发
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

namespace epsilon::ue { class Engine; }

namespace epsilon::payload {

class CommandServer {
public:
    // engine 必须是一个长期存活的对象(由 runtime 持有)。
    explicit CommandServer(ue::Engine& engine) : eng_(engine) {}

    // 执行一条命令行, 输出经 payload::emit 系列写出。
    // 返回是否应该继续运行(false = 收到 quit/detach)。
    bool execute(std::string_view line);

    // 打印帮助。
    void printHelp() const;

private:
    // ---- 元信息 ----
    void cmdStatus();
    void cmdRescan();
    void cmdGlobals();

    // ---- 对象表 ----
    void cmdObjects(std::vector<std::string> const& args);
    void cmdFind(std::vector<std::string> const& args);
    void cmdClass(std::vector<std::string> const& args);

    // ---- 原始内存 ----
    void cmdMem(std::vector<std::string> const& args);

    // ---- 属性/偏移 ----
    void cmdProps(std::vector<std::string> const& args);
    void cmdGet(std::vector<std::string> const& args);
    void cmdSet(std::vector<std::string> const& args);

    // ---- 世界 ----
    void cmdWorld();
    void cmdActors(std::vector<std::string> const& args);
    // 诊断: 找出关卡里的"玩家候选", 并说明功能模块会挑中哪一个。
    // 存在的意义: 功能模块按类名匹配玩家, 一旦类名假设错了, 表现只是
    // "开了没反应", 极难定位。这条命令把类名实情直接摊开。
    void cmdPlayer();

    // 属性链读空时的取证: 把 UStruct 对象里各个"可能是属性链起点"的指针槽
    // 全 dump 出来, 并标注它指向的位置能否解出合法的 FField 名字。
    void dumpStructPointerSlots(uint64_t structObj);

    // ---- 帧钩子 ----
    void cmdHooks();          // 显示状态
    void cmdHookInstall();   // 显式安装

    ue::Engine& eng_;
};

} // namespace epsilon::payload
