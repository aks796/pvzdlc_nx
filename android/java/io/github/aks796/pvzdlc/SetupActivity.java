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
 * launch; then the copy goes. Or they pick the DLC team's patch (an .xdelta: a VCDIFF file) and then their own
 * PlantsVsZombies.exe: both are copied out and the port makes the DLC Edition exe from them (android_jni.c:
 * unpackPatched). After that this only starts the game. */
public class SetupActivity extends Activity {
    static {
        System.loadLibrary("SDL2");
        System.loadLibrary("main");
    }

    private static final int PICK_EXE = 1, PICK_BASE = 2;
    private native String unpack(String exe, String dir);
    private native String unpackPatched(String patch, String base, String dir);

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
        pick.setText("Choose the exe or the patch");
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
                else show(FIRST, true);
            });
        }).start();
    }

    private static final String FIRST = "Choose your Plants vs. Zombies: DLC Edition v2.7.2 exe, or the DLC team's patch "
            + "for your own copy of Plants vs. Zombies (Steam Patch.xdelta, EA Patch.xdelta or OG Patch.xdelta).\n\n"
            + "The game is unpacked from it once; after that it isn't needed.";

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

    private boolean wantBase; /* the patch is copied out: its PlantsVsZombies.exe is picked next */

    private void choose() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, wantBase ? PICK_BASE : PICK_EXE);
    }

    /* the picked document copied to path (the bar shows it against about size bytes); null when done, else why not */
    private String copyOut(Uri uri, File path, long size) {
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(path)) {
            long done = 0;
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                done += n;
                final int p = (int) Math.min(1000, done * 1000 / size);
                runOnUiThread(() -> bar.setProgress(p));
            }
            return null;
        } catch (Exception e) {
            return "The file couldn't be read: " + e.getMessage();
        }
    }

    /* a VCDIFF file (the DLC's .xdelta patches) starts D6 C3 C4 00 */
    private static boolean isPatch(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] m = new byte[4];
            return in.read(m) == 4 && (m[0] & 0xff) == 0xd6 && (m[1] & 0xff) == 0xc3 && (m[2] & 0xff) == 0xc4 && m[3] == 0;
        } catch (Exception e) {
            return false;
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if ((request != PICK_EXE && request != PICK_BASE) || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        boolean base = request == PICK_BASE;
        pick.setVisibility(View.GONE);
        bar.setVisibility(View.VISIBLE);
        bar.setProgress(0);
        text.setText(base ? "Copying PlantsVsZombies.exe…" : "Copying the file…");
        new Thread(() -> {
            File first = new File(getCacheDir(), "picked.bin"), pvz = new File(getCacheDir(), "PlantsVsZombies.exe");
            String err;
            if (!base) {
                err = copyOut(uri, first, 213L << 20);
                if (err == null && isPatch(first)) {
                    /* the DLC's patch: its PlantsVsZombies.exe next */
                    runOnUiThread(() -> {
                        wantBase = true;
                        pick.setText("Choose PlantsVsZombies.exe");
                        show("Now choose your own Plants vs. Zombies exe (PlantsVsZombies.exe from your Steam, EA app or "
                             + "original install): the one this patch is for.", true);
                    });
                    return;
                }
                if (err == null) {
                    runOnUiThread(() -> text.setText("Unpacking the game (first launch only)…"));
                    err = unpack(first.getPath(), gameDir().getPath());
                }
            } else {
                err = copyOut(uri, pvz, 5L << 20);
                if (err == null) {
                    runOnUiThread(() -> text.setText("Patching and unpacking the game (first launch only)…"));
                    err = unpackPatched(first.getPath(), pvz.getPath(), gameDir().getPath());
                }
            }
            first.delete();
            pvz.delete();
            final String msg = err;
            runOnUiThread(() -> {
                wantBase = false;
                pick.setText("Choose the exe or the patch");
                if (msg == null && unpacked()) startGame();
                else show(msg != null ? msg + "\n\n" + FIRST : "The game couldn't be unpacked.", true);
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
