import json
import utils
# try to add required objects to a gamemode by setting the corresponding gamemodemask

src_path = "DumpedLevels/MinasTirith_Top"
gamemodeguid = 144015924

# for finding missing objects in the gamemode, set to dst of get_obj_index.py
obj_index = None

to_remove = set()
to_add = {}

with utils.writer(src_path) as src:
    files = {i.casefold(): i for i in src.files()}

    anim_infos = json.loads(src.read('animation_block_infos.json'))

    gamemodemask = -1
    for i, val in enumerate(anim_infos):
        if val.get('guid', val.get('unk_1')) == gamemodeguid:
            gamemodemask = 1 << i
            print(f'found gamemode at index {i}, {gamemodemask}')
            break

    vals = json.loads(src.read(files['sub_blocks1/level.json']))
    
    # update the gamemodemask of all objects used in the gamemode
    print("updating GameModeMask in level data")
    gmd = utils.find_obj(vals, gamemodeguid)
    # gmd['fields']['GameModeMask'] = -1
    processed = set()
    objs = utils.copy_tree(vals, gamemodeguid, processed=processed)
    guids = set(i['fields']['GUID'] for i in objs)
    for i in utils.get_layer(vals, *gmd['fields']['Layers']):
        if i['fields']['GUID'] not in guids:
            for j in utils.copy_tree(vals, i['fields']['GUID'], processed=processed):
                if j['fields']['GUID'] not in guids:
                    guids.add(j['fields']['GUID'])
                    objs.append(j)
    for obj in objs:
        obj['fields']['GameModeMask'] |= gamemodemask

    to_remove.add('sub_blocks1/level.json')
    to_add['sub_blocks1/level.json'] = json.dumps(vals, indent=1)

    # get all used scripts, meshes, effects and animations 
    # if the gamemode is brand new then this is the same objects as above
    print("finding objects used in gamemode")
    infos = set()
    for i in vals['objs']:
        if 'GameModeMask' in i['fields'] and (i['fields']['GameModeMask'] & gamemodemask) == 0: continue
        utils.scan(vals, i['fields']['GUID'], infos=infos)

    scripts = set([i for i in infos if f'sub_blocks1/{i}.lua' in files])

    for i in scripts:
        if not i.startswith("anm_"): continue
        anim_table = json.loads(src.read(files[f'animation_tables/{i}.json']))
        for anim in anim_table.values():
            if isinstance(anim, list):
                infos.update([i.casefold() for i in anim])
            else:
                infos.add(anim.casefold())

    print("models / effects / anims")
    textures = set([i for i in infos if f'textures/{i}.json' in files])
    for k in infos:
        if (f_name := utils.get_model(files, k)):
            obj = src.read(f_name)
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(f_name)
            to_add[f_name] = obj
            textures.update([val.decode().casefold() for i in obj.split(b'tex')[1:] if i[:4] != b'data' and (val:=i.split(b'"')[2]) != b''])
        f_name = f"effects/{k}.json"
        if f_name in files:
            obj = src.read(files[f_name])
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(f_name)
            to_add[f_name] = obj
        f_name = f"animations/{k}.json"
        if f_name in files:
            obj = src.read(files[f_name])
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(f_name)
            to_add[f_name] = obj
    
    print("textures")
    for k in textures:
        f_name = f"textures/{k}.json"
        if f_name in files:
            obj = src.read(files[f_name])
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(f_name)
            to_add[f_name] = obj
        
    print("applying changes")
    src.remove(*[files[i.casefold()] for i in to_remove])
    for f_name, data in to_add.items():
        src.writestr(files[f_name.casefold()], data)

if obj_index is not None:
    with open(obj_index, "rb") as f:
        obj_index = json.load(f)

    missing_models = [i for i in infos if i in obj_index['models'] and utils.get_model(files, i) is None]
    missing_animations = [i for i in infos if i in obj_index['animations'] and f'animations/{i}.json' not in files]
    missing_effects = [i for i in infos if i in obj_index['effects'] and f'effects/{i}.json' not in files]
    missing_scripts = [i for i in infos if i in obj_index['scripts'] and f'sub_blocks1/{i}.lua' not in files]

    print("missing models:", missing_models)
    print("missing animations:", missing_animations)
    print("missing effects:", missing_effects)
    print("missing scripts:", missing_scripts)
