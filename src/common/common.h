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

// 说明：
//   - 日志记录类型 Log 与日志等级 LogLevel 已归位到日志模块（log/log.h），
//     这里不再定义，避免公共头文件被日志模块的细节耦合。
//   - 这里也不再 include <boost/filesystem.hpp>：只有日志模块用到文件系统，
//     现在由 log/RotateFileWriter.cpp 使用 std::filesystem 自行包含，
//     免得所有模块被动依赖 Boost.Filesystem，并避免头文件里出现全局 fs 别名。
