use std::{collections::{HashMap, HashSet}, iter::zip};
use itertools::Itertools;
use serde::{Serialize, Deserialize};
use serde_json::json;
use gltf::{
    json::{Root, Index, validation::Checked, buffer::Target, Accessor, Node, accessor::ComponentType},
    accessor::{DataType, Dimensions},
};
use anyhow::{Result, anyhow};
use pyo3::prelude::*;

use lotrc_proc::{basicpymethods, PyMethods};
use crate::{
    types::{Crc, Matrix4x4, Version, PC, from_bytes, dump_bytes, AsData},
    pak::{
        ModelInfo, BoundingBox, MatExtra, VBuffInfo, IBuffInfo, BufferInfo, HkConstraintInfo, HkConstraintData,
        VertexBuffer, IndexBuffer, ShapeInfo, VertexUsage, LodMeshes, Mat1, Mat2, Mat3, Mat4 
    },
};

use super::*;

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct BlockHeader1 {
    pub a: u32,
    pub b: u32,
    pub unk_2: u32,
    pub unk_3: u32,
}

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct BlockHeader2 {
    pub n: u32,
    pub unk_1: f32,
    pub unk_2: f32,
    pub unk_3: u32,
    pub unk_4: u32,
}

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct BlockValA {
    pub unk_0: f32,
    pub unk_1: f32,
    pub unk_2: f32,
    pub unk_3: f32,
    pub unk_4: f32,
    pub unk_5: f32,
    pub unk_6: f32,
    pub unk_7: f32,
    pub unk_8: f32,
    pub unk_9: u32,
    pub unk_10: u32,
    pub unk_11: u32,
}

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct BlockValB {
    pub unk_0: u16,
    pub unk_1: u16,
    pub unk_2: f32,
    pub unk_3: f32,
    pub unk_4: f32,
    pub unk_5: f32,
}

#[basicpymethods(no_bytes)]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct Block {
    pub info1: BlockHeader1,
    pub info2: BlockHeader2,
    pub vals_a: Vec<BlockValA>,
    pub vals_b: Vec<BlockValA>,
    pub vals_c: Vec<BlockValB>,
    pub pad: Vec<u8>
}

impl AsData<'_, '_> for Block {
    type InArgs = usize;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], size: Self::InArgs) -> Result<Self> {
        let mut offset = 0;
        let info1: BlockHeader1 = from_bytes!(V, &data[offset..])?;
        offset += info1.size::<V>();
        let vals_a: Vec<BlockValA> = from_bytes!(V, &data[offset..], info1.a as usize)?;
        offset += vals_a.size::<V>();
        let vals_b: Vec<BlockValA> = from_bytes!(V, &data[offset..], info1.b as usize)?;
        offset += vals_b.size::<V>();
        let info2: BlockHeader2 = from_bytes!(V, &data[offset..])?;
        offset += info2.size::<V>();
        let vals_c: Vec<BlockValB> = from_bytes!(V, &data[offset..], info2.n as usize)?;
        offset += vals_c.size::<V>();
        let pad = data[offset..size].to_vec();
        Ok(Self { info1, info2, vals_a, vals_b, vals_c, pad })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(V, self.info1).into_iter()
            .chain(dump_bytes!(V, self.vals_a))
            .chain(dump_bytes!(V, self.vals_b))
            .chain(dump_bytes!(V, self.info2))
            .chain(dump_bytes!(V, self.vals_c))
            .chain(dump_bytes!(V, self.pad))
            .collect()
    }

    fn size<V: Version>(&self) -> usize {
        self.info1.size::<V>() + self.vals_a.size::<V>() + self.vals_b.size::<V>()
            + self.info2.size::<V>() + self.vals_c.size::<V>() + self.pad.size::<V>()
    }
}

#[basicpymethods(no_bytes)]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct Model {
    pub info: ModelInfo,
    pub bone_parents: Vec<i32>, // parent bone
    pub bones: Vec<Crc>, // bone names
    pub bone_transforms: Vec<Matrix4x4>, // relative bone transforms, used for static meshes 
    pub bone_bounding_boxes: Vec<BoundingBox>,
    pub mat_order: Vec<u32>,
    pub mesh_order: Vec<u32>, // order of models (mapped to lod0, lod1, lod2, lod3)
    pub mesh_bounding_boxes: Vec<BoundingBox>,
    pub skin_binds: Vec<Matrix4x4>, // mat4, bind matrices or something??
    pub vals_j: Vec<u32>, // bows & banners, maybe for strings?
    pub val_k_header: Vec<u16>,
    pub vals_k: Vec<u32>, // has to do with trees
    pub skin_order: Vec<u32>, // bone mapping for vals_g, seems to be the mapping used for skinning
    pub slots: Vec<Key2>, // attachment points
    pub slot_map: Vec<u32>, // attachment bone mapping
    pub block_header: Option<u32>, // has to do with havok cloth / hair stuff
    pub block_offsets: Vec<u32>,
    pub blocks: Vec<Block>,
    pub mats: Vec<Mat>,
    pub mat_extras: Vec<Option<MatExtra>>,
    pub buffer_infos: Vec<BufferInfo>,
    pub hk_constraint: Option<HkConstraint>, // stores bone transforms used for ragdoll ??
    pub hk_constraint_datas: Vec<HkConstraintData>,
    pub shapes: Vec<Shape>,
    pub vertex_data: Vec<VertexBuffer>,
    pub index_data: Vec<IndexBuffer>,
}

