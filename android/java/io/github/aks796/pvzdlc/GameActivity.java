package io.github.aks796.pvzdlc;

import org.libsdl.app.SDLActivity;

/* The game: SDL2's activity running libmain.so (main_posix.c's android_setup finds the unpacked game in the app's
 * external files folder and the port's pictures in its internal one). */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }
}
