import bpy
import pathlib
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from ..loader import LOADED_LEVELS

class LoadTextures(bpy.types.Operator):
    """Load Textures from a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.load_textures"
    bl_label = "Load LOTRC Textures"

    def execute(self, context):
        texture = context.scene.lotrc_props.selected_texture
        import_textures(LOADED_LEVELS[context.scene.name], texture)
        return {'FINISHED'}

class ClearTextures(bpy.types.Operator):
    """Delete Previously Loaded Textures from a Lord of the Rings Conquest Level"""
    bl_idname = "lotrc.clear_textures"
    bl_label = "Clear Loaded LOTRC Textures"

    def execute(self, context):
        level = LOADED_LEVELS[context.scene.name]
        for img in level.textures.values():
            img.use_fake_user = False
        level.textures = {}
        bpy.ops.outliner.orphans_purge()
        return {'FINISHED'}

CLASSES = [LoadTextures, ClearTextures]

def import_textures(level, texture):
    textures = level.level.textures
    if texture == 'All Textures':
        pass
    elif texture in textures:
        textures = {texture: textures[texture]}
    else:
        textures = {}

    for name, texture in textures.items():
        (data, h, w, f) = texture.get_img(0)
        if f == 6:
            data = np.frombuffer(bytes(h*w*3) + data, 'B').reshape(4,h,w).transpose(1,2,0)
        else:
            data = np.frombuffer(data, 'B')
        data = data.flatten().astype(np.float32)/255.0
        img = bpy.data.images.new(name, w, h)
        img.pixels.foreach_set(data)
        img.update()
        level.textures[name] = img
        img.use_fake_user = True
        
