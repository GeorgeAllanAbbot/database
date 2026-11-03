#pragma once

#include "../common/errors.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace db {

class RecordException : public std::runtime_error {
public:
    RecordException(RecordError code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    RecordError code() const noexcept { return code_; }
private:
    RecordError code_;
};

enum class DataType : std::uint8_t { INT = 1, FLOAT = 2, VARCHAR = 3 };
using Value = std::variant<std::monostate, std::int32_t, double, std::string>;
using Record = std::vector<Value>;

struct Column {
    std::string name;
    DataType type;
    std::uint32_t max_length = 0; // VARCHAR 的最大字节数；数值列填 0。
    bool nullable = true;
    std::optional<Value> default_value;
};

struct PrimaryKey {
    std::string name; // 可为空。
    std::vector<std::string> columns;
};

struct ForeignKey {
    std::string name;
    std::vector<std::string> columns;
    std::string referenced_table;
    std::vector<std::string> referenced_columns;
};

struct UniqueConstraint {
    std::string name;
    std::vector<std::string> columns;
};

struct Schema {
    std::string table_name;
    std::vector<Column> columns;
    std::optional<PrimaryKey> primary_key;
    std::vector<ForeignKey> foreign_keys;
    std::vector<UniqueConstraint> unique_constraints;

    void validate() const;
    std::size_t columnIndex(const std::string& name) const;
    std::size_t recordSize() const;
};

// 固定宽度编码：NULL 位图 + 各列。VARCHAR 用 uint16 长度和补零的数据区。
// 所有数值按小端编码；FLOAT 使用 IEEE 754 binary64。
class RecordCodec {
public:
    explicit RecordCodec(Schema schema);
    const Schema& schema() const noexcept { return schema_; }
    std::size_t recordSize() const noexcept { return record_size_; }
    std::vector<char> encode(const Record& record) const;
    Record decode(const char* bytes, std::size_t size) const;
private:
    Schema schema_;
    std::size_t record_size_;
};

} // namespace db
