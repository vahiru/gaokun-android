# R18 设置按压轮廓候选：双资源实测

设备仍为R17，boot ID afd51a55-eda7-4c64-ada9-a0ddfa9b0436。当前仅临时shell fabricated覆盖，未改系统镜像；目标保持active，未声称整个HyperOS已完成。

之前preference-only覆盖对首页无效；首页HeaderAdapter走miuix_recyclerview_card_tint_color_light，右侧preference走miuix_preference_item_tint_color_light。本次分别fabricate两个overlay，重启Settings进程后，左右两边都实际呈现蓝色按压反馈。cmd overlay fabricate一次只接收一个资源；初次尝试同一命令传两个资源被拒绝，未生效，已改为两个独立命令，没有将失败截图当成功。

证据在~/.cache/hos-r18-settings：

- right-gray-held.png / right-blue-held.png：同一右侧“网络加速”按住对比，蓝色候选确实作用于子页，非只看lookup。
- left-blue-held.png / left-blue-move.png / released.png：首页“更多连接”同区域，空白内点(500,1520)依次为(225,239,255)、(246,250,255)、(255,255,255)；卡外(620,1520)保持(243,243,243)。取消后恢复白色，圆角图像仍在。
- 黑色遮罩过渡时RGB可与243灰底相遇；蓝色遮罩在白底上B通道保持255，因此不会以同样方式整行与灰底融为一体。这是视觉反馈改良，不把它写成GPU裁剪驱动修复。
- dark.png：临时cmd uimode night yes后深色页面卡片正常，未见浅蓝覆盖误入深色静态背景；随后已恢复night no并读回。没有把此静态图当作完整深色触摸/滑动验收。

临时脚本tools/hos/settings-touch-compat.sh支持apply/restore/status，检查隔离guard和原始颜色；只重启Settings，不清数据。恢复用restore。此donor在重启时会删除shell fabricated，不能把当前效果当永久修复。

静态候选tools/hos/overlays/settings-touch已改为仅两个light color。APK位于cache/hos-r18-settings/overlay/HosSettingsCardOverlay.apk，SHA256 bb94d7e507a28059ecc0f6ae5a0fa76a38c30e4605d6d787d432a49085e3e490；无DEX/native，原测试证书4839029b…21cd。设备idmap2 create --policy vendor成功，未使用ignore-overlayable，映射精确为两个预期color。尚未集成vendor、完整重启验收或得到用户对原始手势问题的确认。

已请求用户确认：原动作是否恢复，以及是否希望保留灰色按压风格。用户若不接受蓝色，应继续实现原配色下的轮廓保持，而不是把本候选当强制最终答案。
