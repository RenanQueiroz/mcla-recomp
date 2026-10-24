// Calls into rexruntime.dll (the memory and string helpers the game's host code uses at start),
// so that the import library, the headers and the DLL (with libc++.dll and libunwind.dll beside
// it) are exercised together. Prints "link test ok" and exits 0 when they work.
#include <cstdio>
#include <string>

#include <rex/memory/utils.h>
#include <rex/string.h>
#include <rex/version.h>

int main() {
  const size_t page = rex::memory::page_size();
  if (page != 4096) {
    std::printf("page_size() returned %zu\n", page);
    return 1;
  }
  const std::string text = "  rexglue " REXGLUE_VERSION_STRING "  ";
  if (rex::string::trim(text) != "rexglue " REXGLUE_VERSION_STRING) {
    std::printf("trim() returned a surprise\n");
    return 2;
  }
  std::printf("link test ok: page %zu, %s\n", page, REXGLUE_VERSION_STRING);
  return 0;
}
