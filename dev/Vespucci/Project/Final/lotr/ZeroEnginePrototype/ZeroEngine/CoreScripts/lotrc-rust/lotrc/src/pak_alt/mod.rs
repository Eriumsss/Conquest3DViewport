use std::iter::zip;
use itertools::Itertools;
use log::warn;
use serde::{Serialize, Deserialize};
use serde_json::Value;
use gltf::{
    json::{Root, Index, validation::Checked, buffer::{View, Target, Buffer, Stride}, Accessor, accessor::GenericComponentType},
    accessor::{DataType, Dimensions},
};
use anyhow::{anyhow, Result};
use pyo3::prelude::*;
use indexmap::IndexMap;

use lotrc_proc::{OrderedData, basicpymethods, PyMethods};
use crate::{
    types::{Crc, Vector4, Version, PC, from_bytes, dump_bytes, AsData, NoArgs, GameObj, BaseTypes, Color, ANIMATION_EVENTS},
    pak::{
        ModelInfo, MatExtra, VBuffInfo, IBuffInfo, BufferInfo, HkConstraintInfo, HkConstraintData,
        ShapeInfo, Header, AnimationInfo, animation,  PFieldInfo,
        Mat1, Mat2, Mat3, Mat4, MatBase, RadiosityValsInfo
    },
};

mod shape;
pub use shape::*;
mod model;
pub use model::*;

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct Key2 {
    pub key: Crc,
    pub val: u32,
}

impl <'py> IntoPyObject<'py> for Key2 {
    type Target = <(Crc, u32) as IntoPyObject<'py>>::Target;
    type Output = <(Crc, u32) as IntoPyObject<'py>>::Output;
    type Error = <(Crc, u32) as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        (self.key, self.val).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Key2 {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        let (key, val) = <(Crc, u32)>::extract_bound(ob)?;
        Ok(Self { key, val })
    }
}

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, OrderedData, PyMethods)]
pub struct TRS {
    pub translation: Vector4,
    pub rotation: Vector4,
    pub scale: Vector4,
}

#[basicpymethods(no_bytes)]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct HkConstraint {
    pub info: HkConstraintInfo,
    pub bone_parents: Vec<i16>,
    pub bone_names: Vec<String>,
    //pub bone_names: Vec<(String, u32)>, // bones ?, always 0, replace with list of strings
    pub bone_transforms: Vec<TRS>, // probably f32
    pub vals2: Vec<f32>, // probably f32
    //pub bone_order: Vec<Key2>, // bones in order of increasing crc, number is index unsorted order
}

