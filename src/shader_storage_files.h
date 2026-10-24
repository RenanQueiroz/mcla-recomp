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
constexpr int kIoError = 6;        // the storage can't be read or written (or another error): a
                                   // read-only file, a permission that denies the replacement, a
                                   // lock file that can't be opened
constexpr int kInUse = 7;          // the storage is in use by another process (a sharing or lock
                                   // violation, a file open elsewhere without delete sharing or
                                   // mapped, the session lock held): try again later

// Reads a whole file. False when it doesn't exist. A file that exists but can't be read throws:
// kInUse while another process holds it (a sharing or lock violation), kIoError otherwise.
bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out);
// The size of a file, or -1 when it doesn't exist; throws as ReadWholeFile.
int64_t FileSizeOrAbsent(const std::filesystem::path& path);

// Writes data beside path (<path>.<process id>.tmp), flushes it to the disk and renames it over
// path, retrying for a moment while another process has the file open. On a failure the
// temporary file is removed and path left as it was; kInUse is thrown for a sharing or lock
// violation, and for access denied when it is the file being open elsewhere without delete
// sharing (or mapped) that causes it; kIoError for anything else, a read-only file and a refused
// permission included: those don't pass by waiting.
void WriteFileAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& data);
// Cuts a file to size, flushed (the rollback of an import); throws kInUse or kIoError.
void TruncateFile(const std::filesystem::path& path, uint64_t size);
// Removes a file; one that doesn't exist is fine. Throws kInUse or kIoError.
void RemoveFile(const std::filesystem::path& path);
// Removes what WriteFileAtomically of another process left behind when it ended mid-write:
// <one of names>.<digits>.tmp (and the fixed name of 2026-10-17, <name>.prepare.tmp). The names are
// complete file names ("545407F8.xsh"), as WriteFileAtomically gets them.
void RemoveStaleTemporaries(const std::filesystem::path& folder,
                            const std::vector<std::string>& names);

// The storage's session lock: an advisory lock on a file beside the storage, which every mcla
// process that uses the storage takes before reading it and holds until it ends, so that a
// preparation never runs beside a game and two starts never merge at once. owner ("game <pid>",
// "prepare <pid>") is written into the file for the others to read. While another process holds
// it, waits up to wait_ms as long as keep_waiting(its owner text) says so.
enum class LockResult {
  kTaken,
  kHeld,   // another process holds it (holder: its owner text); try again later
  kError,  // the lock file can't be opened or locked (holder: why); waiting won't help
};
// (holder: the other's owner text, or the reason for kError.) On a platform without advisory locks
// (the PS5's title, the only process there that uses the storage) it takes nothing and succeeds.
LockResult AcquireSessionLock(const std::filesystem::path& lock_path, const std::string& owner,
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
