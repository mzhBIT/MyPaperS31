# Reader Simplified/Traditional Chinese Conversion

## Scope

Provide per-book display modes `Original`, `Traditional -> Simplified` (`t2s`),
and `Simplified -> Traditional` (`s2t`) for every text-based local reader:
TXT, EPUB, FB2, MOBI/PRC, AZW/AZW3, and Markdown. WeRead books use the same
reflow path. Source files, image pixels, archive members, and server-side
annotations must never be rewritten. Image-only books require OCR and are out
of scope. RSS uses a separate article renderer and is a later adapter, not a
reason to put conversion in the glyph renderer.

## Rules and dictionary provenance

Use a fixed upstream OpenCC revision's standard `s2t` and `t2s` dictionaries.
Match the longest applicable phrase first; use the character dictionary only
when no phrase matches. Preserve input when neither dictionary matches. Keep
dictionary priority consistent with the corresponding OpenCC configuration;
compare converter output against desktop OpenCC with a golden corpus. OpenCC's
CJK compatibility-ideograph normalization prepass remains a separate follow-up.
Do not include regional terminology modes (`*2twp`, `*2sp`) initially.

The PC-side tool builds versioned, checksummed compact binaries from upstream
text dictionaries. Device files belong in `/sdcard/.reader/system/opencc/`.
Only the selected direction is loaded, and it resides in PSRAM. Missing,
incompatible, or corrupt files must produce a visible error and leave text in
Original mode; never silently use incomplete character-only conversion.
Dictionary format is recorded in each binary header; upstream revision and
license attribution are recorded in the generated `SOURCE.txt` sidecar.
Honor OpenCC Apache-2.0 attribution when distributing its dictionaries.

## Conversion boundary

The reusable converter accepts UTF-8 and returns display UTF-8 plus a mapping
of output byte boundaries to source byte boundaries. Decode TXT's source
encoding first. Parse HTML/Markdown/FB2/MOBI markup and entities first, then
convert only visible text nodes/blocks, before text measurement and pagination.
Never convert resource identifiers, links, CSS, image paths, or markup syntax.
Convert chapter titles/TOC labels for display while preserving original IDs.

For TXT, existing file offsets stay canonical; paragraph layout uses converted
text and maps the first byte of each visual line back to a source file offset.
For reflow formats, retain each block's original content span and translate
line starts back to source positions. Remap inline-style and footnote byte
ranges into display text before pagination. A multi-character replacement is
treated as one source span; anchors inside it resolve to the nearest stable
source boundary. WeRead progress/comment requests and quoted source text stay
in the original script.

## Settings, persistence, and cache

Add `Script: Original / Simplified / Traditional` to the reading Settings tab.
Save the selected mode per book identity, default Original. Switching modes
does not immediately jump to a different page while the overlay is open; on
close, invalidate layout and prefetch data, reflow, and seek the previous
source-text anchor. Include mode, dictionary version, and mapping algorithm
version in TXT and reflow layout signatures. On load failures, retain the
book's original text and report why conversion was unavailable.

## Implementation order

1. PC dictionary builder, compact binary specification, golden tests.
2. Device converter and dictionary loader, measured conversion/PSRAM logs.
3. Reflow path with inline style, footnote, progress, and WeRead safeguards.
4. TXT paragraph conversion and source-offset mapping.
5. Reader Settings UI, per-book persistence, cache invalidation.
6. On-device acceptance: text and mixed-image books in each format; resume,
   mode switches, prefetch, bookmarks, footnotes, and missing-dictionary path.

No claim of byte-for-byte OpenCC compatibility is made until the desktop
comparison tests pass for the pinned dictionary revision.

## Current implementation and PC preparation

The first implementation has a checked binary loader, phrase-first longest
matching, single-character fallback, a per-book Settings switch, and source
position mapping for TXT and shared reflow formats. It does not yet implement
OpenCC's normalization prepass or exact generated-dictionary parity. The two
generated OpenCC dictionaries named in the official config may be absent from
a source checkout; the builder logs their absence and uses the available
primary phrase/character dictionaries. This is an OpenCC-style conversion,
not an exact port of OpenCC itself.

On the PC, clone the upstream OpenCC source, note its commit, and run:

```powershell
python tools/build_opencc_dictionary.py "E:\opensource\OpenCC\data\dictionary" "X:\.reader\system\opencc"
```

Replace the example OpenCC directory and `X:` with the actual paths. The output
is `s2t.bin`, `t2s.bin`, and `SOURCE.txt`. Keep the source checkout
and `SOURCE.txt` for license/reproducibility. If only one direction is desired,
both files are still generated; the device only loads the selected direction.
Run `python -m unittest tools.test_opencc_dictionary` before copying.
