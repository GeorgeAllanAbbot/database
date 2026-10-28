#include "file_manage.h"
#include <filesystem>
#include <limits>

namespace fs = std::filesystem;

FileException::FileException(FileError code, const std::string& message)
    : std::runtime_error(message), code_(code) {}
FileError FileException::code() const noexcept { return code_; }
FileManager::~FileManager() { closeAll(); }

FileId FileManager::allocateId() {
    if (files_.size() >= std::numeric_limits<FileId>::max() - 1ULL)
        throw FileException(FileError::FILE_INVALID_ID, "no file identifiers available");
    while (next_file_id_ == 0 || files_.count(next_file_id_)) ++next_file_id_;
    return next_file_id_++;
}

FileId FileManager::createFile(const std::string& path) {
    if (path.empty()) throw FileException(FileError::FILE_INVALID_ARGUMENT, "filename must not be empty");
    if (fs::exists(path)) throw FileException(FileError::FILE_ALREADY_EXISTS, "file already exists: " + path);
    std::ofstream creator(path, std::ios::binary | std::ios::out);
    if (!creator) throw FileException(FileError::FILE_CREATE_ERROR, "cannot create file: " + path);
    creator.close();
    return openFile(path);
}

FileId FileManager::openFile(const std::string& path) {
    if (path.empty()) throw FileException(FileError::FILE_INVALID_ARGUMENT, "filename must not be empty");
    auto old = path_to_id_.find(path);
    if (old != path_to_id_.end()) return old->second;
    if (!fs::exists(path) || !fs::is_regular_file(path))
        throw FileException(FileError::FILE_NOT_FOUND, "file not found: " + path);
    auto file = std::make_unique<OpenedFile>();
    file->path = path;
    file->stream.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file->stream.is_open()) throw FileException(FileError::FILE_OPEN_ERROR, "cannot open file: " + path);
    const FileId id = allocateId();
    files_.emplace(id, std::move(file));
    path_to_id_.emplace(path, id);
    return id;
}

std::string FileManager::closeFile(const std::string& path) {
    auto it = path_to_id_.find(path);
    if (it == path_to_id_.end()) throw FileException(FileError::FILE_NOT_FOUND, "file is not open: " + path);
    closeFile(it->second);
    return path;
}

void FileManager::closeFile(FileId id) {
    OpenedFile& file = checkedFile(id);
    const std::string path = file.path;
    file.stream.flush();
    file.stream.close();
    if (file.stream.fail()) {
        file.stream.clear();
        throw FileException(FileError::FILE_CLOSE_ERROR, "cannot close file: " + path);
    }
    path_to_id_.erase(path);
    files_.erase(id);
}

void FileManager::closeAll() noexcept {
    for (auto& item : files_) { item.second->stream.flush(); item.second->stream.close(); }
    files_.clear(); path_to_id_.clear();
}

bool FileManager::isOpen(FileId id) const noexcept { return files_.count(id) != 0; }
FileId FileManager::idFor(const std::string& path) const {
    auto it = path_to_id_.find(path);
    if (it == path_to_id_.end()) throw FileException(FileError::FILE_NOT_FOUND, "file is not open: " + path);
    return it->second;
}
const std::string& FileManager::filename(FileId id) const { return checkedFile(id).path; }
FileManager::OpenedFile& FileManager::checkedFile(FileId id) {
    auto it = files_.find(id);
    if (it == files_.end()) throw FileException(FileError::FILE_INVALID_ID, "invalid file id");
    return *it->second;
}
const FileManager::OpenedFile& FileManager::checkedFile(FileId id) const {
    auto it = files_.find(id);
    if (it == files_.end()) throw FileException(FileError::FILE_INVALID_ID, "invalid file id");
    return *it->second;
}

std::uint64_t FileManager::fileSize(FileId id) {
    OpenedFile& file = checkedFile(id);
    file.stream.clear(); file.stream.seekg(0, std::ios::end);
    const std::streampos pos = file.stream.tellg();
    if (pos < 0) throw FileException(FileError::FILE_SEEK_ERROR, "cannot determine file size");
    return static_cast<std::uint64_t>(pos);
}

void FileManager::read(FileId id, std::uint64_t offset, char* data, std::size_t size) {
    if (!data && size) throw FileException(FileError::FILE_INVALID_ARGUMENT, "read buffer must not be null");
    if (!size) return;
    OpenedFile& file = checkedFile(id);
    file.stream.clear(); file.stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file.stream) throw FileException(FileError::FILE_SEEK_ERROR, "file read seek failed");
    file.stream.read(data, static_cast<std::streamsize>(size));
    if (static_cast<std::size_t>(file.stream.gcount()) != size) {
        file.stream.clear();
        throw FileException(FileError::FILE_SHORT_READ, "requested bytes extend past end of file");
    }
}

void FileManager::write(FileId id, std::uint64_t offset, const char* data, std::size_t size) {
    if (!data && size) throw FileException(FileError::FILE_INVALID_ARGUMENT, "write buffer must not be null");
    if (!size) return;
    OpenedFile& file = checkedFile(id);
    file.stream.clear(); file.stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file.stream) throw FileException(FileError::FILE_SEEK_ERROR, "file write seek failed");
    file.stream.write(data, static_cast<std::streamsize>(size));
    if (!file.stream) throw FileException(FileError::FILE_WRITE_ERROR, "file write failed");
}

void FileManager::flush(FileId id) {
    OpenedFile& file = checkedFile(id); file.stream.flush();
    if (!file.stream) throw FileException(FileError::FILE_WRITE_ERROR, "file flush failed");
}

void FileManager::removeFile(const std::string& path) {
    if (path_to_id_.count(path)) closeFile(path);
    std::error_code error;
    if (!fs::remove(path, error) || error)
        throw FileException(FileError::FILE_REMOVE_ERROR, "cannot remove file: " + path);
}
