#include "record_manage/record_manager.h"
#include "record_manage/variable_page.h"
#include "backend/disk_b_plus_tree.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>

using namespace db;
namespace fs = std::filesystem;
using db::detail::SlotKind;
using db::detail::VariablePage;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(\
    std::string("check failed at line ") + std::to_string(__LINE__) + ": " #condition); } while (false)

template<class Action> void expect(RecordError error, Action action) {
    try { action(); }
    catch (const RecordException& e) { CHECK(e.code() == error); return; }
    throw std::runtime_error("expected RecordException");
}

Schema textSchema() {
    Schema schema;
    schema.table_name = "texts";
    schema.columns = {{"text", DataType::VARCHAR, 2048, true, std::nullopt}};
    return schema;
}

Schema rowSchema() {
    Schema schema;
    schema.table_name = "rows";
    schema.columns = {{"id", DataType::INT, 0, false, std::nullopt},
                      {"text", DataType::VARCHAR, 2000, true, std::nullopt}};
    return schema;
}

Record row(int id, std::size_t length) {
    auto text = std::string(length, static_cast<char>('A' + id % 26));
    if (id % 13 == 0 && length > 2) text[length / 2] = '\0';
    return {std::int32_t(id), std::move(text)};
}

void checkScan(const std::shared_ptr<RecordFile>& file, const std::map<RID, Record>& expected) {
    auto scan = file->scan(); std::map<RID, Record> actual;
    while (auto entry = scan.next()) CHECK(actual.emplace(entry->rid, entry->values).second);
    CHECK(actual == expected && file->size() == expected.size());
    CHECK(!scan.next());
}

VariablePage inspect(FileManager& files, bufPageManager& pages, const fs::path& path, PageId id) {
    VariablePage::Page bytes{};
    std::memcpy(bytes.data(), pages.getPage(files.idFor(fs::canonical(path).string()), id), bytes.size());
    return VariablePage(bytes);
}

bool emptySlot(const VariablePage& page, std::uint32_t slot) {
    return slot >= VariablePage::slotCount(page.bytes()) || page.slot(slot).kind == SlotKind::EMPTY;
}

void codecTests() {
    Schema schema;
    schema.table_name = "mixed";
    schema.columns = {{"s", DataType::VARCHAR, 12, true, std::nullopt},
                      {"id", DataType::INT, 0, false, std::nullopt},
                      {"t", DataType::VARCHAR, 23, true, std::nullopt},
                      {"score", DataType::FLOAT, 0, true, std::nullopt}};
    RecordCodec codec(schema, RecordLayout::VARIABLE);
    CHECK(codec.minimumRecordSize() == 17 && codec.recordSize() == 52);
    Record record{std::string("a\0b", 3), std::int32_t(-2147483647), std::monostate{}, -1.25};
    auto bytes = codec.encode(record);
    CHECK(bytes.size() == 20 && static_cast<unsigned char>(bytes[0]) == 4);
    CHECK(bytes[13] == 20 && bytes[15] == 20);
    CHECK(codec.decode(bytes.data(), bytes.size()) == record);
    record = {std::string{}, std::int32_t(0), std::string{}, std::monostate{}};
    bytes = codec.encode(record);
    CHECK(bytes.size() == codec.minimumRecordSize());
    CHECK(codec.decode(bytes.data(), bytes.size()) == record);
    auto corrupt = bytes; corrupt[13] = 16;
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(corrupt.data(), corrupt.size()); });
    corrupt = bytes; corrupt[15] = 18;
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(corrupt.data(), corrupt.size()); });
    corrupt = bytes; corrupt[0] = static_cast<char>(2);
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(corrupt.data(), corrupt.size()); });
    corrupt = bytes; corrupt.push_back('x');
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(corrupt.data(), corrupt.size()); });
    expect(RecordError::CORRUPT_FILE, [&] { codec.decode(bytes.data(), 16); });
    expect(RecordError::TYPE_MISMATCH, [&] { codec.encode({}); });
    expect(RecordError::VALUE_TOO_LONG, [&] { auto bad = record; bad[0] = std::string(13, 'x'); codec.encode(bad); });

    RecordCodec text(textSchema(), RecordLayout::VARIABLE);
    const Record null{std::monostate{}};
    bytes = text.encode(null); CHECK(bytes.size() == 3);
    CHECK(text.decode(bytes.data(), bytes.size()) == null);
    const Record maximum{std::string(2048, 'x')};
    bytes = text.encode(maximum); CHECK(bytes.size() == 2051);
    CHECK(text.decode(bytes.data(), bytes.size()) == maximum);
    bytes[1] = 0; bytes[2] = 0;
    expect(RecordError::CORRUPT_FILE, [&] { text.decode(bytes.data(), bytes.size()); });
    Schema ten; ten.table_name = "ten";
    Record values;
    for (int i = 0; i < 10; ++i) {
        ten.columns.push_back(Column{"c" + std::to_string(i), DataType::VARCHAR, 10, true, std::nullopt});
        values.push_back(i % 2 ? Value{std::string{}} : Value{std::monostate{}});
    }
    RecordCodec bitmap(ten, RecordLayout::VARIABLE);
    bytes = bitmap.encode(values); CHECK(bytes.size() == 22);
    CHECK(bitmap.decode(bytes.data(), bytes.size()) == values);
    Schema numeric; numeric.table_name = "numeric";
    numeric.columns = {{"n", DataType::FLOAT, 0, true, std::nullopt}};
    RecordCodec numbers(numeric, RecordLayout::VARIABLE);
    bytes = numbers.encode({3.1415926}); CHECK(bytes.size() == 9);
    CHECK(numbers.decode(bytes.data(), bytes.size()) == Record{3.1415926});
}