impl <'a> AsData<'_, 'a> for HkConstraint {
    type InArgs = usize;
    type OutArgs = (usize, u32, u32, &'a mut DumpInfos);

    fn from_bytes<O: Version>(data: &[u8], offset: Self::InArgs) -> Result<Self> {
        let info: HkConstraintInfo = from_bytes!(O, &data[offset..])?;
        if info.kind != 0 { panic!("Unknown & Unhandled HkConstraint type {} at offset {}", info.kind, offset); }

        let bone_parents: Vec<i16> = from_bytes!(O, &data[info.bone_parents_offset as usize..], info.bone_parents_num as usize)?;
        assert!(bone_parents[0] == -1);

        
        let string_offsets: Vec<u32> = from_bytes!(O, &data[info.bone_names_offset as usize..], info.bone_names_num as usize)?;
        let mut bone_names = Vec::with_capacity(string_offsets.len());
        for offset_ in string_offsets.iter() {
            let (mut offset, _val) = { 
                let vals: Vec<u32> = from_bytes!(O, &data[*offset_ as usize..], 2)?;
                (vals[0], vals[1])
            };
            let start = offset;
            while data[offset as usize] != 0 { offset += 1; }
            let string = String::from_utf8(data[start as usize..offset as usize].to_vec()).unwrap();
            bone_names.push(string);
        }
        let bone_transforms = from_bytes!(O, &data[info.bone_transforms_offset as usize..], info.bone_transforms_num as usize)?;
        let vals2 = from_bytes!(O, &data[info.vals2_offset as usize..], info.vals2_num as usize * 42)?;
        //let bone_order = from_bytes!(O, &data[info.bone_order_offset as usize..], info.bone_order_num as usize)?;
        Ok(Self {
            info, bone_parents, bone_names, bone_transforms, vals2
        })
    }

    fn dump_bytes<O: Version>(&self, (mut offset, bones_offset, bones_num, infos): Self::OutArgs) -> Vec<u8> {
        let mut info = self.info.clone();
        info.bones_num = bones_num as u16;
        info.bones_offset = bones_offset;
        let mut data = vec![];

        info.bone_names_offset = offset as u32;
        info.bone_names_num = self.bone_names.len() as u32;
        offset += 12 * self.bone_names.len();
        let off = (offset + 15) & 0xFFFFFFF0;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.bone_transforms_offset = offset as u32;
        info.bone_transforms_num = self.bone_transforms.len() as u32;
        let vals = dump_bytes!(O, self.bone_transforms);
        offset += vals.len();
        data.extend(vals);

        let bone_order: Vec<_> = self.bone_names.iter().enumerate().map(|(i,val)| Key2 { key: Crc::Str(val.clone()), val: i as u32 }).sorted_by_key(|val| val.key.key()).collect();
        info.bone_order_offset = offset as u32;
        info.bone_order_num = bone_order.len() as u16;
        let vals = dump_bytes!(O, bone_order);
        offset += vals.len();
        data.extend(vals);

        info.bone_parents_offset = offset as u32;
        info.bone_parents_num = self.bone_parents.len() as u32;
        let vals = dump_bytes!(O, self.bone_parents);
        offset += vals.len();
        data.extend(vals);

        let off: usize = (offset + 3) & 0xFFFFFFFC;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        let mut offsets = vec![];
        let mut string_offsets = vec![];
        let mut block2_off = info.bone_names_offset;
        let mut offset_off = info.bone_names_offset + 4 * self.bone_names.len() as u32;
        for string in &self.bone_names {
            let string = string.as_bytes();
            string_offsets.push([offset as u32, 0]);
            offset += string.len();
            data.extend(string);
            let off: usize = (offset + 4) & 0xFFFFFFFC;
            data.extend(vec![0u8; off-offset]);
            offset = off;

            offsets.push(offset_off);
            infos.block2_offsets.push(block2_off);
            infos.block2_offsets.push(offset_off);
            offset_off += 8;
            block2_off += 4;
        }
        let string_offsets = string_offsets.into_iter().flat_map(|x| x).collect::<Vec<_>>();

        if self.vals2.len() != 0 {
            info.vals2_offset = offset as u32;
            info.vals2_num = self.vals2.len() as u32 / 42;
            let vals = dump_bytes!(O, self.vals2);
            data.extend(vals);
        } else {
            info.vals2_num = 0;
            info.vals2_offset = 0;
        }

        infos.hk_constraint.push(info);
        dump_bytes!(O, offsets).into_iter().chain(dump_bytes!(O, string_offsets)).chain(data).collect()
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}


#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum Mat {
    //MaterialNormal
    Mat1(Mat1),
    //MaterialVariation
    Mat2(Mat2),
    //MaterialCharacterVariation
    Mat3(Mat3),
    //MaterialTerrain
    Mat4(Mat4),
}

impl Mat {
    pub fn base(&self) -> &MatBase {
        match self {
            Self::Mat1(mat) => &mat.base,
            Self::Mat2(mat) => &mat.base,
            Self::Mat3(mat) => &mat.base,
            Self::Mat4(mat) => &mat.base,
        }
    }
    
    pub fn base_mut(&mut self) -> &mut MatBase {
        match self {
            Self::Mat1(mat) => &mut mat.base,
            Self::Mat2(mat) => &mut mat.base,
            Self::Mat3(mat) => &mut mat.base,
            Self::Mat4(mat) => &mut mat.base,
        }
    }
}

