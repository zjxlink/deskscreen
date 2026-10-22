@echo off
chcp 65001 >nul
setlocal
rem 把电影转成桌面信息屏能放的格式（Windows）。
rem 用法：把一个或几个电影文件拖到这个文件上，转好的在旁边的 movies 文件夹里，
rem 把整个 movies 文件夹拷到 TF 卡的根目录就行。
rem 需要 ffmpeg：把 ffmpeg.exe 放在这个文件旁边，或者装好（winget install Gyan.FFmpeg）。
rem 格式：画面缩到 320x240 以内，每秒 20 帧，MJPEG；声音 16kHz 单声道，音量调匀一点（小喇叭上对白听得清）

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
  echo 把电影文件拖到这个文件的图标上就开始转换，一次可以拖好几个。
  pause
  exit /b 1
)

set "OUT=%~dp0movies"
if not exist "%OUT%" mkdir "%OUT%"
:next
if "%~1"=="" goto done
echo.
echo 正在转换：%~nx1
"%FFMPEG%" -hide_banner -loglevel error -stats -y -i "%~1" -map 0:v:0 -map "0:a:0?" -sn -vf "scale=320:240:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=20" -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -af "aresample=16000,dynaudnorm=f=250:g=15,asetnsamples=n=800:p=0" -c:a pcm_s16le -ac 1 -ar 16000 -max_interleave_delta 0 -f avi "%OUT%\%~n1.avi"
if errorlevel 1 (
  echo 转换失败：%~nx1
  if exist "%OUT%\%~n1.avi" del "%OUT%\%~n1.avi"
) else (
  echo 好了：%OUT%\%~n1.avi
)
shift
goto next

:done
echo.
echo 全部转完了。把 %OUT% 整个拷到 TF 卡的根目录，插回板子，打开「影片」页就能看到。
pause
