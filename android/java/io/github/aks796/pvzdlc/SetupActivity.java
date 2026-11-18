package io.github.aks796.pvzdlc;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/* The launcher. The port's pictures (assets/wide) are copied into the app's files folder once per version, where
 * the game reads them. On the first launch the player picks their DLC Edition exe: it is copied out of the picked
 * file and unpacked into the app's external files folder (android_jni.c: evb_unpack), as the Switch does on its first
 * launch; then the copy goes. After that this only starts the game. */
public class SetupActivity extends Activity {
    static {
        System.loadLibrary("SDL2");
        System.loadLibrary("main");
    }

    private static final int PICK_EXE = 1;
    private native String unpack(String exe, String dir);

    private TextView text;
    private ProgressBar bar;
    private Button pick;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(Color.BLACK);
        box.setPadding(64, 32, 64, 32);
        text = new TextView(this);
        text.setTextColor(Color.WHITE);
        text.setTextSize(18);
        text.setGravity(Gravity.CENTER);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.GONE);
        pick = new Button(this);
        pick.setText("Choose the exe");
        pick.setVisibility(View.GONE);
        pick.setOnClickListener(v -> choose());
        box.addView(text, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 32;
        box.addView(bar, lp);
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        bp.topMargin = 32;
        box.addView(pick, bp);
        setContentView(box);

        text.setText("Getting ready…");
        new Thread(() -> {
            String err = copyPictures();
            runOnUiThread(() -> {
                if (err != null) show(err, false);
                else if (unpacked()) startGame();
                else show("Choose your Plants vs. Zombies: DLC Edition v2.7.2 exe (the one the DLC team's patch makes "
                          + "from your own copy of Plants vs. Zombies).\n\nThe game is unpacked from it once; after that "
                          + "the exe isn't needed.", true);
            });
        }).start();
    }

    private File gameDir() {
        File d = getExternalFilesDir(null);
        return d != null ? d : getFilesDir();
    }

    private boolean unpacked() {
        File d = gameDir();
        return new File(d, "dlc_game.exe").exists() && new File(d, "main.pak").exists();
    }

    private void show(String msg, boolean canPick) {
        text.setText(msg);
        bar.setVisibility(View.GONE);
        pick.setVisibility(canPick ? View.VISIBLE : View.GONE);
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }

    private void choose() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_EXE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_EXE || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        pick.setVisibility(View.GONE);
        bar.setVisibility(View.VISIBLE);
        bar.setProgress(0);
        text.setText("Copying the exe…");
        new Thread(() -> {
            File copy = new File(getCacheDir(), "dlc.exe");
            String err = null;
            try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(copy)) {
                long size = 213L << 20, done = 0;
                byte[] buf = new byte[1 << 20];
                for (int n; (n = in.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    done += n;
                    final int p = (int) Math.min(1000, done * 1000 / size);
                    runOnUiThread(() -> bar.setProgress(p));
                }
            } catch (Exception e) {
                err = "The exe couldn't be read: " + e.getMessage();
            }
            if (err == null) {
                runOnUiThread(() -> text.setText("Unpacking the game (first launch only)…"));
                err = unpack(copy.getPath(), gameDir().getPath());
            }
            copy.delete();
            final String msg = err;
            runOnUiThread(() -> {
                if (msg == null && unpacked()) startGame();
                else show(msg != null ? msg : "The game couldn't be unpacked.", true);
            });
        }).start();
    }

    /* evb_unpack's progress (from the unpacking thread) */
    @SuppressWarnings("unused")
    private void onUnpackProgress(String what, int permille) {
        runOnUiThread(() -> bar.setProgress(permille));
    }

    /* assets/wide -> files/wide, once per app version (its version code in files/wide/.version) */
    private String copyPictures() {
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            String version = Long.toString(pi.getLongVersionCode());
            File dir = new File(getFilesDir(), "wide"), stamp = new File(dir, ".version");
            if (stamp.exists() && version.equals(new String(java.nio.file.Files.readAllBytes(stamp.toPath())))) return null;
            copyAssets(getAssets(), "wide", dir);
            try (OutputStream out = new FileOutputStream(stamp)) {
                out.write(version.getBytes());
            }
            return null;
        } catch (Exception e) {
            return "The app's pictures couldn't be set up: " + e.getMessage();
        }
    }

    private static void copyAssets(AssetManager am, String path, File to) throws Exception {
        String[] names = am.list(path);
        if (names == null || names.length == 0) { /* a file */
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(to)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
            return;
        }
        to.mkdirs();
        for (String n : names) copyAssets(am, path + "/" + n, new File(to, n));
    }
}
