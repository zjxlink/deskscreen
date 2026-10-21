@echo off
chcp 65001 >nul
setlocal
rem 把歌转成桌面信息屏能放的格式（Windows）。
rem 用法：把一首或几首歌拖到这个文件上，转好的在旁边的 music 文件夹里，
rem 把整个 music 文件夹拷到 TF 卡的根目录就行（也可以在网页设置的「音乐管理」里上传）。同名的 .lrc 歌词会一起拷过去。
rem 需要 ffmpeg：把 ffmpeg.exe 放在这个文件旁边，或者装好（winget install Gyan.FFmpeg）。
rem 格式：MP3 192kbps、44.1kHz 立体声，保留歌名、歌手、专辑；封面缩到 300 像素以内、存成普通 JPG（板子上解得动）。
rem 本来就是 MP3、FLAC、WAV 的不用转，直接拷；加密的（网易云 .ncm、QQ 音乐 .qmc、Apple Music）转不了

set "FFMPEG=ffmpeg"
if exist "%~dp0ffmpeg.exe" set "FFMPEG=%~dp0ffmpeg.exe"
"%FFMPEG%" -version >nul 2>nul
if errorlevel 1 (
  echo 没有找到 ffmpeg。
  echo 方法一：在命令行运行 winget install Gyan.FFmpeg ，装好后重新拖一次。
  echo 方法二：从 https://www.gyan.dev/ffmpeg/builds/ 下载 ffmpeg，把里面 bin 文件夹的 ffmpeg.exe 放到这个文件旁边。
  pause
  exit /b 1
)
if "%~1"=="" (
  echo 把歌曲文件拖到这个文件的图标上就开始转换，一次可以拖好几个。
  pause
  exit /b 1
)

set "OUT=%~dp0music"
if not exist "%OUT%" mkdir "%OUT%"
:next
if "%~1"=="" goto done
echo.
echo 正在转换：%~nx1
"%FFMPEG%" -hide_banner -loglevel error -stats -y -i "%~1" -map 0:a:0 -map "0:v:0?" -map_metadata 0 -c:a libmp3lame -b:a 192k -ar 44100 -ac 2 -c:v mjpeg -q:v 4 -pix_fmt yuvj420p -vf "scale='min(300,iw)':-2" -disposition:v attached_pic -id3v2_version 3 "%OUT%\%~n1.mp3"
if errorlevel 1 (
  echo 转换失败：%~nx1
  if exist "%OUT%\%~n1.mp3" del "%OUT%\%~n1.mp3"
) else (
  if exist "%~dpn1.lrc" copy /y "%~dpn1.lrc" "%OUT%\%~n1.lrc" >nul
  echo 好了：%OUT%\%~n1.mp3
)
shift
goto next

:done
echo.
echo 全部转完了。把 %OUT% 整个拷到 TF 卡的根目录，插回板子，打开「音乐」页就能看到。
pause
