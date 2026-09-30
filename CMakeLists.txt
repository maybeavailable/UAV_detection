cmake_minimum_required(VERSION 3.10)

project(UAV_detection LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -Wall -Wextra")

# 不指定构建类型时 CMAKE_CXX_FLAGS_DEBUG/RELEASE 都不会生效（-Wall -Wextra 随之失效），
# 因此显式给一个默认值，保证警告始终可见
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug/Release)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()


set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)


list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost)
list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl)

find_package(Boost 1.88 REQUIRED COMPONENTS thread filesystem)
find_package(OpenSSL REQUIRED)



include_directories(${PROJECT_SOURCE_DIR}/src)

# 注意：新增模块的 .cpp 必须显式列在这里（旧版本漏掉整个 log 模块，
# 导致日志代码从未被编译、里面的编译错误一直没被发现）
set(UAV_SOURCES
  src/main.cpp
  src/log/log.cpp
  src/log/RotateFileWriter.cpp
)

add_executable(uav_detection
  ${UAV_SOURCES}
)


target_include_directories(uav_detection PRIVATE src)
target_link_libraries(uav_detection PRIVATE
  Boost::headers                 # header-only（asio/beast/json…）
  Boost::thread
  OpenSSL::SSL OpenSSL::Crypto)
# 日志模块已改用 std::filesystem（C++17 标准库），不再需要 Boost::filesystem；
# 其他模块若要使用 Boost.Filesystem，在此处加回 Boost::filesystem 即可。