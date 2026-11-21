param(
    [string]$Clang = 'C:/Espressif/tools/esp-clang/esp-21.1.3_20260408/esp-clang/bin/clang.exe',
    [string]$Lld = 'C:/Espressif/tools/esp-clang/esp-21.1.3_20260408/esp-clang/bin/lld.exe',
    [string]$Node = 'C:/APP/Nodejs/node.exe'
)
$ErrorActionPreference = 'Stop'
$targets = (& $Clang --print-targets | Out-String)
if ($targets -notmatch '(?m)^\s*wasm32\s+-') {
    throw 'This Clang lacks the wasm32 backend. Use an existing full LLVM compiler with -Clang, or run sh tests/frames/run.sh in Linux/WSL with an installed C compiler. No tools are installed by this test.'
}
$framesRoot = $PSScriptRoot
$projectRoot = (Resolve-Path (Join-Path $framesRoot '../..')).Path
$outputRoot = Join-Path $projectRoot 'build/host_frames_tests'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$compileFlags = @('--target=wasm32', '-std=c11', '-O1', '-ffreestanding', '-fno-builtin', '-Wall', '-Wextra', '-Werror', '-I', (Join-Path $framesRoot 'stubs'), '-I', (Join-Path $projectRoot 'main'))
& $Clang @compileFlags -c (Join-Path $projectRoot 'main/app_frames.c') -o (Join-Path $outputRoot 'app_frames.o')
if ($LASTEXITCODE) { throw 'Production app_frames.c compilation failed' }
& $Clang @compileFlags -c (Join-Path $framesRoot 'frames_test.c') -o (Join-Path $outputRoot 'frames_test.o')
if ($LASTEXITCODE) { throw 'Test harness compilation failed' }
& $Lld -flavor wasm --no-entry --export=test_startup_rollback --export=test_copy_latest_front --export=test_disable_validation_contention --export=test_max_frame --export=test_metrics_and_repeated_handover (Join-Path $outputRoot 'app_frames.o') (Join-Path $outputRoot 'frames_test.o') -o (Join-Path $outputRoot 'frames_test.wasm')
if ($LASTEXITCODE) { throw 'Wasm link failed' }
& $Node (Join-Path $framesRoot 'run.mjs') (Join-Path $outputRoot 'frames_test.wasm')
if ($LASTEXITCODE) { throw 'Frame regression failed' }
