package nh;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;

/**
 * Touch source for the no-root build. /dev/input is root:input 0660, so an
 * unprivileged app cannot read it; instead we wrap the Window.Callback of the
 * resumed Activity, which sees every MotionEvent before it is dispatched to any
 * view. The game still receives the event - we only mirror the coordinates.
 */
public final class NhTouch implements Window.Callback {

    private static final String TAG = "NHMENU";

    /** Implemented in libnhmenu.so. Coordinates are view pixels plus the view size. */
    public static native void onMotion(int action, float x, float y, int vw, int vh);

    private final Window.Callback wrapped;
    private volatile View decor;

    public NhTouch(Window.Callback wrapped) {
        this.wrapped = wrapped;
    }

    // ---------------------------------------------------------------- install

    public static void install(Context ctx) {
        final Application app = asApplication(ctx);
        if (app == null) {
            Log.w(TAG, "no Application available yet, retrying");
            retryInstall(ctx);
            return;
        }
        if (!installNow(app)) {
            // No resumed Activity at provider-init time; watch for one.
            app.registerActivityLifecycleCallbacks(new LifecycleWatcher(app));
            retryInstall(app);
        }
    }

    private static void retryInstall(final Context ctx) {
        new Thread(new Runnable() {
            @Override public void run() {
                for (int i = 0; i < 120; i++) {
                    try { Thread.sleep(500); } catch (InterruptedException e) { return; }
                    Application app = asApplication(ctx);
                    if (app != null && installNow(app)) return;
                }
            }
        }, "nh-touch-wait").start();
    }

    /** Wraps the callback of every live Activity. Returns true if at least one was wrapped. */
    private static boolean installNow(Application app) {
        boolean any = false;
        for (Activity a : liveActivities()) {
            any |= wrap(a);
        }
        return any;
    }

    private static boolean wrap(Activity a) {
        try {
            Window w = a.getWindow();
            if (w == null) return false;
            Window.Callback cur = w.getCallback();
            if (cur instanceof NhTouch) return true; // already wrapped
            NhTouch t = new NhTouch(cur);
            t.decor = w.getDecorView();
            w.setCallback(t);
            Log.i(TAG, "touch hook installed on " + a.getClass().getName());
            return true;
        } catch (Throwable e) {
            Log.w(TAG, "wrap failed", e);
            return false;
        }
    }

    /** ActivityThread.mActivities -> ActivityClientRecord.activity. Stable since API 14. */
    @SuppressWarnings("unchecked")
    private static java.util.List<Activity> liveActivities() {
        java.util.List<Activity> out = new java.util.ArrayList<Activity>();
        try {
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Method current = atClass.getMethod("currentActivityThread");
            Object at = current.invoke(null);
            if (at == null) return out;

            Field f = atClass.getDeclaredField("mActivities");
            f.setAccessible(true);
            Object map = f.get(at);
            if (map == null) return out;

            Iterable<?> values;
            if (map instanceof Map) {
                values = ((Map<IBinder, Object>) map).values();
            } else {
                // ArrayMap on some builds exposes values() but is not a java.util.Map
                Method m = map.getClass().getMethod("values");
                Object v = m.invoke(map);
                values = (Iterable<?>) v;
            }

            for (Object rec : values) {
                if (rec == null) continue;
                Field af = rec.getClass().getDeclaredField("activity");
                af.setAccessible(true);
                Object act = af.get(rec);
                if (act instanceof Activity) out.add((Activity) act);
            }
        } catch (Throwable e) {
            Log.w(TAG, "activity enumeration failed", e);
        }
        return out;
    }

