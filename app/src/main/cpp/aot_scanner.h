/**
 * aot_scanner.h - small toolkit for locating code in a Dart AOT snapshot at
 * runtime.
 *
 * libapp.so is an ELF image whose executable PT_LOAD segment holds the compiled
 * Dart code and whose read-only segments hold the snapshot's object pool. There
 * are no Dart symbols, so the only portable way to find a function is to scan
 * for a byte or instruction pattern and then verify its surroundings.
 *
 * Everything here is header-only and allocation-light; the only heap use is the
 * std::vector returned by the scan helpers. A full scan of a 30 MB .text is a
 * few tens of milliseconds, so resolve each target once, at first use.
 *
 * AArch64 facts this file relies on:
 *   - every instruction is 4 bytes and 4-byte aligned;
 *   - `bl`/`b` encode a signed 26-bit word offset relative to the instruction;
 *   - `adrp` + `add` is the usual way to materialise a static address;
 *   - Dart's AOT ABI keeps the object pool in x27 (PP), so a constant - a
 *     string, an IconData, a bool, a double - is loaded with
 *     `ldr xN, [x27, #pool_offset]`, which makes that instruction a precise
 *     cross-reference from code to snapshot object. (x26 is THR and x28 is
 *     HEAP_BITS in the same ABI; x28 is *not* PP.)
 */

