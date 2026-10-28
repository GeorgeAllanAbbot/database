#pragma once

#include "../file_manage/buf_page_manager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// 当前版本要求 SortKey、Value 是固定宽度且可平凡复制的类型。
template <typename SortKey, typename Value>
class DiskBPlusTree {
private:
    using Page = std::array<char, bufPageManager::PAGE_SIZE>;

    static constexpr PageId INVALID_PAGE = std::numeric_limits<PageId>::max();
    static constexpr std::uint64_t META_MAGIC = 0x4250545245453031ULL; // "BPTREE01"
    static constexpr std::uint32_t FORMAT_VERSION = 1;

    struct Metadata {
        std::uint64_t magic;
        std::uint32_t version;
        PageId root_page;
        PageId next_page;
        std::uint64_t size;
    };

    struct NodeHeader {
        std::uint8_t is_leaf;
        std::uint16_t count;
        PageId next_leaf;
    };

    struct LeafEntry {
        SortKey key;
        Value value;
    };

    struct Split {
        bool happened = false;
        SortKey separator{};
        PageId right_page = INVALID_PAGE;
    };

    static constexpr std::size_t LEAF_CAPACITY =
        (bufPageManager::PAGE_SIZE - sizeof(NodeHeader)) / sizeof(LeafEntry);
    static constexpr std::size_t INTERNAL_CAPACITY =
        (bufPageManager::PAGE_SIZE - sizeof(NodeHeader) - sizeof(PageId)) /
        (sizeof(SortKey) + sizeof(PageId)) + 1;

    bufPageManager& pages_;
    FileId file_id_;
    Metadata metadata_{};

    // 读取指定页的完整内容到局部缓冲，避免跨页访问期间持有缓存指针。
    Page readPage(PageId page_id) const {
        Page page{};
        char* source = pages_.getPage(file_id_, page_id);
        std::memcpy(page.data(), source, page.size());
        return page;
    }

    // 将完整页写入缓冲池并标记为脏页。
    void writePage(PageId page_id, const Page& page) {
        char* destination = pages_.getPage(file_id_, page_id);
        std::memcpy(destination, page.data(), page.size());
        pages_.markDirty(file_id_, page_id);
    }

    // 从零号元数据页读取树状态；新文件会初始化元数据。
    void openOrInitialize() {
        Page page = readPage(0);
        Metadata stored{};
        std::memcpy(&stored, page.data(), sizeof(stored));
        if (stored.magic == 0) {
            metadata_ = Metadata{META_MAGIC, FORMAT_VERSION, INVALID_PAGE, 1, 0};
            saveMetadata();
            return;
        }
        if (stored.magic != META_MAGIC || stored.version != FORMAT_VERSION || stored.next_page == 0) {
            throw std::runtime_error("invalid or unsupported B+ tree metadata");
        }
        metadata_ = stored;
    }

    // 将当前树元数据写回零号页。
    void saveMetadata() {
        Page page{};
        std::memcpy(page.data(), &metadata_, sizeof(metadata_));
        writePage(0, page);
    }

    // 申请一个新的页号；当前版本采用递增分配，不回收合并后空出的页。
    PageId allocatePage() {
        if (metadata_.next_page == INVALID_PAGE) {
            throw FileException(FileError::FILE_INVALID_PAGE_ID, "B+ tree has exhausted page ids");
        }
        return metadata_.next_page++;
    }

    // 从叶页解码所有键值项。
    std::vector<LeafEntry> readLeaf(const Page& page, NodeHeader& header) const {
        std::memcpy(&header, page.data(), sizeof(header));
        if (!header.is_leaf || header.count > LEAF_CAPACITY) {
            throw std::runtime_error("invalid B+ tree leaf page");
        }
        std::vector<LeafEntry> entries(header.count);
        if (!entries.empty()) {
            std::memcpy(entries.data(), page.data() + sizeof(NodeHeader),
                        entries.size() * sizeof(LeafEntry));
        }
        return entries;
    }

