# WEM Batch Conversion Script (PowerShell)
# Converts all WEM files to WAV using vgmstream-cli

$WEM_DIR = "Audio\Extracted_WEMs"
$WAV_DIR = "Audio\Extracted_WAVs"
$VGMSTREAM = "Audio\vgmstream-win64\vgmstream-cli.exe"
$REPORT_DIR = "Audio\Reports"

Write-Host "====================================================================================================`n"
Write-Host "WEM BATCH CONVERSION (PowerShell)`n"
Write-Host "====================================================================================================" -ForegroundColor Green

# Check vgmstream
if (-not (Test-Path $VGMSTREAM)) {
    Write-Host "[ERROR] vgmstream-cli not found at $VGMSTREAM" -ForegroundColor Red
    exit 1
}

Write-Host "[1] vgmstream-cli found: $VGMSTREAM" -ForegroundColor Green

# Get WEM files
$wem_files = Get-ChildItem -Path $WEM_DIR -Filter "*.wem" | Sort-Object Name
Write-Host "[2] Found $($wem_files.Count) WEM files to convert`n"

# Conversion loop
$converted = 0
$failed = 0
$log_lines = @()

for ($i = 0; $i -lt $wem_files.Count; $i++) {
    $wem_file = $wem_files[$i]
    $wem_id = $wem_file.BaseName
    $wem_path = $wem_file.FullName
    $wav_path = Join-Path $WAV_DIR "$wem_id.wav"
    
    if (($i + 1) % 100 -eq 0) {
        Write-Host "[3] Converting... [$($i+1)/$($wem_files.Count)] Converted: $converted, Failed: $failed"
    }
    
    try {
        # Run vgmstream-cli
        $output = & $VGMSTREAM $wem_path -o $wav_path 2>&1
        
        # Check if output file was created
        if (Test-Path $wav_path) {
            $file_size = (Get-Item $wav_path).Length
            if ($file_size -gt 512) {
                # Check RIFF header
                $header = Get-Content -Path $wav_path -Encoding Byte -TotalCount 4
                if ($header[0] -eq 0x52 -and $header[1] -eq 0x49 -and $header[2] -eq 0x46 -and $header[3] -eq 0x46) {
                    $converted++
                    $log_lines += "[OK] WEM_ID=$wem_id | Size=$file_size | Output=WAV"
                } else {
                    $failed++
                    $log_lines += "[FAIL] WEM_ID=$wem_id | Reason=Invalid RIFF header"
                }
            } else {
                $failed++
                $log_lines += "[FAIL] WEM_ID=$wem_id | Reason=File too small ($file_size bytes)"
            }
        } else {
            $failed++
            $log_lines += "[FAIL] WEM_ID=$wem_id | Reason=Output file not created"
        }
    } catch {
        $failed++
        $log_lines += "[ERROR] WEM_ID=$wem_id | Error=$($_.Exception.Message)"
    }
}

Write-Host "`n[4] Conversion Summary" -ForegroundColor Green
Write-Host "  ✅ Converted: $converted"
Write-Host "  ❌ Failed: $failed"
Write-Host "  Total: $($wem_files.Count)"
$success_rate = if ($wem_files.Count -gt 0) { ($converted / $wem_files.Count * 100) } else { 0 }
Write-Host "  Success rate: $([Math]::Round($success_rate, 1))%"

# Write log file
$log_file = Join-Path $REPORT_DIR "wem_batch_conversion.log"
$log_lines | Out-File -FilePath $log_file -Encoding UTF8

Write-Host "`n✅ Conversion log: $log_file" -ForegroundColor Green
Write-Host "✅ Converted WAVs: $WAV_DIR" -ForegroundColor Green

