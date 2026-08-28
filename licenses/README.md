# 第三方许可

## 内嵌字体

`main/font_cn_16.c` 与 `main/font_mystic_28.c` 是用 `lv_font_conv` 把下面两款字体
转成的 LVGL 位图,属于 SIL Open Font License 1.1 意义上的衍生作品,随本项目一同以
OFL 1.1 分发。原始许可全文见本目录:

| 字体 | 用途 | 许可 |
|---|---|---|
| [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC) | 正文 16px (`font_cn_16.c`) | `NotoSansSC-OFL.txt` |
| [Ma Shan Zheng](https://fonts.google.com/specimen/Ma+Shan+Zheng) | 书法标题 28px (`font_mystic_28.c`) | `MaShanZheng-OFL.txt` |

生成参数写在各自 .c 文件开头的注释里。

## 历法数据

`main/bazi_tables.c` 里的节气时刻、农历朔日、黄历宜忌由
[lunar-python](https://github.com/6tail/lunar-python)(MIT)计算生成,
生成脚本见 `tools/gen_bazi_tables.py`。
