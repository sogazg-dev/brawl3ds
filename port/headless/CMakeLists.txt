# brawl3ds headless timing build: Brawl Recompiled's runtime + a selection of translated code, no renderer.
#
# Inputs (cache variables):
#   WIICOMPILED_DIR  Brawl Recompiled's wiicompiled/ tree, patched with tools/patch_wiicompiled_arm32.py and
#                    tools/patch_brawlrecomp_rev2.py, with generated/ from a PC translation of the user's disc
#   SHARD_DIR        translated shards selected by tools/select_translated_code.py
#   GEN3DS_DIR       data_sections_init_3ds.cpp (tools/gen_data_init_3ds.py) and link_stubs.cpp
#                    (tools/gen_link_stubs.py)
#   SDL3_DIR         SDL3 built for the 3DS with cmake/3ds-toolchain.cmake (its install prefix)
#
# Configure:
#   cmake -G Ninja -S port/headless -B <build> -DCMAKE_TOOLCHAIN_FILE=cmake/3ds-toolchain.cmake
#         -DBRAWL3DS_CXX_CLANG=<clang++> -DWIICOMPILED_DIR=... -DSHARD_DIR=... -DGEN3DS_DIR=... -DSDL3_DIR=...
cmake_minimum_required(VERSION 3.20)
project(brawl3ds_headless C CXX ASM)

foreach(var WIICOMPILED_DIR SHARD_DIR GEN3DS_DIR SDL3_DIR)
  if(NOT ${var})
    message(FATAL_ERROR "${var} is not set (see the header of this file)")
  endif()
endforeach()

set(W "${WIICOMPILED_DIR}")
set(RT "${W}/runtime")
set(PORT "${CMAKE_CURRENT_LIST_DIR}/../wiicompiled_arm32")

set(BRAWL3DS_DEFINES TARGET_PC NDEBUG CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE SDL_MAIN_HANDLED)
set(BRAWL3DS_INCLUDES
  "${RT}/include" "${RT}/src" "${W}" "${W}/aurora-main/include"
  "${RT}/third_party/toml11" "${RT}/third_party/pugixml" "${RT}/third_party/fatfs" "${RT}/third_party"
  "${W}/Dependencies/xxhash" "${W}/Dependencies/imgui" "${SDL3_DIR}/include")
# The PC build's numeric settings: no fast-math, no FP contraction (results must match the PowerPC's rounding).
set(BRAWL3DS_FP_FLAGS -fno-fast-math -ffp-contract=off)
# The ARM11 allows unaligned word loads/stores (ldr/str/ldrh/strh; not ldrd/ldm), and the 3DS kernel leaves that
# enabled. Without this clang assumes strict alignment and turns every inlined guest access into byte loads/stores.
set(BRAWL3DS_ALIGN_FLAGS -munaligned-access)

function(brawl3ds_target target)
  target_compile_definitions(${target} PRIVATE ${BRAWL3DS_DEFINES})
  target_include_directories(${target} PRIVATE ${BRAWL3DS_INCLUDES})
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${BRAWL3DS_FP_FLAGS} ${BRAWL3DS_ALIGN_FLAGS} -w>)
endfunction()

# --- third-party libraries (the same source sets as Brawl Recompiled's runtime/CMakeLists.txt) ---
file(GLOB CRYPTOPP_SOURCES "${RT}/third_party/cryptopp/*.cpp")
list(FILTER CRYPTOPP_SOURCES EXCLUDE REGEX
  "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
list(FILTER CRYPTOPP_SOURCES EXCLUDE REGEX "(_avx|_ppc|_simd|_sse)\\.cpp$")
add_library(b3_cryptopp STATIC ${CRYPTOPP_SOURCES})
brawl3ds_target(b3_cryptopp)
target_compile_options(b3_cryptopp PRIVATE -Os -std=gnu++17)

add_library(b3_pugixml STATIC "${RT}/third_party/pugixml/pugixml.cpp")
brawl3ds_target(b3_pugixml)
target_compile_options(b3_pugixml PRIVATE -Os -std=gnu++17)

add_library(b3_fatfs STATIC "${RT}/third_party/fatfs/ff.c" "${RT}/third_party/fatfs/ffunicode.c")
target_include_directories(b3_fatfs PRIVATE "${RT}/third_party/fatfs")
target_compile_options(b3_fatfs PRIVATE -Os -w)

add_library(b3_xxhash STATIC "${W}/Dependencies/xxhash/xxhash.c")
target_compile_options(b3_xxhash PRIVATE -O2 -w)

file(GLOB IMGUI_SOURCES "${W}/Dependencies/imgui/imgui.cpp" "${W}/Dependencies/imgui/imgui_draw.cpp"
  "${W}/Dependencies/imgui/imgui_tables.cpp" "${W}/Dependencies/imgui/imgui_widgets.cpp")
add_library(b3_imgui STATIC ${IMGUI_SOURCES})
brawl3ds_target(b3_imgui)
target_compile_options(b3_imgui PRIVATE -Os -std=gnu++20)

# --- runtime ---
file(GLOB_RECURSE RUNTIME_SOURCES "${RT}/src/*.cpp")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/(android|macos)/")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX
  "/(discord_presence|guest_flat_memory|guest_flat_memory_macos|external_audio_macos)\\.cpp$")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/netplay/net_socket\\.cpp$|/product/engine_loader\\.cpp$|/hle/net/network_ssl\\.cpp$")
