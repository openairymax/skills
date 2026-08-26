# Definitions — 官方技能定义文档

> 属于 `ecosystem/skills`（官方技能仓库）的 `definitions` 子模块。

## 定位

`definitions/` 存放 5 个官方技能的**人类可读定义文档**（Markdown），
与 `src/*.py` 的技能实现一一对应、同步维护：每个技能的定义文档是其
契约（输入/输出/能力边界）的单一真相源，实现必须与其一致。

## 技能目录

| 定义文档 | 技能 | 类别 | 说明 |
|----------|------|------|------|
| `code_review.md` | CodeReviewSkill | development | 多维度静态代码审查（安全/性能/可维护性/正确性/风格） |
| `text_summarization.md` | TextSummarizationSkill | text-processing | 长文本摘要（抽取式/生成式/要点/简洁） |
| `security_audit.md` | SecurityAuditSkill | security | 系统安全审计（配置/依赖/权限/网络/合规） |
| `data_analysis.md` | DataAnalysisSkill | analytics | 结构化/半结构化数据统计分析 |
| `web_search.md` | WebSearchSkill | information | 多引擎网页搜索（去重/相关度排序/摘要抽取） |

## 文档结构

每篇定义文档包含：概述、能力维度/步骤、输入参数 Schema、输出结构、
使用示例与注意事项。**输入/输出字段为契约**——修改实现前必须先更新
对应定义文档，保持 SSoT 一致。

## 使用方式

- **技能实现者**：按定义文档实现 `src/<name>.py`；
- **Agent 开发者**：从定义文档了解技能的输入输出契约，在 agent
  配置中注册使用；
- **测试**：`tests/test_skills.py` 按定义文档的契约字段校验实现。
