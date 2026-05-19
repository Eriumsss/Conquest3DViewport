"""
Collision Repack Pipeline
Called by the viewer when "Save to PAK" is clicked for collision.

Usage: python collision_repack.py <pak_path> <collision_json> <model_name> <lotrc_path> [output_pak]

Steps:
  1. Parse PAK with lotrc_rs.exe → temp directory
  2. Find model JSON by name
  3. Patch shapes section with collision data
  4. Repack with lotrc_rs.exe → output PAK
  5. Write progress to collision_progress.txt for the viewer to read

Progress format (written to collision_progress.txt):
  STAGE:<percent>:<message>
  DONE:<output_path>
  ERROR:<message>
"""
import sys, os, json, subprocess, shutil, tempfile, time

def write_progress(progress_file, stage, percent, message):
    with open(progress_file, 'w') as f:
        f.write(f"STAGE:{percent}:{message}\n")
    print(f"[{percent:3d}%] {message}")

def write_done(progress_file, output_path):
    with open(progress_file, 'w') as f:
        f.write(f"DONE:{output_path}\n")
    print(f"[DONE] {output_path}")

def write_error(progress_file, message):
    with open(progress_file, 'w') as f:
        f.write(f"ERROR:{message}\n")
    print(f"[ERROR] {message}")

def compute_lotr_hash(name):
    """Compute LotrHashString CRC32 — same as the game's hash function."""
    h = 0
    for ch in name.lower():
        h = ((h >> 8) & 0x00FFFFFF) ^ CRC_TABLE[(h ^ ord(ch)) & 0xFF]
    return h

# CRC32 table (same as LotrHashString in the engine)
CRC_TABLE = [0] * 256
def _init_crc():
    for i in range(256):
        c = i
        for _ in range(8):
            if c & 1:
                c = 0xEDB88320 ^ (c >> 1)
            else:
                c >>= 1
        CRC_TABLE[i] = c
_init_crc()

