#include "mipi_ctl.h"
#include"log/log.h"

#include <cstring>   // memset（venc_create 里给 hi_venc_chn_attr 清零）

static hi_s32 vpss_stop(hi_vpss_grp grp, const hi_bool *chn_enable, hi_u32 chn_array_size) {
        hi_u32 i;
        hi_s32 ret;
        hi_vpss_chn vpss_chn;

        if (chn_array_size < HI_VPSS_MAX_CHN_NUM) {
            UAV_LOG_ERROR_F("array size(%u) of chn_enable need > %u!\n", chn_array_size, HI_VPSS_MAX_CHN_NUM);
            return HI_FAILURE;
        }

        for (i = 0; i < HI_VPSS_MAX_CHN_NUM; ++i) {
            if (chn_enable[i] == HI_TRUE) {
                vpss_chn = i;
                ret = hi_mpi_vpss_disable_chn(grp, vpss_chn);
                if (ret != HI_SUCCESS) {
                    UAV_LOG_ERROR_F("hi_mpi_vpss_disable_chn failed with %#x!\n", ret);
                }
            }
        }

        ret = hi_mpi_vpss_stop_grp(grp);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vpss_stop_grp failed with %#x!\n", ret);
        }

        ret = hi_mpi_vpss_destroy_grp(grp);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vpss_destroy_grp failed with %#x!\n", ret);
        }

        return HI_SUCCESS;
    }
    
static hi_s32 vpss_start_chn(hi_vpss_grp grp, const hi_bool *chn_enable, const hi_vpss_chn_attr *chn_attr,
                                 hi_u32 chn_array_size) {
        hi_vpss_chn vpss_chn;
        hi_s32 ret, i;

        for (i = 0; i < (hi_s32) chn_array_size; ++i) {
            if (chn_enable[i] == HI_TRUE) {
                vpss_chn = i;
                ret = hi_mpi_vpss_set_chn_attr(grp, vpss_chn, &chn_attr[vpss_chn]);
                if (ret != HI_SUCCESS) {
                    UAV_LOG_ERROR_F("hi_mpi_vpss_set_chn_attr failed with %#x\n", ret);
                    goto disable_chn;
                }

                ret = hi_mpi_vpss_enable_chn(grp, vpss_chn);
                if (ret != HI_SUCCESS) {
                    UAV_LOG_ERROR_F("hi_mpi_vpss_enable_chn failed with %#x\n", ret);
                    goto disable_chn;
                }
            }
        }
        return HI_SUCCESS;

        disable_chn:
        for (i = i - 1; i >= 0; i--) {
            if (chn_enable[i] == HI_TRUE) {
                vpss_chn = i;
                ret = hi_mpi_vpss_disable_chn(grp, vpss_chn);
                if (ret != HI_SUCCESS) {
                    UAV_LOG_ERROR_F("hi_mpi_vpss_disable_chn failed with %#x!\n", ret);
                }
            }
        }

        return HI_FAILURE;
    }

    static hi_s32 vpss_start(hi_vpss_grp grp, const hi_bool *chn_enable, const hi_vpss_grp_attr *grp_attr,
                             const hi_vpss_chn_attr *chn_attr, hi_u32 chn_array_size) {
        hi_s32 ret;
        ret = hi_mpi_vpss_create_grp(grp, grp_attr);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vpss_create_grp(grp:%d) failed with %#x!\n", grp, ret);
            return HI_FAILURE;
        }

        ret = hi_mpi_vpss_start_grp(grp);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vpss_start_grp failed with %#x\n", ret);
            goto destroy_grp;
        }

        ret = vpss_start_chn(grp, chn_enable, chn_attr, chn_array_size);
        if (ret != HI_SUCCESS) {
            goto stop_grp;
        }

        return HI_SUCCESS;

        stop_grp:
        ret = hi_mpi_vpss_stop_grp(grp);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vpss_stop_grp failed with %#x!\n", ret);
        }
        destroy_grp:
        ret = hi_mpi_vpss_destroy_grp(grp);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vpss_destroy_grp failed with %#x!\n", ret);
        }
        return HI_FAILURE;
    }


