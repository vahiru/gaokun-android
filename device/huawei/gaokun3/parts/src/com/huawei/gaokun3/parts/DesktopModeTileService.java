/*
 * 控制中心（快捷设置）里的桌面模式开关，亮起表示桌面优先（应用默认在桌面窗口中打开）。
 *
 * 与「MateBook E Go设置 → 显示」里的开关改同一项设置（DesktopModeSetting），那个页面监听这项设置，
 * 下拉控制中心切换时页面上的开关随即更新。
 *
 * 类名不能改：用户加到控制中心的磁贴按组件名记录，改名后磁贴会从控制中心消失。
 * 自动添加到控制中心的方式同KeyboardTileService。
 */
package com.huawei.gaokun3.parts;

import android.graphics.drawable.Icon;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.util.Log;

public class DesktopModeTileService extends TileService {

    private static final String TAG = "DesktopModeTileService";

    private void refresh() {
        Tile tile = getQsTile();
        if (tile == null) {
            return;
        }
        boolean on = DesktopModeSetting.isEnabled(this);
        tile.setState(on ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setLabel(getString(R.string.desktop_mode_tile_label));
        tile.setSubtitle(getString(on ? R.string.tile_on : R.string.tile_off));
        tile.setIcon(Icon.createWithResource(this, R.drawable.ic_desktop_mode));
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
            if (!DesktopModeSetting.setEnabled(this, !DesktopModeSetting.isEnabled(this))) {
                Log.w(TAG, "cannot write " + DesktopModeSetting.KEY);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot write " + DesktopModeSetting.KEY, e);
        }
        refresh();
    }
}
