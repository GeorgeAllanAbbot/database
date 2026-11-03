#pragma once

#include "schema.h"
#include "../file_manage/buf_page_manager.h"
#include <array>
#include <memory>
#include <unordered_map>

namespace db {

// RID 只在所属记录文件内有效；删除后的槽位可复用，旧 RID 随之失效。
struct RID {
    PageId page;
    std::uint32_t slot;
    bool operator==(const RID& other) const noexcept { return page == other.page && slot == other.slot; }
    bool operator!=(const RID& other) const noexcept { return !(*this == other); }
    bool operator<(const RID& other) const noexcept {
        return page < other.page || (page == other.page && slot < other.slot);
    }
};

struct RecordEntry {
    RID rid;
    Record values;
};

class RecordScan;
class RecordManager;
namespace detail { class VariablePage; }

class RecordFile : public std::enable_shared_from_this<RecordFile> {
public:
    ~RecordFile();
    RecordFile(const RecordFile&) = delete;
    RecordFile& operator=(const RecordFile&) = delete;

    const Schema& schema() const noexcept { return codec_.schema(); }
    std::uint64_t size() const noexcept { return record_count_; }
    std::uint32_t dataPageCount() const noexcept {
        return layout() == RecordLayout::VARIABLE ? variable_data_pages_ : page_count_ - first_data_page_;
    }
    std::uint16_t slotsPerPage() const noexcept { return slots_per_page_; } // 变长模式返回目录理论上限。
    std::size_t recordSize() const noexcept { return codec_.recordSize(); } // 变长模式返回编码最大长度。
    RecordLayout layout() const noexcept { return codec_.layout(); }
    bool isOpen() const noexcept { return open_; }

    RID insert(const Record& record);
    Record get(RID rid) const;
    void update(RID rid, const Record& record); // RID 不变；变长记录可通过转发槽搬迁。
    void remove(RID rid); // 不存在的记录抛出 RECORD_NOT_FOUND。
    bool contains(RID rid) const;
    RecordScan scan() const;
    void flush();
    void close(); // 写回并移除缓存后再关闭文件；可重复调用。

private:
    friend class RecordManager;
    friend class RecordScan;
    using Page = std::array<char, bufPageManager::PAGE_SIZE>;
    RecordFile(FileManager& files, bufPageManager& pages, FileId id, Schema schema, RecordLayout layout);
    FileManager& files_;
    bufPageManager& pages_;
    FileId file_id_;
    RecordCodec codec_;
    bool open_ = true;
    PageId first_data_page_ = 0;
    PageId page_count_ = 0;
    PageId first_free_page_ = 0;
    std::uint64_t record_count_ = 0;
    std::uint64_t revision_ = 0;
    std::uint16_t slots_per_page_ = 0;
    std::uint32_t schema_size_ = 0;
    std::uint32_t variable_data_pages_ = 0;

    void initialize(const std::vector<char>& schema_bytes);
    void loadHeader(const Page& header);
    void saveHeader();
    void requireOpen() const;
    void validateRID(RID rid) const;
    Page readPage(PageId page) const;
    Page readDataPage(PageId page) const;
    void writePage(PageId page, const Page& bytes);
    std::size_t slotOffset(std::uint16_t slot) const;
    bool isDataPage(PageId page) const noexcept;
    PageId variablePageId(std::uint32_t index) const;
    Page readDirectory(std::uint32_t group) const;
    void initializeDirectory();
    detail::VariablePage readVariablePage(PageId page) const;
    void writeVariablePage(PageId page, const detail::VariablePage& bytes);
    RID allocateVariable(const std::vector<char>& bytes, std::optional<RID> owner = std::nullopt,
                         const std::vector<PageId>& excluded = {});
    Record getVariable(RID rid) const;
    bool containsVariable(RID rid) const;
    void updateVariable(RID rid, const std::vector<char>& bytes);
    void removeVariable(RID rid);
    RID forwardedTarget(RID owner, const detail::VariablePage& root) const;
};

// 每次只保存一个数据页，内存不随表大小增长；记录修改会使已有扫描器失效。
class RecordScan {
public:
    std::optional<RecordEntry> next();
private:
    friend class RecordFile;
    explicit RecordScan(std::shared_ptr<const RecordFile> file);
    std::shared_ptr<const RecordFile> file_;
    std::uint64_t revision_;
    PageId page_;
    std::uint16_t slot_ = 0;
    bool loaded_ = false;
    RecordFile::Page bytes_{};
};

class RecordManager {
public:
    RecordManager(FileManager& files, bufPageManager& pages) : files_(files), pages_(pages) {}
    RecordManager(const RecordManager&) = delete;
    RecordManager& operator=(const RecordManager&) = delete;
    std::shared_ptr<RecordFile> createFile(const std::string& path, const Schema& schema,
                                         RecordLayout layout = RecordLayout::FIXED);
    std::shared_ptr<RecordFile> openFile(const std::string& path);
    void removeFile(const std::string& path); // 活跃的记录文件必须先 close()。
private:
    FileManager& files_;
    bufPageManager& pages_;
    std::unordered_map<std::string, std::weak_ptr<RecordFile>> open_files_;
};

} // namespace db