impl AsData<'_, '_> for Mat {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let ty: u32 = if V::ps3() {
            from_bytes!(V, &data[200..])?
        } else {
            from_bytes!(V, &data[208..])?
        };
        Ok(match ty {
            0 => Self::Mat1(from_bytes!(V, &data[..])?),
            1 => Self::Mat4(from_bytes!(V, &data[..])?),
            2 => Self::Mat2(from_bytes!(V, &data[..])?),
            3 => Self::Mat3(from_bytes!(V, &data[..])?),
            _ => return Err(anyhow::anyhow!("Unknown Mat Type {}", ty))
        })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        match self {
            Self::Mat1(mat) => dump_bytes!(V, mat),
            Self::Mat2(mat) => dump_bytes!(V, mat),
            Self::Mat3(mat) => dump_bytes!(V, mat),
            Self::Mat4(mat) => dump_bytes!(V, mat),
        }
    }

    fn size<V: Version>(&self) -> usize {
        match self {
            Self::Mat1(mat) => mat.size::<V>(),
            Self::Mat2(mat) => mat.size::<V>(),
            Self::Mat3(mat) => mat.size::<V>(),
            Self::Mat4(mat) => mat.size::<V>(),
        }
    }
}

#[basicpymethods(no_bytes)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
#[pyclass(module="pak_alt", get_all, set_all)]
pub enum RadiosityVal {
    Radiosity(Vec<Color>),
    NoRadiosity(u32),
}

impl RadiosityVal {
    fn size<V: Version>(&self) -> usize {
        match self {
            Self::Radiosity(vals) => vals.size::<V>(),
            Self::NoRadiosity(_) => 0
        }
    }
}

#[basicpymethods(no_bytes)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
#[pyclass(module="pak_alt", get_all, set_all)]
pub struct Radiosity {
    pub vals: IndexMap<u32, Vec<RadiosityVal>>,
    pub usage: u32,
}

impl Radiosity {
    pub fn rad_size<V: Version>(&self) -> usize {
        self.vals.values().flat_map(|rads| rads.iter().map(|rad| rad.size::<V>())).sum::<usize>()
    }
}

