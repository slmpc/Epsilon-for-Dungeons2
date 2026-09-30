// ============================================================================
//  command_server.cpp
// ============================================================================
#include "payload/command_server.h"

#include "common/pe_image.h"
#include "common/text.h"
#include "payload/payload.h"
#include "payload/hooks.h"
#include "payload/runtime.h"      // install_frame_hook()
#include "payload/ue/engine.h"
#include "payload/ue/world.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>     // WideCharToMultiByte / CP_UTF8

#include <algorithm>
#include <cstring>
#include <format>

namespace mcd2::payload {
namespace {

using namespace mcd2::ue;

// 解析 "key=value" 形式的选项; 返回 value, 不存在则返回 nullopt。
std::optional<std::string> opt_value(std::vector<std::string> const& args,
                                     std::string_view key) {
    const std::string prefix = std::string(key) + "=";
    for (auto const& a : args) {
        if (a.size() > prefix.size() && istarts_with(a, prefix)) {
            return a.substr(prefix.size());
        }
    }
    return std::nullopt;
}

std::optional<int64_t> opt_int(std::vector<std::string> const& args, std::string_view key) {
    if (auto v = opt_value(args, key)) {
        try { return std::stoll(*v, nullptr, 0); } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

// 按属性类型渲染一个内存位置上的值。
std::string render_value(uint64_t owner, PropertyField const& pf) {
    if (!owner) return "(空对象)";
    const uint64_t at = owner + static_cast<uint32_t>(pf.offset);
    const std::string& t = pf.type;

    auto pod = [&](auto tag) -> std::string {
        using T = decltype(tag);
        T v{};
        if (!safe_read(&v, reinterpret_cast<const void*>(at), sizeof(T))) return "(读取失败)";
        return fmt("{}", v);
    };

    if (icontains(t, "BoolProperty")) {
        // UE 的 bool 按位存: 字节值 & (1 << (offset % 8)), 位偏移存在 FProperty 里。
        // 这里只能给出字节层面的近似 —— 标注清楚, 避免误导。
        uint8_t b = 0;
        if (!safe_read(&b, reinterpret_cast<const void*>(at), 1)) return "(读取失败)";
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
        if (!safe_read(&data, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        safe_read(&num, reinterpret_cast<const void*>(at + 8), 4);
        if (!data || num <= 0 || num > 4096) return fmt("ptr={} (空/不合法)", hex(data, 16));
        std::wstring w(static_cast<size_t>(num), L'\0');
        if (!safe_read(w.data(), reinterpret_cast<const void*>(data), static_cast<size_t>(num) * 2))
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
        if (!safe_read(&ptr, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        if (!ptr) return "null";
        return fmt("对象指针 {}", hex(ptr, 16));
    }

    if (icontains(t, "StructProperty")) {
        return fmt("struct {} @ +{:#x} (内联 {:#x} 字节)", 
                   pf.struct_type.empty() ? "?" : pf.struct_type, pf.offset, pf.size);
    }

    if (icontains(t, "ArrayProperty")) {
        // TArray<T>: Data / Num / Max
        uint64_t data = 0; int32_t num = 0, cap = 0;
        if (!safe_read(&data, reinterpret_cast<const void*>(at), 8)) return "(读取失败)";
        safe_read(&num, reinterpret_cast<const void*>(at + 8), 4);
        safe_read(&cap, reinterpret_cast<const void*>(at + 12), 4);
        return fmt("TArray<{}> data={} num={} max={}",
                   pf.object_type.empty() ? pf.struct_type : pf.object_type,
                   hex(data, 16), num, cap);
    }

    return fmt("{} @ +{:#x} ({} 字节, 未做类型解读)", t.empty() ? "未知类型" : t,
               pf.offset, pf.size);
}

} // namespace

// ---------------------------------------------------------------------------
bool CommandServer::execute(std::string_view line) {
    auto args = split_ws(trim(line));
    if (args.empty()) return true;

    const std::string cmd = to_lower(args[0]);
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "help" || cmd == "?")        { print_help(); }
    else if (cmd == "status")               { cmd_status(); }
    else if (cmd == "rescan" || cmd == "init") { cmd_rescan(); }
    else if (cmd == "globals")              { cmd_globals(); }
    else if (cmd == "hooks")                { cmd_hooks(); }
    else if (cmd == "hook")                 { cmd_hook_install(); }
    else if (cmd == "objects" || cmd == "obj") { cmd_objects(rest); }
    else if (cmd == "find")                 { cmd_find(rest); }
    else if (cmd == "class")                { cmd_class(rest); }
    else if (cmd == "props")                { cmd_props(rest); }
    else if (cmd == "get")                  { cmd_get(rest); }
    else if (cmd == "world" || cmd == "level") { cmd_world(); }
    else if (cmd == "actors" || cmd == "list") { cmd_actors(rest); }
    else if (cmd == "mem" || cmd == "dump")    { cmd_mem(rest); }
    else if (cmd == "quit" || cmd == "exit" || cmd == "detach") {
        emit_line("bye");
        return false;
    }
    else {
        emit_fmt("未知命令: {}  (help 看用法)", args[0]);
    }
    return true;
}

void CommandServer::print_help() const {
    emit_line("命令一览:");
    emit_line("  status                     引擎定位总览");
    emit_line("  rescan                     重新定位 GObjects/GNames/属性链");
    emit_line("  globals                    GEngine / GWorld 详情");
    emit_line("  hooks                      帧钩子状态");
    emit_line("  hook                       安装 Present 帧钩子(可选, 采集不需要)");
    emit_line("");
    emit_line("  objects [filter=子串] [class=类名] [limit=N] [skip=N]");
    emit_line("                             列出 UObject");
    emit_line("  find <名字>                按名字查对象(支持子串, 全表扫描)");
    emit_line("  class <类名>               看某个类: 大小 / 父类 / 实例数");
    emit_line("");
    emit_line("  props <类名>               列出该类的属性链与偏移(含继承)");
    emit_line("  get <对象名> <属性名>      读某个对象的属性值");
    emit_line("");
    emit_line("  world                      当前 UWorld / PersistentLevel");
    emit_line("  actors [limit=N] [class=类名]");
    emit_line("                             枚举关卡里的 Actor");
    emit_line("");
    emit_line("  quit                       结束命令循环");
}

// ---------------------------------------------------------------------------
void CommandServer::cmd_status() {
    emit_line("=== 引擎定位状态 ===");
    emit(eng_.report());
}

void CommandServer::cmd_rescan() {
    emit_line("重新定位中...");
    if (eng_.rescan()) emit_line("定位成功。");
    else              emit_line("定位失败 —— 游戏可能还没进到关卡, 稍后再试。");
    emit(eng_.report());
}

void CommandServer::cmd_globals() {
    auto line = [&](ue::GlobalSlot const& g) {
        if (!g.value) { emit_fmt("{:<10} 未定位", g.label); return; }
        emit_fmt("{:<10} 槽位 {} → {} {}   校验={}   来源={}",
                 g.label, hex(g.address, 16), g.object_class, g.object_name,
                 g.verified ? "通过" : "未通过", g.how);
    };
    emit_line("=== 引擎全局 ===");
    line(eng_.gengine());
    line(eng_.gworld());
    emit_line("");
    emit_fmt("模块基址 {}  大小 {}", hex(eng_.module_base(), 16), human_bytes(eng_.module_size()));
}

void CommandServer::cmd_hooks() {
    auto const& st = hooks().status();
    emit_line("=== 帧钩子 (MinHook) ===");
    emit_fmt("状态        : {}", st.installed ? "已安装" : (st.attempted ? "安装失败" : "未安装"));
    if (st.installed) {
        emit_fmt("目标        : {}", st.target);
        emit_fmt("定位方式    : {}", st.how);
        emit_fmt("累计帧数    : {}", thousands(frame_count()));
        emit_fmt("估算 FPS    : {:.1f}", st.fps);
    } else if (!st.error.empty()) {
        emit_fmt("失败原因    : {}", st.error);
    } else {
        emit_line("说明        : 帧钩子不在启动路径上, 用 `hook` 命令显式安装。");
        emit_line("              只读采集(objects/props/actors 等)不需要它。");
    }
}

void CommandServer::cmd_hook_install() {
    if (hooks().installed()) {
        emit_line("帧钩子已经装过了。用 `hooks` 看状态。");
        return;
    }
    emit_line("正在安装 Present 帧钩子...");
    if (install_frame_hook()) {
        auto const& st = hooks().status();
        emit_fmt("已挂上: {}", st.target);
        emit_fmt("定位方式: {}", st.how);
    } else {
        emit_fmt("安装失败: {}", hooks().status().error);
        emit_line("（不影响采集功能 —— 命令驱动的采样不依赖帧回调）");
    }
}

// ---------------------------------------------------------------------------
void CommandServer::cmd_objects(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emit_line("引擎未定位, 先执行 rescan"); return; }

    const auto filter = opt_value(args, "filter");
    const auto cls    = opt_value(args, "class");
    const auto limit  = opt_int(args, "limit").value_or(64);
    const auto skip   = opt_int(args, "skip").value_or(0);

    emit_fmt("=== UObject 列表 (filter={} class={} limit={} skip={}) ===",
             filter ? *filter : "*", cls ? *cls : "*", limit, skip);

    int64_t shown = 0, scanned = 0;
    eng_.objects().for_each([&](ObjectStat const& st) {
        ++scanned;
        if (scanned <= skip) return true;

        if (filter && !icontains(st.full_name, *filter) && !icontains(st.name, *filter)) return true;
        if (cls && !iequals(st.class_name, *cls) && !icontains(st.class_name, *cls)) return true;

        emit_fmt("  [{:>7}] {:>16}  {:<28} {}",
                 st.index, hex(st.address, 16), sanitize(st.class_name, 28),
                 sanitize(st.name, 64));
        ++shown;
        return shown < limit;
    });

    emit_fmt("--- 扫描 {} 个槽位, 显示 {} 条 (全表 {} 个) ---",
             thousands(static_cast<uint64_t>(scanned)), shown,
             thousands(static_cast<uint64_t>(eng_.objects().num_elements())));
}

void CommandServer::cmd_find(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emit_line("引擎未定位, 先执行 rescan"); return; }
    if (args.empty())   { emit_line("用法: find <名字子串>"); return; }

    const std::string needle = args[0];
    const int64_t  limit = opt_int(args, "limit").value_or(32);

    emit_fmt("=== 查找 \"{}\" ===", needle);
    int64_t shown = 0;
    eng_.objects().for_each([&](ObjectStat const& st) {
        if (!icontains(st.name, needle) && !icontains(st.class_name, needle)) return true;
        emit_fmt("  [{:>7}] {:>16}  {:<32} {}",
                 st.index, hex(st.address, 16), sanitize(st.class_name, 32),
                 sanitize(st.name, 64));
        return ++shown < limit;
    });
    if (shown == 0) emit_line("  (无匹配)");
}

void CommandServer::cmd_class(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emit_line("引擎未定位, 先执行 rescan"); return; }
    if (args.empty())   { emit_line("用法: class <类名>  例: class Character"); return; }

    const uint64_t cls = eng_.objects().find_class(args[0]);
    if (!cls) { emit_fmt("找不到类 {}", args[0]); return; }

    emit_line("=== 类信息 ===");
    emit_fmt("类对象地址  : {}", hex(cls, 16));
    emit_fmt("类名        : {}", eng_.objects().name_of(cls));
    emit_fmt("类的大小    : {} 字节", eng_.reflection().struct_size(cls));

    const uint64_t super = eng_.reflection().super_struct(cls);
    emit_fmt("父类        : {}", super ? eng_.objects().name_of(super) : "(无)");

    const int32_t props = static_cast<int32_t>(eng_.reflection().properties_of(cls).size());
    emit_fmt("本类属性数  : {}", props);

    // 统计实例个数(全表扫一遍)
    int64_t instances = 0;
    const std::string my_name = eng_.objects().name_of(cls);
    eng_.objects().for_each([&](ObjectStat const& st) {
        if (iequals(st.class_name, my_name)) ++instances;
        return true;
    });
    emit_fmt("实例个数    : {}", thousands(static_cast<uint64_t>(instances)));
}

// ---------------------------------------------------------------------------
void CommandServer::cmd_props(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emit_line("引擎未定位, 先执行 rescan"); return; }
    if (args.empty())   { emit_line("用法: props <类名> [inherited=1]"); return; }

    const uint64_t cls = eng_.objects().find_class(args[0]);
    if (!cls) { emit_fmt("找不到类 {}", args[0]); return; }

    const bool inherited = opt_int(args, "inherited").value_or(1) != 0;
    const auto& layout = eng_.reflection().layout();

    emit_fmt("=== {} 的属性链 ===", eng_.objects().name_of(cls));
    emit_fmt("布局: Next=+{:#x} Name=+{:#x} Offset=+{:#x}",
             layout.field_next, layout.field_name, layout.prop_offset);
    emit_line("");

    if (!inherited) {
        emit_fmt("{:<36} {:<22} {:>8} {:>7} {:>5}  {}", "名字", "类型", "偏移", "大小", "维数", "内层类型");
        emit_line(std::string(100, '-'));
        for (auto const& pf : eng_.reflection().properties_of(cls)) {
            emit_fmt("{:<36} {:<22} {:#8x} {:>7} {:>5}  {}",
                     sanitize(pf.name, 36), sanitize(pf.type, 22), pf.offset,
                     pf.size, pf.array_dim,
                     pf.struct_type.empty() ? pf.object_type : pf.struct_type);
        }
        return;
    }

    // 含继承: 按继承链分层显示
    uint64_t cur = cls;
    int depth = 0;
    while (cur && depth++ < 64) {
        auto props = eng_.reflection().properties_of(cur);
        emit_fmt("--- [{}] {}  ({} 个属性, {} 字节) ---",
                 depth - 1, eng_.objects().name_of(cur), props.size(),
                 eng_.reflection().struct_size(cur));
        for (auto const& pf : props) {
            emit_fmt("  {:<34} {:<22} {:#8x} {:>7} {:>5}  {}",
                     sanitize(pf.name, 34), sanitize(pf.type, 22), pf.offset,
                     pf.size, pf.array_dim,
                     pf.struct_type.empty() ? pf.object_type : pf.struct_type);
        }
        cur = eng_.reflection().super_struct(cur);
        if (cur) emit_line("");
    }
}

void CommandServer::cmd_get(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emit_line("引擎未定位, 先执行 rescan"); return; }
    if (args.size() < 2) { emit_line("用法: get <对象名> <属性名>   例: get PlayerController MyPawn"); return; }

