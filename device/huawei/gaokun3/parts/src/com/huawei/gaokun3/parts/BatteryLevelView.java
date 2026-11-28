/*
 * 横置的小电池图标：圆角外框、右侧正极凸起，内部按电量比例填充（StylusBatteryPopup 用）。
 * 外框与填充用主题的 colorOnSurfaceVariant，电量不高于 20% 时填充改用 colorError。
 */
package com.huawei.gaokun3.parts;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.View;

import com.google.android.material.color.MaterialColors;

public class BatteryLevelView extends View {

    private final Paint mStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mNub = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mRect = new RectF();
    private float mLevel;   // 0..1

    public BatteryLevelView(Context context, AttributeSet attrs) {
        super(context, attrs);
        float dp = getResources().getDisplayMetrics().density;
        mStroke.setStyle(Paint.Style.STROKE);
        mStroke.setStrokeWidth(1.5f * dp);
        mStroke.setColor(MaterialColors.getColor(this, R.attr.colorOnSurfaceVariant));
        mNub.setColor(mStroke.getColor());
        mFill.setStyle(Paint.Style.FILL);
    }

    /** 电量，0 到 1。 */
    public void setLevel(float level) {
        mLevel = Math.max(0f, Math.min(1f, level));
        mFill.setColor(MaterialColors.getColor(this,
                mLevel <= 0.2f ? R.attr.colorError : R.attr.colorOnSurfaceVariant));
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float dp = getResources().getDisplayMetrics().density;
        float half = mStroke.getStrokeWidth() / 2;
        float nub = 2 * dp;
        float w = getWidth(), h = getHeight();
        // 外框
        mRect.set(half, half, w - nub - half, h - half);
        canvas.drawRoundRect(mRect, 2.5f * dp, 2.5f * dp, mStroke);
        // 正极凸起
        mRect.set(w - nub, h * 0.3f, w, h * 0.7f);
        canvas.drawRoundRect(mRect, dp, dp, mNub);
        // 填充，与外框之间留一圈空隙
        float gap = mStroke.getStrokeWidth() + dp;
        float inner = (w - nub - 2 * gap) * mLevel;
        if (inner > 0) {
            mRect.set(gap, gap, gap + inner, h - gap);
            canvas.drawRoundRect(mRect, dp, dp, mFill);
        }
    }
}