    // 将叶节点编码到固定大小页中。
    Page makeLeaf(const std::vector<LeafEntry>& entries, PageId next_leaf) const {
        if (entries.size() > LEAF_CAPACITY) throw std::logic_error("leaf page overflow");
        Page page{};
        NodeHeader header{1, static_cast<std::uint16_t>(entries.size()), next_leaf};
        std::memcpy(page.data(), &header, sizeof(header));
        if (!entries.empty()) {
            std::memcpy(page.data() + sizeof(NodeHeader), entries.data(),
                        entries.size() * sizeof(LeafEntry));
        }
        return page;
    }

    // 从内部页解码分隔键和子页号。
    void readInternal(const Page& page, NodeHeader& header,
                      std::vector<SortKey>& separators,
                      std::vector<PageId>& children) const {
        std::memcpy(&header, page.data(), sizeof(header));
        if (header.is_leaf || header.count > INTERNAL_CAPACITY - 1) {
            throw std::runtime_error("invalid B+ tree internal page");
        }
        const char* cursor = page.data() + sizeof(NodeHeader);
        children.resize(static_cast<std::size_t>(header.count) + 1);
        separators.resize(header.count);
        std::memcpy(children.data(), cursor, children.size() * sizeof(PageId));
        cursor += children.size() * sizeof(PageId);
        if (!separators.empty()) {
            std::memcpy(separators.data(), cursor, separators.size() * sizeof(SortKey));
        }
    }

    // 将内部节点的分隔键和子页号编码到固定大小页中。
    Page makeInternal(const std::vector<SortKey>& separators,
                      const std::vector<PageId>& children) const {
        if (children.empty() || children.size() > INTERNAL_CAPACITY ||
            separators.size() + 1 != children.size()) {
            throw std::logic_error("invalid B+ tree internal node size");
        }
        Page page{};
        NodeHeader header{0, static_cast<std::uint16_t>(separators.size()), INVALID_PAGE};
        std::memcpy(page.data(), &header, sizeof(header));
        char* cursor = page.data() + sizeof(NodeHeader);
        std::memcpy(cursor, children.data(), children.size() * sizeof(PageId));
        cursor += children.size() * sizeof(PageId);
        if (!separators.empty()) {
            std::memcpy(cursor, separators.data(), separators.size() * sizeof(SortKey));
        }
        return page;
    }

    // 按 key 递归插入；节点溢出时返回新右页及其最小键。
    Split insertRecursive(PageId page_id, const SortKey& key, const Value& value) {
        Page page = readPage(page_id);
        NodeHeader header{};
        std::memcpy(&header, page.data(), sizeof(header));

        if (header.is_leaf) {
            std::vector<LeafEntry> entries = readLeaf(page, header);
            auto position = std::upper_bound(entries.begin(), entries.end(), key,
                [](const SortKey& sought, const LeafEntry& entry) { return sought < entry.key; });
            entries.insert(position, LeafEntry{key, value});

            if (entries.size() <= LEAF_CAPACITY) {
                writePage(page_id, makeLeaf(entries, header.next_leaf));
                return {};
            }

            const std::size_t midpoint = entries.size() / 2;
            std::vector<LeafEntry> right_entries(entries.begin() + midpoint, entries.end());
            entries.erase(entries.begin() + midpoint, entries.end());
            const PageId right_page = allocatePage();
            writePage(page_id, makeLeaf(entries, right_page));
            writePage(right_page, makeLeaf(right_entries, header.next_leaf));
            return Split{true, right_entries.front().key, right_page};
        }

        std::vector<SortKey> separators;
        std::vector<PageId> children;
        readInternal(page, header, separators, children);
        const std::size_t child_index = static_cast<std::size_t>(
            std::lower_bound(separators.begin(), separators.end(), key) - separators.begin());
        Split child_split = insertRecursive(children[child_index], key, value);
        if (!child_split.happened) return {};

        separators.insert(separators.begin() + child_index, child_split.separator);
        children.insert(children.begin() + child_index + 1, child_split.right_page);
        if (children.size() <= INTERNAL_CAPACITY) {
            writePage(page_id, makeInternal(separators, children));
            return {};
        }

        const std::size_t midpoint = children.size() / 2;
        const SortKey promoted = separators[midpoint - 1];
        std::vector<PageId> right_children(children.begin() + midpoint, children.end());
        std::vector<SortKey> right_separators(separators.begin() + midpoint, separators.end());
        children.erase(children.begin() + midpoint, children.end());
        separators.erase(separators.begin() + midpoint - 1, separators.end());
        const PageId right_page = allocatePage();
        writePage(page_id, makeInternal(separators, children));
        writePage(right_page, makeInternal(right_separators, right_children));
        return Split{true, promoted, right_page};
    }

