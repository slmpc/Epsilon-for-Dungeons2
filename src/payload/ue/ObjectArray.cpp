// ============================================================================
//  objectArray.cpp
// ============================================================================
#include "payload/ue/ObjectArray.h"

#include "common/PeImage.h"
#include "common/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace epsilon::ue {
namespace {

std::string readCstrAt(uint64_t va, size_t maxLen) {
    if (!va) return {};
    std::string s(maxLen, '\0');
    for (size_t i = 0; i < maxLen; ++i) {
        char c = 0;
        if (!safeRead(&c, reinterpret_cast<const void*>(va + i), 1)) break;
        if (c == '\0') { s.resize(i); return s; }
        s[i] = c;
    }
    s.resize(maxLen);
    return s;
}

} // namespace

bool ObjectArray::refresh() {
    objects_ = 0;
    chunkTable_ = 0;
    numElements_ = maxElements_ = numChunks_ = maxChunks_ = 0;

    if (!gObjects_) return false;

    // ⚠️ 布局要点: TUObjectArray 是**内嵌**在 FUObjectArray 里的(+0x10), 不是
    //    再一层指针。所以块表就是 *(u64*)(GObjects + 0x10), 计数也在
    //    GObjects 上直接读 —— 不要先解一层引用再 +0x10, 那会读到完全不相关
    //    的地方, 现象是"读不到 FUObjectArray"。
    //
    //    真机实测(UE 5.6.1)在 GObjects 处的字节:
    //        +0x10 块表指针        +0x20 MaxElements=2162688
    //        +0x24 NumElements=149034  +0x28 MaxChunks=33  +0x2C NumChunks=3
    //    149034/65536 向上取整 = 3, 2162688/65536 = 33 —— 完全自洽, 这就是
    //    判定该布局正确的依据。
    if (!safeRead(&chunkTable_, reinterpret_cast<const void*>(gObjects_ + offChunkTable), 8))
        return false;
    if (!chunkTable_) return false;

    safeRead(&maxElements_, reinterpret_cast<const void*>(gObjects_ + offMaxElements), 4);
    safeRead(&numElements_, reinterpret_cast<const void*>(gObjects_ + offNumElements), 4);
    safeRead(&maxChunks_,   reinterpret_cast<const void*>(gObjects_ + offMaxChunks), 4);
    safeRead(&numChunks_,   reinterpret_cast<const void*>(gObjects_ + offNumChunks), 4);

    // objects_ 只是给上层做显示用的"TUObjectArray 起始地址"。
    objects_ = gObjects_ + offObjectsPtr;

    return numElements_ > 0;
}

int ObjectArray::validate() const {
    if (!gObjects_ || !objects_ || !chunkTable_) return 0;

    // 1) 计数字段互相自洽
    if (numElements_ < 1 || numElements_ > 8'000'000) return 0;
    if (numElements_ > maxElements_ || maxElements_ > 32'000'000) return 0;

    const int32_t expectChunks = (numElements_ + chunkItems - 1) / chunkItems;
    if (numChunks_ == 0 || numChunks_ != expectChunks) return 0;
    if (maxChunks_ < 1 || maxChunks_ > 8192 || maxChunks_ < numChunks_) return 0;

    // 2) 块表与第 0 块必须指向真实可读内存
    if (!probeReadable(reinterpret_cast<const void*>(chunkTable_), 8)) return 0;
    const uint64_t c0 = chunkAt(0);
    if (!c0 || !probeReadable(reinterpret_cast<const void*>(c0), itemStride)) return 0;

    // 3) 抽前 8 个对象, 它们的 vtable 必须落在某个已映射模块的范围内。
    //    这是最强的一条证据 —— 垃圾指针几乎不可能连续满足。
    int ok = 0, bad = 0;
    for (int32_t idx = 0; idx < 8; ++idx) {
        const uint64_t obj = objectAt(idx);
        if (!obj) { ++bad; continue; }
        uint64_t vtbl = 0;
        if (!safeRead(&vtbl, reinterpret_cast<const void*>(obj), 8)) { ++bad; continue; }
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
        int nameOk = 0;
        for (int32_t idx = 0; idx < 8; ++idx) {
            const uint64_t obj = objectAt(idx);
            if (obj && !nameOf(obj).empty()) ++nameOk;
        }
        if (nameOk >= 4) return 3;
    }
    return (ok >= 6) ? 2 : 1;
}

uint64_t ObjectArray::chunkAt(int32_t chunkIdx) const {
    if (!chunkTable_ || chunkIdx < 0 || chunkIdx >= maxChunks_) return 0;
    uint64_t ch = 0;
    if (!safeRead(&ch, reinterpret_cast<const void*>(chunkTable_ + 8ull * chunkIdx), 8)) return 0;
    return ch;
}

