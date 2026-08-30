# DeepSeek Chat Design

## Scope

The second home page opens a device-native DeepSeek chat client. It supports:

- creating a new local conversation;
- reopening and continuing conversations stored on the SD card;
- switching between `deepseek-v4-flash` and `deepseek-v4-pro`;
- enabling or disabling DeepSeek thinking mode per conversation;
- Chinese and English input through the shared `TextInputActivity`;
- fully powering Wi-Fi down when the DeepSeek activity exits.

DeepSeek's Chat Completions endpoint is stateless. The device owns the session
history and sends the retained context with every request.

## Components

1. `deepseek/DeepSeekConfig`
   - Stores the API key and default model in NVS.
   - Never writes the API key to logs or SD-card session files.
2. `deepseek/DeepSeekSessionStore`
   - Stores sessions below `/sdcard/.reader/deepseek`.
   - Uses a small `meta.tsv` plus append-only `messages.jsonl` per session.
   - Scans sessions by update time for the history screen.
3. `deepseek/DeepSeekClient`
   - Connects saved Wi-Fi, calls `POST https://api.deepseek.com/chat/completions`,
     parses SSE data, and powers Wi-Fi/C6 off after the request.
   - Uses `deepseek-v4-flash` or `deepseek-v4-pro` exactly as selected.
4. `DeepSeekActivity`
   - Session list: New Chat followed by recent sessions.
   - Chat view: title/model header, paged text transcript, and input action.
   - Network work runs outside the UI loop. The e-paper is updated once for
     the waiting state and once for the completed answer, not once per token.

## On-disk format

```text
/sdcard/.reader/deepseek/
  <session-id>/
    meta.tsv
    messages.jsonl
```

`meta.tsv` contains one escaped field per line: id, title, model, created RTC
timestamp, updated RTC timestamp. `messages.jsonl` contains one JSON object per line:

```json
{"role":"user","content":"Hello"}
{"role":"assistant","content":"Hello!"}
```

Writes are append-only for messages. Metadata is replaced through a temporary
file and rename so a reset cannot leave a partially written session index.

## Context and memory limits

- Retain the full transcript on SD.
- Build each API request from the newest complete turns that fit within a
  bounded request budget, initially 64 KiB.
- Bound a single generated reply to 2048 tokens in the first implementation.
- Stream the response parser so HTTP framing does not require a second full
  response buffer.
- A later enhancement may summarize older messages rather than omitting them.

## UI and refresh policy

- First entry and complete view changes use a full-screen text refresh.
- While waiting, show one `Thinking...` state.
- Accumulate SSE output without per-token panel refresh, then render the final
  answer once.
- Flash is the default model. The model button in the chat header toggles
  Flash/Pro and persists the selected model per conversation.
- The adjacent Think button toggles thinking mode per conversation. Only the
  final `content` is displayed and retained; `reasoning_content` is ignored.

## Failure handling

- Missing Wi-Fi: reuse the existing Wi-Fi setup guide.
- Missing API key: show a DeepSeek setup guide pointing to Settings.
- HTTP 401/402/429/5xx and malformed SSE: show a concise error without adding
  a fake assistant message to history.
- Back remains responsive during normal UI operation; shutdown always releases
  the HTTP client and fully powers down Wi-Fi/C6.

## Implementation order

1. NVS configuration and Settings UI.
2. SD-card session store.
3. Session list and chat transcript UI.
4. HTTPS/SSE client.
5. Home entry, Wi-Fi lifecycle, model switching, and failure validation.
