cmake_minimum_required(VERSION 3.18)
project(airui_protocol_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
get_filename_component(UAV_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_executable(airui_protocol_test airui_protocol_test.cpp
  "${UAV_PROJECT_ROOT}/src/camera/airui_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/camera/thermal_camera.cpp"
  "${UAV_PROJECT_ROOT}/src/log/log.cpp"
  "${UAV_PROJECT_ROOT}/src/log/RotateFileWriter.cpp")
target_include_directories(airui_protocol_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(airui_protocol_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(airui_protocol_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(airui_protocol_test PRIVATE Threads::Threads)
add_test(NAME airui_protocol COMMAND airui_protocol_test "${CMAKE_CURRENT_SOURCE_DIR}/pdf_vectors.txt")
set_tests_properties(airui_protocol PROPERTIES TIMEOUT 25)
