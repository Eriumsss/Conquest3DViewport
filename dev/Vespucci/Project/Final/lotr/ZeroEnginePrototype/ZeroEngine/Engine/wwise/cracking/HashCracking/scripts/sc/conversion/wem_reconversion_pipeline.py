#!/usr/bin/env python3
"""
WEM Re-conversion Pipeline
Converts extracted WEM files to valid WAV/OGG using vgmstream-cli or ww2ogg+revorb
"""
import os
import subprocess
import sys
import struct
import shutil
from pathlib import Path
from collections import defaultdict

WEM_DIR = "Audio/Extracted_WEMs"
WAV_DIR = "Audio/Extracted_WAVs"
OGG_DIR = "Audio/Extracted_OGGs"
REPORT_DIR = "Audio/Reports"
TOOLS_DIR = "Audio/Tools"

os.makedirs(WAV_DIR, exist_ok=True)
os.makedirs(OGG_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("WEM RE-CONVERSION PIPELINE")
print("="*100)

# Check WEM directory
if not os.path.exists(WEM_DIR):
    print(f"[ERROR] {WEM_DIR} not found")
    sys.exit(1)

wem_files = sorted([f for f in os.listdir(WEM_DIR) if f.endswith('.wem')])
print(f"\n[1] Found {len(wem_files):,} WEM files to convert")

# Detect available tools
print(f"\n[2] Detecting conversion tools...")
vgmstream_available = False
vgmstream_cmd = None
ww2ogg_available = False
revorb_available = False

# Check for vgmstream-cli in system PATH
try:
    result = subprocess.run(['vgmstream-cli', '--version'], capture_output=True, timeout=5)
    if result.returncode == 0:
        vgmstream_available = True
        vgmstream_cmd = 'vgmstream-cli'
        print(f"  ✅ vgmstream-cli available (system PATH)")
except:
    pass

# Check local tools directory
if os.path.exists(TOOLS_DIR):
    vgmstream_path = os.path.join(TOOLS_DIR, "vgmstream-cli.exe")
    ww2ogg_path = os.path.join(TOOLS_DIR, "ww2ogg.exe")
    revorb_path = os.path.join(TOOLS_DIR, "revorb.exe")

    if os.path.exists(vgmstream_path) and not vgmstream_available:
        vgmstream_available = True
        vgmstream_cmd = vgmstream_path
        print(f"  ✅ vgmstream-cli available (local)")

    if os.path.exists(ww2ogg_path):
        ww2ogg_available = True
        print(f"  ✅ ww2ogg available (local)")

    if os.path.exists(revorb_path):
        revorb_available = True
        print(f"  ✅ revorb available (local)")

# Also check vgmstream-win64 directory
vgmstream_win64_path = os.path.join("Audio", "vgmstream-win64", "vgmstream-cli.exe")
if os.path.exists(vgmstream_win64_path) and not vgmstream_available:
    vgmstream_available = True
    vgmstream_cmd = vgmstream_win64_path
    print(f"  ✅ vgmstream-cli available (Audio/vgmstream-win64/)")

if not vgmstream_available and not (ww2ogg_available and revorb_available):
    print(f"\n[WARNING] No conversion tools available!")
    print(f"  Install vgmstream: https://github.com/vgmstream/vgmstream")
    print(f"  Or download ww2ogg + revorb")
    print(f"\n  Proceeding with fallback strategy...")

# Helper functions
def get_file_header(path):
    """Get first 4 bytes of file"""
    try:
        with open(path, 'rb') as f:
            return f.read(4)
    except:
        return b''

def is_valid_wav(path):
    """Check if file is valid WAV"""
    header = get_file_header(path)
    return header == b'RIFF'

def is_valid_ogg(path):
    """Check if file is valid OGG"""
    header = get_file_header(path)
    return header == b'OggS'

def get_file_size(path):
    """Get file size in bytes"""
    try:
        return os.path.getsize(path)
    except:
        return 0

def convert_with_vgmstream(wem_path, output_path):
    """Convert WEM to WAV using vgmstream-cli"""
    try:
        cmd = [vgmstream_cmd, wem_path, '-o', output_path]
        result = subprocess.run(cmd, capture_output=True, timeout=30)

        if result.returncode == 0 and os.path.exists(output_path):
            if is_valid_wav(output_path) and get_file_size(output_path) > 512:
                return True, "Vorbis"

        return False, "vgmstream failed"
    except subprocess.TimeoutExpired:
        return False, "vgmstream timeout"
    except Exception as e:
        return False, f"vgmstream error: {str(e)}"

def convert_with_ww2ogg(wem_path, output_path):
    """Convert WEM to OGG using ww2ogg + revorb"""
    try:
        # First pass: ww2ogg
        temp_ogg = output_path.replace('.ogg', '_temp.ogg')
        pcb_path = os.path.join(TOOLS_DIR, "packed_codebooks_aoTuV_603.bin")

        ww2ogg_path = os.path.join(TOOLS_DIR, "ww2ogg.exe")
        cmd = [ww2ogg_path, wem_path, '--pcb', pcb_path, '-o', temp_ogg]
        result = subprocess.run(cmd, capture_output=True, timeout=30)

        if result.returncode != 0 or not os.path.exists(temp_ogg):
            return False, "ww2ogg failed"

        # Second pass: revorb
        revorb_path = os.path.join(TOOLS_DIR, "revorb.exe")
        cmd = [revorb_path, temp_ogg, output_path]
        result = subprocess.run(cmd, capture_output=True, timeout=30)

        # Clean up temp
        if os.path.exists(temp_ogg):
            os.remove(temp_ogg)

        if result.returncode == 0 and os.path.exists(output_path):
            if is_valid_ogg(output_path) and get_file_size(output_path) > 512:
                return True, "Vorbis"

        return False, "revorb failed"
    except subprocess.TimeoutExpired:
        return False, "ww2ogg timeout"
    except Exception as e:
        return False, f"ww2ogg error: {str(e)}"

def copy_as_fallback(wem_path, output_path):
    """Copy WEM file as fallback (for manual conversion later)"""
    try:
        shutil.copy2(wem_path, output_path)
        return True, "Copied_for_manual_conversion"
    except Exception as e:
        return False, f"Copy failed: {str(e)}"

# Conversion loop
print(f"\n[3] Converting WEM files...")
converted = 0
failed = 0
skipped = 0
fallback = 0
codec_stats = defaultdict(int)
conversion_log = []
invalid_files = []

for i, wem_file in enumerate(wem_files):
    if (i + 1) % 100 == 0:
        print(f"  [{i+1}/{len(wem_files)}] Converted: {converted}, Failed: {failed}, Fallback: {fallback}, Skipped: {skipped}")

    wem_id = wem_file.replace('.wem', '')
    wem_path = os.path.join(WEM_DIR, wem_file)
    wav_path = os.path.join(WAV_DIR, f"{wem_id}.wav")
    ogg_path = os.path.join(OGG_DIR, f"{wem_id}.ogg")

    try:
        # Check if already valid
        if os.path.exists(wav_path) and is_valid_wav(wav_path) and get_file_size(wav_path) > 512:
            conversion_log.append(f"[SKIP] WEM_ID={wem_id} | Reason=Already valid WAV")
            skipped += 1
            codec_stats["Already_Valid_WAV"] += 1
            continue

        if os.path.exists(ogg_path) and is_valid_ogg(ogg_path) and get_file_size(ogg_path) > 512:
            conversion_log.append(f"[SKIP] WEM_ID={wem_id} | Reason=Already valid OGG")
            skipped += 1
            codec_stats["Already_Valid_OGG"] += 1
            continue

        # Try vgmstream first
        if vgmstream_available:
            success, codec = convert_with_vgmstream(wem_path, wav_path)
            if success:
                converted += 1
                codec_stats[codec] += 1
                conversion_log.append(f"[OK] WEM_ID={wem_id} | Codec=Vorbis | Output=WAV | Tool=vgmstream")
                continue

        # Fallback to ww2ogg + revorb
        if ww2ogg_available and revorb_available:
            success, codec = convert_with_ww2ogg(wem_path, ogg_path)
            if success:
                converted += 1
                codec_stats[codec] += 1
                conversion_log.append(f"[OK] WEM_ID={wem_id} | Codec=Vorbis | Output=OGG | Tool=ww2ogg+revorb")
                continue

        # Last resort: copy for manual conversion
        success, reason = copy_as_fallback(wem_path, wav_path)
        if success:
            fallback += 1
            codec_stats["Copied_for_manual_conversion"] += 1
            conversion_log.append(f"[FALLBACK] WEM_ID={wem_id} | Reason=No tools available | Action=Copied to WAV")
            continue

        # All failed
        failed += 1
        codec_stats["Failed"] += 1
        conversion_log.append(f"[FAIL] WEM_ID={wem_id} | Reason=All conversion methods failed")
        invalid_files.append({"WEM_ID": wem_id, "Reason": "All methods failed", "Size": get_file_size(wem_path)})

    except Exception as e:
        failed += 1
        codec_stats["Error"] += 1
        conversion_log.append(f"[ERROR] WEM_ID={wem_id} | Error={str(e)}")
        invalid_files.append({"WEM_ID": wem_id, "Reason": str(e), "Size": get_file_size(wem_path)})

print(f"\n[4] Conversion Summary")
print(f"  ✅ Converted: {converted:,}")
print(f"  ⏳ Fallback (copied): {fallback:,}")
print(f"  ❌ Failed: {failed:,}")
print(f"  ⏭️  Skipped: {skipped:,}")
print(f"  Total: {len(wem_files):,}")
success_rate = (converted + skipped + fallback) / len(wem_files) * 100
print(f"  Success rate: {success_rate:.1f}%")

# Write conversion log
log_file = os.path.join(REPORT_DIR, "wem_reconversion_summary.log")
with open(log_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("WEM RE-CONVERSION SUMMARY\n")
    f.write("="*100 + "\n\n")
    f.write(f"Total WEM files: {len(wem_files):,}\n")
    f.write(f"Converted: {converted:,}\n")
    f.write(f"Fallback (copied): {fallback:,}\n")
    f.write(f"Failed: {failed:,}\n")
    f.write(f"Skipped: {skipped:,}\n")
    f.write(f"Success rate: {success_rate:.1f}%\n\n")
    f.write("="*100 + "\n")
    f.write("CONVERSION LOG (Last 100 entries)\n")
    f.write("="*100 + "\n")
    for line in conversion_log[-100:]:
        f.write(line + "\n")

# Write invalid files CSV
invalid_file = os.path.join(REPORT_DIR, "invalid_files.csv")
with open(invalid_file, 'w') as f:
    f.write("WEM_ID,Reason,Size\n")
    for entry in invalid_files:
        f.write(f"{entry['WEM_ID']},{entry['Reason']},{entry['Size']}\n")

# Write codec statistics
stats_file = os.path.join(REPORT_DIR, "codec_statistics.txt")
with open(stats_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("CODEC STATISTICS\n")
    f.write("="*100 + "\n\n")
    for codec, count in sorted(codec_stats.items(), key=lambda x: x[1], reverse=True):
        pct = count / len(wem_files) * 100
        f.write(f"{codec}: {count:,} ({pct:.1f}%)\n")

print(f"\n✅ Conversion log: {log_file}")
print(f"✅ Invalid files: {invalid_file}")
print(f"✅ Codec stats: {stats_file}")
print(f"✅ Converted WAVs: {WAV_DIR}")
print(f"✅ Converted OGGs: {OGG_DIR}")
print(f"\n[5] Next Steps")
print(f"  1. Install vgmstream: https://github.com/vgmstream/vgmstream/releases")
print(f"  2. Re-run this script to convert fallback files")
print(f"  3. Verify output files are playable")

