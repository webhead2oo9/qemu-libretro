<#
run-fixtures.ps1 — assemble and run the mesapt legacy-arbitration boot
fixtures against a built core, comparing debug-console output to the
golden strings documented in each fixture's header.

Fixtures (see the .S headers for the letter meanings):
  revorder_boot  golden LSBO2RFAT  reverse-launch-order refusal
  fwdorder_boot  golden LBRD1WT    forward refusal + crash succession
  smpstress_boot golden LICWPFT    concurrent two-vCPU doorbell stress,
                                   run under tcg,thread=multi AND whpx

Requires:
  - WSL with GNU as/ld (binutils) for the boot-sector build
  - a built libqemu_libretro.dll and retro_host.exe in the build dir
  - RETRO_HOST_SYSTEM_DIR pointing at a libretro system directory whose
    qemu/ subdirectory carries the firmware (pc-bios contents)

Usage:
  powershell -ExecutionPolicy Bypass -File run-fixtures.ps1 `
    [-BuildDir <path>] [-WorkDir <path>]
#>
param(
    [string]$BuildDir,
    [string]$WorkDir
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-xpdriver-win64' }
if (-not $WorkDir) {
    $WorkDir = Join-Path $repo 'scratchpad\fixture-runs'
}
$core = Join-Path $BuildDir 'libqemu_libretro.dll'
$hostExe = Join-Path $BuildDir 'retro_host.exe'
if (-not (Test-Path -LiteralPath $core)) { throw "core not found: $core" }
if (-not (Test-Path -LiteralPath $hostExe)) {
    throw "retro_host not found: $hostExe"
}
if (-not $env:RETRO_HOST_SYSTEM_DIR) {
    throw 'RETRO_HOST_SYSTEM_DIR must point at a libretro system dir ' +
          'with qemu firmware (e.g. <RetroArch>\system)'
}

$defaultAccel = '-accel tcg,thread=single'
$fixtures = @(
    @{ Name = 'revorder'; Src = 'revorder'; Port = 23531;
       Golden = 'LSBO2RFAT'; Wait = 0 },
    @{ Name = 'fwdorder'; Src = 'fwdorder'; Port = 23532;
       Golden = 'LBRD1WT'; Wait = 9000 },
    @{ Name = 'smpstress-tcg'; Src = 'smpstress'; Port = 23533;
       Golden = 'LICWPFT'; Wait = 20000;
       Accel = '-accel tcg,thread=multi' },
    @{ Name = 'smpstress-whpx'; Src = 'smpstress'; Port = 23534;
       Golden = 'LICWPFT'; Wait = 20000;
       Accel = '-accel whpx,ssd=off -accel tcg' }
)

$failures = 0
foreach ($f in $fixtures) {
    $name = $f.Name
    $src = $f.Src
    $accel = $defaultAccel
    if ($f.Accel) { $accel = $f.Accel }
    $dir = Join-Path $WorkDir $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $wslDir = (wsl -e wslpath -a ($dir -replace '\\', '/')).Trim()
    if ($LASTEXITCODE -ne 0) { throw "wslpath failed for $dir" }
    wsl --cd $repo -e sh -c ("as --32 -o '$wslDir/${name}_boot.o' " +
        "tests/retro-host/fixtures/${src}_boot.S && " +
        "ld -m elf_i386 -Ttext 0x7c00 -e _start --oformat binary " +
        "-o '$wslDir/${name}_boot.bin' '$wslDir/${name}_boot.o'")
    if ($LASTEXITCODE -ne 0) { throw "boot-sector build failed: $name" }

    $debugLog = Join-Path $dir "$name-debug.log"
    if (Test-Path -LiteralPath $debugLog) {
        Remove-Item -LiteralPath $debugLog -Force -Confirm:$false
    }
    $cmdLine = "qemu-system-x86_64 $accel -machine pc " +
        "-m 64M -smp 2 -vga cirrus -nic none " +
        "-drive file=${name}_boot.bin,format=raw,if=floppy -boot a " +
        "-qmp tcp:127.0.0.1:$($f.Port),server=on,wait=off " +
        "-debugcon file:$name-debug.log -global isa-debugcon.iobase=0xe9 " +
        "-no-reboot"
    # LF only: the core's argv splitter treats a CR as part of the last
    # option and rejects the command line.
    [IO.File]::WriteAllText((Join-Path $dir "$name.qemu_cmd_line"),
        $cmdLine + "`n")

    $args = @("`"$core`"", "$name.qemu_cmd_line", $f.Port)
    if ($f.Wait -gt 0) { $args += @('--wait', $f.Wait) }
    $proc = Start-Process -FilePath $hostExe -ArgumentList $args `
        -WorkingDirectory $dir -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $dir 'host.stdout.log') `
        -RedirectStandardError (Join-Path $dir 'host.stderr.log')
    # Cache the process handle immediately: Windows PowerShell cannot
    # read ExitCode from a -PassThru process whose handle was first
    # touched after exit.
    $null = $proc.Handle
    if (-not $proc.WaitForExit(180000)) {
        Stop-Process -Id $proc.Id -Force -Confirm:$false
        throw "retro_host timed out: $name"
    }
    $proc.WaitForExit()

    $letters = ''
    if (Test-Path -LiteralPath $debugLog) {
        $letters = (Get-Content -LiteralPath $debugLog -Raw).Trim()
    }
    if ($letters -eq $f.Golden -and $proc.ExitCode -eq 0) {
        Write-Host "PASS $name  $letters"
    } else {
        Write-Host ("FAIL $name  got '$letters' want '$($f.Golden)' " +
            "exit $($proc.ExitCode)  (logs: $dir)")
        $failures++
    }
}

if ($failures -gt 0) { exit 1 }
Write-Host 'All arbitration fixtures passed.'
exit 0
