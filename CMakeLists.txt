cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LESSTOKEN_BUILD_VERSION LIMIT_COUNT 1)
project(LessToken VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(lesstoken
  src/main.cpp
  src/core.cpp
  src/reduction.cpp
  src/storage.cpp
  src/process.cpp
  src/mcp.cpp
  src/hook.cpp
  src/prompt.cpp
  src/context.cpp
  src/query.cpp
  src/records.cpp
  src/batch.cpp
)
target_include_directories(lesstoken PRIVATE include third_party)
target_compile_definitions(lesstoken PRIVATE LESSTOKEN_VERSION="${LESSTOKEN_BUILD_VERSION}")
if(WIN32)
  target_compile_definitions(lesstoken PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(lesstoken PRIVATE bcrypt shell32)
endif()
if(MSVC)
  set_property(TARGET lesstoken PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(lesstoken PRIVATE /W4 /utf-8)
else()
  target_compile_options(lesstoken PRIVATE -Wall -Wextra -Wpedantic)
  if(MINGW)
    target_link_options(lesstoken PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endif()
