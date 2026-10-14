#=============================================================================
# FindAscend.cmake —— 定位“当前系统”的昇腾（Ascend / CANN）编译环境
#
# 查找顺序（先命中者优先）：
#   1) -DAscend_ROOT=/path/to/ascend-toolkit/latest
#   2) 环境变量 $ASCEND_HOME_PATH   （执行 CANN 的 set_env.sh / setenv.bash 后由官方脚本设置）
#   3) 环境变量 $ASCEND_TOOLKIT_HOME
#   4) /usr/local/Ascend/ascend-toolkit/latest     系统级安装的默认位置
#   5) $HOME/Ascend/ascend-toolkit/latest          用户级安装的默认位置
#
# 架构子目录按“本次编译的目标架构” CMAKE_SYSTEM_PROCESSOR 选择：
#   aarch64 / arm64 -> aarch64-linux（兼容 arm64-linux）
#   x86_64  / amd64 -> x86_64-linux
# 两者不一致时（例如在 x86_64 宿主机上交叉编译到昇腾 aarch64 板卡）会给出明确报错，
# 提示改用 cmake/toolchain-ascend-aarch64.cmake。
#
# 输出变量：
#   Ascend_FOUND           是否找到可用的昇腾编译环境
#   ASCEND_HOME            CANN 根目录（即 ASCEND_HOME_PATH）
#   ASCEND_VERSION         CANN 运行时版本（读 version.cfg，可能为空）
#   ASCEND_ARCH_DIR        ${ASCEND_HOME}/<arch>-linux
#   ASCEND_INCLUDE_DIR     ${ASCEND_ARCH_DIR}/include（顶层：aclnn/、ge/、utils/…）
#   ASCEND_INCLUDE_DIRS    ASCEND_INCLUDE_DIR + 三个子目录（见下方“为什么要把子目录也加进来”）
#   ASCEND_LIBRARY_DIR     ${ASCEND_ARCH_DIR}/lib64
#   ASCEND_ACL_LIBRARY     libascendcl.so 的绝对路径
#   ASCEND_MEDIA_LIBRARIES libacl_dvpp*/vi_mpi/vpss_mpi/vo_mpi/hdmi_mpi/isp_*/tde_mpi 中
#                          实际找到的那些（绝对路径列表，见下方“头文件 ↔ 库 对应关系”）
#
# 头文件 ↔ 库 对应关系（用 aarch64-linux-gnu-nm -D 逐个库对账得出的，不是按名字猜的）：
#   hi_mpi_sys.h        → libacl_dvpp_mpi.so   (hi_mpi_sys_*)
#   hi_region.h         → libacl_dvpp_mpi.so   (hi_mpi_rgn_*)
#   hi_dvpp.h（聚合头） → libacl_dvpp_mpi.so + libacl_dvpp.so
#                         · hi_dvpp_vpc/venc/vdec/sys.h 的 hi_mpi_vpc_*/venc_*/vdec_*/sys_*
#                           → libacl_dvpp_mpi.so
#                         · hi_dvpp_*.h 里的 ACL 风格接口 acldvpp*/aclvenc*
#                           → libacl_dvpp.so（它自己 NEEDED libascendcl/libacl_dvpp_mpi）
#   hi_mpi_vi.h         → libacl_vi_mpi.so
#   hi_mpi_vpss.h       → libacl_vpss_mpi.so
#   hi_mpi_vo.h         → libacl_vo_mpi.so
#   hi_mpi_hdmi.h       → libacl_hdmi_mpi.so
#   hi_mipi_rx.h        → 无库：只声明结构体/枚举 + ioctl 命令，走内核驱动
#   hi_mpi_isp.h 等     → libacl_isp_mpi.so / libacl_isp_ae_mpi.so / libacl_isp_awb_mpi.so
#                         （MIPI 接 raw sensor 时才会用到，随包一起找出来备用）
# 注意：hi_dvpp_vb.h 的 hi_mpi_vb_*（VB 池）以及 hi_mpi_tde_* / hi_mpi_mipi_* 在 CANN 的
# lib64 里没有任何库导出，属板卡运行时侧提供，这里不链接；真用到时会明确报 undefined reference。
#
# 为什么要把子目录也加进来（写成 #include "acl.h" / #include "hi_mpi_vi.h" 的前提）：
#   CANN 的头文件不是平铺在 include/ 下，而是分在三个子目录里：
#     include/acl/acl.h                                    → acl/acl.h
#     include/acl/dvpp/{hi_mpi_sys.h, hi_dvpp.h, hi_region.h, …}
#     include/acl/media/{hi_mpi_vi.h, hi_mpi_vpss.h, hi_mpi_vo.h,
#                        hi_mpi_hdmi.h, hi_mipi_rx.h, …}
#   只导出顶层 include/ 时，`#include "acl.h"` 会在编译期直接 fatal error:
#   "acl.h: No such file or directory"（头文件里的相对 include，如 hi_mpi_sys.h 引
#   hi_media_common.h、hi_mpi_vi.h 引 hi_common_vi.h，也是靠同一目录内的文件互相
#   引用，所以这几个目录必须同时在场）。
#
# 导入目标：
#   Ascend::ascendcl        ACL 运行时（头文件 + libascendcl + 链接选项）
#   Ascend::acl_vi_mpi 等   每个媒体库一个（头文件 + 对应 libXXX.so + 链接选项），按需精确链接
#   Ascend::ascend_media    聚合目标：上面这些库的合集，使用方一般只链它一个即可
#=============================================================================

