find_package(Threads REQUIRED)
add_library(uav_rtsp_lib STATIC
  rtspserver.cpp rtp_source.cpp rtp_transport.cpp file_source.cpp h264_access_unit.cpp)
target_include_directories(uav_rtsp_lib PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(uav_rtsp_lib SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/boost/include")
target_link_libraries(uav_rtsp_lib PUBLIC Threads::Threads)

if(UAV_RTSP_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(rtsp_tests "${PROJECT_SOURCE_DIR}/tests/rtsp_tests.cpp"
    "${PROJECT_SOURCE_DIR}/src/log/log.cpp" "${PROJECT_SOURCE_DIR}/src/log/RotateFileWriter.cpp")
  target_link_libraries(rtsp_tests PRIVATE uav_rtsp_lib)
  add_executable(rtsp_test_server "${PROJECT_SOURCE_DIR}/tests/rtsp_test_server.cpp"
    "${PROJECT_SOURCE_DIR}/src/log/log.cpp" "${PROJECT_SOURCE_DIR}/src/log/RotateFileWriter.cpp")
  target_link_libraries(rtsp_test_server PRIVATE uav_rtsp_lib)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(UAV_RTSP_FFMPEG ffmpeg REQUIRED)
  add_test(NAME rtsp_regression COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/rtsp_integration.py"
    $<TARGET_FILE:rtsp_tests> $<TARGET_FILE:rtsp_test_server> "${UAV_RTSP_FFMPEG}")
  set_tests_properties(rtsp_regression PROPERTIES TIMEOUT 90)
endif()
