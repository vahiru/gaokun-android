/*
 * 「设置 → 提示音和振动」里本设备注入的内容：「高级」分组标题，以及扬声器音效条目下方的当前选项。
 *
 * 分组：没有开关也没有点击动作的注入项，设置把它显示成带标题的分组（SettingsLib Tile.Type.GROUP）。
 * 扬声器音效（SpeakerFxDialogActivity）与麦克风降噪（NoiseSuppressionSettingsActivity）两个activity条目
 * 在manifest里以com.android.settings.group_key指向GROUP_KEY，被放进这个分组。
 *
 * 当前选项：扬声器音效条目的com.android.settings.summary_uri指向本provider的getDynamicSummary方法，
 * 键为SPEAKER_FX_KEY。这个键不是本provider的注入项，所以在call()里单独回答，其余交给EntriesProvider。
 * provider受WRITE_SECURE_SETTINGS保护，只有设置等系统组件能调用。
 */
package com.huawei.gaokun3.parts;

import android.os.Bundle;

import com.android.settingslib.drawer.EntriesProvider;
import com.android.settingslib.drawer.EntryController;
import com.android.settingslib.drawer.TileUtils;

import java.util.List;

public class SoundEntriesProvider extends EntriesProvider {

    static final String AUTHORITY = "com.huawei.gaokun3.parts.sound";
    /** 与manifest里两个activity条目的com.android.settings.group_key一致。 */
    static final String GROUP_KEY = "gaokun3_sound_advanced";
    /** 与manifest里扬声器音效条目的com.android.settings.keyhint和summary_uri一致。 */
    static final String SPEAKER_FX_KEY = "gaokun3_speaker_fx";

    @Override
    protected List<? extends EntryController> createEntryControllers() {
        return List.of(new AdvancedGroupController());
    }

    @Override
    public Bundle call(String method, String uriString, Bundle extras) {
        String key = extras != null ? extras.getString(TileUtils.META_DATA_PREFERENCE_KEYHINT) : null;
        if (METHOD_GET_DYNAMIC_SUMMARY.equals(method) && SPEAKER_FX_KEY.equals(key)) {
            Bundle bundle = new Bundle();
            bundle.putString(TileUtils.META_DATA_PREFERENCE_SUMMARY,
                    SpeakerFxDialogActivity.currentLabel(getContext()));
            return bundle;
        }
        return super.call(method, uriString, extras);
    }

    static class AdvancedGroupController extends EntryController {

        @Override
        public String getKey() {
            return GROUP_KEY;
        }

        @Override
        protected MetaData getMetaData() {
            return new MetaData("com.android.settings.category.ia.sound")
                    .setTitle(R.string.sound_advanced_title);
        }
    }
}
