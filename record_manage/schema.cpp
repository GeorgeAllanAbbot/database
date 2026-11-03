#include "schema.h"
#include "../common/config.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace db {
namespace {

[[noreturn]] void invalidSchema(const std::string& message) {
    throw RecordException(RecordError::INVALID_SCHEMA, message);
}

void checkName(const std::string& name, std::size_t limit, const char* kind) {
    if (name.empty() || name.size() > limit || name.find('\0') != std::string::npos)
        invalidSchema(std::string("invalid ") + kind + " name");
}

std::size_t dataWidth(const Column& column) {
    switch (column.type) {
    case DataType::INT: return 4;
    case DataType::FLOAT: return 8;
    case DataType::VARCHAR: return column.max_length;
    }
    invalidSchema("unknown column type");
}

std::size_t storedWidth(const Column& column) {
    return dataWidth(column) + (column.type == DataType::VARCHAR ? 2 : 0);
}

void validateValue(const Column& column, const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        if (!column.nullable)
            throw RecordException(RecordError::NULL_NOT_ALLOWED, "NULL not allowed: " + column.name);
        return;
    }
    const bool matches =
        (column.type == DataType::INT && std::holds_alternative<std::int32_t>(value)) ||
        (column.type == DataType::FLOAT && std::holds_alternative<double>(value)) ||
        (column.type == DataType::VARCHAR && std::holds_alternative<std::string>(value));
    if (!matches)
        throw RecordException(RecordError::TYPE_MISMATCH, "value type mismatch: " + column.name);
    if (column.type == DataType::VARCHAR && std::get<std::string>(value).size() > column.max_length)
        throw RecordException(RecordError::VALUE_TOO_LONG, "string exceeds column length: " + column.name);
    if (column.type == DataType::FLOAT && !std::isfinite(std::get<double>(value)))
        throw RecordException(RecordError::TYPE_MISMATCH, "FLOAT must be finite: " + column.name);
}

void putUnsigned(char* target, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) target[i] = static_cast<char>((value >> (8 * i)) & 255);
}

std::uint64_t getUnsigned(const char* source, std::size_t size) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < size; ++i)
        result |= std::uint64_t(static_cast<unsigned char>(source[i])) << (8 * i);
    return result;
}

} // namespace

std::size_t Schema::columnIndex(const std::string& name) const {
    for (std::size_t i = 0; i < columns.size(); ++i)
        if (columns[i].name == name) return i;
    invalidSchema("unknown column: " + name);
}

void Schema::validate() const {
    checkName(table_name, limits::MAX_TABLE_NAME_BYTES, "table");
    if (columns.empty() || columns.size() > limits::MAX_COLUMNS_PER_TABLE)
        invalidSchema("column count must be between 1 and 10");
    std::set<std::string> names;
    std::size_t total_width = 0;
    for (const auto& column : columns) {
        checkName(column.name, limits::MAX_COLUMN_NAME_BYTES, "column");
        if (!names.insert(column.name).second) invalidSchema("duplicate column: " + column.name);
        if (column.type == DataType::VARCHAR) {
            if (!column.max_length || column.max_length > limits::MAX_COLUMN_BYTES)
                invalidSchema("VARCHAR length must be between 1 and 2048");
        } else if (column.max_length != 0) {
            invalidSchema("numeric columns must have max_length = 0");
        }
        total_width += dataWidth(column);
        if (column.default_value) {
            try { validateValue(column, *column.default_value); }
            catch (const RecordException& e) { invalidSchema(std::string("invalid default: ") + e.what()); }
        }
    }
    if (total_width > limits::MAX_RECORD_DATA_BYTES)
        invalidSchema("sum of column data widths exceeds 2048 bytes");

    auto checkColumns = [&](const std::vector<std::string>& list) {
        if (list.empty() || list.size() > columns.size()) invalidSchema("invalid constraint columns");
        std::set<std::string> used;
        for (const auto& name : list) {
            columnIndex(name);
            if (!used.insert(name).second) invalidSchema("duplicate constraint column: " + name);
        }
    };
    std::set<std::string> constraint_names;
    auto checkConstraintName = [&](const std::string& name, bool optional) {
        if (optional && name.empty()) return;
        checkName(name, limits::MAX_TABLE_NAME_BYTES, "constraint");
        if (!constraint_names.insert(name).second) invalidSchema("duplicate constraint name: " + name);
    };
    if (primary_key) {
        checkConstraintName(primary_key->name, true);
        checkColumns(primary_key->columns);
        for (const auto& name : primary_key->columns) {
            const auto& column = columns[columnIndex(name)];
            if (column.nullable) invalidSchema("primary key column must be NOT NULL: " + name);
        }
    }
    for (const auto& foreign : foreign_keys) {
        checkConstraintName(foreign.name, false);
        checkColumns(foreign.columns);
        checkName(foreign.referenced_table, limits::MAX_TABLE_NAME_BYTES, "referenced table");
        if (foreign.columns.size() != foreign.referenced_columns.size())
            invalidSchema("foreign key column counts differ");
        std::set<std::string> referenced;
        for (const auto& name : foreign.referenced_columns) {
            checkName(name, limits::MAX_COLUMN_NAME_BYTES, "referenced column");
            if (!referenced.insert(name).second) invalidSchema("duplicate referenced column");
        }
    }
    for (const auto& unique : unique_constraints) {
        checkConstraintName(unique.name, false);
        checkColumns(unique.columns);
    }
}

