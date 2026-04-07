import bpy
import pathlib
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from . import lotrc

class LoadLevel(bpy.types.Operator):
    """Load a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.load_level"
    bl_label = "Load LOTRC Level"

    def execute(self, context):
        level_path = context.scene.lotrc_props.load_filepath
        LOADED_LEVELS[context.scene.name] = Level(level_path, context)
        return {'FINISHED'}

class DumpLevel(bpy.types.Operator):
    """Dump a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.dump_level"
    bl_label = "Dump LOTRC Level"

    def execute(self, context):
        level_path = context.scene.lotrc_props.dump_filepath
        LOADED_LEVELS[context.scene.name].dump(level_path)
        return {'FINISHED'}

CLASSES = [LoadLevel, DumpLevel]
GEOM_TREES = {}
LOADED_LEVELS = {}

def new_geom_tree(name, *inputs):
    tree = bpy.data.node_groups.new(name,'GeometryNodeTree')
    tree.use_fake_user = True
    tree_in = tree.nodes.new('NodeGroupInput')
    tree_out = tree.nodes.new('NodeGroupOutput')
    tree.interface.new_socket('Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    for name, ty in inputs:
        tree.interface.new_socket(name, in_out='INPUT', socket_type=ty)
    return tree, tree_in, tree_out

def create_geom_trees():
    trees = {}
    
    name = 'Box.COLLISION'
    tree, tree_in, tree_out = new_geom_tree(name,
        ('half_extents', 'NodeSocketVector')
    )
    cube = tree.nodes.new('GeometryNodeMeshCube')
    scale = tree.nodes.new('ShaderNodeVectorMath')
    scale.operation = 'SCALE'
    scale.inputs['Scale'].default_value = 2
    tree.links.new(tree_in.outputs['half_extents'], scale.inputs['Vector'])
    tree.links.new(scale.outputs['Vector'], cube.inputs['Size'])
    tree.links.new(cube.outputs['Mesh'], tree_out.inputs['Geometry'])
    
    trees[name] = (tree, tree_in, tree_out)
    
    name = 'Sphere.COLLISION'
    tree, tree_in, tree_out = new_geom_tree(name,
        ('radius', 'NodeSocketFloat')
    )
    sphere = tree.nodes.new('GeometryNodeMeshUVSphere')
    tree.links.new(tree_in.outputs['radius'], sphere.inputs['Radius'])
    tree.links.new(sphere.outputs['Mesh'], tree_out.inputs['Geometry'])
    
    trees[name] = (tree, tree_in, tree_out)
    
    name = 'Cylinder.COLLISION'
    tree, tree_in, tree_out = new_geom_tree(name,
        ('point1', 'NodeSocketVector'),
        ('point2', 'NodeSocketVector'),
        ('radius', 'NodeSocketFloat'),
    )
    cylinder = tree.nodes.new('GeometryNodeMeshCylinder')
    transform = tree.nodes.new('GeometryNodeTransform')
    depth = tree.nodes.new('ShaderNodeVectorMath')
    depth.operation = 'DISTANCE'
    scale = tree.nodes.new('ShaderNodeVectorMath')
    scale.operation = 'SCALE'
    scale.inputs['Scale'].default_value = 0.5
    rotation = tree.nodes.new('FunctionNodeAlignRotationToVector')
    rot_vec = tree.nodes.new('ShaderNodeVectorMath')
    rot_vec.operation = 'SUBTRACT'
    depth_vec = tree.nodes.new('ShaderNodeCombineXYZ')
    translation = tree.nodes.new('ShaderNodeVectorMath')
    translation.operation = 'ADD'
    translation_rot = tree.nodes.new('FunctionNodeRotateVector')
    
    tree.links.new(tree_in.outputs['point1'], depth.inputs[0])
    tree.links.new(tree_in.outputs['point2'], depth.inputs[1])
    tree.links.new(depth.outputs['Value'], cylinder.inputs['Depth'])
    tree.links.new(tree_in.outputs['radius'], cylinder.inputs['Radius'])
    tree.links.new(tree_in.outputs['point1'], rot_vec.inputs[1])
    tree.links.new(tree_in.outputs['point2'], rot_vec.inputs[0])
    tree.links.new(rot_vec.outputs['Vector'], rotation.inputs['Vector'])
    tree.links.new(depth.outputs['Value'], depth_vec.inputs['Z'])
    tree.links.new(depth_vec.outputs['Vector'], scale.inputs['Vector'])
    tree.links.new(scale.outputs['Vector'], translation_rot.inputs['Vector'])
    tree.links.new(rotation.outputs['Rotation'], translation_rot.inputs['Rotation'])
    tree.links.new(tree_in.outputs['point1'], translation.inputs[0])
    tree.links.new(translation_rot.outputs['Vector'], translation.inputs[1])
    tree.links.new(cylinder.outputs['Mesh'], transform.inputs['Geometry'])
    tree.links.new(translation.outputs['Vector'], transform.inputs['Translation'])
    tree.links.new(rotation.outputs['Rotation'], transform.inputs['Rotation'])
    tree.links.new(transform.outputs['Geometry'], tree_out.inputs['Geometry'])
    
    trees[name] = (tree, tree_in, tree_out)
    
    name = 'Capsule.COLLISION'
    tree, tree_in, tree_out = new_geom_tree(name,
        ('point1', 'NodeSocketVector'),
        ('point2', 'NodeSocketVector'),
        ('radius', 'NodeSocketFloat'),
    )
    cylinder = tree.nodes.new('GeometryNodeMeshCylinder')
    transform = tree.nodes.new('GeometryNodeTransform')
    depth = tree.nodes.new('ShaderNodeVectorMath')
    depth.operation = 'DISTANCE'
    scale = tree.nodes.new('ShaderNodeVectorMath')
    scale.operation = 'SCALE'
    scale.inputs['Scale'].default_value = 0.5
    rotation = tree.nodes.new('FunctionNodeAlignRotationToVector')
    rot_vec = tree.nodes.new('ShaderNodeVectorMath')
    rot_vec.operation = 'SUBTRACT'
    depth_vec = tree.nodes.new('ShaderNodeCombineXYZ')
    translation = tree.nodes.new('ShaderNodeVectorMath')
    translation.operation = 'ADD'
    translation_rot = tree.nodes.new('FunctionNodeRotateVector')
    sphere = tree.nodes.new('GeometryNodeMeshUVSphere')
    sphere1 = tree.nodes.new('GeometryNodeTransform')
    sphere2 = tree.nodes.new('GeometryNodeTransform')
    join = tree.nodes.new('GeometryNodeMeshBoolean')
    join.operation = 'UNION'
    join.solver = 'EXACT'
    
    tree.links.new(tree_in.outputs['point1'], depth.inputs[0])
    tree.links.new(tree_in.outputs['point2'], depth.inputs[1])
    tree.links.new(depth.outputs['Value'], cylinder.inputs['Depth'])
    tree.links.new(tree_in.outputs['radius'], cylinder.inputs['Radius'])
    tree.links.new(tree_in.outputs['point1'], rot_vec.inputs[1])
    tree.links.new(tree_in.outputs['point2'], rot_vec.inputs[0])
    tree.links.new(rot_vec.outputs['Vector'], rotation.inputs['Vector'])
    tree.links.new(depth.outputs['Value'], depth_vec.inputs['Z'])
    tree.links.new(depth_vec.outputs['Vector'], scale.inputs['Vector'])
    tree.links.new(scale.outputs['Vector'], translation_rot.inputs['Vector'])
    tree.links.new(rotation.outputs['Rotation'], translation_rot.inputs['Rotation'])
    tree.links.new(tree_in.outputs['point1'], translation.inputs[0])
    tree.links.new(translation_rot.outputs['Vector'], translation.inputs[1])
    tree.links.new(cylinder.outputs['Mesh'], transform.inputs['Geometry'])
    tree.links.new(translation.outputs['Vector'], transform.inputs['Translation'])
    tree.links.new(rotation.outputs['Rotation'], transform.inputs['Rotation'])
    tree.links.new(tree_in.outputs['radius'], sphere.inputs['Radius'])
    tree.links.new(sphere.outputs['Mesh'], sphere1.inputs['Geometry'])
    tree.links.new(sphere.outputs['Mesh'], sphere2.inputs['Geometry'])
    tree.links.new(tree_in.outputs['point1'], sphere1.inputs['Translation'])
    tree.links.new(tree_in.outputs['point2'], sphere2.inputs['Translation'])
    tree.links.new(rotation.outputs['Rotation'], sphere1.inputs['Rotation'])
    tree.links.new(rotation.outputs['Rotation'], sphere2.inputs['Rotation'])
    tree.links.new(transform.outputs['Geometry'], join.inputs[1])
    tree.links.new(sphere1.outputs['Geometry'], join.inputs[1])
    tree.links.new(sphere2.outputs['Geometry'], join.inputs[1])
    tree.links.new(join.outputs['Mesh'], tree_out.inputs['Geometry'])
    
    trees[name] = (tree, tree_in, tree_out)
    
    name = 'Billboard'
    tree, tree_in, tree_out = new_geom_tree(name,
        ('Geometry', 'NodeSocketGeometry'),
    )
    psize = tree.nodes.new('GeometryNodeInputNamedAttribute')
    psize.inputs['Name'].default_value = 'PSize'
    psize.data_type = 'FLOAT_VECTOR'
    pos_set = tree.nodes.new('GeometryNodeSetPosition')
    tree.links.new(tree_in.outputs['Geometry'], pos_set.inputs['Geometry'])
    tree.links.new(psize.outputs['Attribute'], pos_set.inputs['Offset'])
    tree.links.new(pos_set.outputs['Geometry'], tree_out.inputs['Geometry'])
    
    trees[name] = (tree, tree_in, tree_out)
    
    return trees

def model_enum(_, context):
    items = [('All Models', 'All Models', '')]
    if context.scene.name in LOADED_LEVELS:
        items.extend([(i, i, '') for i in LOADED_LEVELS[context.scene.name].model_names])
    return items

def texture_enum(_, context):
    items = [('All Textures', 'All Textures', '')]
    if context.scene.name in LOADED_LEVELS:
        items.extend([(i, i, '') for i in LOADED_LEVELS[context.scene.name].texture_names])
    return items

class Level:
    def __init__(self, path, context):
        self.level = lotrc.level_alt.Level.load(path)
        #self.name = pathlib.Path(path).stem
        self.col = bpy.data.collections.new(pathlib.Path(path).stem)
        self.models_col = None
        self.level_col = None
        self.textures = {}
        self.models = {}
        self.types = {}
        self.model_names = list(self.level.models.keys())
        self.texture_names = list(self.level.textures.keys())
        context.scene.collection.children.link(self.col)
        if GEOM_TREES == {}:
            GEOM_TREES.update(create_geom_trees())

    def dump(self, path):
        print(str(pathlib.Path(path).joinpath(self.col.name)))
        self.level.dump_pc(str(pathlib.Path(path).joinpath(self.col.name)))
