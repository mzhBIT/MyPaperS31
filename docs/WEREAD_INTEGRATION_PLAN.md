# WeRead integration plan

## Goal

Add an on-device WeChat Reading client without importing the PaperS3 UI or
display stack.  The implementation reuses MyPaperP4's Activity framework,
ESP-Hosted Wi-Fi lifecycle, SD storage, image decoders, reflow layout, reader
settings, page prefetch, and 2D EPD engine.

The protocol is an undocumented web protocol.  Every network operation must
therefore be cancellable, bounded by a deadline, cache-first, and able to show
a useful error when an endpoint or response schema changes.

## Module boundaries

| Module | Responsibility |
| --- | --- |
| `weread/WereadCrypto` | Book/chapter encoding, request signing, shard validation and decoding |
| `weread/WereadSession` | Persist `wr_vid`, `wr_skey`, and `wr_rt` in NVS |
| `weread/WereadHttpClient` | Cookie-aware HTTP, PSRAM response buffers, keep-alive, renewal |
| `weread/WereadApi` | Login, shelf, catalog, chapter and progress endpoints |
| `weread/WereadStore` | Atomic cache files under `/sdcard/.reader/weread` |
| `weread/WereadContentSource` | Present cached/downloaded chapters to the existing reflow reader |
| `activities/WereadActivity` | Login, shelf, detail and catalog UI |

## Storage

Authentication cookies are stored in the `weread` NVS namespace.  They are
never written to the SD card or logs.  Public metadata and decrypted reading
content are stored below `/sdcard/.reader/weread/`:

```text
shelf.bin
covers/
books/<book-id>/book.wrbook
books/<book-id>/toc.bin
books/<book-id>/chapters/<chapter-uid>.xhtml
books/<book-id>/images/
books/<book-id>/progress.bin
```

Cache files use a versioned binary header and temporary-file/rename updates so
power loss cannot turn a partial download into a valid cache hit.

## Network and power lifecycle

1. Power the C6 and connect only for login, synchronization, or cache misses.
2. Reuse one TLS session for a bounded batch of requests.
3. Download the requested chapter and opportunistically cache the next one.
4. Close HTTP handles, deinitialize Hosted Wi-Fi, and power the C6 off before
   returning to sustained offline reading.
5. Queue cloud progress locally and upload it on the next network session.

No network worker may overlap a memory-heavy EPD quality-frame producer unless
there is enough internal RAM to create the producer task.

## Delivery order

1. Protocol algorithms and deterministic self-tests.
2. NVS session, HTTP transport, renewal, asynchronous QR login.
3. Shelf, cover cache, book details, and catalog.
4. Chapter cache and a reflow content-source adapter.
5. Images, next-chapter prefetch, offline mode, progress synchronization.
6. Failure injection, memory logging, build verification, and device tests.

## Acceptance checks

- A failed or cancelled request never blocks touch handling for an entire HTTP
  timeout chain.
- Cookie values are redacted from logs.
- Cached chapters open with C6 powered off.
- XHTML uses the same line breaking, typography, background, image-quality
  refresh, and reader overlay as local EPUB/MOBI books.
- Login expiration either renews once or returns to the QR page without retry
  loops.

## Device verification sequence

1. With no saved WeRead session, open the second home page and scan the QR
   code. Confirm that the shelf appears only after Wi-Fi has been powered off.
2. Open a book, select a chapter, and confirm `book.wrbook`, `toc.bin`, and a
   chapter payload are created below the book cache directory.
3. Turn the access point off, reopen the same chapter, and verify that the C6
   remains powered down and the cached chapter still opens.
4. Turn past the final page of a chapter. Confirm the next chapter is fetched
   only after the EPD update is idle, then Wi-Fi is shut down again.
5. Test one TXT title, one normal XHTML title, and one title whose content
   endpoint returns a complete EPUB package. Test a chapter containing both a
   baseline/progressive JPEG and PNG image.
6. Exit after several page turns, reconnect, and check for the
   `progress server-confirmed` log and cloud reading-time readback. An accepted
   but unverified report remains queued. See `WEREAD_PROGRESS_SYNC.md`.
   Cookie values must never appear in logs.
7. Let `wr_skey` expire while retaining `wr_rt`; verify one renewal attempt.
   If renewal is rejected, the next shelf load must show a fresh QR login.
