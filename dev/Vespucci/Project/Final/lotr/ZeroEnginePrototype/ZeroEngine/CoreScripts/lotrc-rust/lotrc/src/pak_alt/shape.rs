use serde::{Serialize, Deserialize};
use gltf::{
    json::{Root, Index, Accessor},
    accessor::{DataType, Dimensions},
};
use anyhow::Result;
use pyo3::prelude::*;

use lotrc_proc::{OrderedData, basicpymethods, PyMethods};
use crate::{
    types::{Crc, Vector4, Vector3 , Version, PC, from_bytes, dump_bytes, AsData, NoArgs},
    pak::*,
    pak_alt::{DumpInfos, GltfAsset, GltfData}
};

#[basicpymethods(no_bytes)]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct Shape {
    pub info: ShapeInfo,
    pub extra: Option<ShapeExtra>,
    pub hk_shapes: Vec<HkShape>,
}

impl <'a> AsData<'_, 'a> for Shape {
    type InArgs = usize;
    type OutArgs = (usize, Option<u32>, &'a mut DumpInfos);

    fn from_bytes<O: Version>(data: &[u8], offset: Self::InArgs) -> Result<Self> {
        let info: ShapeInfo = from_bytes!(O, &data[offset..])?;
        let extra = if info.kind == 0 {
            Some(from_bytes!(O, ShapeExtra, data, info.offset as usize)?)
        } else { None };
        let hk_shapes = (0..info.hk_shape_num as usize).map(|i| 
            from_bytes!(O, HkShape, data, info.hk_shape_offset as usize  + i * O::size::<HkShape0>())
        ).collect::<Result<Vec<_>>>()?;
        Ok(Self {
            info,
            extra,
            hk_shapes,
        })
    }

    fn dump_bytes<V: Version>(&self, (mut offset, extra_offset, infos): Self::OutArgs) -> Vec<u8> {
        let mut info = self.info.clone();
        if let Some(extra_offset) = extra_offset {
            info.offset = extra_offset;
            infos.block2_offsets.push(infos.header.shape_info_offset + (infos.shape.len() * V::size::<ShapeInfo>()) as u32);
        }
        info.hk_shape_offset = infos.header.hk_shape_info_offset + (infos.hk_shape.len() * V::size::<HkShape0>()) as u32;
        let mut data = vec![];
        for hk_shape in &self.hk_shapes {
            let vals = dump_bytes!(V, hk_shape, offset, infos);
            offset += vals.len();
            data.extend(vals);
        }

        infos.shape.push(info);
        data
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}

impl Shape {
    pub fn to_gltf(&self, root: &mut Root, bin: &mut Vec<u8>) -> ShapeGltf {
        ShapeGltf {
            info: self.info.clone(),
            extra: self.extra.as_ref().map(|x| x.to_gltf(root, bin)),
            hk_shapes: self.hk_shapes.iter().map(|x| x.to_gltf(root, bin)).collect()
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShapeGltf {
    pub info: ShapeInfo,
    pub extra: Option<ShapeExtraGltf>,
    pub hk_shapes: Vec<HkShapeGltf>,
}

impl ShapeGltf {
    pub fn parse(self, root: &Root, bin: &[u8]) -> Shape {
        Shape {
            info: self.info,
            extra: self.extra.map(|x| x.parse(root, bin)),
            hk_shapes: self.hk_shapes.into_iter().map(|x| x.parse(root, bin)).collect(),
        }
    }
}

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct ShapeExtraInfo {
    pub size: u32,
    pub scale: f32,
    pub a: f32,
    pub b: f32,
}

#[basicpymethods]
#[pyclass(module="pak_alt", set_all, get_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct ShapeExtra {
    pub info: ShapeExtraInfo,
    pub offs: Vec<u32>,
    pub data: Vec<u8>,
}

impl AsData<'_, '_> for ShapeExtra {
    type InArgs = usize;
    type OutArgs = NoArgs;

    fn from_bytes<O: Version>(data: &[u8], mut offset: Self::InArgs) -> Result<Self> {
        let info: ShapeExtraInfo = from_bytes!(O, &data[offset..])?;
        offset += O::size::<ShapeExtraInfo>();
        let offs: Vec<u32> = from_bytes!(O, &data[offset..], info.size as usize)?;
        offset += offs.size::<O>();
        let mut off = offset + *offs.last().unwrap() as usize;
        while (data[off] != 0) || (data[off+1] != 0) { off += 1; }
        Ok(Self { info, offs, data: data[offset..off].to_vec() })
    }

    fn dump_bytes<O: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(O, self.info).into_iter().chain(dump_bytes!(O, self.offs)).chain(self.data.clone()).collect()
    }

    fn size<V: Version>(&self) -> usize {
        self.info.size::<V>() + self.offs.size::<V>() + self.data.size::<V>()
    }
}

impl ShapeExtra {
    pub fn to_gltf(&self, root: &mut gltf::json::Root, bin: &mut Vec<u8>) -> ShapeExtraGltf {
        let info = self.info.clone();
        let offs = GltfAsset { 
            data: dump_bytes!(PC, self.offs),
            count: self.offs.len(),
            ty: DataType::U32,
            dim: Dimensions::Scalar,
            ..Default::default()
        }.to_gltf(root, bin);
        let data = GltfAsset { 
            data: self.data.clone(),
            count: self.data.len(),
            ty: DataType::U8,
            dim: Dimensions::Scalar,
            ..Default::default()
        }.to_gltf(root, bin);
        ShapeExtraGltf { info, offs, data }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShapeExtraGltf {
    #[serde(flatten)]
    pub info: ShapeExtraInfo,
    pub offs: Index<Accessor>,
    pub data: Index<Accessor>,
}

impl ShapeExtraGltf {
    pub fn parse(self, root: &Root, bin: &[u8]) -> ShapeExtra {
        ShapeExtra {
            info: self.info,
            offs: GltfData::from_buffer(self.offs, root, bin).u32().unwrap(),
            data: GltfData::from_buffer(self.data, root, bin).u8().unwrap(),
        }
    }
}

#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct HkShape0 {
    pub unk_0: Vector4,
    pub unk_4: Vector4,
    #[serde(skip, default="HkShape::t0")]
    pub kind: u32,
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
}

// BoxShape / ConvexTransformShape / ConvexTranslateShape
#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Box {
    pub translation: Vector4,
    pub rotation: Vector4,
    #[serde(skip, default="HkShape::t1")]
    pub kind: u32,
    pub key: Crc,
    pub half_extents: Vector3,
    pub unk_13: u32,
    pub unk_14: u32,
    pub unk_15: f32,
    pub unk_16: f32,
    pub unk_17: f32,
    pub unk_18: f32,
    pub unk_19: f32,
}

// SphereShape / ConvexTranslateShape
#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Sphere {
    pub translation: Vector4,
    pub rotation: Vector4,
    #[serde(skip, default="HkShape::t2")]
    pub kind: u32,
    pub key: Crc,
    pub radius: f32,
    pub unk_11: u32,
    pub unk_12: f32,
    pub unk_13: f32,
    pub unk_14: f32,
    pub unk_15: f32,
    pub unk_16: f32,
    pub unk_17: f32,
    pub unk_18: f32,
    pub unk_19: f32,
}

// CapsuleShape
#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Capsule {
    pub translation: Vector4,
    pub rotation: Vector4,
    #[serde(skip, default="HkShape::t3")]
    pub kind: u32,
    pub key: Crc,
    pub point1: Vector3,
    pub point2: Vector3,
    pub radius: f32,
    pub unk_17: f32,
    pub unk_18: f32,
    pub unk_19: f32,
}

