use std::{collections::HashMap, fmt::Display, iter::zip, num::ParseIntError, ops::Div, str::FromStr};
use std::sync::Mutex;
use log::warn;
use serde::{Serialize, Deserialize};
use serde_with::{SerializeDisplay, DeserializeFromStr};
use serde_with::serde_as;
use anyhow::Result;
use pyo3::prelude::*;
use pyo3::exceptions::PyTypeError;
use pyo3::types::PyDict;
use itertools::Itertools;

use lotrc_proc::{OrderedData, basicpymethods, PyMethods};
use crate::{
    pak_alt::GltfData,
    types::{
        BaseTypes, Color, Vector4, Matrix4x4, Vector2, AsData, NoArgs,
        Crc, Vector3, Version, PC, from_bytes, to_bytes, dump_bytes
    }
};

pub mod animation;
pub use animation::*;

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Header {
    #[ordered_data(PC)]
    pub block_a_num: u32, 
    #[ordered_data(PC)]
    pub block_a_offset: u32, 
    pub constx13: u32, 
    pub version: u32, 
    pub strings_offset: u32, 
    pub strings_size: u32, 
    pub strings_num: u32, 
    pub block1_offset: u32, 
    pub block1_size: u32, 
    pub block1_size_comp: u32, 
    pub sub_blocks1_offset: u32, 
    pub block2_offset: u32, 
    pub block2_size: u32, 
    pub block2_size_comp: u32, 
    pub sub_blocks2_offset: u32, 
    pub string_keys_offset: u32, 
    pub unk_16: u32, 
    pub obja_size: u32, 
    pub obj0_size: u32, 
    pub model_info_size: u32, 
    pub buffer_info_size: u32, 
    pub mat1_size: u32, 
    pub mat2_size: u32, 
    pub mat3_size: u32, 
    pub mat4_size: u32, 
    pub mat_extra_size: u32, 
    pub unk_26: u32, 
    pub shape_info_size: u32, 
    pub hk_shape_info_size: u32, 
    pub hk_constraint_data_size: u32, 
    pub vbuff_info_size: u32, 
    pub ibuff_info_size: u32, 
    pub texture_info_size: u32, 
    pub animation_info_size: u32,
    pub hk_constraint_info_size: u32,
    pub effect_info_size: u32,
    pub pfield_info_size: u32,
    pub gfx_block_info_size: u32,
    pub animation_block_info_size: u32, 
    pub foliage_info_size: u32, 
    pub radiosity_vals_info_size: u32,
    pub unk_41: u32, 
    pub obja_num: u32, 
    pub obj0_num: u32, 
    pub model_info_num: u32,
    pub buffer_info_num: u32,
    pub mat1_num: u32, 
    pub mat2_num: u32, 
    pub mat3_num: u32, 
    pub mat4_num: u32, 
    pub mat_extra_num: u32, 
    pub unk_51: u32, 
    pub shape_info_num: u32, 
    pub hk_shape_info_num: u32,
    pub hk_constraint_data_num: u32,
    pub vbuff_info_num: u32,
    pub ibuff_info_num: u32,
    pub texture_info_num: u32,
    pub animation_info_num: u32,
    pub hk_constraint_info_num: u32,
    pub effect_info_num: u32,
    pub pfield_info_num: u32,
    pub gfx_block_info_num: u32, 
    pub animation_block_info_num: u32, 
    pub foliage_info_num: u32, 
    pub radiosity_vals_info_num: u32, 
    pub unk_66: u32, 
    pub obja_offset: u32,
    pub obj0_offset: u32, 
    pub model_info_offset: u32,  // max loaded is 0x400
    pub buffer_info_offset: u32, 
    pub mat1_offset: u32, 
    pub mat2_offset: u32, 
    pub mat3_offset: u32, 
    pub mat4_offset: u32, 
    pub mat_extra_offset: u32, 
    pub unk_76: u32, 
    pub shape_info_offset: u32, 
    pub hk_shape_info_offset: u32, 
    pub hk_constraint_data_offset: u32, 
    pub vbuff_info_offset: u32, 
    pub ibuff_info_offset: u32, 
    pub texture_info_offset: u32,  // max loaded is 0x800
    pub animation_info_offset: u32, 
    pub hk_constraint_info_offset: u32, 
    pub effect_info_offset: u32, 
    pub pfield_info_offset: u32, 
    pub gfx_block_info_offset: u32,  // max loaded is 0x40
    pub animation_block_info_offset: u32,
    pub foliage_info_offset: u32, 
    pub radiosity_vals_info_offset: u32, 
    pub unk_91: u32, 
    pub unk_92: u32, 
    pub unk_93: u32, 
    pub unk_94: u32, 
    pub unk_95: u32, 
    pub unk_96: u32, 
    pub unk_97: u32, 
    pub unk_98: u32, 
    pub unk_99: u32, 
    pub unk_100: u32, 
    pub unk_101: u32, 
    pub unk_102: u32, 
    pub unk_103: u32, 
    pub unk_104: u32, 
    pub unk_105: u32, 
    pub unk_106: u32, 
    pub unk_107: u32, 
    pub unk_108: u32, 
    pub unk_109: u32, 
    pub unk_110: u32, 
    pub unk_111: u32, 
    pub unk_112: u32, 
    pub unk_113: u32, 
    pub unk_114: u32, 
    pub unk_115: u32, 
    pub block2_offsets_num: u32, 
    pub block2_offsets_offset: u32, 
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct ObjA {
    #[ordered_data(PC)]
    pub key: Crc,
    #[ordered_data(PC)]
    pub unk_1: u32,
    #[ordered_data(PC)]
    pub size: u32,
    #[ordered_data(PC)]
    pub size_comp: u32,
    #[ordered_data(PC)]
    pub unk_4: u32,
    #[ordered_data(PC)]
    pub kind: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Obj0 {
    #[ordered_data(PC)]
    pub unk_0: u32,
    #[ordered_data(PC)]
    pub key: Crc,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct LodMeshes {
    pub start: u32,
    pub static_end: u32,
    pub skinned_end: u32,
    pub physics_end: u32,
    pub breakable_end: u32,
}

impl LodMeshes {
    pub const UNKNOWN: u32 = 1;
    pub const STATIC: u32 = 2;
    pub const SKINNED: u32 = 4;
    pub const PHYSICS: u32 = 8;
    pub const BREAKABLE: u32 = 16;
    pub const LOD0: u32 = 32;
    pub const LOD1: u32 = 64;
    pub const LOD2: u32 = 128;
    pub const LOD3: u32 = 256;
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, OrderedData, PyMethods)]
pub struct BoundingBox {
    center: Vector3,
    unk_3: f32,
    half_width: Vector3,
    unk_7: f32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct ModelInfo {
    pub key: Crc,
    pub gamemodemask: i32,
    pub mat_offset: u32,
    pub buffer_info_offset: u32, // pointer to obj2, uses mat_num of sequential objects
    pub bounding_box: BoundingBox, // (center x, y, z, ?, half_width x, y, z, ?) default vals of 1.0 for the ? vals seems to work
    pub mesh_order_offset: u32, // ints (c & 0x3fffffff is an index into the obj2s referenced by this object) (1 for each mesh)
    pub lod0: LodMeshes,
    pub lod1: LodMeshes,
    pub lod2: LodMeshes,
    pub lod3: LodMeshes,
    pub mat_num: u32,
    pub bones_offset: u32, // ints
    pub bone_parents_offset: u32,
    pub bone_transforms_offset: u32, // 16 ints (matrix?) for keys_num
    pub bones_num: u32,
    pub skin_binds_offset: u32,
    pub skin_binds_num: u32,
    pub skin_order_offset: u32,
    pub vbuff_offset: u32,
    pub vbuff_num: u32,
    pub ibuff_offset: u32,
    pub ibuff_num: u32,
    pub mesh_bounding_boxes_offset: u32,
    pub unk_46: f32, // maybe something to do with size? 
    pub variation_counts: u32, // maybe something to do with variation
    pub vals_j_num: u32,
    pub vals_j_offset: u32,
    pub block_offset: u32,
    pub vals_k_offset: u32, // not sure on the size, seems to be 36 ints
    pub asset_key: Crc, // data in bin that is vertex & index buffer values
    pub asset_type: u32,
    pub unk_54: u32, // 1 for occuluder otherwise 0 ??
    #[name_ps3(shape_offset)]
    pub unk_55: u32, // always 0 ??
    #[name_ps3(shape_num)]
    pub shape_offset: u32,
    #[name_ps3(hk_constraint_data_offset)]
    pub shape_num: u32,
    #[name_ps3(hk_constraint_data_num)]
    pub hk_constraint_data_offset: u32, // optional pointer to obje
    #[name_ps3(hk_constraint_offset)]
    pub hk_constraint_data_num: u32,
    #[name_ps3(slots_offset)]
    pub hk_constraint_offset: u32, // optional pointer to hkConstraint
    #[name_ps3(slot_map_offset)]
    pub slots_offset: u32,
    #[name_ps3(bone_bounding_boxes_offset)]
    pub slot_map_offset: u32,
    #[name_ps3(unk_55)]
    pub bone_bounding_boxes_offset: u32, // 8 ints
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct BufferInfo {
    pub vbuff_info_offset: u32, // pointer to objf
    pub vbuff_info_offset_2: u32, // optional pointer to objf
    pub vbuff_info_offset_3: u32, // optional pointer to objf
    pub unk_3: u32,
    pub unk_4: u32,
    pub unk_5: u32,
    pub unk_6: u32,
    pub unk_7: u32,
    pub unk_8: u32,
    pub unk_9: u32,
    pub unk_10: u32,
    pub unk_11: u32,
    pub unk_12: u32,
    pub unk_13: u32,
    pub unk_14: u32,
    pub unk_15: u32,
    pub unk_16: u32,
    pub unk_17: u32,
    pub unk_18: u32,
    pub unk_19: u32,
    #[ordered_data(skipPS3)]
    pub unk_20: u32,
    #[ordered_data(skipPS3)]
    pub unk_21: u32,
    #[ordered_data(skipPS3)]
    pub unk_22: u32,
    #[ordered_data(skipPS3)]
    pub unk_23: u32,
    #[ordered_data(skipPS3)]
    pub unk_24: u32,
    #[ordered_data(skipPS3)]
    pub unk_25: u32,
    #[ordered_data(skipPS3)]
    pub unk_26: u32,
    #[ordered_data(skipPS3)]
    pub unk_27: u32,
    #[ordered_data(skipPS3)]
    pub unk_28: u32,
    #[ordered_data(skipPS3)]
    pub unk_29: u32,
    #[ordered_data(skipPS3)]
    pub unk_30: u32,
    #[ordered_data(skipPS3)]
    pub unk_31: u32,
    #[ordered_data(skipPS3)]
    pub v_size: u32,
    #[ordered_data(skipPS3)]
    pub v_size_2: u32,
    #[ordered_data(skipPS3)]
    pub v_size_3: u32,
    #[ordered_data(skipPS3)]
    pub unk_35: u32,
    #[ordered_data(skipPS3)]
    pub unk_36: u32,
    #[ordered_data(skipPS3)]
    pub unk_37: u32,
    #[ordered_data(skipPS3)]
    pub unk_38: u32,
    #[ordered_data(skipPS3)]
    pub unk_39: u32,
    #[ordered_data(skipPS3)]
    pub unk_40: u32,
    #[ordered_data(skipPS3)]
    pub unk_41: u32,
    #[ordered_data(skipPS3)]
    pub unk_42: u32,
    #[ordered_data(skipPS3)]
    pub unk_43: u32,
    #[ordered_data(skipPS3)]
    pub unk_44: u32,
    #[ordered_data(skipPS3)]
    pub unk_45: u32,
    #[ordered_data(skipPS3)]
    pub unk_46: u32,
    #[ordered_data(skipPS3)]
    pub unk_47: u32,
    #[ordered_data(skipPS3)]
    pub vbuff_size: u32,
    #[ordered_data(skipPS3)]
    pub vbuff_size_2: u32,
    #[ordered_data(skipPS3)]
    pub vbuff_size_3: u32,
    #[ordered_data(skipPS3)]
    pub unk_51: u32,
    #[ordered_data(skipPS3)]
    pub unk_52: u32,
    #[ordered_data(skipPS3)]
    pub unk_53: u32,
    #[ordered_data(skipPS3)]
    pub unk_54: u32,
    #[ordered_data(skipPS3)]
    pub unk_55: u32,
    #[ordered_data(skipPS3)]
    pub unk_56: u32,
    #[ordered_data(skipPS3)]
    pub unk_57: u32,
    #[ordered_data(skipPS3)]
    pub unk_58: u32,
    #[ordered_data(skipPS3)]
    pub unk_59: u32,
    #[ordered_data(skipPS3)]
    pub unk_60: u32,
    #[ordered_data(skipPS3)]
    pub unk_61: u32,
    #[ordered_data(skipPS3)]
    pub unk_62: u32,
    #[ordered_data(skipPS3)]
    pub unk_63: u32,
    #[ordered_data(skipPS3)]
    pub unk_64: u32,
    #[ordered_data(skipPS3)]
    pub ibuff_info_offset: u32, // poiner to objg
    #[ordered_data(skipPS3)]
    pub i_num: u32, // number of indeices in ibuffer
    #[ordered_data(skipPS3)]
    pub unk_67: u32,
    #[ordered_data(skipPS3)]
    pub skin_offset: u32,
    #[ordered_data(skipPS3)]
    pub skin_size: u32,
    #[ordered_data(skipPS3)]
    pub unk_70: u32,
    #[ordered_data(skipPS3)]
    pub tri_num: u32, // number of objects(triangles) in ibufffer
    #[ordered_data(skipPS3)]
    pub unk_72: u32, // possibly index to bone_transform used for mesh
    #[ordered_data(skipPS3)]
    pub unk_73: u32,
    #[ordered_data(skipPS3)]
    pub unk_74: u32,
    #[ordered_data(skipPS3)]
    pub unk_75: u32,
    #[ordered_data(skipPS3)]
    pub unk_76: u32,
    #[ordered_data(skipPS3)]
    pub unk_77: u32,
    #[ordered_data(skipPS3)]
    pub unk_78: u32,
    #[ordered_data(skipPS3)]
    pub unk_79: u32,
    #[ordered_data(skipPS3)]
    pub unk_80: u32,
    #[ordered_data(skipPS3)]
    pub unk_81: u32,
    #[ordered_data(skipPS3)]
    pub unk_82: u32,
    #[ordered_data(skipPS3)]
    pub unk_83: u32,
    #[ordered_data(skipPS3)]
    pub unk_84: u32,
    #[ordered_data(skipPS3)]
    pub unk_85: u32,
    #[ordered_data(skipPS3)]
    pub unk_86: u32,
    #[ordered_data(skipPS3)]
    pub unk_87: u32,
    #[ordered_data(skipPS3)]
    pub variation_id: u8,
    #[ordered_data(skipPS3)]
    pub variation: u8,
    #[ordered_data(skipPS3)]
    pub unk_88c: u8,
    #[ordered_data(skipPS3)]
    pub unk_88d: u8,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct MatBase {
    pub unk_0: u32,
    #[ordered_data(skipPS3)]
    pub unk_1: u32,
    pub tex0: Crc,
    pub tex1: Crc,
    pub tex2: Crc,
    pub tex3: Crc,
    pub tex4: Crc,
    pub tex5: Crc,
    pub key_guid: Crc,
    pub mask0: Crc,
    pub mask1: Crc,
    pub mask2: Crc,
    pub unk_12: u32,
    pub unk_13: u32,
    pub unk_14: u32,
    pub unk_15: u32,
    pub unk_16: u32,
    pub unk_17: u32,
    pub unk_18: u32,
    pub unk_19: u32,
    pub unk_20: u32,
    pub unk_21: u32,
    pub unk_22: u32,
    pub unk_23: u32,
    pub unk_24: u32,
    pub unk_25: u32,
    pub unk_26: u32,
    pub unk_27: u32,
    pub unk_28: u32,
    pub unk_29: u32,
    pub unk_30: u32,
    pub unk_31: u32,
    pub unk_32: u32,
    pub unk_33: u32,
    #[ordered_data(skipPS3)]
    pub z_34: u32,
    pub z_35: u32,
    pub z_36: u32,
    pub z_37: u32,
    pub z_38: u32,
    pub z_39: u32,
    pub unk_40: u32,
    pub unk_41: u32,
    pub unk_42: u32,
    pub unk_43: u32,
    pub unk_44: u32,
    pub unk_45: u32,
    pub unk_46: u32,
    pub unk_47: u32,
    pub unk_48: u32,
    pub unk_49: u32,
    pub flags: u64, //Q', #(flags1, flags2)
    pub kind: u32,
    pub unk_53: u32,
    pub unk_54a: u8,
    pub unk_54b: u8,
    pub side_flags: u16,
    pub unk_55: u32,
    pub unk_56: u32,
    pub unk_57: u32,
    pub unk_58: u32,
    pub unk_59: u32,
    pub unk_60: u32,
    pub unk_61: u32,
    pub unk_62: u32,
    pub unk_63: u32,
    pub unk_64: u32,
    pub unk_65: u32,
    pub unk_66: u32,
    pub unk_67: u32,
    pub unk_68: u32,
    pub unk_69: u32,
    pub unk_70: u32,
    pub unk_71: u32,
    pub unk_72: u32,
    pub unk_73: u32,
    pub unk_74: u32,
    pub unk_75: u32,
    pub unk_76: u32,
    pub unk_77: u32,
    pub unk_78: u32,
    pub unk_79: u32,
    pub unk_80: u32,
    pub unk_81: u32,
    pub unk_82: u32,
    pub unk_83: u32,
    pub unk_84: u32,
    pub unk_85: u32,
    pub mat_extra_offset: u32,
    pub key: Crc,
    pub unk_88: u32,
    pub z_89: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Mat1 {
    pub base: MatBase,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Mat2 {
    pub base: MatBase,
    pub unk_90: u32,
    pub unk_91: u32,
    pub unk_92: u32,
    pub unk_93: u32,
    pub unk_94: u32,
    pub unk_95: u32,
    pub unk_96: u32,
    pub unk_97: u32,
    pub unk_98: u32,
    pub unk_99: u32,
    pub unk_100: u32,
    pub unk_101: u32,
    pub unk_102: u32,
    pub unk_103: u32,
    pub unk_104: u32,
    pub unk_105: u32,
    pub unk_106: u32,
    pub unk_107: u32,
    pub unk_108: u32,
    pub unk_109: u32,
    pub unk_110: u32,
    pub unk_111: u32,
    pub unk_112: u32,
    pub unk_113: u32,
    pub unk_114: u32,
    pub unk_115: u32,
    pub unk_116: u32,
    pub unk_117: u32,
    pub unk_118: u32,
    pub unk_119: u32,
    pub unk_120a: u8,
    pub unk_120b: u8,
    pub unk_120c: u8,
    pub unk_120d: u8,
    pub unk_121: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Mat3 {
    pub base: MatBase,
    pub unk_90: u32,
    pub unk_91: u32,
    pub unk_92: u32,
    pub unk_93: u32,
    pub unk_94: u32,
    pub unk_95: u32,
    pub unk_96: u32,
    pub unk_97: u32,
    pub unk_98: u32,
    pub unk_99: u32,
    pub unk_100: u32,
    pub unk_101: u32,
    pub unk_102: u32,
    pub unk_103: u32,
    pub unk_104: u32,
    pub unk_105: u32,
    pub unk_106: u32,
    pub unk_107: u32,
    pub unk_108: u32,
    pub unk_109: u32,
    pub unk_110: u32,
    pub unk_111: u32,
    pub unk_112: u32,
    pub unk_113: u32,
    pub variation_id_color: u8,
    pub variation_id_texture: u8,
    pub variation_id_specular: u8,
    pub unk_114d: u8,
    pub unk_115: u32,
    #[ordered_data(skipPC, skipXBOX)]
    #[serde(default)]
    pub unk_116: u32,
    #[ordered_data(skipPC, skipXBOX)]
    #[serde(default)]
    pub unk_117: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Mat4 {
    pub base: MatBase,
    pub unk_90: u32,
    pub unk_91: u32,
    pub unk_92: u32,
    pub unk_93: u32,
    pub unk_94: u32,
    pub unk_95: u32,
    pub unk_96: u32,
    pub unk_97: u32,
    pub unk_98: u32,
    pub unk_99: u32,
    pub unk_100: u32,
    pub unk_101: u32,
    pub unk_102: u32,
    pub unk_103: u32,
    pub unk_104: u32,
    pub unk_105: u32,
    pub unk_106: u32,
    pub unk_107: u32,
    pub unk_108: u32,
    pub unk_109: u32,
    pub unk_110: u32,
    pub unk_111: u32,
    pub unk_112: u32,
    pub unk_113: u32,
    pub unk_114: u32,
    pub unk_115: u32,
    pub unk_116: u32,
    pub unk_117: u32,
    pub unk_118: u32,
    pub unk_119: u32,
    pub unk_120: u32,
    pub unk_121: u32,
    pub unk_122: u32,
    pub unk_123: u32,
    pub unk_124: u32,
    pub unk_125: u32,
    pub unk_126: u32,
    pub unk_127: u32,
    pub unk_128: u32,
    pub unk_129: u32,
    pub unk_130: u32,
    pub unk_131: u32,
    pub unk_132: u32,
    pub unk_133: u32,
    pub unk_134: u32,
    pub unk_135: u32,
    pub unk_136: u32,
    pub unk_137: u32,
    pub unk_138: u32,
    pub unk_139: u32,
    pub unk_140: u32,
    pub unk_141: u32,
    pub unk_142: u32,
    pub unk_143: u32,
    pub unk_144: u32,
    pub unk_145: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct MatExtra {
    pub unk_0: u32,
    pub unk_1: u32,
    pub unk_2: u32,
    pub unk_3: u32,
    pub unk_4: u32,
    pub unk_5: u32,
    pub unk_6: u32,
    pub unk_7: u32,
    pub unk_8: u32,
    pub unk_9: u32,
    pub unk_10: u32,
    pub unk_11: u32,
    pub unk_12: u32,
    pub unk_13: u32,
    pub unk_14: u32,
    pub unk_15: u32,
    pub unk_16: u32,
    pub unk_17: u32,
    pub unk_18: u32,
    pub unk_19: u32,
    pub unk_20: u32,
    pub unk_21: u32,
    pub unk_22: u32,
    pub unk_23: u32,
    pub unk_24: u32,
    pub unk_25: u32,
    pub unk_26: u32,
    pub unk_27: u32,
    pub unk_28: u32,
    pub unk_29: u32,
    pub unk_30: u32,
    pub unk_31: u32,
    pub unk_32: u32,
    pub unk_33: u32,
    pub unk_34: u32,
    pub unk_35: u32,
    pub unk_36: u32,
    pub unk_37: u32,
    pub unk_38: u32,
    pub unk_39: u32,
    pub unk_40: u32,
    pub unk_41: u32,
    pub unk_42: u32,
    pub unk_43: u32,
    pub unk_44: u32,
    pub unk_45: u32,
    pub unk_46: u32,
    pub unk_47: u32,
    pub unk_48: u32,
    pub unk_49: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct ShapeInfo {
    pub offset: u32, // sometimes a pointer to something, otherwise the number of strings from the obj1 pointing to this
    pub kind: u32, // 0, 1, 2, 3, 4, 5
    pub unk_2: u32,
    pub unk_3: f32,
    pub unk_4: f32,
    pub unk_5: f32,
    pub translation: Vector3,
    pub rotation: Vector4,
    pub unk_13: f32,
    pub unk_14: f32,
    pub unk_15: f32,
    pub unk_16: f32,
    pub unk_17: f32,
    pub unk_18: f32,
    pub unk_19: f32,
    pub unk_20: f32,
    pub unk_21: f32,
    pub unk_22: f32,
    pub unk_23: f32,
    pub unk_24: f32,
    pub unk_25: f32,
    pub unk_26: f32,
    pub hk_shape_num: u32,
    pub hk_shape_offset: u32, // pointer to objd
    pub unk_29a: u8,
    pub unk_29b: u8,
    pub unk_29c: u8,
    pub unk_29d: u8,
    pub unk_30: f32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct HkShapeInfo {
    pub unk_0: Vector4,
    pub unk_4: Vector4,
    pub kind: u32,
    pub unk_9: u32,
    pub a_num: u32,
    pub a_offset: u32,
    pub b_num: u32,
    pub b_offset: u32,
    pub c_num: u32,
    pub c_offset: u32,
    pub d_num: u32,
    pub d_offset: u32,
    pub e_num: u32,
    pub e_offset: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct HkConstraintData {
    pub kind: u32,
    pub unk_1: u32,
    pub unk_2: u32,
    pub unk_3: u32,
    pub unk_4: u32,
    pub unk_5: u32,
    pub unk_6: u32,
    pub unk_7: u32,
    pub unk_8: u32,
    pub unk_9: u32,
    pub unk_10: u32,
    pub unk_11: u32,
    pub unk_12: u32,
    pub unk_13: u32,
    pub unk_14: u32,
    pub unk_15: u32,
    pub unk_16: u32,
    pub unk_17: u32,
    pub unk_18: u32,
    pub unk_19: u32,
    pub unk_20: u32,
    pub unk_21: u32,
    pub unk_22: u32,
    pub unk_23: u32,
    pub unk_24: u32,
    pub unk_25: u32,
    pub unk_26: u32,
    pub unk_27: u32,
    pub unk_28: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct VBuffInfo {
    pub unk_0: u32,
    #[name_ps3(unk_7)]
    pub size: u32,
    pub unk_3: u32,
    #[name_ps3(unk_8)]
    pub offset: u32,
    #[name_xbox(fmt2)]
    #[name_ps3(unk_9)]
    pub fmt1: u32,
    #[name_xbox(fmt1)]
    #[name_ps3(size)]
    pub fmt2: u32,
    pub unk_6: u32,
    #[name_ps3(offset)]
    pub unk_7: u32,
    #[name_ps3(fmt2)]
    #[ordered_data(skipPC)]
    pub unk_8: u32,
    #[name_ps3(fmt1)]
    #[ordered_data(skipPC)]
    pub unk_9: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_10: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_11: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_12: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_13: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct IBuffInfo {
    pub unk_0: u32,
    #[name_ps3(unk_5)]
    pub size: u32,
    #[name_ps3(unk_6)]
    pub format: u32,
    pub vbuff_alt_fmt: u32, // 1 if vbuff.fmt1 & 0x40000 != 0 else 0; 0 for Xbox
    #[name_ps3(unk_8)]
    pub offset: u32,
    #[name_ps3(size)]
    pub unk_5: u32,
    #[name_ps3(format)]
    #[ordered_data(skipPC)]
    pub unk_6: u32,
    #[ordered_data(skipPC)]
    pub unk_7: u32,
    #[name_ps3(offset)]
    #[ordered_data(skipPC)]
    pub unk_8: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_9: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_10: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_11: u32,
    #[ordered_data(skipPC, skipPS3)]
    pub unk_12: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct TextureInfo {
    pub key: Crc,
    pub gamemodemask: i32,
    pub asset_key: Crc,
    pub asset_type: u32,
    pub kind: u32,
    pub format: u32,
    pub unk_6: u32,
    pub unk_7: u32,
    pub unk_8: u32,
    pub unk_9: u32,
    pub unk_10: u32,
    pub unk_11: u32,
    pub width: u16,
    pub height: u16,
    pub depth: u16,
    pub levels: u16,
    pub unk_16_1: u8,
    pub unk_16_2: u8,
    pub unk_16_3: u8,
    pub unk_16_4: u8,
    pub unk_16_5: u8,
    pub unk_16_6: u8,
    pub unk_16_7: u8,
    pub unk_16_8: u8,
    pub unk_16_9: u8,
    pub unk_16_10: u8,
    pub unk_16_11: u8,
    pub unk_16_12: u8,
    pub unk_16_13: u8,
    pub unk_16_14: u8,
    pub unk_16_15: u8,
    pub unk_16_16: u8,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct HkConstraintInfo {
    pub kind: u32,
    pub bone_parents_offset: u32, 
    pub bone_parents_num: u32,
    pub bone_names_offset: u32,
    pub bone_names_num: u32,
    pub bone_transforms_offset: u32,
    pub bone_transforms_num: u32,
    pub unk_7: u32,
    pub unk_8: u32,
    pub unk_9: u32,
    pub bones_offset: u32,
    pub bones_num: u16,
    pub bone_order_num: u16,
    pub bone_order_offset: u32,
    pub unk_13: u32,
    pub unk_14: f32,
    pub vals2_num: u32,
    pub vals2_offset: u32,
    pub unk_17: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct EffectInfo {
    pub key: Crc,
    pub gamemodemask: i32,
    pub offset: u32,
    pub size: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct PFieldInfo {
    pub link_guid: u32, 
    pub gamemode_guid: u32, 
    pub width: u32, 
    pub height: u32, 
    pub offset: u32, 
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct GFXBlockInfo {
    // GFX blocks?, unchanged by encoding, model as data
    pub key: Crc,
    pub offset: u32,
    pub size: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct AnimationBlockInfo {
    pub key: Crc,
    #[serde(alias="unk_1")]
    pub guid: u32,
    pub key_name: Crc,
    pub offset: u32,
    pub size: u32,
    pub size_comp: u32,
    pub unk_6: u32,
    pub unk_7: u32,
    pub unk_8: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct FoliageInfo {
    pub key: Crc, 
    pub kind: u32, 
    pub lb_w: i32, 
    pub lb_h: i32, 
    pub ub_w: i32, 
    pub ub_h: i32, 
    pub scale: f32, 
    pub offset: u32, 
    pub key_mesh: Crc, 
    pub key_mesh_lod1: Crc, 
    pub key_mesh_lod2: Crc, 
    pub color: Vector4, 
    pub lod1a: f32,
    pub lod1b: f32, 
    pub lod2a: f32, 
    pub lod2b: f32, 
    pub lod_max: f32, 
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
// points to list of ints in block1
pub struct RadiosityValsInfo {
    pub guid: u32,
    pub num: u32,
    pub offset: u32,
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct BlockAVal {
    pub unk_0: u32,
    pub gamemodemask: i32,
    pub key: Crc,
    pub unk_3: u32,
    pub unk_4: u32,
    pub unk_5: u32,
    pub unk_6: u32,
}

pub mod model {
    use super::*;
    #[basicpymethods]
    #[pyclass(module="pak.model", get_all, set_all)]
    #[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
    pub struct BlockHeader {
        pub a: u32,
        pub b: u32,
        pub unk_2: u32,
        pub unk_3: u32,
        pub unk_4: u32,
    }

    #[basicpymethods]
    #[pyclass(module="pak.model", get_all, set_all)]
    #[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
    pub struct BlockVal {
        pub unk_0: u32,
        pub unk_1: u32,
        pub unk_2: u32,
        pub unk_3: u32,
        pub unk_4: u16,
        pub unk_5: u16,
    }
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct Model{
    pub indices: Vec<u32>,
    pub keys: Vec<u32>,
    pub matrices: Vec<Matrix4x4>,
    pub vals_a: Vec<u32>,
    pub mats: Vec<u32>,
    pub vals_c: Vec<u32>,
    pub vals_d: Vec<u32>,
    pub vbuffs: Vec<u32>,
    pub ibuffs: Vec<u32>,
    pub vals_g: Vec<u32>,
    pub vals_j: Vec<u32>,
    pub string_offsets: Vec<u32>,
    pub strings: Vec<String>,
    pub val_k_header: Vec<u16>,
    pub vals_k: Vec<u32>,
    pub vals_i: Vec<u32>,
    pub keys2: Vec<u32>,
    pub keys2_order: Vec<u32>,
    pub block_header: u32,
    pub block_offsets: Vec<u32>,
    pub blocks: Vec<(model::BlockHeader, Vec<u32>, Vec<model::BlockVal>, Vec<u32>)>,
    pub val: Vec<u32>,
}

impl <'a, 'b> AsData<'a, 'b> for Model {
    type InArgs = &'a ModelInfo;
    type OutArgs = &'b ModelInfo;
    
    fn from_bytes<O: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let mut val = Self::default();

        val.indices = from_bytes!(O, &data[info.bone_parents_offset as usize..], info.bones_num.max(4) as usize)?;
        assert!(val.indices[0] == 0xffffffff);
        val.keys = from_bytes!(O, &data[info.bones_offset as usize..], info.bones_num as usize)?;
        val.matrices = from_bytes!(O, &data[info.bone_transforms_offset as usize..], info.bones_num as usize)?;
        val.vals_a = from_bytes!(O, &data[info.bone_bounding_boxes_offset as usize..], info.bones_num as usize * 8)?;
        val.mats = from_bytes!(O, &data[info.mat_offset as usize..], info.mat_num as usize)?;
        val.vals_c = from_bytes!(O, &data[info.mesh_order_offset as usize..], info.lod3.breakable_end as usize)?;
        val.vals_d = from_bytes!(O, &data[info.mesh_bounding_boxes_offset as usize..], info.lod3.breakable_end as usize * 8)?;
        val.vbuffs = from_bytes!(O, &data[info.vbuff_offset as usize..], info.vbuff_num as usize)?;
        val.ibuffs = from_bytes!(O, &data[info.ibuff_offset as usize..], info.ibuff_num as usize)?;
        val.vals_g = from_bytes!(O, &data[info.skin_binds_offset as usize..], info.skin_binds_num as usize * 16)?;
        if (info.vals_j_num == 0) && (info.vals_j_offset != 0) && (info.vals_j_offset != info.skin_binds_offset) {
            // val.vals_j = from_bytes!(O, &data[info.vals_j_offset as usize..], info.keys_num as usize);
            // for v in &val.vals_j {
            //     let mut offset: u32 = from_bytes!(O, &data[*v as usize..]);
            //     let start = offset;
            //     while data[offset as usize] != 0 { offset += 1; }
            //     let string = String::from_utf8(data[start as usize..offset as usize].to_vec()).unwrap();
            //     val.string_offsets.push(start);
            //     val.strings.push(string);
            // }
        } else {
            val.vals_j = from_bytes!(O, &data[info.vals_j_offset as usize..], info.vals_j_num as usize)?;
        }
        if info.vals_k_offset != 0 {
            val.val_k_header = from_bytes!(O, &data[info.vals_k_offset as usize..], 2)?;
            // if (val.val_k_header[0] != 3) || (val.val_k_header[0] != 6) {
            //     warn!("unexpected valsK data {:?}", info.key);
            // }
            val.vals_k = from_bytes!(O, &data[info.vals_k_offset as usize + 4..], 35)?;
        }
        if info.skin_order_offset != 0 {
            val.vals_i = from_bytes!(O, &data[info.skin_order_offset as usize..], info.skin_binds_num as usize)?;
        }
        if info.slots_offset != 0 {
            assert!(info.slot_map_offset != 0);
            let mut i = 0;
            {
                while from_bytes!(O, u32, &data[info.slots_offset as usize + i * 8..])? != 0 {
                    i += 1;
                }
                i += 1;
            }
            val.keys2 = from_bytes!(O, &data[info.slots_offset as usize..], i * 2)?;
            val.keys2_order = from_bytes!(O, &data[info.slot_map_offset as usize..], *val.keys2.last().unwrap() as usize)?;
        }
        if info.block_offset != 0 {
            val.block_header = from_bytes!(O, &data[info.block_offset as usize..])?;
            let n = (info.lod0.physics_end - info.lod0.skinned_end) as usize;
            val.block_offsets = from_bytes!(O, &data[info.block_offset as usize + 4..], n+1)?;
            for i in 0..n {
                let size = (val.block_offsets[i+1] - val.block_offsets[i]) as usize;
                let offset = (val.block_offsets[i] + info.block_offset) as usize;
                let header: model::BlockHeader = from_bytes!(O, &data[offset..])?;
                let mut s = header.size::<O>();
                let vals_a: Vec<u32> = from_bytes!(O, &data[offset+s..], (header.a + header.b) as usize * 12)?;
                s += vals_a.size::<O>();
                let vals_b: Vec<model::BlockVal> = from_bytes!(O, &data[offset+s..], (size - s)/O::size::<model::BlockVal>())?;
                s += vals_b.size::<O>();
                let extra = from_bytes!(O, &data[offset+s..], (size - s)/4)?;
                val.blocks.push((header, vals_a, vals_b, extra));
            }
        }
        // not sure why this pops up once, maybe it is padding between items?
        if (info.mesh_order_offset == info.vbuff_offset) && (info.mesh_order_offset == info.ibuff_offset) && (info.mesh_order_offset == info.mesh_bounding_boxes_offset) {
            val.val = from_bytes!(O, &data[info.mesh_order_offset as usize..], 4)?;
        }
        Ok(val)
    }

    fn to_bytes<O: Version>(&self, data: &mut [u8], info: Self::OutArgs) -> Result<()> {
        to_bytes!(O, self.indices, &mut data[info.bone_parents_offset as usize..])?;
        to_bytes!(O, self.keys, &mut data[info.bones_offset as usize..])?;
        to_bytes!(O, self.matrices, &mut data[info.bone_transforms_offset as usize..])?;
        to_bytes!(O, self.vals_a, &mut data[info.bone_bounding_boxes_offset as usize..])?;
        to_bytes!(O, self.mats, &mut data[info.mat_offset as usize..])?;
        to_bytes!(O, self.vals_c, &mut data[info.mesh_order_offset as usize..])?;
        to_bytes!(O, self.vals_d, &mut data[info.mesh_bounding_boxes_offset as usize..])?;
        to_bytes!(O, self.vbuffs, &mut data[info.vbuff_offset as usize..])?;
        to_bytes!(O, self.ibuffs, &mut data[info.ibuff_offset as usize..])?;
        to_bytes!(O, self.vals_g, &mut data[info.skin_binds_offset as usize..])?;
        if (info.vals_j_num) == 0 && (info.vals_j_offset != 0) && (info.vals_j_offset != info.skin_binds_offset) {
            to_bytes!(O, self.vals_j, &mut data[info.vals_j_offset as usize..])?;
            for (v, (off, string)) in zip(&self.vals_j, zip(&self.string_offsets,& self.strings)) {
                to_bytes!(O, off, &mut data[*v as usize..])?;
                data[*off as usize..*off as usize+string.len()].copy_from_slice(string.as_bytes());
            }
        } else {
            to_bytes!(O, self.vals_j, &mut data[info.vals_j_offset as usize..])?;
        }
        if info.vals_k_offset != 0 {
            to_bytes!(O, self.val_k_header, &mut data[info.vals_k_offset as usize..])?;
            to_bytes!(O, self.vals_k, &mut data[info.vals_k_offset as usize + 4..])?;
        }
        if info.skin_order_offset != 0 {
            to_bytes!(O, self.vals_i, &mut data[info.skin_order_offset as usize..])?;
        }
        if info.slots_offset != 0 {
            to_bytes!(O, self.keys2, &mut data[info.slots_offset as usize..])?;
            to_bytes!(O, self.keys2_order, &mut data[info.slot_map_offset as usize..])?;
        }
        if info.block_offset != 0 {
            to_bytes!(O, self.block_header, &mut data[info.block_offset as usize..])?;
            to_bytes!(O, self.block_offsets, &mut data[info.block_offset as usize + 4..])?;
            for (i, (header, vals_a, vals_b, extra)) in self.blocks.iter().enumerate() {
                let offset = (self.block_offsets[i] + info.block_offset) as usize;
                to_bytes!(O, header, &mut data[offset..])?;
                let mut s = header.size::<O>();
                to_bytes!(O, vals_a, &mut data[offset + s..])?;
                s += vals_a.size::<O>();
                to_bytes!(O, vals_b, &mut data[offset + s..])?;
                s += vals_b.size::<O>();
                to_bytes!(O, extra, &mut data[offset + s..])?;
            }
        }
        if (info.mesh_order_offset == info.vbuff_offset) && (info.mesh_order_offset == info.ibuff_offset) && (info.mesh_order_offset == info.mesh_bounding_boxes_offset) {
            to_bytes!(O, self.val, &mut data[info.mesh_order_offset as usize..])?;
        }
        Ok(())
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implmented")
    }

}

pub mod shape {
    use super::*;
    #[basicpymethods]
    #[pyclass(module="pak.shape", get_all, set_all)]
    #[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
    pub struct Header {
        pub num: u32,
        pub unk_1: u32,
        pub unk_2: u32,
        pub unk_3: u32,
    }
}

#[basicpymethods]
#[pyclass(module="pak", set_all, get_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct Shape {
    pub header: shape::Header,
    pub vals: Vec<u32>,
    pub data: Vec<u8>,
}


impl <'a, 'b> AsData<'a, 'b> for Shape {
    type InArgs = &'a ShapeInfo;
    type OutArgs = &'b ShapeInfo;
    
    fn from_bytes<O: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let mut val = Self::default();
        if info.kind == 0 {
            let mut offset = info.offset as usize;
            val.header = from_bytes!(O, &data[offset..])?;
            offset += val.header.size::<O>();
            val.vals = from_bytes!(O, &data[offset..], val.header.num as usize)?;
            offset += val.vals.size::<O>();
            val.data = from_bytes!(O, &data[offset..], *val.vals.last().unwrap() as usize + 2)?; // might need to be more than +2, not sure    
        }
        Ok(val)
    }

    fn to_bytes<O: Version>(&self, data: &mut [u8], info: Self::OutArgs) -> Result<()> {
        if info.kind == 0 {
            let mut offset = info.offset as usize;
            to_bytes!(O, self.header, &mut data[offset..])?;
            offset += O::size::<shape::Header>();
            to_bytes!(O, self.vals, &mut data[offset..])?;
            offset += self.vals.size::<O>();
            to_bytes!(O, self.data, &mut data[offset..])?;
        }
        Ok(())
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implmented")
    }
}

#[basicpymethods]
#[pyclass(module="pak", set_all, get_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct HkShape {
    pub a: Vec<u32>,
    pub b: Vec<u32>,
    pub c: Vec<u8>,
    pub d: Vec<u32>,
    pub e: Vec<u16>,
}

impl <'a, 'b> AsData<'a, 'b> for HkShape {
    type InArgs = &'a HkShapeInfo;
    type OutArgs = &'a HkShapeInfo;

    fn from_bytes<O: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let mut val = Self::default();
        if info.kind == 5 {
            val.a = from_bytes!(O, &data[info.a_offset as usize..], info.a_num as usize * 4)?;
            let mut b_num = info.b_num as usize; // sketchy stuff to account for data that was not otherwise captured, is it needed?
            while (info.b_offset as usize + b_num * 12) % 16 != 0 { b_num += 1; }
            val.b = from_bytes!(O, &data[info.b_offset as usize..], b_num * 3)?;
        } else if info.kind == 6 {
            val.c = from_bytes!(O, &data[info.c_offset as usize..], info.c_num as usize)?;
            val.d = from_bytes!(O, &data[info.d_offset as usize..], info.d_num as usize * 3)?;
            val.e = from_bytes!(O, &data[info.e_offset as usize..], info.e_num as usize * 3)?;
        } else if info.kind > 6 {
            warn!("Unknown & Unhandled HkShape type {}", info.kind);
        }
        Ok(val)
    }

    fn to_bytes<O: Version>(&self, data: &mut [u8], info: Self::OutArgs) -> Result<()> {
        if info.kind == 5 {
            to_bytes!(O, self.a, &mut data[info.a_offset as usize..])?;
            to_bytes!(O, self.b, &mut data[info.b_offset as usize..])?;
        } else if info.kind == 6 {
            to_bytes!(O, self.c, &mut data[info.c_offset as usize..])?;
            to_bytes!(O, self.d, &mut data[info.d_offset as usize..])?;
            to_bytes!(O, self.e, &mut data[info.e_offset as usize..])?;
        } else if info.kind > 6 {
            warn!("Unknown & Unhandled HkShape type {}", info.kind);
        }
        Ok(())
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implmented")
    }
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct HkConstraint {
    pub shorts: Vec<u16>,
    pub strings: Vec<(String, u32, u32)>,
    pub string_offsets: Vec<u32>,
    pub vals: Vec<u32>,
    pub keys: Vec<u32>,
    pub keys2: Vec<u32>,
}

impl <'a, 'b> AsData<'a, 'b> for HkConstraint {
    type InArgs = &'a HkConstraintInfo;
    type OutArgs = &'b HkConstraintInfo;

    fn from_bytes<V: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let mut val = Self::default();
        if info.kind != 0 { warn!("Unknown & Unhandled HkConstraint type {}", info.kind); }

        val.shorts = from_bytes!(V, &data[info.bone_parents_offset as usize..], info.bone_parents_num as usize)?;
        assert!(val.shorts[0] == 0xFFFF);

        val.string_offsets = from_bytes!(V, &data[info.bone_names_offset as usize..], info.bone_names_num as usize)?;
        for offset_ in val.string_offsets.iter() {
            let (mut offset, val_) = { 
                let vals: Vec<u32> = from_bytes!(V, &data[*offset_ as usize..], 2)?;

                (vals[0], vals[1]) 
            };
            let start = offset;
            while data[offset as usize] != 0 { offset += 1; }
            let string = String::from_utf8(data[start as usize..offset as usize].to_vec()).unwrap();
            val.strings.push((string, start, val_));
        }
        val.vals = from_bytes!(V, &data[info.bone_transforms_offset as usize..], info.bone_transforms_num as usize * 12)?;
        val.keys = from_bytes!(V, &data[info.bones_offset as usize..], info.bones_num as usize)?;
        val.keys2 = from_bytes!(V, &data[info.bone_order_offset as usize..], info.bone_order_num as usize * 2)?;
        Ok(val)
    }

    fn to_bytes<V: Version>(&self, data: &mut [u8], info: Self::OutArgs) -> Result<()> {
        to_bytes!(V, self.shorts, &mut data[info.bone_parents_offset as usize..])?;
        to_bytes!(V, self.string_offsets, &mut data[info.bone_names_offset as usize..])?;
        for (offset_, (string, offset, val)) in zip(&self.string_offsets, &self.strings) {
            to_bytes!(V, offset, &mut data[*offset_ as usize..])?;
            to_bytes!(V, val, &mut data[*offset_ as usize + V::size::<u32>()..])?;
            data[*offset as usize..*offset as usize+string.len()].copy_from_slice(string.as_bytes());
        }
        to_bytes!(V, self.vals, &mut data[info.bone_transforms_offset as usize..])?;
        to_bytes!(V, self.keys, &mut data[info.bones_offset as usize..])?;
        to_bytes!(V, self.keys2, &mut data[info.bone_order_offset as usize..])?;
        Ok(())
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implmented")
    }
}


#[derive(Debug, Clone, SerializeDisplay, DeserializeFromStr, PartialEq, Hash)]
pub enum VertexUsage {
    Position,
    Normal,
    Tangent,
    BlendWeight,
    BlendIndices,
    Color(usize),
    TextureCoord(usize),
    Unknown(usize),
    PSize,
    Pad(usize),
}

impl Default for VertexUsage {
    fn default() -> Self {
        Self::Unknown(0xFFFFFFFF)
    }
}

impl Eq for VertexUsage {}

impl Display for VertexUsage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Position => write!(f, "Position"),
            Self::Normal => write!(f, "Normal"),
            Self::Tangent => write!(f, "Tangent"),
            Self::BlendWeight => write!(f, "BlendWeight"),
            Self::BlendIndices => write!(f, "BlendIndices"),
            Self::Color(i) => write!(f, "Color({})", i),
            Self::TextureCoord(i) => write!(f, "TextureCoord({})", i),
            Self::Unknown(i) => write!(f, "Unknown({})", i),
            Self::PSize => write!(f, "PSize"),
            Self::Pad(i) => write!(f, "Pad({})", i),
        }
    }
}


#[derive(Debug)]
pub struct VertexUsageParseError;
impl Display for VertexUsageParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "VertexUsageParseError")
    }
}

impl From<ParseIntError> for VertexUsageParseError {
    fn from(_value: ParseIntError) -> Self {
        Self
    }
}

impl FromStr for VertexUsage {
    type Err = VertexUsageParseError;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let string = s.to_lowercase();
        let s = string.as_str();
        match s {
            "position" => Ok(Self::Position),
            "normal" => Ok(Self::Normal),
            "tangent" => Ok(Self::Tangent),
            "blendweight" => Ok(Self::BlendWeight),
            "blendindices" => Ok(Self::BlendIndices),
            "psize" => Ok(Self::PSize),
            s => {
                if s.starts_with("color(") {
                    Ok(s[6..].split(')').next().unwrap().parse::<usize>().map(|i| Self::Color(i))?)
                } else if s.starts_with("texturecoord(") {
                    Ok(s[13..].split(')').next().unwrap().parse::<usize>().map(|i| Self::TextureCoord(i))?)
                } else if s.starts_with("unknown(") {
                    Ok(s[8..].split(')').next().unwrap().parse::<usize>().map(|i| Self::Unknown(i))?)
                } else if s.starts_with("pad(") {
                    Ok(s[4..].split(')').next().unwrap().parse::<usize>().map(|i| Self::Pad(i))?)
                } else {
                    Err(VertexUsageParseError)
                }
            }
        }
    }
}

impl <'py> IntoPyObject<'py> for VertexUsage {
    type Target = <String as IntoPyObject<'py>>::Target;
    type Output = <String as IntoPyObject<'py>>::Output;
    type Error = <String as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        format!("{}", self).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for VertexUsage {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        let s = String::extract_bound(ob)?;
        s.parse().map_err(|e| PyErr::new::<PyTypeError, _>(format!("{}", e)))
    }
}

#[basicpymethods(no_bytes)]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum VertexTypes {
    Vector2(Vec<f32>,Vec<f32>),
    Vector3(Vec<f32>,Vec<f32>,Vec<f32>),
    Vector4(Vec<f32>, Vec<f32>, Vec<f32>, Vec<f32>),
    Unorm4x8(Vec<u32>),
    Pad(Vec<u32>),
    None(),
}

impl Default for VertexTypes {
    fn default() -> Self {
        Self::None()
    }
}

impl VertexTypes {
    pub fn new(format: u32) -> Self {
        match format {
            BaseTypes::INT_KEY => Self::Pad(vec![]),
            BaseTypes::COLOR_KEY => Self::Unorm4x8(vec![]),
            BaseTypes::VECTOR2_KEY => Self::Vector2(vec![], vec![]),
            BaseTypes::VECTOR3_KEY => Self::Vector3(vec![], vec![], vec![]),
            BaseTypes::VECTOR4_KEY => Self::Vector4(vec![], vec![], vec![], vec![]),
            _ => Self::None()
        }
    }

    pub fn ty(&self) -> u32 {
        match self {
            Self::Pad(..) => BaseTypes::INT_KEY,
            Self::Unorm4x8(..) => BaseTypes::COLOR_KEY,
            Self::Vector2(..) => BaseTypes::VECTOR2_KEY,
            Self::Vector3(..) => BaseTypes::VECTOR3_KEY,
            Self::Vector4(..) => BaseTypes::VECTOR4_KEY,
            Self::None() => 0,
        }
    }

    pub fn get(&self, i: usize) -> BaseTypes {
        match self {
            Self::Pad(vals) => BaseTypes::Color(Color(vals[i])),
            Self::Unorm4x8(vals) => BaseTypes::Color(Color(vals[i])),
            Self::Vector2(x, y) => BaseTypes::Vector2(Vector2 {x: x[i], y: y[i]}),
            Self::Vector3(x, y, z) => BaseTypes::Vector3(Vector3 {x: x[i], y: y[i], z: z[i]}),
            Self::Vector4(x, y, z, w) => BaseTypes::Vector4(Vector4 {x: x[i], y: y[i], z: z[i], w: w[i]}),
            Self::None() => BaseTypes::Int(0)
        }
    }

    pub fn len(&self) -> usize {
        match self {
            Self::Pad(vals) => vals.len(),
            Self::Unorm4x8(vals) => vals.len(),
            Self::Vector2(x, y) => x.len().min(y.len()),
            Self::Vector3(x, y, z) => x.len().min(y.len()).min(z.len()),
            Self::Vector4(x, y, z, w) => x.len().min(y.len()).min(z.len()).min(w.len()),
            Self::None() => 0
        }
    }

    pub fn size(&self) -> usize {
        match self {
            Self::Pad(..) => 4,
            Self::Unorm4x8(..) => 4,
            Self::Vector2(..) => 8,
            Self::Vector3(..) => 12,
            Self::Vector4(..) => 16,
            Self::None() => 0
        }
    }

    pub fn push(&mut self, val: BaseTypes) {
        match self {
            Self::Pad(vals) => if let BaseTypes::Color(val) = val {
                vals.push(val.0);
            } else { panic!("VertexTypes Mismatch") },
            Self::Unorm4x8(vals) => if let BaseTypes::Color(val) = val {
                vals.push(val.0);
            } else { panic!("VertexTypes Mismatch") },
            Self::Vector2(x, y)=> if let BaseTypes::Vector2(val) = val {
                x.push(val.x); y.push(val.y);
            } else { panic!("VertexTypes Mismatch") },
            Self::Vector3(x, y, z)=> if let BaseTypes::Vector3(val) = val {
                x.push(val.x); y.push(val.y); z.push(val.z);
            } else { panic!("VertexTypes Mismatch") },
            Self::Vector4(x, y, z, w) => if let BaseTypes::Vector4(val) = val {
                x.push(val.x); y.push(val.y); z.push(val.z); w.push(val.w);
            } else { panic!("VertexTypes Mismatch") },
            Self::None() => ()
        }
    }
}

//#[serde_as]
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct VertexData {
    #[serde(rename="$key$")]
    //#[serde_as(as="serde_with::DisplayFromStr")]
    pub usage: VertexUsage,
    pub val: VertexTypes,
}

#[derive(Debug)]
pub struct VertexDataParseError;
impl Display for VertexDataParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "VertexDataParseError")
    }
}

impl VertexData {
    pub fn new(format: u32, usage: VertexUsage) -> Self {
        Self {
            usage,
            val: VertexTypes::new(format)
        }
    }

    pub fn size(&self) -> usize {
        self.val.size() * self.val.len()
    }

    pub fn dump_bytes<O: Version + 'static>(&self) -> Vec<u8> {
        match self {
            Self { usage: VertexUsage::BlendWeight, val: VertexTypes::Unorm4x8(vals) } => dump_bytes!(O, vals.iter().map(|x| ((x & 0xFF0000) >> 16) | ((x & 0xFF) << 16) | (x & 0xFF00FF00)).collect::<Vec<_>>()),
            Self { val: VertexTypes::Pad(vals), .. } => dump_bytes!(O, vals),
            Self { val: VertexTypes::Unorm4x8(vals), .. } => dump_bytes!(O, vals),
            Self { val: VertexTypes::Vector2(x, y), .. } => dump_bytes!(O, x.iter().zip(y).map(|(&x, &y)| Vector2{x,y}).collect::<Vec<_>>()),
            Self { val: VertexTypes::Vector3(x, y, z), .. } => dump_bytes!(O, x.iter().zip(y.iter().zip(z)).map(|(&x, (&y, &z))| Vector3{x,y,z}).collect::<Vec<_>>()),
            Self { val: VertexTypes::Vector4(x, y, z, w), .. } => dump_bytes!(O, x.iter().zip(y.iter().zip(z.iter().zip(w))).map(|(&x, (&y, (&z, &w)))| Vector4{x,y,z,w}).collect::<Vec<_>>()),
            Self { val: VertexTypes::None(), .. } => vec![],
        }
    }

    pub fn gltf_data(&self) -> Vec<u8> {
        fn conv_val(val: u32) -> u32 {
            if val < 127 {
                129 + val
            } else {
                val - 127
            }
        }
        match self {
            Self { usage: VertexUsage::Normal, val: VertexTypes::Unorm4x8(vals) } => {
                dump_bytes!(PC, vals.iter().map(|x|
                    conv_val((x >> 24) & 0xff) << 24 | 
                    conv_val((x >> 16) & 0xff) << 16 | 
                    conv_val((x >> 8) & 0xff) << 8 | 
                    conv_val(x & 0xff)
                ).collect::<Vec<_>>())
            },
            _ => self.dump_bytes::<PC>()
        }
    }
    
    pub fn from_gltf(&mut self, mut data: GltfData) -> Result<(), VertexDataParseError> {
        use gltf::accessor::DataType;
        fn conv_val(val: u32) -> u32 {
            if val > 128 {
                val - 129
            } else if val < 128 {
                val + 127
            } else {
                0
            }
        }

        match self {
            Self { usage: VertexUsage::BlendWeight, val: VertexTypes::Unorm4x8(vals) } => {
                assert!(data.ty == DataType::U8);
                assert!(data.m == 4);
                data.ty = DataType::U32;
                data.m = 1;
                vals.extend(data.u32().unwrap()
                    .into_iter().map(|x| ((x & 0xFF0000) >> 16) | ((x & 0xFF) << 16) | (x & 0xFF00FF00))
                );
            },
            Self { usage: VertexUsage::Normal, val: VertexTypes::Unorm4x8(vals) } => {
                assert!(data.ty == DataType::I8);
                assert!(data.m == 3);
                data.ty = DataType::U32;
                data.m = 1;
                vals.extend(data.u32().unwrap().iter().map(|x|
                    conv_val((x >> 24) & 0xff) << 24 | 
                    conv_val((x >> 16) & 0xff) << 16 | 
                    conv_val((x >> 8) & 0xff) << 8 | 
                    conv_val(x & 0xff)
                ));
            },
            Self { usage: VertexUsage::Normal | VertexUsage::Position, val: VertexTypes::Vector4(x,y,z,w) } => {
                assert!(data.ty == DataType::F32);
                assert!(data.m == 3);
                data.m = 4;
                data.f32().unwrap().as_slice().chunks_exact(4).for_each(|val| {
                    x.push(val[0]); y.push(val[1]); z.push(val[2]); w.push(val[3]); 
                });
            },
            Self { val: VertexTypes::Pad(vals), .. } => vals.extend(data.u32().unwrap()),
            Self { val: VertexTypes::Unorm4x8(vals), .. } => {
                assert!(data.ty == DataType::U8);
                assert!(data.m == 4);
                data.ty = DataType::U32;
                data.m = 1;
                vals.extend(data.u32().unwrap());
            }
            Self { val: VertexTypes::Vector2(x,y), .. } => data.f32().unwrap().as_slice().chunks_exact(2)
                .for_each(|val| { x.push(val[0]); y.push(val[1]); }),
            Self { val: VertexTypes::Vector3(x,y,z), .. } => data.f32().unwrap().as_slice().chunks_exact(3)
                .for_each(|val| { x.push(val[0]); y.push(val[1]); z.push(val[2]); }),
            Self { val: VertexTypes::Vector4(x,y,z,w), .. } => data.f32().unwrap().as_slice().chunks_exact(4)
                .for_each(|val| { x.push(val[0]); y.push(val[1]); z.push(val[2]); w.push(val[3]); }),
            _ => Err(VertexDataParseError)?
        }
        Ok(())
    }

    pub fn min(&self) -> Option<serde_json::value::Value> {
        use serde_json::json;
        match self {
            Self { usage: VertexUsage::Position, val: VertexTypes::Vector3(x,y,z) } => Some(json!(vec![
                    x.iter().copied().reduce(f32::min), 
                    y.iter().copied().reduce(f32::min), 
                    z.iter().copied().reduce(f32::min)
            ])),
            Self { usage: VertexUsage::Position, val: VertexTypes::Vector4(x,y,z,..) } => Some(json!(vec![
                    x.iter().copied().reduce(f32::min), 
                    y.iter().copied().reduce(f32::min), 
                    z.iter().copied().reduce(f32::min),
            ])),
            _ => None
        }
    }

    pub fn max(&self) -> Option<serde_json::value::Value> {
        use serde_json::json;
        match self {
            Self { usage: VertexUsage::Position, val: VertexTypes::Vector3(x,y,z) } => Some(json!(vec![
                    x.iter().copied().reduce(f32::max), 
                    y.iter().copied().reduce(f32::max), 
                    z.iter().copied().reduce(f32::max)
            ])),
            Self { usage: VertexUsage::Position, val: VertexTypes::Vector4(x,y,z,..) } => Some(json!(vec![
                    x.iter().copied().reduce(f32::max), 
                    y.iter().copied().reduce(f32::max), 
                    z.iter().copied().reduce(f32::max),
            ])),
            _ => None
        }
    }

    pub fn component_type(&self) -> gltf::accessor::DataType {
        use gltf::accessor::DataType;
        match self {
            Self { usage: VertexUsage::Normal, val: VertexTypes::Unorm4x8(..) } => DataType::I8,
            Self { val: VertexTypes::Unorm4x8(..), .. } => DataType::U8,
            Self { val: VertexTypes::Pad(..), .. } => DataType::U32,
            Self { val: VertexTypes::Vector2(..), .. } => DataType::F32,
            Self { val: VertexTypes::Vector3(..), .. } => DataType::F32,
            Self { val: VertexTypes::Vector4(..), .. } => DataType::F32,
            _ => DataType::U32,
        }
    }

    pub fn dimensions(&self) -> gltf::accessor::Dimensions {
        use gltf::accessor::Dimensions;
        match self {
            Self { usage: VertexUsage::Normal, val: VertexTypes::Unorm4x8(..) } => Dimensions::Vec3,
            Self { usage: VertexUsage::Normal | VertexUsage::Position, val: VertexTypes::Vector4(..) } => Dimensions::Vec3,
            Self { val: VertexTypes::Unorm4x8(..), .. } => Dimensions::Vec4,
            Self { val: VertexTypes::Pad(..), .. } => Dimensions::Scalar,
            Self { val: VertexTypes::Vector2(..), .. } => Dimensions::Vec2,
            Self { val: VertexTypes::Vector3(..), .. } => Dimensions::Vec3,
            Self { val: VertexTypes::Vector4(..), .. } => Dimensions::Vec4,
            _ => Dimensions::Scalar,
        }
    }

    pub fn stride(&self) -> Option<usize> {
        match self {
            Self { usage: VertexUsage::Normal, val: VertexTypes::Unorm4x8(..) } => Some(4),
            Self { usage: VertexUsage::Normal | VertexUsage::Position, val: VertexTypes::Vector4(..) } => Some(16),
            _ => None,
        }
    }
    
    pub fn normalized(&self) -> bool {
        match self {
            Self { usage: VertexUsage::Normal | VertexUsage::BlendWeight, val: VertexTypes::Unorm4x8(..) } => true,
            _ => false
        }
    }

    pub fn order(&self) -> usize {
        match self {
            VertexData { usage: VertexUsage::Position, .. } => 0,
            VertexData { usage: VertexUsage::BlendWeight | VertexUsage::Pad(0), .. } => 1,
            VertexData { usage: VertexUsage::BlendIndices | VertexUsage::Pad(1), .. } => 2,
            VertexData { usage: VertexUsage::Pad(2), .. } => 3,
            VertexData { usage: VertexUsage::Pad(3), .. } => 4,
            VertexData { usage: VertexUsage::Pad(4), .. } => 5,
            VertexData { usage: VertexUsage::Normal, .. } => 6,
            VertexData { usage: VertexUsage::Color(0), .. } => 7,
            VertexData { usage: VertexUsage::Color(1), .. } => 8,
            VertexData { usage: VertexUsage::TextureCoord(0), .. } => 9,
            VertexData { usage: VertexUsage::TextureCoord(1), .. } => 10,
            VertexData { usage: VertexUsage::TextureCoord(2), .. } => 11,
            VertexData { usage: VertexUsage::TextureCoord(3), .. } => 12,
            VertexData { usage: VertexUsage::Pad(5), .. } => 13,
            VertexData { usage: VertexUsage::Pad(6), .. } => 14,
            VertexData { usage: VertexUsage::Pad(7), .. } => 15,
            VertexData { usage: VertexUsage::Tangent, .. } => 16,
            VertexData { usage: VertexUsage::PSize, .. } => 17,
            _ => usize::MAX
        }
    }
}

lazy_static::lazy_static! {
    static ref FORMATS: Mutex<HashMap<(u32, u32), (Vec<VertexData>, usize)>> = Mutex::new(HashMap::new());
}

#[serde_as]
#[derive(Default, Debug, Clone, Serialize, Deserialize)]
pub struct VertexBuffer {
    //#[serde_as(as = "serde_with::Map<serde_with::DisplayFromStr, _>")]
    pub info: VBuffInfo,
    #[serde_as(as="serde_with::KeyValueMap<_>")]
    pub data: Vec<VertexData>,
}

impl VertexBuffer {
    pub fn len(&self) -> usize {
        self.data.first().map(|x| x.val.len()).unwrap_or(0)
    }

    pub fn v_size(&self) -> usize {
        self.data.iter().map(|x| x.val.size()).sum::<usize>()
    }

    pub fn from_vertex_format<V: Version>(fmt1: u32, fmt2: u32) -> Vec<VertexData> {
        let mut vals = Vec::new();
        let mut s = 0;
        if fmt2 == 0 {
            let b1: bool = (fmt1 & 0x40000) != 0;
            if fmt1 & 1 != 0 {
                vals.push(VertexData::new(if b1 {BaseTypes::VECTOR4_KEY} else {BaseTypes::VECTOR3_KEY}, VertexUsage::Position));
                s += if b1 {16} else {12};
            }
            if (fmt1 & 0x400) != 0 {
                // blend weights
                if b1 {
                    vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Pad(0)));
                } else {
                    vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::BlendWeight));
                }
                s += 4;
            }
            if (fmt1 & 0x800) != 0 {
                // blend indices
                if b1 {
                    vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Pad(1)));
                } else {
                    vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::BlendIndices));
                }
                s += 4;
                // if b1 then Vec4 ?? 
            }
            if (fmt1 & 2) != 0 {
                // normal
                let usage = VertexUsage::Normal;
                if b1 {
                    let mut p = 2;
                    for _ in (0..(((s + 15) & 0xFFFF0) - s)).step_by(4) {
                        vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Pad(p)));
                        s += 4;
                        p += 1;
                    }
                    vals.push(VertexData::new(BaseTypes::VECTOR4_KEY, usage));
                    s += 16;
                } else if V::ps3() {
                    vals.push(VertexData::new(BaseTypes::VECTOR3_KEY, usage));
                    s += 12;
                } else {
                    vals.push(VertexData::new(BaseTypes::COLOR_KEY, usage));
                    s += 4;
                }
            }
            if fmt1 & 0x100 != 0 {
                // color(0)
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Color(0)));
                s += 4;
            }
            if fmt1 & 0x200 != 0 {
                // color(1)
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Color(1)));
                s += 4;
            }
            for i in 0..((fmt1 >> 2) & 0xF) {
                // texture coords
                vals.push(VertexData::new(BaseTypes::VECTOR2_KEY, VertexUsage::TextureCoord(i as usize)));
                s += 8;
            }
            if fmt1 & 0x40 != 0 {
                // tangent
                let usage = VertexUsage::Tangent;
                if b1 {
                    let mut p = 5;
                    for _ in (0..(((s + 15) & 0xFFFF0) - s)).step_by(4) {
                        vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Pad(p)));
                        s += 4;
                        p += 1;
                    }
                    vals.push(VertexData::new(BaseTypes::VECTOR4_KEY, usage));
                    s += 16;
                } else {
                    vals.push(VertexData::new(BaseTypes::COLOR_KEY, usage));
                    s += 4;
                }
            }
            if fmt1 & 0x80 != 0 {
                vals.push(VertexData::new(BaseTypes::VECTOR3_KEY, VertexUsage::PSize));
                s += 12;
            }
            if b1 {
                let mut p = 8;
                for _ in (0..(((s + 15) & 0xFFFF0) - s)).step_by(4) {
                    vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Pad(p)));
                    s += 4;
                    p += 1;
                }
            }
        } else {
            if fmt1 & 1 != 0 {
                vals.push(VertexData::new(BaseTypes::VECTOR3_KEY, VertexUsage::Position));
                s += 12;
            }
            if fmt1 & 0x400 != 0 {
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::BlendWeight));
                s += 4;
            }
            if fmt1 & 0x800 != 0 {
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::BlendIndices));
                s += 4;
            }
            if fmt1 & 2 != 0 {
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Normal));
                s += 4;
            }
            if fmt1 & 0x100 != 0 {
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Color(0)));
                s += 4;
            }
            if fmt1 & 0x200 != 0 {
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Color(1)));
                s += 4;
            }
            let n = (fmt1 >> 2) & 0xf;
            if n <= 2 {
                for i in 0..n {
                    vals.push(VertexData::new(BaseTypes::VECTOR2_KEY, VertexUsage::TextureCoord(i as usize)));
                    s += 8;
                }
            }
            if fmt1 & 0x40 != 0 {
                vals.push(VertexData::new(BaseTypes::COLOR_KEY, VertexUsage::Tangent));
                //s += 4;
            }
            if fmt1 & 0x80 != 0 {
                vals.push(VertexData::new(BaseTypes::VECTOR3_KEY, VertexUsage::PSize));
                //s += 12;
            }
        }
        vals
    }

    pub fn get_vertex_format<V: Version>(&self) -> u32 {
        let mut fmt1 = 0u32;
        for val in &self.data {
            match val {
                VertexData { usage: VertexUsage::Position, val: VertexTypes::Vector3(..) } => {
                    fmt1 |= 1;
                },
                VertexData { usage: VertexUsage::Position, val: VertexTypes::Vector4(..) } => {
                    fmt1 |= 0x40001;
                },
                VertexData { usage: VertexUsage::BlendWeight, .. } => {
                    fmt1 |= 0x400;
                },
                VertexData { usage: VertexUsage::BlendIndices, .. } => {
                    fmt1 |= 0x800;
                },
                VertexData { usage: VertexUsage::Normal, val: VertexTypes::Unorm4x8(..) } => {
                    fmt1 |= 0x2;
                },
                VertexData { usage: VertexUsage::Normal, val: VertexTypes::Vector4(..) } => {
                    fmt1 |= 0x40002;
                },
                VertexData { usage: VertexUsage::PSize, .. } => {
                    fmt1 |= 0x80;
                },
                VertexData { usage: VertexUsage::Tangent, val: VertexTypes::Unorm4x8(..) } => {
                    fmt1 |= 40;
                },
                VertexData { usage: VertexUsage::Tangent, val: VertexTypes::Vector4(..) } => {
                    fmt1 |= 0x40040;
                },
                VertexData { usage: VertexUsage::Color(0), .. } => {
                    fmt1 |= 0x100;
                },
                VertexData { usage: VertexUsage::Color(1), .. } => {
                    fmt1 |= 0x200;
                },
                VertexData { usage: VertexUsage::Pad(0), .. } => {
                    fmt1 |= 0x40400;
                },
                VertexData { usage: VertexUsage::Pad(1), .. } => {
                    fmt1 |= 0x40800;
                },
                VertexData { usage: VertexUsage::TextureCoord(0), .. } => {
                    fmt1 |= 0x4;
                },
                VertexData { usage: VertexUsage::TextureCoord(1), .. } => {
                    fmt1 |= 0x8;
                },
                VertexData { usage: VertexUsage::TextureCoord(2), .. } => {
                    fmt1 |= 0x10;
                },
                VertexData { usage: VertexUsage::TextureCoord(3), .. } => {
                    fmt1 |= 0x20;
                },
                _ => (),
            }
        }
        fmt1
    }
}

