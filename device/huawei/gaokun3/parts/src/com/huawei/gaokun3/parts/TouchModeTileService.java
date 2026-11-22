/*
 * 控制中心（快捷设置）里的防误触开关，亮起表示防误触模式已开启。
 *
 * 与「设置 → 显示」里的开关条目（TouchProtectionProvider）改同一个属性persist.sys.gaokun3.touch_mode，
 * 两个入口不需要互相同步状态。切换后通知设置刷新开关：设置页开着时下拉控制中心切换，页面上的开关也跟着变。
 *
 * 类名不能改：用户加到控制中心的磁贴按组件名记录，改名后磁贴会从控制中心消失。
 * 磁贴默认不出现在控制中心，用户要在快捷设置的编辑页手动加一次（同KeyboardTileService）。
 */
package com.huawei.gaokun3.parts;

import android.graphics.drawable.Icon;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.util.Log;

public class TouchModeTileService extends TileService {

    private static final String TAG = "TouchModeTileService";

    private void refresh() {
        Tile tile = getQsTile();
        if (tile == null) {
            return;
        }
        boolean on = TouchProtectionProvider.isEnabled();
        tile.setState(on ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setLabel(getString(R.string.touch_mode_tile_label));
        tile.setSubtitle(getString(on ? R.string.tile_on : R.string.tile_off));
        tile.setIcon(Icon.createWithResource(this, R.drawable.ic_touch_protection));
        tile.updateTile();
    }

    @Override
    public void onStartListening() {
        super.onStartListening();
        refresh();
    }

    @Override
    public void onClick() {
        super.onClick();
        try {
            TouchProtectionProvider.setEnabled(!TouchProtectionProvider.isEnabled());
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot set " + TouchProtectionProvider.PROP, e);
        }
        TouchProtectionProvider.notifySwitchChanged(this);
        refresh();
    }
}