void pageTests() {
    VariablePage page(3);
    const auto first = page.insert(std::vector<char>(100, 'a'), SlotKind::RECORD, RID{});
    const auto second = page.insert(std::vector<char>(200, 'b'), SlotKind::RECORD, RID{});
    const auto third = page.insert(std::vector<char>(300, 'c'), SlotKind::RECORD, RID{});
    const auto free = page.insertCapacity();
    page.erase(second);
    CHECK(page.insertCapacity() >= free + 200);
    CHECK(page.slot(third).length == 300 && page.bytes()[page.slot(third).offset] == 'c');
    const auto reused = page.insert(std::vector<char>(50, 'd'), SlotKind::RECORD, RID{});
    CHECK(reused == second);
    page.replace(first, std::vector<char>(1000, 'e'), SlotKind::RECORD, RID{});
    CHECK(page.slot(first).length == 1000 && page.bytes()[page.slot(third).offset] == 'c');
    VariablePage validated(page.bytes());
    CHECK(validated.insertCapacity() == page.insertCapacity());
    auto broken = page.bytes(); broken[8] = static_cast<char>(255);
    expect(RecordError::CORRUPT_FILE, [&] { VariablePage invalid(broken); });

    auto reclaimed = page;
    reclaimed.erase(first); reclaimed.erase(second); reclaimed.erase(third);
    CHECK(VariablePage::slotCount(reclaimed.bytes()) == 0);
    CHECK(reclaimed.insert(std::vector<char>(2048, 'x'), SlotKind::RECORD, RID{}) == 0);
    broken = page.bytes(); broken[bufPageManager::PAGE_SIZE - 16] = 0;
    expect(RecordError::CORRUPT_FILE, [&] { VariablePage invalid(broken); });
    broken = page.bytes(); broken[bufPageManager::PAGE_SIZE - 16 + 4] = static_cast<char>(255);
    expect(RecordError::CORRUPT_FILE, [&] { VariablePage invalid(broken); });

    VariablePage full(4);
    for (std::uint32_t i = 0; i + 1 < VariablePage::MAX_SLOTS; ++i) {
        const auto slot = full.insert({'x'}, SlotKind::RECORD, RID{});
        full.replace(slot, {}, SlotKind::FORWARD, RID{5, i});
    }
    CHECK(full.insertCapacity() == 0);
    expect(RecordError::FILE_TOO_LARGE, [&] { full.insert({'x'}, SlotKind::RECORD, RID{}); });
    VariablePage full_validated(full.bytes());
    CHECK(VariablePage::slotCount(full_validated.bytes()) == VariablePage::MAX_SLOTS - 1);
}

