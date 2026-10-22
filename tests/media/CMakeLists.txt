cmake_minimum_required(VERSION 3.18)
project(media_service_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
find_package(Threads REQUIRED)
get_filename_component(UAV_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(media_test_support STATIC
  "${UAV_PROJECT_ROOT}/src/media/video_frame.cpp"
  "${UAV_PROJECT_ROOT}/src/media/media_service.cpp"
  "${UAV_PROJECT_ROOT}/src/media/h264_packetizer.cpp"
  "${UAV_PROJECT_ROOT}/src/encode/encoder_session.cpp"
  "${UAV_PROJECT_ROOT}/src/moq/video_source.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/video_sender.cpp"
  "${UAV_PROJECT_ROOT}/src/rtsp/live_source.cpp"
  "${UAV_PROJECT_ROOT}/src/rtsp/rtp_source.cpp"
  "${UAV_PROJECT_ROOT}/src/rtsp/rtp_transport.cpp"
  "${UAV_PROJECT_ROOT}/src/rtsp/rtspserver.cpp"
  "${UAV_PROJECT_ROOT}/src/log/log.cpp"
  "${UAV_PROJECT_ROOT}/src/log/RotateFileWriter.cpp")
target_include_directories(media_test_support PUBLIC "${UAV_PROJECT_ROOT}/src")
target_include_directories(media_test_support SYSTEM PUBLIC "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(media_test_support PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(media_test_support PUBLIC Threads::Threads)
add_executable(media_service_test media_service_test.cpp)
target_link_libraries(media_service_test PRIVATE media_test_support)
target_compile_options(media_service_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME media_service COMMAND media_service_test)
set_tests_properties(media_service PROPERTIES TIMEOUT 20)

# Compile against the installed real CANN types; tests provide native SDK symbols.
find_path(MEDIA_CANN_INCLUDE_DIR acl/acl.h
  HINTS "$ENV{ASCEND_HOME_PATH}" "$ENV{ASCEND_TOOLKIT_HOME}" /usr/local/Ascend/ascend-toolkit/latest
  PATH_SUFFIXES aarch64-linux/include x86_64-linux/include include)
if(MEDIA_CANN_INCLUDE_DIR)
  add_executable(venc_encoder_test venc_encoder_test.cpp
    "${UAV_PROJECT_ROOT}/src/encode/venc_encoder.cpp"
    "${UAV_PROJECT_ROOT}/src/encode/ascend_video_frame.cpp"
    "${UAV_PROJECT_ROOT}/src/media/ascend_media_pipeline.cpp"
    "${UAV_PROJECT_ROOT}/src/media/ascend_media_device.cpp")
  target_include_directories(venc_encoder_test SYSTEM PRIVATE "${MEDIA_CANN_INCLUDE_DIR}" "${MEDIA_CANN_INCLUDE_DIR}/acl"
    "${MEDIA_CANN_INCLUDE_DIR}/acl/dvpp")
  target_link_libraries(venc_encoder_test PRIVATE media_test_support)
  target_compile_options(venc_encoder_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME venc_encoder COMMAND venc_encoder_test)
  set_tests_properties(venc_encoder PROPERTIES TIMEOUT 10)
endif()
