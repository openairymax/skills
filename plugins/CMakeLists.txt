# Copyright (c) 2026 SPHARX. All Rights Reserved.
# Airymax 技能插件构建（独立于 agentrt 主构建体系）
#
# 构建产物：libairy_skill_<name>.so，部署到 plugin_d 的扫描目录
# （$AIRY_HOME/ecosystem/plugins/<name>/，随 manifest.yaml 一起）。
#
# 用法：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# 插件 ABI 头文件来自 plugin_d（plugin_service.h），仅编译期依赖，
# 运行期只依赖系统 libcjson（plugin_d 亦链接同一共享库）。

cmake_minimum_required(VERSION 3.16)
project(airymax_skills_plugins C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 插件 ABI 头文件目录（plugin_d 提供 plugin_service.h / plugin_discovery.h）
set(PLUGIN_D_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../agentrt/daemons/plugin_d/include)

find_library(CJSON_LIBRARY NAMES cjson)

# ==================== text_summarization 技能插件 ====================
add_library(airy_skill_text_summarization SHARED
    text_summarization/src/plugin_text_summarization.c
)
target_include_directories(airy_skill_text_summarization PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/text_summarization/src
    ${PLUGIN_D_INCLUDE_DIR}
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
    ${PLUGIN_D_INCLUDE_DIR}
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
    ${PLUGIN_D_INCLUDE_DIR}
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
    ${PLUGIN_D_INCLUDE_DIR}
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
    ${PLUGIN_D_INCLUDE_DIR}
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