def main():
    if len(sys.argv) < 5:
        print("Usage: collision_repack.py <pak_path> <collision_json> <model_name> <lotrc_path> [output_pak]")
        sys.exit(1)

    pak_path = sys.argv[1]
    collision_json_path = sys.argv[2]
    model_name = sys.argv[3]
    lotrc_path = sys.argv[4]
    output_pak = sys.argv[5] if len(sys.argv) > 5 else pak_path

    # Progress file next to the script
    script_dir = os.path.dirname(os.path.abspath(__file__))
    progress_file = os.path.join(script_dir, "collision_progress.txt")

    # Temp directory for parsed data
    temp_dir = os.path.join(script_dir, "_collision_temp")

    try:
        # ============================================================
        # Step 1: Parse PAK with lotrc_rs.exe
        # ============================================================
        write_progress(progress_file, "PARSE", 5, "Parsing PAK file...")

        # Clean temp dir
        if os.path.exists(temp_dir):
            shutil.rmtree(temp_dir)
        os.makedirs(temp_dir, exist_ok=True)

        # Find the BIN file (same name, .BIN extension)
        pak_dir = os.path.dirname(pak_path)
        pak_base = os.path.splitext(os.path.basename(pak_path))[0]

        write_progress(progress_file, "PARSE", 10, f"Running lotrc_rs.exe parse on {pak_base}...")

        # lotrc_rs.exe <PAK_path> -o <output_dir>
        # Parses PAK+BIN and outputs a folder with pak_header.json, models/, etc.
        result = subprocess.run(
            [lotrc_path, pak_path, "-o", temp_dir],
            capture_output=True, text=True, timeout=120
        )

        write_progress(progress_file, "PARSE", 30, "PAK parsed")

        # Find the parsed output directory (named after the level)
        parsed_dir = None
        for d in os.listdir(temp_dir):
            dp = os.path.join(temp_dir, d)
            if os.path.isdir(dp) and os.path.exists(os.path.join(dp, "pak_header.json")):
                parsed_dir = dp
                break

        if not parsed_dir:
            # Maybe it parsed directly into temp_dir
            if os.path.exists(os.path.join(temp_dir, "pak_header.json")):
                parsed_dir = temp_dir
            else:
                # List what's in temp_dir for debugging
                contents = os.listdir(temp_dir) if os.path.exists(temp_dir) else []
                write_error(progress_file, f"Parser output not found in {temp_dir}. Contents: {contents}. Stderr: {result.stderr[:200]}")
                sys.exit(1)

        models_dir = os.path.join(parsed_dir, "models")
        if not os.path.exists(models_dir):
            write_error(progress_file, f"Models directory not found: {models_dir}")
            sys.exit(1)

        write_progress(progress_file, "PARSE", 35, f"Found parsed data at {parsed_dir}")

        # ============================================================
        # Step 2: Find model JSON by name
        # ============================================================
        write_progress(progress_file, "PATCH", 40, f"Looking for model: {model_name}")

        model_json_path = os.path.join(models_dir, model_name + ".json")
        if not os.path.exists(model_json_path):
            # Try case-insensitive search
            for fn in os.listdir(models_dir):
                if fn.lower() == (model_name + ".json").lower():
                    model_json_path = os.path.join(models_dir, fn)
                    break
            else:
                write_error(progress_file, f"Model JSON not found: {model_name}.json")
                sys.exit(1)

        write_progress(progress_file, "PATCH", 45, f"Found model: {model_json_path}")

        # ============================================================
        # Step 3: Patch collision data into model JSON
        # ============================================================
        write_progress(progress_file, "PATCH", 50, "Loading collision data...")

        with open(collision_json_path, 'r') as f:
            collision_data = json.load(f)

        with open(model_json_path, 'r') as f:
            model_data = json.load(f)

        # Verify collision data
        shapes = collision_data.get('shapes', [])
        if not shapes:
            write_error(progress_file, "Collision JSON has no shapes")
            sys.exit(1)

        hk_shapes = shapes[0].get('hk_shapes', [])
        if not hk_shapes:
            write_error(progress_file, "Collision JSON has no hk_shapes")
            sys.exit(1)

        bvt_key = list(hk_shapes[0].keys())[0]
        bvt = hk_shapes[0][bvt_key]
        sv = bvt['shape']
        num_verts = len(sv.get('verts', []))
        num_tris = len(sv.get('inds', [])) // 3
        mopp_size = len(sv.get('tree', '')) // 2

        write_progress(progress_file, "PATCH", 55,
                       f"Collision: {num_verts} verts, {num_tris} tris, {mopp_size}B MOPP")

        # Check if model already has shapes
        existing_shapes = model_data.get('info', {}).get('shape_num', 0)
        if existing_shapes > 0:
            write_progress(progress_file, "PATCH", 58,
                           f"WARNING: Model already has {existing_shapes} shapes — replacing")

        # Patch
        model_data['shapes'] = shapes
        model_data['info']['shape_num'] = 1

        write_progress(progress_file, "PATCH", 60, "Writing patched model JSON...")

        with open(model_json_path, 'w') as f:
            json.dump(model_data, f, indent=2)

        write_progress(progress_file, "PATCH", 65, "Model JSON patched successfully")

        # ============================================================
        # Step 4: Repack with lotrc_rs.exe
        # ============================================================
        write_progress(progress_file, "REPACK", 70, "Starting repack...")

        # lotrc_rs.exe <parsed_dir> -o <output_dir>
        # Reads folder with pak_header.json and compiles back to PAK+BIN
        output_dir = os.path.dirname(output_pak)
        result = subprocess.run(
            [lotrc_path, parsed_dir, "-o", output_dir],
            capture_output=True, text=True, timeout=180
        )

        write_progress(progress_file, "REPACK", 85, "Repack finished, locating output...")

        # The parser outputs PAK+BIN named after the folder
        # e.g., parsed_dir = "temp/Helm'sDeep" → outputs "Helm'sDeep.PAK"
        level_name = os.path.basename(parsed_dir)
        expected_pak = os.path.join(output_dir, level_name + ".PAK")

        if not os.path.exists(expected_pak):
            # Search for any recently created PAK in output dir
            found_pak = None
            for fn in os.listdir(output_dir):
                if fn.endswith('.PAK'):
                    candidate = os.path.join(output_dir, fn)
                    if os.path.getmtime(candidate) > time.time() - 60:
                        found_pak = candidate
                        break
            if found_pak:
                expected_pak = found_pak
            else:
                write_error(progress_file, f"Repacked PAK not found. Expected: {expected_pak}. Stderr: {result.stderr[:300]}")
                sys.exit(1)

        # Move to desired output path if different
        if os.path.abspath(expected_pak) != os.path.abspath(output_pak):
            shutil.copy2(expected_pak, output_pak)

        write_progress(progress_file, "REPACK", 95, f"Output: {output_pak}")

        # ============================================================
        # Step 5: Cleanup
        # ============================================================
        write_progress(progress_file, "CLEANUP", 98, "Cleaning up temp files...")
        try:
            shutil.rmtree(temp_dir)
        except:
            pass  # Non-critical

        write_done(progress_file, output_pak)

    except subprocess.TimeoutExpired:
        write_error(progress_file, "Parser timed out (120s)")
        sys.exit(1)
    except Exception as e:
        write_error(progress_file, str(e))
        sys.exit(1)

if __name__ == '__main__':
    main()
