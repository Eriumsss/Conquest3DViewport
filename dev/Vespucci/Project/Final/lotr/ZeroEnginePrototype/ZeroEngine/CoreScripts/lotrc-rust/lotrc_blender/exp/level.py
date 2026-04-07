import bpy
import pathlib
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from .. import lotrc
from ..loader import LOADED_LEVELS
from .conv import *

class DumpLevelBlock(bpy.types.Operator):
    """Dump Level Block from a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.dump_level_block"
    bl_label = "Load LOTRC Level Block"

    def execute(self, context):
        dump_level_block(LOADED_LEVELS[context.scene.name])
        return {'FINISHED'}

CLASSES = [DumpLevelBlock]

BASE_TYPES = {
    'crc': lotrc.types.BaseTypes.CRC,
    'guid': lotrc.types.BaseTypes.GUID,
    'color': lotrc.types.BaseTypes.Color,
    'vector2': lotrc.types.BaseTypes.Vector2,
    'vector3': lotrc.types.BaseTypes.Vector3,
    'vector4': lotrc.types.BaseTypes.Vector4,
    'matrix4x4': lotrc.types.BaseTypes.Matrix4x4,
    'float': lotrc.types.BaseTypes.Float,
    'int': lotrc.types.BaseTypes.Int,
    'bool': lotrc.types.BaseTypes.Bool,
    'string': lotrc.types.BaseTypes.String,
    'stringlist': lotrc.types.BaseTypes.StringList,
    'objectlist': lotrc.types.BaseTypes.ObjectList,
    'nodelist': lotrc.types.BaseTypes.NodeList,
    'intlist': lotrc.types.BaseTypes.IntList,
    'crclist': lotrc.types.BaseTypes.CRCList,
    'weightlist': lotrc.types.BaseTypes.WeightList,
    'matrixlist': lotrc.types.BaseTypes.MatrixList,
    'byte': lotrc.types.BaseTypes.Byte,
}

def dump_level_block(level):
    types = {}
    for name, fields in level.types.items():
        type_fields = []
        for key, (kind, offset) in fields.items():
            t = lotrc.types.GameObjsTypeField()
            t.key = key
            t.kind = kind
            t.offset = offset
            type_fields.append(t)
        types[name] = type_fields

    temp_objs = {}
    parents = {}
    inv_world = {}
    world = {}
    blender_objs = list(level.level_col.children)
    while blender_objs != []:
        blender_obj = blender_objs.pop(0)
        if (fields := blender_obj.get('lotrc')) is None:
            continue
        kind = fields['__type__']
        skip = ['Transform', 'WorldTransform', 'RoadMeshes', 'RoadMatrices']

        ty = level.types[kind]
        dump_fields = {
            key: BASE_TYPES[kind.lower()].from_json(fields[key]) 
            if key not in skip else None
            for key, (kind, _) in ty.items()
        } 
        dump_fields['__type__'] = kind
        dump_fields['__layer__'] = lotrc.types.BaseTypes.Int.from_json(fields['__layer__'])[0]
        guid = dump_fields['GUID'][0]

        if kind in ['Road', 'CPSpline']:
            road_meshes = []
            road_matrices = []
            dump_fields['RoadMeshes'] = road_meshes
            dump_fields['RoadMatrices'] = road_matrices

        for child_obj in blender_obj.objects:
            if child_obj.name == blender_obj.name:
                dump_fields['WorldTransform'] = np.array(child_obj.matrix_world)
                inv_world[guid] = np.array(child_obj.matrix_world.inverted())
                world[guid] = np.array(child_obj.matrix_world)
            else:
                vals = child_obj.name.split('.')
                if vals[1] == 'RoadMeshes' and kind in ['Road', 'CPSpline']:
                    road_meshes.append(vals[3])
                    road_matrices.append(mat_from_blender(child_obj.matrix_world))
        
        temp_objs[guid] = dump_fields

        if kind in ['templateLevel', 'templateGroup', 'templateLayer', 'templateFolder']:
            blender_objs = list(blender_obj.children) + blender_objs

        elif kind == 'ResourceEmitter':
            for child in dump_fields['SpawnRegions'][0]:
                parents[child] = guid
        elif kind == 'spawn_point':
            for child in dump_fields['Nodes'][0]:
                parents[child] = guid
        if (children := dump_fields.get('InitialChildObjects')) is not None:
            for child in children[0]:
                parents[child] = guid
        if (parent := dump_fields['ParentGUID'][0]) != 0:
            parents[guid] = parent
    
    objs = {}
    for guid, fields in temp_objs.items():
        kind = fields['__type__']
        if kind in ['Road', 'CPSpline']:
            fields['RoadMeshes'] = lotrc.types.BaseTypes.CRCList(road_meshes)
            fields['RoadMatrices'] = lotrc.types.BaseTypes.MatrixList(road_matrices)
        if 'Transform' in fields:
            t = fields['WorldTransform'].copy()
            if (parent := parents.get(guid)) is not None:
                if kind == 'child_object':
                    fields['WorldTransform'] = world[parent] @ t
                else:
                    t = inv_world[parent] @ t
            fields['Transform'] = lotrc.types.BaseTypes.Matrix4x4(mat_from_blender(t))
        if 'WorldTransform' in fields:
            fields['WorldTransform'] = lotrc.types.BaseTypes.Matrix4x4(mat_from_blender(
                fields['WorldTransform']
            ))
        obj = lotrc.types.GameObj()
        obj.layer = fields['__layer__']
        obj.key = kind
        obj.fields = {i: fields[i] for i in level.types[kind]}
        objs[guid] = obj

    gameobjs = lotrc.types.GameObjs()
    gameobjs.gamemodemask = -1
    gameobjs.types = types
    gameobjs.objs = objs

    sub_blocks1 = level.level.sub_blocks1
    sub_blocks1['level'] = lotrc.types.SubBlock.GameObjs(gameobjs)
    level.level.sub_blocks1 = sub_blocks1
