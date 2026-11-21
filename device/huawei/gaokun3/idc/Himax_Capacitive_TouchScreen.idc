# 内置触摸屏（HX83121A触控，SPI）。内核驱动上报的总线为SPI、厂商和产品号为0
# （drivers/input/touchscreen/himax-spi-core.c），InputReader按设备名"Himax Capacitive TouchScreen"找到本文件。
#
# 驱动上报面板坐标，即竖屏的1600x2560。屏幕的自然方向是横屏，即面板旋转90°（设备树中面板的
# rotation，见device.mk），触摸坐标轴随之旋转同样的角度。

touch.deviceType = touchScreen
touch.orientation = ORIENTATION_90
