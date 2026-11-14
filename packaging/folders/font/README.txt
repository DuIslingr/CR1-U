Chaos Rings (CR1:U): your own fonts, if you want them - .ttf, .otf or .ttc files put here.

Leave this folder empty and the game uses the system's fonts, as the original asks for them:
Arial (or Liberation Sans / Arimo, made to Arial's widths) for English, French, German, Spanish,
Chinese and Korean, and a bold Japanese Gothic (Hiragino W6 / Noto Sans CJK Bold / Yu Gothic Bold)
for Japanese, with a CJK font behind them for the characters they lack.

A font here takes the place of one of those:
- a font with Japanese / Chinese / Korean characters takes the CJK font's place, and the Japanese
  bold's too - unless a second one with "bold" in its file name is here for that;
- a font without them takes Arial's place (the Latin text; CJK characters still come from the
  CJK font).
Several of a kind: the first by file name. A font far wider than Arial can run long lines off the
message window. The CR_FONT, CR_FONT_BOLD and CR_LATIN_FONT environment variables still come first.
The log (chaosrings.log in the profile folder) names the font/ files it used.
