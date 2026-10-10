#pragma once

#include"common/common.h"


class GenerateSN
{
public:
    GenerateSN() = default;
    ~GenerateSN() = default;

    static std::string generate(){
        uint32_t val = m_sn.fetch_add(1, std::memory_order_relaxed);
        return std::to_string(val);
    }

private:
    static std::atomic<uint32_t> m_sn;


};



// C++17 inline 变量：头文件里定义静态成员必须加 inline，
// 否则多个 .cpp 同时包含本头文件时链接期会报 multiple definition
inline std::atomic<uint32_t> GenerateSN::m_sn{1};