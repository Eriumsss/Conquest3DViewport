# Generate EventMappingData.cpp from the original event_mapping.h
$srcPath = "..\..\..\..\DLL\ConquestConsole-main\ConquestConsole-main\src\event_mapping.h"
$dstPath = "EventMappingData.cpp"

$lines = Get-Content $srcPath
$output = @()
$output += "// ============================================================================"
$output += "// EventMappingData.cpp - 2,817 event->bank mapping entries"
$output += "// Extracted from AudioHook DLL event_mapping.h (ConquestConsole)"
$output += "// C++03 compatible"
$output += "// ============================================================================"
$output += ""
$output += '#include "EventMapping.h"'
$output += ""
$output += "const EventMappingEntry g_EventMappingData[] = {"

# Lines 19-2836 in the original file (0-indexed: 18-2835) contain the data entries
for ($i = 19; $i -le 2835; $i++) {
    $line = $lines[$i]
    # Replace nullptr with 0 for C++03 compatibility
    $line = $line -replace "nullptr", "0"
    $output += $line
}

$output += "};"
$output += ""
$output += "const int g_EventMappingCount = 2817;"
$output += ""

$output | Out-File -FilePath $dstPath -Encoding ASCII
Write-Host ("Wrote {0} lines to {1}" -f $output.Count, $dstPath)

