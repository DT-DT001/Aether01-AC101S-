# 视频播放与格式说明

MP4是容器名称，相同的`.mp4`扩展名可能装着不同编码。当前ESP32-S31使用项目已安装的乐鑫`esp_h264`软件解码器，视频解码范围为H.264 Constrained Baseline；Main、High及HEVC不能直接播放。播放器会显示文件的实际编码参数和失败阶段。

当前MP4还要求：普通非分片容器、I/P帧、CAVLC、逐行YUV420、编码尺寸不超过480×480、源帧率不超过30fps；音轨为AAC-LC单/双声道。以上是可接受格式范围，实际播放速度仍受分辨率、编码复杂度、音频和显示负载影响。

需要转换时，将TF卡中的视频复制到电脑，再使用现有的[转换脚本](../../tools/convert-video.ps1)：

```powershell
./tools/convert-video.ps1 -InputFile C:/Media/input.mp4 -OutputFile C:/Media/input.s31.mp4
```

脚本需要电脑已有FFmpeg，不会安装或更换ESP-IDF。默认输出320×240以内、15fps的H.264 Constrained Baseline＋AAC-LC MP4，保留比例和原文件；输出文件已存在时默认拒绝覆盖。将生成的`input.s31.mp4`复制到TF卡`Video`目录后，从视频应用或文件页打开。设备上没有原文件转码功能。

若在相同条件下仍明显卡顿，可以显式降低尺寸和帧率，例如：

```powershell
./tools/convert-video.ps1 -InputFile C:/Media/input.mp4 -OutputFile C:/Media/input.small.s31.mp4 -Width 160 -Height 120 -Fps 10
```

另一条播放路径是硬件MJPEG：原始`.mjpg/.mjpeg`或普通MJPEG AVI，使用baseline JPEG，帧宽高需为16的倍数且不超过480×480，单帧压缩数据不超过512KiB。此路径当前不播放AVI音轨。

播放失败后，新版会腾出视频显示区域完整展示原因；“重试”会重新执行媒体准备，返回、停止按钮仍可使用。串口仅按每次播放输出开始、格式及最终结果，不逐帧刷错误日志。
