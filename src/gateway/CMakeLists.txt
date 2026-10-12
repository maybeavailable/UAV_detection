# Gateway 仅依赖仓库中与架构无关的 Boost 头文件和系统线程库。
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/boost/include/boost/beast.hpp")
  message(FATAL_ERROR "Gateway requires the bundled Boost.Beast/JSON headers")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/openapi.json" GATEWAY_OPENAPI)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/openapi.json")
configure_file(gateway_openapi.h.in "${CMAKE_CURRENT_BINARY_DIR}/gateway_openapi.h" @ONLY)
add_library(uav_gateway_lib STATIC gateway.cpp config.cpp security.cpp files.cpp boost_json.cpp)
target_include_directories(uav_gateway_lib PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(uav_gateway_lib PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(uav_gateway_lib SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/boost/include")
target_link_libraries(uav_gateway_lib PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(uav_gateway_lib PRIVATE -Wall -Wextra)

add_executable(uav_gateway main.cpp)
target_link_libraries(uav_gateway PRIVATE uav_gateway_lib)

if(UAV_GATEWAY_TESTS AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(gateway_test_driver "${PROJECT_SOURCE_DIR}/tests/gateway_test_driver.cpp")
  target_link_libraries(gateway_test_driver PRIVATE uav_gateway_lib)
  add_test(NAME gateway_integration
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/gateway_integration.py"
            "$<TARGET_FILE:gateway_test_driver>" "$<TARGET_FILE:uav_gateway>")
  set_tests_properties(gateway_integration PROPERTIES TIMEOUT 45)
endif()

install(TARGETS uav_gateway RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/openapi.json" DESTINATION share/uav-gateway)