impl <'py> IntoPyObject<'py> for VertexBuffer {
    type Target = PyDict;
    type Output = Bound<'py, Self::Target>;
    type Error = PyErr;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        let dict = PyDict::new(py);
        for v in self.data {
            dict.set_item(v.usage, v.val)?;
        }
        dict.set_item("info", self.info)?;
        Ok(dict)
    }
}

impl <'py> FromPyObject<'py> for VertexBuffer {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        let dict = ob.downcast::<PyDict>()?;
        let info: VBuffInfo = dict.get_item("info")?.ok_or(anyhow::anyhow!("dict missing info key"))?.extract()?;
        dict.del_item("info")?;
        let data: Vec<VertexData> = dict.into_iter().map(|(k, v)| Ok(VertexData { usage: k.extract()?, val: v.extract()? })).collect::<Result<_>>()?;
        Ok(Self { info, data })
    }
}

impl <'a> AsData<'a, '_> for VertexBuffer {
    type InArgs = VBuffInfo;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], mut info: Self::InArgs) -> Result<Self> {
        //let mut formats = FORMATS.lock().unwrap();
        let mut vals =  VertexBuffer::from_vertex_format::<V>(info.fmt1, info.fmt2);
        let size = vals.iter().map(|x| x.val.size()).sum::<usize>();
        if info.size as usize % size != 0 {
            warn!("Vertex Buffer size is not a multiple of assumed size");
        }
        let n = info.size as usize / size;
        let mut offset = info.offset as usize;
        for _ in 0..n {
            // let mut val = Vec::with_capacity(fmt.len());
            for val in &mut vals {
                let v = BaseTypes::from_data::<V>(&data[offset..], val.val.ty())?;
                offset += v.size::<V>();
                val.val.push(v);
            }
        }
        if V::xbox() {
            if (info.fmt1 & 0x80000 != 0) & (info.fmt1 & 0x400 == 0) {
                info.fmt1 |= 0x400;
                let mut vals_new = VertexBuffer::from_vertex_format::<V>(info.fmt1, info.fmt2);
                let mut blend_inds = Vec::with_capacity(n);
                let mut blend_weights = Vec::with_capacity(n);
                for val in &mut vals {
                    match val {
                        VertexData { 
                            usage: VertexUsage::BlendIndices, 
                            val: VertexTypes::Unorm4x8(val),
                        } => {
                            for v in val {
                                let a = *v & 0xFF;
                                let b = (*v >> 8) & 0xFF;
                                let c = (*v >> 16) & 0xFF;
                                let d = (*v >> 24) & 0xFF;
                                // println!("{}, {:?}",*v, (a,b,c,d));
                                blend_inds.push((d << 24) | (d << 16) | (d << 8) | c);
                                blend_weights.push((a << 16) | (b << 8));
                            }
                        },
                        _ => (),
                    }
                }
                for val in &mut vals_new {
                    match val {
                        VertexData { 
                            usage: VertexUsage::BlendIndices, val
                        } => *val = VertexTypes::Unorm4x8(blend_inds.clone()),
                        VertexData { 
                            usage: VertexUsage::BlendWeight, val
                        } => *val = VertexTypes::Unorm4x8(blend_weights.clone()),
                        _ => for val2 in &vals {
                            if val.usage == val2.usage {
                                val.val = val2.val.clone();
                            }
                        }
                    }
                }
                vals = vals_new;
            }
            for val in &mut vals {
                match val {
                    VertexData {
                        usage: VertexUsage::Normal, val: VertexTypes::Vector4(x, y, z, ..)
                    } => {
                        x.iter_mut().for_each(|x| *x = *x/2.0 + 0.5);
                        y.iter_mut().for_each(|x| *x = *x/2.0 + 0.5);
                        z.iter_mut().for_each(|x| *x = *x/2.0 + 0.5);
                    },
                    VertexData {
                        usage: VertexUsage::Normal, val: VertexTypes::Unorm4x8(v)
                    } => v.iter_mut().for_each(|val| {
                        let z_ = ((*val) & 0x3FF) ^ 0x200;
                        let y_ = (((*val) >> 10) & 0x3FF) ^ 0x200;
                        let x_ = (((*val) >> 20) & 0x3FF) ^ 0x200;
                        let x: u32 = (x_ as f32 - 4.0f32).div(4.0f32).round_ties_even().clamp(0.0, 255.0) as u32;
                        let y: u32 = (y_ as f32 - 4.0f32).div(4.0f32).round_ties_even().clamp(0.0, 255.0) as u32;
                        let z: u32 = (z_ as f32 - 4.0f32).div(4.0f32).round_ties_even().clamp(0.0, 255.0) as u32;
                        *val = (127 << 24) | (z << 16) | (y << 8) | x;
                    }),
                    VertexData { usage: VertexUsage::Normal, .. } => panic!("Unexpected type for vertex normals"),
                    _ => (),
                }
            }
        }
        Ok(Self { info: info.clone(), data: vals })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let i = self.data.iter().map(|x| x.val.len()).min().unwrap();
        let sorted_vals: Vec<_> = self.data.iter().sorted_by_key(|val| val.order()).collect();
        (0..i).flat_map(|i| sorted_vals.iter().flat_map(move |val| dump_bytes!(V, val.val.get(i)))).collect()
    }

    fn size<V: Version>(&self) -> usize {
        self.data.iter().map(|x| x.size()).sum::<usize>()
    }
}

