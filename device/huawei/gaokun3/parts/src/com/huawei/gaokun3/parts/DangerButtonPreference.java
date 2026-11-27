/*
 * 用错误色（红色）绘制的 SettingsLib 按钮，用于无法在本页撤销的操作（手写笔设置页的“断开连接”）。
 *
 * SettingsLib 的 ButtonPreference 只有填充、色调、描边三种样式，没有危险操作的样式，所以沿用填充样式，
 * 绑定后把底色换成主题的 colorError、文字和图标换成 colorOnError。按钮布局的主题是
 * Theme.Material3.DynamicColors.DayNight（SettingsLibButtonStyle.Expressive.Filled），这两个颜色随系统
 * 动态取色与深浅色模式变化。
 */
package com.huawei.gaokun3.parts;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.util.AttributeSet;
import android.widget.Button;

import androidx.preference.PreferenceViewHolder;
import com.android.settingslib.widget.ButtonPreference;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.color.MaterialColors;

public class DangerButtonPreference extends ButtonPreference {

    public DangerButtonPreference(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    public void onBindViewHolder(PreferenceViewHolder holder) {
        super.onBindViewHolder(holder);
        Button button = getButton();
        if (button == null) {
            return;
        }
        // 用本应用合并后的 R：colorError 由 AppCompat 声明，colorOnError 由 Material 声明，各库自己的 R 里只有其一
        int error = MaterialColors.getColor(button, R.attr.colorError, Color.RED);
        int onError = MaterialColors.getColor(button, R.attr.colorOnError, Color.WHITE);
        button.setBackgroundTintList(ColorStateList.valueOf(error));
        button.setTextColor(onError);
        if (button instanceof MaterialButton) {
            ((MaterialButton) button).setIconTint(ColorStateList.valueOf(onError));
        }
    }
}
