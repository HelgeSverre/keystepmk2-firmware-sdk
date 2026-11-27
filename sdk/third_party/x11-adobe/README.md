# X11 Adobe bitmap fonts

`helvR10.bdf` (Helvetica Medium, 10 pixels, 75 dpi) and `helvB12.bdf` (Helvetica Bold, 12 pixels,
75 dpi), unchanged from the X.Org `font/adobe-75dpi` package
(https://gitlab.freedesktop.org/xorg/font/adobe-75dpi, commit 6189f2a6). The SDK's two display
fonts, `ks_font_small` and `ks_font_large` in `sdk/include/keystep_fonts.h`, are made from them by
`tools/bdffont.py`.

They are under the notice in `COPYING`, which allows use, copying, modification, distribution and
sale without fee as long as that notice travels with them. Helvetica is a trademark of its owner;
the SDK's fonts derived from these bitmaps carry no such name.