// CylinderShape
#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Cylinder {
    pub translation: Vector4,
    pub rotation: Vector4,
    #[serde(skip, default="HkShape::t4")]
    pub kind: u32,
    pub key: Crc,
    pub point1: Vector3,
    pub point2: Vector3,
    pub radius: f32,
    pub unk_17: f32,
    pub unk_18: f32,
    pub unk_19: f32,
}

// ConvexVerticesShape
#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct ConvexVerticesInfo {
    pub translation: Vector4,
    pub rotation: Vector4,
    #[serde(skip, default="HkShape::t5")]
    pub kind: u32,
    pub key: Crc,
    #[serde(skip)]
    pub norm_num: u32,
    #[serde(skip)]
    pub norms_offset: u32,
    #[serde(skip)]
    pub vert_num: u32,
    #[serde(skip)]
    pub verts_offset: u32,
    pub unk_14: f32,
    pub unk_15: f32,
    pub unk_16: f32,
    pub unk_17: f32,
    pub unk_18: f32,
    pub unk_19: f32,
}

// ExtendedMeshShape / MoppBVTreeShape
#[basicpymethods]
#[pyclass(module="pak_alt", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct BVTreeMeshInfo {
    pub translation: Vector4,
    pub rotation: Vector4,
    #[serde(skip, default="HkShape::t6")]
    pub kind: u32,
    pub key: Crc,
    /// triangles min bound - 0.05
    pub offset: Vector3,
    /// 254*256*256 / (max triangle bound + 0.1)
    pub tree_scale: f32,
    #[serde(skip)]
    pub tree_size: u32,
    #[serde(skip)]
    pub tree_offset: u32, // u8
    #[serde(skip)]
    pub vert_num: u32, // vert_num
    #[serde(skip)]
    pub verts_offset: u32, // vec3 f32 verts offset
    #[serde(skip)]
    pub tri_num: u32,  // tri_num
    #[serde(skip)]
    pub inds_offset: u32, // vec3 u16 , inds offset
}