    struct ParentStep {
        PageId page_id;
        std::size_t child_index;
    };

    static bool sameKey(const SortKey& left, const SortKey& right) {
        return !(left < right) && !(right < left);
    }

    // 返回子树最左叶节点的首键。内部节点的分隔键始终是右子树的首键。
    SortKey firstKey(PageId page_id) const {
        for (;;) {
            const Page page = readPage(page_id);
            NodeHeader header{};
            std::memcpy(&header, page.data(), sizeof(header));
            if (header.is_leaf) {
                const std::vector<LeafEntry> entries = readLeaf(page, header);
                if (entries.empty()) throw std::runtime_error("empty subtree has no separator key");
                return entries.front().key;
            }
            std::vector<SortKey> separators;
            std::vector<PageId> children;
            readInternal(page, header, separators, children);
            page_id = children.front();
        }
    }

    void writeInternalFromChildren(PageId page_id, const std::vector<PageId>& children) {
        std::vector<SortKey> separators;
        separators.reserve(children.size() - 1);
        for (std::size_t i = 1; i < children.size(); ++i) {
            separators.push_back(firstKey(children[i]));
        }
        writePage(page_id, makeInternal(separators, children));
    }

    bool findLeafPath(PageId current, PageId target,
                      std::vector<ParentStep>& path) const {
        const Page page = readPage(current);
        NodeHeader header{};
        std::memcpy(&header, page.data(), sizeof(header));
        if (header.is_leaf) return current == target;

        std::vector<SortKey> separators;
        std::vector<PageId> children;
        readInternal(page, header, separators, children);
        for (std::size_t i = 0; i < children.size(); ++i) {
            path.push_back(ParentStep{current, i});
            if (findLeafPath(children[i], target, path)) return true;
            path.pop_back();
        }
        return false;
    }

    bool isUnderfull(const NodeHeader& header) const {
        if (header.is_leaf) return header.count < LEAF_CAPACITY / 2;
        return static_cast<std::size_t>(header.count) + 1 < (INTERNAL_CAPACITY + 1) / 2;
    }

