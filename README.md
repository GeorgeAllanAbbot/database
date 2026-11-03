# Database 存储模块

目前实现了文件与页缓存、定长 / 变长记录管理和磁盘 B+ 树。记录模块的接口、磁盘格式和示例见 [record_manage/README.md](record_manage/README.md)。创建变长表时传入 `db::RecordLayout::VARIABLE`，重新打开时自动恢复模式。

```text
RecordManager / DiskBPlusTree → bufPageManager（按页缓存、脏页写回） → FileManager → 磁盘
```

公开头文件：

```cpp
#include "file_manage/file_manage.h"
#include "file_manage/buf_page_manager.h"
#include "backend/disk_b_plus_tree.h"
#include "record_manage/record_manager.h"
```

通过 CMake 构建 `record_manage` 目标即可使用记录管理模块，它会自动链接 `file_manage`：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

CTest 覆盖两种记录布局的编解码、Schema 校验和恢复、增删改查、空闲槽复用、流式扫描、随机操作模型比对、损坏文件检查、B+ 树与 RID 联动，以及独立进程间的持久化。变长测试还覆盖记录反复增长 / 缩短、转发槽、碎片整理和跨空间目录页分配。测试也会重复包含公共错误头文件，防止枚举重复定义问题回归。设置 `-DBUILD_TESTING=OFF` 可仅构建存储库。

## 公共类型与异常

```cpp
using FileId = std::uint32_t;
using PageId = std::uint32_t;
```

`FileId` 和 `PageId` 均为 32 位无符号整数。`FileId` 是一次 `FileManager` 生命周期中打开文件的句柄；`PageId` 是文件内从零开始的页序号。

所有可恢复的文件层错误都会抛出 `FileException`：

```cpp
try {
    // 文件或页操作
} catch (const FileException& error) {
    FileError code = error.code();
    // error.what() 提供说明文字
}
```

错误码定义在 `common/errors.h`，包括无效文件/页 ID、文件不存在、创建/打开/读写/定位失败、短读、以及缓存页不存在等情形。

## `FileManager`

`FileManager` 管理已打开文件，并提供字节级随机读写。它不可复制。

| 接口 | 说明 |
| --- | --- |
| `FileId createFile(const std::string& filename)` | 创建一个新空文件并打开；目标已存在时抛出 `FILE_ALREADY_EXISTS`。 |
| `FileId openFile(const std::string& filename)` | 打开已有普通文件；同一路径已打开时返回已有 `FileId`。 |
| `void closeFile(FileId id)` | 刷新并关闭一个文件。 |
| `std::string closeFile(const std::string& filename)` | 按路径关闭文件的兼容接口，返回传入路径。 |
| `void closeAll() noexcept` | 尽力关闭此管理器中的全部文件。 |
| `bool isOpen(FileId id) const noexcept` | 判断句柄当前是否仍打开。 |
| `FileId idFor(const std::string& filename) const` | 获取已打开路径对应的句柄。 |
| `const std::string& filename(FileId id) const` | 获取句柄对应的路径。 |
| `std::uint64_t fileSize(FileId id)` | 返回当前文件长度（字节）。 |
| `void read(FileId id, std::uint64_t offset, char* data, std::size_t size)` | 从 `offset` 读取恰好 `size` 字节；越过文件末尾会抛异常。 |
| `void write(FileId id, std::uint64_t offset, const char* data, std::size_t size)` | 从 `offset` 写入 `size` 字节；允许扩展文件。 |
| `void flush(FileId id)` | 将 C++ 文件流缓冲的数据刷新到底层文件。 |
| `void removeFile(const std::string& filename)` | 若文件打开则先关闭，然后删除该文件。 |

## `bufPageManager`

`bufPageManager` 是固定大小的页缓存。每页固定为 `PAGE_SIZE` 字节，目前是 **4096**。构造函数的 `capacity` 是可同时缓存的最多页数，默认 64。

