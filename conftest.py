# Copyright (c) 2026 SPHARX. All Rights Reserved.
"""pytest 路径引导：skills 独立叶仓自测入口。

skills 是独立叶仓（可单独 clone/测试），顶层包为 `skills`。本 conftest 将
仓库根加入 sys.path，使 `from skills.src...` 类导入在 CI（.github/workflows/
ci.yml 的 pytest 步骤）与本地均可用；伞仓组装场景下 ecosystem.skills 前缀
由伞仓根 pytest 运行（见 ecosystem/ 聚合测试）处理。
"""

import sys
from pathlib import Path

_SKILLS_ROOT = Path(__file__).resolve().parent

if str(_SKILLS_ROOT) not in sys.path:
    sys.path.insert(0, str(_SKILLS_ROOT))
