package local.preservation.chaosrings;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    // SDL3 is linked into libmain.so (static), so that is the one library to load
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
