// ============================================================================
//  commandServer.cpp
// ============================================================================
#include "payload/CommandServer.h"

#include "common/PeImage.h"
#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/Hooks.h"
#include "payload/Runtime.h"      // installFrameHook()
#include "payload/ue/Engine.h"
#include "payload/ue/World.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>     // WideCharToMultiByte / CP_UTF8

#include <algorithm>
#include <cstring>
#include <format>

namespace epsilon::payload {
namespace {

using namespace epsilon::ue;

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
        // FString / FName 都是 16 字节: { TCHAR* Data; int32 Num; int32 Max }
        uint64_t data = 0;
        int32_t  num = 0;
        if (!safeRead(&data, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        safeRead(&num, reinterpret_cast<const void*>(at + 8), 4);
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
        // TArray<T>: Data / Num / Max
        uint64_t data = 0; int32_t num = 0, cap = 0;
        if (!safeRead(&data, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        safeRead(&num, reinterpret_cast<const void*>(at + 8), 4);
        safeRead(&cap, reinterpret_cast<const void*>(at + 12), 4);
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
    auto line = [&](ue::GlobalSlot const& g) {
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
//  dumpStructPointerSlots — 属性链读空时的取证
//
//  UStruct 里"属性链起点"是个 8 字节指针。静态偏移一旦对这个构建失效,
//  propertiesOf 就会返回空, 而空列表本身不含任何诊断信息。
//
//  这里把 0x20..0x98 每个 8 字节槽都读出来, 并对每个**像指针**的值试着按
//  FField 解析一次名字(NamePrivate 在 FField+0x28, 是个 FName 索引)。
//  哪个槽能解出合法名字, 哪个槽就极可能就是真正的属性链起点 —— 而且名字本身
//  通常就直接告诉你它是什么(比如 "MaxWalkSpeed" 就是第一个属性)。
// ---------------------------------------------------------------------------
void CommandServer::dumpStructPointerSlots(uint64_t structObj) {
    if (!structObj) return;

    emitLine("    该类的指针槽扫描(找属性链起点):");
    emitLine("      slot      value              resolves-to");
    emitLine("      " + std::string(58, '-'));

    const auto& layout = eng_.reflection().layout();
    int plausible = 0;

    for (uint32_t off = 0x20; off <= 0x98; off += 8) {
        uint64_t v = 0;
        if (!safeRead(&v, reinterpret_cast<const void*>(structObj + off), 8)) continue;
        if (!v) continue;
        // 只看像用户态指针的值, 过滤掉整数/标志位字段。
        if (v < 0x10000 || v > 0x7FFFFFFFFFFF) continue;

        std::string resolved;
        // 试着把它当成 FField*, 读 NamePrivate。
        int32_t nameIdx = 0;
        if (safeRead(&nameIdx, reinterpret_cast<const void*>(v + layout.fieldName), 4)) {
            resolved = eng_.names().resolve(nameIdx);
        }
        const bool good = !resolved.empty();
        if (good) ++plausible;

        emitFmt("      +{:#04x}     {:<18} {}{}",
                 off, hex(v, 16), good ? "" : "(not a name) ",
                 sanitize(resolved, 40));
    }

    if (plausible == 0) {
        emitLine("      -> 没有任何槽像属性链起点。");
        emitLine("         可能该类确实没有可反射属性, 或 FField::NamePrivate 偏移也变了。");
    } else {
        emitLine("      -> 有槽能解出名字候选(见上)。用 mem va=<该槽指向的地址> 进一步核对。");
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
                 found, offStructChildProps);
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

    ue::WorldView view(eng_);
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

    ue::WorldView view(eng_);
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
//  功能模块(大跳/加速)靠类名匹配本地玩家, 匹配规则是否与**这个构建的实际
//  类名**相符, 光看代码是看不出来的。这条命令把关卡里所有"长得像玩家"的
//  Actor 全列出来, 并明确标出模块当前会挑中哪一个。
//
//  全部输出保持 ASCII: 覆盖层字体只有 ASCII 位图字形, 中文会渲染成 '?',
//  而这条命令的结果很可能需要对着覆盖层看。
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
        emitLine("  (offset for ULevel::Actors may be wrong for this build)");
        return;
    }

    // 统计出现的类名, 便于一眼看出关卡里到底有哪些角色类型。
    int pawnish = 0;
    int matched = 0;
    size_t firstMatch = SIZE_MAX;

    for (size_t i = 0; i < list.size(); ++i) {
        const auto& a = list[i];
        const bool looksLikePawn =
            icontains(a.className, "Character") || icontains(a.className, "Pawn") ||
            icontains(a.className, "Player");
        if (!looksLikePawn) continue;

        ++pawnish;
        // 这里的判定必须与 feature/MovementAccess.cpp 的 looksLikeLocalPlayer 一致,
        // 否则这条命令会骗人。
        const bool accepted =
            !icontains(a.className, "Mock") && !icontains(a.className, "Mob") &&
            !icontains(a.className, "Projectile") &&
            (icontains(a.className, "PlayerCharacter") || icontains(a.className, "PlayerPawn"));

        if (accepted && firstMatch == SIZE_MAX) firstMatch = i;
        if (accepted) ++matched;

        emitFmt("  [{:>5}] {:>16}  {:<36} {}  {}",
                 a.index, hex(a.address, 16), sanitize(a.className, 36),
                 sanitize(a.name, 40), accepted ? "<== MODULE WILL USE THIS" : "");
    }

    emitLine("");
    emitFmt("pawn-like actors : {}", pawnish);
    emitFmt("module matches   : {}", matched);
    if (matched == 0) {
        emitLine("=> NO MATCH. The modules filter on the class name containing");
        emitLine("   'PlayerCharacter' or 'PlayerPawn' while excluding Mock/Mob.");
        emitLine("   Copy one of the class names above and tell me which one is you.");
    } else if (firstMatch != SIZE_MAX) {
        emitFmt("=> module would pick [{}] {} ({})",
                list[firstMatch].index, sanitize(list[firstMatch].className, 36),
                hex(list[firstMatch].address, 16));
    }
}

} // namespace epsilon::payload