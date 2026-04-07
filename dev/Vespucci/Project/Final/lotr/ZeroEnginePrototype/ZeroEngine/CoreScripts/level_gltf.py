import utils
import json

# constructs a .glb file containing static models from a templatelevel

# point to a dumped level folder or zip
src = "DumpedLevels/Shire"

# where to save the resulting file, should be a .glb file
dst = "Shire.glb"


with utils.reader(src) as src:
    files = {i.casefold(): i for i in src.files()}
    vals = json.loads(src.read(files["sub_blocks1/level.json"]))

    # select objects in layers used by the templatelevel
    template = vals['objs'][0]
    layers = set(template['fields']['Layers'])
    objs = [i for i in vals['objs'] if i['layer'] in layers]

    # select only static objects
    static_objs = [i for i in objs if i['type'] == 'static_object']
    names = set([i['fields']['Mesh'] for i in static_objs])

    # collect the meshes for used models in a sigle gltf
    base = utils.Glb.empty()
    meshes = {}
    for mesh in names:
        glb = utils.Glb.from_data(src.read(files[f"models/{mesh.casefold()}.glb"]))
        meshes[mesh] = len(base.gltf['meshes'])
        base.gltf['meshes'].append({
            'primitives': utils.splice_lod0(base, glb),
            'name': mesh,
        })

# add objects using the meshes
children = []
for obj in static_objs:
    children.append(len(base.gltf['nodes']))
    
    base.gltf['nodes'].append({
        'matrix': obj['fields']['transform'],
        'mesh': meshes[obj['fields']['Mesh']],
        'name': f"{obj['fields']['Name']} {{{obj['fields']['guid']}}}"
    })
base.gltf['scenes'].append({
    'nodes': [len(base.gltf['nodes'])]
})
base.gltf['nodes'].append({
    'children': children
})

# dump the glb file
with open(dst, 'wb') as f:
    f.write(base.dump())
