#pragma once
#include<iostream>
#include"thread"
#include"condition_variable"
#include"memory"
#include"mutex"
#include<string>
#include"vector"
#include<boost/asio.hpp>
#include<boost/asio/io_context.hpp>
#include"queue"
#include"unordered_map"

#include"map"
#include<fstream>

const int RTSP_PORT = 8554;

const int RTSP_RTP_PORT = 5004;
const int RTSP_RTCP_PORT = 5005;