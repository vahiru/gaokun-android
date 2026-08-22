/*
 * 控制中心（快捷设置）里的磁吸键盘开关。
 *
 * 和设置页共用同一条通路：只改 persist.sys.gaokun3.keyboard 这个属性，
 * 真正动硬件的是 init 触发器 + /vendor/bin/gaokun3-keyboard.sh。
 * ★ 这样两个入口不会打架，也不需要谁去同步对方的状态。
 *
 * ⚠️ 磁贴默认【不会】自己出现在控制中心里 —— 这是 Android 的设计：
 *    用户要在快捷设置的编辑页（下拉后点铅笔）把它从"可用磁贴"里拖上去。
 *    装好后第一次得手动加一次。
 */
package com.huawei.gaokun3.parts;

import android.graphics.drawable.Icon;
import android.os.SystemProperties;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

public class KeyboardTileService extends TileService {

    private static final String PROP = "persist.sys.gaokun3.keyboard";

    private boolean isEnabled() {
        // 属性没设过 = 键盘开着。失败方向永远偏向"键盘可用"。
        return !"0".equals(SystemProperties.get(PROP, "1"));
    }

    private void refresh() {
        Tile tile = getQsTile();
        if (tile == null) {
            return;
        }
        boolean on = isEnabled();
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
        SystemProperties.set(PROP, isEnabled() ? "0" : "1");
        refresh();
    }
}
