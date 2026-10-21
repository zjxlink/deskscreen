#!/bin/sh
# 把电影转成桌面信息屏能放的格式（macOS / Linux）。用法：
#   ./convert_movie.sh 电影1.mp4 电影2.mkv ...
# 转好的文件在这个脚本旁边的 movies 文件夹里，把整个 movies 文件夹拷到 TF 卡的根目录就行。
# 需要 ffmpeg：macOS 用 brew install ffmpeg，Ubuntu 用 sudo apt install ffmpeg
#
# 格式：画面缩到 320x240 以内，每秒 20 帧，MJPEG；声音 16kHz 单声道，音量调匀一点（小喇叭上对白听得清）

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "没有找到 ffmpeg。macOS 运行 brew install ffmpeg，Ubuntu 运行 sudo apt install ffmpeg，装好再来。"
  exit 1
fi
if [ $# -eq 0 ]; then
  echo "用法：$0 电影文件 [更多电影文件...]"
  exit 1
fi

out_dir="$(cd "$(dirname "$0")" && pwd)/movies"
mkdir -p "$out_dir"
failed=0
for in in "$@"; do
  name=$(basename "$in")
  out="$out_dir/${name%.*}.avi"
  echo "正在转换：$name"
  if ffmpeg -hide_banner -loglevel error -stats -y -i "$in" -map 0:v:0 -map "0:a:0?" -sn \
      -vf "scale=320:240:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=20" \
      -c:v mjpeg -q:v 5 -pix_fmt yuvj420p \
      -af "aresample=16000,dynaudnorm=f=250:g=15,asetnsamples=n=800:p=0" -c:a pcm_s16le -ac 1 -ar 16000 \
      -max_interleave_delta 0 -f avi "$out"; then
    echo "好了：$out"
  else
    echo "转换失败：$name"
    rm -f "$out"
    failed=1
  fi
done
echo "全部转完了。把 $out_dir 整个拷到 TF 卡的根目录，插回板子，打开「影片」页就能看到。"
exit $failed