std::size_t Schema::recordSize() const {
    validate();
    std::size_t size = (columns.size() + 7) / 8;
    for (const auto& column : columns) size += storedWidth(column);
    return size;
}

RecordCodec::RecordCodec(Schema schema, RecordLayout layout)
    : schema_(std::move(schema)), record_size_(schema_.recordSize()), minimum_size_(record_size_), layout_(layout) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                  "FLOAT requires IEEE 754 binary64");
    if (layout != RecordLayout::FIXED && layout != RecordLayout::VARIABLE) invalidSchema("unknown record layout");
    if (layout == RecordLayout::VARIABLE)
        for (const auto& column : schema_.columns)
            if (column.type == DataType::VARCHAR) minimum_size_ -= column.max_length;
}

std::vector<char> RecordCodec::encode(const Record& record) const {
    if (record.size() != schema_.columns.size())
        throw RecordException(RecordError::TYPE_MISMATCH, "record column count does not match schema");
    if (layout_ == RecordLayout::VARIABLE) return encodeVariable(record);
    std::vector<char> bytes(record_size_, 0);
    std::size_t offset = (record.size() + 7) / 8;
    for (std::size_t i = 0; i < record.size(); ++i) {
        const auto& column = schema_.columns[i];
        const auto& value = record[i];
        validateValue(column, value);
        if (std::holds_alternative<std::monostate>(value)) {
            bytes[i / 8] = static_cast<char>(static_cast<unsigned char>(bytes[i / 8]) | (1U << (i % 8)));
        } else if (column.type == DataType::INT) {
            putUnsigned(bytes.data() + offset, static_cast<std::uint32_t>(std::get<std::int32_t>(value)), 4);
        } else if (column.type == DataType::FLOAT) {
            std::uint64_t bits;
            const double number = std::get<double>(value);
            std::memcpy(&bits, &number, sizeof(bits));
            putUnsigned(bytes.data() + offset, bits, 8);
        } else {
            const auto& string = std::get<std::string>(value);
            putUnsigned(bytes.data() + offset, string.size(), 2);
            std::memcpy(bytes.data() + offset + 2, string.data(), string.size());
        }
        offset += storedWidth(column);
    }
    return bytes;
}

Record RecordCodec::decode(const char* bytes, std::size_t size) const {
    if (layout_ == RecordLayout::VARIABLE) return decodeVariable(bytes, size);
    if (!bytes || size != record_size_)
        throw RecordException(RecordError::CORRUPT_FILE, "invalid record byte length");
    Record record;
    record.reserve(schema_.columns.size());
    std::size_t offset = (schema_.columns.size() + 7) / 8;
    for (std::size_t i = 0; i < schema_.columns.size(); ++i) {
        const auto& column = schema_.columns[i];
        if (static_cast<unsigned char>(bytes[i / 8]) & (1U << (i % 8))) {
            record.emplace_back(std::monostate{});
        } else if (column.type == DataType::INT) {
            const auto bits = static_cast<std::uint32_t>(getUnsigned(bytes + offset, 4));
            std::int32_t number;
            std::memcpy(&number, &bits, 4);
            record.emplace_back(number);
        } else if (column.type == DataType::FLOAT) {
            const auto bits = getUnsigned(bytes + offset, 8);
            double number;
            std::memcpy(&number, &bits, 8);
            record.emplace_back(number);
        } else {
            const auto length = getUnsigned(bytes + offset, 2);
            if (length > column.max_length)
                throw RecordException(RecordError::CORRUPT_FILE, "stored string length exceeds column width");
            record.emplace_back(std::string(bytes + offset + 2, static_cast<std::size_t>(length)));
        }
        try { validateValue(column, record.back()); }
        catch (const RecordException& e) {
            throw RecordException(RecordError::CORRUPT_FILE, std::string("invalid stored value: ") + e.what());
        }
        offset += storedWidth(column);
    }
    return record;
}

