import os
print("Testing Discworld files...")
print(f"CoriSoundID.bin exists: {os.path.exists('Discworld/audio/CoriSoundID.bin')}")
print(f"CoriSoundID.audio.json exists: {os.path.exists('Discworld/audio/CoriSoundID.audio.json')}")
print(f"VO_Cori.bnk exists: {os.path.exists('Discworld/audio/English(US)/VO_Cori.bnk')}")

if os.path.exists('Discworld/audio/CoriSoundID.bin'):
    size = os.path.getsize('Discworld/audio/CoriSoundID.bin')
    print(f"CoriSoundID.bin size: {size} bytes")
    
    with open('Discworld/audio/CoriSoundID.bin', 'rb') as f:
        header = f.read(16)
        print(f"First 16 bytes: {header.hex()}")