impl Model {
    pub fn from_data<O: Version + 'static>(info: ModelInfo, data: &[u8], model_data: &HashMap<Crc, Vec<u8>>) -> Result<Self> {
        let bone_parents: Vec<i32> = from_bytes!(O, &data[info.bone_parents_offset as usize..], info.bones_num as usize)?;
        let bones: Vec<Crc> = if info.bones_offset != 0 {
            from_bytes!(O, &data[info.bones_offset as usize..], info.bones_num as usize)?
        } else { vec![Crc::Key(0); info.bones_num as usize] };
        let bone_transforms: Vec<Matrix4x4> = from_bytes!(O, &data[info.bone_transforms_offset as usize..], info.bones_num as usize)?;
        let bone_bounding_boxes: Vec<BoundingBox> = from_bytes!(O, &data[info.bone_bounding_boxes_offset as usize..], info.bones_num as usize)?;
        let mut mat_order: Vec<u32> = from_bytes!(O, &data[info.mat_offset as usize..], info.mat_num as usize)?;
        let mesh_order: Vec<u32> = from_bytes!(O, &data[info.mesh_order_offset as usize..], info.lod3.breakable_end as usize)?;
        let mesh_bounding_boxes: Vec<BoundingBox> = from_bytes!(O, &data[info.mesh_bounding_boxes_offset as usize..], info.lod3.breakable_end as usize)?;
        let mut vbuff_order: Vec<u32> = from_bytes!(O, &data[info.vbuff_offset as usize..], info.vbuff_num as usize)?;
        let mut ibuff_order: Vec<u32> = from_bytes!(O, &data[info.ibuff_offset as usize..], info.ibuff_num as usize)?;
        let skin_binds: Vec<Matrix4x4> = from_bytes!(O, &data[info.skin_binds_offset as usize..], info.skin_binds_num as usize)?;
        let vals_j: Vec<u32> = from_bytes!(O, &data[info.vals_j_offset as usize..], info.vals_j_num as usize)?;
        let (val_k_header, vals_k) = if info.vals_k_offset != 0 {(
            from_bytes!(O, &data[info.vals_k_offset as usize..], 2)?,
            from_bytes!(O, &data[info.vals_k_offset as usize + 4..], 35)?
        )} else {(
            Vec::new(), Vec::new()
        )};
        let skin_order = if info.skin_order_offset != 0 {
            from_bytes!(O, &data[info.skin_order_offset as usize..], info.skin_binds_num as usize)?
        } else {
            Vec::new()
        };
        let (slots, slot_map) = if info.slots_offset != 0 {
            assert!(info.slot_map_offset != 0);
            let mut i = 0;
            {
                while from_bytes!(O, u32, &data[info.slots_offset as usize + i * 8..])? != 0 {
                    i += 1;
                }
                i += 1;
            }
            let keys2: Vec<Key2> = from_bytes!(O, &data[info.slots_offset as usize..], i)?;
            let keys2_order = from_bytes!(O, &data[info.slot_map_offset as usize..], keys2.last().unwrap().val as usize)?;
            (keys2, keys2_order)
        } else {(
            Vec::new(), Vec::new()
        )};
        let (block_header, block_offsets, blocks) = if info.block_offset != 0 {
            let block_header = from_bytes!(O, &data[info.block_offset as usize..])?;
            let n = (info.lod0.physics_end - info.lod0.skinned_end) as usize;
            let block_offsets: Vec<u32> = from_bytes!(O, &data[info.block_offset as usize + 4..], n+1)?;
            let mut blocks = Vec::with_capacity(n);
            for i in 0..n {
                let size = (block_offsets[i+1] - block_offsets[i]) as usize;
                let offset = (block_offsets[i] + info.block_offset) as usize;
                blocks.push(from_bytes!(O, model::Block, &data[offset..], size)?);
            }
            (Some(block_header), block_offsets, blocks)
        } else {(
            None, Vec::new(), Vec::new()
        )};

        assert!(bone_parents[0] == -1);

        let shapes = (0..info.shape_num as usize).map(|i| 
            from_bytes!(O, Shape, data, info.shape_offset as usize + i * O::size::<ShapeInfo>())
        ).collect::<Result<Vec<_>>>()?;

        let hk_constraint = if info.hk_constraint_offset != 0 {
            Some(from_bytes!(O, HkConstraint, data, info.hk_constraint_offset as usize)?)
        } else { None };
        let hk_constraint_datas: Vec<HkConstraintData> = from_bytes!(O, &data[info.hk_constraint_data_offset as usize..], info.hk_constraint_data_num as usize)?;
        
        let mats = HashSet::<u32>::from_iter(mat_order.iter().cloned()).into_iter().sorted().collect::<Vec<_>>();
        let mat_map: HashMap<_, _> = mats.iter().enumerate().map(|(i, x)| (*x, i as u32)).collect();
        mat_order.iter_mut().for_each(|x| *x = *mat_map.get(x).unwrap());
        let mats: Vec<Mat> = mats.into_iter().map(|off| from_bytes!(O, Mat, &data[off as usize..])).collect::<Result<Vec<_>>>()?;
        let mat_extras: Vec<_> = mats.iter().map(|x| 
            Ok(if x.base().mat_extra_offset != 0 { Some(from_bytes!(O, &data[x.base().mat_extra_offset as usize..])?) } else { None })
        ).collect::<Result<Vec<_>>>()?;

        let vbuffs = HashSet::<u32>::from_iter(vbuff_order.iter().cloned()).into_iter().sorted().collect::<Vec<_>>();
        let mut vbuff_map: HashMap<_, _> = vbuffs.iter().enumerate().map(|(i, x)| (*x, i as u32)).collect();
        vbuff_order.iter_mut().for_each(|x| *x = *vbuff_map.get(x).unwrap());
        let vbuffs: Vec<VBuffInfo> = vbuffs.into_iter().map(|off| from_bytes!(O, &data[off as usize..])).collect::<Result<Vec<_>>>()?;

        let ibuffs = HashSet::<u32>::from_iter(ibuff_order.iter().cloned()).into_iter().sorted().collect::<Vec<_>>();
        let mut ibuff_map: HashMap<_, _> = ibuffs.iter().enumerate().map(|(i, x)| (*x, i as u32)).collect();
        ibuff_order.iter_mut().for_each(|x| *x = *ibuff_map.get(x).unwrap());
        let ibuffs: Vec<IBuffInfo> = ibuffs.into_iter().map(|off| from_bytes!(O, &data[off as usize..])).collect::<Result<Vec<_>>>()?;
        let (vertex_data, index_data) = if info.vbuff_num != 0 || info.ibuff_num != 0 {
            let buffer = model_data.get(&info.asset_key).unwrap();
            (
                vbuffs.into_iter().map(|info|
                    from_bytes!(O, VertexBuffer, &buffer[..], info)
                ).collect::<Result<Vec<_>>>()?,
                ibuffs.into_iter().map(|info| 
                    from_bytes!(O, IndexBuffer, &buffer[..], &info)
                ).collect::<Result<Vec<_>>>()?,
            )
        } else {
            (vec![], vec![])
        };