std::vector<char> RecordCodec::encodeVariable(const Record& record) const {
    std::size_t size = minimum_size_;
    for (std::size_t i = 0; i < record.size(); ++i) {
        validateValue(schema_.columns[i], record[i]);
        if (const auto* string = std::get_if<std::string>(&record[i])) size += string->size();
    }
    std::vector<char> bytes(size, 0);
    std::size_t fixed = (record.size() + 7) / 8;
    std::size_t offsets = fixed;
    for (const auto& column : schema_.columns)
        if (column.type != DataType::VARCHAR) offsets += dataWidth(column);
    std::size_t payload = minimum_size_;
    for (std::size_t i = 0; i < record.size(); ++i) {
        const auto& column = schema_.columns[i];
        const auto& value = record[i];
        const bool null = std::holds_alternative<std::monostate>(value);
        if (null) bytes[i / 8] = static_cast<char>(static_cast<unsigned char>(bytes[i / 8]) | (1U << (i % 8)));
        if (column.type == DataType::VARCHAR) {
            if (!null) {
                const auto& string = std::get<std::string>(value);
                std::memcpy(bytes.data() + payload, string.data(), string.size());
                payload += string.size();
            }
            putUnsigned(bytes.data() + offsets, payload, 2);
            offsets += 2;
        } else {
            if (!null && column.type == DataType::INT) {
                putUnsigned(bytes.data() + fixed, static_cast<std::uint32_t>(std::get<std::int32_t>(value)), 4);
            } else if (!null) {
                const auto number = std::get<double>(value);
                std::uint64_t bits;
                std::memcpy(&bits, &number, 8);
                putUnsigned(bytes.data() + fixed, bits, 8);
            }
            fixed += dataWidth(column);
        }
    }
    return bytes;
}

Record RecordCodec::decodeVariable(const char* bytes, std::size_t size) const {
    if (!bytes || size < minimum_size_ || size > record_size_)
        throw RecordException(RecordError::CORRUPT_FILE, "invalid variable record byte length");
    Record record(schema_.columns.size());
    std::size_t fixed = (record.size() + 7) / 8;
    if (record.size() % 8 && (static_cast<unsigned char>(bytes[fixed - 1]) >> (record.size() % 8)))
        throw RecordException(RecordError::CORRUPT_FILE, "invalid unused NULL bits");
    std::size_t offsets = fixed;
    for (const auto& column : schema_.columns)
        if (column.type != DataType::VARCHAR) offsets += dataWidth(column);
    std::size_t payload = minimum_size_;
    for (std::size_t i = 0; i < record.size(); ++i) {
        const auto& column = schema_.columns[i];
        const bool null = (static_cast<unsigned char>(bytes[i / 8]) & (1U << (i % 8))) != 0;
        if (column.type == DataType::VARCHAR) {
            const auto end = static_cast<std::size_t>(getUnsigned(bytes + offsets, 2));
            if (end < payload || end > size || end - payload > column.max_length || (null && end != payload))
                throw RecordException(RecordError::CORRUPT_FILE, "invalid variable column end offset");
            if (!null) record[i] = std::string(bytes + payload, end - payload);
            payload = end;
            offsets += 2;
        } else {
            if (!null && column.type == DataType::INT) {
                const auto bits = static_cast<std::uint32_t>(getUnsigned(bytes + fixed, 4));
                std::int32_t number;
                std::memcpy(&number, &bits, 4);
                record[i] = number;
            } else if (!null) {
                const auto bits = getUnsigned(bytes + fixed, 8);
                double number;
                std::memcpy(&number, &bits, 8);
                record[i] = number;
            }
            fixed += dataWidth(column);
        }
        try { validateValue(column, record[i]); }
        catch (const RecordException& e) {
            throw RecordException(RecordError::CORRUPT_FILE, std::string("invalid stored value: ") + e.what());
        }
    }
    if (payload != size)
        throw RecordException(RecordError::CORRUPT_FILE, "unexpected variable record trailing bytes");
    return record;
}

} // namespace db
