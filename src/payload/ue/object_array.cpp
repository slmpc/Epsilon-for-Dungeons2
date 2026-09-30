// ============================================================================
//  object_array.cpp
// ============================================================================
#include "payload/ue/object_array.h"

#include "common/pe_image.h"
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace mcd2::ue {
namespace {

std::string read_cstr_at(uint64_t va, size_t max_len) {
    if (!va) return {};
    std::string s(max_len, '\0');
    for (size_t i = 0; i < max_len; ++i) {
        char c = 0;
        if (!safe_read(&c, reinterpret_cast<const void*>(va + i), 1)) break;
        if (c == '\0') { s.resize(i); return s; }
        s[i] = c;
    }
    s.resize(max_len);
    return s;
}

} // namespace

bool ObjectArray::refresh() {
    objects_ = 0;
    chunk_table_ = 0;
    num_elements_ = max_elements_ = num_chunks_ = max_chunks_ = 0;

    if (!gobjects_) return false;
    if (!safe_read(&objects_, reinterpret_cast<const void*>(gobjects_ + kOffObjectsPtr), 8))
        return false;
    if (!objects_) return false;

    // TUObjectArray 自身也在目标堆上, 读它必须逐字段 safe_read。
    if (!safe_read(&chunk_table_, reinterpret_cast<const void*>(objects_ + kOffChunkTable), 8))
        return false;
    safe_read(&max_elements_, reinterpret_cast<const void*>(objects_ + kOffMaxElements), 4);
    safe_read(&num_elements_, reinterpret_cast<const void*>(objects_ + kOffNumElements), 4);
    safe_read(&max_chunks_,   reinterpret_cast<const void*>(objects_ + kOffMaxChunks), 4);
    safe_read(&num_chunks_,   reinterpret_cast<const void*>(objects_ + kOffNumChunks), 4);

    return chunk_table_ != 0 && num_elements_ > 0;
}

int ObjectArray::validate() const {
    if (!gobjects_ || !objects_ || !chunk_table_) return 0;

    // 1) 计数字段互相自洽
    if (num_elements_ < 1 || num_elements_ > 8'000'000) return 0;
    if (num_elements_ > max_elements_ || max_elements_ > 32'000'000) return 0;

    const int32_t expect_chunks = (num_elements_ + kChunkItems - 1) / kChunkItems;
    if (num_chunks_ == 0 || num_chunks_ != expect_chunks) return 0;
    if (max_chunks_ < 1 || max_chunks_ > 8192 || max_chunks_ < num_chunks_) return 0;

    // 2) 块表与第 0 块必须指向真实可读内存
    if (!probe_readable(reinterpret_cast<const void*>(chunk_table_), 8)) return 0;
    const uint64_t c0 = chunk_at(0);
    if (!c0 || !probe_readable(reinterpret_cast<const void*>(c0), kItemStride)) return 0;

    // 3) 抽前 8 个对象, 它们的 vtable 必须落在某个已映射模块的范围内。
    //    这是最强的一条证据 —— 垃圾指针几乎不可能连续满足。
    int ok = 0, bad = 0;
    for (int32_t idx = 0; idx < 8; ++idx) {
        const uint64_t obj = object_at(idx);
        if (!obj) { ++bad; continue; }
        uint64_t vtbl = 0;
        if (!safe_read(&vtbl, reinterpret_cast<const void*>(obj), 8)) { ++bad; continue; }
        // vtable 落在模块映像里 = 合法 C++ 对象
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(vtbl), &mbi, sizeof(mbi)) == 0 ||
            mbi.Type != MEM_IMAGE) {
            ++bad;
            continue;
        }
        ++ok;
    }
    if (ok == 0 || ok < bad) return 0;

    // 4) 如果已连上名字池, 再验一次名字可解析 —— 置信度拉满
    if (names_ && names_->valid()) {
        int name_ok = 0;
        for (int32_t idx = 0; idx < 8; ++idx) {
            const uint64_t obj = object_at(idx);
            if (obj && !name_of(obj).empty()) ++name_ok;
        }
        if (name_ok >= 4) return 3;
    }
    return (ok >= 6) ? 2 : 1;
}

uint64_t ObjectArray::chunk_at(int32_t chunk_idx) const {
    if (!chunk_table_ || chunk_idx < 0 || chunk_idx >= max_chunks_) return 0;
    uint64_t ch = 0;
    if (!safe_read(&ch, reinterpret_cast<const void*>(chunk_table_ + 8ull * chunk_idx), 8)) return 0;
    return ch;
}

