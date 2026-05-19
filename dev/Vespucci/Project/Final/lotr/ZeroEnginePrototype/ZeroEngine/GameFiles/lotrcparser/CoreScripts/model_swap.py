import json
import utils

### Replacing Helm'sDeep good scout model with wormtongue from Isengard
# first dump the needed levels
# run this script pointing to the needed levels with the correct src model and target class name
# then compile the dumped dst level back into a .PAK and .BIN file

new_model = 'CH_hum_Wormtongue_01'
targ_clss = 'CH_NML_SIL_Scout'

src_path = "DumpedLevels/Isengard"
dst_path = "DumpedLevels/Helm'sDeep"

to_remove = set()
to_add = {}

with utils.reader(src_path) as src, utils.writer(dst_path) as dst:
    src_files = {i.casefold(): i for i in src.files()}
    dst_files = {i.casefold(): i for i in dst.files()}

    # dump the level block to move relevant infomation
    vals = json.loads(dst.read(dst_files['sub_blocks1/level.json']))

    for i, obj in enumerate(vals['objs']):
        if obj['fields'].get('Name') == targ_clss: 
            print('found')
            break
     
    if 'Meshes' in vals['objs'][i]['fields']:
        old_model = vals['objs'][i]['fields']['Meshes'].pop()
        vals['objs'][i]['fields']['meshes'].append(new_model)
    else:
        old_model = vals['objs'][i]['fields']['Mesh']
        vals['objs'][i]['fields']['Mesh'] = new_model
    
        
    to_remove.add('sub_blocks1/level.json')
    to_add[dst_files['sub_blocks1/level.json']] = json.dumps(vals, indent=1)
    gamemodemask, *_ = utils.get_gamemodemask(dst.read(utils.get_model(dst_files, old_model)))

    textures = set()
    for k in [new_model]:
        if (dst_name := utils.get_model(dst_files, k)) is not None:
            obj = dst.read(dst_name)
            m, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask | m).encode() + obj[b:]
            to_remove.add(dst_name)
            to_add[dst_name] = obj
            textures.update([val.decode().casefold() for i in obj.split(b'tex')[1:] if i[:4] != b'data' and (val:=i.split(b'"')[2]) != b''])
        elif (src_name := utils.get_model(src_files, k)) is not None:
            obj = src.read(src_name)
            _, a, b = utils.get_gamemodemask(obj)
            obj = obj[:a] + str(gamemodemask).encode() + obj[b:]
            to_add[src_name] = obj
            textures.update([val.decode().casefold() for i in obj.split(b'tex')[1:] if i[:4] != b'data' and (val:=i.split(b'"')[2]) != b''])

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

    dst.remove(*[dst_files[i] for i in to_remove])
    for f_name, data in to_add.items():
        dst.writestr(f_name, data)
