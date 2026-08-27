<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 -->
<!-- Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved. -->

# `.github/` — Ecosystem Skills 仓库自动化

> GitHub Actions 工作流与 CI 模板，服务于
> [Skills](https://atomgit.com/openairymax/skills) 叶子仓库。

---

## 定位

Skills 是 Airymax AI Agent 运行时平台的**官方技能定义与实现库**——
提供 5 个官方技能（代码审查 / 文本摘要 / 安全审计 / 数据分析 / Web 搜索），
每个技能继承 `SkillPlugin` 基类，包含人类可读的规范文档和可执行的 Python 实现。
本目录承载该仓库的 GitHub 级自动化配置。

## 目录内容

```
.github/
├── README.md              # 本文件
└── workflows/
    └── ci.yml             # CI 流水线（技能单元测试）
```

## CI 流水线

| 工作流 | 触发条件 | 职责 |
|--------|----------|------|
| `ci.yml` | PR / push | 5 个技能的单元测试套件 |

## 相关链接

| 资源 | 链接 |
|------|------|
| **主 README** | [skills/README.md](../README.md) |
| **伞仓** | [airymaxhub](https://atomgit.com/openairymax/airymaxhub) |
| **Ecosystem 管理仓** | [ecosystem/](../../) |

## 许可证

双许可证：**AGPL v3 + Apache 2.0**（SPDX: `AGPL-3.0-or-later OR Apache-2.0`）。
详见仓库根目录 [LICENSE](../LICENSE) 与 [NOTICE](../NOTICE)。

Copyright (c) 2025-2026 SPHARX Ltd. All Rights Reserved.