impl <'a, 'b> AsData<'a, 'b> for Radiosity {
    type InArgs = (u32, Vec<RadiosityValsInfo>, &'a IndexMap<Crc, Model>, &'a IndexMap<u32, GameObj>, &'a Vec<u8>);
    type OutArgs = (usize, &'b mut Vec<u8>, &'b mut DumpInfos);

    fn from_bytes<V: Version>(data: &[u8], (usage, infos, models, objs, rad_data): Self::InArgs) -> Result<Self> {
        let mut vals = IndexMap::with_capacity(infos.len());
        let mut off = 0;
        for info in infos {
            let obj = objs.get(&info.guid).unwrap();
            let mesh = if let Some(BaseTypes::CRC(val)) = obj.fields.get(&Crc::Key(2550505638)) {
                Some(val)
            } else { None }.expect("level block obj mesh missing");
            let model = models.get(mesh).expect("model missing");
            let sizes: Vec<_> = model.buffer_infos.iter().map(|x| x.vbuff_size / x.v_size).collect();
            let offsets = from_bytes!(V, Vec<u32>, &data[info.offset as usize..], info.num as usize)?;
            let rads: Vec<_> = zip(offsets, sizes).map(|(offset, size)| {
                Ok(if offset != off {
                    RadiosityVal::NoRadiosity(offset)
                } else {
                    off += size;
                    RadiosityVal::Radiosity(from_bytes!(V, &rad_data[(offset * 4) as usize..], size as usize)?)
                })
            }).collect::<Result<_>>()?;

            vals.insert(info.guid, rads);
        }
        Ok(Self { vals, usage })
    }

    fn dump_bytes<V: Version>(&self, (mut offset, rad_data, infos): Self::OutArgs) -> Vec<u8> {
        let mut data = vec![];
        for (&guid, vals) in self.vals.iter() {
            let mut offsets = Vec::with_capacity(vals.len());
            for val in vals {
                let off = match val {
                    RadiosityVal::NoRadiosity(val) => *val,
                    RadiosityVal::Radiosity(val) => {
                        let off = rad_data.len();
                        let vals = dump_bytes!(V, val);
                        rad_data.extend(vals);
                        off as u32 / 4
                    }
                };
                offsets.push(off);
            }
            infos.radiosity_vals.push(RadiosityValsInfo { 
                guid, 
                num: vals.len() as u32, 
                offset: offset as u32,
            });
            let vals = dump_bytes!(V, offsets);
            offset += vals.len();
            data.extend(vals);
            let off = (offset + 15) & 0xFFFFFFF0;
            data.extend(vec![0u8; off-offset]);
            offset = off;
        }
        data
    }

    fn size<V: Version>(&self) -> usize {
        self.vals.values().map(|x| x.len() * V::size::<u32>()).sum::<usize>()
    }
}

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct AnimationEvent {
    pub event: Crc,
    pub t: f32,
    pub vals: [BaseTypes; 9],
}

impl Default for AnimationEvent {
    fn default() -> Self {
        Self { event: Crc::default(), t: 0.0, vals: [BaseTypes::Int(0), BaseTypes::Int(0), BaseTypes::Int(0), BaseTypes::Int(0), BaseTypes::Int(0), BaseTypes::Int(0), BaseTypes::Int(0), BaseTypes::Int(0), BaseTypes::Int(0)] }
    }
}

impl AsData<'_, '_> for AnimationEvent {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        use std::convert::TryInto;
        let mut offset = 0;
        let t: f32 = from_bytes!(V, &data[offset..])?;
        offset += t.size::<V>();
        let event: Crc = from_bytes!(V, &data[offset..])?;
        offset += event.size::<V>();
        let vals = ANIMATION_EVENTS.get(&event).iter().flat_map(|x| x.iter().cloned())
            .chain(std::iter::repeat(BaseTypes::INT_KEY)).take(9).map(|ty| {
            let val: BaseTypes = from_bytes!(V, &data[offset..], ty)?;
            offset += val.size::<V>();
            Ok(val)
        }).collect::<Result<Vec<_>>>()?.try_into().unwrap();
        Ok(Self { event, t, vals })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(V, self.t).into_iter()
            .chain(dump_bytes!(V, self.event))
            .chain(self.vals.iter().flat_map(|val| dump_bytes!(V, val)))
            .collect()
    }

    fn size<V: Version>(&self) -> usize {
        self.event.size::<V>() + self.t.size::<V>() + self.vals.iter().map(|x| x.size::<V>()).sum::<usize>()
    }
}

#[basicpymethods(no_bytes)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
#[pyclass(module="pak_alt", get_all, set_all)]
pub struct Animation {
    pub info: AnimationInfo,
    pub obj1: Vec<u32>,
    pub obj2: Vec<Vector4>,
    pub events: Vec<AnimationEvent>,
    pub bones: Vec<Crc>,
    pub obj5_a: Vec<animation::Obj5Val>,
    pub obj5_b: Vec<animation::Obj5Val>,
    pub obj_c3: Vec<u32>,
    pub obj_c4: Vec<u32>,
    pub blocks: Vec<(
        Vec<(
            animation::HkaSplineSkeletalAnimationObj1,
            animation::HkaSplineSkeletalAnimationObj2,
            animation::HkaSplineSkeletalAnimationObj1
        )>, 
        Vec<animation::HkaSplineSkeletalAnimationObj1>
    )>,
}

