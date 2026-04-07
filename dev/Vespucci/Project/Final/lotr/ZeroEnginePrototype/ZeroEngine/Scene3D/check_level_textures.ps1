$texDir = "C:\Users\UserName\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Level Creation\level\levels\Isengard\textures"

$allDds = Get-ChildItem "$texDir\*.dds" -ErrorAction SilentlyContinue
Write-Host "Total DDS in level textures: $($allDds.Count)"

$fxDds = $allDds | Where-Object { $_.Name -match '^(FX_|Fx_|fx_)' }
Write-Host "FX DDS in level textures: $($fxDds.Count)"

if ($fxDds.Count -gt 0) {
    Write-Host "`nFX DDS files found:"
    $fxDds | Select-Object -First 30 Name, Length | Format-Table -AutoSize
} else {
    Write-Host "`nNo FX DDS files found in level textures."
}

Write-Host "`nSample of level texture names (first 15):"
$allDds | Select-Object -First 15 Name, Length | Format-Table -AutoSize

Write-Host "`nSample of level texture JSON metadata:"
$fxJsons = Get-ChildItem "$texDir\*.json" -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(FX_|Fx_|fx_)' }
Write-Host "FX JSON metadata files: $($fxJsons.Count)"
if ($fxJsons.Count -gt 0) {
    Write-Host "First 5 FX JSON files:"
    $fxJsons | Select-Object -First 5 Name | ForEach-Object { Write-Host "  $($_.Name)" }
    Write-Host "`nContent of first FX JSON:"
    Get-Content $fxJsons[0].FullName
}

# Check the texture JSON for the one FX texture we know exists
$lightningJson = Get-ChildItem "$texDir\fx_lightning_test.json" -ErrorAction SilentlyContinue
if ($lightningJson) {
    Write-Host "`nfx_lightning_test.json in level textures:"
    Get-Content $lightningJson.FullName
}

# Check obj0s.json for texture entries with unk_0=101 - get first 3 full entries
Write-Host "`n=== First 3 obj0s.json entries with unk_0=101 (FX textures) ==="
$obj0s = "C:\Users\UserName\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Level Creation\level\levels\Isengard\obj0s.json"
$lines = Get-Content $obj0s
$count = 0
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '"unk_0":\s*101') {
        $start = [Math]::Max(0, $i - 1)
        $end = [Math]::Min($lines.Count - 1, $i + 3)
        for ($j = $start; $j -le $end; $j++) {
            Write-Host ("{0}: {1}" -f ($j + 1), $lines[$j])
        }
        Write-Host "---"
        $count++
        if ($count -ge 3) { break }
    }
}

