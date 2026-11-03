// 故意直接、间接、重复包含公共错误头，覆盖原来的枚举重复定义问题。
#include "common/errors.h"
#include "file_manage/file_manage.h"
#include "common/errors.h"
#include "record_manage/record_manager.h"
#include "backend/disk_b_plus_tree.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <type_traits>

namespace fs = std::filesystem;
using namespace db;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(\
    std::string("check failed at line ") + std::to_string(__LINE__) + ": " #condition); } while (false)

template<class Action> void expect(RecordError error, Action action) {
    try { action(); }
    catch (const RecordException& e) { CHECK(e.code() == error); return; }
    throw std::runtime_error("expected RecordException");
}

Column column(std::string name, DataType type, std::uint32_t width = 0, bool nullable = true,
              std::optional<Value> default_value = std::nullopt) {
    return Column{std::move(name), type, width, nullable, std::move(default_value)};
}

Schema students(std::uint32_t width = 64) {
    Schema schema;
    schema.table_name = "students";
    schema.columns = {
        column("id", DataType::INT, 0, false),
        column("score", DataType::FLOAT, 0, true, Value{0.0}),
        column("name", DataType::VARCHAR, width, true, Value{std::string("unknown")})
    };
    schema.primary_key = PrimaryKey{"pk_students", {"id"}};
    schema.foreign_keys = {ForeignKey{"fk_students", {"id"}, "people", {"id"}}};
    schema.unique_constraints = {UniqueConstraint{"uq_students_name", {"name"}}};
    return schema;
}

Record row(int id) { return {std::int32_t(id), id * 0.125, std::string("student_") + std::to_string(id)}; }

void checkSchema(const Schema& actual, const Schema& wanted) {
    CHECK(actual.table_name == wanted.table_name);
    CHECK(actual.columns.size() == wanted.columns.size());
    for (std::size_t i = 0; i < actual.columns.size(); ++i) {
        const auto& a = actual.columns[i]; const auto& b = wanted.columns[i];
        CHECK(a.name == b.name && a.type == b.type && a.max_length == b.max_length);
        CHECK(a.nullable == b.nullable && a.default_value == b.default_value);
    }
    CHECK(actual.primary_key.has_value() == wanted.primary_key.has_value());
    if (actual.primary_key) {
        CHECK(actual.primary_key->name == wanted.primary_key->name);
        CHECK(actual.primary_key->columns == wanted.primary_key->columns);
    }
    CHECK(actual.foreign_keys.size() == wanted.foreign_keys.size());
    for (std::size_t i = 0; i < actual.foreign_keys.size(); ++i) {
        const auto& a = actual.foreign_keys[i]; const auto& b = wanted.foreign_keys[i];
        CHECK(a.name == b.name && a.columns == b.columns);
        CHECK(a.referenced_table == b.referenced_table && a.referenced_columns == b.referenced_columns);
    }
    CHECK(actual.unique_constraints.size() == wanted.unique_constraints.size());
    for (std::size_t i = 0; i < actual.unique_constraints.size(); ++i) {
        CHECK(actual.unique_constraints[i].name == wanted.unique_constraints[i].name);
        CHECK(actual.unique_constraints[i].columns == wanted.unique_constraints[i].columns);
    }
}

void checkScan(const std::shared_ptr<RecordFile>& file, const std::map<RID, Record>& expected) {
    auto scan = file->scan();
    std::map<RID, Record> actual;
    while (auto entry = scan.next()) CHECK(actual.emplace(entry->rid, entry->values).second);
    CHECK(!scan.next());
    CHECK(actual == expected);
    CHECK(file->size() == expected.size());
}

void codecTests() {
    RecordCodec codec(students());
    auto record = row(-17);
    record[1] = -123.456789;
    record[2] = std::string("a\0b", 3);
    auto bytes = codec.encode(record);
    CHECK(codec.decode(bytes.data(), bytes.size()) == record);
    record = {std::numeric_limits<std::int32_t>::min(), std::monostate{}, std::string{}};
    bytes = codec.encode(record);
    CHECK(codec.decode(bytes.data(), bytes.size()) == record);
    record = {std::numeric_limits<std::int32_t>::max(), 0.0, std::monostate{}};
    bytes = codec.encode(record);
    CHECK(codec.decode(bytes.data(), bytes.size()) == record);
    CHECK(static_cast<unsigned char>(bytes[0]) == 4);
    CHECK(static_cast<unsigned char>(bytes[1]) == 255); // INT 小端编码。
    expect(RecordError::TYPE_MISMATCH, [&] { codec.encode({std::int32_t(1)}); });
    expect(RecordError::TYPE_MISMATCH, [&] { codec.encode({1.0, 0.0, std::string{}}); });
    expect(RecordError::TYPE_MISMATCH, [&] { codec.encode({std::int32_t(1), std::int32_t(1), std::string{}}); });
    expect(RecordError::NULL_NOT_ALLOWED, [&] { codec.encode({std::monostate{}, 0.0, std::string{}}); });
    expect(RecordError::VALUE_TOO_LONG, [&] { codec.encode({std::int32_t(1), 0.0, std::string(65, 'x')}); });
    expect(RecordError::TYPE_MISMATCH, [&] { codec.encode({std::int32_t(1), std::numeric_limits<double>::infinity(), std::string{}}); });
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(bytes.data(), bytes.size() - 1); });
    bytes = codec.encode(row(1)); bytes[0] = 1;
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(bytes.data(), bytes.size()); });
    bytes = codec.encode(row(1)); bytes[13] = static_cast<char>(255); bytes[14] = static_cast<char>(255);
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(bytes.data(), bytes.size()); });

    Schema ten;
    ten.table_name = "ten";
    Record nulls;
    for (int i = 0; i < 10; ++i) { ten.columns.push_back(column("c" + std::to_string(i), DataType::INT)); nulls.emplace_back(std::monostate{}); }
    RecordCodec wide_bitmap(ten);
    bytes = wide_bitmap.encode(nulls);
    CHECK(static_cast<unsigned char>(bytes[0]) == 255 && bytes[1] == 3);
    CHECK(wide_bitmap.decode(bytes.data(), bytes.size()) == nulls);
    ten.columns.push_back(column("c10", DataType::INT));
    expect(RecordError::INVALID_SCHEMA, [&] { RecordCodec bad(ten); });

    auto bad = students(); bad.columns[2].name = "id";
    expect(RecordError::INVALID_SCHEMA, [&] { bad.validate(); });
    bad = students(); bad.columns[1].default_value = Value{std::string("wrong")};
    expect(RecordError::INVALID_SCHEMA, [&] { bad.validate(); });
    bad = students(); bad.columns[0].nullable = true;
    expect(RecordError::INVALID_SCHEMA, [&] { bad.validate(); });
    bad = students(); bad.foreign_keys[0].columns = {"missing"};
    expect(RecordError::INVALID_SCHEMA, [&] { bad.validate(); });
    bad = students(); bad.columns[2].max_length = 2048;
    expect(RecordError::INVALID_SCHEMA, [&] { bad.validate(); });
    bad = students(); bad.table_name = std::string(33, 'x');
    expect(RecordError::INVALID_SCHEMA, [&] { bad.validate(); });
    bad = students(); bad.columns[1].type = static_cast<DataType>(99);
    expect(RecordError::INVALID_SCHEMA, [&] { bad.validate(); });
}