    const uint64_t obj = eng_.objects().find_object_by_name(args[0]);
    if (!obj) { emit_fmt("找不到对象 {}", args[0]); return; }

    const uint64_t cls = eng_.objects().class_of(obj);
    auto pf = eng_.reflection().find_property(cls, args[1]);
    if (!pf) {
        // 沿继承链找
        for (auto const& p : eng_.reflection().all_properties_inherited(cls)) {
            if (iequals(p.name, args[1])) { pf = p; break; }
        }
    }
    if (!pf) { emit_fmt("{} 上没有属性 {}", args[0], args[1]); return; }

    emit_fmt("=== {} ({}) . {} ===",
             eng_.objects().name_of(obj), eng_.objects().class_name_of(obj), pf->name);
    emit_fmt("对象地址 : {}", hex(obj, 16));
    emit_fmt("属性偏移 : +{:#x}   大小 {}   类型 {}",
             pf->offset, pf->size, pf->type.empty() ? "?" : pf->type);
    emit_fmt("值       : {}", render_value(obj, *pf));

    // 顺带给一小段十六进制, 便于人工核对布局
    emit(hexdump(reinterpret_cast<const uint8_t*>(obj + static_cast<uint32_t>(pf->offset)),
                 32, obj + static_cast<uint32_t>(pf->offset), 32));
}

// ---------------------------------------------------------------------------
void CommandServer::cmd_mem(std::vector<std::string> const& args) {
    // 用法:
    //   mem <rva十六进制> [len=N]     —— 相对模块基址(最常用)
    //   mem va=<绝对地址> [len=N]
    // 校准基线偏移时必须要有这个: "读数不对" 和 "地址不对" 光看 UObject
    // 是分不清的, 得直接把原始字节打出来看。
    if (args.empty()) {
        emit_line("用法: mem <rva> [len=N]   或   mem va=<绝对地址> [len=N]");
        return;
    }

    uint64_t addr = 0;
    if (auto va = opt_value(args, "va")) {
        try { addr = std::stoull(*va, nullptr, 0); } catch (...) {
            emit_fmt("va 解析失败: {}", *va); return;
        }
    } else {
        try { addr = eng_.module_base() + std::stoull(args[0], nullptr, 0); } catch (...) {
            emit_fmt("rva 解析失败: {}", args[0]); return;
        }
    }

    size_t len = 0x80;
    if (auto l = opt_value(args, "len")) {
        try { len = static_cast<size_t>(std::stoull(*l, nullptr, 0)); } catch (...) {}
    }
    if (len > 0x1000) len = 0x1000;

    emit_fmt("=== 内存 {} ({} 字节) ===", hex(addr, 16), len);
    std::vector<uint8_t> buf(len, 0);
    if (!safe_read(buf.data(), reinterpret_cast<const void*>(addr), len)) {
        emit_line("读取失败 —— 地址未映射或不可读");
        return;
    }
    emit(hexdump(buf.data(), buf.size(), addr, buf.size()));

    // 顺手把里面所有"像指针"的值列出来 —— 校准块表/虚表偏移时最有用。
    emit_line("--- 疑似指针(8 字节对齐, 落在 0x10000..0x7FFFFFFFFFFF) ---");
    int shown = 0;
    for (size_t i = 0; i + 8 <= buf.size(); i += 8) {
        uint64_t v = 0;
        std::memcpy(&v, buf.data() + i, 8);
        if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) continue;
        emit_fmt("  +{:#04x}  {}", i, hex(v, 16));
        if (++shown >= 24) { emit_line("  ..."); break; }
    }
    if (shown == 0) emit_line("  (无)");
}