void relocationTests(const fs::path& dir) {
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    const auto path = dir / "relocate.tbl";
    auto file = records.createFile(path.string(), textSchema(), RecordLayout::VARIABLE);
    CHECK(file->layout() == RecordLayout::VARIABLE);
    std::map<RID, Record> model;
    const Record small{std::string("x")};
    auto root = file->insert(small); model[root] = small;
    RID last = root;
    while (last.page == root.page) { last = file->insert(small); model[last] = small; }
    const auto index_id = files.createFile((dir / "variable.idx").string());
    {
        DiskBPlusTree<std::int32_t, RID> index(pages, index_id); index.insert(1, root);
        auto grown = Record{std::string(1000, 'g')};
        file->update(root, grown); model[root] = grown;
        auto forward = inspect(files, pages, path, root.page).slot(root.slot);
        CHECK(forward.kind == SlotKind::FORWARD);
        const auto old_target = forward.link;
        CHECK(file->get(root) == grown);
        CHECK(!file->contains(old_target));
        expect(RecordError::RECORD_NOT_FOUND, [&] { file->get(old_target); });
        expect(RecordError::RECORD_NOT_FOUND, [&] { file->update(old_target, small); });
        expect(RecordError::RECORD_NOT_FOUND, [&] { file->remove(old_target); });
        CHECK(index.search(1).at(0) == root);
        // 填满目标页，迫使下一次增长再次搬迁。
        while (last.page <= old_target.page) { last = file->insert(small); model[last] = small; }
        grown = Record{std::string(2048, 'z')};
        file->update(root, grown); model[root] = grown;
        forward = inspect(files, pages, path, root.page).slot(root.slot);
        CHECK(forward.kind == SlotKind::FORWARD && forward.link != old_target);
        CHECK(emptySlot(inspect(files, pages, path, old_target.page), old_target.slot));
        CHECK(file->get(index.search(1).at(0)) == grown);
        checkScan(file, model); // 转发目标不能被重复扫描。
        file->close(); file = records.openFile(path.string());
        CHECK(file->layout() == RecordLayout::VARIABLE && file->get(root) == grown);
        checkScan(file, model);
        const auto new_target = forward.link;
        file->update(root, small); model[root] = small;
        CHECK(inspect(files, pages, path, root.page).slot(root.slot).kind == SlotKind::RECORD);
        CHECK(emptySlot(inspect(files, pages, path, new_target.page), new_target.slot));
        auto scan = file->scan();
        expect(RecordError::VALUE_TOO_LONG, [&] { file->update(root, {std::string(2049, 'x')}); });
        CHECK(scan.next());
        file->update(root, small);
        expect(RecordError::SCAN_INVALIDATED, [&] { scan.next(); });
        file->update(root, grown); model[root] = grown;
        forward = inspect(files, pages, path, root.page).slot(root.slot);
        CHECK(forward.kind == SlotKind::FORWARD);
        file->remove(root); model.erase(root); CHECK(index.remove(1, root));
        CHECK(emptySlot(inspect(files, pages, path, forward.link.page), forward.link.slot));
        expect(RecordError::RECORD_NOT_FOUND, [&] { file->remove(root); });
        checkScan(file, model);
    }
    pages.discardFile(index_id); files.closeFile(index_id);
    const auto old_pages = file->dataPageCount();
    for (const auto& entry : model) file->remove(entry.first);
    const auto count = model.size(); model.clear();
    file->close(); file = records.openFile(path.string()); CHECK(!file->scan().next());
    for (std::size_t i = 0; i < count; ++i) model[file->insert(small)] = small;
    CHECK(file->dataPageCount() == old_pages);
    checkScan(file, model);
}

