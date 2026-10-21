# minimp3

MP3 解码，来自 https://github.com/lieff/minimp3 （master，提交 ea99364），CC0 许可（见 LICENSE）。

改了一处：定义了 `MINIMP3_SCRATCH` 时，解码用的约 16KB 临时缓冲区不放在栈上，而是用这个宏给的地址
（播放器把它放在 PSRAM 里），播放任务的栈就不用开那么大。搜索 `MINIMP3_SCRATCH` 能找到改动。
