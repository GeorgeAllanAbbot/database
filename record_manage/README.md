# 记录管理

对应教程 [2.1 记录管理](https://thu-db.github.io/dbs-tutorial/chapter-2/intro/) 与 [2.2 定长记录](https://thu-db.github.io/dbs-tutorial/chapter-2/fixed/)。本模块建立在现有 `FileManager` 和 `bufPageManager` 之上，使用固定宽度槽位；SQL 的 `VARCHAR(n)` 同样分配固定宽度的数据区。

## 功能与接口

公开头文件为 `record_manage/record_manager.h`，类型位于 `db` 命名空间。CMake 调用方链接 `record_manage` 即可。

| 类型 / 接口 | 用途 |
| --- | --- |
| `Value` | `variant<monostate, int32_t, double, string>`；`monostate` 表示 SQL NULL。 |
| `Record` | 按 Schema 列顺序排列的 `vector<Value>`。 |
| `Column` | 列名、类型、VARCHAR 最大字节数、可空标记及可选默认值。 |
| `Schema` | 表名、列定义、主键、外键和 UNIQUE 定义；`validate()` 校验定义。 |
| `RecordCodec` | 记录序列化和反序列化，也可独立使用。 |
| `RID` | 文件内的 `(page, slot)`；可平凡复制，可以作为 `DiskBPlusTree` 的 Value。 |
| `RecordManager::createFile(path, schema)` | 创建新记录文件，返回 `shared_ptr<RecordFile>`；拒绝覆盖已有文件。 |
| `RecordManager::openFile(path)` | 打开并恢复 Schema、记录数、页数与空闲页链表；同一管理器重复打开返回相同活跃句柄。 |
| `RecordManager::removeFile(path)` | 删除已关闭的记录文件；仍打开时拒绝删除。 |
| `RecordFile::insert(record)` | 插入完整记录并返回 RID；优先使用空闲槽位。 |
| `RecordFile::get(rid)` | 根据 RID 获取记录。 |
| `RecordFile::update(rid, record)` | 校验完整新记录后原位更新，RID 不变。 |
| `RecordFile::remove(rid)` | 删除记录并将槽位加入可复用空间；重复删除报错。 |
| `RecordFile::contains(rid)` | 判断槽位是否有记录；越界 RID 返回 false，关闭文件或损坏页仍报错。 |
| `RecordFile::scan()` | 创建逐页扫描器；`next()` 返回 `optional<RecordEntry>`，每项包含 RID 和记录。 |
| `size()` / `dataPageCount()` | 活记录数 / 数据页数；后者不包括文件头和 Schema 页。 |
| `slotsPerPage()` / `recordSize()` | 每页槽位数 / 单条记录编码长度。 |
| `flush()` / `close()` | 仅写回并移除本文件缓存 / 再关闭文件。`close()` 可重复调用。 |

`RecordException::code()` 返回 `common/errors.h` 中的 `RecordError`。文件系统 I/O 错误继续使用 `FileException`。

## 示例

```cpp
#include "record_manage/record_manager.h"
#include "backend/disk_b_plus_tree.h"
#include <filesystem>
#include <iostream>

int main() {
    // 数据库目录和表名到文件路径的映射由上层系统管理器决定。
    std::filesystem::create_directories("data/school");
    FileManager files;
    bufPageManager pages(files, 64);
    db::RecordManager records(files, pages);

    db::Schema schema;
    schema.table_name = "students";
    schema.columns = {
        {"id", db::DataType::INT, 0, false, std::nullopt},
        {"score", db::DataType::FLOAT, 0, true, db::Value{0.0}},
        {"name", db::DataType::VARCHAR, 32, true, std::nullopt}
    };
    schema.primary_key = db::PrimaryKey{"", {"id"}};

    auto table = records.createFile("data/school/students.tbl", schema);
    const db::RID rid = table->insert({std::int32_t{1001}, 95.25, std::string("alice")});
    table->update(rid, {std::int32_t{1001}, 96.5, std::monostate{}});
    const auto row = table->get(rid);

    auto scan = table->scan();
    while (auto entry = scan.next())
        std::cout << std::get<std::int32_t>(entry->values[0]) << '\n';

    // 上层显式维护索引；表记录位置在 update 时保持不变。
    const auto index_id = files.createFile("data/school/students_id.idx");
    DiskBPlusTree<std::int32_t, db::RID> index(pages, index_id);
    index.insert(1001, rid);
    const auto found = table->get(index.search(1001).at(0));

    table->remove(rid);
    index.remove(1001, rid);
    table->close();
    pages.discardFile(index_id);
    files.closeFile(index_id);
}
```

示例需要在空目录运行；重复运行时可以使用 `openFile` 打开已有文件。正常退出时，记录文件析构也会尝试写回和关闭；需要获知写回错误时应显式调用 `close()`。

## 编码与校验

- `INT` 使用 32 位整数，`FLOAT` 使用 64 位 IEEE 754 double。类型严格匹配，FLOAT 拒绝 NaN 和无穷大。
- 数值列的 `max_length` 填 0；`VARCHAR(n)` 的 n 为 1～2048。字符串保存显式长度，空字符串、NULL 和包含 `\0` 的字符串可以区分。
- 每条记录必须提供全部列，依次检查列数、类型、字符串长度和 NOT NULL。`default_value` 保存为元数据，缺省值的应用由上层执行器处理；未指定默认值与显式 `DEFAULT NULL` 可以区分。
- 表名、列名最多 32 字节，列数为 1～10，各列最大数据长度之和不超过 2048 字节。NULL 位图和 VARCHAR 的长度前缀另外计算，因此最大记录编码可能略大于 2048 字节。
- Schema 校验重名列、默认值、约束列名和外键两侧列数；主键列必须显式设置 `nullable = false`。主键、外键和 UNIQUE 定义持久化，跨记录唯一性、外键引用检查和索引同步由后续系统管理 / 查询执行模块负责。记录层不会自动创建索引。
- 当前 Schema 创建后固定；变长存储、跨页记录和 Schema 变更不属于本记录格式。

## 磁盘格式 v1

所有页固定 4096 字节；整数使用小端表示，磁盘上不直接保存 C++ 结构体或指针。文件由一页文件头、若干连续 Schema 页和数据页组成。Schema 当前最多 1 MiB；元数据跨页时自动预留相应页数。

文件头布局：

| 偏移 | 字节数 | 内容 |
| --- | --- | --- |
| 0 | 8 | `RECFILE1` magic |
| 8 | 4 | 格式版本 1 |
| 12 | 4 | 页大小 |
| 16 | 4 | Schema 编码长度 |
| 20 | 4 | Schema 页数 |
| 24 | 4 | 单记录编码长度 |
| 28 | 4 | 每数据页槽位数 |
| 32 | 4 | 文件总页数，包含头和 Schema 页 |
| 36 | 4 | 空闲页链表头，`UINT32_MAX` 表示无空闲页 |
| 40 | 8 | 活记录数 |

Schema 按长度前缀编码表名、各列定义、默认值及约束定义；打开时检查格式版本、页大小、元数据边界和文件长度。

数据页布局为 **16 字节页头 + 槽位占用位图 + 定长记录槽**。页头包含数据页 magic（4 字节）、自身页号（4 字节）、活槽数（2 字节）、槽容量（2 字节）和下一空闲页号（4 字节）。槽容量由页大小、位图长度和记录宽度计算。访问数据页时检查页头、链表页号和位图中的活槽数。

一条记录为 **NULL 位图 + 按列顺序排列的数据**。NULL 位图为每列保留一位；INT 4 字节，FLOAT 8 字节，VARCHAR 为 2 字节实际长度加 n 字节数据区。无效数据区补零。

所有未满的数据页构成单向空闲链表。插入只需定位链表头并在该页位图中找槽，页满后移出链表；从满页删除记录时将其加入链表头。更新原位进行，删除不会移动其他记录。空页保留在链表中复用，不收缩文件。

## 生命周期与扫描约定

- `FileManager` 和 `bufPageManager` 必须比所有记录句柄、索引和扫描器活得更久；同一存储组合使用一个 `RecordManager`。
- 使用记录管理接口操作表文件，避免绕过该层直接关闭文件、修改页或通过路径别名另开底层流。记录管理器会规范化路径，并拒绝通过另一记录管理器重复打开同一底层句柄。
- 扫描器持有记录文件的共享引用，每次只保留一页副本。成功增删改会使现有扫描器失效，下次 `next()` 抛出 `SCAN_INVALIDATED`；关闭文件后扫描报 `FILE_CLOSED`。先收集待操作 RID 再修改，或在修改后重新扫描。
- RID 属于单个表文件。删除后原槽可能被新记录复用，RID 没有代际信息，不能用旧 RID 判断新旧记录；上层删除时必须同步维护索引。
- 正常退出和显式关闭提供持久化。与现有文件模块一致，析构中的 I/O 异常无法上报；发生 I/O 或内存分配错误时没有事务回滚或崩溃一致性保证。
