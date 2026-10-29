# R18 Thermal forecast接口修复候选

当前已部署R17；本修改尚未交叉编译为Android HAL或上机。依据thermal AIDL源码的forecastSkinTemperature契约，forecastSeconds<0必须EX_ILLEGAL_ARGUMENT，unsupported错误须带可读说明。

原实现忽略预测时间，并在皮温缺失时返回无message的EX_UNSUPPORTED_OPERATION。本次只补参数检查和可读错误消息：负数在读取传感器前拒绝且不修改out；皮温/门限不可用时仍unsupported、不给伪造值。非负预测时间沿用现有平坦外推行为；没有新建预测模型或声称未来温度预测已经校准。

`tools/hos/test_thermal_forecast_contract.py`提取生产方法，配最小Binder status与假传感器编译运行；覆盖INT_MIN/-1、0/1/60、无效读数、输出不变与读取次数。C++17 -Wall -Wextra -Werror + ASan/UBSan通过。此测试不是完整Binder IPC或Android二进制兼容验收；须与后续HAL构建一并验证。