impl <'a, 'b> AsData<'a, 'b> for Animation {
    type InArgs = AnimationInfo;
    type OutArgs = (usize, &'b mut DumpInfos);

    fn from_bytes<V: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let obj1 = from_bytes!(V, &data[info.obj1_offset as usize..], info.obj1_num as usize * 2)?;
        let obj2 = from_bytes!(V, &data[info.obj2_offset as usize..], info.obj2_num as usize)?;
        let mut offset = info.obj3_offset as usize;
        let events = (0..info.obj3_num).into_iter().map(|_| {
            let val: AnimationEvent = from_bytes!(V, &data[offset..])?;
            offset += val.size::<V>();
            Ok(val)
        }).collect::<Result<Vec<_>>>()?;
        let bones = from_bytes!(V, &data[info.bones_offset as usize..], (info.vals_num + info.obj1_num) as usize)?;
        let (obj5_a, obj5_b) = if info.obj5_offset != 0 {
            let obj5_header: animation::Obj5Header = from_bytes!(V, &data[info.obj5_offset as usize..])?;
            let obj5_a = from_bytes!(V, &data[obj5_header.obj_a_offset as usize..], obj5_header.obj_a_num as usize)?;
            let obj5_b = from_bytes!(V, &data[obj5_header.obj_b_offset as usize..], obj5_header.obj_b_num as usize)?;
            (obj5_a, obj5_b)
        } else {
            (vec![], vec![])
        };
        let (obj_c3, obj_c4, blocks) = if info.kind == 3 {
            let block_starts: Vec<u32> = from_bytes!(V, &data[info.block_starts_offset as usize..], info.block_starts_num as usize)?;
            let block_starts2: Vec<u32> = from_bytes!(V, &data[info.block_starts2_offset as usize..], info.block_starts2_num as usize)?;
            let obj_c3 = from_bytes!(V, &data[info.obj_c3_offset as usize..], info.obj_c3_num as usize)?;
            let obj_c4 = from_bytes!(V, &data[info.obj_c4_offset as usize..], info.obj_c4_num as usize)?;
            let mut blocks = Vec::with_capacity(block_starts.len());
            for (start, start2) in zip(block_starts, block_starts2) {
                let mut off = (start + info.block_offset) as usize;
                let flags: Vec<animation::HkaSplineSkeletalAnimationFlags> = from_bytes!(V, &data[off..], info.vals_num as usize)?;
                let flags2: Vec<u8> = from_bytes!(V, &data[off + flags.size::<V>()..], info.vals2_num as usize)?;
                off = (info.block_offset + start + info.data_offset) as usize;
                let mut vals = Vec::with_capacity(flags.len());
                for flag in flags {
                    let a = from_bytes!(V, animation::HkaSplineSkeletalAnimationObj1, &data[off..], flag.a, flag.f & 3)?;
                    off += a.size::<V>();
                    off = (off + 3) & 0xfffffffc;
                    let b = from_bytes!(V, animation::HkaSplineSkeletalAnimationObj2, &data[off..], flag.b, (flag.f >> 2) & 0xf)?;
                    off += b.size::<V>();
                    off = (off + 3) & 0xfffffffc;
                    let c = from_bytes!(V, animation::HkaSplineSkeletalAnimationObj1, &data[off..], flag.c, (flag.f >> 6) & 3)?;
                    off += c.size::<V>();
                    off = (off + 3) & 0xfffffffc;
                    vals.push((a, b, c));
                }
                off = (info.block_offset + start + start2) as usize;
                let mut vals2 = Vec::with_capacity(flags2.len());
                for flag in flags2 {
                    let d = from_bytes!(V, animation::HkaSplineSkeletalAnimationObj1, &data[off..], flag & 0xf9, (flag >> 1) & 3)?;
                    off += d.size::<V>();
                    off = (off + 3) & 0xfffffffc;
                    vals2.push(d)
                }
                blocks.push((vals, vals2));
            }
            (obj_c3, obj_c4, blocks)
        } else if info.kind < 3 {
            warn!("Unhandled animation type {}", info.kind);
            (vec![], vec![], vec![])
        } else {
            warn!("Unknown animation type {}", info.kind);
            (vec![], vec![], vec![])
        };
        Ok(Self { info, obj1, obj2, events, bones, obj5_a, obj5_b, obj_c3, obj_c4, blocks })
    }

