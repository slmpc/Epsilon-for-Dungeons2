#include "payload/CommandServer.h"

#include "common/PeImage.h"
#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/Hooks.h"
#include "payload/Runtime.h"      // installFrameHook()
#include "payload/feature/GameContext.h"
#include "payload/game/Field.h"
#include "payload/game/dungeons2/Movement.h"
#include "payload/game/dungeons2/Player.h"
#include "payload/game/ue/Engine.h"
#include "payload/game/ue/World.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>     // WideCharToMultiByte / CP_UTF8

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace epsilon::payload {
namespace {

using namespace epsilon::game::ue;
namespace d2 = epsilon::game::dungeons2;

// 解析 "key=value" 形式的选项; 返回 value, 不存在则返回 nullopt。
std::optional<std::string> optValue(std::vector<std::string> const& args,
                                     std::string_view key) {
    const std::string prefix = std::string(key) + "=";
    for (auto const& a : args) {
        if (a.size() > prefix.size() && istartsWith(a, prefix)) {
            return a.substr(prefix.size());
        }
    }
    return std::nullopt;
}

std::optional<int64_t> optInt(std::vector<std::string> const& args, std::string_view key) {
    if (auto v = optValue(args, key)) {
        try { return std::stoll(*v, nullptr, 0); } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

// 就地写目标进程内存。注入体已经跑在目标进程里, 所以不需要
// WriteProcessMemory —— 直接用 safeWrite: 它会先试直写, 失败才放宽页保护。
// 目标地址可能是只读页(.rdata / 代码段 / 常量池), 裸 memcpy 会抛访问冲突
// 把游戏带崩, 所以这条路必须走 safeWrite 而不是 std::memcpy。
bool remotePoke(uint64_t addr, void const* data, size_t size) {
    if (!addr || !data || size == 0) return false;
    return safeWrite(reinterpret_cast<void*>(addr), data, size);
}

// 按属性类型渲染一个内存位置上的值。
std::string renderValue(uint64_t owner, PropertyField const& pf) {
    if (!owner) return "(空对象)";
    const uint64_t at = owner + static_cast<uint32_t>(pf.offset);
    const std::string& t = pf.type;

    auto pod = [&](auto tag) -> std::string {
        using T = decltype(tag);
        T v{};
        if (!safeRead(&v, reinterpret_cast<const void*>(at), sizeof(T))) return "(读取失败)";
        return fmt("{}", v);
    };

    if (icontains(t, "BoolProperty")) {
        // UE 的 bool 按位存: 字节值 & (1 << (offset % 8)), 位偏移存在 FProperty 里。
        // 这里只能给出字节层面的近似 —— 标注清楚, 避免误导。
        uint8_t b = 0;
        if (!safeRead(&b, reinterpret_cast<const void*>(at), 1)) return "(读取失败)";
        return fmt("byte={:#04x} (bool 位域, 精确位需读 FProperty 的位偏移)", b);
    }
    if (icontains(t, "FloatProperty"))   return pod(float{});
    if (icontains(t, "DoubleProperty"))  return pod(double{});
    if (icontains(t, "Int64Property"))   return pod(int64_t{});
    if (icontains(t, "Int32Property") || icontains(t, "IntProperty")) return pod(int32_t{});
    if (icontains(t, "Int16Property"))   return pod(int16_t{});
    if (icontains(t, "Int8Property"))    return pod(int8_t{});
    if (icontains(t, "ByteProperty"))    return pod(uint8_t{});
    if (icontains(t, "UInt32Property"))  return pod(uint32_t{});

    if (icontains(t, "StrProperty") || icontains(t, "NameProperty") ||
        icontains(t, "TextProperty")) {
        namespace sd = epsilon::game::offsets::stringData;
        uint64_t data = 0;
        int32_t  num = 0;
        if (!safeRead(&data, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        safeRead(&num, reinterpret_cast<const void*>(at + sd::num), 4);
        if (!data || num <= 0 || num > 4096) return fmt("ptr={} (空/不合法)", hex(data, 16));
        std::wstring w(static_cast<size_t>(num), L'\0');
        if (!safeRead(w.data(), reinterpret_cast<const void*>(data), static_cast<size_t>(num) * 2))
            return fmt("ptr={} (字符串读取失败)", hex(data, 16));
        while (!w.empty() && (w.back() == L'\0' || w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(std::max(0, n)), '\0');
        if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                         s.data(), n, nullptr, nullptr);
        return fmt("\"{}\"", sanitize(s, 200));
    }

    if (icontains(t, "ObjectProperty") || icontains(t, "ClassProperty") ||
        icontains(t, "WeakObjectProperty") || icontains(t, "SoftObjectProperty")) {
        uint64_t ptr = 0;
        if (!safeRead(&ptr, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        if (!ptr) return "null";
        return fmt("对象指针 {}", hex(ptr, 16));
    }

    if (icontains(t, "StructProperty")) {
        return fmt("struct {} @ +{:#x} (内联 {:#x} 字节)", 
                   pf.structType.empty() ? "?" : pf.structType, pf.offset, pf.size);
    }

    if (icontains(t, "ArrayProperty")) {
        namespace arr = epsilon::game::offsets::array;
        uint64_t data = 0; int32_t num = 0, cap = 0;
        if (!safeRead(&data, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        safeRead(&num, reinterpret_cast<const void*>(at + arr::num), 4);
        safeRead(&cap, reinterpret_cast<const void*>(at + arr::max), 4);
        return fmt("TArray<{}> data={} num={} max={}",
                   pf.objectType.empty() ? pf.structType : pf.objectType,
                   hex(data, 16), num, cap);
    }

    return fmt("{} @ +{:#x} ({} 字节, 未做类型解读)", t.empty() ? "未知类型" : t,
               pf.offset, pf.size);
}

} // namespace

// ---------------------------------------------------------------------------
bool CommandServer::execute(std::string_view line) {
    auto args = splitWs(trim(line));
    if (args.empty()) return true;

    const std::string cmd = toLower(args[0]);
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "help" || cmd == "?")        { printHelp(); }
    else if (cmd == "status")               { cmdStatus(); }
    else if (cmd == "rescan" || cmd == "init") { cmdRescan(); }
    else if (cmd == "globals")              { cmdGlobals(); }
    else if (cmd == "hooks")                { cmdHooks(); }
    else if (cmd == "hook")                 { cmdHookInstall(); }
    else if (cmd == "objects" || cmd == "obj") { cmdObjects(rest); }
    else if (cmd == "find")                 { cmdFind(rest); }
    else if (cmd == "class")                { cmdClass(rest); }
    else if (cmd == "props")                { cmdProps(rest); }
    else if (cmd == "get")                  { cmdGet(rest); }
    else if (cmd == "set")                  { cmdSet(rest); }
    else if (cmd == "world" || cmd == "level") { cmdWorld(); }
    else if (cmd == "actors" || cmd == "list") { cmdActors(rest); }
    else if (cmd == "player" || cmd == "pawn") { cmdPlayer(); }
    else if (cmd == "mem" || cmd == "dump")    { cmdMem(rest); }
    else if (cmd == "ptr" || cmd == "ptrs") {
        // ptr <地址> [个数] —— 按指针逐个解释一段内存
        if (rest.empty()) { emitLine("用法: ptr <地址> [个数]"); }
        else {
            uint64_t a = 0;
            try { a = std::stoull(rest[0], nullptr, 0); } catch (...) {
                emitFmt("地址解析失败: {}", rest[0]);
            }
            if (a) dumpPointers(a, static_cast<int>(optInt(rest, "n").value_or(16)));
        }
    }
    else if (cmd == "probeff" || cmd == "ffield") {
        // probeff <FField 地址> —— 定出 NamePrivate 的真实偏移
        if (rest.empty()) { emitLine("用法: probeff <FField 地址>"); }
        else {
            uint64_t a = 0;
            try { a = std::stoull(rest[0], nullptr, 0); } catch (...) {}
            if (a) probeFFieldNameOffset(a); else emitLine("地址解析失败");
        }
    }
    else if (cmd == "findprop" || cmd == "fp") {
        // findprop <类名> <属性名> —— 绕开 FField 链直接定位属性偏移
        if (rest.size() < 2) { emitLine("用法: findprop <类名> <属性名>"); }
        else findPropertyDirect(rest[0], rest[1]);
    }
    else if (cmd == "mv" || cmd == "movement") { cmdMovement(); }
    else if (cmd == "attrmv" || cmd == "attrs") { cmdAttributeMovement(); }
    else if (cmd == "currency" || cmd == "emerald" || cmd == "em") { cmdCurrency(rest); }
    else if (cmd == "oodle") { cmdOodle(rest); }
    else if (cmd == "floats" || cmd == "f") {
        // floats <地址> [个数] —— 按 float 解读一段内存
        if (rest.empty()) { emitLine("用法: floats <地址> [个数]"); }
        else {
            uint64_t a = 0;
            try { a = std::stoull(rest[0], nullptr, 0); } catch (...) {}
            // 个数是**第二个位置参数**, 不是 "n=" 选项 —— 之前这里用 optInt 找
            // 选项, 于是调用方传的位置参数被静默忽略, 永远只打 16 个。排查
            // 属性块时正好被这个坑到: 明明要 48 个却只看到 16 个。
            int count = 16;
            if (rest.size() >= 2) {
                try { count = std::stoi(rest[1]); } catch (...) {}
            }
            if (a) dumpFloats(a, count);
            else emitLine("地址解析失败");
        }
    }
    else if (cmd == "poke") {
        // poke <绝对地址> <浮点值> —— 往任意地址写一个 float, 带回读与采样。
        // 与 mvset 的区别: mvset 的偏移相对已解析的移动组件, 而 GAS 属性集
        // (ATR_Movement)是另一个对象, 只能用绝对地址。
        if (rest.size() < 2) { emitLine("用法: poke <绝对地址> <浮点值>"); }
        else {
            uint64_t a = 0;
            float v = 0.0f;
            try { a = std::stoull(rest[0], nullptr, 0); } catch (...) {}
            try { v = std::stof(rest[1]); } catch (...) { a = 0; }
            if (!a) { emitLine("地址或数值解析失败"); }
            else {
                auto before = epsilon::game::readF32(a, 0);
                const bool ok = epsilon::game::writeF32(a, 0, v);
                auto now = epsilon::game::readF32(a, 0);
                emitFmt("{} : {} -> {}   (write {})", hex(a, 16),
                         before ? fmt("{:.4f}", *before) : std::string("?"),
                         now ? fmt("{:.4f}", *now) : std::string("?"),
                         ok ? "ok" : "FAILED");
                if (now && std::fabs(*now - v) > 0.001f) {
                    emitLine("  !! 写入未落地");
                } else {
                    emit("  samples: ");
                    int reverted = 0;
                    for (int i = 0; i < 8; ++i) {
                        ::Sleep(40);
                        auto s = epsilon::game::readF32(a, 0);
                        if (!s) { emit("?"); break; }
                        emit(fmt("{:.1f} ", *s));
                        if (std::fabs(*s - v) > 0.01f) ++reverted;
                    }
                    emitLine("");
                    if (reverted == 0) emitLine("  -> 保住了");
                    else emitFmt("  -> 被改回 {}/8 次", reverted);
                }
            }
        }
    }
    else if (cmd == "mvset") {
        // mvset <字段名> <数值>
        if (rest.size() < 2) { emitLine("用法: mvset <字段名> <数值>"); }
        else {
            try {
                const float v = std::stof(rest[1]);
                setMovementValue(rest[0], v);
            } catch (...) { emitLine("数值解析失败"); }
        }
    }
    else if (cmd == "scanlevel") {
        // scanlevel <ULevel 地址> —— 在对象上找 TArray 形态的 Actors
        if (rest.empty()) { emitLine("用法: scanlevel <ULevel 地址>"); }
        else {
            uint64_t a = 0;
            try { a = std::stoull(rest[0], nullptr, 0); } catch (...) {}
            if (a) scanForActorArray(a); else emitLine("地址解析失败");
        }
    }
    else if (cmd == "quit" || cmd == "exit" || cmd == "detach") {
        emitLine("bye");
        return false;
    }
    else {
        emitFmt("未知命令: {}  (help 看用法)", args[0]);
    }
    return true;
}

void CommandServer::printHelp() const {
    emitLine("命令一览:");
    emitLine("  status                     引擎定位总览");
    emitLine("  rescan                     重新定位 GObjects/GNames/属性链");
    emitLine("  globals                    GEngine / GWorld 详情");
    emitLine("  hooks                      帧钩子状态");
    emitLine("  hook                       安装 Present 帧钩子(可选, 采集不需要)");
    emitLine("");
    emitLine("  objects [filter=子串] [class=类名] [limit=N] [skip=N]");
    emitLine("                             列出 UObject");
    emitLine("  find <名字>                按名字查对象(支持子串, 全表扫描)");
    emitLine("  class <类名>               看某个类: 大小 / 父类 / 实例数");
    emitLine("");
    emitLine("  props <类名>               列出该类的属性链与偏移(含继承)");
    emitLine("  get <对象名> <属性名>      读某个对象的属性值");
    emitLine("");
    emitLine("  world                      当前 UWorld / PersistentLevel");
    emitLine("  actors [limit=N] [class=类名]");
    emitLine("                             枚举关卡里的 Actor");
    emitLine("");
    emitLine("  mv                         读玩家移动组件上的关键 float");
    emitLine("  attrmv                     找 ATR_Movement 并打印属性块");
    emitLine("  currency [class=子串] [limit=N]");
    emitLine("                             货币持有者与 6 个字段; class= 时列候选类");
    emitLine("");
    emitLine("  quit                       结束命令循环");
}

// ---------------------------------------------------------------------------
void CommandServer::cmdStatus() {
    emitLine("=== 引擎定位状态 ===");
    emit(eng_.report());
}

void CommandServer::cmdRescan() {
    emitLine("重新定位中...");
    if (eng_.rescan()) emitLine("定位成功。");
    else              emitLine("定位失败 —— 游戏可能还没进到关卡, 稍后再试。");
    emit(eng_.report());
}

void CommandServer::cmdGlobals() {
    auto line = [&](game::ue::GlobalSlot const& g) {
        if (!g.value) { emitFmt("{:<10} 未定位", g.label); return; }
        emitFmt("{:<10} 槽位 {} → {} {}   校验={}   来源={}",
                 g.label, hex(g.address, 16), g.objectClass, g.objectName,
                 g.verified ? "通过" : "未通过", g.how);
    };
    emitLine("=== 引擎全局 ===");
    line(eng_.gengine());
    line(eng_.gworld());
    emitLine("");
    emitFmt("模块基址 {}  大小 {}", hex(eng_.moduleBase(), 16), humanBytes(eng_.moduleSize()));
}

void CommandServer::cmdHooks() {
    auto const& st = hooks().status();
    emitLine("=== 帧钩子 (MinHook) ===");
    emitFmt("状态        : {}", st.installed ? "已安装" : (st.attempted ? "安装失败" : "未安装"));
    if (st.installed) {
        emitFmt("目标        : {}", st.target);
        emitFmt("定位方式    : {}", st.how);
        emitFmt("累计帧数    : {}", thousands(frameCount()));
        emitFmt("估算 FPS    : {:.1f}", st.fps);
    } else if (!st.error.empty()) {
        emitFmt("失败原因    : {}", st.error);
    } else {
        emitLine("说明        : 帧钩子不在启动路径上, 用 `hook` 命令显式安装。");
        emitLine("              只读采集(objects/props/actors 等)不需要它。");
    }
}

void CommandServer::cmdHookInstall() {
    if (hooks().installed()) {
        emitLine("帧钩子已经装过了。用 `hooks` 看状态。");
        return;
    }
    emitLine("正在安装 Present 帧钩子...");
    if (installFrameHook()) {
        auto const& st = hooks().status();
        emitFmt("已挂上: {}", st.target);
        emitFmt("定位方式: {}", st.how);
    } else {
        emitFmt("安装失败: {}", hooks().status().error);
        emitLine("（不影响采集功能 —— 命令驱动的采样不依赖帧回调）");
    }
}

// ---------------------------------------------------------------------------
void CommandServer::cmdObjects(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }

    const auto filter = optValue(args, "filter");
    const auto cls    = optValue(args, "class");
    const auto limit  = optInt(args, "limit").value_or(64);
    const auto skip   = optInt(args, "skip").value_or(0);

    emitFmt("=== UObject 列表 (filter={} class={} limit={} skip={}) ===",
             filter ? *filter : "*", cls ? *cls : "*", limit, skip);

    int64_t shown = 0, scanned = 0;
    eng_.objects().for_each([&](ObjectStat const& st) {
        ++scanned;
        if (scanned <= skip) return true;

        if (filter && !icontains(st.fullName, *filter) && !icontains(st.name, *filter)) return true;
        if (cls && !iequals(st.className, *cls) && !icontains(st.className, *cls)) return true;

        emitFmt("  [{:>7}] {:>16}  {:<28} {}",
                 st.index, hex(st.address, 16), sanitize(st.className, 28),
                 sanitize(st.name, 64));
        ++shown;
        return shown < limit;
    });

    emitFmt("--- 扫描 {} 个槽位, 显示 {} 条 (全表 {} 个) ---",
             thousands(static_cast<uint64_t>(scanned)), shown,
             thousands(static_cast<uint64_t>(eng_.objects().numElements())));
}

void CommandServer::cmdFind(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }
    if (args.empty())   { emitLine("用法: find <名字子串>"); return; }

    const std::string needle = args[0];
    const int64_t  limit = optInt(args, "limit").value_or(32);

    emitFmt("=== 查找 \"{}\" ===", needle);
    int64_t shown = 0;
    eng_.objects().for_each([&](ObjectStat const& st) {
        if (!icontains(st.name, needle) && !icontains(st.className, needle)) return true;
        emitFmt("  [{:>7}] {:>16}  {:<32} {}",
                 st.index, hex(st.address, 16), sanitize(st.className, 32),
                 sanitize(st.name, 64));
        return ++shown < limit;
    });
    if (shown == 0) emitLine("  (无匹配)");
}

void CommandServer::cmdClass(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }
    if (args.empty())   { emitLine("用法: class <类名>  例: class Character"); return; }

    const uint64_t cls = eng_.objects().findClass(args[0]);
    if (!cls) { emitFmt("找不到类 {}", args[0]); return; }

    emitLine("=== 类信息 ===");
    emitFmt("类对象地址  : {}", hex(cls, 16));
    emitFmt("类名        : {}", eng_.objects().nameOf(cls));
    emitFmt("类的大小    : {} 字节", eng_.reflection().structSize(cls));

    const uint64_t super = eng_.reflection().superStruct(cls);
    emitFmt("父类        : {}", super ? eng_.objects().nameOf(super) : "(无)");

    const int32_t props = static_cast<int32_t>(eng_.reflection().propertiesOf(cls).size());
    emitFmt("本类属性数  : {}", props);

    // 统计实例个数(全表扫一遍)
    int64_t instances = 0;
    const std::string myName = eng_.objects().nameOf(cls);
    eng_.objects().for_each([&](ObjectStat const& st) {
        if (iequals(st.className, myName)) ++instances;
        return true;
    });
    emitFmt("实例个数    : {}", thousands(static_cast<uint64_t>(instances)));
}

// ---------------------------------------------------------------------------
//  probeFFieldNameOffset — 定出 FField::NamePrivate 的真实偏移
//
//  FName 在内存里是一个 32 位索引(比较索引)。FField 的头几个字段是固定的,
//  后面紧跟 NamePrivate。这里从 0x18 到 0x48 每 4 字节试一次, 哪个位置解出
//  的 FName 是**有意义的名字**(而不是空/"None"), 哪个就是真实偏移。
//
//  判据要严一点: 垃圾值恰好落在 FNamePool 范围内并解出字符串是可能的, 所以
//  同时打印所有候选, 由人按上下文判断 —— 通常真偏移只有一两个候选, 且解出的
//  名字是"MaxWalkSpeed"这种一眼能认的。
// ---------------------------------------------------------------------------
void CommandServer::probeFFieldNameOffset(uint64_t fieldAddr) {
    if (!fieldAddr) return;

    emitFmt("=== 在 {} 上找 FName 索引偏移 ===", hex(fieldAddr, 16));
    emitLine("      off    u32 value    resolves-to");
    emitLine("      " + std::string(50, '-'));

    int good = 0;
    for (uint32_t off = 0x18; off <= 0x48; off += 4) {
        int32_t idx = 0;
        if (!safeRead(&idx, reinterpret_cast<const void*>(fieldAddr + off), 4)) continue;
        if (idx <= 0) continue;                       // 0 = "None", 不是我们要的

        const std::string nm = eng_.names().resolve(idx);
        if (nm.empty()) continue;

        ++good;
        emitFmt("      +{:#04x}   {:<10}  \"{}\"", off, idx, sanitize(nm, 40));
    }

    if (good == 0) {
        emitLine("      (没有解出任何名字 —— 这个地址可能不是 FField)");
    } else {
        emitLine("");
        emitLine("=> 真正的 FField::NamePrivate 偏移应当是解出属性名的那个。");
    }
}

// ---------------------------------------------------------------------------
//  findPropertyDirect — 绕开 FField 链, 直接从 UClass 找回属性偏移
//
//  本构建的 UStruct 布局被改过, 链式遍历不可用(见 docs/reverse/reflection-limits.md)。
//  绕行思路: UClass 内部必然存着每个属性的 FName 索引, 于是
//     1. 在 GNames 里定位属性名对应的 FName 索引
//     2. 扫 UClass 内存找存放该索引的位置
//     3. 在命中对象上扫"看起来像结构体偏移"的小整数, 得到 Offset_Internal 候选
// ---------------------------------------------------------------------------
void CommandServer::findPropertyDirect(std::string_view className, std::string_view propName) {
    namespace sc = epsilon::game::offsets::scan;

    if (!eng_.ready()) { emitLine("引擎未定位"); return; }

    const uint64_t cls = eng_.objects().findClass(className);
    if (!cls) { emitFmt("找不到类 {}", className); return; }

    emitFmt("=== 直接定位 {}::{} ===", className, propName);
    emitFmt("UClass @ {}", hex(cls, 16));

    // ---- 1) 在名字池里顺序找属性名, 拿到它的 FName 索引 ----
    const auto found = eng_.names().findByName(propName, 20000);
    if (!found) {
        emitLine("在名字池第一个块里没找到该名字 —— 换一个更常见的属性名试试");
        return;
    }
    const int32_t foundIndex = found->first;
    emitFmt("FName 索引 = {} (在名字池块 0 内定位到)", foundIndex);
    // 自检: 用索引反查一次, 确认推出来的索引是对的。
    emitFmt("反查该索引 -> \"{}\"", sanitize(eng_.names().resolve(foundIndex), 48));

    // ---- 2) 扫 UClass 内存, 找存放该索引的位置 ----
    emitLine("");
    emitLine("在 UClass 里搜该 FName 索引:");
    emitLine("      off     candidate          nearest-heap-ptr      looks-like-offset");
    emitLine("      " + std::string(70, '-'));

    int hits = 0;
    for (uint32_t off = sc::classBodyFirst; off <= sc::classBodyLast && hits < 12; off += 4) {
        auto v = epsilon::game::readI32(cls, off);
        if (!v) break;
        if (*v != foundIndex) continue;

        ++hits;
        // 命中处附近找一个指向堆的指针 —— FProperty 对象就在附近被引用。
        uint64_t nearPtr = 0;
        uint32_t nearOff = 0;
        for (int d = -32; d <= 32; d += 8) {
            const uint32_t probe = static_cast<uint32_t>(static_cast<int32_t>(off) + d);
            auto cand = epsilon::game::readPtr(cls, probe);
            if (!cand) continue;
            if (*cand < 0x10000 || *cand > 0x7FFFFFFFFFFF) continue;
            if (*cand >= eng_.moduleBase() && *cand < eng_.moduleBase() + eng_.moduleSize()) continue;
            nearPtr = *cand;
            nearOff = probe;
            break;
        }

        // 在那个候选对象上扫"像结构体偏移的小整数"(常见的类内字段偏移范围)。
        uint32_t likeOffset = 0;
        if (nearPtr) {
            for (uint32_t o = sc::propertyLikeOffsetFirst; o <= sc::propertyLikeOffsetLast; o += 4) {
                auto iv = epsilon::game::readI32(nearPtr, o);
                if (!iv) continue;
                if (*iv > 0x40 && *iv < 0x2000) {
                    likeOffset = static_cast<uint32_t>(*iv);
                    break;
                }
            }
        }

        emitFmt("      +{:#06x}  {:<16} {:<18} {}",
                 off, hex(static_cast<uint64_t>(*v), 8),
                 nearPtr ? fmt("{}(+{:#x})", hex(nearPtr, 16), nearOff) : std::string("-"),
                 likeOffset ? fmt("+{:#x}", likeOffset) : std::string("-"));
    }

    if (hits == 0) {
        emitLine("      (没找到) —— 该属性名可能不在 UClass 对象体内, 或索引推导有偏");
    } else {
        emitLine("");
        emitLine("=> 命中处即该属性的 FName 存放位置; 旁边的堆对象应为它的 FProperty,");
        emitLine("   其中形如 +0xNNN 的小整数就是 Offset_Internal 的候选值。");
    }
}

// ---------------------------------------------------------------------------
//  scanForActorArray — 在 ULevel 对象上找回 Actors 数组
//
//  实测这个构建上 ULevel::Actors 的静态偏移是错的(PersistentLevel 读出
//  0 actors)。UE 的 TArray 在内存里是 {T* Data; int32 Num; int32 Max} 三个
//  连续字段, 特征很强:
//      * Data 是堆指针(不是 "None" 那种空槽)
//      * 0 < Num <= Max
//      * Max 量级合理(关卡 actor 数通常几十到几万, 不会到百万)
//  于是按这个特征扫对象体的每个 8 字节槽, 把候选全列出来 —— 比继续猜常量可靠。
// ---------------------------------------------------------------------------
void CommandServer::scanForActorArray(uint64_t levelObj) {
    if (!levelObj) return;

    namespace sc = epsilon::game::offsets::scan;

    emitFmt("=== 在 {} ({}) 上扫 TArray 形态的 Actors ===",
             hex(levelObj, 16), sanitize(eng_.objects().nameOf(levelObj), 32));
    emitLine("      slot   data               num      max    first-elem-class");
    emitLine("      " + std::string(72, '-'));

    struct Cand { uint32_t off; uint64_t data; int32_t num; int32_t max; };
    std::vector<Cand> candidates;

    namespace arr = epsilon::game::offsets::array;

    // 按 TArray 的形态特征({ptr, num, max}, num<=max, 量级合理)扫对象体前部。
    for (uint32_t off = sc::levelBodyFirst; off <= sc::levelBodyLast; off += 8) {
        auto data = epsilon::game::readPtr(levelObj, off);
        auto num  = epsilon::game::readI32(levelObj, off + arr::num);
        auto max  = epsilon::game::readI32(levelObj, off + arr::max);
        if (!data || !num || !max) continue;

        if (*data < 0x10000 || *data > 0x7FFFFFFFFFFF) continue;
        if (*num <= 0 || *max <= 0) continue;
        if (*num > *max) continue;
        if (*max > 4'000'000) continue;
        // 元素大小 × max 不应该离谱(元素至少 8 字节)
        if (static_cast<uint64_t>(*max) * 8ull > (1ull << 34)) continue;

        candidates.push_back(Cand{off, *data, *num, *max});
    }

    if (candidates.empty()) {
        emitLine("      (没有找到任何 TArray 候选)");
        return;
    }

    for (auto const& c : candidates) {
        // 读第一个元素看它是不是合法 UObject —— 这能把"碰巧长得像 TArray"的槽筛干净。
        std::string firstCls = "?";
        if (auto first = epsilon::game::readPtr(c.data, 0)) {
            firstCls = sanitize(eng_.objects().classNameOf(*first), 24);
        }
        emitFmt("      +{:#04x}  {:<18} {:>7}  {:>7}   {}",
                 c.off, hex(c.data, 16), c.num, c.max, firstCls);
    }

    emitLine("");
    emitLine("=> 上面 data 指向的对象类名如果是 Actor 系, 那个 slot 就是 ULevel::Actors。");
}

// ---------------------------------------------------------------------------
//  dumpPointers — 按指针逐个解释一段内存
//
//  前一个命令给出候选地址后, 用这个核对它到底指向什么: 是 UObject(能解出
//  类名/对象名) 还是普通数据。两级的诊断缺一不可 —— 只看"像不像指针"会把
//  一堆无关槽也算进来。
// ---------------------------------------------------------------------------
void CommandServer::dumpPointers(uint64_t addr, int count) {
    if (!addr) return;
    if (count <= 0) count = 16;
    if (count > 64) count = 64;

    emitFmt("=== {} 起 {} 个 8 字节槽 ===", hex(addr, 16), count);
    for (int i = 0; i < count; ++i) {
        const uint64_t slot = addr + static_cast<uint64_t>(i) * 8;
        uint64_t v = 0;
        if (!safeRead(&v, reinterpret_cast<const void*>(slot), 8)) {
            emitFmt("  +{:#06x}  <读不到>", i * 8);
            break;
        }
        std::string note;
        if (v >= 0x10000 && v <= 0x7FFFFFFFFFFF) {
            // 先试 UObject: 类名 + 对象名
            const std::string cls = eng_.objects().classNameOf(v);
            const std::string nm  = eng_.objects().nameOf(v);
            if (!cls.empty()) {
                note = fmt("UObject {} :: {}", sanitize(cls, 28), sanitize(nm, 36));
            } else {
                // 再试 FName(低位 32 位是 FName 索引)
                const int32_t idx = static_cast<int32_t>(v & 0xFFFFFFFFull);
                const std::string fname = eng_.names().resolve(idx);
                if (!fname.empty()) note = fmt("FName \"{}\"", sanitize(fname, 32));
                else                note = "(不指向 UObject, 也不是 FName)";
            }
        } else if (v != 0) {
            note = fmt("(小整数 {})", v);
        }
        emitFmt("  +{:#06x}  {:<18} {}", i * 8, hex(v, 16), note);
    }
}

// ---------------------------------------------------------------------------
//  cmdCurrency — ATR_Currency 上绿宝石等货币属性
//
//  货币走 GAS: 每个属性在声明偏移处放一个共享描述指针, 真正的 {BaseValue, CurrentValue}
//  在声明值 + 8。这里把整块属性区摊开, 便于偏移失效后重新判读。
// ---------------------------------------------------------------------------
void CommandServer::cmdCurrency(std::vector<std::string> const& args) {
    namespace ca = epsilon::game::offsets::currencyAttribute;

    if (auto cls = optValue(args, "class")) {
        const int limit = static_cast<int>(optInt(args, "limit").value_or(16));
        emitFmt("=== 类名含 {} 的对象(前 {} 个) ===", *cls, limit);
        emitLine("   地址                类名                          名字");
        emitLine("   " + std::string(78, '-'));
        int shown = 0;
        eng_.objects().for_each([&](ObjectStat const& st) {
            if (!icontains(st.className, *cls)) return true;
            if (shown >= limit) return false;
            ++shown;
            emitFmt("   {}  {:<28}  {}",
                     hex(st.address, 16), sanitize(st.className, 28),
                     sanitize(st.name, 32));
            return true;
        });
        if (shown == 0) emitLine("   (没有命中)");
        return;
    }

    auto& resolver = feature::currency();
    resolver.resolve(eng_, true);

    if (!resolver.ready()) {
        emitFmt("未解析到货币属性集: {}", resolver.lastError());
        emitLine("用 `currency class=ATR_` 看候选对象。");
        return;
    }

    const d2::CurrencyAttributeSet set(resolver.set().address());
    emitFmt("属性集      : {}  ({})", hex(set.address(), 16),
             resolver.target().setName);
    emitFmt("外层对象    : {}",
             resolver.target().outerClass.empty() ? "(未匹配到玩家 pawn)"
                                                  : resolver.target().outerClass);
    emitFmt("形态通过    : {} 个 {}", resolver.candidates(),
             epsilon::game::offsets::currencyOwnerClass);
    emitLine("");
    emitLine("   声明      数据         属性                            值");
    emitLine("   " + std::string(72, '-'));
    for (size_t i = 0; i < d2::currencyAttributeCount; ++i) {
        const auto attribute = static_cast<d2::CurrencyAttribute>(i);
        auto v = set.read(attribute);
        emitFmt("   +{:#06x}  +{:#06x}    {:<30} {}",
                 d2::declaredCurrencyOffset(attribute),
                 d2::currencyDataOffset(attribute),
                 std::string(d2::currencyAttributeName(attribute)),
                 v ? fmt("{:.4f}", *v) : std::string("?"));
    }

    emitLine("");
    emitFmt("=== 属性区原样({:#x}..{:#x}, 步长 {:#x}) ===", ca::blockFirst, ca::blockLast,
             ca::blockStep);
    for (uint32_t off = ca::blockFirst; off < ca::blockLast; off += ca::blockStep) {
        auto base = set.readAt(off + ca::dataShift);
        auto current = set.readAt(off + ca::dataShift + ca::currentValueDelta);
        emitFmt("   +{:#06x}  base={}  current={}",
                 off,
                 base ? fmt("{:.4f}", *base) : std::string("?"),
                 current ? fmt("{:.4f}", *current) : std::string("?"));
    }

    if (auto want = optValue(args, "find")) {
        // 按数值反查: 在实例体内找 float / int32 等于给定值的槽。
        // 偏移失效时用它把真值那格直接钉出来, 不必逐个属性猜。
        double target = 0.0;
        try { target = std::stod(*want); } catch (...) {
            emitLine("find= 数值解析失败");
            return;
        }
        emitFmt("=== 在 {} 上反查数值 {} ===", hex(set.address(), 16), target);
        int hits = 0;
        for (uint32_t off = 0x40; off < 0x400; off += 4) {
            auto f = set.readAt(off);
            auto i = epsilon::game::readI32(set.address(), off);
            const bool asFloat = f && std::fabs(static_cast<double>(*f) - target) <=
                                           std::fmax(0.01, std::fabs(target) * 1e-4);
            const bool asInt = i && std::fabs(static_cast<double>(*i) - target) <= 0.5;
            if (!asFloat && !asInt) continue;
            ++hits;
            emitFmt("   +{:#06x}  {}{}", off,
                     f ? fmt("float={:.4f}", *f) : std::string("?"),
                     asInt ? fmt("   int={}", *i) : std::string());
        }
        if (hits == 0) emitLine("   (没有命中 —— 这个实例里没有该数值)");
        else emitFmt("   命中 {} 处", hits);
    }

    if (auto setVal = optValue(args, "set")) {
        double want = 0.0;
        try { want = std::stod(*setVal); } catch (...) {
            emitLine("set= 数值解析失败");
            return;
        }
        const auto attribute = d2::CurrencyAttribute::emeralds;
        const auto before = set.read(attribute);
        const bool ok = set.write(attribute, static_cast<float>(want));
        const auto after = set.read(attribute);
        emitFmt("写 Emeralds: {} -> {}  (write {})",
                 before ? fmt("{:.3f}", *before) : std::string("?"),
                 after ? fmt("{:.3f}", *after) : std::string("?"),
                 ok ? "ok" : "FAILED");
        emit("   采样: ");
        int reverted = 0;
        for (int i = 0; i < 8; ++i) {
            ::Sleep(40);
            auto now = set.read(attribute);
            if (!now) { emit("?"); break; }
            emit(fmt("{:.1f} ", *now));
            if (std::fabs(*now - static_cast<float>(want)) > 0.01f) ++reverted;
        }
        emitLine("");
        emitLine(reverted == 0 ? "   -> 保住了"
                               : fmt("   -> 被改回 {}/8 次(GAS 会在聚合时重算)", reverted));
    }
}

// ---------------------------------------------------------------------------
//  cmdOodle — 借游戏自己的 OodleLZ_Decompress 解压一个文件
//
//  Oodle 静态链接在映像里(发行版不带 oo2core*.dll), 没有现成的解压器可用,
//  而 pak 内容全是 Oodle 压缩的 —— 所以只能调游戏自己的那份。
//  调用是纯计算, 不改游戏状态。
// ---------------------------------------------------------------------------
void CommandServer::cmdOodle(std::vector<std::string> const& args) {
    namespace od = epsilon::game::offsets::oodle;

    if (args.size() < 3) {
        emitLine("用法: oodle <输入文件> <输出文件> <原始长度>");
        emitLine("  输入应当是**已解密**的 Oodle 压缩数据");
        return;
    }

    const uint64_t fn = eng_.moduleBase() + od::decompressRva;
    if (!fn || !eng_.moduleBase()) {
        emitLine("拿不到模块基址, 无法定位 OodleLZ_Decompress");
        return;
    }

    uint64_t rawLen = 0;
    try { rawLen = std::stoull(args[2], nullptr, 0); } catch (...) {
        emitLine("原始长度解析失败");
        return;
    }
    if (rawLen == 0 || rawLen > (64ull << 20)) {
        emitLine("原始长度不合理(限 64 MiB 以内)");
        return;
    }

    // 读输入
    HANDLE in = ::CreateFileA(args[0].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        emitFmt("打不开输入 {} (GetLastError={})", args[0], ::GetLastError());
        return;
    }
    LARGE_INTEGER li{};
    ::GetFileSizeEx(in, &li);
    std::vector<uint8_t> comp(static_cast<size_t>(li.QuadPart));
    DWORD got = 0;
    const bool readOk = ::ReadFile(in, comp.data(), static_cast<DWORD>(comp.size()),
                                   &got, nullptr) != 0;
    ::CloseHandle(in);
    if (!readOk || got != comp.size()) {
        emitLine("读输入失败");
        return;
    }

    std::vector<uint8_t> plain(static_cast<size_t>(rawLen), 0);

    using DecompressFn = int64_t(__fastcall*)(const void*, int64_t, void*, int64_t,
                                              int32_t, int32_t, int32_t,
                                              void*, int64_t, void*, void*,
                                              void*, int64_t, uint32_t);
    auto decompress = reinterpret_cast<DecompressFn>(fn);

    emitFmt("OodleLZ_Decompress @ {}  输入 {} 字节 -> 期望 {} 字节",
             hex(fn, 16), comp.size(), rawLen);

    const int64_t n = decompress(comp.data(), static_cast<int64_t>(comp.size()),
                                 plain.data(), static_cast<int64_t>(plain.size()),
                                 od::fuzzSafeYes, od::checkCrcNo, od::verbosityNone,
                                 nullptr, 0, nullptr, nullptr, nullptr, 0, 0);
    emitFmt("返回 {}", n);
    if (n <= 0) {
        emitLine("解压失败 —— 输入不是合法的 Oodle 流(或原始长度不对)");
        return;
    }

    HANDLE out = ::CreateFileA(args[1].c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        emitFmt("写输出失败 GetLastError={}", ::GetLastError());
        return;
    }
    DWORD wrote = 0;
    ::WriteFile(out, plain.data(), static_cast<DWORD>(n), &wrote, nullptr);
    ::CloseHandle(out);
    emitFmt("已写出 {}  ({} 字节)", args[1], wrote);
}

// ---------------------------------------------------------------------------
//  cmdMovement — 读出玩家移动组件上几个关键 float 的当前值
//
//  打开大跳/加速前后各跑一次, 数值变化就能直接印证模块是否真的写进去了。
//  这条命令用的是从二进制属性表读出的已知偏移(本构建反射解不出属性链)。
// ---------------------------------------------------------------------------
void CommandServer::cmdMovement() {
    auto& mv = feature::movement();
    if (auto* eng = feature::game().engine; eng != nullptr) {
        mv.resolve(*eng, true);
    } else {
        mv.resolve(eng_, true);
    }

    emitLine("=== player movement ===");
    if (!mv.ready()) {
        emitFmt("not resolved: {}", mv.lastError());
        return;
    }

    const auto& target = mv.target();
    const auto& layout = mv.layout();
    emitFmt("pawn     : {} ({})", sanitize(target.pawnClass, 36), hex(target.pawn, 16));
    emitFmt("movement : {} ({})", sanitize(target.movementClassName, 40),
            hex(target.movement, 16));
    emitFmt("offsets  : {}", mv.offsetSource().empty() ? "?" : mv.offsetSource());
    emitLine("");
    emitFmt("  {:<24} {:>8}  {:>12}", "property", "offset", "value");
    emitLine("  " + std::string(48, '-'));

    auto show = [&](d2::MovementField field) {
        const std::string name(d2::movementFieldName(field));
        const uint32_t off = layout.offsetOf(field);
        if (!off) {
            emitFmt("  {:<24} {:>8}  {:>12}", name, "-", "(no offset)");
            return;
        }
        const auto v = mv.read(field);
        emitFmt("  {:<24} {:>8}  {:>12}", name, fmt("+{:#x}", off),
                v ? fmt("{:.3f}", *v) : std::string("read failed"));
    };
    show(d2::MovementField::maxWalkSpeed);
    show(d2::MovementField::jumpZVelocity);
    show(d2::MovementField::gravityScale);
    show(d2::MovementField::airControl);
    show(d2::MovementField::maxAcceleration);
    show(d2::MovementField::movementSpeedMultiplier);

    // 邻域浮点: 相邻槽里常有"值 / 它的倒数"这类派生量, 只改其中一个会破坏不变量。
    if (target.movement) {
        emitLine("");
        emitLine("  --- float neighbourhood ---");
        for (uint32_t base : epsilon::game::offsets::scan::movementNeighbourhoodBases) {
            emitFmt("  +{:#x}:", base);
            for (int i = 0; i < epsilon::game::offsets::scan::movementNeighbourhoodFloats; ++i) {
                const uint32_t off = base + static_cast<uint32_t>(i) * 4u;
                const auto v = mv.component().readAt(off);
                emitFmt("      +{:#05x} {:<12}", off,
                        v ? fmt("{:.4f}", *v) : std::string("?"));
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  cmdAttributeMovement — 找出玩家的 ATR_Movement 并读出速度属性
//
//  偏移表见 docs/offsets/movement-attributes.md。
// ---------------------------------------------------------------------------
void CommandServer::cmdAttributeMovement() {
    namespace ma = epsilon::game::offsets::movementAttribute;

    auto& mvm = feature::movement();
    if (auto* eng = feature::game().engine; eng != nullptr) mvm.resolve(*eng, true);
    else mvm.resolve(eng_, true);

    const uint64_t pawn = mvm.ready() ? mvm.target().pawn : 0;
    emitFmt("player pawn : {}", pawn ? hex(pawn, 16) : std::string("(未解析)"));
    if (!pawn) { emitLine("先让玩家的移动组件解析成功再看属性集"); return; }

    struct Hit { uint64_t addr; uint64_t via; std::string viaClass; int depth; };
    std::vector<Hit> hits;
    int total = 0;

    eng_.objects().for_each([&](ObjectStat const& st) {
        if (st.className != "ATR_Movement") return true;
        ++total;
        uint64_t o = st.address;
        for (int depth = 1; depth <= 8; ++depth) {
            o = eng_.objects().outerOf(o);
            if (!o) break;
            if (o == pawn) {
                hits.push_back({st.address, o, eng_.objects().classNameOf(o), depth});
                break;
            }
        }
        return true;
    });

    emitFmt("ATR_Movement 实例总数: {}, 其中 Outer 链能走到玩家 pawn 的: {}",
             total, hits.size());
    if (hits.empty()) {
        emitLine("没有直接挂到 pawn 上的实例 —— 可能挂在 PlayerState 上, 换个判别方式试试");
        return;
    }

    const std::string speedOffsetText = fmt("+{:#x} 倍率 / +{:#x} 摩擦",
                                            ma::movementSpeedMultiplier,
                                            ma::movementFriction);

    emitLine("");
    emitLine("   地址                链上命中                        值(" + speedOffsetText + ")");
    emitLine("   " + std::string(76, '-'));
    for (auto const& h : hits) {
        const d2::MovementAttributeSet set(h.addr);
        auto mult = set.readAt(ma::movementSpeedMultiplier);
        auto fric = set.readAt(ma::movementFriction);
        emitFmt("   {}  {} (+{})      {}  {}",
                 hex(h.addr, 16), sanitize(h.viaClass, 24), h.depth,
                 mult ? fmt("{:.4f}", *mult) : std::string("?"),
                 fric ? fmt("{:.4f}", *fric) : std::string("?"));
    }

    // 对第一个命中做一次实测: 写倍率并观察是否保持。
    const uint64_t target = hits.front().addr;
    const d2::MovementAttributeSet set(target);

    // 先把整个属性块按 float 打出来: 要看清楚哪些槽像"倍率"(1.0)、哪些像
    // "速度量级"(几百)、哪些是 0。属性之间间隔 0x10。
    emitLine("");
    emitFmt("=== {} 的属性块 (+{:#x} 起 {} 个 float) ===", hex(target, 16),
            ma::blockDumpStart, ma::blockDumpFloats);
    emitLine("    off      float0      float1  |  按属性对齐");
    emitLine("    " + std::string(66, '-'));

    auto tagForDeclaredOffset = [](uint32_t off) -> std::string_view {
        for (size_t i = 0; i < d2::movementAttributeCount; ++i) {
            const auto attr = static_cast<d2::MovementAttribute>(i);
            if (d2::declaredAttributeOffset(attr) == off) return d2::movementAttributeName(attr);
        }
        return {};
    };

    for (uint32_t i = 0; i < ma::blockDumpFloats; i += 2) {
        const uint32_t off = ma::blockDumpStart + i * 4;
        auto v0 = set.readAt(off);
        auto v1 = set.readAt(off + 4);
        if (!v0) break;

        const std::string_view tag = tagForDeclaredOffset(off);
        emitFmt("    +{:#06x}  {:>10}  {:>10}{}",
                 off,
                 fmt("{:.4f}", *v0),
                 v1 ? fmt("{:.4f}", *v1) : std::string("?"),
                 tag.empty() ? std::string() : fmt("  <- {}", tag));
    }

    // 试写 BaseValue 观察 GAS 是否会在聚合时重算。
    const uint32_t speedOffset = ma::movementSpeedMultiplier;
    emitLine("");
    emitFmt("=== 在 {} 上试写 MovementSpeedMultiplier(+{:#x}) ===", hex(target, 16),
            speedOffset);
    auto before = set.readAt(speedOffset);
    const bool ok = set.writeAt(speedOffset, 5.0f);
    auto immediate = set.readAt(speedOffset);
    emitFmt("   {} -> {}   (write {})",
             before ? fmt("{:.4f}", *before) : std::string("?"),
             immediate ? fmt("{:.4f}", *immediate) : std::string("?"),
             ok ? "ok" : "FAILED");
    emit("   samples: ");
    int reverted = 0;
    for (int i = 0; i < 8; ++i) {
        ::Sleep(40);
        auto now = set.readAt(speedOffset);
        if (!now) { emit("?"); break; }
        emit(fmt("{:.2f} ", *now));
        if (std::fabs(*now - 5.0f) > 0.01f) ++reverted;
    }
    emitLine("");
    if (reverted == 0) emitLine("   -> 保住了! 该属性可以直接改");
    else emitFmt("   -> 被改回 {}/8 次(GAS 会在聚合时重算)", reverted);

    emitLine("");
    emitFmt("注: FGameplayAttributeData = {{BaseValue, CurrentValue}}, 分别在 +{:#x} 与 +{:#x}。",
            speedOffset, speedOffset + ma::currentValueDelta);
    emitLine("    上面的测试只动了 BaseValue; 若被改回, 应改 CurrentValue 或两者都改。");
}

// ---------------------------------------------------------------------------
//  dumpFloats — 按 float 解读一段内存
//
//  排查"某个偏移上到底是速度、倒数、还是别的"时光看十六进制没用 ——
//  100.0f 是 0x42C80000, 和游戏里显示的 100 对不上眼。这里直接按浮点打印。
// ---------------------------------------------------------------------------
void CommandServer::dumpFloats(uint64_t addr, int count) {
    if (!addr) return;
    if (count <= 0) count = 16;
    if (count > 64) count = 64;

    emitFmt("=== {} 起 {} 个 float ===", hex(addr, 16), count);
    emitLine("    offset      float            hex");
    emitLine("    " + std::string(40, '-'));
    for (int i = 0; i < count; ++i) {
        float v = 0.0f;
        if (!safeRead(&v, reinterpret_cast<const void*>(addr + static_cast<uint64_t>(i) * 4),
                      sizeof(v))) {
            emitLine("    (read failed)");
            break;
        }
        uint32_t raw = 0;
        std::memcpy(&raw, &v, 4);
        emitFmt("    +{:#06x}   {:>14.4f}   {:#010x}", i * 4, v, raw);
    }
}

// ---------------------------------------------------------------------------
//  setMovementValue — 手工写一个移动属性
//
//  与面板里开模块的区别: 这里是一次性直写, 不做"每帧补写"。
//  两个用途:
//    1. 对照测试 —— 直写一次看游戏是保留还是立刻覆盖, 立刻就能区分
//       "写不进去" 与 "写进去被覆盖"
//    2. 验证偏移 —— 改完值看游戏内表现是否变化
// ---------------------------------------------------------------------------
void CommandServer::setMovementValue(std::string_view field, float value) {
    auto& mv = feature::movement();
    if (!mv.ready()) {
        emitFmt("movement not resolved: {}", mv.lastError());
        return;
    }

    uint32_t off = 0;
    // 支持直接给原始偏移(以 '+' 或 '0' 开头) —— 排查阶段经常要在已知字段周围的
    // 槽上做对照写入(例如怀疑真值落在相邻 4 字节上)。
    if (!field.empty() && (field[0] == '+' || field[0] == '0')) {
        try { off = static_cast<uint32_t>(std::stoll(std::string(field), nullptr, 0)); }
        catch (...) { off = 0; }
    }
    if (!off) {
        for (size_t i = 0; i < d2::movementFieldCount; ++i) {
            const auto f = static_cast<d2::MovementField>(i);
            if (!iequals(field, d2::movementFieldName(f))) continue;
            off = mv.layout().offsetOf(f);
            break;
        }
    }
    if (!off) {
        emitLine("unknown field. use a known field name (MaxWalkSpeed / JumpZVelocity / "
                 "MaxAcceleration / AirControl / GravityScale) or a raw offset (+0x230)");
        return;
    }

    auto before = mv.component().readAt(off);
    const bool ok = mv.component().writeAt(off, value);

    // 立刻回读是关键的一步: 它把"根本没写进去(地址错/写被丢弃)"与
    // "写进去了但被游戏改回来"两种情况分开 —— 只看写入返回值分不清。
    auto immediate = mv.component().readAt(off);

    emitFmt("{} @ {} : {} -> {}", field, fmt("+{:#x}", off),
             before ? fmt("{:.3f}", *before) : std::string("?"),
             immediate ? fmt("{:.3f}", *immediate) : std::string("?"));

    if (!ok) {
        emitLine("    write 返回失败(内存不可写)");
        return;
    }
    if (!immediate) { emitLine("    回读失败, 无法判断"); return; }
    if (std::fabs(*immediate - value) > 0.001f) {
        emitFmt("    !! 写入未生效: 期望 {:.3f}, 回读 {:.3f} —— 该地址写不进去 "
                "(不是被游戏覆盖, 而是我们的写没有落地)", value, *immediate);
        return;
    }
    emitLine("    写入已生效(回读一致); 下面继续观察是否被改回");

    // 连续采样才能看出覆盖是持续发生(每帧重算)还是偶发(某个事件触发)。
    emit("    samples: ");
    int reverted = 0;
    for (int i = 0; i < 8; ++i) {
        ::Sleep(40);
        auto now = mv.component().readAt(off);
        if (!now) { emit("?"); break; }
        emit(fmt("{:.1f} ", *now));
        if (std::fabs(*now - value) > 0.01f) ++reverted;
    }
    emitLine("");
    if (reverted == 0) {
        emitLine("    -> 值保持住了(8 次采样都没被改回)");
    } else if (reverted >= 6) {
        emitLine("    -> 值基本立刻被改回: 该字段被游戏每帧重算, 直写压不住");
    } else {
        emitFmt("    -> 值被改回 {}/8 次: 覆盖是间歇性的(可能由某个事件触发)",
                 reverted);
    }
}

// ---------------------------------------------------------------------------
//  dumpStructPointerSlots — 属性链读空时的取证
//
//  把 UStruct 里每个可能存放"属性链起点"的指针槽摊出来, 并标注它指向的位置
//  能否解出合法 FField 名字。判据的演进史见 docs/reverse/reflection-limits.md。
// ---------------------------------------------------------------------------
void CommandServer::dumpStructPointerSlots(uint64_t structObj) {
    if (!structObj) return;

    namespace sc = epsilon::game::offsets::scan;

    emitLine("    该类的指针槽扫描(找属性链起点):");
    emitLine("      slot      value              resolves-to");
    emitLine("      " + std::string(58, '-'));
    emitLine("");

    const auto& layout = eng_.reflection().layout();
    int plausible = 0;

    for (uint32_t off = sc::structPointerSlotFirst; off <= sc::structPointerSlotLast;
         off += sc::structPointerSlotStep) {
        auto v = epsilon::game::readPtr(structObj, off);
        if (!v) continue;
        // 只看像用户态指针的值, 过滤掉整数/标志位字段。
        if (*v < 0x10000 || *v > 0x7FFFFFFFFFFF) continue;

        const std::string resolved = eng_.reflection().fieldNameAt(*v);
        const bool good = !resolved.empty();
        if (good) ++plausible;

        emitFmt("      +{:#04x}     {:<18} {}{}",
                 off, hex(*v, 16), good ? "" : "(not a name) ",
                 sanitize(resolved, 40));
    }

    if (plausible == 0) {
        emitLine("      -> 没有任何槽像属性链起点。");
        emitLine("         可能该类确实没有可反射属性, 或 FField::NamePrivate 偏移也变了。");
    } else {
        emitLine("      -> 有槽能解出名字候选(见上)。用 mem va=<该槽指向的地址> 进一步核对。");
    }

    // 只对第一个类自动做 FField 名字偏移探测。可靠的判据是**链的自洽性**:
    // 沿 Next 连续多跳都能在同一个偏移上解出名字, 垃圾指针凑不出这种性质。
    if (plausible > 0 && fieldProbeBudget_ > 0) {
        --fieldProbeBudget_;

        auto chainScore = [&](uint64_t first, uint32_t nameOff,
                              std::vector<std::string>* names) {
            uint64_t f = first;
            int ok = 0;
            std::vector<uint64_t> visited;
            for (int hop = 0; hop < 6 && f; ++hop) {
                bool loop = false;
                for (uint64_t v : visited) if (v == f) { loop = true; break; }
                if (loop) break;
                visited.push_back(f);

                auto idx = epsilon::game::readI32(f, nameOff);
                if (!idx || *idx <= 0) break;
                const std::string nm = eng_.names().resolve(*idx);
                if (nm.empty()) break;
                // 含 '/' 的是包名/路径, 不是字段名。
                if (nm.find('/') != std::string::npos) break;
                if (names) names->push_back(nm);
                ++ok;

                f = eng_.reflection().fieldNextOf(f);
            }
            return ok;
        };

        int bestScore = 0;
        uint32_t bestSlot = 0, bestOff = 0;
        uint64_t bestAddr = 0;
        std::vector<std::string> bestNames;

        for (uint32_t slot = sc::structPointerSlotFirst; slot <= sc::structPointerSlotLast;
             slot += sc::structPointerSlotStep) {
            auto cand = epsilon::game::readPtr(structObj, slot);
            if (!cand) continue;
            if (*cand < 0x10000 || *cand > 0x7FFFFFFFFFFF) continue;
            if (*cand >= eng_.moduleBase() && *cand < eng_.moduleBase() + eng_.moduleSize()) continue;

            for (uint32_t off = sc::fieldNameProbeFirst; off <= sc::fieldNameProbeLast;
                 off += sc::fieldNameProbeStep) {
                std::vector<std::string> names;
                const int score = chainScore(*cand, off, &names);
                if (score > bestScore) {
                    bestScore = score;
                    bestSlot = slot;
                    bestOff = off;
                    bestAddr = *cand;
                    bestNames = std::move(names);
                }
            }
        }

        if (bestScore > 1) {
            emitLine("");
            emitFmt("      [自动探测] 链自洽最佳: 槽 +{:#x} -> {} , 名字偏移 +{:#x} , 连续 {} 跳",
                     bestSlot, hex(bestAddr, 16), bestOff, bestScore);
            std::string chain;
            for (size_t i = 0; i < bestNames.size() && i < 6; ++i) {
                if (i) chain += " -> ";
                chain += bestNames[i];
            }
            emitFmt("                 链上名字: {}", sanitize(chain, 90));
            if (bestOff != layout.fieldName) {
                emitFmt("      -> FField::NamePrivate 实际为 +{:#x} (当前常量 +{:#x}) 建议固化",
                         bestOff, layout.fieldName);
            } else {
                emitLine("      -> 名字偏移与常量一致, 那么问题在链起点偏移上。");
            }
        } else {
            emitLine("");
            emitFmt("      [自动探测] 没有候选能连续自洽(最好只有 {} 跳) —— 换个大类再试。",
                     bestScore);
        }
    }
}

// ---------------------------------------------------------------------------
void CommandServer::cmdProps(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }
    if (args.empty())   { emitLine("用法: props <类名> [inherited=1]"); return; }

    const uint64_t cls = eng_.objects().findClass(args[0]);
    if (!cls) { emitFmt("找不到类 {}", args[0]); return; }

    const bool inherited = optInt(args, "inherited").value_or(1) != 0;
    const auto& layout = eng_.reflection().layout();

    emitFmt("=== {} 的属性链 ===", eng_.objects().nameOf(cls));
    emitFmt("布局: Next=+{:#x} Name=+{:#x} Offset=+{:#x}",
             layout.fieldNext, layout.fieldName, layout.propOffset);
    // 自愈探到的偏移要报出来 —— 一旦确认, 就该把它固化回 Reflection.h 的常量,
    // 不必每次都靠扫描。
    if (const uint32_t found = eng_.reflection().discoveredChildPropsOffset(); found != 0) {
        emitFmt("属性链起点: 自愈探到 +{:#x} (静态常量是 +{:#x}) -> 建议固化",
                 found, epsilon::game::offsets::uStruct::childProps);
    }
    emitLine("");

    if (!inherited) {
        emitFmt("{:<36} {:<22} {:>8} {:>7} {:>5}  {}", "名字", "类型", "偏移", "大小", "维数", "内层类型");
        emitLine(std::string(100, '-'));
        for (auto const& pf : eng_.reflection().propertiesOf(cls)) {
            emitFmt("{:<36} {:<22} {:#8x} {:>7} {:>5}  {}",
                     sanitize(pf.name, 36), sanitize(pf.type, 22), pf.offset,
                     pf.size, pf.arrayDim,
                     pf.structType.empty() ? pf.objectType : pf.structType);
        }
        return;
    }

    // 含继承: 按继承链分层显示
    uint64_t cur = cls;
    int depth = 0;
    while (cur && depth++ < 64) {
        auto props = eng_.reflection().propertiesOf(cur);
        emitFmt("--- [{}] {}  ({} 个属性, {} 字节) ---",
                 depth - 1, eng_.objects().nameOf(cur), props.size(),
                 eng_.reflection().structSize(cur));
        // 属性数为 0 时把候选指针槽全部 dump 出来。
        // 属性数恒为 0 意味着 UStruct 里的"属性链起点"偏移对这个构建是错的;
        // 光看空列表没法定位, 必须看到原始槽位才能确定真实偏移。
        if (props.empty()) {
            dumpStructPointerSlots(cur);
        }
        for (auto const& pf : props) {
            emitFmt("  {:<34} {:<22} {:#8x} {:>7} {:>5}  {}",
                     sanitize(pf.name, 34), sanitize(pf.type, 22), pf.offset,
                     pf.size, pf.arrayDim,
                     pf.structType.empty() ? pf.objectType : pf.structType);
        }
        cur = eng_.reflection().superStruct(cur);
        if (cur) emitLine("");
    }
}

void CommandServer::cmdGet(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }
    if (args.size() < 2) { emitLine("用法: get <对象名> <属性名>   例: get PlayerController MyPawn"); return; }

    const uint64_t obj = eng_.objects().findObjectByName(args[0]);
    if (!obj) { emitFmt("找不到对象 {}", args[0]); return; }

    const uint64_t cls = eng_.objects().classOf(obj);
    auto pf = eng_.reflection().findProperty(cls, args[1]);
    if (!pf) {
        // 沿继承链找
        for (auto const& p : eng_.reflection().allPropertiesInherited(cls)) {
            if (iequals(p.name, args[1])) { pf = p; break; }
        }
    }
    if (!pf) { emitFmt("{} 上没有属性 {}", args[0], args[1]); return; }

    emitFmt("=== {} ({}) . {} ===",
             eng_.objects().nameOf(obj), eng_.objects().classNameOf(obj), pf->name);
    emitFmt("对象地址 : {}", hex(obj, 16));
    emitFmt("属性偏移 : +{:#x}   大小 {}   类型 {}",
             pf->offset, pf->size, pf->type.empty() ? "?" : pf->type);
    emitFmt("值       : {}", renderValue(obj, *pf));

    // 顺带给一小段十六进制, 便于人工核对布局
    emit(hexdump(reinterpret_cast<const uint8_t*>(obj + static_cast<uint32_t>(pf->offset)),
                 32, obj + static_cast<uint32_t>(pf->offset), 32));
}

// ---------------------------------------------------------------------------
void CommandServer::cmdMem(std::vector<std::string> const& args) {
    // 用法:
    //   mem <rva十六进制> [len=N]     —— 相对模块基址(最常用)
    //   mem va=<绝对地址> [len=N]
    // 校准基线偏移时必须要有这个: "读数不对" 和 "地址不对" 光看 UObject
    // 是分不清的, 得直接把原始字节打出来看。
    if (args.empty()) {
        emitLine("用法: mem <rva> [len=N]   或   mem va=<绝对地址> [len=N]");
        return;
    }

    uint64_t addr = 0;
    if (auto va = optValue(args, "va")) {
        try { addr = std::stoull(*va, nullptr, 0); } catch (...) {
            emitFmt("va 解析失败: {}", *va); return;
        }
    } else {
        try { addr = eng_.moduleBase() + std::stoull(args[0], nullptr, 0); } catch (...) {
            emitFmt("rva 解析失败: {}", args[0]); return;
        }
    }

    size_t len = 0x80;
    if (auto l = optValue(args, "len")) {
        try { len = static_cast<size_t>(std::stoull(*l, nullptr, 0)); } catch (...) {}
    }
    if (len > 0x1000) len = 0x1000;

    emitFmt("=== 内存 {} ({} 字节) ===", hex(addr, 16), len);
    std::vector<uint8_t> buf(len, 0);
    if (!safeRead(buf.data(), reinterpret_cast<const void*>(addr), len)) {
        emitLine("读取失败 —— 地址未映射或不可读");
        return;
    }
    emit(hexdump(buf.data(), buf.size(), addr, buf.size()));

    // 顺手把里面所有"像指针"的值列出来 —— 校准块表/虚表偏移时最有用。
    emitLine("--- 疑似指针(8 字节对齐, 落在 0x10000..0x7FFFFFFFFFFF) ---");
    int shown = 0;
    for (size_t i = 0; i + 8 <= buf.size(); i += 8) {
        uint64_t v = 0;
        std::memcpy(&v, buf.data() + i, 8);
        if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) continue;
        emitFmt("  +{:#04x}  {}", i, hex(v, 16));
        if (++shown >= 24) { emitLine("  ..."); break; }
    }
    if (shown == 0) emitLine("  (无)");
}

void CommandServer::cmdSet(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }
    if (args.size() < 3) {
        emitLine("用法: set <对象名> <属性名> <值>");
        emitLine("  例: set PlayerCharacter MaxWalkSpeed 3000");
        return;
    }

    const uint64_t obj = eng_.objects().findObjectByName(args[0]);
    if (!obj) { emitFmt("找不到对象 {}", args[0]); return; }

    const uint64_t cls = eng_.objects().classOf(obj);
    auto pf = eng_.reflection().findProperty(cls, args[1]);
    if (!pf) {
        for (auto const& p : eng_.reflection().allPropertiesInherited(cls)) {
            if (iequals(p.name, args[1])) { pf = p; break; }
        }
    }
    if (!pf) {
        emitFmt("{} 上找不到属性 {} (用 props <类名> 看可用属性)", args[0], args[1]);
        return;
    }

    const uint64_t at = obj + static_cast<uint32_t>(pf->offset);
    const std::string& t = pf->type;
    const std::string before = renderValue(obj, *pf);

    // 按类型写入。只支持数值/布尔这类"裸值"属性 ——
    // 字符串/数组/结构体走裸写会破坏容器内部指针, 那是制造崩溃而不是改数据。
    auto writePod = [&](auto tag) -> bool {
        using T = decltype(tag);
        try {
            const double d = std::stod(args[2]);
            const T v = static_cast<T>(d);
            if (!remotePoke(at, &v, sizeof(T))) return false;
            return true;
        } catch (...) { return false; }
    };

    bool ok = false;
    if      (icontains(t, "FloatProperty"))  ok = writePod(float{});
    else if (icontains(t, "DoubleProperty")) ok = writePod(double{});
    else if (icontains(t, "Int64Property"))  ok = writePod(int64_t{});
    else if (icontains(t, "Int32Property") || icontains(t, "IntProperty")) ok = writePod(int32_t{});
    else if (icontains(t, "Int16Property"))  ok = writePod(int16_t{});
    else if (icontains(t, "Int8Property"))   ok = writePod(int8_t{});
    else if (icontains(t, "ByteProperty"))   ok = writePod(uint8_t{});
    else if (icontains(t, "BoolProperty"))   ok = writePod(uint8_t{});
    else {
        emitFmt("属性类型 {} 不支持裸写 ——", t.empty() ? "(未知)" : t);
        emitLine("  字符串/数组/结构体含内部指针, 裸写会破坏它们导致崩溃。");
        emitLine("  这类要改得走游戏自己的函数(ProcessEvent), 不是写内存。");
        return;
    }

    if (!ok) { emitLine("写入失败(值解析错误, 或目标内存不可写)"); return; }

    emitFmt("=== set {} . {} ===", args[0], pf->name);
    emitFmt("对象地址 : {}", hex(obj, 16));
    emitFmt("属性偏移 : +{:#x}   类型 {}", pf->offset, t.empty() ? "?" : t);
    emitFmt("写入前   : {}", before);
    emitFmt("写入后   : {}", renderValue(obj, *pf));
}


void CommandServer::cmdWorld() {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }

    game::ue::WorldView view(eng_);
    auto world = view.currentWorld();
    if (!world) { emitLine("拿不到 UWorld(可能还没进关卡)"); return; }

    emitLine("=== UWorld ===");
    emitFmt("地址            : {}", hex(world->address, 16));
    emitFmt("对象名          : {} [{}]", world->name, world->className);
    emitFmt("PersistentLevel : {} ({})",
             world->persistentLevel ? hex(world->persistentLevel, 16) : "空",
             world->levelName.empty() ? "?" : world->levelName);
    emitFmt("  偏移来源      : {}", world->offsetSource);

    auto level = view.persistentLevel();
    if (level) {
        emitLine("");
        emitLine("=== ULevel ===");
        emitFmt("地址            : {}", hex(level->address, 16));
        emitFmt("名字            : {}", level->name);
        emitFmt("Actors 偏移     : +{:#x}  ({})", level->offsetActors, level->offsetSource);
        emitFmt("Actor 数量      : {}", thousands(static_cast<uint64_t>(level->actorCount)));
    }
}

void CommandServer::cmdActors(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emitLine("引擎未定位, 先执行 rescan"); return; }

    const auto limit = static_cast<size_t>(optInt(args, "limit").value_or(40));
    const auto cls   = optValue(args, "class");

    game::ue::WorldView view(eng_);
    const auto list = view.actors(0);

    if (list.empty()) { emitLine("关卡里没读到 Actor(可能还在加载)"); return; }

    emitFmt("=== 关卡 Actor (共 {}, 显示上限 {}) ===",
             thousands(static_cast<uint64_t>(list.size())), limit);

    size_t shown = 0;
    for (auto const& a : list) {
        if (cls && !icontains(a.className, *cls)) continue;
        emitFmt("  [{:>5}] {:>16}  {:<34} {}",
                 a.index, hex(a.address, 16), sanitize(a.className, 34),
                 sanitize(a.name, 48));
        if (++shown >= limit) break;
    }
    emitFmt("--- 显示 {} 条 ---", shown);
}

// ---------------------------------------------------------------------------
//  player — 玩家候选诊断
//
//  把"长得像玩家"的 Actor 全列出来, 并明确标出模块当前会挑中哪一个。
//  判定规则只有一份(见 d2::playerClassRank), 这条命令直接调用它。
//  输出保持 ASCII: 覆盖层字体只有 ASCII 位形, 而结果很可能要对面板看。
// ---------------------------------------------------------------------------
void CommandServer::cmdPlayer() {
    // WorldView 与功能模块用的是同一条路径: 模块解析玩家也走这里。
    WorldView view(eng_);

    auto world = view.currentWorld();
    if (!world) {
        emitLine("no UWorld (not in a level yet?)");
        return;
    }

    emitLine("=== player candidates ===");
    emitFmt("world          : {} {}", hex(world->address, 16),
            sanitize(world->className, 40));

    auto level = view.persistentLevel();
    if (!level) {
        emitLine("PersistentLevel unavailable -- cannot enumerate actors");
        return;
    }
    emitFmt("PersistentLevel: {} ({} actors)", hex(level->address, 16),
            thousands(static_cast<uint64_t>(level->actorCount)));

    const auto list = view.actors(*world);
    if (list.empty()) {
        emitLine("actor list is EMPTY -- the level's Actors array read failed");
        // 同一趟里把 ULevel 对象扫一遍, 免得还要人工搬地址再跑一次 scanlevel。
        scanForActorArray(level->address);
        return;
    }

    int pawnish = 0;
    int matched = 0;
    int bestRank = 9999;
    size_t bestIndex = SIZE_MAX;

    for (size_t i = 0; i < list.size(); ++i) {
        const auto& a = list[i];
        const bool looksLikePawn =
            icontains(a.className, "Character") || icontains(a.className, "Pawn") ||
            icontains(a.className, "Player");
        if (!looksLikePawn) continue;

        ++pawnish;
        const int rank = d2::playerClassRank(a.className);
        const bool accepted = rank >= 0;
        if (accepted) {
            ++matched;
            if (rank < bestRank) { bestRank = rank; bestIndex = i; }
        }

        emitFmt("  [{:>5}] {:>16}  {:<36} {}  {}",
                 a.index, hex(a.address, 16), sanitize(a.className, 36),
                 sanitize(a.name, 40), accepted ? "<== MODULE WILL USE THIS" : "");
    }

    emitLine("");
    emitFmt("pawn-like actors : {}", pawnish);
    emitFmt("module matches   : {}", matched);
    if (matched == 0) {
        emitFmt("=> NO MATCH. The modules look for {}, and exclude", d2::playerClassRulesText());
        emitLine("   Controller / PlayerState / HUD / Mock / Mob / Wolf.");
        emitLine("   Tell me which class name above is the actual player.");
    } else if (bestIndex != SIZE_MAX) {
        emitFmt("=> module would pick [{}] {} ({})",
                list[bestIndex].index, sanitize(list[bestIndex].className, 36),
                hex(list[bestIndex].address, 16));
    }
}

} // namespace epsilon::payload