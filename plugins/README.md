# Plugins — 官方技能 C 插件

> 属于 `ecosystem/skills`（官方技能仓库）的 `plugins` 子模块。

## 定位

`plugins/` 是 5 个官方技能的 **C 语言本地实现**（动态库），经
`plugin_d`（AgentRT 运行时 daemon）动态加载。它们与 `src/*.py` 的
Python 实现对应同一组技能契约，提供零解释器开销的本地执行路径，
供高性能/资源受限场景选用。

## 目录结构

```
plugins/
├── CMakeLists.txt                # 插件统一构建（产出 libairy_skill_<name>.so）
├── code_review/                  # 代码审查插件
│   ├── manifest.yaml             # 插件清单（plugin_discovery 解析）
│   └── src/plugin_code_review.c
├── data_analysis/                # 数据分析插件
├── security_audit/               # 安全审计插件
├── text_summarization/           # 文本摘要插件
└── web_search/                   # 网页搜索插件
```

## 构建

插件独立于 agentrt 主构建体系（源码区外构建铁律 BAN-33 同样适用）：

```bash
cmake -S . -B <build-dir> -DCMAKE_BUILD_TYPE=Release
cmake --build <build-dir>
```

产物 `libairy_skill_<name>.so` 部署到 `plugin_d` 扫描目录
`$AIRY_HOME/ecosystem/plugins/<name>/`，随 `manifest.yaml` 一起。

## 插件 ABI

- ABI 头来自 `agentrt/daemons/plugin_d/include`（`plugin_service.h` /
  `plugin_discovery.h`），仅编译期依赖；
- 运行期仅依赖系统 `libcjson`（`plugin_d` 链接同一共享库）；
- 每个插件以 `manifest.yaml` 声明 `type: tool_provider`、权限与超时
  配置，供 `plugin_discovery` 解析。

## 与 Python 实现的关系

同一技能存在两套实现（`src/*.py` 与 `plugins/*`），契约（输入输出）
以 `definitions/*.md` 为单一真相源。运行时按部署形态选载其一或并存，
两套实现均须通过 `tests/test_skills.py` 的契约校验。