#pragma once

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace aot {

/** A runtime view of one ELF segment (or any other contiguous byte range). */
struct Span {
    std::uintptr_t address = 0;      // runtime address of data[0]
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;

    bool Valid() const { return data != nullptr && size != 0; }
};

/** Result of FindImage(): load bias plus the program header table. */
struct Image {
    std::uintptr_t base = 0;
    const ElfW(Phdr) *phdrs = nullptr;
    std::size_t phnum = 0;

    bool Valid() const { return base != 0 && phdrs != nullptr && phnum != 0; }
};

namespace detail {

struct ImageSearch {
    std::string_view soname;
    Image *result;
};

inline int FindImageCallback(dl_phdr_info *info, std::size_t /*size*/, void *data) {
    auto *search = static_cast<ImageSearch *>(data);
    if (info == nullptr || info->dlpi_name == nullptr) {
        return 0;
    }
    const std::string_view name(info->dlpi_name);
    if (name.size() < search->soname.size()) {
        return 0;
    }
    if (name.compare(name.size() - search->soname.size(), search->soname.size(),
                     search->soname) != 0) {
        return 0;
    }
    search->result->base = static_cast<std::uintptr_t>(info->dlpi_addr);
    search->result->phdrs = info->dlpi_phdr;
    search->result->phnum = static_cast<std::size_t>(info->dlpi_phnum);
    return 1;  // stop iterating
}

}  // namespace detail

/**
 * Fills |out| with the first loaded image whose path ends in |soname|, e.g.
 * "libapp.so". Returns false when the image is not mapped.
 */
inline bool FindImage(std::string_view soname, Image *out) {
    if (out == nullptr) {
        return false;
    }
    *out = Image{};
    detail::ImageSearch search{soname, out};
    dl_iterate_phdr(&detail::FindImageCallback, &search);
    return out->Valid();
}

/**
 * Returns the first PT_LOAD segment whose protection contains all of |flags|.
 * Use PF_X for code, PF_R (and check the result) for the snapshot's read-only
 * data. |size| is the file-backed size, so a scan never walks into .bss.
 */
inline Span FindSegment(const Image &image, std::uint32_t flags,
                        std::uint32_t type = PT_LOAD) {
    Span span{};
    if (!image.Valid()) {
        return span;
    }
    for (std::size_t i = 0; i < image.phnum; ++i) {
        const ElfW(Phdr) &phdr = image.phdrs[i];
        if (phdr.p_type != type || (phdr.p_flags & flags) != flags ||
            phdr.p_filesz == 0) {
            continue;
        }
        span.address = image.base + static_cast<std::uintptr_t>(phdr.p_vaddr);
        span.data = reinterpret_cast<const std::uint8_t *>(span.address);
        span.size = static_cast<std::size_t>(phdr.p_filesz);
        break;
    }
    return span;
}

/**
 * True when the runtime range [|address|, |address| + |size|) is backed by one
 * of the image's PT_LOAD segments. |p_memsz| is used because the snapshot's
 * read-only data (including its object pool) may extend past |p_filesz| in the
 * mapped image. This is the range check every hook uses before dereferencing a
 * candidate Dart object.
 */
inline bool ContainsAddress(const Image &image, std::uintptr_t address,
                            std::size_t size = 1) {
    if (!image.Valid() || size == 0) {
        return false;
    }
    const std::uint64_t begin = address;
    const std::uint64_t end = begin + size;
    if (end < begin) {
        return false;  // overflow
    }
    for (std::size_t i = 0; i < image.phnum; ++i) {
        const ElfW(Phdr) &phdr = image.phdrs[i];
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        const std::uint64_t segment_begin =
            static_cast<std::uint64_t>(image.base) + phdr.p_vaddr;
        const std::uint64_t segment_end = segment_begin + phdr.p_memsz;
        if (begin >= segment_begin && end <= segment_end) {
            return true;
        }
    }
    return false;
}

/** Reads one instruction, or 0 when |offset| is out of range. */
inline std::uint32_t LoadInstruction(const Span &span, std::size_t offset) {
    if (span.data == nullptr || offset > span.size ||
        span.size - offset < sizeof(std::uint32_t)) {
        return 0;
    }
    std::uint32_t insn = 0;
    std::memcpy(&insn, span.data + offset, sizeof(insn));
    return insn;
}

/**
 * A masked byte pattern. mask[i] == 'x' matches bytes[i] exactly, '?' matches
 * any byte. The mask array includes the terminating NUL, so PatternOf() can
 * check its length at compile time.
 */
struct Pattern {
    const std::uint8_t *bytes;
    const char *mask;
    std::size_t size;
};

template <std::size_t N>
constexpr Pattern PatternOf(const std::uint8_t (&bytes)[N],
                            const char (&mask)[N + 1]) {
    return Pattern{bytes, mask, N};
}

/** All (up to |max_hits|) start addresses of |pattern| inside |span|. */
inline std::vector<std::uintptr_t> Scan(const Span &span, const Pattern &pattern,
                                        std::size_t max_hits = 64) {
    std::vector<std::uintptr_t> hits;
    if (!span.Valid() || pattern.bytes == nullptr || pattern.mask == nullptr ||
        pattern.size == 0 || pattern.size > span.size) {
        return hits;
    }
    hits.reserve(max_hits < 16 ? max_hits : 16);

    const std::uint8_t *const begin = span.data;
    const std::uint8_t *const last = span.data + (span.size - pattern.size);
    for (const std::uint8_t *cursor = begin; cursor <= last; ++cursor) {
        std::size_t i = 0;
        for (; i < pattern.size; ++i) {
            if (pattern.mask[i] == '?' || pattern.mask[i] == '*') {
                continue;
            }
            if (cursor[i] != pattern.bytes[i]) {
                break;
            }
        }
        if (i != pattern.size) {
            continue;
        }
        hits.push_back(span.address + static_cast<std::uintptr_t>(cursor - begin));
        if (hits.size() >= max_hits) {
            break;
        }
    }
    return hits;
}

/**
 * Scans one instruction stream. The common use is
 *   ScanInstruction(text, 0xFC000000u, 0x94000000u)  // every `bl`
 *   ScanInstruction(text, 0xFFC003E0u, 0xF9400360u)  // every `ldr xN,[x27,#imm]`
 */
inline std::vector<std::uintptr_t> ScanInstruction(const Span &span,
                                                   std::uint32_t mask,
                                                   std::uint32_t value,
                                                   std::size_t max_hits = 64) {
    std::vector<std::uintptr_t> hits;
    if (!span.Valid() || span.size < sizeof(std::uint32_t)) {
        return hits;
    }
    hits.reserve(max_hits < 16 ? max_hits : 16);
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= span.size;
         offset += sizeof(std::uint32_t)) {
        const std::uint32_t insn = LoadInstruction(span, offset);
        if ((insn & mask) != value) {
            continue;
        }
        hits.push_back(span.address + static_cast<std::uintptr_t>(offset));
        if (hits.size() >= max_hits) {
            break;
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// AArch64 decoders. Only the handful of forms Dart's code generator emits.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kRet = 0xD65F03C0u;  // ret x30
constexpr std::uint32_t kNop = 0xD503201Fu;

/** `bl` (opcode 100101, 26-bit signed word offset). */
constexpr bool IsBranchLink(std::uint32_t insn) {
    return (insn & 0xFC000000u) == 0x94000000u;
}

/** Unconditional `b` (opcode 000101, same immediate layout as `bl`). */
constexpr bool IsBranch(std::uint32_t insn) {
    return (insn & 0xFC000000u) == 0x14000000u;
}

/** `b.cond` (opcode 0101010, 19-bit signed word offset plus a condition). */
constexpr bool IsConditionalBranch(std::uint32_t insn) {
    return (insn & 0xFF000010u) == 0x54000000u;
}

/** Target of `b`/`bl`. |pc| is the address of the branch instruction. */
inline std::uintptr_t DecodeBranchTarget(std::uint32_t insn, std::uintptr_t pc) {
    std::int64_t imm = static_cast<std::int64_t>(insn & 0x03FFFFFFu);
    if ((imm & 0x02000000) != 0) {
        imm -= 0x04000000;  // sign-extend 26 bits
    }
    return pc + static_cast<std::uintptr_t>(imm * 4);
}

/** Target of `b.cond`. */
inline std::uintptr_t DecodeConditionalBranchTarget(std::uint32_t insn,
                                                    std::uintptr_t pc) {
    std::int64_t imm = static_cast<std::int64_t>((insn >> 5) & 0x7FFFFu);
    if ((imm & 0x00040000) != 0) {
        imm -= 0x00080000;  // sign-extend 19 bits
    }
    return pc + static_cast<std::uintptr_t>(imm * 4);
}

/** `adrp xD, label` (opcode 1xx10000, page-relative 21-bit immediate). */
constexpr bool IsAdrp(std::uint32_t insn) {
    return (insn & 0x9F000000u) == 0x90000000u;
}

inline std::uintptr_t DecodeAdrp(std::uint32_t insn, std::uintptr_t pc) {
    std::int64_t imm = static_cast<std::int64_t>(
        (((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 0x3u));
    if ((imm & (1 << 20)) != 0) {
        imm -= (1 << 21);  // sign-extend 21 bits
    }
    const std::uintptr_t page = pc & ~static_cast<std::uintptr_t>(0xFFFu);
    return page + static_cast<std::uintptr_t>(imm << 12);
}

/** `add xD, xN, #imm` (optionally shifted left by 12). */
constexpr bool IsAddImmediate64(std::uint32_t insn) {
    return (insn & 0xFF800000u) == 0x91000000u;
}

constexpr std::uint32_t DecodeAddImmediate(std::uint32_t insn) {
    std::uint32_t imm = (insn >> 10) & 0xFFFu;
    if ((insn & (1u << 22)) != 0) {
        imm <<= 12;
    }
    return imm;
}

constexpr std::uint32_t RegisterField(std::uint32_t insn, unsigned shift) {
    return (insn >> shift) & 0x1Fu;
}

/**
 * `ldr xN, [xPP, #imm]` - LLVM emits this for every Dart object-pool access,
 * i.e. for every string, const widget, IconData, bool and double a function
 * uses. imm12 is the pool byte offset divided by 8 and Rt is free.
 *
 * PP is x27 from Dart 2.19 through current Dart 3.x. It is *not* x28: the same
 * ABI uses x26 for THR and x28 for HEAP_BITS (the write-barrier mask/base).
 * These constants let callers override the base if they ever meet a different
 * ABI.
 */
constexpr std::uint32_t kDartPpRegister = 27;
constexpr std::uint32_t kDartThrRegister = 26;
constexpr std::uint32_t kDartHeapBitsRegister = 28;

constexpr bool IsLoadFromPoolReg(std::uint32_t insn, std::uint32_t reg) {
    return (insn & 0xFFC003E0u) == (0xF9400000u | ((reg & 0x1Fu) << 5));
}

constexpr bool IsLoadFromPool(std::uint32_t insn) {
    return IsLoadFromPoolReg(insn, kDartPpRegister);
}

/** Base register (Rn) of any `ldr xN, [xM, #imm]` load. */
constexpr std::uint32_t DecodeLoadBaseRegister(std::uint32_t insn) {
    return (insn >> 5) & 0x1Fu;
}

constexpr std::uint32_t DecodePoolOffset(std::uint32_t insn) {
    return ((insn >> 10) & 0xFFFu) * 8u;
}

// Encoding guard: `ldr x0, [x27, #0]` is 0xF9400360, while the old x28 form is
// 0xF9400380. Keeping both here makes a register mix-up a compile error.
static_assert(IsLoadFromPool(0xF9400360u));
static_assert(!IsLoadFromPool(0xF9400380u));
static_assert(DecodeLoadBaseRegister(0xF9400360u) == kDartPpRegister);

/** `ret` (x30). Used to reject hooks that would overwrite a neighbouring function. */
constexpr bool IsReturn(std::uint32_t insn) {
    return insn == 0xD65F03C0u;
}

/**
 * `stp x29, x30, [sp, #-N]!` - the frame setup LLVM puts at the entry of any
 * Dart function that is not a leaf. Scanning backwards for it turns an
 * instruction hit inside a function into a hookable entry address.
 */
constexpr bool IsFrameSetup(std::uint32_t insn) {
    return (insn & 0xFFC07FFFu) == 0xA9807BFDu;
}

/** Nearest frame setup at or before |address|, or 0 if none was found. */
inline std::uintptr_t FindFunctionStartBefore(const Span &span,
                                              std::uintptr_t address,
                                              std::size_t max_distance = 0x1000) {
    if (!span.Valid() || address < span.address || address >= span.address + span.size) {
        return 0;
    }
    std::size_t offset = static_cast<std::size_t>(address - span.address) & ~std::size_t{3};
    const std::size_t limit = offset > max_distance ? offset - max_distance : 0;
    while (offset >= limit) {
        if (IsFrameSetup(LoadInstruction(span, offset))) {
            return span.address + static_cast<std::uintptr_t>(offset);
        }
        if (offset < sizeof(std::uint32_t)) {
            break;
        }
        offset -= sizeof(std::uint32_t);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Cross references from code to code and from code to the object pool.
// ---------------------------------------------------------------------------

/** Every `bl` in |text| whose target is |target| (call sites). */
inline std::vector<std::uintptr_t> FindBranchLinkCallers(
    const Span &text, std::uintptr_t target, std::size_t max_hits = 64) {
    std::vector<std::uintptr_t> hits;
    if (!text.Valid()) {
        return hits;
    }
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= text.size;
         offset += sizeof(std::uint32_t)) {
        const std::uint32_t insn = LoadInstruction(text, offset);
        if (!IsBranchLink(insn)) {
            continue;
        }
        const std::uintptr_t pc = text.address + static_cast<std::uintptr_t>(offset);
        if (DecodeBranchTarget(insn, pc) != target) {
            continue;
        }
        hits.push_back(pc);
        if (hits.size() >= max_hits) {
            break;
        }
    }
    return hits;
}

/** Every `bl` or `b` to |target|. Useful once a build helper is known. */
inline std::vector<std::uintptr_t> FindBranchCallers(
    const Span &text, std::uintptr_t target, std::size_t max_hits = 64) {
    std::vector<std::uintptr_t> hits;
    if (!text.Valid()) {
        return hits;
    }
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= text.size;
         offset += sizeof(std::uint32_t)) {
        const std::uint32_t insn = LoadInstruction(text, offset);
        if (!IsBranchLink(insn) && !IsBranch(insn)) {
            continue;
        }
        const std::uintptr_t pc = text.address + static_cast<std::uintptr_t>(offset);
        if (DecodeBranchTarget(insn, pc) != target) {
            continue;
        }
        hits.push_back(pc);
        if (hits.size() >= max_hits) {
            break;
        }
    }
    return hits;
}

/**
 * Every `ldr xN, [x27, #pool_offset]`. |pool_offset| is the byte offset encoded
 * in the instruction (the `#imm` IDA shows), not the pool index. The returned
 * address is the load instruction; FindFunctionStartBefore() then gives the
 * containing Dart function.
 */
inline std::vector<std::uintptr_t> FindPoolReferences(
    const Span &text, std::uint32_t pool_offset, std::size_t max_hits = 64) {
    std::vector<std::uintptr_t> hits;
    if (!text.Valid() || (pool_offset % 8u) != 0 || (pool_offset / 8u) > 0xFFFu) {
        return hits;
    }
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= text.size;
         offset += sizeof(std::uint32_t)) {
        const std::uint32_t insn = LoadInstruction(text, offset);
        if (!IsLoadFromPool(insn) || DecodePoolOffset(insn) != pool_offset) {
            continue;
        }
        hits.push_back(text.address + static_cast<std::uintptr_t>(offset));
        if (hits.size() >= max_hits) {
            break;
        }
    }
    return hits;
}

/**
 * Every `adrp xD, page; add xD, xD, #off` pair whose result is |target|. This
 * catches direct references to snapshot/static data that do not go through the
 * Dart object pool (rare in Dart code, common in the engine glue around it).
 */
inline std::vector<std::uintptr_t> FindAdrpAddReferences(
    const Span &span, std::uintptr_t target, std::size_t max_hits = 64) {
    std::vector<std::uintptr_t> hits;
    if (!span.Valid()) {
        return hits;
    }
    for (std::size_t offset = 0; offset + 2 * sizeof(std::uint32_t) <= span.size;
         offset += sizeof(std::uint32_t)) {
        const std::uint32_t adrp = LoadInstruction(span, offset);
        if (!IsAdrp(adrp)) {
            continue;
        }
        const std::uint32_t add = LoadInstruction(span, offset + sizeof(std::uint32_t));
        if (!IsAddImmediate64(add)) {
            continue;
        }
        const std::uint32_t adrp_rd = RegisterField(adrp, 0);
        if (RegisterField(add, 5) != adrp_rd || RegisterField(add, 0) != adrp_rd) {
            continue;
        }
        const std::uintptr_t pc = span.address + static_cast<std::uintptr_t>(offset);
        if (DecodeAdrp(adrp, pc) + DecodeAddImmediate(add) != target) {
            continue;
        }
        hits.push_back(pc);
        if (hits.size() >= max_hits) {
            break;
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Patching. Prefer DobbyHook() from dart_hooks.cpp for whole-function hooks;
// these helpers are for turning a conditional branch into an unconditional one,
// or for neutralising a single instruction when a whole-function hook would be
// too coarse.
// ---------------------------------------------------------------------------

/** Encodes `b <to>` placed at |from| (both must be within +/-128 MB). */
inline std::uint32_t EncodeUnconditionalBranch(std::uintptr_t from,
                                               std::uintptr_t to) {
    const std::int64_t delta =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    return 0x14000000u | (static_cast<std::uint32_t>(delta >> 2) & 0x03FFFFFFu);
}

/**
 * Overwrites one instruction. libapp.so's .text is mapped read-only, so the
 * page is switched to RWX for the store and back to RX afterwards; the icache
 * is flushed for the touched word. Returns false if the store was rejected.
 */
inline bool PatchInstruction(std::uintptr_t address, std::uint32_t insn) {
    if ((address % sizeof(std::uint32_t)) != 0) {
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return false;
    }
    const std::uintptr_t page_mask = static_cast<std::uintptr_t>(page_size) - 1;
    void *const page = reinterpret_cast<void *>(address & ~page_mask);
    const std::size_t length = static_cast<std::size_t>(page_size);
    if (mprotect(page, length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(reinterpret_cast<void *>(address), &insn, sizeof(insn));
    __builtin___clear_cache(reinterpret_cast<char *>(address),
                            reinterpret_cast<char *>(address + sizeof(insn)));
    mprotect(page, length, PROT_READ | PROT_EXEC);
    return true;
}

}  // namespace aot
