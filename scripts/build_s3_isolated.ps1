[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
# Native 8.3 paths stay on C:. SUBST mixed with Python-resolved C: paths breaks
# ESP-IDF's os.path.relpath while building components.
$taskShortRoot = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($taskRoot).ShortPath
$taskEnvironment = 'waveshare_ESP32_S3_RS485_CAN'
$taskPython = Join-Path $taskShortRoot '_tools\python\Scripts\python.exe'
$taskVariables = @('PLATFORMIO_CORE_DIR','PLATFORMIO_PACKAGES_DIR','PLATFORMIO_PLATFORMS_DIR',
    'PLATFORMIO_BUILD_DIR','PLATFORMIO_LIBDEPS_DIR','PLATFORMIO_SETTING_ENABLE_TELEMETRY','PYTHONPATH')
$savedEnvironment = @{}
foreach ($key in $taskVariables) { $savedEnvironment[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
try {
    if (-not (Test-Path -LiteralPath $taskPython)) { throw 'Missing project-local Python runtime' }
    & $taskPython (Join-Path $taskRoot 'scripts\patch_isolated_idf_builder.py')
    if ($LASTEXITCODE -ne 0) { throw 'Could not prepare isolated Windows builder fixes' }
    $env:PLATFORMIO_CORE_DIR = Join-Path $taskShortRoot '_tools\pio-core'
    $env:PLATFORMIO_PACKAGES_DIR = Join-Path $env:PLATFORMIO_CORE_DIR 'packages'
    $env:PLATFORMIO_PLATFORMS_DIR = Join-Path $env:PLATFORMIO_CORE_DIR 'platforms'
    $env:PLATFORMIO_BUILD_DIR = Join-Path $taskShortRoot '.pio\build-s3-rx'
    $env:PLATFORMIO_LIBDEPS_DIR = Join-Path $taskShortRoot '.pio\libdeps'
    $env:PLATFORMIO_SETTING_ENABLE_TELEMETRY = 'No'
    $env:PYTHONPATH = Join-Path $taskShortRoot '_tools\python\Lib\site-packages'
    $taskEvidence = Join-Path $taskRoot '.tests\build'
    New-Item -ItemType Directory -Path $taskEvidence -Force | Out-Null
    & $taskPython -m platformio run -d $taskShortRoot -e $taskEnvironment 2>&1 |
        Tee-Object -FilePath (Join-Path $taskEvidence 'waveshare-s3.log')
    $taskExit = $LASTEXITCODE
    if ($taskExit -ne 0) { throw "S3 build failed: $taskExit" }
    $artifactRoot = Join-Path $taskRoot ".pio\build-s3-rx\$taskEnvironment"
    $artifacts = foreach ($name in @('firmware.bin','firmware.elf','bootloader.bin','partitions.bin')) {
        $path = Join-Path $artifactRoot $name
        [ordered]@{name=$name;size=(Get-Item -LiteralPath $path).Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    [ordered]@{environment=$taskEnvironment;build_exit=$taskExit;evidence='compile_and_link';artifacts=@($artifacts)} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskEvidence 'artifacts.json') -Encoding utf8
} finally {
    foreach ($key in $taskVariables) { [Environment]::SetEnvironmentVariable($key,$savedEnvironment[$key],'Process') }
}
