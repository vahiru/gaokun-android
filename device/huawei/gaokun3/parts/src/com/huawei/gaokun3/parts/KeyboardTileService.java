/*
 * 控制中心（快捷设置）里的磁吸键盘开关。
 *
 * 与「设置 → 已连接的设备」里的开关条目（KeyboardProvider）改同一个属性persist.sys.gaokun3.keyboard，
 * 真正动硬件的是init触发器和/vendor/bin/gaokun3-keyboard.sh，两个入口不需要互相同步状态。
 * 切换后通知设置刷新开关：设置页开着时下拉控制中心切换，页面上的开关也跟着变。
 *
 * 类名不能改：用户加到控制中心的磁贴按组件名记录，改名后磁贴会从控制中心消失。
 * 磁贴默认不出现在控制中心，用户要在快捷设置的编辑页把它拖上去，这是Android的设计。
 */
package com.huawei.gaokun3.parts;

import android.graphics.drawable.Icon;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.util.Log;

public class KeyboardTileService extends TileService {

    private static final String TAG = "KeyboardTileService";

    private void refresh() {
        Tile tile = getQsTile();
        if (tile == null) {
            return;
        }
        boolean on = KeyboardProvider.isEnabled();
        tile.setState(on ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setLabel(getString(R.string.keyboard_title));
        tile.setSubtitle(getString(on ? R.string.tile_on : R.string.tile_off));
        tile.setIcon(Icon.createWithResource(this, R.drawable.ic_keyboard));
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
            KeyboardProvider.setEnabled(!KeyboardProvider.isEnabled());
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot set " + KeyboardProvider.PROP, e);
        }
        KeyboardProvider.notifySwitchChanged(this);
        refresh();
    }
}