uint64_t ObjectArray::itemAt(int32_t index) const {
    if (index < 0 || !chunkTable_) return 0;
    const uint64_t ch = chunkAt(index / chunkItems);
    if (!ch) return 0;
    return ch + static_cast<uint64_t>(index % chunkItems) * itemStride;
}

uint64_t ObjectArray::objectAt(int32_t index) const {
    const uint64_t it = itemAt(index);
    if (!it) return 0;
    uint64_t obj = 0;
    if (!safeRead(&obj, reinterpret_cast<const void*>(it), 8)) return 0;
    return obj;
}

uint64_t ObjectArray::classOf(uint64_t obj) const {
    uint64_t c = 0;
    if (!obj || !safeRead(&c, reinterpret_cast<const void*>(obj + offObjClass), 8)) return 0;
    return c;
}

uint64_t ObjectArray::outerOf(uint64_t obj) const {
    uint64_t o = 0;
    if (!obj || !safeRead(&o, reinterpret_cast<const void*>(obj + offObjOuter), 8)) return 0;
    return o;
}

int32_t ObjectArray::nameIndexOf(uint64_t obj) const {
    int32_t idx = 0;
    if (!obj || !safeRead(&idx, reinterpret_cast<const void*>(obj + offObjName), 4)) return 0;
    return idx;
}

std::string ObjectArray::nameOf(uint64_t obj) const {
    if (!obj || !names_ || !names_->valid()) return {};
    return names_->resolve(nameIndexOf(obj));
}

std::string ObjectArray::classNameOf(uint64_t obj) const {
    const uint64_t c = classOf(obj);
    if (!c || !names_ || !names_->valid()) return {};
    // UClass 本身也是 UObject, 所以类名 = 类的对象的 NamePrivate
    return names_->resolve(nameIndexOf(c));
}

std::string ObjectArray::outerNameOf(uint64_t obj) const {
    const uint64_t o = outerOf(obj);
    if (!o || !names_ || !names_->valid()) return {};
    return names_->resolve(nameIndexOf(o));
}

std::string ObjectArray::fullNameOf(uint64_t obj) const {
    if (!obj) return {};
    const std::string cls  = classNameOf(obj);
    const std::string name = nameOf(obj);
    if (cls.empty() && name.empty()) return {};
    if (cls.empty()) return name;
    if (name.empty()) return cls;
    return cls + " " + name;
}

ObjectStat ObjectArray::describe(int32_t index) const {
    ObjectStat st;
    st.index = index;
    st.address = objectAt(index);
    if (!st.address) return st;
    st.klass = classOf(st.address);
    st.name = nameOf(st.address);
    st.className = classNameOf(st.address);
    st.outerName = outerNameOf(st.address);
    st.fullName = fullNameOf(st.address);
    return st;
}

void ObjectArray::for_each(std::function<bool(ObjectStat const&)> const& fn,
                           int32_t start, int32_t maxCount) const {
    if (!valid()) return;
    const int32_t total = numElements_;
    const int32_t from  = std::max(0, start);
    const int32_t to    = (maxCount < 0) ? total : std::min(total, from + maxCount);

    for (int32_t i = from; i < to; ++i) {
        ObjectStat st = describe(i);
        if (!st.address) continue;      // 空槽
        if (!fn(st)) break;
    }
}

uint64_t ObjectArray::findObjectByName(std::string_view name) const {
    if (!valid() || name.empty()) return 0;
    const int32_t total = numElements_;
    for (int32_t i = 0; i < total; ++i) {
        const uint64_t obj = objectAt(i);
        if (!obj) continue;
        if (iequals(nameOf(obj), name)) return obj;
    }
    return 0;
}

uint64_t ObjectArray::findObjectByFullName(std::string_view full) const {
    if (!valid() || full.empty()) return 0;
    const int32_t total = numElements_;
    for (int32_t i = 0; i < total; ++i) {
        const uint64_t obj = objectAt(i);
        if (!obj) continue;
        if (iequals(fullNameOf(obj), full)) return obj;
    }
    return 0;
}

uint64_t ObjectArray::findClass(std::string_view className) const {
    if (!valid() || className.empty()) return 0;
    const int32_t total = numElements_;
    for (int32_t i = 0; i < total; ++i) {
        const uint64_t obj = objectAt(i);
        if (!obj) continue;
        // 类的对象名字就是类名, 且它的类是 "Class"
        if (!iequals(nameOf(obj), className)) continue;
        if (iequals(classNameOf(obj), "Class")) return obj;
    }
    return 0;
}

} // namespace epsilon::ue
