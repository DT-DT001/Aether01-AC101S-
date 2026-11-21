param(
    [Parameter(Mandatory=$true)][string]$InputFile,
    [Parameter(Mandatory=$true)][string]$OutputFile,
    [ValidateRange(1,30)][int]$Fps=15,
    [ValidateRange(16,480)][int]$Width=320,
    [ValidateRange(16,480)][int]$Height=240,
    [switch]$Force
)
$ErrorActionPreference='Stop'
$ffmpegCommand=Get-Command ffmpeg -ErrorAction SilentlyContinue
if(-not $ffmpegCommand) { throw '请在PC上自行准备FFmpeg并加入PATH。本脚本不会安装软件或更改ESP-IDF。' }
$sourcePath=(Resolve-Path -LiteralPath $InputFile).Path
$outputPath=[IO.Path]::GetFullPath($OutputFile)
if($sourcePath -eq $outputPath) { throw '输出文件必须与输入文件不同。' }
if([IO.Path]::GetExtension($outputPath) -ne '.mp4') { throw '输出扩展名必须是.mp4。' }
$overwrite=if($Force) {'-y'} else {'-n'}
$filter="scale=${Width}:${Height}:force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1"
& $ffmpegCommand.Source $overwrite -i $sourcePath -map '0:v:0' -map '0:a:0?' `
    -vf $filter -c:v libx264 -profile:v baseline -pix_fmt yuv420p -bf 0 -refs 1 `
    -preset fast -tune fastdecode -r $Fps -g $Fps -tag:v avc1 `
    -c:a aac -profile:a aac_low -ac 2 -ar 48000 -b:a 96k -movflags +faststart $outputPath
if($LASTEXITCODE -ne 0) { throw "FFmpeg转换失败（退出码 $LASTEXITCODE）。" }
