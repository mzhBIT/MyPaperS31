Place the built-in reader bitmap font here as:

```text
reader.bin
```

The build embeds this file into the app binary with `target_add_binary_data`.
At runtime the default reader font path is:

```text
builtin:reader.bin
```

SD card fonts still work as user-selected overrides. If the saved SD font path
is missing, the reader falls back to this built-in font.