# ---- 1. 收集候选 CANN 根目录 ------------------------------------------------
set(_ascend_home_candidates "")

if(Ascend_ROOT)
  list(APPEND _ascend_home_candidates "${Ascend_ROOT}")
endif()
foreach(_env_var ASCEND_HOME_PATH ASCEND_TOOLKIT_HOME ASCEND_HOME)
  if(DEFINED ENV{${_env_var}} AND NOT "$ENV{${_env_var}}" STREQUAL "")
    list(APPEND _ascend_home_candidates "$ENV{${_env_var}}")
  endif()
endforeach()
list(APPEND _ascend_home_candidates
  "/usr/local/Ascend/ascend-toolkit/latest"
  "$ENV{HOME}/Ascend/ascend-toolkit/latest"
  "/usr/local/Ascend/latest")

list(REMOVE_DUPLICATES _ascend_home_candidates)

# ---- 2. 目标架构对应的 CANN 子目录名 ---------------------------------------
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _ascend_proc)
if(_ascend_proc MATCHES "^(aarch64|arm64)")
  set(_ascend_arch_dirs aarch64-linux arm64-linux)
elseif(_ascend_proc MATCHES "^(x86_64|amd64)")
  set(_ascend_arch_dirs x86_64-linux)
else()
  set(_ascend_arch_dirs "${_ascend_proc}-linux")
endif()

# ---- 3. 逐个候选目录探测：必须同时具备 acl 头文件 ---------------------------------
set(ASCEND_ARCH_DIR "")
foreach(_home IN LISTS _ascend_home_candidates)
  foreach(_arch_dir IN LISTS _ascend_arch_dirs)
    if(EXISTS "${_home}/${_arch_dir}/include/acl/acl.h")
      set(ASCEND_HOME "${_home}")
      set(ASCEND_ARCH_DIR "${_home}/${_arch_dir}")
      break()
    endif()
  endforeach()
  if(ASCEND_ARCH_DIR)
    break()
  endif()
endforeach()

