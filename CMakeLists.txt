# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# PROJECT_VER = 1.2-<git短SHA>[-dirty]，配置期从 git 读取（2026-10-27）。
# 背景：硬编码版本 + 增量构建不重编 esp_app_desc.c.obj，导致启动日志
# "Compile time"/版本号无法区分构建（板上镜像曾被"Oct 23"旧元数据误导）。
# --exclude 排除 v* tag（仓库有 v1.2，不排除会得到冗余的
# "1.2-gv1.2-114-g<sha>"），使 describe 回退为纯短 SHA。
# 版本串最终出现在：启动日志 App version、device.info.firmware、MQTT 遥测
# fw_version；esp_app_desc.version 上限 32 字符，本格式最长 17。
execute_process(
    COMMAND git describe --always --dirty --exclude=v*
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE _project_git_desc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _project_git_rc
)
if(_project_git_rc EQUAL 0 AND NOT "${_project_git_desc}" STREQUAL "")
    set(PROJECT_VER "1.2-${_project_git_desc}")
else()
    set(PROJECT_VER "1.2-unknown")
endif()
# .git/HEAD|.git/index 变化触发 cmake 重新配置：切分支/提交后增量构建也能刷新
# SHA（普通文件编辑不触发，代价可忽略）。无 .git 的导出树静默回退 1.2-unknown。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/.git/HEAD"
    "${CMAKE_CURRENT_LIST_DIR}/.git/index")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(iothub)
