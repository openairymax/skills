# Skills — 官方技能定义与实现

> Airymax 平台的官方技能集合：基于 `SkillPlugin` 基类构建的可复用 Agent 能力。
> 隶属于 [Airymax ecosystem](https://atomgit.com/openairymax/ecosystem) 的叶子仓。

**语言:** [English](README.md) | 简体中文

[![Version](https://img.shields.io/badge/version-0.1.1-5a6b7e)](https://atomgit.com/openairymax/skills)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![Branch](https://img.shields.io/badge/branch-feature%2Fofficial--hubs--01-6f7b8e)](https://atomgit.com/openairymax/skills)

**仓库:** `git@atomgit.com:openairymax/skills.git` · **分支:** `feature/official-hubs-01`

---

## 概述

`ecosystem/skills/` 是 Airymax AI Agent 运行时平台的**官方技能定义与实现库**。Airymax 中的 *技能*（skill）是一种可复用、自包含的能力，封装了提示词模板、输入 / 输出 schema 与执行入口，任何 Agent 都可按需激活。本仓中每个技能都继承自 `SkillPlugin` 基类（定义于 `sdk-python/agentos/plugin_types.py`），并实现两个契约：`get_definition()` → 返回 `SkillDefinition`（名称、版本、类别、标签、输入 / 输出 JSON Schema）；`execute(parameters)` → 对校验通过的输入执行技能，返回结构化结果。

本仓提供 5 个官方技能，覆盖 5 大类别（development、text-processing、security、analytics、information），同时提供面向人类的技能文档（`definitions/*.md`）与可执行实现（`src/*.py`），二者保持同步，确保每个技能的契约只有单一真相源。每个实现声明 `PLUGIN_TYPE = "skill"`，可选地暴露 `get_prompt_template()` / `get_system_instructions()` 供提示词驱动型技能使用，并产出对应 `definitions/*.md` 中所文档化的结构化输出。

在生态层中，`skills/` 是自包含模块，**唯一的一方上游依赖是 Airymax SDK**（`agentos.plugin_types`）。它有意避免对 `ecosystem/prompts` 或 `ecosystem/manager` 的硬运行时依赖，可被任意兼容 AgentRT 的运行时加载。下游被 Agent 应用（在 `agent.yaml` / `config.yaml` 中注册技能）、OpenLab（`contrib/skills/` 扩展并与这些官方技能互操作）、示例（`code-review-agent` 消费 `CodeReviewSkill`）以及插件市场（通过 `openlab/markets/skills/` 中定义的契约分发技能）消费。

## 目录结构

```
skills/
├── __init__.py                        # 包入口 — 导出官方技能
├── definitions/                       # 面向人类的技能文档（Markdown）
│   ├── code_review.md                 # 代码审查技能规格
│   ├── text_summarization.md          # 文本摘要技能规格
│   ├── security_audit.md              # 安全审计技能规格
│   ├── data_analysis.md               # 数据分析技能规格
│   └── web_search.md                  # 网络搜索技能规格
├── src/                               # SkillPlugin 实现
│   ├── __init__.py
│   ├── code_review.py                 # CodeReviewSkill
│   ├── text_summarization.py          # TextSummarizationSkill
│   ├── security_audit.py              # SecurityAuditSkill
│   ├── data_analysis.py               # DataAnalysisSkill
│   └── web_search.py                  # WebSearchSkill
├── tests/
│   ├── __init__.py
│   └── test_skills.py                 # 全部 5 个技能的单元测试
├── .github/workflows/ci.yml           # CI 流水线
├── .gitignore
├── .gitkeep
└── README.md                          # 本文件
```

## 核心组件 — 技能列表

| 技能 | 类别 | 版本 | 说明 |
|------|------|:----:|------|
| [`code_review`](definitions/code_review.md) | development | 1.0.0 | 多维度代码审查：安全漏洞、性能问题、最佳实践偏差 |
| [`text_summarization`](definitions/text_summarization.md) | text-processing | 1.0.0 | 长文本智能摘要：提取式、抽象式、要点式、精简式 |
| [`security_audit`](definitions/security_audit.md) | security | 1.0.0 | 系统安全审计：配置、依赖、权限、网络、合规 |
| [`data_analysis`](definitions/data_analysis.md) | analytics | 1.0.0 | 对结构化 / 半结构化数据执行统计分析，生成洞察报告 |
| [`web_search`](definitions/web_search.md) | information | 1.0.0 | 多引擎网络搜索，含去重、相关性排序与摘要提取 |

### 架构

所有技能继承自 `SkillPlugin` 基类，共享统一的生命周期：

```
SkillPlugin  (定义于 sdk-python/agentos/plugin_types.py)
├── CodeReviewSkill          # development     — 代码审查
├── TextSummarizationSkill   # text-processing — 文本摘要
├── SecurityAuditSkill       # security        — 安全审计
├── DataAnalysisSkill        # analytics       — 数据分析
└── WebSearchSkill           # information     — 网络搜索
```

每个技能实现：
1. 声明 `PLUGIN_TYPE = "skill"`。
2. 通过 `get_definition()` 返回 `SkillDefinition` — 含用于输入校验的 JSON Schema。
3. 可选地暴露 `get_prompt_template()` 与 `get_system_instructions()` 供提示词驱动型技能使用。
4. 实现 `async execute(context)`，产出对应 `definitions/*.md` 中所文档化的结构化输出。

## 上游依赖

`skills/` 仅以 Airymax SDK 作为唯一的一方依赖。`SkillPlugin` 基类与 `SkillDefinition` dataclass 定义于 SDK 中，运行时导入：

| 依赖 | 用途 |
|------|------|
| `sdk-python`（`agentos.plugin_types`） | 提供 `SkillPlugin`、`SkillDefinition` — 每个技能继承的基类 |
| AgentRT 运行时 | 托管插件加载器以发现并实例化技能；提供执行上下文 |
| Python ≥ 3.10 | 技能实现的运行时（仅使用 `asyncio` 与标准库） |

技能有意避免对 `ecosystem/prompts` 或 `ecosystem/manager` 的硬运行时依赖 — 它们是自包含模块，可被任意兼容 AgentRT 的运行时加载。

## 下游消费方

| 消费方 | 使用方式 |
|--------|----------|
| **Agent 应用** | 通过 `from ecosystem.skills import CodeReviewSkill, ...` 导入技能，并在 `agent.yaml` / `config.yaml` 的 `skills:` 节注册 |
| **OpenLab（`ecosystem/openlab`）** | `contrib/skills/` 扩展并与本仓定义的官方技能互操作 |
| **示例（`ecosystem/examples`）** | `code-review-agent` 等示例消费官方 `CodeReviewSkill` |
| **插件市场** | 技能通过 `openlab/markets/skills/` 中定义的市场契约注册并分发 |
| **Agent 开发者** | 参照本仓的模式子类化 `SkillPlugin` 以构建自定义技能 |

## 使用说明 / 快速开始

### 导入并执行技能

```python
import asyncio
from ecosystem.skills import CodeReviewSkill

skill = CodeReviewSkill()
result = asyncio.run(skill.execute({
    "code": 'password = "secret123"',
    "language": "python",
    "focus": "security",
}))
print(result["overall_score"])   # 75.0
print(len(result["findings"]))   # 1 (critical: hardcoded secret)
```

### 查询技能契约

```python
from ecosystem.skills import WebSearchSkill

skill = WebSearchSkill()
definition = skill.get_definition()
print(definition.name)            # "web_search"
print(definition.category)        # "information"
print(definition.input_schema)    # 用于校验的 JSON Schema
```

### 在 Agent 中注册技能

```yaml
# config.yaml
skills:
  - ecosystem.skills:CodeReviewSkill
  - ecosystem.skills:WebSearchSkill
  - ecosystem.skills:DataAnalysisSkill
```

### 开发新技能

1. 子类化 `SkillPlugin`（来自 `agentos.plugin_types`）。
2. 实现 `get_definition()` → 返回含名称、版本、类别、标签与输入 / 输出 JSON Schema 的 `SkillDefinition`。
3. 实现 `async execute(context)` → 返回结构化结果。
4. 编写配套的 `definitions/<skill_name>.md` 文档，使面向人类的规格与实现保持同步。
5. 在 `tests/test_skills.py` 下添加单元测试。
6. 在 `__init__.py` 中导出新技能。

## 构建

`skills/` 是纯 Python 包，无编译产物。安装 SDK 依赖并运行测试套件：

```bash
# 安装 Airymax SDK（提供 agentos.plugin_types）
pip install agentrt

# 运行技能单元测试（全部 5 个技能）
python -m pytest tests/ -v
```

CI 定义在 `.github/workflows/ci.yml`，每次推送时运行单元测试。

## 分支策略

本叶子仓位于 **`feature/official-hubs-01`** 分支（活跃开发）。聚合它的管理仓保持在 `main`。

## 许可证

采用 **AGPL v3 + Apache 2.0** 双许可证（SPDX: `AGPL-3.0-or-later OR Apache-2.0`）。详见 [LICENSE](LICENSE)。

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.
