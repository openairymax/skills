# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Airymax 技能插件构建（独立于 agentrt 主构建体系）
#
# 构建产物：libairy_skill_<name>.so，部署到 tool_d 插件域的扫描目录
# （$AIRY_HOME/ecosystem/plugins/<name>/，随 manifest.yaml 一起）。
#
# 用法：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# 插件 ABI 头文件来自 tool_d（plugin_service.h；0.1.9 M4 起插件执行域
# 由 plugin_d 并入 tool_d），仅编译期依赖，运行期只依赖系统 libcjson
# （tool_d 亦链接同一共享库）。

cmake_minimum_required(VERSION 3.16)
project(airymax_skills_plugins C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 插件 ABI 头文件（tool_d 提供 plugin_service.h / plugin_discovery.h）。
# 0.1.6 P2-1 独立组装修复：头文件仅编译期依赖，经 find_path 柔性探测——
#   1) 显式 -DTOOL_D_INCLUDE_DIR=<path>（独立部署指向已发布/已安装的
#      tool_d 头目录）；
#   2) 回退伞仓相对路径（agentrt 在目录树内，开发期）；
#   3) 都不可得 → WARN + 跳过 C 插件编译，不阻断 ecosystem 独立组装
#      （Python 技能不受影响；C 插件由 tool_d 运行时按需加载）。
set(TOOL_D_INCLUDE_DIR "" CACHE PATH "tool_d 插件 ABI 头文件目录（agentrt）")
if(NOT TOOL_D_INCLUDE_DIR)
    set(TOOL_D_INCLUDE_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../agentrt/daemons/tool_d/include)
endif()

find_path(TOOL_D_FOUND_DIR plugin_service.h
    PATHS ${TOOL_D_INCLUDE_DIR}
    NO_DEFAULT_PATH)

if(NOT TOOL_D_FOUND_DIR)
    message(WARNING
        "tool_d 插件 ABI 头文件未找到（agentrt 不在当前目录树）：跳过 C 技能插件编译。"
        "独立部署请在 cmake 时指定 -DTOOL_D_INCLUDE_DIR=<path>")
    return()
endif()

find_library(CJSON_LIBRARY NAMES cjson)

# ==================== text_summarization 技能插件 ====================
add_library(airy_skill_text_summarization SHARED
    text_summarization/src/plugin_text_summarization.c
)
target_include_directories(airy_skill_text_summarization PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/text_summarization/src
    ${TOOL_D_INCLUDE_DIR}
)
target_link_libraries(airy_skill_text_summarization PRIVATE ${CJSON_LIBRARY})
target_compile_options(airy_skill_text_summarization PRIVATE
    -Wall -Wextra
    -Werror=implicit-function-declaration -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
set_target_properties(airy_skill_text_summarization PROPERTIES
    OUTPUT_NAME airy_skill_text_summarization)

# ==================== code_review 技能插件 ====================
add_library(airy_skill_code_review SHARED
    code_review/src/plugin_code_review.c
)
target_include_directories(airy_skill_code_review PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/code_review/src
    ${TOOL_D_INCLUDE_DIR}
)
target_link_libraries(airy_skill_code_review PRIVATE ${CJSON_LIBRARY})
target_compile_options(airy_skill_code_review PRIVATE
    -Wall -Wextra
    -Werror=implicit-function-declaration -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
set_target_properties(airy_skill_code_review PROPERTIES
    OUTPUT_NAME airy_skill_code_review)

# ==================== security_audit 技能插件 ====================
add_library(airy_skill_security_audit SHARED
    security_audit/src/plugin_security_audit.c
)
target_include_directories(airy_skill_security_audit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/security_audit/src
    ${TOOL_D_INCLUDE_DIR}
)
target_link_libraries(airy_skill_security_audit PRIVATE ${CJSON_LIBRARY})
target_compile_options(airy_skill_security_audit PRIVATE
    -Wall -Wextra
    -Werror=implicit-function-declaration -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
set_target_properties(airy_skill_security_audit PROPERTIES
    OUTPUT_NAME airy_skill_security_audit)

# ==================== data_analysis 技能插件 ====================
add_library(airy_skill_data_analysis SHARED
    data_analysis/src/plugin_data_analysis.c
)
target_include_directories(airy_skill_data_analysis PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/data_analysis/src
    ${TOOL_D_INCLUDE_DIR}
)
target_link_libraries(airy_skill_data_analysis PRIVATE ${CJSON_LIBRARY} m)
target_compile_options(airy_skill_data_analysis PRIVATE
    -Wall -Wextra
    -Werror=implicit-function-declaration -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
set_target_properties(airy_skill_data_analysis PROPERTIES
    OUTPUT_NAME airy_skill_data_analysis)

# ==================== web_search 技能插件 ====================
add_library(airy_skill_web_search SHARED
    web_search/src/plugin_web_search.c
)
target_include_directories(airy_skill_web_search PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/web_search/src
    ${TOOL_D_INCLUDE_DIR}
)
target_link_libraries(airy_skill_web_search PRIVATE ${CJSON_LIBRARY})
target_compile_options(airy_skill_web_search PRIVATE
    -Wall -Wextra
    -Werror=implicit-function-declaration -Werror=return-type
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
set_target_properties(airy_skill_web_search PROPERTIES
    OUTPUT_NAME airy_skill_web_search)

# ==================== 后续技能插件在此追加（示例模板） ====================
# add_library(airy_skill_xxx SHARED
#     xxx/src/plugin_xxx.c
# )
# ...