# ---- 4. 头文件 / 库 / 版本 ---------------------------------------------------
if(ASCEND_ARCH_DIR)
  set(ASCEND_INCLUDE_DIR "${ASCEND_ARCH_DIR}/include")

  # 顶层 + 三个子目录（acl.h 在 acl/，DVPP 与媒体 MPI 头分别在 acl/dvpp、acl/media）。
  # 只加入真实存在的目录：不同 CANN 版本的子目录集合略有差异（例如有的版本
  # acl/media 下还软链了一份 hi_mpi_sys.h），不存在的目录不写进 -I，避免污染搜索顺序。
  set(ASCEND_INCLUDE_DIRS "")
  foreach(_inc "${ASCEND_INCLUDE_DIR}" "${ASCEND_INCLUDE_DIR}/acl"
               "${ASCEND_INCLUDE_DIR}/acl/dvpp" "${ASCEND_INCLUDE_DIR}/acl/media")
    if(IS_DIRECTORY "${_inc}")
      list(APPEND ASCEND_INCLUDE_DIRS "${_inc}")
    endif()
  endforeach()
  if(NOT ASCEND_INCLUDE_DIRS)
    set(ASCEND_INCLUDE_DIRS "${ASCEND_INCLUDE_DIR}")
  endif()

  # CANN 目录里放的是“目标架构”的库，但它们位于宿主机路径上（交叉编译时
  # CMAKE_FIND_ROOT_PATH 会给所有搜索路径重新加前缀），因此这里显式不参与
  # root path 重写。
  find_library(ASCEND_ACL_LIBRARY
    NAMES ascendcl
    PATHS "${ASCEND_ARCH_DIR}/lib64" "${ASCEND_ARCH_DIR}/lib"
          "${ASCEND_HOME}/lib64" "${ASCEND_HOME}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH)

  if(ASCEND_ACL_LIBRARY)
    get_filename_component(ASCEND_LIBRARY_DIR "${ASCEND_ACL_LIBRARY}" DIRECTORY)
  endif()

  # ---- 4b. DVPP / 媒体 MPI 库（头文件 ↔ 库 的对应关系见文件头注释）----------
  # 顺序 = 链接顺序，被依赖者放后面（libacl_dvpp.so NEEDED libacl_dvpp_mpi.so）。
  set(ASCEND_MEDIA_LIB_NAMES
    acl_dvpp           # acldvpp*/aclvenc*（hi_dvpp.h 里的 ACL 风格接口）
    acl_dvpp_mpi       # hi_mpi_sys_* / hi_mpi_rgn_* / hi_mpi_vpc_* / hi_mpi_venc_* / hi_mpi_vdec_*
    acl_vi_mpi         # hi_mpi_vi_*
    acl_vpss_mpi       # hi_mpi_vpss_*
    acl_vo_mpi         # hi_mpi_vo_*
    acl_hdmi_mpi       # hi_mpi_hdmi_*
    acl_isp_mpi        # hi_mpi_isp_*
    acl_isp_ae_mpi     # hi_mpi_isp_ae_*
    acl_isp_awb_mpi    # hi_mpi_isp_awb_*
    acl_tde_mpi)       # 2D 加速

  set(ASCEND_MEDIA_LIBRARIES "")
  set(ASCEND_MEDIA_LIBRARIES_MISSING "")
  foreach(_name IN LISTS ASCEND_MEDIA_LIB_NAMES)
    find_library(ASCEND_LIB_${_name}
      NAMES ${_name}
      PATHS "${ASCEND_LIBRARY_DIR}" "${ASCEND_ARCH_DIR}/lib64" "${ASCEND_ARCH_DIR}/lib"
      NO_DEFAULT_PATH
      NO_CMAKE_FIND_ROOT_PATH)
    if(ASCEND_LIB_${_name})
      list(APPEND ASCEND_MEDIA_LIBRARIES "${ASCEND_LIB_${_name}}")
    else()
      # 不设 REQUIRED：只装了 CANN runtime 的机器上这些库可能不全，缺哪个由使用方
      # 在链接期看到明确的 undefined reference，而不是在这里直接配置失败。
      list(APPEND ASCEND_MEDIA_LIBRARIES_MISSING "lib${_name}.so")
    endif()
  endforeach()
  if(ASCEND_MEDIA_LIBRARIES_MISSING)
    message(WARNING "CANN 里缺少这些库（链接期若用到会报 undefined reference）: "
                    "${ASCEND_MEDIA_LIBRARIES_MISSING}")
  endif()

  # 版本号（CANN 各版本 version.cfg 格式一致，取 runtime_installed_version）
  foreach(_cfg "${ASCEND_ARCH_DIR}/../version.cfg" "${ASCEND_HOME}/version.cfg")
    if(NOT ASCEND_VERSION AND EXISTS "${_cfg}")
      file(STRINGS "${_cfg}" _line REGEX "runtime_installed_version")
      if(_line MATCHES "\\[([^]]+)\\]")
        set(ASCEND_VERSION "${CMAKE_MATCH_1}")
      endif()
    endif()
  endforeach()
endif()

# ---- 5. 未找到时，给出“本机到底装了什么架构”的提示 ---------------------------
if(NOT ASCEND_ARCH_DIR)
  set(_ascend_installed_arches "")
  foreach(_home IN LISTS _ascend_home_candidates)
    file(GLOB _arch_candidates "${_home}/*-linux")
    foreach(_dir IN LISTS _arch_candidates)
      if(EXISTS "${_dir}/include/acl/acl.h")
        # 用真实路径取名，避免 arm64-linux -> aarch64-linux 这类软链接重复列出
        get_filename_component(_real "${_dir}" REALPATH)
        get_filename_component(_name "${_real}" NAME)
        list(APPEND _ascend_installed_arches "${_name} (${_home})")
      endif()
    endforeach()
  endforeach()
  if(_ascend_installed_arches)
    list(REMOVE_DUPLICATES _ascend_installed_arches)
    string(REPLACE ";" "\n    " _ascend_installed_arches "${_ascend_installed_arches}")
    string(CONCAT _ascend_arch_hint
      "当前系统的昇腾环境架构与本次编译的目标架构（${CMAKE_SYSTEM_PROCESSOR}）不一致。\n"
      "本机已安装的 CANN：\n    ${_ascend_installed_arches}\n"
      "如果目标就是昇腾 aarch64 板卡，请改用交叉编译：cmake --preset ascend-aarch64\n"
      "（等价于 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ascend-aarch64.cmake）\n"
      "如果本次确实不需要昇腾环境，请配置 -DUAV_ASCEND=OFF")
  else()
    string(REPLACE ";" "\n    " _ascend_home_candidates "${_ascend_home_candidates}")
    string(CONCAT _ascend_arch_hint
      "未在本机找到可用的 CANN 安装（已查找：\n    ${_ascend_home_candidates}）。\n"
      "可以手动指定：-DAscend_ROOT=/path/to/ascend-toolkit/latest，\n"
      "或配置 -DUAV_ASCEND=OFF 关闭昇腾编译环境。")
  endif()
endif()

# ---- 6. 标准 find_package 收尾 ----------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ascend
  REQUIRED_VARS ASCEND_INCLUDE_DIR ASCEND_ACL_LIBRARY
  VERSION_VAR   ASCEND_VERSION
  FAIL_MESSAGE  "${_ascend_arch_hint}")