    // 修复 parent 的一个可能下溢的子节点，并重建 parent 的分隔键。
    void rebalanceChild(PageId parent_id, std::size_t child_index) {
        const Page parent_page = readPage(parent_id);
        NodeHeader parent_header{};
        std::vector<SortKey> ignored_separators;
        std::vector<PageId> children;
        readInternal(parent_page, parent_header, ignored_separators, children);
        if (child_index >= children.size()) throw std::runtime_error("invalid B+ tree child index");

        const Page child_page = readPage(children[child_index]);
        NodeHeader child_header{};
        std::memcpy(&child_header, child_page.data(), sizeof(child_header));
        if (!isUnderfull(child_header)) {
            writeInternalFromChildren(parent_id, children);
            return;
        }

        const bool leaf = child_header.is_leaf != 0;
        if (child_index > 0) {
            const Page left_page = readPage(children[child_index - 1]);
            NodeHeader left_header{};
            std::memcpy(&left_header, left_page.data(), sizeof(left_header));
            if (left_header.is_leaf != child_header.is_leaf) {
                throw std::runtime_error("B+ tree siblings have incompatible node types");
            }
            if (!isUnderfull(left_header) &&
                (leaf ? left_header.count > LEAF_CAPACITY / 2
                      : static_cast<std::size_t>(left_header.count) + 1 > (INTERNAL_CAPACITY + 1) / 2)) {
                if (leaf) {
                    std::vector<LeafEntry> left = readLeaf(left_page, left_header);
                    std::vector<LeafEntry> child = readLeaf(child_page, child_header);
                    child.insert(child.begin(), left.back());
                    left.pop_back();
                    writePage(children[child_index - 1], makeLeaf(left, left_header.next_leaf));
                    writePage(children[child_index], makeLeaf(child, child_header.next_leaf));
                } else {
                    std::vector<SortKey> separators;
                    std::vector<PageId> left_children;
                    readInternal(left_page, left_header, separators, left_children);
                    std::vector<PageId> child_children;
                    readInternal(child_page, child_header, separators, child_children);
                    child_children.insert(child_children.begin(), left_children.back());
                    left_children.pop_back();
                    writeInternalFromChildren(children[child_index - 1], left_children);
                    writeInternalFromChildren(children[child_index], child_children);
                }
                writeInternalFromChildren(parent_id, children);
                return;
            }
        }

        if (child_index + 1 < children.size()) {
            const Page right_page = readPage(children[child_index + 1]);
            NodeHeader right_header{};
            std::memcpy(&right_header, right_page.data(), sizeof(right_header));
            if (right_header.is_leaf != child_header.is_leaf) {
                throw std::runtime_error("B+ tree siblings have incompatible node types");
            }
            if (!isUnderfull(right_header) &&
                (leaf ? right_header.count > LEAF_CAPACITY / 2
                      : static_cast<std::size_t>(right_header.count) + 1 > (INTERNAL_CAPACITY + 1) / 2)) {
                if (leaf) {
                    std::vector<LeafEntry> child = readLeaf(child_page, child_header);
                    std::vector<LeafEntry> right = readLeaf(right_page, right_header);
                    child.push_back(right.front());
                    right.erase(right.begin());
                    writePage(children[child_index], makeLeaf(child, child_header.next_leaf));
                    writePage(children[child_index + 1], makeLeaf(right, right_header.next_leaf));
                } else {
                    std::vector<SortKey> separators;
                    std::vector<PageId> child_children;
                    readInternal(child_page, child_header, separators, child_children);
                    std::vector<PageId> right_children;
                    readInternal(right_page, right_header, separators, right_children);
                    child_children.push_back(right_children.front());
                    right_children.erase(right_children.begin());
                    writeInternalFromChildren(children[child_index], child_children);
                    writeInternalFromChildren(children[child_index + 1], right_children);
                }
                writeInternalFromChildren(parent_id, children);
                return;
            }
        }

        // 两侧均不能借位，合并，并从父节点移除一个孩子指针。
        if (child_index > 0) {
            const Page left_page = readPage(children[child_index - 1]);
            NodeHeader left_header{};
            std::memcpy(&left_header, left_page.data(), sizeof(left_header));
            if (leaf) {
                std::vector<LeafEntry> left = readLeaf(left_page, left_header);
                const std::vector<LeafEntry> child = readLeaf(child_page, child_header);
                left.insert(left.end(), child.begin(), child.end());
                writePage(children[child_index - 1], makeLeaf(left, child_header.next_leaf));
            } else {
                std::vector<SortKey> separators;
                std::vector<PageId> left_children;
                readInternal(left_page, left_header, separators, left_children);
                std::vector<PageId> child_children;
                readInternal(child_page, child_header, separators, child_children);
                left_children.insert(left_children.end(), child_children.begin(), child_children.end());
                writeInternalFromChildren(children[child_index - 1], left_children);
            }
            children.erase(children.begin() + child_index);
        } else {
            const Page right_page = readPage(children[child_index + 1]);
            NodeHeader right_header{};
            std::memcpy(&right_header, right_page.data(), sizeof(right_header));
            if (leaf) {
                std::vector<LeafEntry> child = readLeaf(child_page, child_header);
                const std::vector<LeafEntry> right = readLeaf(right_page, right_header);
                child.insert(child.end(), right.begin(), right.end());
                writePage(children[child_index], makeLeaf(child, right_header.next_leaf));
            } else {
                std::vector<SortKey> separators;
                std::vector<PageId> child_children;
                readInternal(child_page, child_header, separators, child_children);
                std::vector<PageId> right_children;
                readInternal(right_page, right_header, separators, right_children);
                child_children.insert(child_children.end(), right_children.begin(), right_children.end());
                writeInternalFromChildren(children[child_index], child_children);
            }
            children.erase(children.begin() + child_index + 1);
        }
        writeInternalFromChildren(parent_id, children);
    }

public:
    // 绑定一个索引文件。文件为空时初始化元数据，已有文件则读取根页信息。
    DiskBPlusTree(bufPageManager& pages, FileId index_file)
        : pages_(pages), file_id_(index_file) {
        static_assert(std::is_trivially_copyable<SortKey>::value,
                      "SortKey must be trivially copyable");
        static_assert(std::is_trivially_copyable<Value>::value,
                      "Value must be trivially copyable");
        static_assert(LEAF_CAPACITY >= 2, "SortKey and Value are too large for a leaf page");
        static_assert(INTERNAL_CAPACITY >= 3, "SortKey is too large for an internal page");
        if (!pages_.isOpen(index_file)) {
            throw FileException(FileError::FILE_INVALID_ID, "index file id is not open");
        }
        openOrInitialize();
    }

