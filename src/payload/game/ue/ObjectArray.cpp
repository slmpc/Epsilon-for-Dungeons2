#include "payload/game/ue/ObjectArray.h"

#include "common/PeImage.h"
#include "common/Text.h"
#include "payload/game/Field.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace epsilon::game::ue {
namespace {

namespace oa = offsets::objectArray;
namespace uo = offsets::object;

// 抽 count 个对象, 统计 vtable 落在已映射模块映像里的比例。
struct VtableScan {
    int ok = 0;
    int bad = 0;
};

VtableScan scanVtables(ObjectArray const& objects, int32_t count) {
    VtableScan s;
    for (int32_t idx = 0; idx < count; ++idx) {
        const uint64_t obj = objects.objectAt(idx);
        if (!obj) { ++s.bad; continue; }

        auto vtbl = readPtr(obj, 0);
        if (!vtbl) { ++s.bad; continue; }

        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(*vtbl), &mbi, sizeof(mbi)) == 0 ||
            mbi.Type != MEM_IMAGE) {
            ++s.bad;
            continue;
        }
        ++s.ok;
    }
    return s;
}

} // namespace

bool ObjectArray::refresh() {
    objects_ = 0;
    chunkTable_ = 0;
    numElements_ = maxElements_ = numChunks_ = maxChunks_ = 0;

    if (!gObjects_) return false;

    // TUObjectArray 内嵌在 FUObjectArray 里, 计数直接读 GObjects 本身。
    auto chunkTable = readPtr(gObjects_, oa::chunkTable);
    if (!chunkTable) return false;
    chunkTable_ = *chunkTable;

    maxElements_ = readI32(gObjects_, oa::maxElements).value_or(0);
    numElements_ = readI32(gObjects_, oa::numElements).value_or(0);
    maxChunks_   = readI32(gObjects_, oa::maxChunks).value_or(0);
    numChunks_   = readI32(gObjects_, oa::numChunks).value_or(0);

    objects_ = gObjects_ + oa::chunkTable;

    return numElements_ > 0;
}

int ObjectArray::validate() const {
    if (!gObjects_ || !objects_ || !chunkTable_) return 0;

    // 1) 计数字段互相自洽
    if (numElements_ < 1 || numElements_ > 8'000'000) return 0;
    if (numElements_ > maxElements_ || maxElements_ > 32'000'000) return 0;

    const int32_t expectChunks =
        (numElements_ + static_cast<int32_t>(oa::itemsPerChunk) - 1) /
        static_cast<int32_t>(oa::itemsPerChunk);
    if (numChunks_ == 0 || numChunks_ != expectChunks) return 0;
    if (maxChunks_ < 1 || maxChunks_ > 8192 || maxChunks_ < numChunks_) return 0;

    // 2) 块表与第 0 块必须指向真实可读内存
    if (!probeReadable(reinterpret_cast<const void*>(chunkTable_), 8)) return 0;
    const uint64_t c0 = chunkAt(0);
    if (!c0 || !probeReadable(reinterpret_cast<const void*>(c0), oa::itemStride)) return 0;

    // 3) 抽样对象的 vtable 必须落在模块映像里 —— 垃圾指针几乎不可能连续满足
    const VtableScan scan = scanVtables(*this, 8);
    if (scan.ok == 0 || scan.ok < scan.bad) return 0;

    // 4) 名字池已就绪时再验一次名字可解析, 置信度拉满
    if (names_ && names_->valid()) {
        int nameOk = 0;
        for (int32_t idx = 0; idx < 8; ++idx) {
            const uint64_t obj = objectAt(idx);
            if (obj && !nameOf(obj).empty()) ++nameOk;
        }
        if (nameOk >= 4) return 3;
    }
    return (scan.ok >= 6) ? 2 : 1;
}

uint64_t ObjectArray::chunkAt(int32_t chunkIdx) const {
    if (!chunkTable_ || chunkIdx < 0 || chunkIdx >= maxChunks_) return 0;
    return readPtr(chunkTable_, static_cast<uint32_t>(chunkIdx) * 8u).value_or(0);
}

uint64_t ObjectArray::itemAt(int32_t index) const {
    if (index < 0 || !chunkTable_) return 0;

    const uint64_t ch = chunkAt(index / static_cast<int32_t>(oa::itemsPerChunk));
    if (!ch) return 0;

    return ch + static_cast<uint64_t>(index % static_cast<int32_t>(oa::itemsPerChunk)) *
                    oa::itemStride;
}

uint64_t ObjectArray::objectAt(int32_t index) const {
    const uint64_t item = itemAt(index);
    if (!item) return 0;
    return readPtr(item, 0).value_or(0);
}

uint64_t ObjectArray::classOf(uint64_t obj) const {
    return readPtr(obj, uo::classPrivate).value_or(0);
}

uint64_t ObjectArray::outerOf(uint64_t obj) const {
    return readPtr(obj, uo::outerPrivate).value_or(0);
}

int32_t ObjectArray::nameIndexOf(uint64_t obj) const {
    return readI32(obj, uo::namePrivate).value_or(0);
}

std::string ObjectArray::nameOf(uint64_t obj) const {
    if (!obj || !names_ || !names_->valid()) return {};
    return names_->resolve(nameIndexOf(obj));
}

std::string ObjectArray::classNameOf(uint64_t obj) const {
    const uint64_t klass = classOf(obj);
    if (!klass || !names_ || !names_->valid()) return {};
    // UClass 本身也是 UObject, 类名就是那个 UClass 对象的 NamePrivate
    return names_->resolve(nameIndexOf(klass));
}

std::string ObjectArray::outerNameOf(uint64_t obj) const {
    const uint64_t outer = outerOf(obj);
    if (!outer || !names_ || !names_->valid()) return {};
    return names_->resolve(nameIndexOf(outer));
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
        if (!st.address) continue;
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
        // 类对象的对象名就是类名, 且它自己的类是 "Class"
        if (!iequals(nameOf(obj), className)) continue;
        if (iequals(classNameOf(obj), "Class")) return obj;
    }
    return 0;
}

} // namespace epsilon::game::ue
