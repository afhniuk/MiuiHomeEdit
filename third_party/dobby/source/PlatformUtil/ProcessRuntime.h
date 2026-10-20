#pragma once

#include "MemoryAllocator/MemoryAllocator.h"
#include "PlatformUnifiedInterface/platform.h"

struct RuntimeModule {
  // Darwin fills base; Linux fills load_address. Keep both names for the
  // built-in plugins that have not been unified yet.
  void *base;
  void *load_address;
  char path[1024];
};

#define MEM_PERM_R 0x1
#define MEM_PERM_W 0x2
#define MEM_PERM_X 0x4
struct MemRegion : MemRange {
  int perm;

  MemRegion(addr_t addr, size_t size, int perm) : MemRange(addr, size), perm(perm) {
  }
};

class ProcessRuntime {
public:
  static const stl::vector<MemRegion> &getMemoryLayout();

  static const stl::vector<RuntimeModule> &getModuleMap();

  static RuntimeModule getModule(const char *name);
};