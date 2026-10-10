package nh.overlay;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.InputStreamReader;

public class OverlayService extends Service {
    static final String PKG = "com.axlebolt.standoff2";
    private WindowManager wm;
    private View menuView;
    private boolean menuOpen;
    static boolean hasRoot;

    // feature toggles, [tab][item]; offsets/reader hook in later
    static final String[][] ITEMS = {
            { "aimbot", "silent", "trigger" },
            { "esp box", "esp name", "esp hp" },
            { "speed", "no recoil", "no spread" },
            { "root status", "unload", "" },
    };
    static final boolean[][] ON = new boolean[4][3];

    @Override
    public void onCreate() {
        super.onCreate();
        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        checkRoot();
        addRedButton();
    }

    private void checkRoot() {
        new Thread(() -> {
            hasRoot = runSu("id") != null;
            new Handler(Looper.getMainLooper()).post(() ->
                Toast.makeText(this, hasRoot ? "nh: root ok" : "nh: нет root — дай права su",
                        Toast.LENGTH_LONG).show());
        }).start();
    }

    // run a command as root; returns first output line or null
    static String runSu(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            p.waitFor();
            return line;
        } catch (Exception e) {
            return null;
        }
    }

    private WindowManager.LayoutParams lp(int w, int h, int gravity) {
        int type = Build.VERSION.SDK_INT >= 26
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;
        WindowManager.LayoutParams p = new WindowManager.LayoutParams(w, h, type,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        p.gravity = gravity;
        return p;
    }

    private void addRedButton() {
        final View btn = new RedButton(this);
        int sz = (int) (64 * getResources().getDisplayMetrics().density);
        WindowManager.LayoutParams p = lp(sz, sz, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        btn.setOnTouchListener((v, ev) -> {
            if (ev.getAction() == MotionEvent.ACTION_UP) toggleMenu();
            return true;
        });
        wm.addView(btn, p);
    }

    private void toggleMenu() {
        if (menuOpen) {
            if (menuView != null) wm.removeView(menuView);
            menuView = null;
            menuOpen = false;
        } else {
            menuView = new MenuView(this);
            int mh = (int) (270 * getResources().getDisplayMetrics().density);
            wm.addView(menuView, lp(WindowManager.LayoutParams.MATCH_PARENT, mh, Gravity.BOTTOM));
            menuOpen = true;
        }
    }

    @Override
    public IBinder onBind(Intent i) { return null; }

    @Override
    public void onDestroy() {
        super.onDestroy();
    }

    // the small red floating button at the bottom
    static class RedButton extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        RedButton(Context c) { super(c); }
        @Override
        protected void onDraw(Canvas c) {
            p.setColor(0xEF140F0F);
            RectF r = new RectF(4, 4, getWidth() - 4, getHeight() - 4);
            c.drawRoundRect(r, 14, 14, p);
            p.setColor(Color.WHITE);
            p.setTextSize(getHeight() / 5f);
            p.setTextAlign(Paint.Align.CENTER);
            c.drawText("MENU", getWidth() / 2f, getHeight() / 2f + getHeight() / 10f, p);
        }
    }

    // the menu: red bottom bar with tabs + a panel of toggles above it
    static class MenuView extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int tab;
        private final float d = getResources().getDisplayMetrics().density;
        MenuView(Context c) { super(c); }

        private float barH() { return 46 * d; }

        @Override
        protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            // dim panel above the bar
            float panelH = 220 * d;
            p.setColor(0xCC111114);
            c.drawRect(0, h - barH() - panelH, w, h - barH(), p);
            // title
            p.setColor(0xFF39FF5A);
            p.setTextSize(16 * d);
            p.setTextAlign(Paint.Align.LEFT);
            c.drawText("nh menu 1.0.0  |  root:" + (hasRoot ? "ok" : "no"), 12 * d, h - barH() - panelH + 22 * d, p);
            // items of current tab
            p.setColor(Color.WHITE);
            p.setTextSize(15 * d);
            for (int i = 0; i < 3; i++) {
                String it = ITEMS[tab][i];
                if (it.isEmpty()) continue;
                float y = h - barH() - panelH + (48 + i * 34) * d;
                // checkbox
                p.setStyle(Paint.Style.STROKE);
                p.setColor(ON[tab][i] ? 0xFF39FF5A : 0xFF777777);
                c.drawRect(12 * d, y - 14 * d, 26 * d, y, p);
                p.setStyle(Paint.Style.FILL);
                if (ON[tab][i]) c.drawRect(15 * d, y - 11 * d, 23 * d, y - 3 * d, p);
                p.setColor(Color.WHITE);
                c.drawText(it, 34 * d, y, p);
            }
            // red bottom bar + 4 tabs
            p.setColor(0xEFB81010);
            c.drawRect(0, h - barH(), w, h, p);
            p.setTextSize(14 * d);
            String[] tabs = { u8("БОЙ"), u8("ВИЗУАЛ"), u8("РАЗНОЕ"), u8("НАСТРОЙКИ") };
            for (int i = 0; i < 4; i++) {
                p.setColor(i == tab ? Color.WHITE : 0xAAFFFFFF);
                p.setTextAlign(Paint.Align.CENTER);
                c.drawText(tabs[i], w / 8f + i * w / 4f, h - barH() / 2f + 5 * d, p);
            }
        }

        private String u8(String s) { return s; }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            if (ev.getAction() != MotionEvent.ACTION_UP) return true;
            int w = getWidth(), h = getHeight();
            float x = ev.getX(), y = ev.getY();
            // red bar: right edge closes, else tab select
            if (y > h - barH()) {
                if (x > w - 70 * d) { ((OverlayService) getContext()).toggleMenu(); return true; }
                tab = Math.min(3, Math.max(0, (int) (x / (w / 4f))));
                invalidate();
                return true;
            }
            // panel -> toggle rows
            float panelH = 220 * d;
            float top = h - barH() - panelH;
            if (y > top && y < h - barH()) {
                for (int i = 0; i < 3; i++) {
                    float ry = top + (48 + i * 34) * d;
                    if (Math.abs(y - ry + 7 * d) < 17 * d && x < w / 2f) {
                        String it = ITEMS[tab][i];
                        if (it.equals("unload")) {
                            stopSelf();
                            return true;
                        }
                        if (!it.isEmpty()) { ON[tab][i] = !ON[tab][i]; invalidate(); }
                        return true;
                    }
                }
            }
            return true; // swallow touches inside the menu
        }
    }
}