void storageTests(const fs::path& dir) {
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    const auto path = (dir / "students.tbl").string();
    auto invalid_schema = students(); invalid_schema.columns.clear();
    expect(RecordError::INVALID_SCHEMA, [&] { records.createFile(path, invalid_schema); });
    CHECK(!fs::exists(path));
    auto file = records.createFile(path, students());
    CHECK(records.openFile((dir / "." / "students.tbl").string()) == file);
    RecordManager other(files, pages);
    expect(RecordError::FILE_IN_USE, [&] { other.openFile(path); });
    expect(RecordError::FILE_IN_USE, [&] { records.removeFile(path); });
    CHECK(!file->scan().next());
    std::map<RID, Record> model;
    std::vector<RID> ids;
    const auto capacity = file->slotsPerPage();
    for (int i = 0; i < capacity * 4; ++i) {
        auto rid = file->insert(row(i)); ids.push_back(rid); model[rid] = row(i);
    }
    CHECK(file->dataPageCount() == 4);
    for (const auto& [rid, value] : model) CHECK(file->get(rid) == value);
    auto changed = row(-1); changed[2] = std::string(64, 'x');
    file->update(ids[3], changed); model[ids[3]] = changed;
    CHECK(file->get(ids[3]) == changed);
    const auto before = file->get(ids[4]);
    expect(RecordError::VALUE_TOO_LONG, [&] { auto invalid = row(1); invalid[2] = std::string(65, 'x'); file->update(ids[4], invalid); });
    CHECK(file->get(ids[4]) == before);
    auto scan = file->scan();
    expect(RecordError::TYPE_MISMATCH, [&] { file->insert({}); });
    CHECK(scan.next()); // 拒绝非法输入不使扫描器失效。
    file->update(ids[4], before);
    expect(RecordError::SCAN_INVALIDATED, [&] { scan.next(); });

    for (std::size_t i = 0; i < ids.size(); i += 3) {
        file->remove(ids[i]); model.erase(ids[i]);
        CHECK(!file->contains(ids[i]));
        expect(RecordError::RECORD_NOT_FOUND, [&] { file->get(ids[i]); });
        expect(RecordError::RECORD_NOT_FOUND, [&] { file->remove(ids[i]); });
    }
    file->close(); file->close();
    expect(RecordError::FILE_CLOSED, [&] { file->get(ids[1]); });
    expect(RecordError::FILE_CLOSED, [&] { scan.next(); });
    file = records.openFile(path);
    checkSchema(file->schema(), students());
    checkScan(file, model);
    for (std::size_t i = 0; i < ids.size(); i += 3) {
        auto value = row(static_cast<int>(i + 10000));
        auto rid = file->insert(value);
        CHECK(!model.count(rid)); model[rid] = value;
    }
    CHECK(file->dataPageCount() == 4); // 从多个已满页删除后，链表应覆盖所有空槽。
    checkScan(file, model);
    expect(RecordError::INVALID_RID, [&] { file->get(RID{0, 0}); });
    expect(RecordError::INVALID_RID, [&] { file->get(RID{std::numeric_limits<PageId>::max(), 0}); });
    expect(RecordError::INVALID_RID, [&] { file->get(RID{ids[0].page, capacity}); });
    CHECK(!file->contains(RID{0, 0}));

    for (const auto& [rid, value] : model) { (void)value; file->remove(rid); }
    model.clear(); checkScan(file, model);
    file->close(); file = records.openFile(path);
    for (std::size_t i = 0; i < ids.size(); ++i) { auto value = row(static_cast<int>(i)); model[file->insert(value)] = value; }
    CHECK(file->dataPageCount() == 4);
    checkScan(file, model);
    file->flush(); checkScan(file, model);
    file->close(); records.removeFile(path);
    CHECK(!fs::exists(path));

    // 最大基础记录，以及 default 的各种类型和显式 NULL，均需能持久化。
    Schema large;
    large.table_name = "large";
    large.columns = {column("text", DataType::VARCHAR, 2048, true, Value{std::monostate{}})};
    auto large_file = records.createFile((dir / "large.tbl").string(), large);
    CHECK(large_file->slotsPerPage() == 1);
    const Record max_row{std::string(2048, 'z')};
    const auto max_rid = large_file->insert(max_row);
    large_file->close(); large_file = records.openFile((dir / "large.tbl").string());
    CHECK(large_file->get(max_rid) == max_row); checkSchema(large_file->schema(), large);
    const auto null_rid = large_file->insert({std::monostate{}});
    large_file->close(); large_file = records.openFile((dir / "large.tbl").string());
    CHECK(large_file->get(null_rid) == Record{std::monostate{}});

    auto many = students();
    many.columns[0].default_value = Value{std::int32_t(-1)};
    for (int i = 1; i < 150; ++i) many.foreign_keys.push_back(ForeignKey{"fk_" + std::to_string(i), {"id"}, "other_table", {"id"}});
    const auto many_path = (dir / "many.tbl").string();
    auto many_file = records.createFile(many_path, many);
    auto rid = many_file->insert(row(42));
    CHECK(rid.page > 2); // Schema 横跨多个页。
    many_file->close(); many_file = records.openFile(many_path);
    checkSchema(many_file->schema(), many);
    CHECK(many_file->get(rid) == row(42));
}

