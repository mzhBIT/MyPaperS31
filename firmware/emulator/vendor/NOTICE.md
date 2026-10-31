# Sources and licenses

`crankboy/` is copied from the local Paperboy snapshot at
`E:/opensource/paperboy-main/main/crankboy_core`, on 2026-10-31.
The headers retain the upstream Peanut-GB / CrankBoy MIT copyright and license.
Peanut-GB credits Mahyar Koshkouei and the marked SameBoy code by Lior Halphon.
The core files are unchanged; S31 integration is in `gb_core.c`.

`minigb_apu/` is copied from the local Paperboy snapshot at
`E:/opensource/paperboy-main/main/minigb_apu`, on 2026-10-31.
The MIT LICENSE retains Alex Baines (2017) and Mahyar Koshkouei (2019).
The S31 port adds `minigb_apu_audio_render(ctx, stream, frames)` and passes the
frame count to the four synthesizers. The original 704-frame callback remains
available. The GB adapter accumulates clock fractions for 548/549 stereo frames
per GB frame instead of incorrectly using 704 as the video-frame audio length.

`VideoState.h` and the scan sequencing in `PaperDisplay.cpp` adapt the pair
state machine and double-buffered row streaming from Paperboy `msg/msg.c`:

Copyright 2026 Wenting Zhang

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

The temporary ROM browser renders into an offscreen LGFX sprite and uses
M5GFX's bundled efontCN bitmap font. It does not initialize an M5 display.
Its existing efont copyright is in
`components/M5GFX/src/lgfx/Fonts/efont/COPYRIGHT.txt`.
