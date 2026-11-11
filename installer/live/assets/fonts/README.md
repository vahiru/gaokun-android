# 安装器内置字体说明

字体二进制文件不纳入版本控制（由`python3 tool/fetch-fonts.py`拉取并校验SHA-256），本目录仅保留此说明文件。

| 文件 | 字体 | 许可证 | 上游仓库 |
|---|---|---|---|
| `NotoSansSC-VF.otf` | Noto Sans CJK SC（可变字重400–900） | SIL Open Font License 1.1 | <https://github.com/notofonts/noto-cjk> |
| `Roboto-VF.ttf` | Roboto（可变字重100–900、字宽75–100） | Apache License 2.0 / OFL 1.1 | <https://github.com/googlefonts/roboto-3-classic> |

两份字体均提取自Android系统自带的`/system/fonts/`（`NotoSansSC-VF.otf`由Noto CJK合集字体拆出简体中文子集），对应Material Design 3的拉丁字母与中日韩正文字体规范。
