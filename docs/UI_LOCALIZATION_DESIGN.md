# System UI localization

## Scope and invariants

The first locales are English (`en`), Simplified Chinese (`zh-Hans`) and
Traditional Chinese (`zh-Hant`). The selected locale affects application UI
labels, buttons, prompts and status text. It does **not** convert book text,
filenames, SMS contents, lyrics, web responses, API identifiers or logs. The
per-book OpenCC script-conversion preference remains independent.

All recovery-critical UI text and glyphs must be present in firmware; changing
the language must not depend on an SD card or network connection. An absent or
invalid NVS preference falls back to English.

## Architecture

`ui/UiLocale` owns a small cached locale enum, one NVS byte and a firmware
resident catalog addressed by stable `TextId` values. English is the fallback
for an unavailable translation. UI code must never use English source text as
the lookup key. Dynamic values such as SSIDs, byte counts, percentages, dates
and names are composed after translation; they are not sent through book
script conversion.

Load the NVS choice immediately after `init_nvs()` and before the first screen.
A successful Settings choice commits NVS and changes the in-memory locale.
Returning from the choice screen redraws Settings with Quality; other
Activities read the current locale when shown. Screens retaining rendered
text or layouts must invalidate that cache when the locale generation changes.

The existing Settings list uses ten fixed 110-pixel rows. Adding Language
requires a scroll offset and focus tracking so the eleventh row is reachable
by touch swipe and keys. The language choice page always displays the native
names `English`, `简体中文`, `繁體中文` to make recovery intuitive.

## Font and layout

The built-in system font currently comes from a broad CJK font with a CN
source. Before declaring Traditional support complete, validate every catalog
codepoint and visually inspect Traditional glyph forms. Supply a firmware
resident Traditional UI font or relevant subset if the current glyph forms
are wrong. Never use `/sdcard/fonts` for recovery-critical menus. All labels
must be measured/ellipsized or wrapped within their existing touch targets.

## Rollout

1. Add locale catalog, boot loading, NVS persistence and Settings selector.
2. Migrate Home and Settings, including all Settings child screens.
3. Migrate Files, readers and their overlays.
4. Migrate music/radio, phone/SMS, transfer and network applications.
5. Audit remaining user-visible literals and font coverage; test all three
   languages, reboot persistence, SD-absent recovery and long strings.

Current implementation covers step 1, Home/recent bookshelf, the primary
Settings list and its WiFi, Network, Frontlight, Time Basis, DeepSeek,
Map, and RSS cache child screens. The shared reader Settings/Beauty/Chapters
overlay, common text-input UI, font/background/ringtone pickers, and main File
Browser actions are also migrated. DeepSeek's session/chat/composer screens and
image picker, and the map screen's fixed prompts and known client errors are
migrated. Phone/SMS and WeRead screens now use the catalog for fixed controls,
loading states and known local errors. Existing call/SMS records retain their
on-disk representation; legacy status values are translated only when drawn.
RSS, Music/Radio, image-viewer errors, and the device-side USB/Wi-Fi
transfer screens now use the catalog for their fixed controls and known local
status strings. News content, lyrics, filenames, station names, network/API
responses and stored call/SMS records remain untranslated. EPD Diagnostics
and touch calibration prompts are migrated as well. The transfer web UI and
remaining less-used screens still need a final literal/font audit. This
partial coverage must not be presented as complete system localization.
