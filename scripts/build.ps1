<#
.SYNOPSIS
  Compile (and optionally upload) a stream deck sketch with arduino-cli.

.EXAMPLE
  .\scripts\build.ps1 firmware\bringup\01_chipinfo
  .\scripts\build.ps1 firmware\bringup\01_chipinfo -Port COM5          # compile + upload via "UART" port
  .\scripts\build.ps1 firmware\streamdeck -Port COM9 -Usb              # upload via native "USB" port
  .\scripts\build.ps1 firmware\bringup\03_keys_leds -Define DECK_NUM_LEDS=10
  .\scripts\build.ps1 -ListPorts

.NOTES
  Uses arduino-cli from PATH, else the copy bundled with Arduino IDE 2.

  The DevKitC-1 has two USB-C ports:
    UART : USB-serial bridge -> UART0. Upload always works here (auto-reset).
    USB  : native USB (GPIO19/20). Our sketches run TinyUSB on it (HID + CDC
           console). Uploading through it needs -Usb (1200-baud touch reset);
           if the running sketch is crashed, hold BOOT, tap RST, then upload.
  The console (Serial) is always on the native USB port (CDCOnBoot=cdc).

  Partition scheme: sketch folder has partitions.csv -> custom, else the stock
  8 MB "3MB APP / 1.5MB SPIFFS" layout (fine for bring-up sketches).
#>
param(
  [Parameter(Position = 0)] [string] $Sketch,
  [string]   $Port,
  [switch]   $Usb,                     # upload through native USB instead of the UART bridge
  [string]   $LogLevel = 'none',       # core debug level: none|error|warn|info|debug|verbose
  [string[]] $Define = @(),            # extra -D flags, e.g. DECK_NUM_LEDS=10
  [switch]   $ListPorts
)
$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent $PSScriptRoot
$Libs = Join-Path $Repo 'firmware\libraries'

$Cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $Cli) {
  $Cli = Join-Path $env:ProgramFiles 'Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
  if (-not (Test-Path $Cli)) { throw 'arduino-cli not found (PATH or Arduino IDE 2 bundle).' }
}

if ($ListPorts) { & $Cli board list; exit $LASTEXITCODE }
if (-not $Sketch) { throw 'Pass a sketch folder, e.g. firmware\bringup\01_chipinfo' }
$SketchPath = (Resolve-Path $Sketch).Path
if ((Split-Path -Leaf $SketchPath) -eq 'streamdeck') { & (Join-Path $PSScriptRoot 'embed_webui.ps1') }

$Partition  = if (Test-Path (Join-Path $SketchPath 'partitions.csv')) { 'custom' } else { 'default_8MB' }
$UploadMode = if ($Usb) { 'cdc' } else { 'default' }

# ESP32-S3 N8R2 DevKitC-1 clone (esptool: QFN56 rev v0.2, embedded 2 MB quad PSRAM,
# 8 MB GigaDevice flash). NOT the N16R8 the build guide lists. TinyUSB on native USB.
$Fqbn = 'esp32:esp32:esp32s3:' + (@(
  'USBMode=default',          # USB-OTG (TinyUSB): HID + CDC composite
  'CDCOnBoot=cdc',            # Serial = USB CDC
  "UploadMode=$UploadMode",
  'CPUFreq=240',
  'FlashMode=qio',
  'FlashSize=8M',
  'PSRAM=enabled',            # quad (QSPI) PSRAM

  "PartitionScheme=$Partition",
  "DebugLevel=$LogLevel",
  'LoopCore=1',
  'EventsCore=1'
) -join ',')

$CliArgs = @('compile', '--fqbn', $Fqbn, '--libraries', $Libs, '--warnings', 'default')
if ($Define.Count -gt 0) {
  $flags = ($Define | ForEach-Object { "-D$_" }) -join ' '
  $CliArgs += @('--build-property', "compiler.cpp.extra_flags=$flags", '--build-property', "compiler.c.extra_flags=$flags")
}
if ($Port) { $CliArgs += @('--upload', '--port', $Port) }
$CliArgs += $SketchPath

Write-Host "arduino-cli $($CliArgs -join ' ')" -ForegroundColor DarkGray
& $Cli @CliArgs
exit $LASTEXITCODE
