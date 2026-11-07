# 参与贡献

感谢你对gaokun-android感兴趣。在提交issue或PR之前，请先阅读这份说明。

## 关于这个项目

gaokun-android是一个业余开源项目，目前由两名活跃贡献者维护。

- 开发优先级由维护者根据项目进展决定，不以单条需求的呼声高低为唯一依据。
- 没有固定的响应时间，issue和PR可能会很快得到回复，也可能需要等待一段时间。
- 安装或升级前，请先阅读[安装说明](docs/INSTALL.zh-CN.md)、[已知限制](docs/known-limitations.zh-CN.md)与[常见问题](docs/FAQ.zh-CN.md)。

## 报告问题

请通过[GitHub Issue表单](https://github.com/vahiru/gaokun-android/issues/new/choose)提交报告，并附上：

- 当前使用的系统版本号与构建时间戳（`ro.build.date.utc`）
- 问题的具体表现与复现步骤
- 对应的系统或内核日志

如果你身边有电脑并开启了adb，也可以直接将仓库根目录下的[issues.md](issues.md)交给AI编程助手，由它按只读流程收集日志、核对已知问题并生成完整的报告草稿。

提交前请先查看[已知限制](docs/known-limitations.zh-CN.md)和已有的issue，避免重复提交；缺少基本版本信息与日志、无法定位的报告可能会被直接关闭。

### 提问与日常交流

日常使用疑问与经验交流请前往[Telegram群组](https://t.me/gaokunAndroid)或QQ群（`920133252`）。GitHub issue仅用于明确的bug报告、实机测试反馈与开发讨论。

群内其他用户的经验分享不一定准确，请以仓库文档和维护者的说明为准。

## 参与测试与提交代码

欢迎参与实机测试或提交PR：

- **实机测试反馈**：[待办清单](docs/TODO.md)第2节列出了已合入代码但尚未经实测的功能与测量项。如果你在MateBook E Go上测试了其中某一项，无论成功与否，都欢迎提交issue反馈结果。
- **认领开发任务**：各子系统的架构、内核补丁与设计说明见[`docs/dev/`](docs/dev/)。准备开展较大改动前，请先开issue说明思路，避免与他人工作重复或方向不一致。
- **保持提交清晰**：每个提交只做一件事，提交信息与PR描述需写明问题原因、解决方式以及实机验证结果（如果尚未经实测，请在PR中如实注明）。

### 内核补丁

- 内核补丁位于[`patches/kernel/`](patches/README.md)，请遵循Linux内核的[编码风格](https://www.kernel.org/doc/html/latest/process/coding-style.html)和提交信息格式（`子系统: 简短英文描述`），并按[`patches/README.md`](patches/README.md)附上`Origin:`与`Upstream-Status:`标签。
- 适合合入主线的通用修复在[`patches/`](patches/README.md)中标有`Upstream-Status: Pending`。如果你希望协助将某个补丁推向上游主线，欢迎在issue或PR中告知。

## 许可

提交到本仓库的贡献，将按照本项目的许可证（见[LICENSE](LICENSE)与[NOTICE](NOTICE)）发布。
