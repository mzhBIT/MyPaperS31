# Application header layout

`main/ui/HeaderLayout.h` owns the 76 px application navigation header.

- Use `height` for both the painted header and the first content row.
- Use `text_y()` and `item_y()` to center text and icons above the separator.
- Use `draw_separator()` for the full-width bottom line.
- On tap events, use `left_hit()` / `right_hit()` for navigation/actions.
  Button widths remain page-specific so existing actions do not overlap.
- Header hit bounds are half-open: y=0..75 belongs to the header; y=76
  belongs to content. Negative coordinates and x outside the button are rejected.

TXT and reflow readers retain compact 50 px or hidden headers. RSS uses the
common header for feeds/news lists and a compact/hidden header for articles;
its hit test passes the actual header height. Music/radio retain custom layouts.
Image viewer retains its existing whole-page tap-to-exit behavior; font picker
retains its existing Back gesture/key behavior, without adding a new button.

Changing navigation geometry does not alter refresh modes or EPD timing.
