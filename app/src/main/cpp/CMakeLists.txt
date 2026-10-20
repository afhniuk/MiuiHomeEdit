cmake_minimum_required(VERSION 3.22.1)

project(miuihome_edit LANGUAGES CXX)

# Produces libmiuihome_edit.so. The name is part of the contract: it must match
# META-INF/xposed/native_init.list and System.loadLibrary("miuihome_edit").
add_library(
    miuihome_edit
    SHARED
    main.cpp
    module_config.cpp
    dart_hooks.cpp
    diag.cpp
)

target_compile_features(miuihome_edit PRIVATE cxx_std_17)

target_compile_options(
    miuihome_edit
    PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    # The module has no C++ exceptions or RTTI to carry, and the NDK's static
    # libc++ would otherwise link both in.
    -fno-exceptions
    -fno-rtti
    # Only native_init (annotated in main.cpp) is exported.
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# Dart AOT on arm64 keeps live values in registers that the AAPCS treats as
# ordinary callee-saved registers - or, for x15, as scratch. The C++
# replacements must not allocate the callee-saved ones, or the pool load in
# dart_hooks.cpp would see a corrupted dispatch table / null object / THR / PP /
# heap-bits value:
#   x21 dispatch table, x22 NullObject, x24 code, x26 THR, x27 PP, x28 HEAP_BITS
# x15 (Dart's stack pointer) cannot be protected this way: -ffixed-x15 would bind
# only this library, while mincore()/__android_log_print() are free to destroy
# it. The asm shims in dart_hooks.cpp therefore save and restore it themselves.
if (ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_options(
        miuihome_edit
        PRIVATE
        -ffixed-x21
        -ffixed-x22
        -ffixed-x24
        -ffixed-x26
        -ffixed-x27
        -ffixed-x28
    )
endif()

# Android 15+ devices may run 16 KB pages; NDK r28+ already defaults to this,
# but state it explicitly so the requirement survives a toolchain change.
target_link_options(
    miuihome_edit
    PRIVATE
    "-Wl,-z,max-page-size=16384"
    # libc++ is linked statically (see ANDROID_STL in build.gradle.kts). Without
    # this, every std:: symbol would be re-exported from the module and could
    # interpose on the launcher's own runtime once the .so is dlopen()ed.
    "-Wl,--exclude-libs,ALL"
)

# liblog for __android_log_print; libdl for dlsym/dl_iterate_phdr.
target_link_libraries(miuihome_edit PRIVATE log dl)

# Dobby (Apache-2.0, https://github.com/jmpews/Dobby) is vendored under
# third_party/dobby at a pinned commit. It provides the arm64 inline hook and
# its trampoline allocator. Link the static target so the APK still contains
# exactly one native library.
#
# Dobby's upstream ARM32 backend does not compile with NDK r30, and the launcher
# is 64-bit, so the dependency - and the hooks - exist only for arm64-v8a.
# dart_hooks.cpp compiles to a one-line stub on other ABIs.
if (ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_definitions(miuihome_edit PRIVATE MIUIHOME_DART_HOOKS=1)

    # Plugin.SymbolResolver is off on purpose: it pulls in dlopen/dlsym
    # machinery, and Install() calls DobbyHook() from the framework's dlopen()
    # callback, where a nested dlopen() would re-enter the registry mutex.
    set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
    set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(DOBBY_BUILD_KERNEL_MODE OFF CACHE BOOL "" FORCE)
    set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
    set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
    set(NearBranch ON CACHE BOOL "" FORCE)
    set(FullFloatingPointRegisterPack OFF CACHE BOOL "" FORCE)

    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/dobby
        ${CMAKE_CURRENT_BINARY_DIR}/dobby
    )

    # Dobby's CMakeLists creates both a shared and a static target. Only the
    # static one is linked; keep libdobby.so out of the APK.
    if (TARGET dobby)
        set_target_properties(dobby PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    target_include_directories(
        miuihome_edit
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/dobby/include
    )
    target_link_libraries(miuihome_edit PRIVATE dobby_static)
else()
    target_compile_definitions(miuihome_edit PRIVATE MIUIHOME_DART_HOOKS=0)
endif()
