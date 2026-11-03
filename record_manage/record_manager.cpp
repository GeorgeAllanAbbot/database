#include "record_manager.h"
#include "variable_page.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>

namespace db {
namespace {

constexpr PageId NO_PAGE = std::numeric_limits<PageId>::max();
constexpr std::uint64_t FILE_MAGIC = 0x31454c4946434552ULL; // "RECFILE1"
constexpr std::uint32_t FORMAT_VERSION = 1;
constexpr std::uint32_t DATA_MAGIC = 0x31475052U;
constexpr std::size_t PAGE_BYTES = bufPageManager::PAGE_SIZE;
constexpr std::size_t DATA_HEADER_BYTES = 16;
constexpr std::size_t MAX_SCHEMA_BYTES = 1024 * 1024;
using Page = std::array<char, PAGE_BYTES>;

[[noreturn]] void corrupt(const std::string& message) {
    throw RecordException(RecordError::CORRUPT_FILE, message);
}

std::uint64_t number(const char* source, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value |= std::uint64_t(static_cast<unsigned char>(source[i])) << (8 * i);
    return value;
}

void put(char* target, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) target[i] = static_cast<char>((value >> (8 * i)) & 255);
}

struct Writer {
    std::vector<char> bytes;
    void integer(std::uint64_t value, std::size_t size) {
        const auto offset = bytes.size();
        bytes.resize(offset + size);
        put(bytes.data() + offset, value, size);
    }
    void string(const std::string& value) {
        if (value.size() > MAX_SCHEMA_BYTES) throw RecordException(RecordError::INVALID_SCHEMA, "schema string too long");
        integer(value.size(), 4);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void strings(const std::vector<std::string>& values) {
        integer(values.size(), 4);
        for (const auto& value : values) string(value);
    }
    void value(const Value& value) {
        integer(value.index(), 1);
        if (const auto* v = std::get_if<std::int32_t>(&value)) {
            integer(static_cast<std::uint32_t>(*v), 4);
        } else if (const auto* v = std::get_if<double>(&value)) {
            std::uint64_t bits;
            std::memcpy(&bits, v, 8);
            integer(bits, 8);
        } else if (const auto* v = std::get_if<std::string>(&value)) {
            string(*v);
        }
    }
};

struct Reader {
    const std::vector<char>& bytes;
    std::size_t offset = 0;
    std::size_t remaining() const { return bytes.size() - offset; }
    std::uint64_t integer(std::size_t size) {
        if (size > remaining()) corrupt("truncated schema");
        const auto value = number(bytes.data() + offset, size);
        offset += size;
        return value;
    }
    bool boolean() {
        const auto value = integer(1);
        if (value > 1) corrupt("invalid schema boolean");
        return value != 0;
    }
    std::size_t count() {
        const auto count = integer(4);
        if (count > remaining() / 4) corrupt("invalid schema item count");
        return static_cast<std::size_t>(count);
    }
    std::string string() {
        const auto size = integer(4);
        if (size > remaining()) corrupt("truncated schema string");
        std::string value(bytes.data() + offset, static_cast<std::size_t>(size));
        offset += size;
        return value;
    }
    std::vector<std::string> strings() {
        const auto n = count();
        std::vector<std::string> result;
        for (std::size_t i = 0; i < n; ++i) result.push_back(string());
        return result;
    }
    Value value() {
        switch (integer(1)) {
        case 0: return std::monostate{};
        case 1: {
            const auto bits = static_cast<std::uint32_t>(integer(4));
            std::int32_t result;
            std::memcpy(&result, &bits, 4);
            return result;
        }
        case 2: {
            const auto bits = integer(8);
            double result;
            std::memcpy(&result, &bits, 8);
            return result;
        }
        case 3: return string();
        default: corrupt("invalid schema value tag");
        }
    }
};

std::vector<char> encodeSchema(const Schema& schema) {
    schema.validate();
    Writer w;
    w.string(schema.table_name);
    w.integer(schema.columns.size(), 4);
    for (const auto& column : schema.columns) {
        w.string(column.name);
        w.integer(static_cast<std::uint8_t>(column.type), 1);
        w.integer(column.max_length, 4);
        w.integer(column.nullable, 1);
        w.integer(column.default_value.has_value(), 1);
        if (column.default_value) w.value(*column.default_value);
    }
    w.integer(schema.primary_key.has_value(), 1);
    if (schema.primary_key) {
        w.string(schema.primary_key->name);
        w.strings(schema.primary_key->columns);
    }
    w.integer(schema.foreign_keys.size(), 4);
    for (const auto& foreign : schema.foreign_keys) {
        w.string(foreign.name);
        w.strings(foreign.columns);
        w.string(foreign.referenced_table);
        w.strings(foreign.referenced_columns);
    }
    w.integer(schema.unique_constraints.size(), 4);
    for (const auto& unique : schema.unique_constraints) {
        w.string(unique.name);
        w.strings(unique.columns);
    }
    if (w.bytes.size() > MAX_SCHEMA_BYTES)
        throw RecordException(RecordError::INVALID_SCHEMA, "schema metadata exceeds 1 MiB");
    return std::move(w.bytes);
}

Schema decodeSchema(const std::vector<char>& bytes) {
    Reader r{bytes};
    Schema schema;
    schema.table_name = r.string();
    const auto count = r.count();
    if (count == 0 || count > 10) corrupt("invalid schema column count");
    for (std::size_t i = 0; i < count; ++i) {
        Column column;
        column.name = r.string();
        column.type = static_cast<DataType>(r.integer(1));
        column.max_length = static_cast<std::uint32_t>(r.integer(4));
        column.nullable = r.boolean();
        if (r.boolean()) column.default_value = r.value();
        schema.columns.push_back(std::move(column));
    }
    if (r.boolean()) {
        PrimaryKey key;
        key.name = r.string();
        key.columns = r.strings();
        schema.primary_key = std::move(key);
    }
    const auto foreign_count = r.count();
    for (std::size_t i = 0; i < foreign_count; ++i) {
        ForeignKey foreign;
        foreign.name = r.string();
        foreign.columns = r.strings();
        foreign.referenced_table = r.string();
        foreign.referenced_columns = r.strings();
        schema.foreign_keys.push_back(std::move(foreign));
    }
    const auto unique_count = r.count();
    for (std::size_t i = 0; i < unique_count; ++i) {
        UniqueConstraint unique;
        unique.name = r.string();
        unique.columns = r.strings();
        schema.unique_constraints.push_back(std::move(unique));
    }
    if (r.remaining()) corrupt("unexpected schema trailing bytes");
    try { schema.validate(); }
    catch (const RecordException& e) { corrupt(std::string("invalid stored schema: ") + e.what()); }
    return schema;
}

std::uint16_t slotCapacity(std::size_t record_size) {
    std::size_t slots = (PAGE_BYTES - DATA_HEADER_BYTES) / record_size;
    while (DATA_HEADER_BYTES + (slots + 7) / 8 + slots * record_size > PAGE_BYTES) --slots;
    if (!slots) throw RecordException(RecordError::INVALID_SCHEMA, "record does not fit a page");
    return static_cast<std::uint16_t>(slots);
}

bool occupied(const Page& page, std::uint16_t slot) {
    return (static_cast<unsigned char>(page[DATA_HEADER_BYTES + slot / 8]) & (1U << (slot % 8))) != 0;
}

void setOccupied(Page& page, std::uint16_t slot, bool used) {
    auto value = static_cast<unsigned char>(page[DATA_HEADER_BYTES + slot / 8]);
    value = static_cast<unsigned char>(used ? value | (1U << (slot % 8)) : value & ~(1U << (slot % 8)));
    page[DATA_HEADER_BYTES + slot / 8] = static_cast<char>(value);
}

std::string normalizePath(const std::string& path) {
    if (path.empty()) throw FileException(FileError::FILE_INVALID_ARGUMENT, "filename must not be empty");
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path)).string();
}

