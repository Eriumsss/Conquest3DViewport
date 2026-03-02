import json
import os
import zipfile
from pathlib import Path

# produces a file listing the locations of various objects, 
# useful for determining what objects are missing in a gamemode and determining where they can be found

# should be a folder that contains several dumped levels
src_dir = "DumpedLevels"
dst = "DumpedLevels/index.json"

files = [i for i in os.listdir(src_dir) if (i.endswith('.zip') or os.path.isdir(i)) and not i.startswith('level_info')]
animations = {}
effects = {}
models = {}
scripts = {}
for file in files:
    with zipfile.ZipFile(Path(src_dir).joinpath(file), "r") as f:
        src_files = [Path(i.filename) for i in f.filelist]
        for i in src_files:
            kind = i.parent.name
            name = i.stem.casefold()
            if kind == 'animations':
                obj = animations.get(name, [])
                obj.append((file, str(i)))
                animations[name] = obj
            if kind == 'effects':
                obj = effects.get(name, [])
                obj.append((file, str(i)))
                effects[name] = obj
            if kind == 'models':
                obj = models.get(name, [])
                obj.append((file, str(i)))
                models[name] = obj
            if kind == 'scripts':
                obj = scripts.get(name, [])
                obj.append((file, str(i)))
                scripts[name] = obj
with open(dst, "w") as f:
    json.dump({
        'animations': animations,
        'effects': effects,
        'models': models,
        'scripts': scripts,
    }, f, indent=1)
