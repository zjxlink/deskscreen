#!/bin/sh
# 把歌转成桌面信息屏能放的格式（macOS / Linux）。用法：
#   ./convert_music.sh 歌1.m4a 歌2.ape ...
# 转好的文件在这个脚本旁边的 music 文件夹里，把整个 music 文件夹拷到 TF 卡的根目录就行
# （也可以在网页设置的「显示 › 音乐」里上传）。同名的 .lrc 歌词会一起拷过去。
# 需要 ffmpeg：macOS 用 brew install ffmpeg，Ubuntu 用 sudo apt install ffmpeg
#
# 格式：MP3 192kbps、44.1kHz 立体声，保留歌名、歌手、专辑；封面缩到 300 像素以内、存成普通 JPG（板子上解得动）。
# 本来就是 MP3、FLAC、WAV 的不用转，直接拷；加密的（网易云 .ncm、QQ 音乐 .qmc、Apple Music）转不了

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "没有找到 ffmpeg。macOS 运行 brew install ffmpeg，Ubuntu 运行 sudo apt install ffmpeg，装好再来。"
  exit 1
fi
if [ $# -eq 0 ]; then
  echo "用法：$0 歌曲文件 [更多歌曲文件...]"
  exit 1
fi

out_dir="$(cd "$(dirname "$0")" && pwd)/music"
mkdir -p "$out_dir"
failed=0
for in in "$@"; do
  name=$(basename "$in")
  stem="${name%.*}"
  out="$out_dir/$stem.mp3"
  echo "正在转换：$name"
  if ffmpeg -hide_banner -loglevel error -stats -y -i "$in" -map 0:a:0 -map "0:v:0?" -map_metadata 0 \
      -c:a libmp3lame -b:a 192k -ar 44100 -ac 2 \
      -c:v mjpeg -q:v 4 -pix_fmt yuvj420p -vf "scale='min(300,iw)':-2" -disposition:v attached_pic \
      -id3v2_version 3 "$out"; then
    lrc="${in%.*}.lrc"
    [ -f "$lrc" ] && cp "$lrc" "$out_dir/$stem.lrc"
    echo "好了：$out"
  else
    echo "转换失败：$name"
    rm -f "$out"
    failed=1
  fi
done
echo "全部转完了。把 $out_dir 整个拷到 TF 卡的根目录，插回板子，打开「音乐」页就能看到。"
exit $failed
