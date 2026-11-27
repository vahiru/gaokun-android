/*
 * SettingsLib 的滑块，滑条下方只显示一段说明文字（手写笔设置页的笔迹平滑）。
 *
 * SliderPreference 滑条下方的两端标签（textStart / textEnd）各占半行：右端标签即使没有文字，也照样占着右半行，
 * 放在左端标签位置的说明文字只能在左半行里换行。这里在绑定后隐藏右端标签，说明文字占满整行。
 */
package com.huawei.gaokun3.parts;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;

import androidx.preference.PreferenceViewHolder;
import com.android.settingslib.widget.SliderPreference;

public class NoteSliderPreference extends SliderPreference {

    public NoteSliderPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    public void onBindViewHolder(PreferenceViewHolder holder) {
        super.onBindViewHolder(holder);
        View end = holder.findViewById(android.R.id.text2);
        if (end != null) {
            end.setVisibility(View.GONE);
        }
    }
}
