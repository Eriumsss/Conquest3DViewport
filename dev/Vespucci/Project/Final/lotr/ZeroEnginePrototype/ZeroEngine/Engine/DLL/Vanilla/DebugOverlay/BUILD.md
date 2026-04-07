# Debug Overlay Build Instructions

## Prerequisites

1. **Visual Studio 2019/2022** with C++ desktop development
2. **DirectX SDK (June 2010)** - Required for D3DX9
   - Download: https://www.microsoft.com/en-us/download/details.aspx?id=6812
3. **CMake 3.15+**

## Build Steps

### Option 1: Visual Studio Developer Command Prompt

```batch
cd DebugOverlay
mkdir build
cd build

# For 32-bit build (required - game is 32-bit)
cmake -G "Visual Studio 17 2022" -A Win32 ..
cmake --build . --config Release
```

### Option 2: Using CMake GUI

1. Open CMake GUI
2. Set source to `DebugOverlay` folder
3. Set build to `DebugOverlay/build`
4. Click Configure
5. Select "Visual Studio 17 2022" and platform "Win32"
6. Click Generate
7. Open the .sln and build

## Output

After building, you'll find in `build/bin/Release/`:
- `DebugOverlay.dll` - The hook DLL
- `Injector.exe` - The DLL injector

## Usage

1. Start LOTR: Conquest
2. Run `Injector.exe` as Administrator
3. The overlay should appear in-game

### Controls

| Key | Function |
|-----|----------|
| F1  | Toggle entire overlay |
| F2  | Toggle FPS counter |
| F3  | Toggle position display |
| F4  | Toggle wireframe mode |
| F5  | Toggle debug menu |

## Troubleshooting

### "D3DX9 not found"
Install the DirectX SDK June 2010.

### "Injection failed"
Run Injector.exe as Administrator.

### No overlay visible
- Check if antivirus is blocking the DLL
- Try running the game in windowed mode first