    fn dump_bytes<V: Version>(&self, (offset, infos): Self::OutArgs) -> Vec<u8> {
        let mut info = self.info.clone();
        let mut data = vec![];
        info.offset = offset as u32;
        info.obj1_offset = data.len() as u32;
        info.obj1_num = self.obj1.len() as u32 / 2;
        data.extend(dump_bytes!(V, self.obj1));
        info.obj2_offset = if self.obj2.is_empty() { 0 } else { data.len() as u32 };
        info.obj2_num = self.obj2.len() as u32;
        data.extend(dump_bytes!(V, self.obj2));
        
        info.vals_num = self.blocks[0].0.len() as u32;
        info.vals2_num = self.blocks[0].1.len() as u32;
        info.data_offset = ((self.blocks[0].0.len() * V::size::<animation::HkaSplineSkeletalAnimationFlags>()) + self.blocks[0].1.len()) as u32;
        let mut block_starts = Vec::with_capacity(self.blocks.len());
        let mut block_starts2 = Vec::with_capacity(self.blocks.len());
        let mut block = vec![];
        for (vals, vals2) in &self.blocks {
            let block_start = block.len() as u32;
            block_starts.push(block_start);
            let flags: Vec<_> = vals.iter().map(|(a,b,c)| {
                let f = a.vals.kind() | (b.vals.kind() << 2) | (c.vals.kind() << 6);
                animation::HkaSplineSkeletalAnimationFlags { a: a.flags, b: b.flags, c: c.flags, f }
            }).collect();
            let flags2: Vec<_> = vals2.iter().map(|d| d.flags | (d.vals.kind() << 1)).collect();
            block.extend(dump_bytes!(V, flags));
            block.extend(dump_bytes!(V, flags2));
            let off = (block.len() + 3) & 0xfffffffc;
            block.extend(vec![0u8; off-block.len()]);
            for (a, b, c) in vals {
                block.extend(dump_bytes!(V, a));
                let off = (block.len() + 3) & 0xfffffffc;
                block.extend(vec![0u8; off-block.len()]);
                block.extend(dump_bytes!(V, b));
                let off = (block.len() + 3) & 0xfffffffc;
                block.extend(vec![0u8; off-block.len()]);
                block.extend(dump_bytes!(V, c));
                let off = (block.len() + 3) & 0xfffffffc;
                block.extend(vec![0u8; off-block.len()]);
            }
            block_starts2.push(block.len() as u32 - block_start);
            for d in vals2 {
                block.extend(dump_bytes!(V, d));
                let off = (block.len() + 3) & 0xfffffffc;
                block.extend(vec![0u8; off-block.len()]);
            }
            let off = (block.len() + 15) & 0xfffffff0;
            block.extend(vec![0u8; off-block.len()]);
        }
        info.block_starts_offset = data.len() as u32;
        info.block_starts_num = self.blocks.len() as u32;
        data.extend(dump_bytes!(V, block_starts));
        info.block_starts2_offset = data.len() as u32;
        info.block_starts2_num = self.blocks.len() as u32;
        data.extend(dump_bytes!(V, block_starts2));
        info.obj_c3_offset = data.len() as u32;
        info.obj_c3_num = self.obj_c3.len() as u32;
        data.extend(dump_bytes!(V, self.obj_c3));
        info.obj_c4_offset = data.len() as u32;
        info.obj_c4_num = self.obj_c4.len() as u32;
        data.extend(dump_bytes!(V, self.obj_c4));
        info.block_size = block.len() as u32;
        info.block_offset = data.len() as u32;
        data.extend(block);
        if self.events.is_empty() {
            info.obj3_offset = 0;
            info.obj3_num = 0;
        } else {
            info.obj3_offset = data.len() as u32;
            info.obj3_num = self.events.len() as u32;
            for obj in &self.events {
                data.extend(dump_bytes!(V, obj))
            }
        }
        info.bones_offset = data.len() as u32;
        data.extend(dump_bytes!(V, self.bones));
        if !self.obj5_a.is_empty() || !self.obj5_b.is_empty() {
            info.obj5_offset = data.len() as u32;
            let header = animation::Obj5Header {
                obj_a_num: self.obj5_a.len() as u32,
                obj_a_offset: if self.obj5_a.len() != 0 {
                    (data.len() + V::size::<animation::Obj5Header>()) as u32
                } else { 0 },
                obj_b_num: self.obj5_b.len() as u32,
                obj_b_offset: if self.obj5_b.len() != 0 {
                    (data.len() + V::size::<animation::Obj5Header>() + (self.obj5_a.len() * V::size::<animation::Obj5Val>())) as u32
                } else {0}
            };
            data.extend(dump_bytes!(V, header));
            data.extend(dump_bytes!(V, self.obj5_a));
            data.extend(dump_bytes!(V, self.obj5_b));
        } else {
            info.obj5_offset = 0;
        }
        let off = (data.len() + 15) & 0xfffffff0;
        data.extend(vec![0u8; off-data.len()]);
        info.size = data.len() as u32;
        infos.animation.push(info);
        data
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}

impl Animation {
    pub fn from_data<O: Version + 'static>(info: AnimationInfo, offsets: &mut Vec<usize>, blocks: & Vec<Vec<u8>>) -> Result<Self> {
        let (_, (offset, block)) = zip(offsets.iter().cloned(), blocks.iter()).enumerate().find(|(i, _)| {
            info.gamemodemask & (1 << i) != 0
        }).unwrap();
        let val = from_bytes!(O, Animation, &block[offset..], info)?;
        offsets.iter_mut().enumerate().filter(|(i, _)| val.info.gamemodemask & (1 << i) != 0).for_each(|(_, x)| *x += val.info.size as usize );
        Ok(val)
    }
}

