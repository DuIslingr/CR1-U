#pragma once
// The game's frame: the original runs everything - scripts, menus, battles, movement, fades,
// animation frames - once a frame at 20 frames a second. 1.0.1 and 2.2.0 schedule EAGLView's
// drawView (-> CRView prc -> gameapp_prc, one frame) on an NSTimer of 0.05 s; the Android release
// paces onDrawFrame to 50 ms and adds 50 ms of play time a frame. The engine steps its logic at
// that rate and draws frames as often as the display takes them (60 a second by default), showing
// what moves between steps where it will be.

namespace cr {

constexpr float kFramesPerSecond = 20.0f;

} // namespace cr
