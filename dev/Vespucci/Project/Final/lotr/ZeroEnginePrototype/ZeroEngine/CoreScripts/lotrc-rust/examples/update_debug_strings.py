import json
import utils
# updates the debug strings with added values that will be hashed to a crc32 when converting back to a level
# if you are adding new names to files outside of effects and level.json then it's possible that they might be missed
# make sure to keep a backup of original modified files when converting to and from a level

src_path = "DumpedLevels/MinasTirith_Top"

to_remove = set()
to_add = {}

# get strings for gameobjs that will be converted to crc32
def gameobjs_crcs(objs, types):
    types = {ty['name']: {t['name']: t['type'] for t in ty['fields']} for ty in types}
    for obj in objs:
        ty = types[obj['type']]
        for name, v in obj['fields'].items():
            t = ty[name]
            if t == 'crc':
                yield v
            elif t == 'crclist':
                for v_ in v:
                    yield v_

with utils.writer(src_path) as src:
    files = {i.casefold(): i for i in src.files()}
    pak_strings = json.loads(src.read(files['pak_strings.json']))
    bin_strings = json.loads(src.read(files['bin_strings.json']))
    old_debug_strings = set([i.casefold() for i in pak_strings + bin_strings])

    to_remove.add('pak_strings.json')
    to_remove.add('bin_strings.json')

    # get names of things
    for file in files:
        # get name and types
        vals = file.split('/')
        if len(vals) != 2: continue
        ty, name = vals
        # strip suffix
        name, *ext = name.split('.')
        if ext[-1] != 'json': continue
        
        if ty == 'animations':
            name = src.read(files[file]).split(b'key": "')[1].split(b'"')[0].decode()
            if name.casefold() not in old_debug_strings and not name.startswith('0x'):
                pak_strings.append(name)
                old_debug_strings.add(name.casefold())
        elif ty == 'textures':
            name = src.read(files[file]).split(b'key": "')[1].split(b'"')[0].decode()
            if name.casefold() not in old_debug_strings and not name.startswith('0x'):
                bin_strings.append(name)
                old_debug_strings.add(name.casefold())
        elif ty == 'models':
            text = src.read(files[file])
            name = text.split(b'key": "')[1].split(b'"')[0].decode()
            if name.casefold() not in old_debug_strings and not name.startswith('0x'):
                pak_strings.append(name)
                old_debug_strings.add(name.casefold())
            name = text.split(b'asset_key": "')[1].split(b'"')[0].decode()
            if name.casefold() not in old_debug_strings and not name.startswith('0x'):
                bin_strings.append(name)
                old_debug_strings.add(name.casefold())
        elif ty == 'effects':
            vals = json.loads(src.read(files[file]))
            for name in gameobjs_crcs(vals['objs'], vals['types']):
                if name.casefold() not in old_debug_strings and not name.startswith('0x'):
                    pak_strings.append(name)
                    old_debug_strings.add(name.casefold())
    
    vals = json.loads(src.read(files['sub_blocks1/level.json']))
    for name in gameobjs_crcs(vals['objs'], vals['types']):
        if name.casefold() not in old_debug_strings and not name.startswith('0x'):
            pak_strings.append(name)
            old_debug_strings.add(name.casefold())

    to_add['pak_strings.json'] = json.dumps(pak_strings, indent=1)
    to_add['bin_strings.json'] = json.dumps(bin_strings, indent=1)
        
    print("applying changes")
    src.remove(*[files[i.casefold()] for i in to_remove])
    for f_name, data in to_add.items():
        src.writestr(files[f_name.casefold()], data)
