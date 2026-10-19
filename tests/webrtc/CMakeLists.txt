cmake_minimum_required(VERSION 3.18)
project(webrtc_protocol_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()
get_filename_component(UAV_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_library(webrtc_test_log STATIC
  "${UAV_PROJECT_ROOT}/src/log/log.cpp"
  "${UAV_PROJECT_ROOT}/src/log/RotateFileWriter.cpp")
target_include_directories(webrtc_test_log PUBLIC "${UAV_PROJECT_ROOT}/src")
target_include_directories(webrtc_test_log SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(webrtc_test_log PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(webrtc_test_log PUBLIC Threads::Threads)
list(APPEND CMAKE_MODULE_PATH "${UAV_PROJECT_ROOT}/cmake")
find_package(USRSCTP REQUIRED)
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

find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(SRTP REQUIRED)
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

add_executable(dtls_transport_test dtls_transport_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtcp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/dtlstransport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/pacer.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_transport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_agent.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/stun_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/sdp.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(dtls_transport_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(dtls_transport_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(dtls_transport_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dtls_transport_test PRIVATE SRTP::SRTP Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME dtls_transport COMMAND dtls_transport_test)
set_tests_properties(dtls_transport PROPERTIES TIMEOUT 30)

add_executable(peerconnection_test peerconnection_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtcp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/datachannel.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/peerconnection.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/dtlstransport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/pacer.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_transport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_agent.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/stun_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/sdp.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(peerconnection_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(peerconnection_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(peerconnection_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(peerconnection_test PRIVATE SRTP::SRTP USRSCTP::USRSCTP Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME peerconnection COMMAND peerconnection_test)
set_tests_properties(peerconnection PROPERTIES TIMEOUT 30)

# 独立测试工程使用宿主架构 libsrtp2；可通过 -DSRTP_ROOT=/path/to/native/prefix 指定。
add_executable(srtp_ctl_test srtp_ctl_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtcp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/datachannel.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/peerconnection.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/dtlstransport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/pacer.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_transport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_agent.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/stun_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/sdp.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(srtp_ctl_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(srtp_ctl_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(srtp_ctl_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(srtp_ctl_test PRIVATE USRSCTP::USRSCTP SRTP::SRTP Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME srtp_ctl COMMAND srtp_ctl_test rtp)
add_test(NAME srtcp_ctl COMMAND srtp_ctl_test rtcp)
add_test(NAME dtls_srtp COMMAND srtp_ctl_test integration)
set_tests_properties(srtp_ctl srtcp_ctl dtls_srtp PROPERTIES TIMEOUT 30)

add_executable(datachannel_test datachannel_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/datachannel.cpp")
target_include_directories(datachannel_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(datachannel_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(datachannel_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(datachannel_test PRIVATE USRSCTP::USRSCTP Threads::Threads)
add_test(NAME datachannel COMMAND datachannel_test)
set_tests_properties(datachannel PROPERTIES TIMEOUT 30)

add_executable(datachannel_peer_test datachannel_peer_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/srtcp_ctl.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/datachannel.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/peerconnection.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/dtlstransport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/pacer.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_session.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_transport.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_agent.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/stun_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/ice_candidate.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/sdp.cpp"
  "${UAV_PROJECT_ROOT}/src/common/udp_socket.cpp")
target_include_directories(datachannel_peer_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(datachannel_peer_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(datachannel_peer_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(datachannel_peer_test PRIVATE SRTP::SRTP USRSCTP::USRSCTP Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME datachannel_peer COMMAND datachannel_peer_test)
set_tests_properties(datachannel_peer PROPERTIES TIMEOUT 30)

# 使用真实日志库验证状态/故障路径，主工程已经包含这两个日志实现文件。
foreach(webrtc_logged_target IN ITEMS ice_agent_test ice_transport_test dtls_transport_test
    peerconnection_test srtp_ctl_test datachannel_test datachannel_peer_test)
  target_link_libraries(${webrtc_logged_target} PRIVATE webrtc_test_log)
endforeach()

add_executable(rtcp_session_test rtcp_session_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_session.cpp")
target_include_directories(rtcp_session_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(rtcp_session_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(rtcp_session_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rtcp_session_test PRIVATE webrtc_test_log)
add_test(NAME rtcp_session COMMAND rtcp_session_test)
set_tests_properties(rtcp_session PROPERTIES TIMEOUT 20)

add_executable(pacer_test pacer_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/pacer.cpp")
target_include_directories(pacer_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_compile_options(pacer_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME pacer COMMAND pacer_test)
set_tests_properties(pacer PROPERTIES TIMEOUT 10)

# Each DTLS/PeerConnection executable embeds the pacing engine independently.
foreach(pacing_target IN ITEMS pacer_test dtls_transport_test peerconnection_test srtp_ctl_test datachannel_peer_test)
  target_sources(${pacing_target} PRIVATE "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/interval_budget.cpp")
endforeach()
add_executable(interval_budget_test interval_budget_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/interval_budget.cpp")
target_include_directories(interval_budget_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_compile_options(interval_budget_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME interval_budget COMMAND interval_budget_test)
set_tests_properties(interval_budget PROPERTIES TIMEOUT 10)

foreach(adapter_target IN ITEMS rtcp_session_test dtls_transport_test peerconnection_test srtp_ctl_test datachannel_peer_test)
  target_sources(${adapter_target} PRIVATE "${UAV_PROJECT_ROOT}/src/webrtc/transport_adapter.cpp")
endforeach()
add_executable(transport_adapter_test transport_adapter_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/transport_adapter.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp")
target_include_directories(transport_adapter_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(transport_adapter_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(transport_adapter_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(transport_adapter_test PRIVATE webrtc_test_log)
add_test(NAME transport_adapter COMMAND transport_adapter_test)
set_tests_properties(transport_adapter PROPERTIES TIMEOUT 10)

add_executable(bitrate_estimator_test bitrate_estimator_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/bitrate_estimator.cpp")
add_executable(acknowledged_bitrate_estimator_test acknowledged_bitrate_estimator_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/acknowledged_bitrate_estimator.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/bitrate_estimator.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/transport_adapter.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp")
foreach(estimator_target IN ITEMS bitrate_estimator_test acknowledged_bitrate_estimator_test)
  target_include_directories(${estimator_target} PRIVATE "${UAV_PROJECT_ROOT}/src")
  target_include_directories(${estimator_target} SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
  target_compile_options(${estimator_target} PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(${estimator_target} PRIVATE webrtc_test_log)
endforeach()
add_test(NAME bitrate_estimator COMMAND bitrate_estimator_test)
add_test(NAME acknowledged_bitrate_estimator COMMAND acknowledged_bitrate_estimator_test)
set_tests_properties(bitrate_estimator acknowledged_bitrate_estimator PROPERTIES TIMEOUT 10)

add_executable(inter_arrival_delta_test inter_arrival_delta_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/inter_arrival_delta.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/transport_adapter.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/rtcp_protocol.cpp")
target_include_directories(inter_arrival_delta_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(inter_arrival_delta_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(inter_arrival_delta_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(inter_arrival_delta_test PRIVATE webrtc_test_log)
add_test(NAME inter_arrival_delta COMMAND inter_arrival_delta_test)
set_tests_properties(inter_arrival_delta PROPERTIES TIMEOUT 10)

add_executable(alr_detector_test alr_detector_test.cpp
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/alr_detector.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/interval_budget.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/acknowledged_bitrate_estimator.cpp"
  "${UAV_PROJECT_ROOT}/src/webrtc/congestion_control/bitrate_estimator.cpp")
target_include_directories(alr_detector_test PRIVATE "${UAV_PROJECT_ROOT}/src")
target_include_directories(alr_detector_test SYSTEM PRIVATE "${UAV_PROJECT_ROOT}/third_party/boost/include")
target_compile_options(alr_detector_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(alr_detector_test PRIVATE webrtc_test_log)
add_test(NAME alr_detector COMMAND alr_detector_test)
set_tests_properties(alr_detector PROPERTIES TIMEOUT 10)