#[basicpymethods(no_bytes)]
#[pyclass(module="pak_alt", set_all, get_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct ConvexVertices {
    pub norms: Vec<Vector4>,
    pub verts: Vec<Vector3>,
    pub verts_extra: usize,
}

impl <'a, 'b> AsData<'a, 'b> for ConvexVertices {
    type InArgs = &'a ConvexVerticesInfo;
    type OutArgs = (usize, &'b mut ConvexVerticesInfo);

    fn from_bytes<O: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let norms = from_bytes!(O, &data[info.norms_offset as usize..], info.norm_num as usize)?;
        let mut vert_num = info.vert_num as usize; // sketchy stuff to account for data that was not otherwise captured, is it needed?
        while (info.verts_offset as usize + vert_num * 12) % 16 != 0 { vert_num += 1; }
        let verts = from_bytes!(O, &data[info.verts_offset as usize..], vert_num)?;
        let verts_extra = vert_num - info.vert_num as usize;
        Ok(Self { norms, verts, verts_extra })
    }

    fn dump_bytes<O: Version>(&self, (mut offset, info): Self::OutArgs) -> Vec<u8> {
        let mut data = vec![];

        let off: usize = (offset + 15) & 0xFFFFFFF0;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.norm_num = self.norms.len() as u32;
        info.norms_offset = offset as u32;
        let vals = dump_bytes!(O, self.norms);
        offset += vals.len();
        data.extend(vals);

        info.vert_num = (self.verts.len() - self.verts_extra) as u32;
        info.verts_offset = offset as u32;
        let vals = dump_bytes!(O, self.verts);
        data.extend(vals);

        data
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}

use serde_with::serde_as;
#[serde_as]
#[basicpymethods(no_bytes)]
#[pyclass(module="pak_alt", set_all, get_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct BVTreeMesh {
    #[serde_as(as = "serde_with::hex::Hex")]
    pub tree: Vec<u8>,
    pub verts: Vec<Vector3>,
    pub inds: Vec<u16>,
}

