#include "record_manager.h"
#include "variable_page.h"
#include <algorithm>
#include <limits>

namespace db {
namespace {
using detail::VariablePage;
using detail::SlotKind;
constexpr std::size_t PAGE_BYTES = bufPageManager::PAGE_SIZE;
constexpr std::size_t ENTRIES = detail::DIRECTORY_ENTRIES;
constexpr std::size_t DIRECTORY_HEADER = detail::DIRECTORY_HEADER_BYTES;
constexpr std::uint32_t DIRECTORY_MAGIC = 0x32445346U;

std::uint32_t number(const char* p, std::size_t n) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < n; ++i) value |= std::uint32_t(static_cast<unsigned char>(p[i])) << (8 * i);
    return value;
}
void put(char* p, std::uint32_t value, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<char>((value >> (8 * i)) & 255);
}
[[noreturn]] void corrupt(const char* message) { throw RecordException(RecordError::CORRUPT_FILE, message); }
[[noreturn]] void missing() { throw RecordException(RecordError::RECORD_NOT_FOUND, "record slot is empty or internal"); }
bool visible(SlotKind kind) { return kind == SlotKind::RECORD || kind == SlotKind::FORWARD; }

VariablePage::Page directory(PageId id, std::uint32_t group) {
    VariablePage::Page page{};
    put(page.data(), DIRECTORY_MAGIC, 4);
    put(page.data() + 4, id, 4);
    put(page.data() + 8, group, 4);
    return page;
}
}

bool RecordFile::isDataPage(PageId page) const noexcept {
    if (page < first_data_page_ || page >= page_count_) return false;
    return layout() != RecordLayout::VARIABLE || (page - first_data_page_ + 1) % (ENTRIES + 1) != 0;
}

PageId RecordFile::variablePageId(std::uint32_t index) const {
    const auto id = std::uint64_t(first_data_page_) + index + index / ENTRIES;
    if (id >= std::numeric_limits<PageId>::max() - 1)
        throw RecordException(RecordError::FILE_TOO_LARGE, "record file has exhausted page identifiers");
    return static_cast<PageId>(id);
}

void RecordFile::initializeDirectory() { writePage(first_data_page_ - 1, directory(first_data_page_ - 1, 0)); }

RecordFile::Page RecordFile::readDirectory(std::uint32_t group) const {
    const auto groups = std::max<std::uint64_t>(1, (std::uint64_t(variable_data_pages_) + ENTRIES - 1) / ENTRIES);
    if (group >= groups) corrupt("free-space directory group out of range");
    const auto id = static_cast<PageId>(first_data_page_ - 1 + std::uint64_t(group) * (ENTRIES + 1));
    const auto bytes = readPage(id);
    const auto count = std::min<std::uint64_t>(ENTRIES, std::uint64_t(variable_data_pages_) - group * ENTRIES);
    if (number(bytes.data(), 4) != DIRECTORY_MAGIC || number(bytes.data() + 4, 4) != id ||
        number(bytes.data() + 8, 4) != group || number(bytes.data() + 12, 2) != count)
        corrupt("invalid free-space directory header");
    for (std::size_t i = 0; i < count; ++i)
        if (number(bytes.data() + DIRECTORY_HEADER + i * 2, 2) > PAGE_BYTES - VariablePage::HEADER_BYTES)
            corrupt("invalid free-space directory capacity");
    return bytes;
}

VariablePage RecordFile::readVariablePage(PageId id) const {
    if (!isDataPage(id)) corrupt("variable record references a non-data page");
    VariablePage page(readPage(id));
    if (number(page.bytes().data() + 4, 4) != id) corrupt("variable data page id mismatch");
    for (std::uint32_t i = 0; i < VariablePage::slotCount(page.bytes()); ++i) {
        const auto slot = page.slot(i);
        if ((slot.kind == SlotKind::FORWARD || slot.kind == SlotKind::TARGET) &&
            (!isDataPage(slot.link.page) || slot.link.page == id || slot.link.slot >= slots_per_page_))
            corrupt("invalid variable slot reference");
    }
    return page;
}

void RecordFile::writeVariablePage(PageId id, const VariablePage& page) {
    const auto relative = id - first_data_page_;
    const auto group = static_cast<std::uint32_t>(relative / (ENTRIES + 1));
    const auto index = static_cast<std::size_t>(relative % (ENTRIES + 1));
    auto bytes = readDirectory(group);
    put(bytes.data() + DIRECTORY_HEADER + index * 2, page.insertCapacity(), 2);
    writePage(id, page.bytes());
    const auto directory_id = static_cast<PageId>(first_data_page_ - 1 + std::uint64_t(group) * (ENTRIES + 1));
    writePage(directory_id, bytes);
}