void ensureNotOpen(FileManager& files, const std::string& path) {
    try { files.idFor(path); }
    catch (const FileException& e) {
        if (e.code() == FileError::FILE_NOT_FOUND) return;
        throw;
    }
    throw RecordException(RecordError::FILE_IN_USE, "file is already open outside this record manager");
}

void closeAfterFailure(FileManager& files, bufPageManager& pages, FileId id) noexcept {
    try { pages.discardFile(id); } catch (...) {}
    try { if (files.isOpen(id)) files.closeFile(id); } catch (...) {}
}

void validateFileHeader(const Page& header, std::uint64_t file_size) {
    const auto version = number(header.data() + 8, 4);
    const auto layout = number(header.data() + 48, 4);
    if (number(header.data(), 8) != FILE_MAGIC ||
        !((version == FORMAT_VERSION && layout == 0) || (version == 2 && layout == 1)) ||
        number(header.data() + 12, 4) != PAGE_BYTES)
        corrupt("invalid or unsupported record file header");
    const auto schema_size = number(header.data() + 16, 4);
    const auto schema_pages = number(header.data() + 20, 4);
    const auto page_count = number(header.data() + 32, 4);
    if (schema_size == 0 || schema_size > MAX_SCHEMA_BYTES ||
        schema_pages != (schema_size + PAGE_BYTES - 1) / PAGE_BYTES ||
        page_count < 1 + schema_pages || page_count == NO_PAGE ||
        file_size != page_count * PAGE_BYTES)
        corrupt("invalid record file size or metadata page count");
}

} // namespace

