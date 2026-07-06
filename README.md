# Skills — Official Skill Definitions & Implementations

> Official skill collection for the Airymax platform: reusable agent capabilities built on the `SkillPlugin` base class.
> A leaf repository under the [Airymax ecosystem](https://atomgit.com/openairymax/ecosystem).

**Language:** English | [简体中文](README_zh.md)

[![Version](https://img.shields.io/badge/version-0.1.1-5a6b7e)](https://atomgit.com/openairymax/skills)
[![License](https://img.shields.io/badge/license-AGPL--3.0+Apache--2.0-4a90d9)](LICENSE)
[![Branch](https://img.shields.io/badge/branch-feature%2Fofficial--hubs--01-6f7b8e)](https://atomgit.com/openairymax/skills)

---

## Module Positioning

`ecosystem/skills/` is the **official skill definition and implementation library** of the Airymax AI Agent Runtime Platform. A *skill* in Airymax is a reusable, self-contained capability that packages a prompt template, an input/output schema and an execution entry point so that any agent can activate it on demand.

Every skill in this repository inherits from the `SkillPlugin` base class (defined in `sdk-python/agentos/plugin_types.py`) and implements two contracts:

- `get_definition()` → returns a `SkillDefinition` (name, version, category, tags, input/output JSON Schema)
- `execute(parameters)` → runs the skill against validated input and returns a structured result

The repository ships both the human-facing skill documentation (`definitions/*.md`) and the executable implementations (`src/*.py`), kept in lockstep so that a single source of truth describes each skill's contract.

## Skill Catalog

| Skill | Category | Version | Description |
|-------|----------|:-------:|-------------|
| [`code_review`](definitions/code_review.md) | development | 1.0.0 | Multi-dimensional code review: security vulnerabilities, performance issues, best-practice deviations |
| [`text_summarization`](definitions/text_summarization.md) | text-processing | 1.0.0 | Long-text summarization: extractive, abstractive, bullet, concise |
| [`security_audit`](definitions/security_audit.md) | security | 1.0.0 | System security audit: config, dependencies, permissions, network, compliance |
| [`data_analysis`](definitions/data_analysis.md) | analytics | 1.0.0 | Statistical analysis on structured/semi-structured data with insight reporting |
| [`web_search`](definitions/web_search.md) | information | 1.0.0 | Multi-engine web search with deduplication, relevance ranking and summary extraction |

## Directory Structure

```
skills/
├── __init__.py                        # Package entry — exports official skills
├── definitions/                       # Human-facing skill documentation (Markdown)
│   ├── code_review.md                 # Code review skill spec
│   ├── text_summarization.md          # Text summarization skill spec
│   ├── security_audit.md              # Security audit skill spec
│   ├── data_analysis.md               # Data analysis skill spec
│   └── web_search.md                  # Web search skill spec
├── src/                               # SkillPlugin implementations
│   ├── __init__.py
│   ├── code_review.py                 # CodeReviewSkill
│   ├── text_summarization.py          # TextSummarizationSkill
│   ├── security_audit.py              # SecurityAuditSkill
│   ├── data_analysis.py               # DataAnalysisSkill
│   └── web_search.py                  # WebSearchSkill
├── tests/
│   ├── __init__.py
│   └── test_skills.py                 # Unit tests for all 5 skills
├── .github/workflows/ci.yml           # CI pipeline
├── .gitignore
├── .gitkeep
└── README.md                          # This file
```

## Architecture

All skills inherit from the `SkillPlugin` base class and share a uniform lifecycle:

```
SkillPlugin  (defined in sdk-python/agentos/plugin_types.py)
├── CodeReviewSkill          # development    — code review
├── TextSummarizationSkill   # text-processing — summarization
├── SecurityAuditSkill       # security       — security audit
├── DataAnalysisSkill        # analytics      — data analysis
└── WebSearchSkill           # information    — web search
```

Each skill implementation:

1. Declares `PLUGIN_TYPE = "skill"`.
2. Returns a `SkillDefinition` from `get_definition()` — including JSON Schema for input validation.
3. Optionally exposes `get_prompt_template()` and `get_system_instructions()` for prompt-driven skills.
4. Implements `execute(parameters)` to produce the structured output documented in the corresponding `definitions/*.md`.

## Upstream / Downstream Dependencies

### Upstream

`skills/` consumes the Airymax SDK as its only first-party dependency. The `SkillPlugin` base class and `SkillDefinition` dataclass are defined in the SDK and imported at runtime:

| Dependency | Purpose |
|------------|---------|
| `sdk-python` (`agentos.plugin_types`) | Provides `SkillPlugin`, `SkillDefinition` — the base class every skill inherits from |
| AgentRT runtime | Hosts the plugin loader that discovers and instantiates skills; provides the execution context |
| Python ≥ 3.10 | Runtime for skill implementations (uses `asyncio`, standard library only) |

The skills intentionally avoid hard runtime dependencies on `ecosystem/prompts` or `ecosystem/manager` — they are self-contained modules that can be loaded by any AgentRT-compatible runtime.

### Downstream

| Consumer | How it uses `skills/` |
|----------|------------------------|
| **Agent applications** | Import skills via `from ecosystem.skills import CodeReviewSkill, ...` and register them in `agent.yaml` / `config.yaml` under `skills:` |
| **OpenLab (`ecosystem/openlab`)** | `contrib/skills/` extends and interoperates with the official skills defined here |
| **Examples (`ecosystem/examples`)** | `code-review-agent` and similar examples consume the official `CodeReviewSkill` |
| **Plugin marketplace** | Skills are registered and distributed through the marketplace contract defined in `openlab/markets/skills/` |
| **Agent developers** | Subclass `SkillPlugin` following the patterns in this repository to build custom skills |

## Usage

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

### Run the test suite

```bash
python -m pytest tests/ -v
```

## Developing a new skill

1. Subclass `SkillPlugin` (from `agentos.plugin_types`).
2. Implement `get_definition()` → return a `SkillDefinition` with name, version, category, tags and input/output JSON Schema.
3. Implement `execute(parameters)` → return the structured result.
4. Author a matching `definitions/<skill_name>.md` document so the human-facing spec stays in sync with the implementation.
5. Add unit tests under `tests/test_skills.py`.
6. Export the new skill from `__init__.py`.

## Branch Strategy

This leaf repository is on the **`feature/official-hubs-01`** branch (active development). The management repository that aggregates it stays on `main`.

## License

Dual-licensed under **AGPL v3 + Apache 2.0** (SPDX: `AGPL-3.0-or-later OR Apache-2.0`). See [LICENSE](LICENSE) for the full text.

Copyright (c) 2025-2026 **SPHARX Ltd.** All Rights Reserved.
