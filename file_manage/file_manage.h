#pragma once
#include "../common/errors.h"
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

// 这两个 ID 都是磁盘格式和缓冲池接口的一部分，统一使用相同的
// 无符号 32 位表示，避免 FileId/PageId 与裸 int 混用。
using FileId = std::uint32_t;
using PageId = std::uint32_t;

class FileException : public std::runtime_error {
public:
    FileException(FileError code, const std::string& message);
    FileError code() const noexcept;
private:
    FileError code_;
};

class FileManager {
public:
    FileManager() = default;
    ~FileManager();
    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;

    FileId createFile(const std::string& filename);
    FileId openFile(const std::string& filename);
    std::string closeFile(const std::string& filename);
    void closeFile(FileId id);
    void closeAll() noexcept;
    bool isOpen(FileId id) const noexcept;
    FileId idFor(const std::string& filename) const;
    const std::string& filename(FileId id) const;
    std::uint64_t fileSize(FileId id);
    void read(FileId id, std::uint64_t offset, char* data, std::size_t size);
    void write(FileId id, std::uint64_t offset, const char* data, std::size_t size);
    void flush(FileId id);
    void removeFile(const std::string& filename);
private:
    struct OpenedFile { std::string path; std::fstream stream; };
    std::unordered_map<FileId, std::unique_ptr<OpenedFile>> files_;
    std::unordered_map<std::string, FileId> path_to_id_;
    FileId next_file_id_ = 1;
    OpenedFile& checkedFile(FileId id);
    const OpenedFile& checkedFile(FileId id) const;
    FileId allocateId();
};