hi_s32 mipi_ctl::vi_set_mipi_hs_mode(lane_divide_mode_t enHsMode)
{
    hi_s32 fd;
        hi_s32 s32Ret;

        fd = open(MIPI_DEV_NODE, O_RDWR);
        if (fd < 0)
        {
            UAV_LOG_ERROR_F("open hi_mipi_rx dev failed\n");
            return -1;
        }

        s32Ret = ioctl(fd, HI_MIPI_SET_HS_MODE, &enHsMode);
        if (HI_SUCCESS != s32Ret)
        {
            UAV_LOG_ERROR_F("HI_MIPI_SET_HS_MODE failed\n");
        }

        close(fd);
        return s32Ret;
}

hi_s32 mipi_ctl::vi_enable_mipi_clock(combo_dev_t devno)
{
    hi_s32 s32Ret = HI_SUCCESS;
    hi_s32 fd;

    fd = open(MIPI_DEV_NODE, O_RDWR);
    if (fd < 0)
    {
        UAV_LOG_FATAL_F("open hi_mipi_rx dev failed\n");
        return HI_FAILURE;
    }

    s32Ret = ioctl(fd, HI_MIPI_ENABLE_MIPI_CLOCK, &devno);
    if (HI_SUCCESS != s32Ret)
    {
        UAV_LOG_FATAL_F("MIPI_ENABLE_CLOCK %d failed\n", devno);
    }

    close(fd);
    return s32Ret;
}

hi_s32 mipi_ctl::vi_disable_mipi_clock(combo_dev_t devno)
{
    const int fd = open(MIPI_DEV_NODE, O_RDWR);
    if (fd < 0)
    {
        UAV_LOG_ERROR_F("open hi_mipi_rx dev failed");
        return HI_FAILURE;
    }
    const hi_s32 ret = ioctl(fd, HI_MIPI_DISABLE_MIPI_CLOCK, &devno);
    if (ret != HI_SUCCESS)
        UAV_LOG_ERROR_F("MIPI_DISABLE_CLOCK %u failed: %#x", devno, static_cast<unsigned>(ret));
    close(fd);
    return ret;
}

hi_s32 mipi_ctl::vi_reset_mipi(combo_dev_t devno)
{
        hi_s32              s32Ret = HI_SUCCESS;
        hi_s32              fd;

        fd = open(MIPI_DEV_NODE, O_RDWR);

        if (fd < 0)
        {
            UAV_LOG_ERROR_F("open hi_mipi_rx dev failed\n");
            return HI_FAILURE;
        }

        s32Ret = ioctl(fd, HI_MIPI_RESET_MIPI, &devno);
        if (HI_SUCCESS != s32Ret)
        {
            UAV_LOG_ERROR_F("RESET_MIPI %d failed\n", devno);
        }

        close(fd);
        return s32Ret;
}

hi_s32 mipi_ctl::vi_set_mipi_attr(combo_dev_attr_t stcomboDevAttr)
{
        hi_s32 s32Ret = HI_SUCCESS;
        hi_s32 fd;

        fd = open(MIPI_DEV_NODE, O_RDWR);
        if (fd < 0)
        {
            UAV_LOG_ERROR_F("open hi_mipi_rx dev failed\n");
            return HI_FAILURE;
        }

        s32Ret = ioctl(fd, HI_MIPI_SET_DEV_ATTR, &stcomboDevAttr);

        if (HI_SUCCESS != s32Ret)
        {
            UAV_LOG_ERROR_F("MIPI_SET_DEV_ATTR failed: %#x\n", s32Ret);
        }

        close(fd);

        return s32Ret;
}

