# S31 本地音乐时长

S31 外放后端通过 `audio_radio_get_file_timing()` 向现有音乐界面提供已播放时间与总时长。
已播放时间按输出 PCM 帧数 / BH1098G 时钟采样率计算；总时长独立读取源文件，
不会因 44.1 kHz 音源重采样到 16 kHz 而改变。

| 格式 | 总时长来源 |
| --- | --- |
| MP3 | 复用 `mp3_duration.c`：Xing/VBRI 优先，无时长标签时扫描编码帧 |
| FLAC | STREAMINFO 的总采样数和采样率，兼容前置 ID3v2 |
| WAV | RIFF 中的 fmt/data/fact 信息，处理附加 chunk 及奇数字节填充 |
| M4A | moov/trak/mdia 中音频轨道的 mdhd，支持版本 0/1 和扩展 atom 长度 |
| AAC | 扫描 ADTS 帧并累计原始数据块采样数，兼容前置 ID3v2 |
| OGG/Vorbis | 音频识别头的采样率与结束页 granule position |
| Opus | 48 kHz granule position 减去 pre-skip；顺序拼接的逻辑流累计时长 |

实现位于 `main/audio/file_duration.cpp`，仅接入 S31 应用构建。
文件损坏、元数据缺失或不支持的容器变体继续返回未知时长，不以文件大小和瞬时码率
猜测可变码率音频的总时长。AAC 此处支持 ADTS，不包含 ADIF/裸 AAC；M4A 此处读取
普通非分片文件音轨的 mdhd 时长，不解析分片 MP4 的完整时间线或 edit list。

## 后台任务

`s31_duration` 优先级 2、栈 4096 字节，播放开始后才执行元数据读取。
扫描使用独立 FILE 和 32 KiB PSRAM 窗口，不移动播放线程的文件位置。
任务按时间主动让出 CPU；切歌、停止、反初始化使 generation 失效，旧结果不会写回新歌曲。
待处理队列仅保存一首歌，新请求释放被替换的路径；扫描内存随文件处理结束释放。
任务与队列在后端生命周期内复用。

MP3 扫描排除尾部 ID3v1 和 APEv2；APEv2 的长度不包含可选的 32 字节头，
带头文件需要额外验证并排除该头。真实文件 `Maksim Mrvica - 克罗地亚狂想曲.mp3`
（8,494,265 字节）已在 PC 复现旧版本尾部误判，修复后完整计数 8129 帧，时长 212349 ms。
读取失败时日志会额外输出阶段、原因、首帧位置、已扫描帧数及读取次数。

日志形如：

```text
AUDIO_S31: duration metadata: applied=1 duration=301933 ms time=... ms path=...
```

## PC 验证

`tools/tests/file_duration_probe.cpp` 与 `file_duration.cpp`、`mp3_duration.c` 链接为本机程序，
执行 `python tools/tests/test_file_duration.py <probe.exe>`。
用例包括七种格式、MP3 有/无 Xing、MP4 音视频轨道区分及 64 位时长、Opus pre-skip/
串联、WAV chunk 填充、损坏/截断输入、取消和文件位置恢复。
这些测试检查解析逻辑；SD 并发读取和实际播放需在设备上验证。