    private static Application asApplication(Context ctx) {
        if (ctx instanceof Application) return (Application) ctx;
        if (ctx != null) {
            Context app = ctx.getApplicationContext();
            if (app instanceof Application) return (Application) app;
        }
        try {
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Object at = atClass.getMethod("currentActivityThread").invoke(null);
            if (at != null) {
                Object app = atClass.getMethod("getApplication").invoke(at);
                if (app instanceof Application) return (Application) app;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private static final class LifecycleWatcher implements Application.ActivityLifecycleCallbacks {
        private final Application app;
        private final Handler main = new Handler(Looper.getMainLooper());

        LifecycleWatcher(Application app) { this.app = app; }

        @Override public void onActivityResumed(Activity a) {
            // Defer: the app may replace the callback during its own resume.
            final Activity act = a;
            main.postDelayed(new Runnable() {
                @Override public void run() { wrap(act); }
            }, 200);
        }

        @Override public void onActivityCreated(Activity a, android.os.Bundle b) {}
        @Override public void onActivityStarted(Activity a) {}
        @Override public void onActivityPaused(Activity a) {}
        @Override public void onActivityStopped(Activity a) {}
        @Override public void onActivitySaveInstanceState(Activity a, android.os.Bundle b) {}
        @Override public void onActivityDestroyed(Activity a) {}
    }

    // ------------------------------------------------------------- forwarding

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        if (ev != null) {
            try {
                View d = decor;
                int vw = d != null ? d.getWidth() : 0;
                int vh = d != null ? d.getHeight() : 0;
                onMotion(ev.getActionMasked(), ev.getX(), ev.getY(), vw, vh);
            } catch (Throwable ignored) {
            }
        }
        return wrapped.dispatchTouchEvent(ev);
    }

    // Everything else is a straight pass-through to the original callback.

    @Override public boolean dispatchKeyEvent(android.view.KeyEvent e) { return wrapped.dispatchKeyEvent(e); }
    @Override public boolean dispatchKeyShortcutEvent(android.view.KeyEvent e) { return wrapped.dispatchKeyShortcutEvent(e); }
    @Override public boolean dispatchTrackballEvent(MotionEvent e) { return wrapped.dispatchTrackballEvent(e); }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) { return wrapped.dispatchGenericMotionEvent(e); }
    @Override public boolean dispatchPopulateAccessibilityEvent(android.view.accessibility.AccessibilityEvent e) { return wrapped.dispatchPopulateAccessibilityEvent(e); }
    @Override public View onCreatePanelView(int id) { return wrapped.onCreatePanelView(id); }
    @Override public boolean onCreatePanelMenu(int id, android.view.Menu m) { return wrapped.onCreatePanelMenu(id, m); }
    @Override public boolean onPreparePanel(int id, View v, android.view.Menu m) { return wrapped.onPreparePanel(id, v, m); }
    @Override public boolean onMenuOpened(int id, android.view.Menu m) { return wrapped.onMenuOpened(id, m); }
    @Override public boolean onMenuItemSelected(int id, android.view.MenuItem i) { return wrapped.onMenuItemSelected(id, i); }
    @Override public void onWindowAttributesChanged(android.view.WindowManager.LayoutParams a) { wrapped.onWindowAttributesChanged(a); }
    @Override public void onContentChanged() { wrapped.onContentChanged(); }
    @Override public void onWindowFocusChanged(boolean f) { wrapped.onWindowFocusChanged(f); }
    @Override public void onAttachedToWindow() { wrapped.onAttachedToWindow(); }
    @Override public void onDetachedFromWindow() { wrapped.onDetachedFromWindow(); }
    @Override public void onPanelClosed(int id, android.view.Menu m) { wrapped.onPanelClosed(id, m); }
    @Override public boolean onSearchRequested() { return wrapped.onSearchRequested(); }
    @Override public android.view.ActionMode onWindowStartingActionMode(android.view.ActionMode.Callback c) { return wrapped.onWindowStartingActionMode(c); }
    @Override public void onActionModeStarted(android.view.ActionMode m) { wrapped.onActionModeStarted(m); }
    @Override public void onActionModeFinished(android.view.ActionMode m) { wrapped.onActionModeFinished(m); }

    // Added after API 23/24/26 - required when compiling against android-30.
    @Override public boolean onSearchRequested(android.view.SearchEvent e) { return wrapped.onSearchRequested(e); }
    @Override public android.view.ActionMode onWindowStartingActionMode(android.view.ActionMode.Callback c, int type) { return wrapped.onWindowStartingActionMode(c, type); }
    @Override public void onProvideKeyboardShortcuts(java.util.List<android.view.KeyboardShortcutGroup> data, android.view.Menu m, int deviceId) { wrapped.onProvideKeyboardShortcuts(data, m, deviceId); }
    @Override public void onPointerCaptureChanged(boolean hasCapture) { wrapped.onPointerCaptureChanged(hasCapture); }
}