RID RecordFile::allocateVariable(const std::vector<char>& bytes, std::optional<RID> owner,
                                 const std::vector<PageId>& excluded) {
    const auto kind = owner ? SlotKind::TARGET : SlotKind::RECORD;
    const auto groups = std::max<std::uint64_t>(1, (std::uint64_t(variable_data_pages_) + ENTRIES - 1) / ENTRIES);
    for (std::uint32_t group = 0; group < groups; ++group) {
        const auto info = readDirectory(group);
        const auto count = number(info.data() + 12, 2);
        for (std::uint32_t entry = 0; entry < count; ++entry) {
            if (number(info.data() + DIRECTORY_HEADER + entry * 2, 2) < bytes.size()) continue;
            const auto id = variablePageId(static_cast<std::uint32_t>(group * ENTRIES + entry));
            if (std::find(excluded.begin(), excluded.end(), id) != excluded.end()) continue;
            auto page = readVariablePage(id);
            if (page.insertCapacity() != number(info.data() + DIRECTORY_HEADER + entry * 2, 2))
                corrupt("free-space directory disagrees with data page");
            const auto slot = page.insert(bytes, kind, owner.value_or(RID{}));
            writeVariablePage(id, page);
            return RID{id, slot};
        }
    }

    const auto index = variable_data_pages_;
    const auto group = static_cast<std::uint32_t>(index / ENTRIES);
    const auto entry = static_cast<std::size_t>(index % ENTRIES);
    const auto id = variablePageId(index); // 在任何写入之前检查页号溢出。
    VariablePage page(id);
    const auto slot = page.insert(bytes, kind, owner.value_or(RID{}));
    const auto directory_id = static_cast<PageId>(first_data_page_ - 1 + std::uint64_t(group) * (ENTRIES + 1));
    auto info = (entry == 0 && index != 0) ? directory(directory_id, group) : readDirectory(group);
    put(info.data() + 12, static_cast<std::uint32_t>(entry + 1), 2);
    put(info.data() + DIRECTORY_HEADER + entry * 2, page.insertCapacity(), 2);
    writePage(directory_id, info);
    writePage(id, page.bytes());
    ++variable_data_pages_;
    page_count_ = id + 1;
    return RID{id, slot};
}

RID RecordFile::forwardedTarget(RID owner, const VariablePage& root) const {
    const auto forward = root.slot(owner.slot);
    if (forward.kind != SlotKind::FORWARD) corrupt("expected forwarding slot");
    const auto target = readVariablePage(forward.link.page);
    if (forward.link.slot >= VariablePage::slotCount(target.bytes())) corrupt("forwarding slot points outside directory");
    const auto slot = target.slot(forward.link.slot);
    if (slot.kind != SlotKind::TARGET || slot.link != owner) corrupt("invalid forwarding target or backlink");
    return forward.link;
}

Record RecordFile::getVariable(RID rid) const {
    const auto page = readVariablePage(rid.page);
    const auto slot = page.slot(rid.slot);
    if (slot.kind == SlotKind::RECORD) return codec_.decode(page.bytes().data() + slot.offset, slot.length);
    if (slot.kind != SlotKind::FORWARD) missing();
    const auto target_id = forwardedTarget(rid, page);
    const auto target = readVariablePage(target_id.page);
    const auto payload = target.slot(target_id.slot);
    return codec_.decode(target.bytes().data() + payload.offset, payload.length);
}

bool RecordFile::containsVariable(RID rid) const {
    const auto page = readVariablePage(rid.page);
    if (rid.slot >= VariablePage::slotCount(page.bytes())) return false;
    const auto slot = page.slot(rid.slot);
    if (slot.kind == SlotKind::FORWARD) forwardedTarget(rid, page);
    return visible(slot.kind);
}

void RecordFile::updateVariable(RID rid, const std::vector<char>& bytes) {
    auto root = readVariablePage(rid.page);
    const auto slot = root.slot(rid.slot);
    if (!visible(slot.kind)) missing();
    if (slot.kind == SlotKind::RECORD) {
        if (root.canReplace(rid.slot, bytes.size())) {
            root.replace(rid.slot, bytes, SlotKind::RECORD, RID{});
        } else {
            const auto target = allocateVariable(bytes, rid, {rid.page});
            root.replace(rid.slot, {}, SlotKind::FORWARD, target);
        }
        writeVariablePage(rid.page, root);
        return;
    }
    const auto old_id = forwardedTarget(rid, root);
    auto old = readVariablePage(old_id.page);
    if (root.canReplace(rid.slot, bytes.size())) {
        // 缩短后优先搬回原槽，移除转发，提高后续读取效率。
        root.replace(rid.slot, bytes, SlotKind::RECORD, RID{});
        writeVariablePage(rid.page, root);
        old.erase(old_id.slot);
        writeVariablePage(old_id.page, old);
    } else if (old.canReplace(old_id.slot, bytes.size())) {
        old.replace(old_id.slot, bytes, SlotKind::TARGET, rid);
        writeVariablePage(old_id.page, old);
    } else {
        const auto target = allocateVariable(bytes, rid, {rid.page, old_id.page});
        root.replace(rid.slot, {}, SlotKind::FORWARD, target);
        writeVariablePage(rid.page, root);
        old.erase(old_id.slot);
        writeVariablePage(old_id.page, old);
    }
}

void RecordFile::removeVariable(RID rid) {
    auto root = readVariablePage(rid.page);
    const auto slot = root.slot(rid.slot);
    if (!visible(slot.kind)) missing();
    if (!record_count_) corrupt("record count is zero for an occupied slot");
    if (slot.kind == SlotKind::FORWARD) {
        const auto target = forwardedTarget(rid, root);
        auto page = readVariablePage(target.page);
        page.erase(target.slot);
        writeVariablePage(target.page, page);
    }
    root.erase(rid.slot);
    writeVariablePage(rid.page, root);
}

} // namespace db
