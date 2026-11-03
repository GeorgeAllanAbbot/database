#include "variable_page.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace db::detail {
namespace {
constexpr std::uint32_t MAGIC = 0x32504752U;
std::uint32_t get(const char* p, std::size_t n) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < n; ++i) value |= std::uint32_t(static_cast<unsigned char>(p[i])) << (8 * i);
    return value;
}
void put(char* p, std::uint32_t value, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<char>((value >> (8 * i)) & 255);
}
[[noreturn]] void corrupt(const char* message) { throw RecordException(RecordError::CORRUPT_FILE, message); }
std::size_t directoryOffset(std::uint32_t slot) { return bufPageManager::PAGE_SIZE - (slot + 1) * VariablePage::SLOT_BYTES; }
bool hasPayload(SlotKind kind) { return kind == SlotKind::RECORD || kind == SlotKind::TARGET; }
}

VariablePage::VariablePage(PageId id) {
    put(bytes_.data(), MAGIC, 4);
    put(bytes_.data() + 4, id, 4);
    put(bytes_.data() + 12, HEADER_BYTES, 2);
    put(bytes_.data() + 14, bytes_.size() - HEADER_BYTES, 2);
}

VariablePage::VariablePage(Page bytes) : bytes_(std::move(bytes)) { validate(); }

std::uint16_t VariablePage::slotCount(const Page& bytes) { return static_cast<std::uint16_t>(get(bytes.data() + 8, 2)); }

VariableSlot VariablePage::slot(const Page& bytes, std::uint32_t index) {
    if (slotCount(bytes) > MAX_SLOTS) corrupt("invalid variable slot count");
    if (index >= slotCount(bytes)) throw RecordException(RecordError::RECORD_NOT_FOUND, "record slot is empty");
    const char* p = bytes.data() + directoryOffset(index);
    return VariableSlot{static_cast<std::uint16_t>(get(p, 2)), static_cast<std::uint16_t>(get(p + 2, 2)),
        static_cast<SlotKind>(get(p + 4, 1)), RID{get(p + 8, 4), get(p + 12, 4)}};
}

void VariablePage::validate() const {
    if (get(bytes_.data(), 4) != MAGIC || slotCount(bytes_) > MAX_SLOTS) corrupt("invalid variable data page header");
    const auto count = slotCount(bytes_);
    const auto directory = bytes_.size() - count * SLOT_BYTES;
    std::size_t end = HEADER_BYTES;
    std::size_t used = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = slot(i);
        if (entry.kind != SlotKind::EMPTY && entry.kind != SlotKind::RECORD &&
            entry.kind != SlotKind::FORWARD && entry.kind != SlotKind::TARGET) corrupt("invalid variable slot kind");
        if (entry.kind != SlotKind::EMPTY) ++used;
        if (hasPayload(entry.kind)) {
            if (entry.length == 0 || entry.offset != end || entry.length > directory - end)
                corrupt("variable record payload overlaps or exceeds its page");
            end += entry.length;
        } else if (entry.length || entry.offset) corrupt("invalid empty or forwarding slot payload");
        if ((entry.kind == SlotKind::EMPTY || entry.kind == SlotKind::RECORD) && (entry.link.page || entry.link.slot))
            corrupt("unexpected slot link");
    }
    if (get(bytes_.data() + 10, 2) != used || get(bytes_.data() + 12, 2) != end ||
        get(bytes_.data() + 14, 2) != directory - end) corrupt("invalid variable page free-space accounting");
}

std::uint16_t VariablePage::insertCapacity() const {
    const auto free = get(bytes_.data() + 14, 2);
    for (std::uint32_t i = 0; i < slotCount(bytes_); ++i)
        if (slot(i).kind == SlotKind::EMPTY) return static_cast<std::uint16_t>(free);
    return static_cast<std::uint16_t>(free > SLOT_BYTES ? free - SLOT_BYTES : 0);
}

bool VariablePage::canReplace(std::uint32_t index, std::size_t size) const {
    return size <= get(bytes_.data() + 14, 2) + slot(index).length;
}

std::uint32_t VariablePage::insert(const std::vector<char>& record, SlotKind kind, RID link) {
    if (record.empty() || record.size() > insertCapacity())
        throw RecordException(RecordError::FILE_TOO_LARGE, "record does not fit the selected variable page");
    std::uint32_t index = 0;
    while (index < slotCount(bytes_) && slot(index).kind != SlotKind::EMPTY) ++index;
    rebuild(index, record, kind, link);
    return index;
}

void VariablePage::replace(std::uint32_t index, const std::vector<char>& record, SlotKind kind, RID link) {
    if (!canReplace(index, record.size()))
        throw RecordException(RecordError::FILE_TOO_LARGE, "updated record does not fit its page");
    rebuild(index, record, kind, link);
}

void VariablePage::erase(std::uint32_t index) { replace(index, {}, SlotKind::EMPTY, RID{}); }

void VariablePage::rebuild(std::uint32_t changed, const std::vector<char>& record, SlotKind kind, RID link) {
    // 一页内重排 payload，目录下标不变；删除和缩短产生的碎片立即回收。
    Page result{};
    std::memcpy(result.data(), bytes_.data(), HEADER_BYTES);
    auto count = static_cast<std::uint16_t>(std::max<std::uint32_t>(slotCount(bytes_), changed + 1));
    if (count > MAX_SLOTS) corrupt("variable slot directory overflow");
    // 只裁掉末尾的空槽，现存记录的槽号保持不变；空页可完全回收槽目录空间。
    while (count) {
        const auto last = static_cast<std::uint32_t>(count - 1);
        if ((last == changed ? kind : slot(last).kind) != SlotKind::EMPTY) break;
        --count;
    }
    std::size_t end = HEADER_BYTES;
    std::size_t used = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = i == changed ? VariableSlot{0, static_cast<std::uint16_t>(record.size()), kind, link} : slot(i);
        char* destination = result.data() + directoryOffset(i);
        if (hasPayload(entry.kind)) {
            if (end + entry.length > result.size() - count * SLOT_BYTES) corrupt("variable page payload overflow");
            put(destination, static_cast<std::uint32_t>(end), 2);
            put(destination + 2, entry.length, 2);
            const char* source = i == changed ? record.data() : bytes_.data() + entry.offset;
            std::memcpy(result.data() + end, source, entry.length);
            end += entry.length;
        }
        put(destination + 4, static_cast<std::uint8_t>(entry.kind), 1);
        put(destination + 8, entry.link.page, 4);
        put(destination + 12, entry.link.slot, 4);
        used += entry.kind != SlotKind::EMPTY;
    }
    put(result.data() + 8, count, 2);
    put(result.data() + 10, static_cast<std::uint32_t>(used), 2);
    put(result.data() + 12, static_cast<std::uint32_t>(end), 2);
    put(result.data() + 14, static_cast<std::uint32_t>(result.size() - count * SLOT_BYTES - end), 2);
    bytes_ = result;
}

} // namespace db::detail
