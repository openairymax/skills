# Skills — 社区实验区（非官方契约）

**模块路径**: `ecosystem/skills/contrib/`
**版本**: v0.1.1

> **Status（0.1.6 生态 SSoT 收敛 S-5）**: 本目录为**社区实验区**，非 AgentRT
> 官方技能契约的一部分。官方技能契约唯一权威 = `ecosystem/skills/definitions/*.md`
> + `plugins/*/manifest.yaml`（SkillPlugin 契约：`get_definition()` + `execute()`）。
> 本目录内技能处于**规范定义阶段**，仅含 README 无实现、无契约文件；其描述的
> `initialize()/execute()` 接口与官方 SkillPlugin 契约不同，**不承诺兼容、不参与
> 发布链路**。社区贡献者若需正式发布技能，请按官方 `plugins/` 目录规范实现。

## 概述

Skills 是 Airymax 社区贡献的可复用技能模块集合，为 Agent 提供特定领域的操作能力。当前包含三大技能：Browser Skill（浏览器自动化）、Database Skill（数据库操作）和 GitHub Skill（GitHub 平台集成），均处于规范定义阶段。

## 目录结构

```
contrib/
├── browser_skill/                  # 浏览器自动化技能（规范定义阶段）
│   └── README.md
├── database_skill/                 # 数据库操作技能（规范定义阶段）
│   └── README.md
├── github_skill/                   # GitHub 集成技能（规范定义阶段）
│   └── README.md
└── README.md                       # 本文件
```

## 技能列表

| Skill | 路径 | 领域 | 核心能力 | 状态 |
|-------|------|------|----------|------|
| **Browser Skill** | `browser_skill/` | Web 自动化 | 网页导航、数据抓取、表单操作、页面交互、截图 | 规范定义阶段 |
| **Database Skill** | `database_skill/` | 数据管理 | 多库支持、安全查询、模式探索、数据导出、事务 | 规范定义阶段 |
| **GitHub Skill** | `github_skill/` | 代码协作 | 仓库管理、Issue/PR 管理、CI/CD、代码审查、搜索 | 规范定义阶段 |

## 技能接口规范

所有 Skills 遵循统一的接口规范：

```python
class Skill:
    async def initialize(self)       # 初始化技能资源
    async def execute(self, action, params)  # 执行技能操作
    async def cleanup(self)          # 清理技能资源
```

### 契约规范

每个 Skill 应配备 `contract.json`，遵循 Markets 的 Skill 契约 Schema，必填字段：

| 字段 | 说明 |
|------|------|
| `name` | 技能名称 |
| `version` | 语义化版本号 |
| `description` | 功能描述 |
| `capabilities` | 能力列表 |
| `interface` | 接口类型（stdio/http_rest/websocket/grpc/message_queue/function_call） |
| `permissions` | 权限声明（filesystem/network/process/memory/storage/system） |

## 依赖关系

| Skill | 核心依赖 | 可选依赖 |
|-------|----------|----------|
| Browser Skill | AgentRT orchestration Core | Playwright >= 1.40.0 或 Selenium >= 4.15.0 |
| Database Skill | AgentRT orchestration Core, SQLAlchemy | psycopg2 (PostgreSQL), pymysql (MySQL) |
| GitHub Skill | AgentRT orchestration Core, PyGithub >= 1.59.0, requests >= 2.31.0 | — |

- **Python**: >= 3.10
- **协议依赖**: AgentRT protocols 层（JSON-RPC 2.0）

---

© 2025-2026 SPHARX Ltd. All Rights Reserved.
