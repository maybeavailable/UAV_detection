cmake_minimum_required(VERSION 3.18)
project(webrtc_protocol_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
get_filename_component(UAV_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_executable(ice_candidate_test ice_candidate_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(ice_candidate_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(ice_candidate_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(ice_candidate_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ice_candidate_test PRIVATE Threads::Threads)
add_test(NAME ice_candidate COMMAND ice_candidate_test)
set_tests_properties(ice_candidate PROPERTIES TIMEOUT 10)

add_executable(sdp_test sdp_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/sdp.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp")
target_include_directories(sdp_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(sdp_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(sdp_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sdp_test PRIVATE Threads::Threads)
add_test(NAME sdp COMMAND sdp_test)
set_tests_properties(sdp PROPERTIES TIMEOUT 10)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_executable(stun_protocol_test stun_protocol_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/stun_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(stun_protocol_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(stun_protocol_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(stun_protocol_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(stun_protocol_test PRIVATE Threads::Threads OpenSSL::Crypto)
add_test(NAME stun_protocol COMMAND stun_protocol_test)
set_tests_properties(stun_protocol PROPERTIES TIMEOUT 10)

add_executable(ice_agent_test ice_agent_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_agent.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/stun_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/sdp.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(ice_agent_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(ice_agent_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(ice_agent_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ice_agent_test PRIVATE Threads::Threads OpenSSL::Crypto)
add_test(NAME ice_agent COMMAND ice_agent_test)
set_tests_properties(ice_agent PROPERTIES TIMEOUT 45)

add_executable(ice_transport_test ice_transport_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_transport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_agent.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/stun_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/sdp.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(ice_transport_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(ice_transport_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(ice_transport_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ice_transport_test PRIVATE Threads::Threads OpenSSL::Crypto)
add_test(NAME ice_transport COMMAND ice_transport_test)
set_tests_properties(ice_transport PROPERTIES TIMEOUT 20)
