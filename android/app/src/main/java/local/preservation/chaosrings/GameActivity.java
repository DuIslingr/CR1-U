package local.preservation.chaosrings;

import org.libsdl.app.SDLActivity;
import android.content.pm.ActivityInfo;

public class GameActivity extends SDLActivity {
    // Called by the native Options screen after restoring or changing its lock.
    public void setOrientationLock(boolean locked, boolean flipped) {
        final int orientation = locked
            ? (flipped ? ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)
            : ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE;
        runOnUiThread(() -> setRequestedOrientation(orientation));
    }

    // SDL3 is linked into libmain.so (static), so that is the one library to load
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
