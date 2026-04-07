$file = "C:\Users\UserName\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Level Creation\level\levels\Isengard\obj0s.json"
$lines = Get-Content $file
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Fx_FlameLoop_04"') {
        $start = [Math]::Max(0, $i - 5)
        $end = [Math]::Min($lines.Count - 1, $i + 25)
        for ($j = $start; $j -le $end; $j++) {
            Write-Host ("{0}: {1}" -f ($j + 1), $lines[$j])
        }
        Write-Host "---"
        break
    }
}