impl <'a, 'b> AsData<'a, 'b> for BVTreeMesh {
    type InArgs = &'a BVTreeMeshInfo;
    type OutArgs = (usize, &'b mut BVTreeMeshInfo);

    fn from_bytes<O: Version>(data: &[u8], info: Self::InArgs) -> Result<Self> {
        let tree = from_bytes!(O, &data[info.tree_offset as usize..], info.tree_size as usize)?;
        let verts = from_bytes!(O, &data[info.verts_offset as usize..], info.vert_num as usize)?;
        let inds = from_bytes!(O, &data[info.inds_offset as usize..], info.tri_num as usize * 3)?;
        Ok(Self { tree, verts, inds })
    }

    fn dump_bytes<O: Version>(&self, (mut offset, info): Self::OutArgs) -> Vec<u8> {
        let mut data = vec![];

        info.vert_num = self.verts.len() as u32;
        info.verts_offset = offset as u32;
        let vals = dump_bytes!(O, self.verts);
        offset += vals.len();
        data.extend(vals);

        info.tri_num = self.inds.len() as u32 / 3;
        info.inds_offset = offset as u32;
        let vals = dump_bytes!(O, self.inds);
        offset += vals.len();
        data.extend(vals);

        let off: usize = (offset + 3) & 0xFFFFFFFC;
        data.extend(vec![0u8; off-offset]);
        offset = off;

        info.tree_size = self.tree.len() as u32;
        info.tree_offset = offset as u32;
        let vals = dump_bytes!(O, self.tree);
        offset += vals.len();
        data.extend(vals);

        let off: usize = (offset + 3) & 0xFFFFFFFC;
        data.extend(vec![0u8; off-offset]);
        data
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}

#[basicpymethods]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
#[pyclass(module="pak_alt", get_all, set_all)]
pub enum HkShapeInfo {
    HkShape0(HkShape0),
    Box(Box),
    Sphere(Sphere),
    Capsule(Capsule),
    Cylinder(Cylinder),
    ConvexVertices(ConvexVerticesInfo),
    BVTreeMesh(BVTreeMeshInfo)
}

impl AsData<'_, '_> for HkShapeInfo {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;

    fn from_bytes<O: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let ty: u32 = from_bytes!(O, &data[32..])?;
        Ok(match ty {
            0 => Self::HkShape0(from_bytes!(O, &data[..])?),
            1 => Self::Box(from_bytes!(O, &data[..])?),
            2 => Self::Sphere(from_bytes!(O, &data[..])?),
            3 => Self::Capsule(from_bytes!(O, &data[..])?),
            4 => Self::Cylinder(from_bytes!(O, &data[..])?),
            5 => Self::ConvexVertices(from_bytes!(O, &data[..])?),
            6 => Self::BVTreeMesh(from_bytes!(O, &data[..])?),
            _ => return Err(anyhow::anyhow!("Unknown HkShape type {}", ty)),
        })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        match self {
            Self::HkShape0(val) => dump_bytes!(V, val),
            Self::Box(val) => dump_bytes!(V, val),
            Self::Sphere(val) => dump_bytes!(V, val),
            Self::Capsule(val) => dump_bytes!(V, val),
            Self::Cylinder(val) => dump_bytes!(V, val),
            Self::ConvexVertices(val) => dump_bytes!(V, val),
            Self::BVTreeMesh(val) => dump_bytes!(V, val),
        }
    }

    fn size<V: Version>(&self) -> usize {
        match self {
            Self::HkShape0(val) => val.size::<V>(),
            Self::Box(val) => val.size::<V>(),
            Self::Sphere(val) => val.size::<V>(),
            Self::Capsule(val) => val.size::<V>(),
            Self::Cylinder(val) => val.size::<V>(),
            Self::ConvexVertices(val) => val.size::<V>(),
            Self::BVTreeMesh(val) => val.size::<V>(),
        }
    }
}

#[basicpymethods(no_bytes)]
#[pyclass(module="pak_alt")]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum HkShape {
    HkShape0 { info: HkShape0 },
    Box { info: Box },
    Sphere { info: Sphere },
    Capsule { info: Capsule },
    Cylinder { info: Cylinder },
    ConvexVertices { info: ConvexVerticesInfo, shape: ConvexVertices },
    BVTreeMesh { info: BVTreeMeshInfo, shape: BVTreeMesh },
}

