#pragma once
#include "file_manage.h"
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

class bufPageManager {
public:
    static constexpr std::size_t PAGE_SIZE = 4096;
    explicit bufPageManager(FileManager& fm, std::size_t capacity = 64);
    ~bufPageManager();
    bufPageManager(const bufPageManager&) = delete;
    bufPageManager& operator=(const bufPageManager&) = delete;
    bool isOpen(FileId file_id) const noexcept;
    char* getPage(FileId file_id, PageId page_id); // 获取页
    void markDirty(FileId file_id, PageId page_id);// 标记脏页
    void flushPage(FileId file_id, PageId page_id);// 刷新页
    void discardFile(FileId file_id);              // 丢弃文件
    void flushAll();                           // 刷新所有页
private:
    struct PageKey {
        FileId file;
        PageId page;
        bool operator==(const PageKey& other) const noexcept { return file == other.file && page == other.page; }
    };
    struct PageKeyHash { std::size_t operator()(const PageKey& key) const noexcept; };
    struct Frame { std::unique_ptr<char[]> data; bool dirty = false; std::list<PageKey>::iterator lru; };
    FileManager& file_manager;
    std::size_t capacity_;
    std::list<PageKey> lru_;
    std::unordered_map<PageKey, Frame, PageKeyHash> pages_;
    void touch(Frame& frame, const PageKey& key);       // 更新 LRU
    void evictOne();                                    // 放逐
    void writeFrame(const PageKey& key, Frame& frame);  // 写回脏页
};