#[basicpymethods(no_bytes)]
#[pyclass(get_all, set_all)]
#[derive(Default, Clone, Debug, Serialize, Deserialize, PyMethods)]
pub struct DumpInfos {
    pub header: Header,
    pub animation: Vec<AnimationInfo>,
    pub hk_shape: Vec<HkShapeInfo>,
    pub shape: Vec<ShapeInfo>,
    pub model: Vec<ModelInfo>,
    pub mat1: Vec<Mat1>,
    pub mat2: Vec<Mat2>,
    pub mat3: Vec<Mat3>,
    pub mat4: Vec<Mat4>,
    pub mat_extra: Vec<MatExtra>,
    pub hk_constraint: Vec<HkConstraintInfo>,
    pub hk_constraint_data: Vec<HkConstraintData>,
    pub vbuff: Vec<VBuffInfo>,
    pub ibuff: Vec<IBuffInfo>,
    pub buffer: Vec<BufferInfo>,
    pub radiosity_vals: Vec<RadiosityValsInfo>,
    pub pfields: Vec<PFieldInfo>,
    pub block2_offsets: Vec<u32>,
}

pub struct GltfAsset {
    pub data: Vec<u8>,
    pub count: usize,
    pub stride: Option<usize>,
    pub target: Option<Target>,
    pub ty: DataType,
    pub dim: Dimensions,
    pub min: Option<Value>,
    pub max: Option<Value>,
    pub normalized: bool,
    pub extras: Option<Value>,
}

impl Default for GltfAsset {
    fn default() -> Self {
        Self {
            data: vec![],
            count: 0,
            stride: None,
            target: None,
            ty: DataType::U32,
            dim: Dimensions::Scalar,
            min: None,
            max: None,
            normalized: false,
            extras: None,
        }
    }
}

impl GltfAsset {
    pub fn to_gltf(self, root: &mut Root, bin: &mut Vec<u8>) -> Index<Accessor> {
        assert!(self.data.len() % self.count == 0);
        let s = match self.ty {
            DataType::I8 | DataType::U8 => 1,
            DataType::I16 | DataType::U16 => 2,
            _ => 4
        };
        bin.extend(vec![0u8; ((bin.len() + s - 1) & (0xFFFFFFFF - s + 1)) - bin.len()]);

        let buffer_view = root.push(View {
            name: None,
            buffer: Index::<Buffer>::new(0),
            byte_length: self.data.len().into(),
            byte_offset: Some(bin.len().into()),
            byte_stride: self.stride.map(|x| Stride(x)),
            target: self.target.map(|x| Checked::Valid(x)),
            extensions: None,
            extras: Default::default(),
        });
        bin.extend(self.data);

        root.push(gltf::json::Accessor {
            name: None,
            buffer_view: Some(buffer_view),
            byte_offset: None,
            count: self.count.into(),
            component_type: Checked::Valid(GenericComponentType(self.ty)),
            extensions: None,
            extras: self.extras.map(|x| serde_json::value::to_raw_value(&x).unwrap()),
            type_: Checked::Valid(self.dim),
            min: self.min,
            max: self.max,
            normalized: self.normalized,
            sparse: None,
        })
    }
}

