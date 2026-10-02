# Skills — Official Skill Definitions & Implementations

> Official skill collection for the Airymax platform: reusable agent capabilities built on the `SkillPlugin` base class.
> A leaf repository under the [Airymax ecosystem](https://atomgit.com/openairymax/ecosystem).

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.9-5a6b7e)](https://atomgit.com/openairymax/skills)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![Branch](https://img.shields.io/badge/branch-develop%2Fhubs--01-6f7b8e)](https://atomgit.com/openairymax/skills)

**Repository:** `git@atomgit.com:openairymax/skills.git` · **Branch:** `develop/hubs-01`

---

## Overview

`ecosystem/skills/` is the **official skill definition and implementation library** of the Airymax AI Agent Runtime Platform. A *skill* in Airymax is a reusable, self-contained capability that packages a prompt template, an input/output schema and an execution entry point so that any agent can activate it on demand. Every skill in this repository inherits from the `SkillPlugin` base class (defined in `sdk-python/agentrt/plugin_types.py`) and implements two contracts: `get_definition()` → returns a `SkillDefinition` (name, version, category, tags, input/output JSON Schema), and `execute(parameters)` → runs the skill against validated input and returns a structured result.

The repository ships 5 official skills across 5 categories (development, text-processing, security, analytics, information), providing both the human-facing skill documentation (`definitions/*.md`) and the executable implementations (`src/*.py`), kept in lockstep so that a single source of truth describes each skill's contract. Each implementation declares `PLUGIN_TYPE = "skill"`, optionally exposes `get_prompt_template()` / `get_system_instructions()` for prompt-driven skills, and produces the structured output documented in its matching `definitions/*.md`.

Within the ecosystem layer, `skills/` is a self-contained module whose **only first-party upstream dependency is the Airymax SDK** (`agentrt.plugin_types`). It intentionally avoids hard runtime dependencies on `ecosystem/prompts` or `ecosystem/manager` so it can be loaded by any AgentRT-compatible runtime. Downstream it is consumed by agent applications (which register skills in `agent.yaml` / `config.yaml`), the marketplace examples (e.g. `code-review-agent` in `ecosystem/markets/examples`), and the plugin marketplace `ecosystem/markets` (which distributes skills).

## Directory Structure

```
skills/
├── __init__.py                        # Package entry — exports official skills
├── conftest.py                        # Test bootstrap (package-chain import discipline)
├── definitions/                       # Human-facing skill documentation (Markdown) — 契约 SSoT
│   ├── code_review.md                 # Code review skill spec
│   ├── text_summarization.md          # Text summarization skill spec
│   ├── security_audit.md              # Security audit skill spec
│   ├── data_analysis.md               # Data analysis skill spec
│   └── web_search.md                  # Web search skill spec
├── src/                               # SkillPlugin implementations (Python, SDK 加载)
│   ├── __init__.py
│   ├── code_review.py                 # CodeReviewSkill
│   ├── text_summarization.py          # TextSummarizationSkill
│   ├── security_audit.py              # SecurityAuditSkill
│   ├── data_analysis.py               # DataAnalysisSkill
│   └── web_search.py                  # WebSearchSkill
├── plugins/                           # C 语言本地实现（tool_d 加载，与 src/ 并存）
│   ├── CMakeLists.txt                 # 统一构建（产出 libairy_skill_<name>.so）
│   ├── README.md
│   ├── code_review/                   # 代码审查插件（manifest.yaml + src/）
│   ├── data_analysis/
│   ├── security_audit/
│   ├── text_summarization/
│   └── web_search/
├── contrib/                           # 规范定义阶段的技能（仅 README，尚无实现）
│   ├── README.md
│   ├── browser_skill/
│   ├── database_skill/
│   └── github_skill/
├── tests/
│   ├── __init__.py
│   └── test_skills.py                 # Unit tests for all 5 skills
├── pytest.ini                         # Test configuration
├── .github/workflows/ci.yml           # CI pipeline
├── .gitignore
├── .gitkeep
└── README.md                          # This file
```

## Core Components — Skill Catalog

| Skill | Category | Version | Description |
|-------|----------|:-------:|-------------|
| [`code_review`](definitions/code_review.md) | development | 1.0.0 | Multi-dimensional code review: security vulnerabilities, performance issues, best-practice deviations |
| [`text_summarization`](definitions/text_summarization.md) | text-processing | 1.0.0 | Long-text summarization: extractive, abstractive, bullet, concise |
| [`security_audit`](definitions/security_audit.md) | security | 1.0.0 | System security audit: config, dependencies, permissions, network, compliance |
| [`data_analysis`](definitions/data_analysis.md) | analytics | 1.0.0 | Statistical analysis on structured/semi-structured data with insight reporting |
| [`web_search`](definitions/web_search.md) | information | 1.0.0 | Multi-engine web search with deduplication, relevance ranking and summary extraction |

### Architecture

All skills inherit from the `SkillPlugin` base class and share a uniform lifecycle:

```
SkillPlugin  (defined in sdk-python/agentrt/plugin_types.py)
├── CodeReviewSkill          # development     — code review
├── TextSummarizationSkill   # text-processing — summarization
├── SecurityAuditSkill       # security        — security audit
├── DataAnalysisSkill        # analytics       — data analysis
└── WebSearchSkill           # information     — web search
```

Each skill implementation:
1. Declares `PLUGIN_TYPE = "skill"`.
2. Returns a `SkillDefinition` from `get_definition()` — including JSON Schema for input validation.
3. Optionally exposes `get_prompt_template()` and `get_system_instructions()` for prompt-driven skills.
4. Implements `async execute(context)` to produce the structured output documented in the corresponding `definitions/*.md`.

## Upstream Dependencies

`skills/` consumes the Airymax SDK as its only first-party dependency. The `SkillPlugin` base class and `SkillDefinition` dataclass are defined in the SDK and imported at runtime:

| Dependency | Purpose |
|------------|---------|
| `sdk-python` (`agentrt.plugin_types`) | Provides `SkillPlugin`, `SkillDefinition` — the base class every skill inherits from |
| AgentRT runtime | Hosts the plugin loader that discovers and instantiates skills; provides the execution context |
| Python ≥ 3.10 | Runtime for skill implementations (uses `asyncio`, standard library only) |

The skills intentionally avoid hard runtime dependencies on `ecosystem/prompts` or `ecosystem/manager` — they are self-contained modules that can be loaded by any AgentRT-compatible runtime.

## Downstream Consumers

| Consumer | How it uses `skills/` |
|----------|------------------------|
| **Agent applications** | Import skills via `from ecosystem.skills import CodeReviewSkill, ...` and register them in `agent.yaml` / `config.yaml` under `skills:` |
| **Orchestration (`ecosystem/agents/orchestration`)** | Interoperates with the official skills through its tool bridge |
| **Marketplace examples (`ecosystem/markets/examples`)** | `code-review-agent` and similar examples consume the official `CodeReviewSkill` |
| **Plugin marketplace (`ecosystem/markets`)** | Skills are registered and distributed through the marketplace contract defined in `ecosystem/markets` |
| **Agent developers** | Subclass `SkillPlugin` following the patterns in this repository to build custom skills |

## Usage / Quick Start

### Import and execute a skill

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

### Discover a skill's contract

```python
from ecosystem.skills import WebSearchSkill

skill = WebSearchSkill()
definition = skill.get_definition()
print(definition.name)            # "web_search"
print(definition.category)        # "information"
print(definition.input_schema)    # JSON Schema for validation
```

### Register skills in an agent

```yaml
# config.yaml
skills:
  - ecosystem.skills:CodeReviewSkill
  - ecosystem.skills:WebSearchSkill
  - ecosystem.skills:DataAnalysisSkill
```

### Developing a new skill

1. Subclass `SkillPlugin` (from `agentrt.plugin_types`).
2. Implement `get_definition()` → return a `SkillDefinition` with name, version, category, tags and input/output JSON Schema.
3. Implement `async execute(context)` → return the structured result.
4. Author a matching `definitions/<skill_name>.md` document so the human-facing spec stays in sync with the implementation.
5. Add unit tests under `tests/test_skills.py`.
6. Export the new skill from `__init__.py`.

## Build

The official skills' Python implementation is a pure Python package. `plugins/`
additionally carries C native implementations of the same skill contracts
(shared libraries dynamically loaded by `tool_d`), which require a separate
CMake build — see [plugins/README.md](plugins/README.md). Install the SDK dependency and run the test suite:

```bash
# Install the Airymax SDK (provides agentrt.plugin_types)
pip install agentrt

# Run the skill unit tests (all 5 skills)
python -m pytest tests/ -v
```

CI is defined in `.github/workflows/ci.yml` and runs the unit tests on every push.

## Branch Strategy

This leaf repository is on the **`develop/hubs-01`** branch (active development). The management repository that aggregates it stays on `main`.

## License

Dual-licensed under **AGPL v3 + Apache 2.0** (SPDX: `AGPL-3.0-or-later OR Apache-2.0`). See [LICENSE](LICENSE) for the full text.

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.