impl <'a> AsData<'_, 'a> for HkShape {
    type InArgs = usize;
    type OutArgs = (usize, &'a mut DumpInfos);
    
    fn from_bytes<V: Version>(data: &[u8], offset: Self::InArgs) -> Result<Self> {
        Ok(match from_bytes!(V, HkShapeInfo, &data[offset..])? {
            HkShapeInfo::HkShape0(info) => Self::HkShape0 { info },
            HkShapeInfo::Box(info) => Self::Box { info },
            HkShapeInfo::Sphere(info) => Self::Sphere { info },
            HkShapeInfo::Capsule(info) => Self::Capsule { info },
            HkShapeInfo::Cylinder(info) => Self::Cylinder { info },
            HkShapeInfo::ConvexVertices(info) => {
                let shape = from_bytes!(V, ConvexVertices, data, &info)?;
                Self::ConvexVertices { info, shape }
            },
            HkShapeInfo::BVTreeMesh(info) => {
                let shape = from_bytes!(V, BVTreeMesh, data, &info)?;
                Self::BVTreeMesh { info, shape }
            }
        })
    }

    fn dump_bytes<V: Version>(&self, (offset, infos): Self::OutArgs) -> Vec<u8> {
        let (info, data) = match self {
            Self::HkShape0 { info } => (HkShapeInfo::HkShape0(info.clone()), vec![]),
            Self::Box { info } => (HkShapeInfo::Box(info.clone()), vec![]),
            Self::Sphere { info } => (HkShapeInfo::Sphere(info.clone()), vec![]),
            Self::Capsule { info } => (HkShapeInfo::Capsule(info.clone()), vec![]),
            Self::Cylinder { info } => (HkShapeInfo::Cylinder(info.clone()), vec![]),
            Self::ConvexVertices { info, shape } => {
                let mut info = info.clone();
                let data = dump_bytes!(V, shape, offset, &mut info);
                (HkShapeInfo::ConvexVertices(info), data)
            },
            Self::BVTreeMesh { info, shape } => {
                let mut info = info.clone();
                let data = dump_bytes!(V, shape, offset, &mut info);
                (HkShapeInfo::BVTreeMesh(info), data)
            },
        };
        infos.hk_shape.push(info);
        data
    }
    
    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}

impl HkShape {
    fn t0() -> u32 { 0 }
    fn t1() -> u32 { 1 }
    fn t2() -> u32 { 2 }
    fn t3() -> u32 { 3 }
    fn t4() -> u32 { 4 }
    fn t5() -> u32 { 5 }
    fn t6() -> u32 { 6 }

