# Markets — Skill 安装器与契约校验

> 属于 `ecosystem/skills`（官方技能仓库）的 `markets` 子模块。

## 定位

`markets/` 是技能分发的**客户端工具链**：为 AgentRT 提供从技能市场
安装、管理、移除技能的 CLI，以及技能契约（Skill Contract）的校验器。
它消费市场分发包，与 `ecosystem/markets` 仓库（市场**服务端/分发层**）
是客户端-分发包的关系。

## 与 `ecosystem/markets` 的区别（避免混淆）

| | `ecosystem/markets` | `skills/markets` |
|---|---|---|
| 所在仓库 | markets（市场仓库） | skills（技能仓库） |
| 角色 | 市场**服务端**：分发包仓库（tools/examples/templates） | 市场**客户端**：技能安装与契约校验 |
| 用途 | 分发、存放可安装包 | 把包安装进运行时的工具 |

两者通过 `contracts/schema.json`（技能契约 Schema）对齐：市场仓库中的
技能分发包必须满足本模块校验器所执行的契约。

## 目录结构

```
markets/
├── __init__.py             # 包入口（markets.skills）
├── contracts/              # 技能契约校验
│   ├── schema.json         # 技能契约 Schema（权威，draft-07）
│   ├── validator.py        # SkillContractValidator
│   └── __init__.py
└── installer/              # 技能安装器
    ├── cli.py              # CLI：install / list / remove
    └── __init__.py
```

## 契约校验

`SkillContractValidator` 在安装前校验技能契约，必填字段：
`name` / `version` / `description` / `capabilities` / `interface` /
`permissions`；权限范围受 `ALLOWED_PERMISSION_SCOPES` 白名单约束
（filesystem/network/process/memory/storage/system 等），拒绝未授权
权限，保证供应链安全。

```bash
python -m agentrt.openlab.markets.skills.contracts.validator --skill <skill_path>
```

## 安装器

`installer/cli.py` 提供技能安装管理：

```bash
python -m ecosystem.openlab.markets.skills.installer.cli install <skill_package>
python -m ecosystem.openlab.markets.skills.installer.cli list
python -m ecosystem.openlab.markets.skills.installer.cli remove <skill_name>
```

支持本地目录与远程包（zip/tar），安装目标遵循 `$AIRY_RUNTIME_DIR` /
`$AIRY_HOME`，回退 `~/.agentrt/skills`。