RecordFile::RecordFile(FileManager& files, bufPageManager& pages, FileId id, Schema schema, RecordLayout layout)
    : files_(files), pages_(pages), file_id_(id), codec_(std::move(schema), layout),
      slots_per_page_(layout == RecordLayout::VARIABLE ? detail::VariablePage::MAX_SLOTS : slotCapacity(codec_.recordSize())) {}

RecordFile::~RecordFile() { try { close(); } catch (...) {} }

void RecordFile::requireOpen() const {
    if (!open_ || !files_.isOpen(file_id_))
        throw RecordException(RecordError::FILE_CLOSED, "record file is closed");
}

RecordFile::Page RecordFile::readPage(PageId page) const {
    requireOpen();
    Page bytes{};
    std::memcpy(bytes.data(), pages_.getPage(file_id_, page), bytes.size());
    return bytes;
}

void RecordFile::writePage(PageId page, const Page& bytes) {
    requireOpen();
    std::memcpy(pages_.getPage(file_id_, page), bytes.data(), bytes.size());
    pages_.markDirty(file_id_, page);
}

void RecordFile::initialize(const std::vector<char>& schema_bytes) {
    schema_size_ = static_cast<std::uint32_t>(schema_bytes.size());
    first_data_page_ = 1 + (schema_size_ + PAGE_BYTES - 1) / PAGE_BYTES;
    const auto schema_end = first_data_page_;
    if (layout() == RecordLayout::VARIABLE) ++first_data_page_;
    page_count_ = first_data_page_;
    first_free_page_ = NO_PAGE;
    for (PageId id = 1; id < schema_end; ++id) {
        Page page{};
        const std::size_t offset = (id - 1) * PAGE_BYTES;
        const auto size = std::min(PAGE_BYTES, schema_bytes.size() - offset);
        std::memcpy(page.data(), schema_bytes.data() + offset, size);
        writePage(id, page);
    }
    if (layout() == RecordLayout::VARIABLE) initializeDirectory();
    saveHeader();
}

