from bpy_extras.io_utils import ImportHelper
import bpy
from .loader import LOADED_LEVELS, model_enum, texture_enum

class PanelBase(bpy.types.Panel):
    bl_idname = "lotrc.panel_base"
    bl_label = "Base Panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "LOTRC"

class ModelSelectOperator(bpy.types.Operator):
    bl_idname = "lotrc.model_select"
    bl_label = "Model Select Operator"
    bl_property = "model"

    model: bpy.props.EnumProperty(items=model_enum)

    def execute(self, context):
        context.scene.lotrc_props.selected_model = self.model
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {'FINISHED'}

class TextureSelectOperator(bpy.types.Operator):
    bl_idname = "lotrc.texture_select"
    bl_label = "Texture Select Operator"
    bl_property = "texture"

    texture: bpy.props.EnumProperty(items=texture_enum)

    def execute(self, context):
        context.scene.lotrc_props.selected_texture = self.texture
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {'FINISHED'}

class LoadPanel(PanelBase):
    bl_idname = "lotrc.load_panel"
    bl_label = "Load LOTRC Objects"

    def draw(self, context):
        props = context.scene.lotrc_props
        self.layout.prop(props, 'load_filepath', text='filepath')
        row = self.layout.row()
        row.enabled = props.load_filepath != ''
        row.operator('lotrc.load_level', text='Load Level')
        if context.scene.name in LOADED_LEVELS:
            (header, box) = self.layout.panel('load_textures')
            header.label(text='Textures')
            if box is not None:
                box.operator('lotrc.clear_textures', text='Clear')
                box.operator('lotrc.texture_select', text=f'Select Texture')
                box.operator('lotrc.load_textures', text=f'Load: {props.selected_texture}')
            (header, box) = self.layout.panel('load_models')
            header.label(text='Models')
            if box is not None:
                box.operator('lotrc.clear_models', text='Clear')
                box.operator('lotrc.model_select', text=f'Select Model')
                box.prop(props, 'models_only_lod1', text='Load Only LOD1')
                box.prop(props, 'models_skeleton', text='Load Skeleton')
                row = box.row()
                row.enabled = props.models_skeleton
                row.prop(props, 'models_collision', text='Load Collision')
                box.prop(props, 'models_hk_skeleton', text='Load HkSkeleton')
                box.operator('lotrc.load_models', text=f'Load: {props.selected_model}')
            (header, box) = self.layout.panel('load_level_block')
            header.label(text='Level Block')
            if box is not None:
                box.operator('lotrc.clear_level_block', text='Clear')
                box.operator('lotrc.load_level_block', text='Load')

class DumpPanel(PanelBase):
    bl_idname = "lotrc.dump_panel"
    bl_label = "Dump LOTRC Objects"

    @classmethod
    def poll(cls, context):
        return context.scene.name in LOADED_LEVELS

    def draw(self, context):
        props = context.scene.lotrc_props
        self.layout.prop(props, 'dump_filepath', text='folder')
        row = self.layout.row()
        row.enabled = props.dump_filepath != ''
        row.operator('lotrc.dump_level', text='Dump Level')

        (header, box) = self.layout.panel('dump_level_block')
        header.label(text='Level Block')
        if box is not None:
            box.operator('lotrc.dump_level_block', text='Dump')

class ValuePanel(PanelBase):
    bl_idname = "lotrc.value_panel"
    bl_label = "LOTRC Value Panel"
    
    @classmethod
    def poll(cls, context):
        return '__lotrc__' in context.view_layer.active_layer_collection.collection

    def draw(self, context):
        for key in context.view_layer.active_layer_collection.collection['__lotrc__']:
            self.layout.prop(context.view_layer.active_layer_collection.collection, f'["{key}"]', text=key)

CLASSES = [
    ValuePanel,
    LoadPanel, 
    DumpPanel,
    ModelSelectOperator,
    TextureSelectOperator,
]
