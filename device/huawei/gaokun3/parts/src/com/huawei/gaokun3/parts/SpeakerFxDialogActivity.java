/*
 * 扬声器音效（试验功能，PR #7）的选择对话框：关闭、低音增强或华为Histen。
 *
 * 「设置 → 提示音和振动」里只有一个条目，点开后显示这个对话框。注入的条目只能启动一个activity，
 * 设置不会替注入方弹对话框，所以这里是一个透明的activity（Theme.Gaokun3Parts.DialogHost），
 * 窗口里只有对话框，设置页留在下面；选择、取消或返回后activity随即结束。
 * 条目下方显示当前选项，由SoundEntriesProvider按currentLabel()提供，选完后通知设置刷新。
 *
 * 只写两个属性：persist.sys.gaokun3.histen为"1"或"0"（开启或关闭），persist.sys.gaokun3.speakerfx.engine
 * 为"bass"或"histen"（引擎）。init（etc/histen.rc）把它们镜像成vendor.gaokun3.histen.on和
 * vendor.gaokun3.speakerfx.engine。bass由音频HAL里的speakerfx（device/huawei/gaokun3/speakerfx/）处理，
 * histen由会话音效（device/huawei/gaokun3/effects/）处理；两边按同一个镜像各自判断由谁处理、何时生效、
 * 只处理内置扬声器，这里不重复。选择引擎时先写引擎再开启，避免短暂用错引擎；选择关闭时引擎保持不变。
 *
 * 绕init一次的原因：本应用是system_app（coredomain），不允许写任何vendor属性
 * （sepolicy/vendor_gaokun3_props.te顶部）；vendor的HAL又读不了system_prop
 * （core_property_type，sepolicy/hal_audio_default.te）。只有init两边都能访问。
 *
 * 对话框的说明文字在标题与选项之间：AlertDialog的message与内置列表只能二选一，所以选项是放在
 * setView()里的ListView，表项布局取对话框主题的singleChoiceItemLayout，与设置自己的单选对话框一致。
 */
package com.huawei.gaokun3.parts;

import android.app.Activity;
import android.content.Context;
import android.content.res.TypedArray;
import android.os.Bundle;
import android.os.SystemProperties;
import android.util.Log;
import android.widget.ArrayAdapter;
import android.widget.ListView;

import androidx.appcompat.app.AlertDialog;

import com.android.settingslib.widget.SettingsThemeHelper;

public class SpeakerFxDialogActivity extends Activity {

    private static final String TAG = "SpeakerFxDialog";

    /** effect与speakerfx读的总开关。只有"1"算开启；没设过 = 关闭。 */
    private static final String PROP = "persist.sys.gaokun3.histen";
    /** 引擎。只有"histen"是Histen；其余都是低音增强。 */
    private static final String ENGINE_PROP = "persist.sys.gaokun3.speakerfx.engine";

    // 与R.array.speaker_fx_engine_values一致
    private static final String VALUE_OFF = "off";
    private static final String VALUE_HISTEN = "histen";
    private static final String VALUE_BASS = "bass";

    private AlertDialog mDialog;

    /** 当前选项，与effect / speakerfx的判据一致。 */
    static String currentValue() {
        if (!"1".equals(SystemProperties.get(PROP, "0"))) {
            return VALUE_OFF;
        }
        return VALUE_HISTEN.equals(SystemProperties.get(ENGINE_PROP, "")) ? VALUE_HISTEN : VALUE_BASS;
    }

    /** 当前选项的显示文字，用作设置里条目的说明。 */
    static String currentLabel(Context context) {
        String[] labels = context.getResources().getStringArray(R.array.speaker_fx_engine_entries);
        String[] values = context.getResources().getStringArray(R.array.speaker_fx_engine_values);
        return labels[indexOf(values, currentValue())];
    }

    private static int indexOf(String[] values, String value) {
        for (int i = 0; i < values.length; i++) {
            if (values[i].equals(value)) {
                return i;
            }
        }
        return 0;
    }

    private static void apply(String value) {
        if (VALUE_OFF.equals(value)) {
            SystemProperties.set(PROP, "0");
        } else {
            SystemProperties.set(ENGINE_PROP, value);
            SystemProperties.set(PROP, "1");
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // 与设置自己的对话框同一主题：Expressive时用SettingsLib的Expressive对话框主题，
        // 否则（传0）按本activity主题的alertDialogTheme取Theme.AlertDialog.SettingsLib。
        AlertDialog.Builder builder = new AlertDialog.Builder(this,
                SettingsThemeHelper.isExpressiveTheme(this)
                        ? com.android.settingslib.widget.theme.R.style
                                .Theme_AlertDialog_SettingsLib_Expressive
                        : 0);
        Context dialogContext = builder.getContext();

        String[] labels = getResources().getStringArray(R.array.speaker_fx_engine_entries);
        String[] values = getResources().getStringArray(R.array.speaker_fx_engine_values);
        ListView list = new ListView(dialogContext);
        list.setAdapter(new ArrayAdapter<>(dialogContext, singleChoiceItemLayout(dialogContext),
                android.R.id.text1, labels));
        list.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
        list.setDivider(null);
        list.setItemChecked(indexOf(values, currentValue()), true);
        list.setOnItemClickListener((parent, view, position, id) -> {
            try {
                apply(values[position]);
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot set " + PROP + " / " + ENGINE_PROP, e);
            }
            SettingsEntryUris.notifySummaryChanged(this, SoundEntriesProvider.AUTHORITY,
                    SoundEntriesProvider.SPEAKER_FX_KEY);
            mDialog.dismiss();
        });

        mDialog = builder.setTitle(R.string.speaker_fx_engine_title)
                .setMessage(R.string.speaker_fx_note)
                .setView(list)
                .setNegativeButton(android.R.string.cancel, null)
                .create();
        // 旋转屏幕时activity重建、对话框重新显示，那一次关闭不结束activity。
        mDialog.setOnDismissListener(dialog -> {
            if (!isChangingConfigurations()) {
                finish();
            }
        });
        mDialog.show();
    }

    @Override
    protected void onDestroy() {
        if (mDialog != null) {
            mDialog.dismiss();
        }
        super.onDestroy();
    }

    /** 对话框主题的单选表项布局（AppCompat AlertController为单选列表用的同一个）。 */
    private static int singleChoiceItemLayout(Context dialogContext) {
        TypedArray a = dialogContext.obtainStyledAttributes(null,
                androidx.appcompat.R.styleable.AlertDialog,
                androidx.appcompat.R.attr.alertDialogStyle, 0);
        int layout = a.getResourceId(androidx.appcompat.R.styleable.AlertDialog_singleChoiceItemLayout,
                android.R.layout.simple_list_item_single_choice);
        a.recycle();
        return layout;
    }
}
