#include"log/log.h"

#include<ostream>

// 演示类型：花括号初始化里带逗号（老宏方案会把 {1,2} 拆成两个参数导致编译错）
struct Point
{
    int x;
    int y;
};

inline std::ostream& operator<<(std::ostream& os, const Point& p)
{
    return os << "(" << p.x << "," << p.y << ")";
}

int main()
{
    UAV_LOG_INFO  << "test log";
    UAV_LOG_DEBUG << "test log";
    UAV_LOG_WARN  << "test log";
    UAV_LOG_ERROR << "test log";
    UAV_LOG_FATAL << "test log";
    UAV_LOG_TRACE << "test log";

    // printf 风格（格式串字面量享受编译期检查）
    UAV_LOG_INFO_F("test log with format: %d, %s", 123, "abc");

    // cout 风格：花括号里的逗号不再是问题
    UAV_LOG_INFO << "stream x=" << 42 << " point=" << Point{1,2};

    // 条件日志：cond 为假时连实参都不求值
    UAV_LOG_IF(LogLevel::INFO, false) << "this must NOT appear";
    UAV_LOG_IF(LogLevel::INFO, true)  << "this must appear";

    return 0;
}
