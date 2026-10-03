# Independent RSS reading preferences

RSS explicitly passes `ReaderProfile::Rss` to its settings overlay, font/image
pickers and renderer. Default API arguments remain `ReaderProfile::Shared`, so
local books and WeRead retain their existing configuration and storage paths.
No global profile switch or temporary replacement of book preferences is used.

## Persistence

- RSS layout/beauty: NVS `rss_reader/style_v7` (same validated ReaderStyle schema).
- RSS font: NVS `rss_reader/font`; shared font still uses its existing SD config.
- RSS background: NVS `rss_reader_bg/path`, independent cache
  `/sdcard/.reader/rss_background.g4` and temporary `.g4.tmp`.
- RSS book cover: NVS `rss_reader_cover/path`.

The first RSS visit snapshots current shared settings and selections, then saves
them independently. Background/cover `profile_v1` markers prevent a disabled
selection from inheriting book artwork again after reboot. Background RAM/cache
belongs to each profile; the decoded payload is not copied or generated during
initial migration. Renaming/deleting the source image updates both selections
if they reference that file. Settings are persisted on changes, not on page flips.

Script conversion/writing-direction options remain book-path based; RSS did not
implement those operations before this separation and this change does not add them.
Network settings and system language remain global.

## Device verification

1. Set RSS to a distinct font/size/margins/refresh mode and hide its header.
2. Open local TXT/EPUB and WeRead: existing book settings must remain unchanged.
3. Change book settings, return to RSS: RSS retains its own values.
4. Set different RSS/book backgrounds; disable RSS background and reboot.
   RSS stays disabled while the book background stays enabled.
5. Select/disable the RSS book cover and verify the book selection is unchanged.
6. Rename/delete an image referenced by either profile; return to the relevant
   reading settings to verify its selection follows the file operation.

P4 build verified. Device persistence/UI checks require flashing the firmware.
