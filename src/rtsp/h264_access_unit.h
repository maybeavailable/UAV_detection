#pragma once

#include<cstddef>
#include<cstdint>
#include<vector>

struct H264AccessUnit
{
    std::vector<std::uint8_t> data; // Annex-B；一张图像的全部切片及关联非 VCL NAL
    bool keyFrame{false};
};

// 支持 AVC 普通切片/数据分区切片；按 H.264 7.4.1.2.4 的图像边界分组。
// 缺失参数集、截断头或未支持的扩展码流抛出 invalid_argument。
std::vector<H264AccessUnit> parseH264AccessUnits(const std::uint8_t* data, std::size_t size);
