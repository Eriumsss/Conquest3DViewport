$base = "C:\Users\UserName\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Level Creation\level\levels\Isengard"

Write-Host "=== FX entries in objas.json ==="
$lines = Get-Content "$base\objas.json"
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '"key":\s*"(FX_|Fx_|fx_)') {
        $start = [Math]::Max(0, $i - 2)
        $end = [Math]::Min($lines.Count - 1, $i + 6)
        for ($j = $start; $j -le $end; $j++) {
            Write-Host ("{0}: {1}" -f ($j + 1), $lines[$j])
        }
        Write-Host "---"
    }
}

Write-Host ""
Write-Host "=== FX entries in pak_vals_a.json ==="
$lines2 = Get-Content "$base\pak_vals_a.json"
for ($i = 0; $i -lt $lines2.Count; $i++) {
    if ($lines2[$i] -match '"key":\s*"(FX_|Fx_|fx_)') {
        $start = [Math]::Max(0, $i - 2)
        $end = [Math]::Min($lines2.Count - 1, $i + 8)
        for ($j = $start; $j -le $end; $j++) {
            Write-Host ("{0}: {1}" -f ($j + 1), $lines2[$j])
        }
        Write-Host "---"
    }
}

Write-Host ""
Write-Host "=== All unique unk_0 values in obj0s.json ==="
$lines3 = Get-Content "$base\obj0s.json"
$values = @{}
for ($i = 0; $i -lt $lines3.Count; $i++) {
    if ($lines3[$i] -match '"unk_0":\s*(\d+)') {
        $val = $Matches[1]
        if (-not $values.ContainsKey($val)) {
            $values[$val] = 0
        }
        $values[$val]++
    }
}
foreach ($kv in $values.GetEnumerator() | Sort-Object Name) {
    Write-Host ("  unk_0={0}  count={1}" -f $kv.Name, $kv.Value)
}

Write-Host ""
Write-Host "=== pak_header.json texture/effect info ==="
$header = Get-Content "$base\pak_header.json"
foreach ($line in $header) {
    if ($line -match 'texture|effect|_num|block') {
        Write-Host $line.Trim()
    }
}

