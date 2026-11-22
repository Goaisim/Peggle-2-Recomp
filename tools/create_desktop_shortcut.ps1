param(
    [string]$TargetExe = "C:\Peggle2Recomp\peggle2\out\build\win-amd64-release\peggle2.exe",
    [string]$ShortcutName = "Peggle 2.lnk"
)

if (-not (Test-Path $TargetExe)) {
    throw "Target executable not found: $TargetExe"
}

$desktop = [Environment]::GetFolderPath("Desktop")
$shortcutPath = Join-Path $desktop $ShortcutName
$workingDir = Split-Path -Parent $TargetExe

$wsh = New-Object -ComObject WScript.Shell
$sc = $wsh.CreateShortcut($shortcutPath)
$sc.TargetPath = (Resolve-Path $TargetExe).Path
$sc.WorkingDirectory = $workingDir
$sc.IconLocation = "$($sc.TargetPath),0"
$sc.Description = "Peggle 2 (ReXGlue Static Recompilation)"
$sc.Save()

Write-Output "Created shortcut: $shortcutPath -> $($sc.TargetPath) (Icon: $($sc.IconLocation))"