void RecordFile::loadHeader(const Page& header) {
    schema_size_ = static_cast<std::uint32_t>(number(header.data() + 16, 4));
    first_data_page_ = 1 + static_cast<PageId>(number(header.data() + 20, 4));
    if (layout() == RecordLayout::VARIABLE) ++first_data_page_;
    if (number(header.data() + 24, 4) != codec_.recordSize() ||
        number(header.data() + 28, 4) != slots_per_page_)
        corrupt("record layout does not match schema");
    page_count_ = static_cast<PageId>(number(header.data() + 32, 4));
    first_free_page_ = static_cast<PageId>(number(header.data() + 36, 4));
    record_count_ = number(header.data() + 40, 8);
    if (layout() == RecordLayout::VARIABLE) {
        variable_data_pages_ = static_cast<std::uint32_t>(number(header.data() + 52, 4));
        const auto directories = std::max<std::uint64_t>(1,
            (std::uint64_t(variable_data_pages_) + detail::DIRECTORY_ENTRIES - 1) / detail::DIRECTORY_ENTRIES);
        if (first_free_page_ != NO_PAGE ||
            page_count_ != std::uint64_t(first_data_page_ - 1) + directories + variable_data_pages_ ||
            record_count_ > std::uint64_t(variable_data_pages_) * slots_per_page_)
            corrupt("invalid variable page or record count");
        readDirectory(0);
        return;
    }
    if ((first_free_page_ != NO_PAGE &&
         (first_free_page_ < first_data_page_ || first_free_page_ >= page_count_)) ||
        record_count_ > std::uint64_t(page_count_ - first_data_page_) * slots_per_page_)
        corrupt("invalid free page or record count");
}

void RecordFile::saveHeader() {
    Page page{};
    put(page.data(), FILE_MAGIC, 8);
    put(page.data() + 8, layout() == RecordLayout::VARIABLE ? 2 : FORMAT_VERSION, 4);
    put(page.data() + 12, PAGE_BYTES, 4);
    put(page.data() + 16, schema_size_, 4);
    put(page.data() + 20, first_data_page_ - 1 - (layout() == RecordLayout::VARIABLE ? 1 : 0), 4);
    put(page.data() + 24, codec_.recordSize(), 4);
    put(page.data() + 28, slots_per_page_, 4);
    put(page.data() + 32, page_count_, 4);
    put(page.data() + 36, first_free_page_, 4);
    put(page.data() + 40, record_count_, 8);
    put(page.data() + 48, static_cast<std::uint32_t>(layout()), 4);
    put(page.data() + 52, variable_data_pages_, 4);
    writePage(0, page);
}

RecordFile::Page RecordFile::readDataPage(PageId id) const {
    if (id < first_data_page_ || id >= page_count_) corrupt("invalid data page number");
    Page page = readPage(id);
    const auto count = number(page.data() + 8, 2);
    const auto next = number(page.data() + 12, 4);
    if (number(page.data(), 4) != DATA_MAGIC || number(page.data() + 4, 4) != id ||
        number(page.data() + 10, 2) != slots_per_page_ || count > slots_per_page_ ||
        (next != NO_PAGE && (next < first_data_page_ || next >= page_count_ || next == id)) ||
        (count == slots_per_page_ && next != NO_PAGE))
        corrupt("invalid record data page header");
    std::size_t used = 0;
    for (std::uint16_t slot = 0; slot < slots_per_page_; ++slot) used += occupied(page, slot);
    if (used != count) corrupt("slot bitmap does not match data page count");
    return page;
}

std::size_t RecordFile::slotOffset(std::uint16_t slot) const {
    return DATA_HEADER_BYTES + (slots_per_page_ + 7) / 8 + slot * codec_.recordSize();
}

void RecordFile::validateRID(RID rid) const {
    requireOpen();
    if (!isDataPage(rid.page) || rid.slot >= slots_per_page_)
        throw RecordException(RecordError::INVALID_RID, "RID is outside the data pages or slot range");
}

