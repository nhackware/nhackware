package nh;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;

/**
 * Loaded by the patched manifest as a ContentProvider, which Android instantiates
 * before Application.onCreate and before any Activity exists. Using a provider
 * instead of an Application subclass means we never collide with the app's own
 * Application class and the whole thing compiles against android.jar alone.
 *
 * Everything runs as the app's own uid: no root, no ptrace, no /dev/input.
 */
public final class NhLoader extends ContentProvider {

    @Override
    public boolean onCreate() {
        try {
            System.loadLibrary("nhmenu");
        } catch (Throwable t) {
            android.util.Log.e("NHMENU", "loadLibrary failed", t);
            return false;
        }
        // The native ctor already spawned its worker; kick off activity discovery.
        try {
            NhTouch.install(getContext());
        } catch (Throwable t) {
            android.util.Log.e("NHMENU", "touch hook install failed", t);
        }
        return true;
    }

    @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String s, String[] a) { return 0; }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) { return 0; }
}