pub struct GltfData<'a> {
    pub ty: DataType,
    pub m: usize,
    pub n: usize,
    pub stride: usize,
    pub bin: &'a [u8],
}

impl <'a> GltfData<'a> {
    pub fn from_buffer(i: Index<Accessor>, root: &Root, bin: &'a [u8]) -> Self {
        let accessor = root.get(i).unwrap();
        let buffer_view = root.get(accessor.buffer_view.unwrap()).unwrap();

        let ty = accessor.component_type.unwrap().0;
        let dim = accessor.type_.unwrap();
        let n = accessor.count.0 as usize;
        let m = dim.multiplicity();
        let stride = buffer_view.byte_stride.map(|x| x.0).unwrap_or_else(|| ty.size() * m);
        let off = (buffer_view.byte_offset.map(|x| x.0).unwrap_or(0) + accessor.byte_offset.map(|x| x.0).unwrap_or(0)) as usize;
        Self {
            ty, m, n, stride, bin: &bin[off..]
        }
    }

    pub fn u8(self) -> Result<Vec<u8>> {
        match self.ty {
            DataType::U8 => (0..self.n).map(|x| Vec::<u8>::from_bytes::<PC>(&self.bin[x * self.stride..], self.m)).flatten_ok().collect::<Result<Vec<_>>>(),
            _ => Result::Err(anyhow!("Invalid Data Type {:?} for U8", self.ty)) 
        }
    }

    #[allow(dead_code)]
    pub fn i8(self) -> Result<Vec<i8>> {
        match self.ty {
            DataType::I8 => (0..self.n).map(|x| Vec::<i8>::from_bytes::<PC>(&self.bin[x * self.stride..], self.m)).flatten_ok().collect::<Result<Vec<_>>>(),
            _ => Result::Err(anyhow!("Invalid Data Type {:?} for I8", self.ty)) 
        }
    }

    pub fn u16(self) -> Result<Vec<u16>> {
        match self.ty {
            DataType::U16 => (0..self.n).map(|x| Vec::<u16>::from_bytes::<PC>(&self.bin[x * self.stride..], self.m)).flatten_ok().collect::<Result<Vec<_>>>(),
            _ => Result::Err(anyhow!("Invalid Data Type {:?} for U16", self.ty)) 
        }
    }

    pub fn i16(self) -> Result<Vec<i16>> {
        match self.ty {
            DataType::I16 => (0..self.n).map(|x| Vec::<i16>::from_bytes::<PC>(&self.bin[x * self.stride..], self.m)).flatten_ok().collect::<Result<Vec<_>>>(),
            _ => Result::Err(anyhow!("Invalid Data Type {:?} for I16", self.ty)) 
        }
    }

    pub fn u32(self) -> Result<Vec<u32>> {
        match self.ty {
            DataType::U32 => (0..self.n).map(|x| Vec::<u32>::from_bytes::<PC>(&self.bin[x * self.stride..], self.m)).flatten_ok().collect::<Result<Vec<_>>>(),
            _ => Result::Err(anyhow!("Invalid Data Type {:?} for U32", self.ty)) 
        }
    }

    pub fn f32(self) -> Result<Vec<f32>> {
        match self.ty {
            DataType::F32 => (0..self.n).map(|x| Vec::<f32>::from_bytes::<PC>(&self.bin[x * self.stride..], self.m)).flatten_ok().collect::<Result<Vec<_>>>(),
            _ => Result::Err(anyhow!("Invalid Data Type {:?} for F32", self.ty)) 
        }
    }
}
