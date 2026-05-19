"""
compile_camera.py - dev workflow helper: recompile Scene3DCamera.cpp + relink the engine EXE.

NOTE: this is a DEV-ONLY script that drives two batch files (compile_cam.bat,
relink_exe.bat). Those batch files do NOT ship with the release; they live in
the dev's working tree. Users rebuilding from source should run rebuild_all.bat
in Scene3D/ directly, not this script.

Usage:
    python compile_camera.py [path_to_Scene3D_dir]

If no path is given, the script assumes the canonical layout:
    <Scene3D>/Vespucci/Tools/Python/compile_camera.py
and walks four levels up to find Scene3D/.
"""
import os, subprocess, sys, tempfile

if len(sys.argv) > 1:
    SCENE3D = sys.argv[1]
else:
    # __file__ -> .../Scene3D/Vespucci/Tools/Python/compile_camera.py
    # parent x4  -> .../Scene3D/
    SCENE3D = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))

cam_bat    = os.path.join(SCENE3D, "compile_cam.bat")
relink_bat = os.path.join(SCENE3D, "relink_exe.bat")
cam_out    = os.path.join(tempfile.gettempdir(), "cam_out.txt")

for required in (cam_bat, relink_bat):
    if not os.path.isfile(required):
        print(f"ERROR: {required} not found.")
        print("This dev helper depends on batch files that are not part of the release.")
        sys.exit(1)

# Step 1: compile Scene3DCamera.cpp
print("=== Compiling Scene3DCamera.cpp ===")
r = subprocess.run(['cmd', '/c', cam_bat], capture_output=True, text=True, timeout=120)
print(r.stdout)
print(r.stderr)

# Read the output file
content = ""
try:
    with open(cam_out, encoding='utf-8', errors='replace') as f:
        content = f.read()
    print(content)
except Exception as e:
    print(f"(could not read {cam_out}: {e})")

# Parse exit code from file
exit_code = 1
if "EXIT:0" in content:
    exit_code = 0
print(f"Compile exit: {exit_code}")
if exit_code != 0:
    sys.exit(exit_code)

# Step 2: relink
print("\n=== Relinking exe ===")
r2 = subprocess.run(['cmd', '/c', relink_bat], capture_output=True, text=True, timeout=180,
                    cwd=SCENE3D)
print(r2.stdout)
print(r2.stderr)
print(f"Relink exit: {r2.returncode}")