RID RecordFile::insert(const Record& record) {
    requireOpen();
    const auto bytes = codec_.encode(record); // 先校验，拒绝非法记录时不修改文件。
    if (layout() == RecordLayout::VARIABLE) {
        const auto rid = allocateVariable(bytes);
        ++record_count_;
        ++revision_;
        saveHeader();
        return rid;
    }
    Page page{};
    PageId id = first_free_page_;
    if (id == NO_PAGE) {
        if (page_count_ >= NO_PAGE - 1)
            throw RecordException(RecordError::FILE_TOO_LARGE, "record file has exhausted page identifiers");
        id = page_count_;
        put(page.data(), DATA_MAGIC, 4);
        put(page.data() + 4, id, 4);
        put(page.data() + 10, slots_per_page_, 2);
        put(page.data() + 12, NO_PAGE, 4);
    } else {
        page = readDataPage(id);
    }
    const auto count = number(page.data() + 8, 2);
    if (count >= slots_per_page_) corrupt("free page has no empty slot");
    std::uint16_t slot = 0;
    while (slot < slots_per_page_ && occupied(page, slot)) ++slot;
    if (slot == slots_per_page_) corrupt("free page bitmap has no empty slot");
    std::memcpy(page.data() + slotOffset(slot), bytes.data(), bytes.size());
    setOccupied(page, slot, true);
    put(page.data() + 8, count + 1, 2);
    const auto next = static_cast<PageId>(number(page.data() + 12, 4));
    if (count + 1 == slots_per_page_) put(page.data() + 12, NO_PAGE, 4);
    writePage(id, page);
    if (id == page_count_) ++page_count_;
    first_free_page_ = count + 1 == slots_per_page_ ? next : id;
    ++record_count_;
    ++revision_;
    saveHeader();
    return RID{id, slot};
}

Record RecordFile::get(RID rid) const {
    validateRID(rid);
    if (layout() == RecordLayout::VARIABLE) return getVariable(rid);
    const auto page = readDataPage(rid.page);
    if (!occupied(page, rid.slot))
        throw RecordException(RecordError::RECORD_NOT_FOUND, "record slot is empty");
    return codec_.decode(page.data() + slotOffset(rid.slot), codec_.recordSize());
}

bool RecordFile::contains(RID rid) const {
    requireOpen();
    if (!isDataPage(rid.page) || rid.slot >= slots_per_page_) return false;
    if (layout() == RecordLayout::VARIABLE) return containsVariable(rid);
    return occupied(readDataPage(rid.page), rid.slot);
}

void RecordFile::update(RID rid, const Record& record) {
    validateRID(rid);
    const auto bytes = codec_.encode(record);
    if (layout() == RecordLayout::VARIABLE) {
        updateVariable(rid, bytes);
        ++revision_;
        saveHeader();
        return;
    }
    auto page = readDataPage(rid.page);
    if (!occupied(page, rid.slot))
        throw RecordException(RecordError::RECORD_NOT_FOUND, "record slot is empty");
    std::memcpy(page.data() + slotOffset(rid.slot), bytes.data(), bytes.size());
    writePage(rid.page, page);
    ++revision_;
}

void RecordFile::remove(RID rid) {
    validateRID(rid);
    if (layout() == RecordLayout::VARIABLE) {
        removeVariable(rid);
        --record_count_;
        ++revision_;
        saveHeader();
        return;
    }
    auto page = readDataPage(rid.page);
    if (!occupied(page, rid.slot))
        throw RecordException(RecordError::RECORD_NOT_FOUND, "record slot is empty");
    if (record_count_ == 0) corrupt("record count is zero for an occupied slot");
    const auto count = number(page.data() + 8, 2);
    setOccupied(page, rid.slot, false);
    std::fill_n(page.data() + slotOffset(rid.slot), codec_.recordSize(), '\0');
    put(page.data() + 8, count - 1, 2);
    if (count == slots_per_page_) put(page.data() + 12, first_free_page_, 4);
    writePage(rid.page, page);
    if (count == slots_per_page_) first_free_page_ = rid.page;
    --record_count_;
    ++revision_;
    saveHeader();
}

RecordScan RecordFile::scan() const { requireOpen(); return RecordScan(shared_from_this()); }

void RecordFile::flush() { requireOpen(); pages_.discardFile(file_id_); }

void RecordFile::close() {
    if (!open_) return;
    flush();
    files_.closeFile(file_id_);
    open_ = false;
}

RecordScan::RecordScan(std::shared_ptr<const RecordFile> file)
    : file_(std::move(file)), revision_(file_->revision_), page_(file_->first_data_page_) {}

