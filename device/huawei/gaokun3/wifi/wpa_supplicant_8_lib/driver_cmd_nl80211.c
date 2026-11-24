// SPDX-License-Identifier: Apache-2.0
/*
 * wpa_supplicant的设备私有驱动命令库（BoardConfig.mk的BOARD_WPA_SUPPLICANT_PRIVATE_LIB）。
 *
 * 框架通过ISupplicantStaIface.setCountryCode下发Wi-Fi国家码，wpa_supplicant把它转成私有驱动命令
 * “COUNTRY XX”（wpa_supplicant/aidl/sta_iface.cpp的setCountryCodeInternal）。不指定私有库时，
 * wpa_supplicant以桩模式（ANDROID_LIB_STUB）构建，没有driver_cmd，这条命令必然失败，国家码无法送达内核。
 *
 * 这里只实现COUNTRY：调用wpa_supplicant自带的nl80211实现发送NL80211_CMD_REQ_SET_REG，
 * 与配置文件里写country=XX时走同一条路径。ath11k收到国家码后由固件更换信道表，
 * 这一步还需要内核开启CONFIG_ATH_REG_DYNAMIC_USER_REG_HINTS（scripts/kernel-config-android.sh）。
 *
 * 其余私有命令一律返回-1，与桩模式下没有driver_cmd时相同。四个P2P函数照搬桩模式的返回值
 * （src/drivers/driver_nl80211_android.c中ANDROID_LIB_STUB分支），行为与之前一致。
 */

#include "includes.h"

#include "common.h"
#include "driver_nl80211.h"

int wpa_driver_nl80211_driver_cmd(void *priv, char *cmd, char *buf,
				  size_t buf_len)
{
	if (os_strncasecmp(cmd, "COUNTRY ", 8) == 0) {
		const char *alpha2 = cmd + 8;
		int ret;

		if (os_strlen(alpha2) != 2) {
			wpa_printf(MSG_ERROR, "gaokun3: bad country command '%s'",
				   cmd);
			return -1;
		}
		ret = wpa_driver_nl80211_ops.set_country(priv, alpha2);
		wpa_printf(MSG_DEBUG, "gaokun3: set country %s: %d", alpha2,
			   ret);
		return ret ? -1 : 0;
	}

	return -1;
}

int wpa_driver_set_p2p_noa(void *priv, u8 count, int start, int duration)
{
	return 0;
}

int wpa_driver_get_p2p_noa(void *priv, u8 *buf, size_t len)
{
	return 0;
}

int wpa_driver_set_p2p_ps(void *priv, int legacy_ps, int opp_ps, int ctwindow)
{
	return -1;
}

int wpa_driver_set_ap_wps_p2p_ie(void *priv, const struct wpabuf *beacon,
				 const struct wpabuf *proberesp,
				 const struct wpabuf *assocresp)
{
	return 0;
}