#[basicpymethods]
#[pyclass(module="pak", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum IndexBuffer {
    U16 { vals: Vec<u16> },
    U32 { vals: Vec<u32> },
}

impl IndexBuffer {
    pub fn len(&self) -> usize {
        match self {
            Self::U16 { vals } => vals.len(),
            Self::U32 { vals } => vals.len(),
        }
    }
}

impl <'a> AsData<'a, '_> for IndexBuffer {
    type InArgs = &'a IBuffInfo;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let size = match info.format {
            0x10 => V::size::<u16>(),
            _ => V::size::<u32>(),
        };
        assert!(info.size as usize % size == 0);
        let mut n = info.size as usize / size;
        if data.len() < (info.offset + info.size) as usize {
            warn!("Index buffer of size {} only has data of size {} available", info.size, data.len() - info.offset as usize);
            n = (data.len() - info.offset as usize) / size;
        }
        Ok(match info.format {
            0x10 => Self::U16 { vals: from_bytes!(V, &data[info.offset as usize..], n)? },
            _ => Self::U32 { vals: from_bytes!(V, &data[info.offset as usize..], n)? },
        })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        match self {
            Self::U16 { vals } => dump_bytes!(V, vals),
            Self::U32 { vals } => dump_bytes!(V, vals)
        }
    }

    fn size<V: Version>(&self) -> usize {
        match self {
            Self::U16 { vals } => vals.size::<V>(),
            Self::U32 { vals } => vals.size::<V>()
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(transparent)]
// list of ints pointing to the radiosity data
// has a value for each mesh in the model 
// corresponding to the object pointed to by the guid
// (value if offset to consecutive values in radiosity, one for each vertex of the mesh)
// a value of -1 means no radiosity for that mesh
pub struct RadiosityVals {
    pub vals: Vec<i32>
}

impl <'py> IntoPyObject<'py> for RadiosityVals {
    type Target = <Vec<i32> as IntoPyObject<'py>>::Target;
    type Output = <Vec<i32> as IntoPyObject<'py>>::Output;
    type Error = <Vec<i32> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.vals.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for RadiosityVals {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { vals: Vec::extract_bound(ob)? })
    }
}