void spaceAndDirectoryTests(const fs::path& dir) {
    FileManager files; bufPageManager pages(files, 2); RecordManager records(files, pages);
    auto variable = records.createFile((dir / "space-var.tbl").string(), textSchema(), RecordLayout::VARIABLE);
    auto fixed = records.createFile((dir / "space-fixed.tbl").string(), textSchema());
    for (int i = 0; i < 200; ++i) { variable->insert({std::string("x")}); fixed->insert({std::string("x")}); }
    CHECK(variable->dataPageCount() == 1 && fixed->dataPageCount() == 200);

    const auto path = (dir / "directories.tbl").string();
    auto large = records.createFile(path, textSchema(), RecordLayout::VARIABLE);
    CHECK(large->dataPageCount() == 0 && !large->scan().next());
    const Record maximum{std::string(2048, 'm')};
    std::vector<RID> ids;
    const auto count = detail::DIRECTORY_ENTRIES + 2;
    for (std::size_t i = 0; i < count; ++i) ids.push_back(large->insert(maximum));
    CHECK(large->dataPageCount() == count);
    const auto second_directory = ids[detail::DIRECTORY_ENTRIES].page - 1;
    expect(RecordError::INVALID_RID, [&] { large->get(RID{second_directory, 0}); });
    large->close(); large = records.openFile(path);
    std::size_t scanned = 0; auto scan = large->scan();
    while (auto entry = scan.next()) { CHECK(entry->values == maximum); ++scanned; }
    CHECK(scanned == count);
    large->remove(ids.front()); large->remove(ids[detail::DIRECTORY_ENTRIES]);
    large->close(); large = records.openFile(path);
    const auto first = large->insert(maximum); const auto second = large->insert(maximum);
    CHECK(first == ids.front() && second == ids[detail::DIRECTORY_ENTRIES]);
    CHECK(large->dataPageCount() == count);
}

void randomizedTests(const fs::path& dir) {
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    const auto path = (dir / "random-var.tbl").string();
    auto file = records.createFile(path, rowSchema(), RecordLayout::VARIABLE);
    std::map<RID, Record> model; std::mt19937 random(867713);
    for (int i = 0; i < 500; ++i) { const auto value = row(i, 10); model[file->insert(value)] = value; }
    for (int i = 0; i < 5000; ++i) {
        auto value = row(i + 500, random() % 2001);
        if (i % 11 == 0) value[1] = std::monostate{};
        const auto action = random() % 4;
        if (model.empty() || action == 0) {
            const auto rid = file->insert(value); CHECK(model.emplace(rid, value).second);
        } else {
            auto it = model.begin(); std::advance(it, random() % model.size());
            if (action == 1) { file->remove(it->first); model.erase(it); }
            else { file->update(it->first, value); it->second = value; CHECK(file->get(it->first) == value); }
        }
        if (i % 100 == 0) checkScan(file, model);
        if (i % 500 == 0) { file->close(); file = records.openFile(path); checkScan(file, model); }
    }
    checkScan(file, model);
}

void writeNumber(const fs::path& path, std::size_t offset, std::uint32_t value, std::size_t size) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out); CHECK(file.is_open());
    file.seekp(static_cast<std::streamoff>(offset));
    for (std::size_t i = 0; i < size; ++i) file.put(static_cast<char>((value >> (8 * i)) & 255));
    CHECK(file.good());
}

