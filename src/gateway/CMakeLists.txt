# Gateway 仅依赖仓库中与架构无关的 Boost 头文件和系统线程库。
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
get_filename_component(UAV_GATEWAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT EXISTS "${UAV_GATEWAY_ROOT}/third_party/boost/include/boost/beast.hpp")
  message(FATAL_ERROR "Gateway requires the bundled Boost.Beast/JSON headers")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/openapi.json" GATEWAY_OPENAPI)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/openapi.json")
configure_file(gateway_openapi.h.in "${CMAKE_CURRENT_BINARY_DIR}/gateway_openapi.h" @ONLY)
add_library(uav_gateway_lib STATIC gateway.cpp config.cpp security.cpp files.cpp boost_json.cpp)
add_custom_target(uav_web_assets
  COMMAND "${CMAKE_COMMAND}" "-DUAV_WEB_SOURCE=${UAV_GATEWAY_ROOT}/www/dist" "-DUAV_WEB_DEST=${CMAKE_BINARY_DIR}/www"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/copy_web_assets.cmake"
  COMMENT "同步 www 前端构建资源")
add_dependencies(uav_gateway_lib uav_web_assets)
# 首次构建提供配置文件；重新配置或安装不覆盖用户已经编辑的凭据。
if(NOT EXISTS "${CMAKE_BINARY_DIR}/config/gateway.json")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  file(COPY "${UAV_GATEWAY_ROOT}/config/gateway.json" DESTINATION "${CMAKE_BINARY_DIR}/config"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE)
endif()
target_include_directories(uav_gateway_lib PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(uav_gateway_lib PUBLIC "${UAV_GATEWAY_ROOT}/src")
target_include_directories(uav_gateway_lib SYSTEM PUBLIC "${UAV_GATEWAY_ROOT}/third_party/boost/include")
target_link_libraries(uav_gateway_lib PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(uav_gateway_lib PRIVATE -Wall -Wextra)

add_executable(uav_gateway main.cpp)
target_link_libraries(uav_gateway PRIVATE uav_gateway_lib)

if(UAV_GATEWAY_TESTS AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(gateway_test_driver "${UAV_GATEWAY_ROOT}/tests/gateway_test_driver.cpp")
  target_link_libraries(gateway_test_driver PRIVATE uav_gateway_lib)
  add_test(NAME gateway_integration
    COMMAND "${Python3_EXECUTABLE}" "${UAV_GATEWAY_ROOT}/tests/gateway_integration.py"
            "$<TARGET_FILE:gateway_test_driver>" "$<TARGET_FILE:uav_gateway>")
  set_tests_properties(gateway_integration PROPERTIES TIMEOUT 45)
endif()

install(TARGETS uav_gateway RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/openapi.json" DESTINATION share/uav-gateway)
install(DIRECTORY "${UAV_GATEWAY_ROOT}/www/dist/" DESTINATION share/uav-gateway/www OPTIONAL)
install(CODE "
  set(settings \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/etc/uav-gateway/gateway.json\")
  if(NOT EXISTS \"\${settings}\")
    file(INSTALL \"${UAV_GATEWAY_ROOT}/config/gateway.json\"
      DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/etc/uav-gateway\"
      FILE_PERMISSIONS OWNER_READ OWNER_WRITE)
  endif()
")
