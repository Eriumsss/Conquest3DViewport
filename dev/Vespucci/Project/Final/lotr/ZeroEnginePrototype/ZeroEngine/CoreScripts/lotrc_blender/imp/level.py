import bpy
import pathlib
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from .. import lotrc
from ..loader import LOADED_LEVELS
from .conv import *

class LoadLevelBlock(bpy.types.Operator):
    """Load Level Block from a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.load_level_block"
    bl_label = "Load LOTRC Level Block"

    def execute(self, context):
        parse_level_block(LOADED_LEVELS[context.scene.name])
        return {'FINISHED'}

class ClearLevelBlock(bpy.types.Operator):
    """Delete Previously Loaded Level Block from a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.clear_level_block"
    bl_label = "Clear Loaded LOTRC Level Block"

    def execute(self, context):
        level = LOADED_LEVELS[context.scene.name]
        if level.level_col is None: return {'FINISHED'}
        level.col.children.unlink(level.level_col)
        level.level_col = None
        bpy.ops.outliner.orphans_purge()
        return {'FINISHED'}

CLASSES = [LoadLevelBlock, ClearLevelBlock]

def empty_obj(name, col, m=None):
    obj = bpy.data.objects.new(name, None)
    col.objects.link(obj)
    obj.empty_display_type = 'SPHERE'
    obj.empty_display_size = 0
    if m is not None:
        obj.matrix_world = m
    return obj

def parse_level_block(level):
    level_col = bpy.data.collections.new("Level")
    level.level_col = level_col
    level.col.children.link(level_col)
    level_col.hide_viewport = True
    gameobjs = level.level.sub_blocks1['level'][0]
    level.types.update({key: {i.key: (i.kind, i.offset) for i in fields} for key, fields in gameobjs.types.items()})

    blender_objs = {}
    objs = {}
    obj_children = {}
    templateLevel = None
    templateGroup = None
    templateLayer = None
    templateFolder = None
    for guid, obj in gameobjs.objs.items():
        ty = obj.key
        fields = obj.fields
        fields['__layer__'] = lotrc.types.BaseTypes.Int(obj.layer)
        fields['__type__'] = ty
        name = fields['Name'][0]
        col = bpy.data.collections.new(f'{name}.{ty}.{guid}')
        if ty == 'templateLevel':
            templateLevel = col
            templateGroup = None
            templateLayer = None
            templateFolder = None
            for model in level.models:
                if model.startswith("Terrain_".lower()):
                    ter_obj = empty_obj(f"{guid}.{model}", col)
                    ter_obj.instance_collection = level.models[model]['base']
                    ter_obj.instance_type = 'COLLECTION'
            parent_col = level_col
        elif ty == 'templateGroup':
            templateGroup = col
            templateLayer = None
            templateFolder = None
            parent_col = templateLevel if templateLevel is not None else level_col
        elif ty == 'templateLayer':
            templateLayer = col
            templateFolder = None
            parent_col = templateGroup if templateGroup is not None else templateLevel if templateLevel is not None else level_col
        elif ty == 'templateFolder':
            templateFolder = col
            parent_col = templateLayer if templateLayer is not None else templateGroup if templateGroup is not None else templateLevel if templateLevel is not None else level_col
        else:
            parent_col = templateFolder if templateFolder is not None else templateLayer if templateLayer is not None else templateGroup if templateGroup is not None else templateLevel if templateLevel is not None else level_col
        parent_col.children.link(col)
        skip_store = ['WorldTransform', 'Transform', '__type__']
        if (m := fields.get('WorldTransform')) is not None:
            obj = empty_obj(col.name, col, mat_to_blender(m[0]))
            if ty in ['Road', 'Collision', 'CPSpline']:
                if ty == 'CPSpline': 
                    model_name = f"Road_{guid}".lower()
                else:
                    model_name = f'{ty}_{guid}'.lower()
                model = ''
                for name_ in level.models.keys():
                    if name_.lower().endswith(model_name):
                        model = name_
                        break
            elif ty == 'speed_tree':
                model = fields.get('Tree', [''])[0] + '#spt'
            else:
                model = fields.get('Mesh', [''])[0]
            model = model.lower()
            if model != '' and model in level.models:
                if (model := level.models.get(model)) is None: continue
                obj.instance_collection = model['base']
                obj.instance_type = 'COLLECTION'
                if 'collision' in model:
                    col_obj = empty_obj(f"{col.name}.Collision", col)
                    col_obj.instance_collection = model['collision']
                    col_obj.instance_type = 'COLLECTION'
                    col_obj.parent = obj
            else:
                obj.empty_display_size = 0.25

        if ty == "Road":
            skip_store.extend(['RoadMeshes', 'RoadMatrices'])
            for i, (model, m) in enumerate(zip(fields['RoadMeshes'][0], fields['RoadMatrices'][0])):
                child_obj = empty_obj(f"{guid}.RoadMeshes.{i}.{model}", col, mat_to_blender(m))
                if model not in level.models: continue
                child_obj.instance_collection = level.models[model.lower()]['base']
                child_obj.instance_type = 'COLLECTION'
        elif ty == "child_object":
            obj.matrix_world = mat_to_blender(fields['Transform'][0])

        col['__lotrc__'] = ['__type__']
        col['__type__'] = fields['__type__']
        for k,v in fields.items():
            if k in skip_store: continue
            col[k] = to_json(v)
            col['__lotrc__'].append(k)
        
    level_col.hide_viewport = False
