import json
import utils
from pathlib import Path

### Porting the Balrog from the Shire to Minas Tirith Top. 
# first dump the needed levels
# run this script pointing to the needed levels with the correct guids, gamemodemask and soundbanks
# then compile the dumped dst level back into a .PAK and .BIN file

# soundbanks required for the character
soundbnks = [
    "ChatterHeroBalrog.bnk",
    "SFXBalrog.bnk",
]

src_path = "DumpedLevels/Shire"
dst_path = "DumpedLevels/MinasTirith_Top"

# these are the guids for the corresponding objects in the dst level
spawn_emmiter_guid = 7052754
gamemode_guid = 7052744 # gamemode object, needed for adding relevant soundbanks
gamemodemask = 3 # grabbed from a different class in the spawn list, ensures the ported objects will be visible in the targeted gamemode

# from the src level
class_guid = 109012487

to_remove = set()
to_add = {}

with utils.reader(src_path) as src, utils.writer(dst_path) as dst:
    src_files = {i.casefold(): i for i in src.files()}
    dst_files = {i.casefold(): i for i in dst.files()}
    
    # dump the level block to move relevant infomation
    vals_dest = json.loads(dst.read(dst_files['sub_blocks1/level.json']))
    vals = json.loads(src.read(src_files['sub_blocks1/level.json']))

    # get the needed objects and items associated with them
    infos = set()
    class_items = utils.copy_tree(vals, class_guid, infos=infos, gamemodemask=gamemodemask)
    scripts = set([i for i in infos if f'sub_blocks1/{i}.lua' in src_files])
    
    script_strings = {}
    for path in src_files.values():
        if path.endswith('.lua'):
            name = Path(path).stem.casefold()
            data = src.read(path)
            strings = set(utils.get_lua_strings(data))
            script_strings[name] = strings
    
    for i in scripts:
        if not i.startswith("anm_"): continue
        anim_table = json.loads(src.read(src_files[f'animation_tables/{i}.json']))
        for anim in anim_table.values():
            if isinstance(anim, list):
                infos.update([i.casefold() for i in anim])
            else:
                infos.add(anim.casefold())
    
    old_scripts = set(Path(i).stem for i in dst_files.keys() if i.endswith('.lua'))
    scripts.difference_update(old_scripts)
    new_scripts = set()
    common_scripts = set()
    while len(scripts) != 0:
        k = scripts.pop()
        new_scripts.add(k)
        strings = script_strings[k]
        children = strings.intersection(script_strings.keys()).difference(new_scripts).difference(scripts)
        common_scripts.update(children.intersection(old_scripts))
        for k in children.difference(old_scripts):
            scripts.add(k)

    textures = set()
    for k in infos:
        if (dst_name := utils.get_model(dst_files, k)) is not None:
            obj = dst.read(dst_name)
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(dst_name)
            to_add[dst_name] = obj
            textures.update([val.decode().casefold() for i in obj.split(b'"tex')[1:] if (val:=i.split(b'"')[2]) != b''])
        elif (src_name := utils.get_model(src_files, k)) is not None:
            obj = src.read(src_name)
            _, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask).encode() + obj[b:]
            to_add[src_name] = obj
            textures.update([val.decode().casefold() for i in obj.split(b'"tex')[1:] if (val:=i.split(b'"')[2]) != b''])

        f_name = f"effects/{k}.json"
        if f_name in dst_files:
            obj = dst.read(dst_files[f_name])
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(f_name)
            to_add[dst_files[f_name]] = obj
        elif f_name in src_files:
            obj = src.read(src_files[f_name])
            _, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask).encode() + obj[b:]
            to_add[src_files[f_name]] = obj
        
        f_name = f"animations/{k}.json"
        if f_name in dst_files:
            obj = dst.read(dst_files[f_name])
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(f_name)
            to_add[dst_files[f_name]] = obj
        elif f_name in src_files:
            obj = src.read(src_files[f_name])
            _, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask).encode() + obj[b:]
            to_add[src_files[f_name]] = obj

    for k in textures:
        f_name = f"textures/{k}.json"
        f_name_alt = f"textures/{k}.dds"
        if f_name in dst_files:
            obj = dst.read(dst_files[f_name])
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(f_name)
            to_add[dst_files[f_name]] = obj
        elif f_name in src_files:
            obj = src.read(src_files[f_name])
            _, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask).encode() + obj[b:]
            to_add[src_files[f_name]] = obj
            to_add[src_files[f_name_alt]] = src.read(src_files[f_name_alt])

    index = json.loads(dst.read(dst_files['sub_blocks1/index.json']))
    for script in new_scripts:
        f_name = src_files[f"sub_blocks1/{script}.lua"]
        index.insert(-3, f_name[len("sub_blocks1/"):])
        to_add[f_name] = src.read(f_name)
        if script.startswith("anm_"):
            f_name = src_files[f'animation_tables/{script}.json']
            to_add[f_name] = src.read(f_name)
    to_remove.add('sub_blocks1/index.json')
    to_add[dst_files['sub_blocks1/index.json']] = json.dumps(index, indent=1)
    
    # add the soundbanks for the balrog to the team death match gamemode. You'll need to check the source level.json to which banks (if any) are needed
    o = utils.find_obj(vals_dest, gamemode_guid)
    o['fields']['ModeSpecificBanks'].extend(i for i in soundbnks if i not in o['fields']['ModeSpecificBanks'])
    
    # add the balrog to the evil team
    o = utils.find_obj(vals_dest, spawn_emmiter_guid)
    o['fields']['Classes'].append(class_guid)

    # get radiosity vals
    old_rad = json.loads(src.read(src_files['radiosity.json']))
    new_rad = json.loads(dst.read(dst_files['radiosity.json']))
    
    # add the objects for the target class, only if they are missing
    # also copy over radiosity data if there is any
    old_class_objs = []
    new_class_objs = []
    for i in class_items:
        guid = i['fields']['GUID']
        val = utils.find_obj(vals_dest, guid)
        if val is not None:
            old_class_objs.append(val)
        else:
            new_class_objs.append(i)
            rad_val = old_rad['vals'].get(str(guid))
            if rad_val is not None:
                new_rad['vals'][str(guid)] = rad_val 
    
    vals_dest['objs'].extend(new_class_objs)
    valid_types = set(i['name'] for i in vals_dest['types'])
    needed_types = [utils.find_type(vals, t) for t in set(o['type'] for o in class_items).difference(valid_types)]
    vals_dest['types'].extend(needed_types)
    
    # update the gamemode mask for needed existing objects
    for i in old_class_objs:
        if 'GameModeMask' in i['fields']:
            i['fields']['GameModeMask'] |= gamemodemask
    
    to_remove.add('sub_blocks1/level.json')
    to_add[dst_files['sub_blocks1/level.json']] = json.dumps(vals_dest, indent=1)
    to_add[dst_files['radiosity.json']] = json.dumps(new_rad, indent=1)

    bin_strings_src = json.loads(src.read(src_files['bin_strings.json']))
    pak_strings_src = json.loads(src.read(src_files['pak_strings.json']))
    bin_strings_dst = json.loads(dst.read(dst_files['bin_strings.json']))
    pak_strings_dst = json.loads(dst.read(dst_files['pak_strings.json']))
        
    pak_strings = set(pak_strings_dst)
    bin_strings = set(bin_strings_dst)
    pak_strings_dst.extend([i for i in pak_strings_src if i not in pak_strings])
    bin_strings_dst.extend([i for i in bin_strings_src if i not in bin_strings])

    to_remove.add('pak_strings.json')
    to_remove.add('bin_strings.json')
    to_add[dst_files['pak_strings.json']] = json.dumps(pak_strings_dst, indent=1)
    to_add[dst_files['bin_strings.json']] = json.dumps(bin_strings_dst, indent=1)

    dst.remove(*[dst_files[i.casefold()] for i in to_remove])
    for f_name, data in to_add.items():
        dst.writestr(f_name, data)
