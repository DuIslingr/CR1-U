package local.preservation.chaosrings;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;
import android.content.pm.ActivityInfo;
import android.app.AlertDialog;
import android.content.Context;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.SparseArray;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public class GameActivity extends SDLActivity {
    // Called by the native Options screen after restoring or changing its lock.
    public void setOrientationLock(boolean locked, boolean flipped) {
        final int orientation = locked
            ? (flipped ? ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)
            : ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE;
        runOnUiThread(() -> setRequestedOrientation(orientation));
    }

    // SDL3 is linked into libmain.so (static), so that is the one library to load
    // The game's surface without Android's focus highlight: a controller's or a keyboard's keys put
    // Android in its keyboard-navigation mode, and the surface - the focused view - had a coloured
    // frame drawn round the whole screen from then on
    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface surface = super.createSDLSurface(context);
        if (android.os.Build.VERSION.SDK_INT >= 26) surface.setDefaultFocusHighlightEnabled(false);
        return surface;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    // SDL's message box (its setup's questions, its errors), redone: SDL's puts a row of buttons under
    // the text with nothing to scroll, so on a landscape phone a long text pushes the buttons off the
    // dialog and nothing can be chosen. Here the text scrolls (capped to part of the screen), the
    // buttons always show (one row, scrolled sideways if they don't fit), and the dialog takes most
    // of the screen's width.
    static final class CappedScroll extends ScrollView {
        private final int cap;
        CappedScroll(Context context, int cap) { super(context); this.cap = cap; }
        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            super.onMeasure(widthSpec, MeasureSpec.makeMeasureSpec(cap, MeasureSpec.AT_MOST));
        }
    }

    @Override
    protected void messageboxCreateAndShow(Bundle args) {
        final DisplayMetrics screen = getResources().getDisplayMetrics();
        final float dp = screen.density;
        final AlertDialog dialog = new AlertDialog.Builder(this).create();
        dialog.setTitle(args.getString("title"));
        dialog.setCancelable(false);
        dialog.setOnDismissListener(unused -> {
            synchronized (messageboxSelection) {
                messageboxSelection.notify();
            }
        });

        TextView message = new TextView(this);
        message.setText(args.getString("message"));
        message.setGravity(Gravity.START);
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        final int pad = (int) (20 * dp);
        message.setPadding(pad, (int) (8 * dp), pad, (int) (8 * dp));
        CappedScroll scroll = new CappedScroll(this, (int) (screen.heightPixels * 0.5f));
        scroll.addView(message);

        int[] buttonFlags = args.getIntArray("buttonFlags");
        int[] buttonIds = args.getIntArray("buttonIds");
        String[] buttonTexts = args.getStringArray("buttonTexts");
        final SparseArray<Button> keys = new SparseArray<>();
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        for (int i = 0; buttonTexts != null && i < buttonTexts.length; ++i) {
            Button button = new Button(this);
            final int id = buttonIds[i];
            button.setText(buttonTexts[i]);
            button.setAllCaps(false);
            button.setOnClickListener(v -> {
                messageboxSelection[0] = id;
                dialog.dismiss();
            });
            if ((buttonFlags[i] & 0x00000001) != 0) keys.put(KeyEvent.KEYCODE_ENTER, button); // (SDL_messagebox.h: return's)
            if ((buttonFlags[i] & 0x00000002) != 0) keys.put(KeyEvent.KEYCODE_ESCAPE, button); // (escape's)
            row.addView(button);
        }
        HorizontalScrollView buttons = new HorizontalScrollView(this);
        buttons.setFillViewport(true);
        buttons.addView(row);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        content.addView(buttons, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        dialog.setView(content);
        dialog.setOnKeyListener((d, keyCode, event) -> {
            Button button = keys.get(keyCode);
            if (button == null) return false;
            if (event.getAction() == KeyEvent.ACTION_UP) button.performClick();
            return true;
        });
        dialog.show();
        if (dialog.getWindow() != null)
            dialog.getWindow().setLayout((int) (screen.widthPixels * (screen.widthPixels > screen.heightPixels ? 0.8f : 0.94f)),
                                         ViewGroup.LayoutParams.WRAP_CONTENT);
    }
}
