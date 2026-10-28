#include "buf_page_manager.h"
#include <algorithm>
#include <limits>

// 构造与析构、哈希

bufPageManager::bufPageManager(FileManager& fm, std::size_t capacity)
    : file_manager(fm), capacity_(capacity) {
    if (!capacity) throw FileException(FileError::FILE_INVALID_ARGUMENT, "buffer capacity must be positive");
}
bufPageManager::~bufPageManager() { try { flushAll(); } catch (...) {} }

std::size_t bufPageManager::PageKeyHash::operator()(const PageKey& key) const noexcept {
    const auto a = std::hash<FileId>{}(key.file);
    const auto b = std::hash<PageId>{}(key.page);
    return a ^ (b + 0x9e3779b9U + (a << 6U) + (a >> 2U));
}

// private区域
void bufPageManager::touch(Frame& frame, const PageKey& key) {
    lru_.erase(frame.lru);
    lru_.push_front(key);
    frame.lru = lru_.begin();
}
void bufPageManager::writeFrame(const PageKey& key, Frame& frame) {
    if (!frame.dirty) return;
    if (key.page > std::numeric_limits<std::uint64_t>::max() / PAGE_SIZE)
        throw FileException(FileError::FILE_INVALID_PAGE_ID, "page offset overflow");
    file_manager.write(key.file, key.page * PAGE_SIZE, frame.data.get(), PAGE_SIZE);
    frame.dirty = false;
}
void bufPageManager::evictOne() {
    const PageKey key = lru_.back();
    auto it = pages_.find(key);
    writeFrame(key, it->second);
    lru_.pop_back();
    pages_.erase(it);
}

// public区域

bool bufPageManager::isOpen(FileId file_id) const noexcept {
    return file_manager.isOpen(file_id);
}

char* bufPageManager::getPage(FileId file_id, PageId page_id) {
    if (!file_manager.isOpen(file_id)) throw FileException(FileError::FILE_INVALID_ID, "invalid file id");
    const PageKey key{file_id, page_id};
    auto old = pages_.find(key);
    if (old != pages_.end()) { touch(old->second, key); return old->second.data.get(); }
    if (pages_.size() == capacity_) evictOne();
    Frame frame;
    frame.data = std::make_unique<char[]>(PAGE_SIZE);
    std::fill_n(frame.data.get(), PAGE_SIZE, '\0');
    if (key.page > std::numeric_limits<std::uint64_t>::max() / PAGE_SIZE)
        throw FileException(FileError::FILE_INVALID_PAGE_ID, "page offset overflow");
    const std::uint64_t offset = key.page * PAGE_SIZE;
    const std::uint64_t size = file_manager.fileSize(file_id);
    if (offset < size) {
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(PAGE_SIZE, size - offset));
        file_manager.read(file_id, offset, frame.data.get(), available);
    }
    lru_.push_front(key); frame.lru = lru_.begin();
    auto result = pages_.emplace(key, std::move(frame));
    return result.first->second.data.get();
}

void bufPageManager::markDirty(FileId file_id, PageId page_id) {
    const PageKey key{file_id, page_id};
    auto it = pages_.find(key);
    if (it == pages_.end()) throw FileException(FileError::FILE_PAGE_NOT_FOUND, "page is not cached");
    it->second.dirty = true; touch(it->second, key);
}
void bufPageManager::flushPage(FileId file_id, PageId page_id) {
    const PageKey key{file_id, page_id};
    auto it = pages_.find(key);
    if (it == pages_.end()) throw FileException(FileError::FILE_PAGE_NOT_FOUND, "page is not cached");
    writeFrame(key, it->second); file_manager.flush(file_id);
}
void bufPageManager::discardFile(FileId file_id) {
    for (auto it = pages_.begin(); it != pages_.end();) {
        if (it->first.file != file_id) { ++it; continue; }
        writeFrame(it->first, it->second); lru_.erase(it->second.lru); it = pages_.erase(it);
    }
    if (file_manager.isOpen(file_id)) file_manager.flush(file_id);
}
void bufPageManager::flushAll() {
    std::unordered_map<FileId, bool> flushed;
    for (auto& item : pages_) writeFrame(item.first, item.second);
    for (const auto& item : pages_)
        if (flushed.emplace(item.first.file, true).second && file_manager.isOpen(item.first.file))
            file_manager.flush(item.first.file);
}
