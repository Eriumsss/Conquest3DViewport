import bpy
import pathlib
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from .. import lotrc
from ..loader import GEOM_TREES, LOADED_LEVELS
from .conv import *

class DumpModels(bpy.types.Operator):
    """Dump Models from a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.dump_models"
    bl_label = "Load LOTRC Models"

    def execute(self, context):
        model = context.scene.lotrc_props.selected_model
        parse_models(self, LOADED_LEVELS[context.scene.name], model, context)
        return {'FINISHED'}

CLASSES = [DumpModels]


def parse_skeleton(model, arma_obj):
    bones = arma_obj['bones']

    bone_order = {bone.name: i for i,bone in enumerate(bones)}
    bone_parents = []
    bone_mats = []
    for name in bones:
        bone = arma_obj.data.bones[name]
        mat = bone.matrix_local
        bone_parent = -1
        if (parent := bone.parent) is not None:
            mat = parent.matrix_local.inverted() @ mat
            bone_parent = bone_order[parent.name]
        bone_parents.append(bone_parent)
        bone_mats.append(mat_from_blender(mat))
    model.bones = bones
    model.bone_parents = bone_parents
    model.bone_mats = bone_mats

def parse_hk_skeleton(model, arma_obj):
    hk_constraint = lotrc.pak_alt.HkConstraint()
    hk_constraint.info = lotrc.pak.HkConstraintInfo.from_json(arma_obj['info'])
    hk_constraint.vals2 = arma_obj['vals2']
    hk_constraint.bone_names = arma_obj['bone_names'] 

    bones = [i for i in arma_obj['bone_names']] 

    bone_order = {bone: i for i,bone in enumerate(bones)}
    bone_parents = []
    bone_transforms = []
    for name in bones:
        bone = arma_obj.data.bones[name]
        mat = bone.matrix_local
        bone_parent = -1
        if (parent := bone.parent) is not None:
            mat = parent.matrix_local.inverted() @ mat
            bone_parent = bone_order[parent.name]
        bone_parents.append(bone_parent)
        transform = lotrc.pak_alt.TRS()
        transform.translation = pos_from_blender_single(mat.to_translation()) + (1,)
        transform.rotation = quat_from_blender(mat.to_quaternion())
        transform.scale = size_from_blender(mat.to_scale()) + (0,)
        bone_transforms.append(transform)

    hk_constraint.bone_parents = bone_parents
    hk_constraint.bone_transforms = bone_transforms
    model.hk_constraint = hk_constraint

def parse_mesh(obj, info, vertex_data, index_data):
    mesh = obj.data
    info.variation_id = mesh["variation_id"]
    info.variation = mesh["variation"]
    
    skin_name = "Armature"
    if skin_name in obj.modifiers:
        skin = obj.modifiers[skin_name]

    inds = index_data[info.ibuff_info_offset]
    vals = [i for tri in mesh.polygons for i in tri.vertices]
    if isinstance(inds, lotrc.pak.IndexBuffer.U16):
        inds = lotrc.pak.IndexBuffer.U16(vals)
    elif isinstance(inds, lotrc.pak.IndexBuffer.U32):
        inds = lotrc.pak.IndexBuffer.U32(vals)
    index_data[info.ibuff_info_offset] = inds

    vertex_inds = np.zeros(len(mesh.vertices), 'I')
    for i, j in enumerate(vals):
        vertex_inds[j] = i    
    
    offset = info.vbuff_info_offset_2
    if offset == 0xFFFFFFFF:
        offset = info.vbuff_info_offset
    attrs = {i: j for i,j in vertex_data[offset].items()}
    positions = attrs.pop('Position', None)
    new_attrs = {} 
    pos = pos_from_blender(np.array([vert.co for vert in mesh.vertices]))
    if isinstance(positions, lotrc.pak.VertexTypes.Vector3):
        positions = lotrc.pak.VertexTypes.Vector3(*pos)
    if isinstance(positions, lotrc.pak.VertexTypes.Vector4):
        positions = lotrc.pak.VertexTypes.Vector4(*pos, np.zeros(len(mesh.vertices)))
    new_attrs['Position'] = positions

    if (normals := attrs.pop('Normal', None)) is not None:
        norms = np.empty(3 * len(mesh.loops))
        mesh.loops.foreach_get("normal", norms)
        norms = np.vstack([pos_from_blender(norms.reshape(-1, 3)[vertex_inds]), np.zeros(len(vertex_inds))])
        if isinstance(normals, lotrc.pak.VertexTypes.Unorm4x8):
            normals = lotrc.pak.VertexTypes.Unorm4x8(
                np.frombuffer(((norms.T + 1.0) * 127.5).astype('B').tobytes(), 'I')
            )
        elif isinstance(normals, lotrc.pak.VertexTypes.Vector4):
            normals = lotrc.pak.VertexTypes.Vector4(*norms)
        new_attrs['Normal'] = normals

    for i in range(4):
        uv_attr_name = f'TextureCoord({i})'
        uv_name = 'UVMap' if i == 0 else f'UV{i}'
        if (uv := attrs.pop(uv_attr_name, None)) is not None and (uv_layer := mesh.uv_layers.get(uv_name)) is not None:
            vals = np.empty(2 * len(uv_layer.data))
            uv_layer.data.foreach_get("uv", vals)
            vals = vals.reshape(-1, 2)[vertex_inds].T
            new_attrs[uv_attr_name] = lotrc.pak.VertexTypes.Vector2(*vals)

    if (psize := attrs.pop('PSize', None)) is not None and (attr := mesh.attributes.get('PSize')) is not None:
        vals = np.empty(len(attr.data) * 3)
        attr.data.foreach_get('vector', vals)
        vals = vals.reshape(-1, 3).T
        new_attrs['PSize'] = lotrc.pak.VertexTypes.Vector3(*vals)
    
    weights = attrs.pop('BlendWeight', None)
    indices = attrs.pop('BlendIndices', None)
    if weights is not None and indices is not None:
        inds = np.zeros((len(mesh.vertices),4))
        ws = np.zeros((len(mesh.vertices),4))
        for i, v in enumerate(mesh.vertices):
            for j, (g, k) in enumerate(zip(v.groups, [2,1,0,3])):
                inds[i][j] = g.group
                ws[i][k] = g.weight
        new_attrs['BlendWeight'] = lotrc.pak.VertexTypes.Unorm4x8(np.frombuffer((ws * 255.0).astype('B').tobytes(), 'I'))
        new_attrs['BlendIndices'] = lotrc.pak.VertexTypes.Unorm4x8(np.frombuffer(inds.astype('B').tobytes(), 'I'))
    for i, (attr_name, data) in enumerate(attrs.items()):
        attr = mesh.attributes.get(attr_name)
        if attr is None: continue
        if isinstance(data, lotrc.pak.VertexTypes.Vector3):
            vals = np.empty(len(attr.data) * 3)
            attr.data.foreach_get('vector', vals)
            data = lotrc.pak.VertexTypes.Vector3(*vals.reshape(-1, 3).T)
        elif isinstance(data, lotrc.pak.VertexTypes.Vector4):
            vals = np.empty(len(attr.data) * 4)
            attr.data.foreach_get('color', vals)
            data = lotrc.pak.VertexTypes.Vector4(*vals.reshape(-1, 4).T)
        elif isinstance(data, lotrc.pak.VertexTypes.Unorm4x8):
            vals = np.empty(len(attr.data) * 4)
            attr.data.foreach_get('color', vals)
            data = lotrc.pak.VertexTypes.Unorm4x8(np.frombuffer(vals.reshape(-1, 4).astype('B').tobytes(), 'I'))
        elif isinstance(data, lotrc.pak.VertexTypes.Pad):
            vals = np.empty(len(attr.data), dtype='I')
            attr.data.foreach_get('value', vals)
            data = lotrc.pak.VertexTypes.Pad(vals)
        new_attrs[attr_name] = data
    
    new_attrs.update({i:j for i,j in attrs.items() if i not in new_attrs})
    vertex_data[offset] = new_attrs

def add_mesh(info, vertex_data, index_data, usage, name, col, obj_arma, skin_bones):
    for i, (attr, data) in enumerate(attrs.items()):
        if isinstance(data, lotrc.pak.VertexTypes.Vector3):
            ty = 'FLOAT_VECTOR'
            data = [i for j in zip(data[0], data[1], data[2]) for i in j]
            dat_name = 'vector'
        elif isinstance(data, lotrc.pak.VertexTypes.Vector4):
            ty = 'FLOAT_COLOR'
            data = [i for j in zip(data[0], data[1], data[2], data[3]) for i in j]
            dat_name = 'color'
        elif isinstance(data, lotrc.pak.VertexTypes.Vector2):
            ty = 'FLOAT2'
            data = [i for j in zip(data[0], data[1]) for i in j]
            dat_name = 'vector'
        elif isinstance(data, lotrc.pak.VertexTypes.Unorm4x8):
            ty = 'BYTE_COLOR'
            data = np.frombuffer(np.array(data[0], 'I').tobytes(), 'B').astype('f')/255.0
            dat_name = 'color'
        elif isinstance(data, lotrc.pak.VertexTypes.Pad):
            ty = 'INT'
            data = data[0]
            dat_name = 'value'
        else:
            # handle buffer_info case
            continue
        attribute = mesh.attributes.new(attr, ty, 'POINT')
        attribute.data.foreach_set(dat_name, data)
    return obj


def parse_model(op, model, name, col):
    skeleton_name = "SKELETON.{name}"
    if skeleton_name in col.objects:
        parse_skeleton(model, col.objects[skeleton_name])
    hk_skeleton_name = f"HK_SKELETON.{name}"
    if hk_skeleton_name in col.objects:
        parse_hk_skeleton(model, col.objects[hk_skeleton_name])

    vertex_data = model.vertex_data
    index_data = model.index_data
    buffer_infos = model.buffer_infos
    meshes_name = f"MESHES.{name}"
    if meshes_name not in col.children:
        op.report({"WARNING"}, f"Model {name} collection {meshes_name} not found")
        return
    meshes_col = col.children[meshes_name]
    for i, info in enumerate(zip(model.buffer_infos)):
        mesh_name =  f"MESH{i}.{name}"
        if mesh_name not in meshes_col.objects:
            op.report({"WARNING"}, f"Model {name} mesh {mesh_name} not found")
            continue
        info = buffer_infos[i]
        parse_mesh(meshes_col.objects[mesh_name], info, vertex_data, index_data)
        buffer_infos[i] = info
    model.vertex_data = vertex_data
    model.index_data = index_data
    model.buffer_infos = buffer_infos

    # reconstruct lods based on mesh_order
    


def import_model(level, model, model_name, models_col, context):
    
    # collections to organize meshes
    info = model.info
    k = 0
    col_base = bpy.data.collections.new(f"BASE.{model_name}")
    model_col.children.link(col_base)
    for i, lod in enumerate(lods):
        col = bpy.data.collections.new(f"LOD{i}.{model_name}")
        model_col.children.link(col)
        for j, f in mesh_order[k:lod.start]:
            obj = copy_mesh(f"LOD{i}.MESH{j}.UNKNOWN{f}.{model_name}", meshes[j])
            col.objects.link(obj)
            #if i == 0 and f != 1 and meshes[j].data['variation_id'] == 0xFF: col_base.objects.link(obj)
            if i == 0 and f != 1: col_base.objects.link(obj)
        for j, f in mesh_order[lod.start:lod.static_end]:
            obj = copy_mesh(f"LOD{i}.MESH{j}.STATIC{f}.{model_name}", meshes[j])
            col.objects.link(obj)
            #if i == 0 and f != 1 and meshes[j].data['variation_id'] == 0xFF: col_base.objects.link(obj)
            if i == 0 and f != 1: col_base.objects.link(obj)
        for j, f in mesh_order[lod.static_end:lod.skinned_end]:
            obj = copy_mesh(f"LOD{i}.MESH{j}.SKINNED{f}.{model_name}", meshes[j])
            col.objects.link(obj)
            #if i == 0 and f != 1 and meshes[j].data['variation_id'] == 0xFF: col_base.objects.link(obj)
            if i == 0 and f != 1: col_base.objects.link(obj)
        for j, f in mesh_order[lod.skinned_end:lod.physics_end]:
            obj = copy_mesh(f"LOD{i}.MESH{j}.PHYSICS{f}.{model_name}", meshes[j])
            col.objects.link(obj)
            #if i == 0 and f != 1 and meshes[j].data['variation_id'] == 0xFF: col_base.objects.link(obj)
            if i == 0 and f != 1: col_base.objects.link(obj)
        for j, f in mesh_order[lod.physics_end:lod.breakable_end]:
            obj = copy_mesh(f"LOD{i}.MESH{j}.BREAKABLE{f}.{model_name}", meshes[j])
            col.objects.link(obj)
            #if i == 0 and f != 1 and meshes[j].data['variation_id'] == 0xFF: col_base.objects.link(obj)
            if i == 0 and f != 1: col_base.objects.link(obj)
        k = lod.breakable_end
    model_col['base'] = col_base

def parse_models(op, level, model, context):
    models = level.level.models
    if model == 'All Models':
        to_process = models
    elif model in models:
        to_process = {model: models[model]}
    else:
        return
    
    models_col = level.col.children["Models"]

    for model_name, model in to_process.items():
        if model_name not in models_col.children:
            op.report({"WARNING"}, f"Model {model_name} not found")
            continue
        parse_model(op, model, model_name, models_col.children[model_name])
        models[model_name] = model
    level.level.models = models