std::optional<RecordEntry> RecordScan::next() {
    file_->requireOpen();
    if (revision_ != file_->revision_)
        throw RecordException(RecordError::SCAN_INVALIDATED, "record file changed during scan");
    while (page_ < file_->page_count_) {
        if (!file_->isDataPage(page_)) { ++page_; continue; }
        if (file_->layout() == RecordLayout::VARIABLE) {
            if (!loaded_) { bytes_ = file_->readVariablePage(page_).bytes(); loaded_ = true; }
            while (slot_ < detail::VariablePage::slotCount(bytes_)) {
                const auto index = slot_++;
                const auto slot = detail::VariablePage::slot(bytes_, index);
                if (slot.kind == detail::SlotKind::RECORD)
                    return RecordEntry{RID{page_, index}, file_->codec_.decode(bytes_.data() + slot.offset, slot.length)};
                if (slot.kind == detail::SlotKind::FORWARD)
                    return RecordEntry{RID{page_, index}, file_->getVariable(RID{page_, index})};
            }
            ++page_; slot_ = 0; loaded_ = false;
            continue;
        }
        if (!loaded_) { bytes_ = file_->readDataPage(page_); loaded_ = true; }
        while (slot_ < file_->slots_per_page_) {
            const auto slot = slot_++;
            if (occupied(bytes_, slot))
                return RecordEntry{RID{page_, slot}, file_->codec_.decode(
                    bytes_.data() + file_->slotOffset(slot), file_->codec_.recordSize())};
        }
        ++page_;
        slot_ = 0;
        loaded_ = false;
    }
    return std::nullopt;
}

std::shared_ptr<RecordFile> RecordManager::createFile(const std::string& path, const Schema& schema, RecordLayout layout) {
    const auto bytes = encodeSchema(schema);
    RecordCodec checked(schema, layout);
    const auto normalized = normalizePath(path);
    // createFile 自身拒绝已存在的文件，绝不覆盖。
    const auto id = files_.createFile(normalized);
    try {
        auto file = std::shared_ptr<RecordFile>(new RecordFile(files_, pages_, id, schema, layout));
        file->initialize(bytes);
        open_files_[normalized] = file;
        return file;
    } catch (...) {
        closeAfterFailure(files_, pages_, id);
        try { files_.removeFile(normalized); } catch (...) {}
        throw;
    }
}

std::shared_ptr<RecordFile> RecordManager::openFile(const std::string& path) {
    const auto normalized = normalizePath(path);
    auto old = open_files_.find(normalized);
    if (old != open_files_.end()) {
        if (auto file = old->second.lock(); file && file->isOpen()) return file;
    }
    ensureNotOpen(files_, normalized);
    const auto id = files_.openFile(normalized);
    try {
        const auto size = files_.fileSize(id);
        if (size < PAGE_BYTES || size % PAGE_BYTES) corrupt("record file is empty or truncated");
        Page header{};
        std::memcpy(header.data(), pages_.getPage(id, 0), PAGE_BYTES);
        validateFileHeader(header, size);
        const auto schema_size = static_cast<std::size_t>(number(header.data() + 16, 4));
        const auto schema_pages = static_cast<PageId>(number(header.data() + 20, 4));
        std::vector<char> bytes(schema_size);
        for (PageId page = 1; page <= schema_pages; ++page) {
            const auto offset = (page - 1) * PAGE_BYTES;
            const auto n = std::min(PAGE_BYTES, bytes.size() - offset);
            std::memcpy(bytes.data() + offset, pages_.getPage(id, page), n);
        }
        const auto layout = static_cast<RecordLayout>(number(header.data() + 48, 4));
        auto file = std::shared_ptr<RecordFile>(new RecordFile(files_, pages_, id, decodeSchema(bytes), layout));
        file->loadHeader(header);
        open_files_[normalized] = file;
        return file;
    } catch (...) {
        closeAfterFailure(files_, pages_, id);
        throw;
    }
}

void RecordManager::removeFile(const std::string& path) {
    const auto normalized = normalizePath(path);
    ensureNotOpen(files_, normalized);
    files_.removeFile(normalized);
    open_files_.erase(normalized);
}

} // namespace db
