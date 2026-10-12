#pragma once


#include"common/common.h"
#include <time.h>

#include "acl.h"

#include "hi_mpi_sys.h"
#include "hi_dvpp.h"
// VENC 在这份 CANN 里属于 DVPP 那一套（**没有** hi_mpi_venc.h）：
// hi_venc_chn / hi_venc_chn_attr / hi_venc_start_param / hi_mpi_venc_* 都在这里，
// hi_dvpp.h 会带进来；显式写出来是为了标明依赖。
#include "hi_dvpp_venc.h"
#include "hi_mpi_vpss.h"
#include "hi_mpi_vo.h"
#include "hi_mpi_hdmi.h"
#include "hi_region.h"
#include "hi_mpi_vi.h"
#include "hi_mipi_rx.h"

#define MIPI_DEV_NODE       "/dev/hi_mipi_rx"
#define MAX_FRAME_WIDTH     8192

namespace mipi_ctl{
    typedef struct {
      hi_size          max_size;                                        // 组级：最大输入尺寸
      hi_pixel_format  pixel_format;                                    // 组级+通道级：像素格式
      hi_size          output_size[HI_VPSS_MAX_CHN_NUM];                // 每通道：输出尺寸
      hi_compress_mode compress_mode[HI_VPSS_MAX_CHN_NUM];              // 每通道：压缩模式
      hi_bool          enable[HI_VPSS_MAX_CHN_NUM];                     // 每通道：是否启用
    } sample_venc_vpss_chn_attr;

    // VENC 通道参数（本模块自用）。
    // 不能用海思 sample 层的 sample_comm_venc_chn_param：那是 SDK **示例代码**里的结构体，
    // 不属于任何 API 头文件，本工程和这份 CANN 都没有；而且它引用的 hi_pic_size / sample_rc
    // 在本 SDK 里也不存在。这里按 acl/dvpp/hi_dvpp_venc.h 的类型重新定义，码控模式直接复用
    // SDK 的 hi_venc_rc_mode（HI_VENC_RC_MODE_H264_CBR 等），不再自造 sample_rc 枚举。
    typedef struct {
      hi_venc_chn     chn;        // VENC 通道号
      hi_payload_type type;       // HI_PT_H264 / HI_PT_H265 / HI_PT_JPEG
      hi_size         venc_size;  // 编码图像尺寸
      hi_u32          frame_rate; // 帧率（源=目标）
      hi_u32          gop;        // I 帧间隔
      hi_u32          bitrate;    // 码率，单位 kbps（CBR 取平均码率，VBR 取最大码率）
      hi_u32          stats_time; // 码率统计周期，单位秒
      hi_u32          profile;    // H.264: 0=baseline/1=MP/2=HP；H.265: 0=MP/1=Main10
      hi_venc_rc_mode rc_mode;    // 码控模式
    } venc_chn_param;

    // 返回类型统一用 hi_s32：这些函数失败时返回 HI_FAILURE（负数），
    // 写成 hi_u32 会被转成一个巨大的正数，语义就错了。
    hi_s32 vpss_init(hi_vpss_grp vpss_grp, sample_venc_vpss_chn_attr *vpss_chn_cfg);

    hi_s32 vdec_bind_vpss(hi_vdec_chn vdec_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn);
    hi_s32 vdec_un_bind_vpss(hi_vdec_chn vdec_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn);
    hi_void venc_vpss_deinit(hi_vpss_grp vpss_grp, sample_venc_vpss_chn_attr *vpss_chan_cfg);
    hi_s32 venc_start(venc_chn_param *chn_param);
    hi_s32 vpss_bind_venc(hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn, hi_venc_chn venc_chn);
    hi_s32 venc_stop(hi_venc_chn venc_chn);
    hi_s32 vpss_un_bind_venc(hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn, hi_venc_chn venc_chn);
    // VI→VPSS 绑定的源是 vi_pipe / vi_chn（类型 hi_vi_pipe / hi_vi_chn），不是 vdec 的。
    hi_s32 vi_bind_vpss(hi_vi_pipe vi_pipe, hi_vi_chn vi_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn);
    hi_s32 vi_un_bind_vpss(hi_vi_pipe vi_pipe, hi_vi_chn vi_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn);

    // VI 建链路参数（YUV 直出、不过 ISP 的场景）。
    // 注意：raw sensor（RAW Bayer）走这条路不行 —— 那种要额外 hi_mpi_isp_mem_init /
    // hi_mpi_isp_init / hi_mpi_isp_run + sensor 回调注册，并把 pipe_attr.isp_bypass
    // 设成 HI_FALSE、dev_attr.intf_mode 换成 HI_VI_MODE_MIPI(RAW)。
    typedef struct {
      hi_vi_dev       vi_dev;           // VI 设备号，与 mipi_rx 那边下发的 combo dev 号对应（通常 0）
      hi_vi_pipe      pipe;             // VI pipe 号（ISP/3A 的载体；vi_bind_vpss 用的就是它）
      hi_vi_chn       chn;              // VI 物理通道号
      hi_size         size;             // 采集尺寸，必须和 mipi_rx 的 img_rect 一致
      hi_pixel_format chn_pixel_format; // 通道输出格式（交给 VPSS 的），通常 YUV420SP
      hi_u32          pipe_depth;       // pipe 缓存深度，offline 建议 2~3
      hi_u32          chn_depth;        // 通道缓存深度，0 = 不缓存（8.11 用的 0）
      hi_bool         vpss_offline;     // HI_TRUE: VI/VPSS 走 offline（帧落 DDR，可单独取帧）
    } vi_cfg;

    // 建链路顺序（YUV 直出、无 ISP）：
    //   1) vi_enable_mipi_clock / vi_reset_mipi / vi_set_mipi_attr / vi_unreset_mipi
    //      —— 配 MIPI RX 物理层（combo_dev_attr_t：combo dev 号、INPUT_MODE_MIPI、
    //         DATA_TYPE_YUV422_8BIT、lane_id、img_rect）
    //   2) vi_init(&cfg)         —— VI dev → pipe → chn（本模块）
    //   3) vpss_init(grp, &cfg)  —— VPSS 组 + 通道
    //   4) vi_bind_vpss(...)     —— VI chn → VPSS grp
    //   5) vpss_bind_venc(...)   —— VPSS chn → VENC
    //   6) venc_start(&param)    —— 建编码通道并开始收帧
    // 拆的时候按相反顺序：先 venc_stop / vpss_un_bind_venc / vi_un_bind_vpss，
    // 再 venc_vpss_deinit / vi_deinit（vi_deinit 内部已按 chn→pipe→dev 收）。
    hi_s32 vi_init(const vi_cfg *cfg);
    hi_s32 vi_deinit(const vi_cfg *cfg);


    hi_s32 vi_set_mipi_hs_mode(lane_divide_mode_t enHsMode);
    hi_s32 vi_enable_mipi_clock(combo_dev_t devno);
    hi_s32 vi_reset_mipi(combo_dev_t devno);
    hi_s32 vi_set_mipi_attr(combo_dev_attr_t stcomboDevAttr);
    hi_s32 vi_unreset_mipi(combo_dev_t devno);


}
