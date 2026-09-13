# Pinyin Chinese Input Method Plan

## Goals

- Provide reusable UTF-8 text entry for file rename and future search, notes, and settings.
- Support offline full-pinyin input with candidates, paging, English, numbers, and symbols.
- Keep input responsive on the EPD by redrawing only the editor/composition/candidate regions with `epd_fast`.
- Work without an SD dictionary, while allowing a larger SD dictionary to extend the built-in vocabulary.

## Architecture

1. `ui/Utf8Text`
   - Validates/truncates UTF-8 at codepoint boundaries.
   - Deletes one complete codepoint rather than one byte.
   - Enforces both character and filesystem byte limits.
2. `ime/PinyinEngine`
   - Owns the ASCII composition string and candidate page.
   - Uses a compact built-in common-word table.
   - Looks up `/sdcard/.reader/system/ime/pinyin.bin` with an on-disk binary search, or falls back to `/sdcard/.reader/system/ime/pinyin.txt` during development. Text lines are `pinyin<TAB>candidate1 candidate2 ...`.
   - Keeps lookup memory bounded; the complete external dictionary is never loaded into RAM.
3. `ui/VirtualKeyboard`
   - Remains the reusable key hit-test and drawing component.
   - Gains an explicit Chinese/English mode key and UTF-8-safe committed text handling.
4. `activities/TextInputActivity`
   - Presents committed text, pinyin composition, candidates, keyboard, cancel, and submit.
   - Returns the submitted string through a completion callback.
5. `FileBrowserActivity`
   - Long press opens Rename/Delete/Cancel.
   - Rename pre-fills the original filename, preserves its extension by default, validates the result, and calls `rename()`.
   - Successful book renames update the bookshelf path record.

## Input behavior

- Letter keys in Chinese mode append lowercase letters to the active pinyin composition.
- Candidate tap commits the selected UTF-8 word and clears composition.
- Space commits the first candidate; with no composition it inserts a normal space.
- Backspace first edits composition, then deletes one committed UTF-8 codepoint.
- `中/英` switches modes. `123/ABC` keeps the existing symbol layout.
- Candidate arrows page through results. Apostrophe is accepted as a pinyin syllable separator.
- The first version edits at the end of the string. Cursor movement can be added without changing the engine API.

## Dictionary and storage

- Built-in table: common characters and words needed for immediate offline use.
- External text dictionary: easy to generate and inspect during bring-up.
- `tools/build_pinyin_dict.py` converts that source into the sorted `PYD1` binary dictionary used for normal operation. New dictionaries append a `PY92` digit-sorted list of pinyin spellings, allowing sequential nine-key lookup without random SD reads. New firmware also accepts the earlier `PY91` index; old firmware ignores either trailer.
- The converter also accepts pyim dictionaries (`pinyin-with-hyphens candidate1 candidate2 ...`) and removes syllable hyphens automatically.
- User frequency is deliberately deferred until candidate quality is validated; it will be stored in an atomic file under `/sdcard/.reader/ime` rather than NVS.

## Rename safety

- Reject empty names, `.`, `..`, `/`, control characters, and names exceeding the FAT path component byte limit.
- Reject collisions and report filesystem errors without leaving the browser.
- Preserve the original extension unless the user explicitly includes a different one in the submitted name.
- Handle case-only renames through a temporary name when the filesystem requires it.
- Update bookshelf entries atomically after a successful book rename.

## Delivery order and acceptance

1. UTF-8 helpers and generic input activity compile and delete Chinese characters correctly.
2. Built-in pinyin lookup returns paged candidates offline.
3. Candidate UI supports touch, Chinese/English switching, submit, and cancel.
4. File browser can rename supported files and directories and immediately reload the list.
5. Renamed TXT/EPUB shelf entries remain valid.
6. Full firmware build succeeds; existing Wi-Fi password keyboard remains functional.