| 接口 | 说明 |
| --- | --- |
| `explicit bufPageManager(FileManager&, std::size_t capacity = 64)` | 创建页缓存；容量必须大于零。 |
| `bool isOpen(FileId) const noexcept` | 转发查询底层文件是否打开。 |
| `char* getPage(FileId, PageId)` | 获取一页的内存地址。缓存命中时不产生磁盘 I/O；未命中时读取文件，文件末尾后的部分以零填充。 |
| `void markDirty(FileId, PageId)` | 将已缓存且被调用方修改的页标为脏页。 |
| `void flushPage(FileId, PageId)` | 将指定缓存页（若为脏页）写回，然后刷新对应文件。 |
| `void discardFile(FileId)` | 写回并从缓存中移除该文件的全部页。应在关闭该文件前调用。 |
| `void flushAll()` | 写回所有脏页并刷新涉及的文件。 |

缓存满时按 LRU（最近最少使用）策略淘汰页面；被淘汰的脏页会先写回磁盘。析构 `bufPageManager` 时也会尝试 `flushAll()`。

### 页面指针约定

`getPage()` 返回的 `char*` 指向缓存帧。该指针只在页面仍被缓存期间有效；后续访问其他页可能导致 LRU 淘汰，因此不要长期保存它。页面内容被改动后，必须调用 `markDirty()`，否则改动不会保证写回磁盘。

关闭一个文件前，应先调用 `discardFile(file_id)` 或 `flushAll()`，并确保不再持有该文件页面的指针。

## `DiskBPlusTree`

`DiskBPlusTree<SortKey, Value>` 是建立在 `bufPageManager` 之上的持久化 B+ 树索引。它不由文件模块自动创建；应由上层的 `TableManager`、`IndexManager` 或 `RecordManager` 在打开索引文件后显式持有和调用。

```text
TableManager / IndexManager
        │ insert / search / flush
        ▼
DiskBPlusTree<SortKey, Value>
        ▼
bufPageManager → FileManager → 索引文件
```

构造时传入的 `FileId` 必须是仍处于打开状态的索引文件。空文件会自动初始化 B+ 树元数据；已有文件会从零号页恢复根节点、下一可分配页号及记录数。`SortKey` 与 `Value` 必须是固定大小、可平凡复制的类型，例如整数、固定布局的记录 ID 或 POD 结构体。

| 接口 | 说明 |
| --- | --- |
| `DiskBPlusTree(bufPageManager&, FileId index_file)` | 绑定一个已打开的索引文件，并初始化或恢复树。 |
| `void insert(const SortKey&, const Value&)` | 插入键值映射；相同的 `(key, value)` 也会被保留为独立记录。 |
| `std::vector<Value> search(const SortKey&) const` | 返回某键的全部关联值；未命中时返回空向量。 |
| `bool remove(const SortKey&, const Value&)` | 删除一条精确匹配的键值映射；未找到时返回 `false`。删除会按需借位、合并节点及收缩根。 |
| `bool update(const SortKey& old_key, const Value& old_value, const SortKey& new_key, const Value& new_value)` | 以一条调用替换精确匹配的旧映射；旧映射不存在时返回 `false`。 |
| `std::uint64_t size() const noexcept` | 返回已插入的键值映射数量。 |
| `void flush()` | 写回缓冲池中的脏页。当前会刷新该缓冲池涉及的全部打开文件。 |

当前磁盘版 B+ 树提供插入、精确删除、查询和映射更新。`update` 封装了“删除旧映射，再插入新映射”；它不是事务，在底层 I/O 或内存分配异常时不保证自动回滚。

## 最小示例

```cpp
#include "file_manage/buf_page_manager.h"
#include "backend/disk_b_plus_tree.h"
#include <cstring>
#include <cstdint>
#include <vector>

FileManager files;
FileId students = files.createFile("students.db");
FileId student_id_index = files.createFile("student_id.idx");

{
    bufPageManager pages(files, 64);

    // 表数据页：调用方负责修改后标记脏页。
    char* page = pages.getPage(students, PageId{10});
    std::memcpy(page, "alice", 6);
    pages.markDirty(students, PageId{10});

    // 索引：上层显式构造和调用磁盘 B+ 树。
    DiskBPlusTree<std::int32_t, std::int32_t> index(pages, student_id_index);
    index.insert(1001, 10);            // student_id -> 数据记录/页号
    std::vector<std::int32_t> rows = index.search(1001);
    index.flush();

    // 写回并解除页面缓存后，才关闭文件。
    pages.discardFile(students);
    pages.discardFile(student_id_index);
}

files.closeFile(students);
files.closeFile(student_id_index);
```
