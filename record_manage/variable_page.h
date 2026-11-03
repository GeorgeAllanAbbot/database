#pragma once

// 记录层内部页格式；调用方通过 RecordFile 操作，不应直接修改这些页面。
#include "record_manager.h"

namespace db::detail {

inline constexpr std::size_t DIRECTORY_HEADER_BYTES = 16;
inline constexpr std::size_t DIRECTORY_ENTRIES = (bufPageManager::PAGE_SIZE - DIRECTORY_HEADER_BYTES) / 2;

enum class SlotKind : std::uint8_t { EMPTY = 0, RECORD = 1, FORWARD = 2, TARGET = 3 };

struct VariableSlot {
    std::uint16_t offset;
    std::uint16_t length;
    SlotKind kind;
    RID link;
};

class VariablePage {
public:
    using Page = std::array<char, bufPageManager::PAGE_SIZE>;
    static constexpr std::size_t HEADER_BYTES = 32;
    static constexpr std::size_t SLOT_BYTES = 16;
    static constexpr std::uint16_t MAX_SLOTS = (bufPageManager::PAGE_SIZE - HEADER_BYTES) / SLOT_BYTES;
    explicit VariablePage(PageId id);
    explicit VariablePage(Page bytes);
    const Page& bytes() const noexcept { return bytes_; }
    static std::uint16_t slotCount(const Page& bytes);
    static VariableSlot slot(const Page& bytes, std::uint32_t index);
    VariableSlot slot(std::uint32_t index) const { return slot(bytes_, index); }
    std::uint16_t insertCapacity() const;
    bool canReplace(std::uint32_t slot, std::size_t size) const;
    std::uint32_t insert(const std::vector<char>& record, SlotKind kind, RID link);
    void replace(std::uint32_t slot, const std::vector<char>& record, SlotKind kind, RID link);
    void erase(std::uint32_t slot);
private:
    Page bytes_{};
    void validate() const;
    void rebuild(std::uint32_t changed, const std::vector<char>& record, SlotKind kind, RID link);
};

} // namespace db::detail
