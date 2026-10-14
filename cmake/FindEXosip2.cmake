#=============================================================================
# FindEXosip2.cmake —— 定位 libeXosip2（GB28181 的 SIP 信令栈）
#
# libeXosip2 依赖 libosip2（并提供两个库：libosip2 + libosipparser2），
# 二者都必须找到，缺一会在链接期报大量 undefined reference。
#
# 查找顺序（先命中者优先）：
#   1) -DEXosip2_ROOT=/path/to/libeXosip2  （同时可用 -DOsip2_ROOT 指定 libosip2）
#   2) 环境变量 $EXOSIP2_ROOT / $OSIP2_ROOT
#   3) 仓库内 third_party/libeXosip2、third_party/libosip2（默认，交叉编译 aarch64 静态库）
#   4) 系统安装位置（/usr、/usr/local，便于宿主机原生调试）
#
# 注意：这两个库在 third_party 下是“目标架构”的静态库，但位于宿主机路径上。
# 交叉编译时 CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY 会给所有搜索路径加上 sysroot
# 前缀，导致找不到，因此这里统一使用 NO_CMAKE_FIND_ROOT_PATH（与 FindAscend.cmake 同理）。
#
# 输出变量：
#   EXosip2_FOUND           是否找到
#   EXOSIP2_INCLUDE_DIR     eXosip2/ 的上一级目录（#include <eXosip2/eXosip.h>）
#   OSIP2_INCLUDE_DIR       osip2/ 与 osipparser2/ 的上一级目录
#   EXOSIP2_LIBRARY         libeXosip2.a 的绝对路径
#   OSIP2_LIBRARIES         libosip2.a + libosipparser2.a
#
# 导入目标：
#   EXosip2::EXosip2        头文件 + 两个静态库 + 线程库（含传递依赖）
#=============================================================================

find_package(Threads REQUIRED)

# ---- 1. 候选根目录 ----------------------------------------------------------
set(_exosip2_root_candidates "")
set(_osip2_root_candidates "")

if(EXosip2_ROOT)
  list(APPEND _exosip2_root_candidates "${EXosip2_ROOT}")
endif()
if(Osip2_ROOT)
  list(APPEND _osip2_root_candidates "${Osip2_ROOT}")
endif()
foreach(_env_var EXOSIP2_ROOT)
  if(DEFINED ENV{${_env_var}} AND NOT "$ENV{${_env_var}}" STREQUAL "")
    list(APPEND _exosip2_root_candidates "$ENV{${_env_var}}")
  endif()
endforeach()
foreach(_env_var OSIP2_ROOT)
  if(DEFINED ENV{${_env_var}} AND NOT "$ENV{${_env_var}}" STREQUAL "")
    list(APPEND _osip2_root_candidates "$ENV{${_env_var}}")
  endif()
endforeach()
list(APPEND _exosip2_root_candidates "${PROJECT_SOURCE_DIR}/third_party/libeXosip2")
list(APPEND _osip2_root_candidates   "${PROJECT_SOURCE_DIR}/third_party/libosip2")

list(REMOVE_DUPLICATES _exosip2_root_candidates)
list(REMOVE_DUPLICATES _osip2_root_candidates)

# ---- 2. 头文件 --------------------------------------------------------------
find_path(EXOSIP2_INCLUDE_DIR
  NAMES eXosip2/eXosip.h
  PATHS ${_exosip2_root_candidates}
  PATH_SUFFIXES include
  NO_CMAKE_FIND_ROOT_PATH)

find_path(OSIP2_INCLUDE_DIR
  NAMES osip2/osip.h osipparser2/osip_parser.h
  PATHS ${_osip2_root_candidates}
  PATH_SUFFIXES include
  NO_CMAKE_FIND_ROOT_PATH)

# ---- 3. 静态库 --------------------------------------------------------------
# 先按上一步命中的根目录找；找不到再退回系统搜索路径（宿主机原生调试场景）
find_library(EXOSIP2_LIBRARY
  NAMES eXosip2 libeXosip2
  PATHS ${_exosip2_root_candidates}
  PATH_SUFFIXES lib lib64
  NO_CMAKE_FIND_ROOT_PATH)

find_library(OSIP2_CORE_LIBRARY
  NAMES osip2 libosip2
  PATHS ${_osip2_root_candidates}
  PATH_SUFFIXES lib lib64
  NO_CMAKE_FIND_ROOT_PATH)

find_library(OSIP2_PARSER_LIBRARY
  NAMES osipparser2 libosipparser2
  PATHS ${_osip2_root_candidates}
  PATH_SUFFIXES lib lib64
  NO_CMAKE_FIND_ROOT_PATH)

if(OSIP2_CORE_LIBRARY AND OSIP2_PARSER_LIBRARY)
  set(OSIP2_LIBRARIES "${OSIP2_CORE_LIBRARY}" "${OSIP2_PARSER_LIBRARY}")
endif()

# ---- 4. 未找到时的提示 ------------------------------------------------------
if(NOT EXOSIP2_INCLUDE_DIR OR NOT EXOSIP2_LIBRARY OR NOT OSIP2_LIBRARIES)
  string(CONCAT _exosip2_hint
    "未找到可用的 libeXosip2 / libosip2。\n"
    "仓库内的 aarch64 静态库需要先交叉编译生成（两条命令按顺序执行）：\n"
    "    ./third_party/libosip2/build-aarch64.sh\n"
    "    ./third_party/libeXosip2/build-aarch64.sh\n"
    "或指定已有安装路径：-DEXosip2_ROOT=/path -DOsip2_ROOT=/path\n"
    "完全不需要 GB28181 时：-DUAV_GB28181=OFF")
endif()

# ---- 5. 标准 find_package 收尾 ----------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EXosip2
  REQUIRED_VARS EXOSIP2_INCLUDE_DIR EXOSIP2_LIBRARY OSIP2_LIBRARIES OSIP2_INCLUDE_DIR
  FAIL_MESSAGE  "${_exosip2_hint}")

mark_as_advanced(EXOSIP2_INCLUDE_DIR EXOSIP2_LIBRARY
                 OSIP2_INCLUDE_DIR OSIP2_CORE_LIBRARY OSIP2_PARSER_LIBRARY)

# ---- 6. 导入目标 ------------------------------------------------------------
if(EXosip2_FOUND AND NOT TARGET EXosip2::EXosip2)
  add_library(EXosip2::EXosip2 STATIC IMPORTED)
  set_target_properties(EXosip2::EXosip2 PROPERTIES
    IMPORTED_LOCATION             "${EXOSIP2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${EXOSIP2_INCLUDE_DIR};${OSIP2_INCLUDE_DIR}"
    # 静态库的传递依赖：两个 osip 库 + pthread（eXosip 内置线程/信号量支持）
    INTERFACE_LINK_LIBRARIES      "${OSIP2_LIBRARIES};Threads::Threads")
endif()