mark_as_advanced(ASCEND_INCLUDE_DIR ASCEND_ACL_LIBRARY ASCEND_LIBRARY_DIR
                 ASCEND_MEDIA_LIBRARIES)

# ---- 7. 导入目标 ------------------------------------------------------------
if(Ascend_FOUND)
  # 共享库的“传递依赖”里有板卡侧才有的库：例如 libacl_dvpp_mpi.so NEEDED
  # libascend_hal.so、libascendcl.so 指向 libge_runner 等，本机 lib64 里并不齐；
  # 这里放行共享库的未定义符号，不影响本工程自身目标文件的符号检查
  # （真正用错/用缺库时，链接本工程目标文件那一步仍会报 undefined reference）。
  set(_ascend_allow_undefined "-Wl,--allow-shlib-undefined")

  if(NOT TARGET Ascend::ascendcl)
    add_library(Ascend::ascendcl SHARED IMPORTED)
    set_target_properties(Ascend::ascendcl PROPERTIES
      IMPORTED_LOCATION             "${ASCEND_ACL_LIBRARY}"
      # 必须带上 acl / acl/dvpp / acl/media 三个子目录，否则 "acl.h"、"hi_mpi_vi.h" 这类
      # 写法的 #include 编译期就会报 No such file or directory（详见文件头部说明）。
      INTERFACE_INCLUDE_DIRECTORIES "${ASCEND_INCLUDE_DIRS}"
      INTERFACE_LINK_OPTIONS        "${_ascend_allow_undefined}")
  endif()

  # 每个媒体库一个导入目标：Ascend::acl_vi_mpi、Ascend::acl_vpss_mpi …（按需精确链接）
  set(_ascend_media_targets "")
  foreach(_lib IN LISTS ASCEND_MEDIA_LIBRARIES)
    get_filename_component(_stem "${_lib}" NAME_WE)          # libacl_vi_mpi -> libacl_vi_mpi
    string(REGEX REPLACE "^lib" "" _stem "${_stem}")         # -> acl_vi_mpi
    if(NOT TARGET Ascend::${_stem})
      add_library(Ascend::${_stem} SHARED IMPORTED)
      set_target_properties(Ascend::${_stem} PROPERTIES
        IMPORTED_LOCATION             "${_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${ASCEND_INCLUDE_DIRS}"
        INTERFACE_LINK_OPTIONS        "${_ascend_allow_undefined}")
    endif()
    list(APPEND _ascend_media_targets "Ascend::${_stem}")
  endforeach()

  # 聚合目标：一次拿到 ACL + 全部 DVPP/媒体 MPI 库（使用方一般只链这一个）。
  # 它们是共享库，链接器默认 --as-needed：没被调用的库不会写进 NEEDED，
  # 所以现在就链上不会有副作用，等真调用了对应接口才会成为真实依赖。
  if(NOT TARGET Ascend::ascend_media)
    add_library(Ascend::ascend_media INTERFACE IMPORTED)
    set_target_properties(Ascend::ascend_media PROPERTIES
      INTERFACE_LINK_LIBRARIES      "Ascend::ascendcl;${_ascend_media_targets}"
      INTERFACE_INCLUDE_DIRECTORIES "${ASCEND_INCLUDE_DIRS}"
      INTERFACE_LINK_OPTIONS        "${_ascend_allow_undefined}")
  endif()
endif()