    DiskBPlusTree(const DiskBPlusTree&) = delete;
    DiskBPlusTree& operator=(const DiskBPlusTree&) = delete;

    // 返回磁盘元数据中的记录数。
    std::uint64_t size() const noexcept { return metadata_.size; }

    // 插入键值项。相同键和值可重复插入；重复键按键顺序保存在叶页中。
    void insert(const SortKey& key, const Value& value) {
        if (metadata_.root_page == INVALID_PAGE) {
            const PageId root = allocatePage();
            writePage(root, makeLeaf({LeafEntry{key, value}}, INVALID_PAGE));
            metadata_.root_page = root;
        } else {
            Split split = insertRecursive(metadata_.root_page, key, value);
            if (split.happened) {
                const PageId new_root = allocatePage();
                writePage(new_root, makeInternal({split.separator},
                                                  {metadata_.root_page, split.right_page}));
                metadata_.root_page = new_root;
            }
        }
        ++metadata_.size;
        saveMetadata();
    }

    // 删除一条精确匹配的键值映射；未找到时返回 false。
    bool remove(const SortKey& key, const Value& value) {
        if (metadata_.root_page == INVALID_PAGE) return false;

        PageId leaf_id = metadata_.root_page;
        for (;;) {
            const Page page = readPage(leaf_id);
            NodeHeader header{};
            std::memcpy(&header, page.data(), sizeof(header));
            if (header.is_leaf) break;
            std::vector<SortKey> separators;
            std::vector<PageId> children;
            readInternal(page, header, separators, children);
            const std::size_t child_index = static_cast<std::size_t>(
                std::lower_bound(separators.begin(), separators.end(), key) - separators.begin());
            leaf_id = children[child_index];
        }

        NodeHeader leaf_header{};
        std::vector<LeafEntry> entries;
        for (;;) {
            const Page page = readPage(leaf_id);
            entries = readLeaf(page, leaf_header);
            const auto it = std::find_if(entries.begin(), entries.end(),
                [&](const LeafEntry& entry) {
                    return sameKey(entry.key, key) && entry.value == value;
                });
            if (it != entries.end()) {
                entries.erase(it);
                break;
            }
            if (leaf_header.next_leaf == INVALID_PAGE) return false;
            const Page next_page = readPage(leaf_header.next_leaf);
            NodeHeader next_header{};
            const std::vector<LeafEntry> next_entries = readLeaf(next_page, next_header);
            if (next_entries.empty() || key < next_entries.front().key) return false;
            leaf_id = leaf_header.next_leaf;
        }

        if (leaf_id == metadata_.root_page) {
            if (entries.empty()) {
                metadata_.root_page = INVALID_PAGE;
            } else {
                writePage(leaf_id, makeLeaf(entries, leaf_header.next_leaf));
            }
            --metadata_.size;
            saveMetadata();
            return true;
        }

        writePage(leaf_id, makeLeaf(entries, leaf_header.next_leaf));
        std::vector<ParentStep> path;
        if (!findLeafPath(metadata_.root_page, leaf_id, path)) {
            throw std::runtime_error("B+ tree leaf is unreachable from root");
        }
        for (std::size_t depth = path.size(); depth > 0; --depth) {
            rebalanceChild(path[depth - 1].page_id, path[depth - 1].child_index);
        }

        const Page root_page = readPage(metadata_.root_page);
        NodeHeader root_header{};
        std::memcpy(&root_header, root_page.data(), sizeof(root_header));
        if (!root_header.is_leaf) {
            std::vector<SortKey> separators;
            std::vector<PageId> children;
            readInternal(root_page, root_header, separators, children);
            if (children.size() == 1) metadata_.root_page = children.front();
        }
        --metadata_.size;
        saveMetadata();
        return true;
    }