void randomizedTests(const fs::path& dir) {
    FileManager files; bufPageManager pages(files, 2); RecordManager records(files, pages);
    const auto path = (dir / "random.tbl").string();
    auto file = records.createFile(path, students());
    std::map<RID, Record> model;
    std::mt19937 rng(63821);
    for (int i = 0; i < 4000; ++i) {
        const auto action = rng() % 3;
        if (model.empty() || action == 0) {
            const auto value = row(i); const auto rid = file->insert(value);
            CHECK(model.emplace(rid, value).second);
        } else {
            auto it = model.begin(); std::advance(it, rng() % model.size());
            if (action == 1) { file->remove(it->first); model.erase(it); }
            else { it->second = row(i); file->update(it->first, it->second); }
        }
        if (i % 100 == 0) checkScan(file, model);
        if (i % 500 == 0) { file->close(); file = records.openFile(path); checkScan(file, model); }
    }
    checkScan(file, model);
}

void indexTests(const fs::path& dir) {
    static_assert(std::is_trivially_copyable<RID>::value, "RID must be usable as a disk index value");
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    const auto path = (dir / "indexed.tbl").string();
    const auto index_path = (dir / "indexed.idx").string();
    auto file = records.createFile(path, students());
    auto id = files.createFile(index_path);
    {
        DiskBPlusTree<std::int32_t, RID> index(pages, id);
        for (int i = 0; i < 1500; ++i) index.insert(i, file->insert(row(i)));
        for (int i = 0; i < 1500; i += 11) {
            const auto found = index.search(i); CHECK(found.size() == 1); CHECK(file->get(found[0]) == row(i));
        }
        const auto rid = index.search(10).at(0);
        file->update(rid, row(2000));
        CHECK(index.update(10, rid, 2000, rid));
        const auto removed = index.search(11).at(0);
        file->remove(removed); CHECK(index.remove(11, removed));
        index.flush();
    }
    pages.discardFile(id); files.closeFile(id); file->close();
    file = records.openFile(path); id = files.openFile(index_path);
    {
        DiskBPlusTree<std::int32_t, RID> index(pages, id);
        CHECK(file->get(index.search(2000).at(0)) == row(2000));
        CHECK(index.search(10).empty() && index.search(11).empty());
        CHECK(index.size() == 1499 && file->size() == 1499);
    }
    pages.discardFile(id); files.closeFile(id);
}

