#pragma once

#include <cstddef>
#include <cstdint>

namespace db::limits {

// 所有表的数据总量必须严格小于此上限；单位为字节（1 GiB）。
inline constexpr std::uint64_t MAX_TOTAL_DATA_BYTES = 1ULL << 30;

// 数据库中允许创建的表数量上限。
inline constexpr std::size_t MAX_TABLE_COUNT = 8;

// 单张表最多允许的列数。
inline constexpr std::size_t MAX_COLUMNS_PER_TABLE = 10;

// 单列数据的最大长度，单位为字节。
inline constexpr std::size_t MAX_COLUMN_BYTES = 2048;

// 基础记录格式要求所有列的最大数据长度之和不超过 2048 字节。
inline constexpr std::size_t MAX_RECORD_DATA_BYTES = 2048;

// 表名和列名的最大长度，单位为字节，不包含字符串结束符。
inline constexpr std::size_t MAX_TABLE_NAME_BYTES = 32;
inline constexpr std::size_t MAX_COLUMN_NAME_BYTES = 32;

} // namespace db::limits
