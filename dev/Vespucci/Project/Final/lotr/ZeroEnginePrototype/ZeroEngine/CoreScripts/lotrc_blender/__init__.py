import bpy
from . import ui, loader
from .imp import level, model, textures

CLASSES = ui.CLASSES + loader.CLASSES + level.CLASSES + model.CLASSES + textures.CLASSES
from .exp import level

CLASSES += level.CLASSES

# things to add / fix
#   - bow / banner mesh (second vbuff thing)
#   - visualizations for some stuff (spline, world_bbox, trigger_box/sphere)


def enum_callback(self, context):
    return [(i, i, '') for i in context.view_layer.active_layer_collection.collection['lotrc']]

def enum_update(self, context):
    self.object_value = context.view_layer.active_layer_collection.collection['lotrc'][self.object_key]
    
def value_update(self, context):
    context.view_layer.active_layer_collection.collection['lotrc'][self.object_key] = self.object_value

class LotrcProps(bpy.types.PropertyGroup):
    load_filepath: bpy.props.StringProperty(subtype="FILE_PATH")
    dump_filepath: bpy.props.StringProperty(subtype="FILE_PATH")
    selected_model: bpy.props.StringProperty(default="All Models")
    models_collision: bpy.props.BoolProperty(default=True)
    models_only_lod1: bpy.props.BoolProperty(default=False)
    models_skeleton: bpy.props.BoolProperty(default=True)
    models_hk_skeleton: bpy.props.BoolProperty(default=True)
    selected_texture: bpy.props.StringProperty(default="All Textures")
    object_value: bpy.props.StringProperty(
        update=value_update
    )
    object_key: bpy.props.EnumProperty(
        items=enum_callback,
        update=enum_update,
    )

CLASSES.append(LotrcProps)

def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.lotrc_props = bpy.props.PointerProperty(type=LotrcProps)

def unregister():
    for cls in CLASSES:
        bpy.utils.unregister_class(cls)
    #del bpy.types.Scene.lotrc_props