void writeByte(const fs::path& path, std::size_t offset, char byte) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    CHECK(file.is_open()); file.seekp(static_cast<std::streamoff>(offset)); file.put(byte); CHECK(file.good());
}

void corruptionTests(const fs::path& dir) {
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    const auto base = dir / "valid.tbl";
    auto file = records.createFile(base.string(), students()); const auto rid = file->insert(row(0)); file->close();
    auto damaged = dir / "damaged.tbl";
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeByte(damaged, 0, 0);
    expect(RecordError::CORRUPT_FILE, [&] { records.openFile(damaged.string()); });
    CHECK(fs::file_size(damaged) == fs::file_size(base));
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    fs::resize_file(damaged, fs::file_size(damaged) - 1);
    expect(RecordError::CORRUPT_FILE, [&] { records.openFile(damaged.string()); });
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeByte(damaged, 19, static_cast<char>(127)); // 防止畸形元数据诱导巨量分配。
    expect(RecordError::CORRUPT_FILE, [&] { records.openFile(damaged.string()); });
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeByte(damaged, rid.page * bufPageManager::PAGE_SIZE + 8, 0); // 位图与记录数矛盾。
    file = records.openFile(damaged.string());
    expect(RecordError::CORRUPT_FILE, [&] { file->get(rid); });
    file->close();
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeByte(damaged, bufPageManager::PAGE_SIZE + 3, static_cast<char>(127)); // 非法表名长度。
    expect(RecordError::CORRUPT_FILE, [&] { records.openFile(damaged.string()); });
}

void persistence(const std::string& mode, const fs::path& dir) {
    fs::create_directories(dir);
    const auto path = dir / "persistent.tbl";
    const auto manifest = dir / "manifest.txt";
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    if (mode == "cleanup") {
        if (fs::exists(path)) records.removeFile(path.string());
        fs::remove(manifest); return;
    }
    if (mode == "seed") {
        if (fs::exists(path)) records.removeFile(path.string());
        auto file = records.createFile(path.string(), students());
        std::vector<RID> ids;
        for (int i = 0; i < 600; ++i) ids.push_back(file->insert(row(i)));
        for (int i = 0; i < 600; i += 3) file->remove(ids[i]);
        for (int i = 0; i < 100; ++i) file->insert(row(i + 600));
        file->update(ids[1], row(9999));
        auto scan = file->scan(); std::ofstream output(manifest);
        while (auto entry = scan.next()) output << entry->rid.page << ' ' << entry->rid.slot << ' ' << std::get<std::int32_t>(entry->values[0]) << '\n';
        CHECK(output.good());
        return; // 不显式调用 flush/close，验证正常析构写回。
    }
    auto file = records.openFile(path.string()); checkSchema(file->schema(), students());
    std::ifstream input(manifest); CHECK(input.is_open());
    std::map<RID, Record> model;
    PageId page; std::uint16_t slot; int id;
    while (input >> page >> slot >> id) {
        CHECK(file->get(RID{page, slot}) == row(id)); model[RID{page, slot}] = row(id);
    }
    CHECK(model.size() == 500); checkScan(file, model);
    const auto old_pages = file->dataPageCount();
    for (int i = 0; i < 100; ++i) file->insert(row(i + 1000));
    CHECK(file->dataPageCount() == old_pages); // 重启后保留的空闲链表仍能复用。
}

int main(int argc, char** argv) {
    try {
        if (argc == 3) { persistence(argv[1], argv[2]); std::cout << "PASS " << argv[1] << '\n'; return 0; }
        const auto dir = fs::temp_directory_path() / ("record-manage-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        CHECK(fs::create_directory(dir));
        struct Cleanup { fs::path dir; ~Cleanup() { std::error_code ec; fs::remove_all(dir, ec); } } cleanup{dir};
        codecTests(); storageTests(dir); randomizedTests(dir); indexTests(dir); corruptionTests(dir);
        std::cout << "PASS record codec, metadata, CRUD, scan, free slots, randomized model, index integration, corrupt files\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
