# 安装器内置字体说明

字体二进制文件不纳入版本控制（由`python3 tool/fetch-fonts.py`下载并校验SHA-256），本目录仅保留此说明文件。

| 文件 | 字体 | 许可证 | 来源 |
|---|---|---|---|
| `NotoSansSC-VF.ttf` | Noto Sans SC（思源黑体简体中文，可变字重100–900） | SIL Open Font License 1.1 | <https://github.com/google/fonts/tree/main/ofl/notosanssc> |
| `Roboto-VF.ttf` | Roboto（可变字重100–900、字宽75–100） | SIL Open Font License 1.1 | <https://github.com/google/fonts/tree/main/ofl/roboto> |

两份字体取自Google Fonts的发布仓库`google/fonts`，提交号与SHA-256钉在`tool/fetch-fonts.py`里，对应Material Design 3的拉丁字母与中日韩正文字体规范。Noto Sans SC的默认实例是100（Thin），界面里每个文字样式都要显式给出`wght`轴（见`lib/ui/theme.dart`）。
