#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::game::ue { class Engine; }

namespace epsilon::payload {

class CommandServer {
public:
    // engine 必须是一个长期存活的对象(由 Runtime 持有)。
    explicit CommandServer(game::ue::Engine& engine) : eng_(engine) {}

    // 执行一条命令行, 输出经 payload::emit 系列写出。
    // 返回是否应该继续运行(false = 收到 quit/detach)。
    bool execute(std::string_view line);

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
    // 诊断: 列出关卡里的玩家候选, 并说明模块会挑中哪一个(规则来自 d2::playerClassRank)。
    void cmdPlayer();

    // ---- 只读取证(属性链读空 / Actors 读空时用) ----
    // 把 UStruct 各"可能是属性链起点"的指针槽摊出来, 标注能否解出 FField 名字。
    void dumpStructPointerSlots(uint64_t structObj);
    // 在 ULevel 上按 TArray 形态扫 Actors 数组的候选槽。
    void scanForActorArray(uint64_t levelObj);
    // 按"8 字节指针 + 解释"打印一段内存。
    void dumpPointers(uint64_t addr, int count);
    // 在已知是 FField 的对象上定出 NamePrivate 的真实偏移。
    void probeFFieldNameOffset(uint64_t fieldAddr);
    // 绕开 FField 链: 用属性名的 FName 索引回搜 UClass 内存, 反查 Offset_Internal。
    void findPropertyDirect(std::string_view className, std::string_view propName);

    // ---- 移动参数 ----
    void cmdMovement();
    void dumpFloats(uint64_t addr, int count);
    void cmdAttributeMovement();
    // mvset <字段名|裸偏移> <数值>: 一次性直写, 带回读与采样。
    void setMovementValue(std::string_view field, float value);

    // ---- 货币 (绿宝石) ----
    // currency: 列出已解析的货币持有者与 6 个字段; 带 class= 时列出候选类。
    void cmdCurrency(std::vector<std::string> const& args);

    // ---- 资源解包 ----
    // oodle <输入文件> <输出文件> <原始长度>: 调用游戏自己的 OodleLZ_Decompress。
    void cmdOodle(std::vector<std::string> const& args);
    // oodlefind <输入文件> <原始长度> [扫描上限]: 在文件里扫出能解开的起点。
    void cmdOodleFind(std::vector<std::string> const& args);

    // ---- 帧钩子 ----
    void cmdHooks();
    void cmdHookInstall();

    game::ue::Engine& eng_;

    // 槽扫描里自动做 FField 名字偏移探测的次数预算: 只对继承链的第一个类做一次。
    int fieldProbeBudget_ = 1;
};

} // namespace epsilon::payload
