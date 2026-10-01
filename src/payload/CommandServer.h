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

    // 在 ULevel 对象上找 TArray 形态的 Actors 数组。
    // 这个构建的 ULevel::Actors 静态偏移是错的(实测 actorCount=0), 而
    // TArray 有很强的可识别特征: {ptr, count, capacity}, ptr 落在堆区、
    // 0 < count <= capacity 且 capacity 量级合理。按这个特征扫比继续猜常量可靠。
    void scanForActorArray(uint64_t levelObj);

    // 把一段内存按"8 字节指针 + 解释"的形式打印出来。
    // 用于核对某个槽到底指向什么(前一个命令会给出候选地址)。
    void dumpPointers(uint64_t addr, int count);

    // 在给定的 FField 对象上找出"哪个偏移存放 FName 索引"。
    //
    // 存在的意义: 属性链之所以读不出来, 只剩一个可能 —— FField::NamePrivate
    // 的静态偏移(+0x28)对这个构建不对。这个命令拿一个**已知是 FField 的指针**
    // (由 props 的槽扫描给出), 遍历 0x18..0x48 的每个 4 字节位置, 看哪个能解出
    // 合法 FName, 直接把真实偏移定下来。
    void probeFFieldNameOffset(uint64_t fieldAddr);

    // 绕开 FField 链, 直接从 UClass 里找某个属性的 FProperty* 并读出它的偏移。
    //
    // 为什么需要这条路: 本构建的 UStruct 布局被改过, 走 ChildProperties/Next
    // 的传统链式遍历三种判据都失败(见 dumpStructPointerSlots 里的记录)。
    // 但有一件事是确定的: UClass 对象内部**必然**存放着它各属性的 FName 索引,
    // 因为引擎自己也要按名字查属性。
    //
    // 做法: 先从 GNames 里找出属性名对应的 FName 索引, 然后扫 UClass 的那一段
    // 内存, 找哪个位置存的正好是这个索引; 命中处再按"附近有没有一个指向
    // FProperty 的指针"来确认, 最后从该 FProperty 读出 Offset_Internal。
    void findPropertyDirect(std::string_view className, std::string_view propName);

    // 读出玩家移动组件上几个关键 float 的当前值。
    // 用于验证模块是否真的生效(以及生效前后的对照)。
    void cmdMovement();

    // 把一段内存按 float 解读后打印。排查"这个字段到底是速度还是别的"时,
    // 十六进制没法和游戏里的数值对上, 必须看成浮点。
    void dumpFloats(uint64_t addr, int count);

    // 找出**玩家的** ATR_Movement(移动属性集)实例, 并读出其中的速度相关属性。
    //
    // 为什么需要它: 属性的真身不在 UCharacterMovementComponent 上, 而在
    // GAS 的属性集 ATR_Movement 里(反汇编确认: OnRep_MovementSpeedMultiplier
    // 的 Outer 是 Class :: ATR_Movement)。而场景里有二十多个 ATR_Movement
    // 实例(玩家 + 各种敌人/NPC), 必须挑出属于玩家的那个。
    //
    // 判别方式: 沿 Outer 链往上走, 看哪一级正好是玩家 pawn。属性集挂在
    // AbilitySystemComponent 上, ASC 又挂在 pawn 或 playerState 上,
    // 所以链上一定能撞到玩家 pawn 或其 PlayerState。
    void cmdAttributeMovement();

    // 手工写一个移动属性: mvset <字段名> <数值>
    // 用于在面板之外做对照测试(面板要开模块才写, 这里直接写)。
    void setMovementValue(std::string_view field, float value);

    // ---- 帧钩子 ----
    void cmdHooks();          // 显示状态
    void cmdHookInstall();   // 显式安装

    ue::Engine& eng_;

    // 槽扫描里自动做 FField 名字偏移探测的次数预算。
    // 一次 props 会遍历整条继承链(9 个类), 每个类都探一遍既慢又刷屏,
    // 只对第一个类做一次就够定位偏移了。
    int fieldProbeBudget_ = 1;
};

} // namespace epsilon::payload
