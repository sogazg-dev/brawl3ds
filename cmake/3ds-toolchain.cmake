# CMake toolchain for the Nintendo 3DS (ARM11 MPCore, ARMv6K + VFPv2) that works with a Windows-native CMake.
#
# devkitPro's own cmake/3DS.cmake refuses to run outside its msys2 shell, so this file sets up the same compiler
# and flags directly (they mirror devkitPro's Platform/Nintendo3DS.cmake). C goes through devkitARM's gcc; C++ can
# use either devkitARM's g++ (default) or clang with devkitARM's headers (-DBRAWL3DS_CXX_CLANG=<path to clang++>),
# which the translated Brawl code needs (it uses clang extensions).
#
# Usage: cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/3ds-toolchain.cmake [-DDEVKITPRO=C:/devkitPro] ...

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv6k)
set(NINTENDO_3DS TRUE)  # SDL3 (and others) switch their 3DS backends on with this

if(NOT DEVKITPRO)
  if(DEFINED ENV{DEVKITPRO} AND NOT "$ENV{DEVKITPRO}" MATCHES "^/")
    set(DEVKITPRO "$ENV{DEVKITPRO}")
  else()
    set(DEVKITPRO "C:/devkitPro")
  endif()
endif()
set(DEVKITPRO "${DEVKITPRO}" CACHE PATH "devkitPro root")
set(DEVKITARM "${DEVKITPRO}/devkitARM")
set(CTR_ROOT "${DEVKITPRO}/libctru")

set(CMAKE_C_COMPILER "${DEVKITARM}/bin/arm-none-eabi-gcc.exe")
set(CMAKE_ASM_COMPILER "${DEVKITARM}/bin/arm-none-eabi-gcc.exe")
set(CMAKE_AR "${DEVKITARM}/bin/arm-none-eabi-gcc-ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${DEVKITARM}/bin/arm-none-eabi-gcc-ranlib.exe" CACHE FILEPATH "")
if(BRAWL3DS_CXX_CLANG)
  set(CMAKE_CXX_COMPILER "${BRAWL3DS_CXX_CLANG}")
  file(GLOB _gcc_versions LIST_DIRECTORIES true "${DEVKITARM}/lib/gcc/arm-none-eabi/*")
  list(GET _gcc_versions -1 _gcc_dir)
  get_filename_component(_gcc_version "${_gcc_dir}" NAME)
  set(_cxx_inc "${DEVKITARM}/arm-none-eabi/include/c++/${_gcc_version}")
  set(BRAWL3DS_CLANG_FLAGS "--target=arm-none-eabi -mcpu=mpcore -mfpu=vfp -nostdinc++ \
-isystem ${_cxx_inc} -isystem ${_cxx_inc}/arm-none-eabi -isystem ${DEVKITARM}/arm-none-eabi/include")
else()
  set(CMAKE_CXX_COMPILER "${DEVKITARM}/bin/arm-none-eabi-g++.exe")
  set(BRAWL3DS_CLANG_FLAGS "")
endif()

set(CTR_ARCH_FLAGS "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft")
set(CTR_COMMON_FLAGS "-mword-relocations -ffunction-sections -D__3DS__ -isystem ${CTR_ROOT}/include")
set(CMAKE_C_FLAGS_INIT "${CTR_ARCH_FLAGS} ${CTR_COMMON_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${CTR_ARCH_FLAGS} ${CTR_COMMON_FLAGS}")
if(BRAWL3DS_CXX_CLANG)
  # clang spells the CPU as -mcpu=mpcore and does not know -mword-relocations / -mtune=mpcore.
  set(CMAKE_CXX_FLAGS_INIT "${BRAWL3DS_CLANG_FLAGS} -mfloat-abi=hard -mtp=soft -ffunction-sections -D__3DS__ \
-isystem ${CTR_ROOT}/include")
else()
  set(CMAKE_CXX_FLAGS_INIT "${CTR_ARCH_FLAGS} ${CTR_COMMON_FLAGS}")
endif()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CTR_ARCH_FLAGS} -specs=3dsx.specs -L${CTR_ROOT}/lib")
set(CMAKE_C_STANDARD_LIBRARIES "-lctru -lm")
set(CMAKE_CXX_STANDARD_LIBRARIES "-lctru -lm")

# Compile checks must not try to link a 3DS executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${DEVKITARM}" "${CTR_ROOT}" "${DEVKITPRO}/portlibs/3ds")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