uint64_t ObjectArray::item_at(int32_t index) const {
    if (index < 0 || !chunk_table_) return 0;
    const uint64_t ch = chunk_at(index / kChunkItems);
    if (!ch) return 0;
    return ch + static_cast<uint64_t>(index % kChunkItems) * kItemStride;
}

uint64_t ObjectArray::object_at(int32_t index) const {
    const uint64_t it = item_at(index);
    if (!it) return 0;
    uint64_t obj = 0;
    if (!safe_read(&obj, reinterpret_cast<const void*>(it), 8)) return 0;
    return obj;
}

uint64_t ObjectArray::class_of(uint64_t obj) const {
    uint64_t c = 0;
    if (!obj || !safe_read(&c, reinterpret_cast<const void*>(obj + kOffObjClass), 8)) return 0;
    return c;
}

uint64_t ObjectArray::outer_of(uint64_t obj) const {
    uint64_t o = 0;
    if (!obj || !safe_read(&o, reinterpret_cast<const void*>(obj + kOffObjOuter), 8)) return 0;
    return o;
}

int32_t ObjectArray::name_index_of(uint64_t obj) const {
    int32_t idx = 0;
    if (!obj || !safe_read(&idx, reinterpret_cast<const void*>(obj + kOffObjName), 4)) return 0;
    return idx;
}

std::string ObjectArray::name_of(uint64_t obj) const {
    if (!obj || !names_ || !names_->valid()) return {};
    return names_->resolve(name_index_of(obj));
}

std::string ObjectArray::class_name_of(uint64_t obj) const {
    const uint64_t c = class_of(obj);
    if (!c || !names_ || !names_->valid()) return {};
    // UClass 本身也是 UObject, 所以类名 = 类的对象的 NamePrivate
    return names_->resolve(name_index_of(c));
}

std::string ObjectArray::outer_name_of(uint64_t obj) const {
    const uint64_t o = outer_of(obj);
    if (!o || !names_ || !names_->valid()) return {};
    return names_->resolve(name_index_of(o));
}

std::string ObjectArray::full_name_of(uint64_t obj) const {
    if (!obj) return {};
    const std::string cls  = class_name_of(obj);
    const std::string name = name_of(obj);
    if (cls.empty() && name.empty()) return {};
    if (cls.empty()) return name;
    if (name.empty()) return cls;
    return cls + " " + name;
}

ObjectStat ObjectArray::describe(int32_t index) const {
    ObjectStat st;
    st.index = index;
    st.address = object_at(index);
    if (!st.address) return st;
    st.klass = class_of(st.address);
    st.name = name_of(st.address);
    st.class_name = class_name_of(st.address);
    st.outer_name = outer_name_of(st.address);
    st.full_name = full_name_of(st.address);
    return st;
}

void ObjectArray::for_each(std::function<bool(ObjectStat const&)> const& fn,
                           int32_t start, int32_t max_count) const {
    if (!valid()) return;
    const int32_t total = num_elements_;
    const int32_t from  = std::max(0, start);
    const int32_t to    = (max_count < 0) ? total : std::min(total, from + max_count);

    for (int32_t i = from; i < to; ++i) {
        ObjectStat st = describe(i);
        if (!st.address) continue;      // 空槽
        if (!fn(st)) break;
    }
}

uint64_t ObjectArray::find_object_by_name(std::string_view name) const {
    if (!valid() || name.empty()) return 0;
    const int32_t total = num_elements_;
    for (int32_t i = 0; i < total; ++i) {
        const uint64_t obj = object_at(i);
        if (!obj) continue;
        if (iequals(name_of(obj), name)) return obj;
    }
    return 0;
}

uint64_t ObjectArray::find_object_by_full_name(std::string_view full) const {
    if (!valid() || full.empty()) return 0;
    const int32_t total = num_elements_;
    for (int32_t i = 0; i < total; ++i) {
        const uint64_t obj = object_at(i);
        if (!obj) continue;
        if (iequals(full_name_of(obj), full)) return obj;
    }
    return 0;
}

uint64_t ObjectArray::find_class(std::string_view class_name) const {
    if (!valid() || class_name.empty()) return 0;
    const int32_t total = num_elements_;
    for (int32_t i = 0; i < total; ++i) {
        const uint64_t obj = object_at(i);
        if (!obj) continue;
        // 类的对象名字就是类名, 且它的类是 "Class"
        if (!iequals(name_of(obj), class_name)) continue;
        if (iequals(class_name_of(obj), "Class")) return obj;
    }
    return 0;
}

} // namespace mcd2::ue
