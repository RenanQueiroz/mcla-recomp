// File handling for the shader storage (src/shader_prepare.cpp): reads that never mistake an
// unreadable file for a missing one, replacement by an atomic rename, the session lock every mcla
// process holds on the storage, and the display drivers (for the storage's stamp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::shader_storage {

// A failure, with the code a preparation run (--mcla_prepare_shaders) ends with.
struct Failure {
  int code;
  std::string message;
};
constexpr int kNoPack = 2;         // no shader pack
constexpr int kBadPack = 3;        // a damaged pack, or one this game can't read
constexpr int kOtherGame = 4;      // the game's files aren't the ones the pack was made for
constexpr int kOtherVersions = 5;  // the pack is for other storage versions
constexpr int kIoError = 6;        // the storage can't be read or written (or another error)
constexpr int kInUse = 7;          // the storage is in use by another process: try again later

// Reads a whole file. False when it doesn't exist. A file that exists but can't be read throws:
// kInUse while another process holds it, kIoError otherwise.
bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out);
// The size of a file, or -1 when it doesn't exist; throws as ReadWholeFile.
int64_t FileSizeOrAbsent(const std::filesystem::path& path);

// Writes data beside path (<path>.<process id>.tmp), flushes it to the disk and renames it over
// path, retrying for a moment while another process has the file open. On a failure the
// temporary file is removed and kInUse or kIoError thrown, path left as it was.
void WriteFileAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& data);
// Cuts a file to size, flushed (the rollback of an import); throws kInUse or kIoError.
void TruncateFile(const std::filesystem::path& path, uint64_t size);
// Removes a file; one that doesn't exist is fine. Throws kInUse or kIoError.
void RemoveFile(const std::filesystem::path& path);
// Removes what WriteFileAtomically of another process left behind when it ended mid-write:
// <one of names>.<digits>.tmp (and the fixed name of 2026-10-17, <name>.prepare.tmp).
void RemoveStaleTemporaries(const std::filesystem::path& folder,
                            const std::vector<std::string>& names);

// The storage's session lock: an advisory lock on a file beside the storage, which every mcla
// process that uses the storage takes before reading it and holds until it ends, so that a
// preparation never runs beside a game and two starts never merge at once. owner ("game <pid>",
// "prepare <pid>") is written into the file for the others to read. While another process holds
// it, waits up to wait_ms as long as keep_waiting(its owner text) says so; false if not taken
// (holder: the other's owner text).
bool AcquireSessionLock(const std::filesystem::path& lock_path, const std::string& owner,
                        uint32_t wait_ms, bool (*keep_waiting)(const std::string& holder),
                        std::string* holder);
bool SessionLockHeld();

// This process's run time so far, in milliseconds (0 where unknown).
int64_t ProcessUptimeMs();

// The hardware display adapters and their user-mode driver versions, as a JSON array (Windows;
// "[]" elsewhere): what the launcher compares to notice a driver update.
std::string DisplayDriversJson();

// text as a JSON string literal.
std::string JsonString(std::string_view text);

}  // namespace mcla::shader_storage
