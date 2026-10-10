package nh.overlay;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.graphics.Color;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;

public class MainActivity extends Activity {
    private static final int OVERLAY_REQ = 1001;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(0xEE111114);

        Button load = new Button(this);
        load.setText("load (1.0.0 menu)");
        load.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        load.setBackgroundColor(0xEF1F0F0F); // red, like the bar
        load.setTextColor(Color.WHITE);
        load.setOnClickListener(v -> tryStart());
        root.addView(load, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        setContentView(root);
    }

    private void tryStart() {
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
            startActivityForResult(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName())), OVERLAY_REQ);
            return;
        }
        start();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == OVERLAY_REQ) {
            if (Settings.canDrawOverlays(this)) start();
            else finish();
        }
    }

    private void start() {
        startService(new Intent(this, OverlayService.class));
        finish(); // go back to the game; the overlay stays
    }
}