impl <'a, 'b> AsData<'a, 'b> for RadiosityVals {
    type InArgs = &'a RadiosityValsInfo;
    type OutArgs = &'b RadiosityValsInfo;

    fn from_bytes<V: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        Ok(Self { vals: from_bytes!(V, &data[info.offset as usize..], info.num as usize)? })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        self.vals.size::<V>()
    }

    fn to_bytes<V: Version>(&self, data: &mut [u8], info: Self::OutArgs) -> Result<()> {
        to_bytes!(V, self.vals, &mut data[info.offset as usize..])
    }
}

type FoliageValProxy = (u16, u16, i16, i16);
#[derive(Debug, Clone, Default, Serialize, Deserialize, OrderedData)]
#[serde(from = "FoliageValProxy", into = "FoliageValProxy")]
// height: u16, var_mask: u16, slope_x: i16, slope_z: i16
// position and orientation are randomized for each instance (within own square)
// var mask only checks high component vs alpha of vertex attr (if it exists)
pub struct FoliageVal(u16, u16, i16, i16);

impl From<FoliageVal> for FoliageValProxy {
    fn from(value: FoliageVal) -> Self {
        (value.0, value.1, value.2, value.3)
    }
}

impl From<FoliageValProxy> for FoliageVal {
    fn from(value: FoliageValProxy) -> Self {
        Self(value.0, value.1, value.2, value.3)
    }
}

impl <'py> IntoPyObject<'py> for FoliageVal {
    type Target = <FoliageValProxy as IntoPyObject<'py>>::Target;
    type Output = <FoliageValProxy as IntoPyObject<'py>>::Output;
    type Error = <FoliageValProxy as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        <FoliageValProxy>::from(self).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for FoliageVal {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(<FoliageValProxy>::extract_bound(ob)?.into())
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Foliage {
    pub vals: Vec<FoliageVal>
}

impl <'py> IntoPyObject<'py> for Foliage {
    type Target = <Vec<FoliageVal> as IntoPyObject<'py>>::Target;
    type Output = <Vec<FoliageVal> as IntoPyObject<'py>>::Output;
    type Error = <Vec<FoliageVal> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.vals.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Foliage {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { vals: Vec::extract_bound(ob)? })
    }
}

impl <'a> AsData<'a, '_> for Foliage {
    type InArgs = &'a FoliageInfo;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let n = (info.ub_w - info.lb_w) * (info.ub_h - info.lb_h);
        Ok(Self { vals: from_bytes!(V, &data[info.offset as usize..], n as usize)? })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(V, self.vals)
    }

    fn size<V: Version>(&self) -> usize {
        self.vals.size::<V>()
    }
}