void corruptionTests(const fs::path& dir) {
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    const auto base = dir / "valid-var.tbl"; const auto damaged = dir / "corrupt-var.tbl";
    auto file = records.createFile(base.string(), textSchema(), RecordLayout::VARIABLE);
    const Record small{std::string("x")}; const auto root = file->insert(small);
    auto last = root;
    while (last.page == root.page) last = file->insert(small);
    file->update(root, {std::string(2048, 'x')});
    const auto target = inspect(files, pages, base, root.page).slot(root.slot).link;
    file->close();
    const auto slot_offset = root.page * bufPageManager::PAGE_SIZE + bufPageManager::PAGE_SIZE - (root.slot + 1) * 16;
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeNumber(damaged, slot_offset + 8, root.page, 4); // 自引用，不能无限跟随。
    file = records.openFile(damaged.string());
    expect(RecordError::CORRUPT_FILE, [&] { file->get(root); }); file->close();
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    const auto target_offset = target.page * bufPageManager::PAGE_SIZE + bufPageManager::PAGE_SIZE - (target.slot + 1) * 16;
    writeNumber(damaged, target_offset + 12, root.slot + 1, 4); // 错误的反向归属。
    file = records.openFile(damaged.string());
    expect(RecordError::CORRUPT_FILE, [&] { file->get(root); }); file->close();
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeNumber(damaged, target_offset, 0, 2); // payload 偏移越界。
    file = records.openFile(damaged.string());
    expect(RecordError::CORRUPT_FILE, [&] { file->get(root); }); file->close();
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeNumber(damaged, 2 * bufPageManager::PAGE_SIZE + 12, 2041, 2); // 页目录计数越界。
    expect(RecordError::CORRUPT_FILE, [&] { records.openFile(damaged.string()); });
    fs::copy_file(base, damaged, fs::copy_options::overwrite_existing);
    writeNumber(damaged, 48, 0, 4); // v2 不得误读为定长格式。
    expect(RecordError::CORRUPT_FILE, [&] { records.openFile(damaged.string()); });
}

void persistence(const std::string& mode, const fs::path& dir) {
    fs::create_directories(dir);
    const auto path = dir / "variable.tbl"; const auto manifest = dir / "variable.txt";
    FileManager files; bufPageManager pages(files, 1); RecordManager records(files, pages);
    if (mode == "cleanup") {
        if (fs::exists(path)) records.removeFile(path.string());
        fs::remove(manifest); return;
    }
    if (mode == "seed") {
        if (fs::exists(path)) records.removeFile(path.string());
        auto file = records.createFile(path.string(), rowSchema(), RecordLayout::VARIABLE);
        std::vector<RID> ids;
        for (int i = 0; i < 600; ++i) ids.push_back(file->insert(row(i, i % 16)));
        file->update(ids[0], row(0, 2000)); file->update(ids[1], row(1, 1000));
        for (int i = 7; i < 600; i += 7) file->remove(ids[i]);
        for (int i = 600; i < 700; ++i) file->insert(row(i, i % 16));
        auto scan = file->scan(); std::ofstream output(manifest);
        while (auto entry = scan.next()) output << entry->rid.page << ' ' << entry->rid.slot << ' '
            << std::get<std::int32_t>(entry->values[0]) << ' ' << std::get<std::string>(entry->values[1]).size() << '\n';
        CHECK(output.good()); return; // 依靠正常析构持久化。
    }
    auto file = records.openFile(path.string()); CHECK(file->layout() == RecordLayout::VARIABLE);
    std::ifstream input(manifest); CHECK(input.is_open());
    std::map<RID, Record> model;
    PageId page; std::uint32_t slot; int id; std::size_t length;
    while (input >> page >> slot >> id >> length) {
        const RID rid{page, slot}; model[rid] = row(id, length); CHECK(file->get(rid) == model[rid]);
    }
    CHECK(model.size() == 615); checkScan(file, model);
    for (auto& [rid, value] : model) if (std::get<std::int32_t>(value[0]) == 0) {
        value = row(0, 1); file->update(rid, value); CHECK(file->get(rid) == value);
    }
    checkScan(file, model);
}

int main(int argc, char** argv) {
    try {
        if (argc == 3) { persistence(argv[1], argv[2]); std::cout << "PASS variable " << argv[1] << '\n'; return 0; }
        const auto dir = fs::temp_directory_path() / ("variable-record-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        CHECK(fs::create_directory(dir));
        struct Cleanup { fs::path dir; ~Cleanup() { std::error_code ec; fs::remove_all(dir, ec); } } cleanup{dir};
        codecTests(); std::cout << "PASS variable codec\n";
        pageTests(); std::cout << "PASS slotted pages\n";
        relocationTests(dir); std::cout << "PASS stable RID and forwarding\n";
        spaceAndDirectoryTests(dir); std::cout << "PASS free-space directory and storage savings\n";
        randomizedTests(dir); std::cout << "PASS randomized variable records\n";
        corruptionTests(dir);
        std::cout << "PASS variable codec, compaction, forwarding, space directories, random model, corrupt files\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