        ibuff_map.insert(0, 0xFFFFFFFF);
        vbuff_map.insert(0, 0xFFFFFFFF);
        let mut buffer_infos: Vec<BufferInfo> = from_bytes!(O, &data[info.buffer_info_offset as usize..], info.mat_num as usize)?;
        buffer_infos.iter_mut().for_each(|buff| {
            buff.vbuff_info_offset = *vbuff_map.get(&buff.vbuff_info_offset).unwrap_or(&buff.vbuff_info_offset);
            buff.vbuff_info_offset_2 = *vbuff_map.get(&buff.vbuff_info_offset_2).unwrap_or(&buff.vbuff_info_offset_2);
            buff.vbuff_info_offset_3 = *vbuff_map.get(&buff.vbuff_info_offset_3).unwrap_or(&buff.vbuff_info_offset_3);
            buff.ibuff_info_offset = *ibuff_map.get(&buff.ibuff_info_offset).unwrap_or(&buff.ibuff_info_offset);
        });

        Ok(Self {
            info,
            bone_parents,
            bones,
            bone_transforms,
            mat_order,
            bone_bounding_boxes,
            mesh_order,
            mesh_bounding_boxes,
            skin_binds,
            vals_j,
            val_k_header,
            vals_k,
            skin_order,
            slots,
            slot_map,
            block_header,
            block_offsets,
            blocks,
            mats,
            mat_extras,
            buffer_infos,
            shapes,
            hk_constraint,
            hk_constraint_datas,
            vertex_data,
            index_data,
        })
    }

    fn dump_mesh_data<O: Version + 'static>(&self, infos: &mut DumpInfos) -> (Vec<u8>, Vec<u32>, Vec<u32>) {
        // size, offset, vsize, data, fmt_alt
        let mut tot_size = 0;
        let mut vbuffs_info: Vec<_> = self.vertex_data.iter().map(|data| {
            let v_size = data.v_size() as u32;
            let vals = dump_bytes!(O, data);
            let size = vals.len();
            tot_size += size + 16;
            (size as u32, None, v_size, vals, (data.info.fmt1 & 0x40000) != 0)
        }).collect();

        // size, offset, num, data, ty
        let mut ibuffs_info: Vec<_> = self.index_data.iter().map(|data| {
            let num = data.len();
            let vals = dump_bytes!(O, data);
            let size = vals.len();
            tot_size += size + 16;
            let ty = match data {
                IndexBuffer::U16 { .. } => 16u32,
                IndexBuffer::U32 { .. } => 32u32,
            };
            (size as u32, None, num as u32, vals, ty)
        }).collect();

        let vbuff_order: Vec<u32> = (0..self.vertex_data.len()).map(|x| infos.header.vbuff_info_offset + (O::size::<VBuffInfo>() * (infos.vbuff.len() + x)) as u32).collect();
        let mut vbuff_map: HashMap<_, _> = vbuff_order.iter().enumerate().map(|(i,x)| (i as u32, *x)).collect();
        vbuff_map.insert(0xFFFFFFFF, 0);

        let ibuff_order: Vec<u32> = (0..self.index_data.len()).map(|x| infos.header.ibuff_info_offset + (O::size::<IBuffInfo>() * (infos.ibuff.len() + x)) as u32).collect();
        let mut ibuff_map: HashMap<_, _> = ibuff_order.iter().enumerate().map(|(i,x)| (i as u32, *x)).collect();
        ibuff_map.insert(0xFFFFFFFF, 0);

        let mut buffer_infos = self.buffer_infos.clone();
        let mut vbuffs: Vec<_> = self.vertex_data.iter().map(|x| x.info.clone()).collect();
        let mut ibuffs: Vec<_> = self.index_data.iter().map(|_| IBuffInfo::default()).collect();
        
        let mut data = Vec::with_capacity(tot_size);
        for buffer_info in &mut buffer_infos {
            let (vbuff_size, offset, v_size, vals, alt) = &mut vbuffs_info[buffer_info.vbuff_info_offset as usize];
            if *alt {
                ibuffs[buffer_info.ibuff_info_offset as usize].vbuff_alt_fmt = 1;
            }
            (buffer_info.vbuff_size, buffer_info.v_size) = (*vbuff_size, *v_size);
            if offset.is_none() {
                data.extend(vec![0u8; ((data.len() + 15) & 0xFFFFFFF0) - data.len()]);
                offset.replace(data.len());
                data.extend(vals.drain(..));
            }
            if buffer_info.vbuff_info_offset_2 != 0xFFFFFFFF {
                let (vbuff_size, offset, v_size, vals, _) = &mut vbuffs_info[buffer_info.vbuff_info_offset_2 as usize];
                (buffer_info.vbuff_size_2, buffer_info.v_size_2) = (*vbuff_size, *v_size);
                if offset.is_none() {
                    data.extend(vec![0u8; ((data.len() + 15) & 0xFFFFFFF0) - data.len()]);
                    offset.replace(data.len());
                    data.extend(vals.drain(..));
                }
            }
            if buffer_info.vbuff_info_offset_3 != 0xFFFFFFFF {
                let (vbuff_size, offset, v_size, vals, _) = &mut vbuffs_info[buffer_info.vbuff_info_offset_3 as usize];
                (buffer_info.vbuff_size_3, buffer_info.v_size_3) = (*vbuff_size, *v_size);
                if offset.is_none() {
                    data.extend(vec![0u8; ((data.len() + 15) & 0xFFFFFFF0) - data.len()]);
                    offset.replace(data.len());
                    data.extend(vals.drain(..));
                }
            }
            if buffer_info.ibuff_info_offset != 0xFFFFFFFF {
                let (_, offset, num, vals, _) = &mut ibuffs_info[buffer_info.ibuff_info_offset as usize];
                (buffer_info.i_num, buffer_info.tri_num) = (*num, (*num)/3);
                if offset.is_none() {
                    data.extend(vec![0u8; ((data.len() + 15) & 0xFFFFFFF0) - data.len()]);
                    offset.replace(data.len());
                    data.extend(vals.drain(..));
                }
            }
            buffer_info.vbuff_info_offset = *vbuff_map.get(&buffer_info.vbuff_info_offset).unwrap();
            buffer_info.vbuff_info_offset_2 = *vbuff_map.get(&buffer_info.vbuff_info_offset_2).unwrap();
            buffer_info.vbuff_info_offset_3 = *vbuff_map.get(&buffer_info.vbuff_info_offset_3).unwrap();
            buffer_info.ibuff_info_offset = *ibuff_map.get(&buffer_info.ibuff_info_offset).unwrap();
        }
    
        for (info, (size, offset, _, _, _)) in vbuffs.iter_mut().zip(vbuffs_info) {
            info.size = size;
            info.offset = offset.unwrap_or(0) as u32;
        }
        for (info, (size, offset, _, _, ty)) in ibuffs.iter_mut().zip(ibuffs_info) {
            info.size = size;
            info.offset = offset.unwrap_or(0) as u32;
            info.format = ty;
        }

        infos.vbuff.extend(vbuffs);
        infos.ibuff.extend(ibuffs);
        infos.buffer.extend(buffer_infos);

        (data, vbuff_order, ibuff_order)
    }

    pub fn dump<O: Version + 'static>(&self, mut offset: usize, infos: &mut DumpInfos) -> (Vec<u8>, Vec<u8>) {
        let mut info = self.info.clone();
        let mut data = vec![];

        let mut mat_order = self.mat_order.clone();
        let mut mat_map = HashMap::with_capacity(self.mats.len());
        for (i, (mat, mat_extra)) in zip(&self.mats, &self.mat_extras).enumerate() {
            let mut mat = mat.clone();
            if let Some(mat_extra) = mat_extra {
                mat.base_mut().mat_extra_offset = infos.header.mat_extra_offset + (O::size::<MatExtra>() * infos.mat_extra.len()) as u32;
                infos.mat_extra.push(mat_extra.clone())
            }
            match mat {
                Mat::Mat1(mat) => {
                    mat_map.insert(i as u32, infos.header.mat1_offset + (O::size::<Mat1>() * infos.mat1.len()) as u32);
                    infos.mat1.push(mat);
                },
                Mat::Mat2(mat) => {
                    mat_map.insert(i as u32, infos.header.mat2_offset + (O::size::<Mat2>() * infos.mat2.len()) as u32);
                    infos.mat2.push(mat);
                },
                Mat::Mat3(mat) => {
                    mat_map.insert(i as u32, infos.header.mat3_offset + (O::size::<Mat3>() * infos.mat3.len()) as u32);
                    infos.mat3.push(mat);
                },
                Mat::Mat4(mat) => {
                    mat_map.insert(i as u32, infos.header.mat4_offset + (O::size::<Mat4>() * infos.mat4.len()) as u32);
                    infos.mat4.push(mat);
                },
            }
        }
        mat_order.iter_mut().for_each(|x| *x = *mat_map.get(x).unwrap());

        let (mesh_data, vbuff_order, ibuff_order) = self.dump_mesh_data::<O>(infos);
        info.buffer_info_offset = infos.header.buffer_info_offset + (O::size::<BufferInfo>() * (infos.buffer.len() - self.buffer_infos.len())) as u32;

        info.hk_constraint_data_num = self.hk_constraint_datas.len() as u32;
        info.hk_constraint_data_offset = if self.hk_constraint_datas.len() != 0 {
            infos.header.hk_constraint_data_offset + (O::size::<HkConstraintData>() * infos.hk_constraint_data.len()) as u32
        } else { 0 };
        infos.hk_constraint_data.extend(self.hk_constraint_datas.clone());

        info.bones_offset = offset as u32;
        info.bones_num = self.bones.len() as u32;
        let vals = dump_bytes!(O, self.bones);
        offset += vals.len();
        data.extend(vals);

        let off = (offset+ 15) & 0xFFFFFFF0;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.bone_bounding_boxes_offset = offset as u32;
        let vals = dump_bytes!(O, self.bone_bounding_boxes);
        offset += vals.len();
        data.extend(vals);

        info.vals_j_offset = offset as u32;
        info.vals_j_num = self.vals_j.len() as u32;
        let vals = dump_bytes!(O, self.vals_j);
        offset += vals.len();
        data.extend(vals);

        if let Some(hk_constraint) = &self.hk_constraint {
            info.hk_constraint_offset = infos.header.hk_constraint_info_offset + (O::size::<HkConstraintInfo>() * infos.hk_constraint.len()) as u32;
            let vals = dump_bytes!(O, hk_constraint, offset, info.bones_offset, info.bones_num, infos);
            offset += vals.len();
            data.extend(vals);
        } else {
            info.hk_constraint_offset = 0;
        }

        let off = (offset+ 15) & 0xFFFFFFF0;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.skin_binds_offset = offset as u32;
        info.skin_binds_num = self.skin_binds.len() as u32;
        let vals = dump_bytes!(O, self.skin_binds);
        offset += vals.len();
        data.extend(vals);

        if self.skin_order.len() != 0 {
            info.skin_order_offset = offset as u32;
            let vals = dump_bytes!(O, self.skin_order);
            offset += vals.len();
            data.extend(vals);
        } else {
            info.skin_order_offset = 0;
        }

        info.bone_parents_offset = offset as u32;
        let vals = dump_bytes!(O, self.bone_parents);
        offset += vals.len();
        data.extend(vals);

        let off = (offset+ 15) & 0xFFFFFFF0;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.bone_transforms_offset = offset as u32;
        let vals = dump_bytes!(O, self.bone_transforms);
        offset += vals.len();
        data.extend(vals);

        info.mat_offset = offset as u32;
        info.mat_num = mat_order.len() as u32;
        infos.block2_offsets.extend((0..mat_order.len() as u32).map(|x| offset as u32 + x * 4));
        let vals = dump_bytes!(O, mat_order);
        offset += vals.len();
        data.extend(vals);

        info.shape_num = self.shapes.len() as u32;
        info.shape_offset = if self.shapes.len() != 0 {
            infos.header.shape_info_offset + (O::size::<ShapeInfo>() * infos.shape.len()) as u32
        } else { 0 };
        for shape in &self.shapes {
            let vals = dump_bytes!(O, shape, offset, None, infos);
            offset += vals.len();
            data.extend(vals);    
        }

        info.mesh_order_offset = offset as u32;
        //info.lod3.breakable_end = self.mesh_order.len() as u32;
        let vals = dump_bytes!(O, self.mesh_order);
        offset += vals.len();
        data.extend(vals);

        let off = (offset+ 15) & 0xFFFFFFF0;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.mesh_bounding_boxes_offset = offset as u32;
        let vals = dump_bytes!(O, self.mesh_bounding_boxes);
        offset += vals.len();
        data.extend(vals);    

        info.vbuff_offset = offset as u32;
        info.vbuff_num = vbuff_order.len() as u32;
        infos.block2_offsets.extend((0..vbuff_order.len() as u32).map(|x|  offset as u32 + x * 4));
        let vals = dump_bytes!(O, vbuff_order);
        offset += vals.len();
        data.extend(vals);

        info.ibuff_offset = offset as u32;
        info.ibuff_num = ibuff_order.len() as u32;
        infos.block2_offsets.extend((0..ibuff_order.len() as u32).map(|x|  offset as u32 + x * 4));
        let vals = dump_bytes!(O, ibuff_order);
        offset += vals.len();
        data.extend(vals);

        if self.val_k_header.len() != 0 {
            let off = (offset+ 15) & 0xFFFFFFF0;
            data.extend(vec![0u8; off-offset]);
            offset = off;

            info.vals_k_offset = offset as u32;
            let vals = dump_bytes!(O, self.val_k_header);
            offset += vals.len();
            data.extend(vals);
            let vals = dump_bytes!(O, self.vals_k);
            offset += vals.len();
            data.extend(vals);
        }

        if self.slots.len() != 0 {
            info.slots_offset = offset as u32;
            let vals = dump_bytes!(O, self.slots);
            offset += vals.len();
            data.extend(vals);
            info.slot_map_offset = offset as u32;
            let vals = dump_bytes!(O, self.slot_map);
            offset += vals.len();
            data.extend(vals);
        }

        if let Some(block_header) = self.block_header {
            let off = (offset+ 15) & 0xFFFFFFF0;
            data.extend(vec![0u8; off-offset]);
            offset = off;

            info.block_offset = offset as u32;
            let vals = dump_bytes!(O, block_header);
            offset += vals.len();
            data.extend(vals);
            // should be recalculated for each block
            let vals = dump_bytes!(O, self.block_offsets);
            offset += vals.len();
            data.extend(vals);

            for (i, block) in self.blocks.iter().enumerate() {
                let off = (self.block_offsets[i] + info.block_offset) as usize;
                data.extend(vec![0u8; off-offset]);
                offset = off;
                let vals = dump_bytes!(O, block);
                offset += vals.len();
                data.extend(vals);
            }
        }

        infos.model.push(info);
        (data, mesh_data)
    }

    pub fn dump_terrain<O: Version + 'static>(&self, mut offset: usize, indices_offset: u32, infos: &mut DumpInfos) -> (Vec<u8>, Vec<u8>) {
        let mut info = self.info.clone();
        let mut data = vec![];

        let mut mat_order = self.mat_order.clone();
        let mut mat_map = HashMap::with_capacity(self.mats.len());
        for (i, (mat, mat_extra)) in zip(&self.mats, &self.mat_extras).enumerate() {
            let mut mat = mat.clone();
            if let Some(mat_extra) = mat_extra {
                mat.base_mut().mat_extra_offset = infos.header.mat_extra_offset + (O::size::<MatExtra>() * infos.mat_extra.len()) as u32;
                infos.mat_extra.push(mat_extra.clone())
            }
            match mat {
                Mat::Mat1(mat) => {
                    mat_map.insert(i as u32, infos.header.mat1_offset + (O::size::<Mat1>() * infos.mat1.len()) as u32);
                    infos.mat1.push(mat);
                },
                Mat::Mat2(mat) => {
                    mat_map.insert(i as u32, infos.header.mat2_offset + (O::size::<Mat2>() * infos.mat2.len()) as u32);
                    infos.mat2.push(mat);
                },
                Mat::Mat3(mat) => {
                    mat_map.insert(i as u32, infos.header.mat3_offset + (O::size::<Mat3>() * infos.mat3.len()) as u32);
                    infos.mat3.push(mat);
                },
                Mat::Mat4(mat) => {
                    mat_map.insert(i as u32, infos.header.mat4_offset + (O::size::<Mat4>() * infos.mat4.len()) as u32);
                    infos.mat4.push(mat);
                },
            }
        }
        mat_order.iter_mut().for_each(|x| *x = *mat_map.get(x).unwrap());

        let (mesh_data, vbuff_order, ibuff_order) = self.dump_mesh_data::<O>(infos);
        info.buffer_info_offset = infos.header.buffer_info_offset + (O::size::<BufferInfo>() * (infos.buffer.len() - self.buffer_infos.len())) as u32;

        info.hk_constraint_data_num = self.hk_constraint_datas.len() as u32;
        info.hk_constraint_data_offset = if self.hk_constraint_datas.len() != 0 {
            infos.header.hk_constraint_data_offset + (O::size::<HkConstraintData>() * infos.hk_constraint_data.len()) as u32
        } else { 0 };
        infos.hk_constraint_data.extend(self.hk_constraint_datas.clone());

        info.bones_offset = 0;
        info.bones_num = self.bone_bounding_boxes.len() as u32;

        info.bone_bounding_boxes_offset = infos.header.model_info_offset + (infos.model.len() * O::size::<ModelInfo>()) as u32 + 16;

        info.skin_binds_offset = indices_offset as u32;
        info.skin_binds_num = 0;
        info.skin_order_offset = 0;
        info.bone_parents_offset = indices_offset as u32;

        if let Some(hk_constraint) = &self.hk_constraint {
            info.hk_constraint_offset = infos.header.hk_constraint_info_offset + (O::size::<HkConstraintInfo>() * infos.hk_constraint.len()) as u32;
            let vals = dump_bytes!(O, hk_constraint, offset, info.bones_offset, info.bones_num, infos);
            offset += vals.len();
            data.extend(vals);    
        } else {
            info.hk_constraint_offset = 0;
        }

        let mut shape_offsets = Vec::with_capacity(self.shapes.len());
        for shape in &self.shapes {
            if let Some(extra) = &shape.extra {
                shape_offsets.push(Some(offset as u32));
                let vals = dump_bytes!(O, extra);
                offset += vals.len();
                data.extend(vals);
            } else {
                shape_offsets.push(None);
            }
        }

        let off = (offset+ 15) & 0xFFFFFFF0;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.mesh_bounding_boxes_offset = offset as u32;
        let vals = dump_bytes!(O, self.mesh_bounding_boxes);
        offset += vals.len();
        data.extend(vals);    

        info.bone_transforms_offset = offset as u32;
        let vals = dump_bytes!(O, self.bone_transforms);
        offset += vals.len();
        data.extend(vals);

        info.mat_offset = offset as u32;
        info.mat_num = mat_order.len() as u32;
        infos.block2_offsets.extend((0..mat_order.len() as u32).map(|x| offset as u32 + x * 4));
        let vals = dump_bytes!(O, mat_order);
        offset += vals.len();
        data.extend(vals);
        
        info.mesh_order_offset = offset as u32;
        //info.lod3.breakable_end = self.mesh_order.len() as u32;
        let vals = dump_bytes!(O, self.mesh_order);
        offset += vals.len();
        data.extend(vals);

        info.vbuff_offset = offset as u32;
        info.vbuff_num = vbuff_order.len() as u32;
        infos.block2_offsets.extend((0..vbuff_order.len() as u32).map(|x|  offset as u32 + x * 4));
        let vals = dump_bytes!(O, vbuff_order);
        offset += vals.len();
        data.extend(vals);

        let off_dest = offset + 320;
        info.ibuff_offset = offset as u32;
        info.ibuff_num = ibuff_order.len() as u32;
        infos.block2_offsets.extend((0..ibuff_order.len() as u32).map(|x|  offset as u32 + x * 4));
        let vals = dump_bytes!(O, ibuff_order);
        offset += vals.len();
        data.extend(vals);

        if self.val_k_header.len() != 0 {
            let off = (offset+ 15) & 0xFFFFFFF0;
            data.extend(vec![0u8; off-offset]);
            offset = off;

            info.vals_k_offset = offset as u32;
            let vals = dump_bytes!(O, self.val_k_header);
            offset += vals.len();
            data.extend(vals);
            let vals = dump_bytes!(O, self.vals_k);
            offset += vals.len();
            data.extend(vals);
        }

        if self.slots.len() != 0 {
            info.slots_offset = offset as u32;
            let vals = dump_bytes!(O, self.slots);
            offset += vals.len();
            data.extend(vals);
            info.slot_map_offset = offset as u32;
            let vals = dump_bytes!(O, self.slot_map);
            offset += vals.len();
            data.extend(vals);
        }

        if let Some(block_header) = self.block_header {
            let off = (offset+ 15) & 0xFFFFFFF0;
            data.extend(vec![0u8; off-offset]);
            offset = off;

            info.block_offset = offset as u32;
            let vals = dump_bytes!(O, block_header);
            offset += vals.len();
            data.extend(vals);
            let vals = dump_bytes!(O, self.block_offsets);
            offset += vals.len();
            data.extend(vals);

            for (i, block) in self.blocks.iter().enumerate() {
                let off = (self.block_offsets[i] + info.block_offset) as usize;
                data.extend(vec![0u8; off-offset]);
                offset = off;
                let vals = dump_bytes!(O, block);
                offset += vals.len();
                data.extend(vals);
            }
        }

        data.extend(vec![0u8; off_dest - offset]);
        offset = off_dest;

        info.shape_num = self.shapes.len() as u32;
        info.shape_offset = if self.shapes.len() != 0 {
            infos.header.shape_info_offset + (O::size::<ShapeInfo>() * infos.shape.len()) as u32
        } else { 0 };
        for (shape, off) in zip(&self.shapes, shape_offsets) {
            let vals = dump_bytes!(O, shape, offset, off, infos);
            offset += vals.len();
            data.extend(vals);    
        }


        infos.model.push(info);
        (data, mesh_data)
    }

    pub fn infos_count(&self) -> (u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32) {
        let hk_shapes = self.shapes.iter().map(|shape| shape.hk_shapes.len()).sum::<usize>();
        let (mut mat1_num, mut mat2_num, mut mat3_num, mut mat4_num) = (0u32, 0u32, 0u32, 0u32);
        for mat in &self.mats {
            match mat {
                Mat::Mat1(_) => { mat1_num += 1; },
                Mat::Mat2(_) => { mat2_num += 1; },
                Mat::Mat3(_) => { mat3_num += 1; },
                Mat::Mat4(_) => { mat4_num += 1; },
            }
        }
        return (
            self.shapes.len() as u32,
            hk_shapes as u32,
            if self.hk_constraint.is_some() { 1u32 } else { 0 },
            self.hk_constraint_datas.len() as u32,
            mat1_num, mat2_num, mat3_num, mat4_num,
            self.mat_extras.iter().map(|x| if x.is_some() { 1 } else { 0 }).sum::<u32>(),
            self.buffer_infos.len() as u32,
            self.vertex_data.len() as u32,
            self.index_data.len() as u32,
        )
    }

    pub fn to_gltf(&self) -> Result<gltf::Glb<'_>> {
        let mut root = gltf::json::root::Root::default();
        let mut bin = Vec::new();

        // add index data
        let ibuffs: Vec<_> = self.index_data.iter().map(|ibuff| match ibuff {
            IndexBuffer::U16 { vals } => GltfAsset { data: dump_bytes!(PC, vals), count: vals.len(), ty: DataType::U16, dim: Dimensions::Scalar,  target: Some(Target::ElementArrayBuffer), ..Default::default() },
            IndexBuffer::U32 { vals } => GltfAsset { data: dump_bytes!(PC, vals), count: vals.len(), ty: DataType::U32, dim: Dimensions::Scalar,  target: Some(Target::ElementArrayBuffer), ..Default::default() },
        }.to_gltf(&mut root, &mut bin)).collect();

        let vbuffs: Vec<(VBuffInfo, HashMap<VertexUsage, Index<Accessor>>)> = self.vertex_data.iter().map(|vbuff| (
            vbuff.info.clone(),
            vbuff.data.iter().map(|val| (val.usage.clone(), GltfAsset { 
                data: val.gltf_data(),
                count: val.val.len(),
                stride: val.stride(),
                target: Some(Target::ArrayBuffer),
                ty: val.component_type(),
                dim: val.dimensions(),
                min: val.min(),
                max: val.max(),
                normalized: val.normalized(),
                extras: Some(json!(val.usage.clone())),
            }.to_gltf(&mut root, &mut bin))).collect()
        )).collect();

        // add skeleton data if relevant
        let mut children = self.bones.iter().map(|_| Vec::new()).collect::<Vec<_>>();
        for (i, &parent) in self.bone_parents.iter().enumerate() {
            if parent == -1 { continue }
            children[parent as usize].push(Index::<Node>::new((root.nodes.len() + i) as u32));
        }

        let bones: Vec<_> = self.bones.iter().zip(self.bone_transforms.iter().zip(children)).map(|(bone, (mat, children))| {
            let children = if children.is_empty() {
                None 
            } else {
                Some(children)
            };
            root.push(Node {
                name: Some(bone.to_string()),
                children,
                matrix: Some(mat.into()),
                ..Default::default()
            })
        }).collect();

        // add skin if relevant
        let skin = if self.skin_order.len() != 0 {
            let joints: Vec<_> = self.skin_order.iter().map(|&x| bones[x as usize]).collect();
            let accessor = GltfAsset { 
                data: dump_bytes!(PC, self.skin_binds), 
                count: joints.len(), 
                ty: DataType::F32,
                dim: Dimensions::Mat4, 
                ..Default::default()
            }.to_gltf(&mut root, &mut bin);
            let skeleton = bones.first().copied();

            Some(root.push(gltf::json::Skin {
                name: None,
                extensions: None,
                extras: Default::default(),
                inverse_bind_matrices: Some(accessor),
                joints,
                skeleton,
            }))
        } else {
            None
        };

        // add meshesand nodes

        let mut i = 0;
        let mut uses = vec![0u32; self.buffer_infos.len()];
        for (lod, f) in [&self.info.lod0, &self.info.lod1, &self.info.lod2, &self.info.lod3].iter().zip(
            [LodMeshes::LOD0, LodMeshes::LOD1, LodMeshes::LOD2, LodMeshes::LOD3]
        ) {
            for i in i..lod.start { 
                uses[(self.mesh_order[i as usize] & 0x3FFFFFFF) as usize] |= f | LodMeshes::UNKNOWN; 
            }
            for i in lod.start..lod.static_end {
                uses[(self.mesh_order[i as usize] & 0x3FFFFFFF) as usize] |= f | LodMeshes::STATIC;
            }
            for i in lod.static_end..lod.skinned_end {
                uses[(self.mesh_order[i as usize] & 0x3FFFFFFF) as usize] |= f | LodMeshes::SKINNED;
            }
            for i in lod.skinned_end..lod.physics_end {
                uses[(self.mesh_order[i as usize] & 0x3FFFFFFF) as usize] |= f | LodMeshes::PHYSICS;
            }
            for i in lod.physics_end..lod.breakable_end {
                uses[(self.mesh_order[i as usize] & 0x3FFFFFFF) as usize] |= f | LodMeshes::BREAKABLE;
            }
            i = lod.breakable_end;
        }

        let mut nodes: Vec<_> = self.buffer_infos.iter().zip(uses).enumerate().map(|(i, (info, usage))| {
            let vbuff = &vbuffs[info.vbuff_info_offset as usize].1;
            let indices = ibuffs.get(info.ibuff_info_offset as usize).copied();
            //let i_size = root.accessors[indices.unwrap().value()].max.as_ref().unwrap().as_u64().unwrap();
            let mut attributes = std::collections::BTreeMap::new();
            let skinned = usage & LodMeshes::SKINNED != 0;
            //let skinned = vbuff.contains_key(&VertexUsage::BlendWeight) && vbuff.contains_key(&VertexUsage::BlendIndices);
            for (k, v) in vbuff.iter() {
                match k {
                    VertexUsage::Position => {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Positions), v.clone());
                    },
                    VertexUsage::Normal => {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Normals), v.clone());
                    },
                    VertexUsage::TextureCoord(i) => {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::TexCoords(*i as u32)), v.clone());
                    },
                    VertexUsage::BlendIndices => if skinned { 
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Joints(0)), v.clone());
                    } else {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Extras(k.to_string())), v.clone());
                    },
                    VertexUsage::BlendWeight => if skinned {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Weights(0)), v.clone());
                    } else {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Extras(k.to_string())), v.clone());
                    },
                    VertexUsage::Tangent => {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Tangents), v.clone());
                    },
                    _ => {
                        attributes.insert(Checked::Valid(gltf::mesh::Semantic::Extras(k.to_string())), v.clone());
                    }
                }
            }
            let skin = if skinned { skin } else { None };

            let mut name = String::new() + "(";
            if usage & LodMeshes::LOD0 != 0 { name += "Lod0, "; }
            if usage & LodMeshes::LOD1 != 0 { name += "Lod1, "; }
            if usage & LodMeshes::LOD2 != 0 { name += "Lod2, "; }
            if usage & LodMeshes::LOD3 != 0 { name += "Lod3, "; }
            name.truncate(name.len() - 2);
            name += format!(") Mesh {} [", i).as_str();
            if usage & LodMeshes::UNKNOWN != 0 { name += "Unknown, "; }
            if usage & LodMeshes::STATIC != 0 { name += "Static, "; }
            if usage & LodMeshes::SKINNED != 0 { name += "Skinned, "; }
            if usage & LodMeshes::PHYSICS != 0 { name += "Physics, "; }
            if usage & LodMeshes::BREAKABLE != 0 { name += "Breakable, "; }
            name.truncate(name.len() - 2);
            name += "]";

            let mesh = root.push(gltf::json::Mesh {
                name: None,
                extensions: None,
                extras: Default::default(),
                primitives: vec![gltf::json::mesh::Primitive {
                    attributes,
                    extensions: None,
                    extras: Default::default(),
                    indices, 
                    material: None,
                    mode: Checked::Valid(gltf::json::mesh::Mode::Triangles),
                    targets: None,
                }],
                weights: None,
            });

            let skin = if let Some(skin) = skin {
                let skin_start = info.skin_offset as usize;
                let skin_end = (info.skin_offset + info.skin_size) as usize;
                let skin = root.get(skin).unwrap();
                let joints = skin.joints[skin_start..skin_end].to_vec();
                let skeleton = skin.skeleton;
                let inverse_bind_matrices = if let Some(accessor) = skin.inverse_bind_matrices {
                    let mut accessor = root.get(accessor).unwrap().clone();
                    accessor.count = (info.skin_size as usize).into();
                    accessor.byte_offset = Some((info.skin_offset as usize * 64).into());
                    Some(root.push(accessor))
                } else {
                    None
                };

                Some(root.push(gltf::json::Skin {
                    name: None,
                    extensions: None,
                    extras: Default::default(),
                    inverse_bind_matrices,
                    joints,
                    skeleton,
                }))
            } else { None };

            root.push(Node {
                name: Some(name),
                skin,
                mesh: Some(mesh),
                ..Default::default()
            })
        }).collect();

        // construct scene
    
        if let Some(node) = bones.first() {
            nodes.push(node.clone());
        }
        
        root.push(gltf::json::Scene {
            name: None,
            extensions: None,
            extras: Default::default(),
            nodes
        });

        root.push(gltf::json::Buffer {
            name: None,
            byte_length: bin.len().into(),
            extensions: None,
            extras: Default::default(),
            uri: None,
        });
            
        root.extensions_used.push("KHR_mesh_quantization".into());
        root.extensions_required.push("KHR_mesh_quantization".into());
        root.extras = Some(serde_json::value::to_raw_value(&json!(ModelGltf {
            info: self.info.clone(),
            bone_bounding_boxes: self.bone_bounding_boxes.clone(),
            mat_order: self.mat_order.clone(),
            mesh_order: self.mesh_order.clone(),
            mesh_bounding_boxes: self.mesh_bounding_boxes.clone(),
            vals_j: self.vals_j.clone(),
            val_k_header: self.val_k_header.clone(),
            vals_k: self.vals_k.clone(),
            slots: self.slots.clone(),
            slot_map: self.slot_map.clone(),
            block_header: self.block_header.clone(),
            block_offsets: self.block_offsets.clone(),
            blocks: self.blocks.clone(),
            mats: self.mats.clone(),
            mat_extras: self.mat_extras.clone(),
            buffer_infos: self.buffer_infos.clone(),
            hk_constraint: self.hk_constraint.clone(),
            hk_constraint_datas: self.hk_constraint_datas.clone(),
            shapes: self.shapes.iter().map(|x| x.to_gltf(&mut root, &mut bin)).collect(),
            index_data: ibuffs,
            vertex_data: vbuffs,
            bones,
        }))?);

        // decode and reformat so that extras are properly formatted in the result
        let json = serde_json::to_vec_pretty(&serde_json::from_slice::<serde_json::Value>(root.to_vec()?.as_slice())?)?;
        let length = json.len();
        //json.extend(vec![' ' as u8; ((json.len() + 3) & 0xFFFFFFFC) - json.len()]);
    
        let header = gltf::binary::Header {
            magic: *b"GLTF",
            version: 2,
            length: (length + bin.len()) as u32,
        };

        Ok(gltf::Glb {
            header,
            bin: Some(bin.into()),
            json: json.into(),
        })
    }

    pub fn from_gltf(root: &Root, bin: &[u8]) -> Result<Self> {
        let model: ModelGltf = serde_json::from_str(root.extras.as_ref().unwrap().get())?;

        // get vertex / index data
        let index_data = model.index_data.into_iter().map(|i| {
            match root.get::<Accessor>(i.clone()).unwrap().component_type.as_ref().unwrap().0 {
                ComponentType::U32 => Ok(IndexBuffer::U32 { vals: GltfData::from_buffer(i, root, bin).u32().unwrap() }),
                ComponentType::U16 => Ok(IndexBuffer::U16 { vals: GltfData::from_buffer(i, root, bin).u16().unwrap() }),
                x => Err(anyhow!("Invlaid Index Buffer Format {:?}", x))
            }
        }).collect::<Result<Vec<_>>>()?;

        let vertex_data = model.vertex_data.iter().map(|(info, data)| {
            let fmt = VertexBuffer::from_vertex_format::<PC>(info.fmt1, info.fmt2);

            let mut vals = fmt.clone();
            for val in &mut vals {
                let i = data.get(&val.usage).unwrap();
                val.from_gltf(GltfData::from_buffer(*i, root, bin)).unwrap();
            }
            VertexBuffer { info: info.clone(), data: vals }
        }).collect();

        let mut bones = Vec::with_capacity(model.bones.len());
        let mut bone_parents = vec![-1; model.bones.len()];
        let mut bone_transforms = Vec::with_capacity(model.bones.len());
        let bone_map: HashMap<Index<Node>, usize> = model.bones.iter().cloned().enumerate().map(|(x,y)| (y,x)).collect();
        for (i, node) in model.bones.into_iter().map(|x| root.get(x).unwrap()).enumerate() {
            if let Some(children) = &node.children {
                for j in children {
                    bone_parents[*bone_map.get(j).unwrap()] = i as i32;
                }
            }
            bones.push(Crc::from_string(node.name.as_ref().unwrap().as_str()));
            bone_transforms.push(node.matrix.as_ref().unwrap().into());
        }
        
        let mut skin_order = Vec::new();
        let mut skin_binds = Vec::new();
        if let Some(skin) = root.skins.first() {
            skin_order.extend(skin.joints.iter().map(|x| *bone_map.get(x).unwrap() as u32));
            skin_binds.extend(GltfData::from_buffer(skin.inverse_bind_matrices.unwrap(), root, bin).f32().unwrap().as_slice().chunks_exact(16).map(|x| x.try_into().unwrap()));
        }

        Ok(Self {
            bones,
            bone_parents,
            bone_transforms,
            skin_order,
            skin_binds,
            vertex_data,
            index_data,
            info: model.info,
            bone_bounding_boxes: model.bone_bounding_boxes,
            mat_order: model.mat_order,
            mesh_order: model.mesh_order,
            mesh_bounding_boxes: model.mesh_bounding_boxes,
            vals_j: model.vals_j,
            val_k_header: model.val_k_header,
            vals_k: model.vals_k,
            slots: model.slots,
            slot_map: model.slot_map,
            block_header: model.block_header,
            block_offsets: model.block_offsets,
            blocks: model.blocks,
            mats: model.mats,
            mat_extras: model.mat_extras,
            buffer_infos: model.buffer_infos,
            hk_constraint: model.hk_constraint,
            hk_constraint_datas: model.hk_constraint_datas,
            shapes: model.shapes.into_iter().map(|x| x.parse(root, bin)).collect(),
        })
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ModelGltf {
    pub info: ModelInfo,
    pub bone_bounding_boxes: Vec<BoundingBox>,
    pub mat_order: Vec<u32>,
    pub mesh_order: Vec<u32>, // order of meshes (mapped to lod0, lod1, lod2, lod3)
    pub mesh_bounding_boxes: Vec<BoundingBox>, // 1 per contained mesh, stores result of some absolute position calculation?
    pub vals_j: Vec<u32>,
    pub val_k_header: Vec<u16>,
    pub vals_k: Vec<u32>,
    pub slots: Vec<Key2>, // attachment points
    pub slot_map: Vec<u32>, // attachment bone mapping
    pub block_header: Option<u32>, // has to do with havok cloth / hair stuff
    pub block_offsets: Vec<u32>,
    pub blocks: Vec<Block>,
    pub mats: Vec<Mat>,
    pub mat_extras: Vec<Option<MatExtra>>,
    pub buffer_infos: Vec<BufferInfo>,
    pub hk_constraint: Option<HkConstraint>,
    pub hk_constraint_datas: Vec<HkConstraintData>,
    pub shapes: Vec<ShapeGltf>,
    pub index_data: Vec<Index<Accessor>>,
    pub vertex_data: Vec<(VBuffInfo, HashMap<VertexUsage, Index<Accessor>>)>,
    pub bones: Vec<Index<Node>>,
}