    // 用新映射替换一条精确匹配的旧映射。返回 false 表示旧映射不存在。
    // 此操作不是事务：底层 I/O 或内存分配异常时，由调用方决定如何恢复。
    bool update(const SortKey& old_key, const Value& old_value,
                const SortKey& new_key, const Value& new_value) {
        if (!remove(old_key, old_value)) return false;
        insert(new_key, new_value);
        return true;
    }

    // 返回指定键关联的所有值；重复键跨叶页时会继续沿叶链查找。
    std::vector<Value> search(const SortKey& key) const {
        std::vector<Value> result;
        if (metadata_.root_page == INVALID_PAGE) return result;

        PageId page_id = metadata_.root_page;
        for (;;) {
            Page page = readPage(page_id);
            NodeHeader header{};
            std::memcpy(&header, page.data(), sizeof(header));
            if (header.is_leaf) {
                std::vector<LeafEntry> entries = readLeaf(page, header);
                for (const LeafEntry& entry : entries) {
                    if (key < entry.key) return result;
                    if (!(entry.key < key)) result.push_back(entry.value);
                }
                page_id = header.next_leaf;
                while (page_id != INVALID_PAGE) {
                    page = readPage(page_id);
                    entries = readLeaf(page, header);
                    if (entries.empty() || key < entries.front().key) break;
                    for (const LeafEntry& entry : entries) {
                        if (key < entry.key) return result;
                        if (!(entry.key < key)) result.push_back(entry.value);
                    }
                    page_id = header.next_leaf;
                }
                return result;
            }

            std::vector<SortKey> separators;
            std::vector<PageId> children;
            readInternal(page, header, separators, children);
            const std::size_t child_index = static_cast<std::size_t>(
                std::lower_bound(separators.begin(), separators.end(), key) - separators.begin());
            page_id = children[child_index];
        }
    }

    // 将缓冲池中该文件的脏页写回磁盘。
    void flush() { pages_.flushAll(); }
};