# One product per binary: Brawl is the base product (retro_rewind_product.cpp is Mario Kart Wii's Retro Rewind).
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/product/retro_rewind_product\\.cpp$")
add_library(b3_runtime STATIC ${RUNTIME_SOURCES} "${RT}/src/platform/3ds/co_switch.S")
brawl3ds_target(b3_runtime)
target_compile_options(b3_runtime PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-O2;-std=gnu++20;-include;${RT}/include/mkw_pch.h>")

# --- generated (translator output that is not translated code) ---
file(GLOB GENERATED_DISPATCH "${W}/generated/build_shards/base_dispatch/*.cpp"
  "${W}/generated/build_shards/base_registration/*.cpp")
add_library(b3_generated STATIC ${GENERATED_DISPATCH} "${W}/generated/guest_symbol_table.cpp"
  "${GEN3DS_DIR}/data_sections_init_3ds.cpp")
brawl3ds_target(b3_generated)
target_compile_options(b3_generated PRIVATE -Os -std=gnu++20 -include "${RT}/include/mkw_pch.h")

# --- translated game code (the selected shards; -Oz keeps the VS set at ~55 MiB) ---
file(GLOB SHARD_SOURCES "${SHARD_DIR}/shard_*.cpp")
add_library(b3_shards STATIC ${SHARD_SOURCES})
brawl3ds_target(b3_shards)
target_include_directories(b3_shards PRIVATE "${SHARD_DIR}")
target_compile_options(b3_shards PRIVATE -Oz -std=gnu++17 -include "${RT}/include/mkw_pch.h")
# The shards holding the functions a real-hardware profile found hot (tools/hot_shards.py shards
# port/headless/hot_functions.txt <SHARD_DIR> <SHARD_DIR>/hot_shards.txt) inline every guest load and store; the rest
# call one shared copy of the same fast path (memory_access.h), which keeps the code small enough to boot.
set(HOT_SHARDS_LIST "${SHARD_DIR}/hot_shards.txt")
if(EXISTS "${HOT_SHARDS_LIST}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HOT_SHARDS_LIST}")
  file(STRINGS "${HOT_SHARDS_LIST}" HOT_SHARDS)
  list(TRANSFORM HOT_SHARDS PREPEND "${SHARD_DIR}/")
  # -O2 after the target's -Oz: -Oz runs clang's machine outliner, which turns repeated instruction runs in the
  # hot loops into calls; -O2 costs about 30% more code in these shards only.
  set_source_files_properties(${HOT_SHARDS} PROPERTIES COMPILE_DEFINITIONS BRAWL3DS_INLINE_GUEST_ACCESS=1
    COMPILE_OPTIONS -O2)
  list(LENGTH HOT_SHARDS HOT_SHARD_COUNT)
  message(STATUS "brawl3ds: ${HOT_SHARD_COUNT} hot shards inline guest memory accesses")
endif()

# --- the program ---
add_executable(brawl3ds "${PORT}/main_3ds.cpp" "${PORT}/aurora_headless_3ds.cpp"
  "${PORT}/posix_and_aurora_extras_3ds.cpp" "${PORT}/alloc_tracker_3ds.cpp" "${PORT}/fast_tree_3ds.cpp" "${PORT}/profiler_3ds.cpp"
  "${GEN3DS_DIR}/link_stubs.cpp")
brawl3ds_target(brawl3ds)
target_compile_options(brawl3ds PRIVATE -O2 -std=gnu++20 -include "${RT}/include/mkw_pch.h")
set_target_properties(brawl3ds PROPERTIES SUFFIX ".elf")
# Link with devkitARM's g++ driver: it knows 3dsx.specs (crt0, linker script) and the multilib for armv6k/fpu.
set(DKA_GXX "${DEVKITARM}/bin/arm-none-eabi-g++.exe")
# No <FLAGS> here: those are clang's compile flags (--target, -nostdinc++), which g++ does not take. The toolchain's
# CMAKE_EXE_LINKER_FLAGS_INIT (in <LINK_FLAGS>) already carries the arch flags and -specs=3dsx.specs.
set(CMAKE_CXX_LINK_EXECUTABLE
  "\"${DKA_GXX}\" <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
target_link_options(brawl3ds PRIVATE "-Wl,-Map,${CMAKE_BINARY_DIR}/brawl3ds.map" -Wl,--gc-sections
  # Live-allocation tracker (alloc_tracker_3ds.cpp).
  -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -Wl,--wrap=_Znwj -Wl,--wrap=_Znaj -Wl,--wrap=rename)
# The runtime registers its HLE functions and the translator's function tables through static initializers that
# nothing references by name, so those archives must be linked whole (the PC build links them as object libraries).
target_link_libraries(brawl3ds PRIVATE
  -Wl,--whole-archive b3_shards b3_generated b3_runtime -Wl,--no-whole-archive
  -Wl,--start-group b3_cryptopp b3_pugixml b3_fatfs b3_xxhash b3_imgui
  "${SDL3_DIR}/lib/libSDL3.a" -Wl,--end-group
  "-L${CTR_ROOT}/lib" -lctru -lm)

add_custom_command(TARGET brawl3ds POST_BUILD
  COMMAND "${DEVKITPRO}/tools/bin/3dsxtool.exe" "$<TARGET_FILE:brawl3ds>" "${CMAKE_BINARY_DIR}/brawl3ds.3dsx"
  COMMENT "3dsxtool -> brawl3ds.3dsx")
