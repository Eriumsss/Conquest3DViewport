import bpy
import pathlib
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from .. import lotrc
from ..loader import GEOM_TREES, LOADED_LEVELS
from .conv import *

class DumpModels(bpy.types.Operator):
    """Dump Models from a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.load_models"
    bl_label = "Load LOTRC Models"

    def execute(self, context):
        model = context.scene.lotrc_props.selected_model
        import_models(LOADED_LEVELS[context.scene.name], model, context)
        return {'FINISHED'}

CLASSES = [LoadModels, ClearModels]

UNKNOWN = np.uint32(1)
STATIC = np.uint32(2)
SKINNED = np.uint32(4)
PHYSICS = np.uint32(8)
BREAKABLE = np.uint32(16)

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

def parse_hk_skeleton(arma_obj):
    hk_constraint = lotrc.pak_alt.HkConstraint()
    hk_constraint.info = lotrc.pak.HkConstraintInfo.from_json(arma_obj['info'])
    hk_constraint.vals2 = arma_obj['vals2']
    hk_constraint.bone_names = arma_obj['bone_names'] 
    hk_constraint.bone_order = arma_obj['bone_order']

    bones = [i for i, _ in arma_obj['bone_names']] 

    bone_order = {bone.name: i for i,bone in enumerate(bones)}
    bone_order = {
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
        transform.translation = pos_from_blender_single(mat.to_translation())
        transform.rotation = quat_from_blender(mat.to_quaternion())
        transform.scale = size_from_blender(mat.to_scale())
        bone_transforms.append(transform)

    hk_constraint.bone_parents = bone_parents
    hk_constraint.bone_transforms = bone_transforms
    return hk_constraint

def add_collision_box(obj, shape, name):
    tree, tree_in, tree_out = GEOM_TREES['Box.COLLISION']
    mod = obj.modifiers.new(name, 'NODES')
    mod.node_group = tree
    mod[tree_in.outputs['half_extents'].identifier] = size_to_blender(shape.half_extents)

def add_collision_sphere(obj, shape, name):
    tree, tree_in, tree_out = GEOM_TREES['Sphere.COLLISION']
    mod = obj.modifiers.new(name, 'NODES')
    mod.node_group = tree
    mod[tree_in.outputs['radius'].identifier] = shape.radius
    return obj

def add_collision_cylinder(obj, shape, name):
    tree, tree_in, tree_out = GEOM_TREES['Cylinder.COLLISION']
    mod = obj.modifiers.new(name, 'NODES')
    mod.node_group = tree
    mod[tree_in.outputs['point1'].identifier] = pos_to_blender_single(shape.point1)
    mod[tree_in.outputs['point2'].identifier] = pos_to_blender_single(shape.point2)
    mod[tree_in.outputs['radius'].identifier] = shape.radius
    return obj

def add_collision_capsule(obj, shape, name):
    tree, tree_in, tree_out = GEOM_TREES['Capsule.COLLISION']
    mod = obj.modifiers.new(name, 'NODES')
    mod.node_group = tree
    mod[tree_in.outputs['point1'].identifier] = pos_to_blender_single(shape.point1)
    mod[tree_in.outputs['point2'].identifier] = pos_to_blender_single(shape.point2)
    mod[tree_in.outputs['radius'].identifier] = shape.radius
    return obj

def add_collision(shape, col, base_name, i, skeleton, bones):
    par_obj = bpy.data.objects.new(f"{base_name}.COLLISION{i}", None)
    col.objects.link(par_obj)
#    bone = skeleton.pose.bones[bones[shape.info.offset]]
    if shape.info.kind != 0 and bones[shape.info.offset] != '':
        par_obj.parent = skeleton
        par_obj.parent_type = "BONE"
        par_obj.parent_bone = bones[shape.info.offset]
        # account for the position being at the tail of the bone
        par_obj.matrix_local = Matrix.Translation([0, -0.2, 0])
    par_obj.empty_display_size = 0.1
    for j, hkshp in enumerate(shape.hk_shapes):
        name = hkshp.__class__.__name__.split('_')[-1]
        name = f"{base_name}.COLLISION{i}.{name}{j}"
        mesh = bpy.data.meshes.new(name)
        obj = bpy.data.objects.new(name, mesh)
        obj.parent = par_obj
        obj.display_type = 'WIRE'      
        col.objects.link(obj)
        if isinstance(hkshp, lotrc.pak_alt.HkShape.Box):
            add_collision_box(obj, hkshp.info, name)
        elif isinstance(hkshp, lotrc.pak_alt.HkShape.Sphere):
            add_collision_sphere(obj, hkshp.info, name)
        elif isinstance(hkshp, lotrc.pak_alt.HkShape.Capsule):
            add_collision_capsule(obj, hkshp.info, name)
        elif isinstance(hkshp, lotrc.pak_alt.HkShape.Cylinder):
            add_collision_cylinder(obj, hkshp.info, name)
        elif isinstance(hkshp, lotrc.pak_alt.HkShape.ConvexVertices):
            mesh.from_pydata(pos_to_blender_alt(hkshp.shape.verts), [], [])
        elif isinstance(hkshp, lotrc.pak_alt.HkShape.BVTreeMesh):
            inds = hkshp.shape.inds
            mesh.from_pydata(pos_to_blender_alt(hkshp.shape.verts), [], [inds[i:i+3] for i in range(0,len(inds),3)])
        else:
            return
        
        obj.matrix_local = Matrix.LocRotScale(
            pos_to_blender_single(hkshp.info.translation),
            quat_to_blender(hkshp.info.rotation),
            (1,1,1)
        )

def parse_collision(obj):
    ty = obj['type']
    if ty == 'Box':
        hkshp = lotrc.pak_alt.HkShape.Box()
    elif ty == 'Sphere':
        hkshp = lotrc.pak_alt.HkShape.Sphere()
    elif ty == 'Capsule':
        hkshp = lotrc.pak_alt.HkShape.Capsule()
    elif ty == 'Cylinder':
        hkshp = lotrc.pak_alt.HkShape.Cylinder()
    elif ty == 'ConvexVertices':
        hkshp = lotrc.pak_alt.HkShape.ConvexVertices()
    elif ty == 'BVTreeMesh':
        hkshp = lotrc.pak_alt.HkShape.BVTreeMesh()
    else:
        return
    
    

def parse_mat():
    pass

def parse_mesh():
    pass

def parse_model():
    pass

def parse_models():
    pass
