Set-Location $PSScriptRoot
$result = cmd.exe /c "`"$PSScriptRoot\_build_now.bat`"" 2>&1
$result
