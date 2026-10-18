// File handling for the shader storage: see shader_storage_files.h.

#include "shader_storage_files.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <fmt/format.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/file.h>
#define MCLA_HAVE_FLOCK 1
#endif
#endif

namespace mcla::shader_storage {

namespace {

std::string Name(const std::filesystem::path& path) { return path.filename().string(); }

#ifdef _WIN32
bool IsInUseError(DWORD error) {
  return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

Failure ErrorFailure(const char* what, const std::filesystem::path& path, DWORD error) {
  if (IsInUseError(error)) {
    return {kInUse, fmt::format("{} is in use by another program", Name(path))};
  }
  return {kIoError, fmt::format("can't {} {} (error {})", what, path.string(), error)};
}

// Closes a handle when it goes out of scope.
struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};
#else
Failure ErrorFailure(const char* what, const std::filesystem::path& path, int error) {
  if (error == EWOULDBLOCK || error == EBUSY || error == ETXTBSY) {
    return {kInUse, fmt::format("{} is in use by another program", Name(path))};
  }
  return {kIoError, fmt::format("can't {} {} ({})", what, path.string(), std::strerror(error))};
}

struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) close(fd);
  }
};

// The folder's own entry flushed, so that a rename survives a power loss.
void SyncFolder(const std::filesystem::path& folder) {
  Fd dir{open(folder.c_str(), O_RDONLY)};
  if (dir.fd >= 0) fsync(dir.fd);
}
#endif

}  // namespace

bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  out.clear();
#ifdef _WIN32
  // Others may read meanwhile, nobody may write.
  Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.h == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw ErrorFailure("open", path, error);
  }
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file.h, &size) || size.QuadPart < 0 || size.QuadPart > (int64_t(1) << 31)) {
    throw ErrorFailure("read", path, GetLastError());
  }
  out.resize(size_t(size.QuadPart));
  size_t done = 0;
  while (done < out.size()) {
    DWORD read = 0;
    const DWORD chunk = DWORD(std::min<size_t>(out.size() - done, size_t(1) << 24));
    if (!::ReadFile(file.h, out.data() + done, chunk, &read, nullptr) || !read) {
      throw ErrorFailure("read", path, read ? GetLastError() : ERROR_HANDLE_EOF);
    }
    done += read;
  }
#else
  Fd file{open(path.c_str(), O_RDONLY)};
  if (file.fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) return false;
    throw ErrorFailure("open", path, errno);
  }
  struct stat st;
  if (fstat(file.fd, &st) != 0 || st.st_size < 0) throw ErrorFailure("read", path, errno);
  out.resize(size_t(st.st_size));
  size_t done = 0;
  while (done < out.size()) {
    const ssize_t read_now = read(file.fd, out.data() + done, out.size() - done);
    if (read_now < 0 && errno == EINTR) continue;
    if (read_now <= 0) throw ErrorFailure("read", path, read_now < 0 ? errno : EIO);
    done += size_t(read_now);
  }
#endif
  return true;
}

int64_t FileSizeOrAbsent(const std::filesystem::path& path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return -1;
    throw ErrorFailure("read", path, error);
  }
  return (int64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
#else
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) return -1;
    throw ErrorFailure("read", path, errno);
  }
  return int64_t(st.st_size);
#endif
}

void WriteFileAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::filesystem::path temp = path;
#ifdef _WIN32
  temp += fmt::format(".{}.tmp", GetCurrentProcessId());
  {
    Handle file{CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.h == INVALID_HANDLE_VALUE) throw ErrorFailure("write", temp, GetLastError());
    size_t done = 0;
    bool ok = true;
    DWORD error = 0;
    while (ok && done < data.size()) {
      DWORD written = 0;
      const DWORD chunk = DWORD(std::min<size_t>(data.size() - done, size_t(1) << 24));
      ok = ::WriteFile(file.h, data.data() + done, chunk, &written, nullptr) && written;
      if (!ok) error = GetLastError();
      done += written;
    }
    if (ok && !FlushFileBuffers(file.h)) {
      ok = false;
      error = GetLastError();
    }
    if (!ok) {
      CloseHandle(file.h);
      file.h = INVALID_HANDLE_VALUE;
      DeleteFileW(temp.c_str());
      throw ErrorFailure("write", temp, error);
    }
  }
  // Another process (an antivirus, a backup, a game of an older version) may have the file open
  // for a moment.
  DWORD error = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return;
    }
    error = GetLastError();
    if (!IsInUseError(error) && error != ERROR_ACCESS_DENIED) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  DeleteFileW(temp.c_str());
  throw ErrorFailure("replace", path, error == ERROR_ACCESS_DENIED ? ERROR_SHARING_VIOLATION : error);
#else
  temp += fmt::format(".{}.tmp", int(getpid()));
  {
    Fd file{open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
    if (file.fd < 0) throw ErrorFailure("write", temp, errno);
    size_t done = 0;
    while (done < data.size()) {
      const ssize_t written = write(file.fd, data.data() + done, data.size() - done);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) {
        const int error = written < 0 ? errno : EIO;
        unlink(temp.c_str());
        throw ErrorFailure("write", temp, error);
      }
      done += size_t(written);
    }
    if (fsync(file.fd) != 0) {
      const int error = errno;
      unlink(temp.c_str());
      throw ErrorFailure("write", temp, error);
    }
  }
  if (rename(temp.c_str(), path.c_str()) != 0) {
    const int error = errno;
    unlink(temp.c_str());
    throw ErrorFailure("replace", path, error);
  }
  SyncFolder(path.parent_path());
