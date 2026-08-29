本项目的说明在 [AGENTS.md](AGENTS.md)，请先读那一份。

要点速记（细节都在 AGENTS.md）：

- **排盘绝不交给 LLM**，全部走 `main/bazi_engine.c` 的确定性代码
- **改了排盘必须跑** `./tools/verify_bazi.sh`，两侧引擎要逐字段一致
- `main/bazi_tables.c` 是生成物，不要手改
- ESP32-C3 没有 PSRAM，空闲堆只有 50KB 上下，加缓冲前先看水位
- 发往设备的文本要过 `for_device()`（GB2312 字库），切句要落在 UTF-8 边界上