    pub fn to_gltf(&self, root: &mut gltf::json::Root, bin: &mut Vec<u8>) -> HkShapeGltf {
        match self {
            Self::HkShape0 { info } => HkShapeGltf::HkShape0(info.clone()),
            Self::Box { info } => HkShapeGltf::Box(info.clone()),
            Self::Sphere { info } => HkShapeGltf::Sphere(info.clone()),
            Self::Capsule { info } => HkShapeGltf::Capsule(info.clone()),
            Self::Cylinder { info } => HkShapeGltf::Cylinder(info.clone()),
            Self::ConvexVertices { info, shape: ConvexVertices { norms, verts, verts_extra } } => {
                let info = info.clone();
                let verts_extra = *verts_extra;
                let norms = GltfAsset { 
                    data: dump_bytes!(PC, norms),
                    count: norms.len(),
                    ty: DataType::F32,
                    dim: Dimensions::Vec4,
                    ..Default::default()
                }.to_gltf(root, bin);
                let verts = GltfAsset { 
                    data: dump_bytes!(PC, verts),
                    count: verts.len(),
                    ty: DataType::F32,
                    dim: Dimensions::Vec3,
                    ..Default::default()
                }.to_gltf(root, bin);
                HkShapeGltf::ConvexVertices { info, norms, verts, verts_extra }
            },
            Self::BVTreeMesh { info, shape: BVTreeMesh { tree, verts, inds } } => {
                let info = info.clone();
                let tree = GltfAsset { 
                    data: tree.clone(),
                    count: tree.len(),
                    ty: DataType::U8,
                    dim: Dimensions::Scalar,
                    ..Default::default()
                }.to_gltf(root, bin);
                let verts = GltfAsset { 
                    data: dump_bytes!(PC, verts),
                    count: verts.len(),
                    ty: DataType::F32,
                    dim: Dimensions::Vec3,
                    ..Default::default()
                }.to_gltf(root, bin);
                let inds = GltfAsset { 
                    data: dump_bytes!(PC, inds),
                    count: inds.len() / 3,
                    ty: DataType::U16,
                    dim: Dimensions::Vec3,
                    ..Default::default()
                }.to_gltf(root, bin);

                HkShapeGltf::BVTreeMesh { info, tree, verts, inds }
            }
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub enum HkShapeGltf {
    HkShape0(HkShape0),
    Box(Box),
    Sphere(Sphere),
    Capsule(Capsule),
    Cylinder(Cylinder),
    ConvexVertices {
        #[serde(flatten)]
        info: ConvexVerticesInfo,
        norms: gltf::json::Index<gltf::json::Accessor>,
        verts: gltf::json::Index<gltf::json::Accessor>,
        verts_extra: usize,
    },
    BVTreeMesh {
        #[serde(flatten)]
        info: BVTreeMeshInfo,
        tree: gltf::json::Index<gltf::json::Accessor>,
        verts: gltf::json::Index<gltf::json::Accessor>,
        inds: gltf::json::Index<gltf::json::Accessor>,
    },
}

impl HkShapeGltf {
    pub fn parse(self, root: &Root, bin: &[u8]) -> HkShape {
        match self {
            Self::HkShape0(info) => HkShape::HkShape0 { info },
            Self::Box(info) => HkShape::Box { info },
            Self::Sphere(info) => HkShape::Sphere { info },
            Self::Capsule(info) => HkShape::Capsule { info },
            Self::Cylinder(info) => HkShape::Cylinder { info },
            Self::ConvexVertices { info, norms, verts, verts_extra } => {
                let norms = GltfData::from_buffer(norms, root, bin).f32().unwrap();
                let verts = GltfData::from_buffer(verts, root, bin).f32().unwrap();
                let norms = norms.as_slice().chunks_exact(4).map(|x| Vector4 { x: x[0], y: x[1], z: x[2], w: x[3] }).collect();
                let verts = verts.as_slice().chunks_exact(3).map(|x| Vector3 { x: x[0], y: x[1], z: x[2] }).collect();
                HkShape::ConvexVertices { info, shape: ConvexVertices { norms, verts, verts_extra } }
            },
            Self::BVTreeMesh { info, tree, verts, inds } => {
                let tree = GltfData::from_buffer(tree, root, bin).u8().unwrap();
                let verts = GltfData::from_buffer(verts, root, bin).f32().unwrap();
                let inds = GltfData::from_buffer(inds, root, bin).u16().unwrap();
                let verts = verts.as_slice().chunks_exact(3).map(|x| Vector3 { x: x[0], y: x[1], z: x[2] }).collect();
                HkShape::BVTreeMesh { info, shape: BVTreeMesh { tree, verts, inds } }
            }
        }
    }
}
