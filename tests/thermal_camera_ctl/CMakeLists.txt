cmake_minimum_required(VERSION 3.18)
project(thermal_camera_ctl_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
find_package(Threads REQUIRED)
get_filename_component(UAV_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# 使用真实 SDK 类型、替换媒体函数实现；在 x86 主机运行，不链接板端媒体库。
find_path(THERMAL_CANN_INCLUDE_DIR acl/acl.h
  HINTS "$ENV{ASCEND_HOME_PATH}" "$ENV{ASCEND_TOOLKIT_HOME}"
        /usr/local/Ascend/ascend-toolkit/latest "$ENV{HOME}/Ascend/ascend-toolkit/latest"
  PATH_SUFFIXES aarch64-linux/include x86_64-linux/include include)
if(NOT THERMAL_CANN_INCLUDE_DIR)
  message(FATAL_ERROR "需要 CANN 头文件，请设置 -DTHERMAL_CANN_INCLUDE_DIR=/path/to/include")
endif()

add_library(camera_test_support STATIC
  "${UAV_PROJECT_ROOT}/src/camera/thermal_camera.cpp"
  "${UAV_PROJECT_ROOT}/src/camera/airui_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/log/log.cpp"
  "${UAV_PROJECT_ROOT}/src/log/RotateFileWriter.cpp")
target_include_directories(camera_test_support PUBLIC "${UAV_PROJECT_ROOT}/src")
target_include_directories(camera_test_support SYSTEM PUBLIC
  "${UAV_PROJECT_ROOT}/third_party/boost/include"
  "${THERMAL_CANN_INCLUDE_DIR}" "${THERMAL_CANN_INCLUDE_DIR}/acl"
  "${THERMAL_CANN_INCLUDE_DIR}/acl/dvpp" "${THERMAL_CANN_INCLUDE_DIR}/acl/media")
target_compile_options(camera_test_support PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camera_test_support PUBLIC Threads::Threads)
add_executable(thermal_camera_ctl_test thermal_camera_ctl_test.cpp
  "${UAV_PROJECT_ROOT}/src/camera/thermal_camera_ctl.cpp")
target_compile_options(thermal_camera_ctl_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(thermal_camera_ctl_test PRIVATE camera_test_support)
add_test(NAME thermal_camera_ctl COMMAND thermal_camera_ctl_test)
set_tests_properties(thermal_camera_ctl PROPERTIES TIMEOUT 15)

# 执行真实 main，控制器/ACL 由测试替身提供，验证双相机编排而无需板卡。
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(camera_capture_main_test camera_capture_stubs.cpp
  "${UAV_PROJECT_ROOT}/src/main.cpp"
  "${UAV_PROJECT_ROOT}/src/camera/rgb_camera.cpp"
  "${UAV_PROJECT_ROOT}/src/camera/shihui_protocol.cpp")
target_compile_definitions(camera_capture_main_test PRIVATE UAV_HAS_ASCEND=1)
target_compile_options(camera_capture_main_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(camera_capture_main_test PRIVATE camera_test_support)
add_test(NAME camera_capture_main COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/camera_capture_integration.py" "$<TARGET_FILE:camera_capture_main_test>")
set_tests_properties(camera_capture_main PROPERTIES TIMEOUT 25)