#endif
}

void TruncateFile(const std::filesystem::path& path, uint64_t size) {
#ifdef _WIN32
  Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.h == INVALID_HANDLE_VALUE) throw ErrorFailure("cut", path, GetLastError());
  LARGE_INTEGER position;
  position.QuadPart = int64_t(size);
  if (!SetFilePointerEx(file.h, position, nullptr, FILE_BEGIN) || !SetEndOfFile(file.h) ||
      !FlushFileBuffers(file.h)) {
    throw ErrorFailure("cut", path, GetLastError());
  }
#else
  Fd file{open(path.c_str(), O_WRONLY)};
  if (file.fd < 0 || ftruncate(file.fd, off_t(size)) != 0 || fsync(file.fd) != 0) {
    throw ErrorFailure("cut", path, errno);
  }
#endif
}

void RemoveFile(const std::filesystem::path& path) {
#ifdef _WIN32
  if (!DeleteFileW(path.c_str())) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
    throw ErrorFailure("remove", path, error);
  }
#else
  if (unlink(path.c_str()) != 0 && errno != ENOENT) throw ErrorFailure("remove", path, errno);
  SyncFolder(path.parent_path());
#endif
}

void RemoveStaleTemporaries(const std::filesystem::path& folder,
                            const std::vector<std::string>& names) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    const std::string file = entry.path().filename().string();
    for (const std::string& name : names) {
      if (file.size() <= name.size() + 5 || file.compare(0, name.size() + 1, name + ".") != 0 ||
          file.compare(file.size() - 4, 4, ".tmp") != 0) {
        continue;
      }
      const std::string middle = file.substr(name.size() + 1, file.size() - name.size() - 5);
      const bool digits =
          !middle.empty() && middle.find_first_not_of("0123456789") == std::string::npos;
      if (digits || middle == "prepare") {
        std::error_code remove_error;
        std::filesystem::remove(entry.path(), remove_error);
      }
    }
  }
}

namespace {
bool g_session_lock_held = false;
}

bool AcquireSessionLock(const std::filesystem::path& lock_path, const std::string& owner,
                        uint32_t wait_ms, bool (*keep_waiting)(const std::string& holder),
                        std::string* holder) {
  if (g_session_lock_held) return true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
#ifdef _WIN32
  // Kept open and locked until the process ends; the system releases it then, also after a
  // crash. The lock is on a byte far past the owner text, which stays readable.
  HANDLE file = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (holder) *holder = fmt::format("(the lock file can't be opened: error {})", GetLastError());
    return false;
  }
  for (;;) {
    OVERLAPPED overlapped = {};
    overlapped.Offset = 0x7FFFFFFF;
    if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &overlapped)) {
      DWORD written = 0;
      SetFilePointer(file, 0, nullptr, FILE_BEGIN);
      SetEndOfFile(file);
      WriteFile(file, owner.data(), DWORD(owner.size()), &written, nullptr);
      g_session_lock_held = true;
      return true;
    }
    char text[128] = {};
    DWORD read = 0;
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    ::ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    const std::string other(text, read);
    if (holder) *holder = other;
    if (std::chrono::steady_clock::now() >= deadline || !keep_waiting || !keep_waiting(other)) {
      CloseHandle(file);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#elif defined(MCLA_HAVE_FLOCK)
  const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    if (holder) *holder = fmt::format("(the lock file can't be opened: {})", std::strerror(errno));
    return false;
  }
  for (;;) {
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
      if (ftruncate(fd, 0) == 0) {
        [[maybe_unused]] ssize_t written = pwrite(fd, owner.data(), owner.size(), 0);
      }
      g_session_lock_held = true;
      return true;
    }
    char text[128] = {};
    const ssize_t read_now = pread(fd, text, sizeof(text) - 1, 0);
    const std::string other(text, read_now > 0 ? size_t(read_now) : 0);
    if (holder) *holder = other;
    if (std::chrono::steady_clock::now() >= deadline || !keep_waiting || !keep_waiting(other)) {
      close(fd);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#else
  // No advisory locks on this platform: a single process uses the storage (the PS5's title).
  (void)lock_path;
  (void)owner;
  (void)deadline;
  (void)keep_waiting;
  (void)holder;
  g_session_lock_held = true;
  return true;
#endif
}

bool SessionLockHeld() { return g_session_lock_held; }

int64_t ProcessUptimeMs() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user, now;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  GetSystemTimeAsFileTime(&now);
  const auto ticks = [](const FILETIME& t) {
    return (int64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return (ticks(now) - ticks(created)) / 10000;
#else
  return 0;
#endif
}

std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += fmt::format("\\u{:04x}", unsigned(static_cast<unsigned char>(c)));
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

std::string DisplayDriversJson() {
  std::string out = "[";
#ifdef _WIN32
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc;
      LARGE_INTEGER umd{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                            nullptr);
        out += fmt::format(
            "{}{{\"name\":{},\"vendor\":{},\"device\":{},\"subsys\":{},\"driver\":\"{}.{}.{}.{}\"}}",
            out.size() > 1 ? "," : "", JsonString(name), desc.VendorId, desc.DeviceId,
            desc.SubSysId, uint32_t(umd.HighPart) >> 16, uint32_t(umd.HighPart) & 0xFFFF,
            uint32_t(umd.LowPart) >> 16, uint32_t(umd.LowPart) & 0xFFFF);
      }
      adapter->Release();
    }
    factory->Release();
  }
#endif
  return out + "]";
}

}  // namespace mcla::shader_storage