void CommandServer::cmd_world() {
    if (!eng_.ready()) { emit_line("引擎未定位, 先执行 rescan"); return; }

    ue::WorldView view(eng_);
    auto world = view.current_world();
    if (!world) { emit_line("拿不到 UWorld(可能还没进关卡)"); return; }

    emit_line("=== UWorld ===");
    emit_fmt("地址            : {}", hex(world->address, 16));
    emit_fmt("对象名          : {} [{}]", world->name, world->class_name);
    emit_fmt("PersistentLevel : {} ({})",
             world->persistent_level ? hex(world->persistent_level, 16) : "空",
             world->level_name.empty() ? "?" : world->level_name);
    emit_fmt("  偏移来源      : {}", world->offset_source);

    auto level = view.persistent_level();
    if (level) {
        emit_line("");
        emit_line("=== ULevel ===");
        emit_fmt("地址            : {}", hex(level->address, 16));
        emit_fmt("名字            : {}", level->name);
        emit_fmt("Actors 偏移     : +{:#x}  ({})", level->offset_actors, level->offset_source);
        emit_fmt("Actor 数量      : {}", thousands(static_cast<uint64_t>(level->actor_count)));
    }
}

void CommandServer::cmd_actors(std::vector<std::string> const& args) {
    if (!eng_.ready()) { emit_line("引擎未定位, 先执行 rescan"); return; }

    const auto limit = static_cast<size_t>(opt_int(args, "limit").value_or(40));
    const auto cls   = opt_value(args, "class");

    ue::WorldView view(eng_);
    const auto list = view.actors(0);

    if (list.empty()) { emit_line("关卡里没读到 Actor(可能还在加载)"); return; }

    emit_fmt("=== 关卡 Actor (共 {}, 显示上限 {}) ===",
             thousands(static_cast<uint64_t>(list.size())), limit);

    size_t shown = 0;
    for (auto const& a : list) {
        if (cls && !icontains(a.class_name, *cls)) continue;
        emit_fmt("  [{:>5}] {:>16}  {:<34} {}",
                 a.index, hex(a.address, 16), sanitize(a.class_name, 34),
                 sanitize(a.name, 48));
        if (++shown >= limit) break;
    }
    emit_fmt("--- 显示 {} 条 ---", shown);
}

} // namespace mcd2::payload