hi_s32 mipi_ctl::vi_unreset_mipi(combo_dev_t devno)
{
        hi_s32 s32Ret = HI_SUCCESS;
        hi_s32 fd;

        fd = open(MIPI_DEV_NODE, O_RDWR);
        if (fd < 0)
        {
            UAV_LOG_ERROR_F("open hi_mipi_rx dev failed\n");
            return HI_FAILURE;
        }

        UAV_LOG_INFO_F("..........HI_MIPI_UNRESET_MIPI data: %d\n", devno);
        s32Ret = ioctl(fd, HI_MIPI_UNRESET_MIPI, &devno);
        if (HI_SUCCESS != s32Ret)
        {
            UAV_LOG_ERROR_F("UNRESET_MIPI %d failed\n", devno);
        }

        close(fd);
        return s32Ret;
}

    // ---- VI：dev → pipe → chn -------------------------------------------------
    // YUV/BT.1120 直出（ISP bypass）的建链路顺序，照 8.11 工程里已验证过的写法。
    // 这里**不做** MIPI RX 的配置：调用方先按
    //   vi_enable_mipi_clock → vi_reset_mipi → vi_set_mipi_attr → vi_unreset_mipi
    // 把 combo_dev_attr_t（输入模式 / lane / img_rect）下发下去，再进来建 VI。
    hi_s32 mipi_ctl::vi_init(const vi_cfg *cfg)
    {
        hi_s32 ret;
        hi_vi_dev_attr dev_attr;
        hi_vi_dev_bind_pipe dev_bind_pipe;
        hi_vi_pipe_attr pipe_attr;
        hi_vi_chn_attr chn_attr;
        hi_vi_vpss_mode vpss_mode;
        hi_u32 i;

        if (cfg == nullptr) {
            UAV_LOG_ERROR_F("vi_init: cfg is null\n");
            return HI_FAILURE;
        }

        /* step 0: VI→VPSS 工作模式，必须在建 pipe 之前设置。
           offline：帧落 DDR，VI/VPSS 都能单独取帧（接 AI 时最方便），代价是带宽/延迟；
           online ：硬件直连不落 DDR，省带宽低延迟，但用户态拿不到中间帧。
           要换 online 只改这一处（也可混搭，如 HI_VI_ONLINE_VPSS_OFFLINE）。 */
        for (i = 0; i < HI_VI_MAX_PIPE_NUM; ++i) {
            vpss_mode.mode[i] = cfg->vpss_offline ? HI_VI_OFFLINE_VPSS_OFFLINE
                                                  : HI_VI_ONLINE_VPSS_ONLINE;
        }
        ret = hi_mpi_vi_set_vi_vpss_mode(&vpss_mode);
        if (ret != HI_SUCCESS) {
            /* 不致命：个别平台不支持该接口，会保持 SDK 默认模式，所以只告警不返回 */
            UAV_LOG_WARN_F("hi_mpi_vi_set_vi_vpss_mode failed with %#x, keep sdk default\n", ret);
        }

        /* step 1: VI dev 属性。intf_mode 必须和 mipi_rx 那边下发的模式对应：
           MIPI YUV 直出用 HI_VI_MODE_MIPI_YUV422；BT.1120 要换成对应的接口模式。 */
        memset(&dev_attr, 0, sizeof(dev_attr));
        dev_attr.intf_mode = HI_VI_MODE_MIPI_YUV422;
        dev_attr.scan_mode = HI_VI_SCAN_PROGRESSIVE;
        dev_attr.data_seq  = HI_VI_DATA_SEQ_UVUV;
        dev_attr.data_type = HI_VI_DATA_TYPE_YUV;
        dev_attr.in_size.width  = cfg->size.width;
        dev_attr.in_size.height = cfg->size.height;
        dev_attr.data_rate = HI_DATA_RATE_X1;
        dev_attr.wdr_attr.wdr_mode = HI_WDR_MODE_NONE;

        ret = hi_mpi_vi_set_dev_attr(cfg->vi_dev, &dev_attr);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vi_set_dev_attr(dev %d) failed with %#x!\n", cfg->vi_dev, ret);
            return ret;
        }

        /* step 2: 使能 dev */
        ret = hi_mpi_vi_enable_dev(cfg->vi_dev);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vi_enable_dev(dev %d) failed with %#x!\n", cfg->vi_dev, ret);
            return ret;
        }

        /* step 3: dev → pipe 绑定（一个 dev 可以喂多个 pipe，WDR 时一个 VC 进一个 pipe） */
        dev_bind_pipe.num = 1;
        dev_bind_pipe.pipe_id[0] = cfg->pipe;
        ret = hi_mpi_vi_set_dev_bind_pipe(cfg->vi_dev, &dev_bind_pipe);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vi_set_dev_bind_pipe(dev %d, pipe %d) failed with %#x!\n",
                            cfg->vi_dev, cfg->pipe, ret);
            goto disable_dev;
        }

        /* step 4: 建 pipe 并启动。pipe 是 ISP/3A 的载体；YUV 直出要 isp_bypass = HI_TRUE，
           而且 pixel_format 要和输入对齐（YUV422 输入 → SEMIPLANAR_422）。 */
        memset(&pipe_attr, 0, sizeof(pipe_attr));
        pipe_attr.pipe_bypass_mode = HI_VI_PIPE_BYPASS_NONE;
        pipe_attr.isp_bypass       = HI_TRUE;
        pipe_attr.size.width       = cfg->size.width;
        pipe_attr.size.height      = cfg->size.height;
        pipe_attr.pixel_format     = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_422;
        pipe_attr.compress_mode    = HI_COMPRESS_MODE_NONE;
        pipe_attr.bit_width        = HI_DATA_BIT_WIDTH_8;
        pipe_attr.frame_rate_ctrl.src_frame_rate = -1;   /* -1 = 跟随输入帧率 */
        pipe_attr.frame_rate_ctrl.dst_frame_rate = -1;
        pipe_attr.depth            = cfg->pipe_depth;

        ret = hi_mpi_vi_create_pipe(cfg->pipe, &pipe_attr);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vi_create_pipe(%d) failed with %#x!\n", cfg->pipe, ret);
            goto disable_dev;
        }

        ret = hi_mpi_vi_start_pipe(cfg->pipe);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vi_start_pipe(%d) failed with %#x!\n", cfg->pipe, ret);
            goto destroy_pipe;
        }

        /* step 5: 物理通道，产出 YUV 给 VPSS 或用户态取帧 */
        memset(&chn_attr, 0, sizeof(chn_attr));
        chn_attr.size.width    = cfg->size.width;
        chn_attr.size.height   = cfg->size.height;
        chn_attr.pixel_format  = cfg->chn_pixel_format;
        chn_attr.dynamic_range = HI_DYNAMIC_RANGE_SDR8;
        chn_attr.video_format  = HI_VIDEO_FORMAT_LINEAR;
        chn_attr.compress_mode = HI_COMPRESS_MODE_NONE;
        chn_attr.depth         = cfg->chn_depth;
        chn_attr.frame_rate_ctrl.src_frame_rate = -1;
        chn_attr.frame_rate_ctrl.dst_frame_rate = -1;

        ret = hi_mpi_vi_set_chn_attr(cfg->pipe, cfg->chn, &chn_attr);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vi_set_chn_attr(pipe %d, chn %d) failed with %#x!\n",
                            cfg->pipe, cfg->chn, ret);
            goto stop_pipe;
        }

        ret = hi_mpi_vi_enable_chn(cfg->pipe, cfg->chn);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_vi_enable_chn(pipe %d, chn %d) failed with %#x!\n",
                            cfg->pipe, cfg->chn, ret);
            goto stop_pipe;
        }

        UAV_LOG_INFO_F("vi dev %d pipe %d chn %d ready: %ux%u, %s\n",
                       cfg->vi_dev, cfg->pipe, cfg->chn,
                       cfg->size.width, cfg->size.height,
                       cfg->vpss_offline ? "offline(VI+VPSS)" : "online(VI+VPSS)");
        return HI_SUCCESS;

        /* 失败回滚：倒着收，避免留下半建成的 pipe/dev */
        stop_pipe:
        hi_mpi_vi_stop_pipe(cfg->pipe);
        destroy_pipe:
        hi_mpi_vi_destroy_pipe(cfg->pipe);
        disable_dev:
        hi_mpi_vi_disable_dev(cfg->vi_dev);
        return ret;
    }

    // 拆 VI：顺序和建的时候相反（chn → pipe → dev）。
    // 调用方要先 vi_un_bind_vpss 解绑、并停掉下游 VPSS/VENC，再调这里。
    hi_s32 mipi_ctl::vi_deinit(const vi_cfg *cfg)
    {
        hi_s32 ret;
        hi_s32 first_err = HI_SUCCESS;

        if (cfg == nullptr) {
            UAV_LOG_ERROR_F("vi_deinit: cfg is null\n");
            return HI_FAILURE;
        }

        ret = hi_mpi_vi_disable_chn(cfg->pipe, cfg->chn);
        if (ret != HI_SUCCESS) {
            UAV_LOG_WARN_F("hi_mpi_vi_disable_chn(pipe %d, chn %d) failed with %#x\n",
                           cfg->pipe, cfg->chn, ret);
            first_err = ret;
        }

        ret = hi_mpi_vi_stop_pipe(cfg->pipe);
        if (ret != HI_SUCCESS) {
            UAV_LOG_WARN_F("hi_mpi_vi_stop_pipe(%d) failed with %#x\n", cfg->pipe, ret);
            if (first_err == HI_SUCCESS) {
                first_err = ret;
            }
        }

        ret = hi_mpi_vi_destroy_pipe(cfg->pipe);
        if (ret != HI_SUCCESS) {
            UAV_LOG_WARN_F("hi_mpi_vi_destroy_pipe(%d) failed with %#x\n", cfg->pipe, ret);
            if (first_err == HI_SUCCESS) {
                first_err = ret;
            }
        }

        ret = hi_mpi_vi_disable_dev(cfg->vi_dev);
        if (ret != HI_SUCCESS) {
            UAV_LOG_WARN_F("hi_mpi_vi_disable_dev(%d) failed with %#x\n", cfg->vi_dev, ret);
            if (first_err == HI_SUCCESS) {
                first_err = ret;
            }
        }

        UAV_LOG_INFO_F("vi dev %d pipe %d chn %d deinit done\n", cfg->vi_dev, cfg->pipe, cfg->chn);
        return first_err;
    }

    hi_s32 mipi_ctl::vpss_init(hi_vpss_grp vpss_grp, sample_venc_vpss_chn_attr *vpss_chn_cfg)
    {
        hi_s32 ret;
        hi_vpss_chn vpss_chn;
        /* = {} 是值初始化：整个结构（含数组元素）全部清零。海思的 attr 结构字段很多，
           写成 {(hi_bool) 0} 只初始化第一个成员，还会触发 -Wmissing-field-initializers。 */
        hi_vpss_grp_attr grp_attr = {};
        hi_vpss_chn_attr chn_attr[HI_VPSS_MAX_CHN_NUM] = {};

        /* 组级最大尺寸：优先用调用方给的 max_size（0 = 未设置），没给就回退到 chn0 的输出尺寸。
           以前这里直接取 output_size[0]，导致 max_size 字段是死代码；如果哪天只启用 chn1、
           或者 chn0 的输出比输入大，组尺寸就会取错。 */
        grp_attr.max_width  = (vpss_chn_cfg->max_size.width != 0)
                                  ? vpss_chn_cfg->max_size.width
                                  : vpss_chn_cfg->output_size[0].width;
        grp_attr.max_height = (vpss_chn_cfg->max_size.height != 0)
                                  ? vpss_chn_cfg->max_size.height
                                  : vpss_chn_cfg->output_size[0].height;
        grp_attr.nr_en      = HI_FALSE;//cht change to true
        grp_attr.pixel_format = vpss_chn_cfg->pixel_format;
        grp_attr.dynamic_range = HI_DYNAMIC_RANGE_SDR8;
        grp_attr.frame_rate.src_frame_rate = -1;
        grp_attr.frame_rate.dst_frame_rate = -1;

        UAV_LOG_INFO_F("vpss_grp_max_width   %d\n", grp_attr.max_width);
        UAV_LOG_INFO_F("vpss_grp_max_height   %d\n", grp_attr.max_height);
        UAV_LOG_INFO_F("vpss_grp_pixel_format   %d\n", grp_attr.pixel_format);

        for (vpss_chn = 0; vpss_chn < (hi_vpss_chn) HI_VPSS_MAX_CHN_NUM; vpss_chn++) {
            if (vpss_chn_cfg->enable[vpss_chn] == 1) {
                chn_attr[vpss_chn].width = vpss_chn_cfg->output_size[vpss_chn].width;
                chn_attr[vpss_chn].height = vpss_chn_cfg->output_size[vpss_chn].height;
                chn_attr[vpss_chn].chn_mode = HI_VPSS_CHN_MODE_USER;
                chn_attr[vpss_chn].compress_mode = vpss_chn_cfg->compress_mode[vpss_chn];
                chn_attr[vpss_chn].pixel_format = vpss_chn_cfg->pixel_format;
                chn_attr[vpss_chn].video_format = HI_VIDEO_FORMAT_LINEAR;
                chn_attr[vpss_chn].frame_rate.src_frame_rate = -1;
                chn_attr[vpss_chn].frame_rate.dst_frame_rate = -1;
                chn_attr[vpss_chn].depth = 1;
                chn_attr[vpss_chn].mirror_en = (hi_bool) 0;
                chn_attr[vpss_chn].flip_en = (hi_bool) 0;
                chn_attr[vpss_chn].aspect_ratio.mode                 = HI_ASPECT_RATIO_AUTO;
                chn_attr[vpss_chn].aspect_ratio.video_rect.width     = 0;
                chn_attr[vpss_chn].aspect_ratio.video_rect.height    = 0;
                chn_attr[vpss_chn].aspect_ratio.video_rect.x         = 0;
                chn_attr[vpss_chn].aspect_ratio.video_rect.y         = 0;
                chn_attr[vpss_chn].aspect_ratio.bg_color             = 0;

                UAV_LOG_INFO_F("vpss_chn_attr[%d].width  %d\n", vpss_chn, chn_attr[vpss_chn].width);
                UAV_LOG_INFO_F("vpss_chn_attr[%d].height   %d\n", vpss_chn, chn_attr[vpss_chn].height);
                UAV_LOG_INFO_F("vpss_chn_pixel_format[%d]   %d\n", vpss_chn, chn_attr[vpss_chn].pixel_format);
            }
        }

        ret = vpss_start(vpss_grp, vpss_chn_cfg->enable, &grp_attr, chn_attr, HI_VPSS_MAX_CHN_NUM);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("failed with %#x!\n", ret);
        }

        return ret;
    }

    hi_s32 mipi_ctl::vdec_bind_vpss(hi_vdec_chn vdec_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn)
    {
        hi_mpp_chn src_chn;
        hi_mpp_chn dest_chn;

        src_chn.mod_id = HI_ID_VDEC;
        src_chn.dev_id = 0;
        src_chn.chn_id = vdec_chn;

        dest_chn.mod_id = HI_ID_VPSS;
        dest_chn.dev_id = vpss_grp;
        dest_chn.chn_id = vpss_chn;

        hi_s32 ret = hi_mpi_sys_bind(&src_chn, &dest_chn);
        if(ret != 0)
        {
            UAV_LOG_ERROR_F("hi_mpi_sys_bind failed  %#x!\n", ret);
            return ret;
        }

        UAV_LOG_INFO_F("GOOD: bind vdec pipe %d chn %d to vpss group %d chn %d\n",
                          src_chn.dev_id, src_chn.chn_id, dest_chn.dev_id, dest_chn.chn_id);
        return HI_SUCCESS;
    }

    // VDEC 没有 dev 概念，dev_id 固定 0（与上面 bind 侧、以及 8.11 的
    // mpp_comm_vdec_bind_vpss 一致）。原来这里写的是 vdec_pipe，和 bind 侧对不上 ——
    // hi_mpi_sys_bind/unbind 的 hi_mpp_chn 必须完全一致，否则 unbind 一定失败。
    hi_s32 mipi_ctl::vdec_un_bind_vpss(hi_vdec_chn vdec_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn)
    {
        hi_mpp_chn src_chn;
        hi_mpp_chn dest_chn;

        src_chn.mod_id = HI_ID_VDEC;
        src_chn.dev_id = 0;
        src_chn.chn_id = vdec_chn;

        dest_chn.mod_id = HI_ID_VPSS;
        dest_chn.dev_id = vpss_grp;
        dest_chn.chn_id = vpss_chn;

        hi_s32 ret = hi_mpi_sys_unbind(&src_chn, &dest_chn);
        if(ret != 0)
        {
            UAV_LOG_ERROR_F("hi_mpi_sys_unbind failed  %#x!\n", ret);
            return ret;
        }
        return HI_SUCCESS;
    }

    hi_void mipi_ctl::venc_vpss_deinit(hi_vpss_grp vpss_grp, sample_venc_vpss_chn_attr *vpss_chan_cfg)
    {
        hi_s32 ret;

        ret = vpss_stop(vpss_grp, vpss_chan_cfg->enable, HI_VPSS_MAX_CHN_NUM);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("failed with %#x!\n", ret);
        }
    }

    // ---- VENC 建通道 ----------------------------------------------------------
    // 海思 sample 层的 sample_comm_venc_create / venc_channel_param_init 本工程没有
    //（那份代码属于 SDK 示例，不在任何 API 头文件里），这里按 acl/dvpp/hi_dvpp_venc.h
    // 自己实现。目前支持 H.264 / H.265 的 CBR、VBR；其它码控模式显式返回失败并打日志，
    // 免得"看着建成功了、其实码控没配上"。
    static hi_s32 venc_create(const mipi_ctl::venc_chn_param *chn_param)
    {
        hi_s32 ret;
        hi_venc_chn_attr chn_attr;
        hi_u32 width;
        hi_u32 height;

        if (chn_param == nullptr) {
            UAV_LOG_ERROR_F("venc_create: chn_param is null\n");
            return HI_FAILURE;
        }

        width = chn_param->venc_size.width;
        height = chn_param->venc_size.height;

        /* hi_venc_chn_attr 里带 union、字段多，先整体清零再逐项赋值 */
        memset(&chn_attr, 0, sizeof(chn_attr));

        chn_attr.venc_attr.type           = chn_param->type;
        chn_attr.venc_attr.max_pic_width  = width;
        chn_attr.venc_attr.max_pic_height = height;
        chn_attr.venc_attr.pic_width      = width;
        chn_attr.venc_attr.pic_height     = height;
        chn_attr.venc_attr.profile        = chn_param->profile;
        chn_attr.venc_attr.is_by_frame    = HI_TRUE;   /* 按帧取码流 */
        /* 码流缓冲区：一帧 YUV420SP 对齐到 64 字节，压缩后肯定放得下 */
        chn_attr.venc_attr.buf_size = ALIGN_UP(width * height * 3 / 2, 64);
        /* 参考帧缓冲占比，取 8.11 工程里验证过的 70（清零点可能不被接受） */
        if (chn_param->type == HI_PT_H265) {
            chn_attr.venc_attr.h265_attr.frame_buf_ratio = 70;
        } else {
            chn_attr.venc_attr.h264_attr.frame_buf_ratio = 70;
        }

        chn_attr.gop_attr.gop_mode = HI_VENC_GOP_MODE_NORMAL_P;
        chn_attr.gop_attr.normal_p.ip_qp_delta = 2;

        chn_attr.rc_attr.rc_mode = chn_param->rc_mode;
        switch (chn_param->rc_mode) {
            case HI_VENC_RC_MODE_H264_CBR:
                chn_attr.rc_attr.h264_cbr.gop            = chn_param->gop;
                chn_attr.rc_attr.h264_cbr.stats_time     = chn_param->stats_time;
                chn_attr.rc_attr.h264_cbr.src_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h264_cbr.dst_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h264_cbr.bit_rate       = chn_param->bitrate;
                break;
            case HI_VENC_RC_MODE_H265_CBR:
                chn_attr.rc_attr.h265_cbr.gop            = chn_param->gop;
                chn_attr.rc_attr.h265_cbr.stats_time     = chn_param->stats_time;
                chn_attr.rc_attr.h265_cbr.src_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h265_cbr.dst_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h265_cbr.bit_rate       = chn_param->bitrate;
                break;
            case HI_VENC_RC_MODE_H264_VBR:
                chn_attr.rc_attr.h264_vbr.gop            = chn_param->gop;
                chn_attr.rc_attr.h264_vbr.stats_time     = chn_param->stats_time;
                chn_attr.rc_attr.h264_vbr.src_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h264_vbr.dst_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h264_vbr.max_bit_rate   = chn_param->bitrate;
                break;
            case HI_VENC_RC_MODE_H265_VBR:
                chn_attr.rc_attr.h265_vbr.gop            = chn_param->gop;
                chn_attr.rc_attr.h265_vbr.stats_time     = chn_param->stats_time;
                chn_attr.rc_attr.h265_vbr.src_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h265_vbr.dst_frame_rate = chn_param->frame_rate;
                chn_attr.rc_attr.h265_vbr.max_bit_rate   = chn_param->bitrate;
                break;
            default:
                /* FIXQP / QPMAP / AVBR / QVBR / CVBR / MJPEG 等暂未实现：
                   需要时照上面加 case（参数结构体也要补上对应字段，如 qp）。 */
                UAV_LOG_ERROR_F("venc_create: rc_mode %d not supported yet\n", chn_param->rc_mode);
                return HI_FAILURE;
        }

        ret = hi_mpi_venc_create_chn(chn_param->chn, &chn_attr);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_venc_create_chn[%d] failed with %#x!\n", chn_param->chn, ret);
            return ret;
        }

        UAV_LOG_INFO_F("venc chn %d created: type %d, %ux%u, %u fps, gop %u, %u kbps\n",
                       chn_param->chn, chn_param->type, width, height,
                       chn_param->frame_rate, chn_param->gop, chn_param->bitrate);
        return HI_SUCCESS;
    }

    hi_s32 mipi_ctl::venc_start(venc_chn_param *chn_param)
    {
        hi_s32 ret;
        hi_venc_start_param start_param;

        /* step 1: create encode chn */
        if ((ret = venc_create(chn_param)) != HI_SUCCESS) {
            UAV_LOG_ERROR_F("venc_create failed with %#x!\n", ret);
            return HI_FAILURE;
        }

        /* step 2:  start recv venc pictures */
        start_param.recv_pic_num = -1;
        if ((ret = hi_mpi_venc_start_chn(chn_param->chn, &start_param)) != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_venc_start_chn[%d] failed with %#x!\n", chn_param->chn, ret);
            return HI_FAILURE;
        }

        return HI_SUCCESS;
    }

    hi_s32 mipi_ctl::vpss_bind_venc(hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn, hi_venc_chn venc_chn)
    {
        hi_mpp_chn src_chn;
        hi_mpp_chn dest_chn;

        src_chn.mod_id = HI_ID_VPSS;
        src_chn.dev_id = vpss_grp;
        src_chn.chn_id = vpss_chn;

        dest_chn.mod_id = HI_ID_VENC;
        dest_chn.dev_id = 0;
        dest_chn.chn_id = venc_chn;

        hi_s32 ret = hi_mpi_sys_bind(&src_chn, &dest_chn);
        if(ret != 0)
        {
            UAV_LOG_ERROR_F("hi_mpi_sys_bind failed  %#x!\n", ret);
            return ret;
        }

        UAV_LOG_INFO_F("GOOD: bind vpss group %d chn %d to venc %d chn %d\n",
                          src_chn.dev_id, src_chn.chn_id, dest_chn.dev_id, dest_chn.chn_id);
        return HI_SUCCESS;
    }

    hi_s32 mipi_ctl::venc_stop(hi_venc_chn venc_chn)
    {
        hi_s32 ret;

        /* stop venc chn */
        ret = hi_mpi_venc_stop_chn(venc_chn);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_venc_stop_chn vechn[%d] failed with %#x!\n", venc_chn, ret);
        }

        /* distroy venc channel */
        ret = hi_mpi_venc_destroy_chn(venc_chn);
        if (ret != HI_SUCCESS) {
            UAV_LOG_ERROR_F("hi_mpi_venc_destroy_chn vechn[%d] failed with %#x!\n", venc_chn, ret);
            return HI_FAILURE;
        }
        return HI_SUCCESS;
    }

    hi_s32 mipi_ctl::vpss_un_bind_venc(hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn, hi_venc_chn venc_chn)
    {
        hi_mpp_chn src_chn;
        hi_mpp_chn dest_chn;

        src_chn.mod_id = HI_ID_VPSS;
        src_chn.dev_id = vpss_grp;
        src_chn.chn_id = vpss_chn;

        dest_chn.mod_id = HI_ID_VENC;
        dest_chn.dev_id = 0;
        dest_chn.chn_id = venc_chn;

        hi_s32 ret = hi_mpi_sys_unbind(&src_chn, &dest_chn);
        if(ret != 0)
        {
            UAV_LOG_ERROR_F("hi_mpi_sys_unbind failed  %#x!\n", ret);
            return ret;
        }

        return HI_SUCCESS;
    }

    hi_s32 mipi_ctl::vi_bind_vpss(hi_vi_pipe vi_pipe, hi_vi_chn vi_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn)
    {
        hi_mpp_chn src_chn;
        hi_mpp_chn dest_chn;

        src_chn.mod_id = HI_ID_VI;
        src_chn.dev_id = vi_pipe;
        src_chn.chn_id = vi_chn;

        dest_chn.mod_id = HI_ID_VPSS;
        dest_chn.dev_id = vpss_grp;
        dest_chn.chn_id = vpss_chn;

        hi_s32 ret = hi_mpi_sys_bind(&src_chn, &dest_chn);
        if(ret != 0)
        {
            UAV_LOG_ERROR_F("hi_mpi_sys_bind failed  %#x!\n", ret);
            return ret;
        }

        UAV_LOG_INFO_F("GOOD: bind vi dev_id %d chn %d to vpss group %d chn %d\n",
              src_chn.dev_id, src_chn.chn_id, dest_chn.dev_id, dest_chn.chn_id);
        return HI_SUCCESS;
    }

    hi_s32 mipi_ctl::vi_un_bind_vpss(hi_vi_pipe vi_pipe, hi_vi_chn vi_chn, hi_vpss_grp vpss_grp, hi_vpss_chn vpss_chn)
    {
        hi_mpp_chn src_chn;
        hi_mpp_chn dest_chn;

        src_chn.mod_id = HI_ID_VI;
        src_chn.dev_id = vi_pipe;
        src_chn.chn_id = vi_chn;

        dest_chn.mod_id = HI_ID_VPSS;
        dest_chn.dev_id = vpss_grp;
        dest_chn.chn_id = vpss_chn;

        hi_s32 ret = hi_mpi_sys_unbind(&src_chn, &dest_chn);
        if(ret != 0)
        {
            UAV_LOG_ERROR_F("hi_mpi_sys_unbind failed  %#x!\n", ret);
            return ret;
        }
        return HI_SUCCESS;
    }
