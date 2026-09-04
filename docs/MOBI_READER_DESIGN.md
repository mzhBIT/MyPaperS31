# MyPaperP4 MOBI reader design

Status: implementation in progress

## Scope

The first device-side implementation targets reflowable, DRM-free MOBI/PalmDOC
documents. It reuses the existing HTML block parser, document paginator, text
renderer, reader overlay, image decoders, bookshelf and progress UI.

Supported by the first reader pipeline:

- `.mobi` and `.prc` Palm database containers;
- MOBI6/KF7 text records;
- uncompressed, PalmDOC-compressed and HUFF/CDIC-compressed text;
- UTF-8 and Windows-1252 source text;
- EXTH title, author and cover metadata;
- embedded JPEG, PNG and BMP resources;
- generated sections, chapter navigation and persistent progress;
- explicit rejection of DRM-protected documents.

Pure KF8 reconstruction is a separate compatibility stage. A hybrid MOBI may
use its KF7 part while the KF8 reconstruction stage is not yet enabled. KFX and
fixed-layout Print Replica files are out of scope.

## Architecture

```text
EpubBook -----\
               ReflowBook -> Reflow reader activity -> HTML blocks
MobiBook -----/                                      -> DocumentLayout
                                                     -> TextRenderer
```

`ReflowBook` is a small format adapter. The reader does not depend on ZIP/PDB
details and only requests sections, TOC entries and resources.

`MobiBook` is split into these responsibilities:

1. Parse the PDB record table and record-zero PalmDOC/MOBI/EXTH headers.
2. Reject encrypted content before allocating large buffers.
3. Decode individual text records and build a compact uncompressed-offset map.
4. Split the monolithic MOBI HTML stream at explicit page breaks or bounded
   record-aligned positions.
5. Normalize legacy MOBI image/page-break markup before passing it to the
   existing HTML parser.
6. Extract embedded image records on demand to the reader cache.

The entire MOBI file is never loaded into memory. At runtime the large buffers
are limited to one decoded section plus the current page/image working set.

## Persistence

- Progress: `/sdcard/.reader/mobiprogress/<path-hash>.pos`
- Extracted images: `/sdcard/.reader/mobi_tmp/<path-hash>/`
- Parser metadata: `/sdcard/.reader/mobi_meta/<path-hash>.meta`

The metadata cache is invalidated by source file size and modification time.
It stores the record offset table, decoded text lengths, generated sections,
TOC labels and resource metadata, not a second copy of the whole book.

## Compatibility stages

1. Container, metadata, DRM detection, uncompressed/PalmDOC text.
2. Reader integration, images, cover, sections, progress and bookshelf.
3. INDX/NCX navigation refinement.
4. Pure KF8 skeleton/fragment reconstruction and AZW3 exposure.
