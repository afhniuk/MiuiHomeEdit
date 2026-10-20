#pragma once

#include <sys/types.h>
#include <stddef.h>
#include "pac_kit.h"

#if defined(__arm64e__) && __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

namespace features {

template <typename T> inline T arm_thumb_fix_addr(T &addr) {
#if defined(__arm__) || defined(__aarch64__)
  addr = (T)((uintptr_t)addr & ~1);
#endif
  return addr;
}

namespace apple {
template <typename T> inline T arm64e_pac_strip(T &addr) {
  return pac_strip(addr);
}

template <typename T> inline T arm64e_pac_sign(T &addr) {
  return pac_sign(addr);
}

template <typename T> inline T arm64e_pac_strip_and_sign(T &addr) {
  return pac_strip_and_sign(addr);
}
} // namespace apple

namespace android {
// Defined in platform-posix.cc. It cannot live inline here: this header is
// reached through dobby/common.h -> platform.h -> dobby/common.h, so OSMemory
// is not declared yet when the compiler first reads this file.
void make_memory_readable(void *address, size_t size);
} // namespace android
} // namespace features