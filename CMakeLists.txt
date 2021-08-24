cmake_minimum_required (VERSION 3.12)

project(mule)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MULE_ENABLE_ASAN "Enable ASAN" OFF)
option(MULE_ENABLE_MSAN "Enable MSAN" OFF)
option(MULE_ENABLE_TSAN "Enable TSAN" OFF)
option(MULE_ENABLE_UBSAN "Enable UBSAN" OFF)

macro(add_compiler_flag)
   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ARGN}")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ARGN}")
endmacro(add_compiler_flag)

macro(add_linker_flag)
  set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} ${ARGN}")
endmacro(add_linker_flag)

include(CheckCXXCompilerFlag)

# We need haswell for TZCNT/LZCNT
check_cxx_compiler_flag("-march=x86-64-v3" has_march_x86_64_v3 "int main() { return 0; }")
check_cxx_compiler_flag("-march=haswell" has_march_haswell "int main() { return 0; }")
if (has_march_x86_64_v3)
	add_compiler_flag(-march=x86-64-v3)
elseif (has_march_haswell)
	add_compiler_flag(-march=haswell)
endif()

if (MULE_ENABLE_ASAN)
  add_compiler_flag(-fsanitize=address)
  add_linker_flag(-fsanitize=address)
endif()

if(MULE_ENABLE_MSAN)
  add_compiler_flag(-fsanitize=memory)
  add_linker_flag(-fsanitize=memory)
endif()

if(MULE_ENABLE_TSAN)
  add_compiler_flag("-fPIE -fsanitize=thread")
  add_linker_flag("-fPIE -fsanitize=thread")
endif()

if(MULE_ENABLE_UBSAN)
  add_compiler_flag(-fsanitize=undefined)
  add_linker_flag(-fsanitize=undefined)
endif()

# check for the C thread support library
find_package(Threads REQUIRED)
check_include_file("threads.h" have_threads_h)
set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
check_c_source_compiles("#include <threads.h>
int main() { thrd_t t; thrd_create(&t, &main, NULL); }" has_thread_support)
check_c_source_compiles("#include <time.h>
int main() { struct timespec ts; timespec_get(&ts, 0); }" has_timespec_get)

if(NOT has_thread_support)
message(FATAL_ERROR C11 thread support library required)
endif()

add_executable(test_mumule test_mumule.c)
target_link_libraries(test_mumule ${CMAKE_THREAD_LIBS_INIT})
