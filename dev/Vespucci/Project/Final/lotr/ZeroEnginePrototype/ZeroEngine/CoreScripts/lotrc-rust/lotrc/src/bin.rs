use std::any::TypeId;
use log::warn;
use serde::{Serialize, Deserialize};
use serde_with::serde_as;
use anyhow::Result;
use crate::types::Crc;
use pyo3::prelude::*;

use super::pak::TextureInfo;
use lotrc_proc::{OrderedData, basicpymethods, PyMethods};
use super::types::{AsData, Version, PC, from_bytes, dump_bytes, NoArgs};
use super::read_write::{Reader, Writer, PathStuff};

#[basicpymethods]
#[pyclass(module="bin", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Header {
    pub constx06: u32,
    pub version: u32,
    pub strings_offset: u32,
    pub strings_size: u32,
    pub strings_num: u32,
    pub asset_handle_num: u32,
    pub asset_handle_offset: u32,
    pub unk_7: u32,
    pub vdata_num: u32,
    pub vdata_num_: u32,
    pub texdata_num: u32,
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
}

#[basicpymethods]
#[pyclass(module="bin", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct AssetHandle {
    pub key: Crc,
    pub offset: u32,
    pub size: u32,
    pub size_comp: u32,
    pub kind: u32,
}

#[basicpymethods]
#[pyclass(module="bin", get_all, set_all)]
#[derive(Default, Clone, Debug, Serialize, Deserialize, PyMethods)]
pub struct Radiosity {
    pub data: Vec<u32>,
    pub usage: u32
}

impl AsData<'_, '_> for Radiosity {
    type InArgs = u32;
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], usage: Self::InArgs) -> Result<Self> {
        if data.len() % 4 != 0 {
            warn!("Radiosity length is incorrect?")
        }
        let data = from_bytes!(V, &data[..], data.len()/4)?;
        Ok(Self { data, usage })
    }
    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(V, self.data)
    }
    fn size<V: Version>(&self) -> usize {
        self.data.size::<V>()
    }
}

/*
type:
    0,7,8 -> Texture
    1,9 -> CubeTexture
    2,10 -> VolumeTexture
    4 -> RenderTarget
    5 -> DepthStencilSurface
    3 -> Nothing
    11 -> Surface

format
    0 -> 0x17 R5G6B5
    1 -> 0x1a A4R4G4B4
    2 -> 0x19 A1R5G5B5
    3, 0x10, 0x12, 0x13, 0x26 -> 0x15 A8R8G8B8
    4 -> 0x16 X8R8G8B8
    defualt -> 0 UNKNOWN
    6 -> 0x1c A8
    7,8 -> 0x31545844 DXT1
    9 -> 0x33545844 DXT3
    10, 0xb, 0xc, 0x11 -> 0x35545844 DXT5
    0x15 -> 0x23 A2R10G10B10
    0x17 -> 0x24 A16B16G16R16
    0x18 -> 0x71 A16B16G16R16F
    0x1b -> 0x4b D24S8
    0x1c -> 0x53 D24FS8
    0x1d -> 0x50 D16
    0x1e -> 0x5a574152 or 0x5a544e49 or 0x34324644, RAWZ or INTZ or DF24
    0x1f, 0x20 -> 0x32 L8
    0x21 -> 0x6f R16F
    0x22 -> 0x70 G16R16F
    0x23 -> 0x72 R32F
    0x24 -> 0x73 G32R32F
    0x25 -> 0x74 A32B32G32R32F 
    0x27 -> 0x20 A8B8G8R8
    0x28 -> 0x4c4c554e or 0x15, Null or A8R8G8B8
    13 -> bc4 alpha texture (xbox only, converts to A8 texture on PC)
"""
*/

#[serde_as]
#[pyclass(module="bin", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum Tex {
    Texture {
        #[serde_as(as = "Vec::<serde_with::hex::Hex>")]
        levels: Vec<Vec<u8>>,
        info: TextureInfo,
    },
    CubeTexture {
        #[serde_as(as = "Vec::<serde_with::hex::Hex>")]
        faces: Vec<Vec<u8>>,
        info: TextureInfo,
    },
    Unknown { 
        #[serde_as(as = "Vec::<serde_with::hex::Hex>")]
        vals: Vec<Vec<u8>>, 
        info: TextureInfo 
    },
}

#[basicpymethods(no_new)]
#[pymethods]
impl Tex {
    #[getter]
    fn get_data(&self) -> Vec<Vec<u8>> {
        match self {
            Self::Texture { levels, .. } => levels.clone(),
            Self::CubeTexture { faces, .. } => faces.clone(),
            Self::Unknown { vals, .. } => vals.clone(),
        }
    }

    fn get_img(&self, i: usize) -> Result<(Vec<u8>, usize, usize, u32)> {
        match self {
            Self::Texture { levels, info } => {
                let height = ((info.height >> i) as usize).max(1); 
                let width = ((info.width >> i) as usize).max(1); 
                get_img(
                    &levels.get(i).ok_or(anyhow::anyhow!("Invalid Index"))?[..], 
                    height, width, info.format
                )
            },
            Tex::CubeTexture { faces, info } => get_img(
                &faces.get(i).ok_or(anyhow::anyhow!("Invalid Index"))?[..], 
                info.height as usize, info.width as usize, info.format
            ),
            _ => Err(anyhow::anyhow!("Unsupported Texture Type"))
        }
    }
}

impl AsData<'_, '_> for Tex {
    type InArgs = (usize, TextureInfo);
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], (i, mut info): Self::InArgs) -> Result<Self> {
        Ok(match info.kind {
            0 | 7 | 8 => Self::texture_from_data::<V>(&data[..i], &data[i..], &mut info),
            1 | 9 => Self::cube_from_data::<V>(&data[..i], &data[i..], &mut info)?,
            _ => {
                warn!("Unsupported Texture Type {} for texture {:?}", info.kind, info.key);
                Self::Unknown { vals: vec![data[..i].to_vec(), data[i..].to_vec()], info: info.clone() }
            } 
        })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        if !V::pc() {
            warn!("Exporting Textures to Xbox format is not supported");
            return vec![];
        }
        match self {
            Self::Texture { levels, .. } => levels.iter().flatten().cloned().collect(),
            Self::CubeTexture { faces, .. } => faces.iter().flatten().cloned().collect(),
            Self::Unknown { vals, .. } => vals.iter().flatten().cloned().collect(),
        }
    }

    fn size<V: Version>(&self) -> usize {
        match self {
            Self::Texture { levels, .. } => levels.iter().map(|x| x.len()).sum::<usize>(),
            Self::CubeTexture { faces, .. } => faces.iter().map(|x| x.len()).sum::<usize>(),
            Self::Unknown { vals, .. } => vals.iter().map(|x| x.len()).sum::<usize>()
        }
    }

}

impl Tex {
    pub fn kind(&self) -> u32 {
        match self {
            Self::Texture { info, .. } => info.asset_type,
            Self::CubeTexture { info, .. } => info.asset_type,
            _ => 0,
        }
    }

    pub fn from_data<O: Version + 'static>(data0: &[u8], data1: &[u8], info: &mut TextureInfo) -> Result<Self> {
        Ok(match info.kind {
            0 | 7 | 8 => Self::texture_from_data::<O>(data0, data1, info),
            1 | 9 => Self::cube_from_data::<O>(data0, data1, info)?,
            _ => {
                warn!("Unsupported Texture Type {} for texture {:?}", info.kind, info.key);
                Self::Unknown { vals: vec![data0.to_vec(), data1.to_vec()], info: info.clone() }
            }
        })
    }

    pub fn dump<O: Version + 'static>(&self) -> (Vec<u8>, Vec<u8>) {
        if TypeId::of::<O>() != TypeId::of::<PC>() {
            warn!("Exporting Textures to Xbox format is not supported");
            return (vec![], vec![]);
        }
        match self {
            Self::Texture { levels, info } => match info.format {
                3 | 4 | 6 | 7 | 8 | 10 | 0xb | 0xc | 0x11 => {
                    if levels.len() > 1 {
                        (levels[0].clone(), levels[1..].iter().flatten().cloned().collect())
                    } else {
                        (vec![], levels[0].clone())
                    }
                },
                _ => {
                    (levels[0].clone(), levels[1].clone())
                }
            },
            Self::CubeTexture { faces, info } => match info.format {
                3 | 4 | 7 | 8 | 10 | 0xb | 0xc | 0x11 => (vec![], faces.iter().flatten().cloned().collect()),
                _ => (faces[0].clone(), faces[1].clone()),
            },
            Self::Unknown { vals, .. } => (vals[0].clone(), vals[1].clone()),
        }
    }

    pub fn info(&self) -> &TextureInfo {
        match self {
            Self::Texture { info, .. } => info,
            Self::CubeTexture { info, .. } => info,
            Self::Unknown { info, .. } => info
        }
    }

    pub fn size(&self) -> usize {
        match self {
            Self::Texture { levels, .. } => levels.iter().map(|x| x.len()).sum::<usize>(),
            Self::CubeTexture { faces, .. } => faces.iter().map(|x| x.len()).sum::<usize>(),
            Self::Unknown { vals, .. } => vals.iter().map(|x| x.len()).sum::<usize>()
        }
    }

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        match self {
            Self::Texture { levels, info } => {
                writer.with_extension("json").write(&serde_json::to_vec_pretty(info)?)?;
                let mut dds = ddsfile::Dds::new_d3d(ddsfile::NewD3dParams {
                    height: info.height as u32,
                    width: info.width as u32,
                    depth: None, 
                    format: get_format(info.format).expect(format!("Unknown format {}", info.format).as_str()),
                    mipmap_levels: Some(levels.len() as u32), 
                    caps2: None
                }).unwrap();
                dds.data.clear();
                dds.data.extend(levels.iter().flatten());
                let mut out = Vec::new();
                dds.write(&mut out).unwrap();
                writer.with_extension("dds").write(&out)?;
            },
            Self::CubeTexture { faces, info } => {
                writer.with_extension("json").write(&serde_json::to_vec_pretty(info)?)?;
                let mut dds = ddsfile::Dds::new_d3d(ddsfile::NewD3dParams { 
                    height: info.height as u32,
                    width: info.width as u32,
                    depth: None,
                    format: get_format(info.format).expect(format!("Unknown format {}", info.format).as_str()),
                    mipmap_levels: None, 
                    caps2: Some(ddsfile::Caps2::CUBEMAP | ddsfile::Caps2::CUBEMAP_ALLFACES),
                }).unwrap();
                dds.data.clear();
                dds.data.extend(faces.iter().flatten());
                let mut out = Vec::new();
                dds.write(&mut out).unwrap();
                writer.with_extension("dds").write(&out)?;
            },
            Self::Unknown { vals, info } => {
                let name = writer.name();
                writer.with_extension("json").write(&serde_json::to_vec_pretty(info)?)?;
                for (i, val) in vals.iter().enumerate() {
                    writer.with_file_name(format!("{}-{}.bin", name, i)).write(val)?;
                }
            }
        }
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        let mut info: TextureInfo = serde_json::from_slice(&&reader.with_extension("json").read()?)?;
        Ok(match info.kind {
            0 | 7 | 8 => {
                let dds = ddsfile::Dds::read(reader.with_extension("dds").read()?.as_slice())?;
                let size = dds.get_main_texture_size().unwrap() as usize;
                let data = &dds.data;
                if info.levels == 1 {
                    Self::texture_from_data::<PC>(&[], data, &mut info)
                } else {
                    Self::texture_from_data::<PC>(&data[..size], &data[size..], &mut info)
                }
            },
            1 | 9 => {
                let dds = ddsfile::Dds::read(reader.with_extension("dds").read()?.as_slice())?;
                Self::cube_from_data::<PC>(&[], &dds.data, &info)?
            },
            _ => {
                let name = reader.name();
                let data0 = reader.with_file_name(&format!("{}-0.bin", name)).read()?;
                let data1 = reader.with_file_name(&format!("{}-1.bin", name)).read()?;
                Self::Unknown { vals: vec![data0, data1], info }
            }
        })
    }

    fn texture_from_data<O: Version>(data0: &[u8], data1: &[u8], info: &mut TextureInfo) -> Self {
        let sizes = (0..info.levels).map(|x| 2u32.pow(x as u32)).map(|x| (info.width as u32/x, info.height as u32/x)).collect::<Vec<_>>();
        let (s, d) = match get_stride_width(info.format) {
            Some((s,d)) => (s,d),
            None => {
                warn!("Unhandled Texture Format {}", info.format);
                return Self::Texture {
                    levels: vec![data0.to_vec(), data1.to_vec()],
                    info: info.clone()
                }
            }
        };

        let block_sizes = sizes.iter().map(|(x,y)| ((x/s).max(1), (y/s).max(1))).collect::<Vec<_>>();
        let data = data0.iter().chain(data1.iter()).cloned().collect::<Vec<_>>();
        let levels = if O::pc() || O::ps3() {
            let data_sizes = block_sizes.iter().map(|(x,y)| (x * y * d) as usize).collect::<Vec<_>>();
            let mut levels = Vec::with_capacity(data_sizes.len());
            let mut offset = 0;
            for size in data_sizes {
                levels.push(data[offset..offset+size].to_vec());
                offset += size;
            } 
            levels
        } else {
            if info.levels == 1 {
                vec![conv_img(&data[..], sizes[0].1 as usize, sizes[0].0 as usize, info.format).0]
            } else {
                let data_sizes = block_sizes.iter().map(|(x,y)| (x.max(&32) * y.max(&32) * d) as usize).collect::<Vec<_>>();
                let wide_img = info.width > info.height;
                let mut levels = Vec::with_capacity(data_sizes.len());
                let mut packed_data = vec![];
                let mut offset = 0;
                let mut d = 0;
                let (mut pw, mut _ph) = (0,0);
                for i in 0..info.levels as usize {
                    let (m, m_) = (sizes[i].0.min(sizes[i].1) as usize, sizes[i].0.max(sizes[i].1) as usize);
                    if m > 16 {
                        levels.push(conv_img(&data[offset..offset+data_sizes[i]], sizes[i].1 as usize, sizes[i].0 as usize, info.format).0);
                        offset += data_sizes[i];
                    } else {
                        if m == 16 {
                            (packed_data, d, pw, _ph) = if wide_img {
                                conv_img(&data[offset..], sizes[i].1 as usize*2, sizes[i].0 as usize, info.format)
                            } else {
                                conv_img(&data[offset..], sizes[i].1 as usize, sizes[i].0 as usize*2, info.format)
                            };
                        }
                        if m >= 4 {
                            let off = m >> 2;
                            levels.push(if wide_img  {
                                packed_data.chunks(pw * d).skip(off).take(block_sizes[i].1 as usize).flat_map(|x| &x[..block_sizes[i].0 as usize*d]).cloned().collect()
                            } else {
                                packed_data.chunks(pw * d).take(block_sizes[i].1 as usize).flat_map(|x| &x[off*d..(off + block_sizes[i].0 as usize)*d]).cloned().collect()
                            });
                        } else {
                            let off = m_;
                            levels.push(if wide_img  {
                                packed_data.chunks(pw * d).take(block_sizes[i].1 as usize).flat_map(|x| &x[off*d..(off + block_sizes[i].0 as usize)*d]).cloned().collect()
                            } else {
                                packed_data.chunks(pw * d).skip(off).take(block_sizes[i].1 as usize).flat_map(|x| &x[..block_sizes[i].0 as usize*d]).cloned().collect()
                            });
                        }
                    }
                }
                if info.format == 13 {
                    info.format = 6;
                    levels = levels.into_iter().enumerate().map(|(i, x)| decomp_bc4(&x[..], sizes[i].0.max(4) as usize, sizes[i].1.max(4) as usize)).collect();
                    levels[info.levels as usize-2] = bin_mip(&levels[info.levels as usize-3][..], sizes[info.levels as usize-3].0 as usize);
                    levels[info.levels as usize-1] = bin_mip(&levels[info.levels as usize-2][..], sizes[info.levels as usize-2].0 as usize);
                }
                levels
            }
        };

        Self::Texture { levels, info: info.clone() }
    }

    pub fn cube_from_data<O: Version>(data0: &[u8], data1: &[u8], info: &TextureInfo) -> Result<Self> {
        assert!(info.levels <= 1, "Cube Textures with > 1 level are unhanded");
        let (s, d) = match get_stride_width(info.format) {
            Some(val) => val,
            None => {
                warn!("Unhandled Cube Texture Format {}", info.format);
                return Ok(Self::CubeTexture {
                    faces: vec![data0.to_vec(), data1.to_vec()],
                    info: info.clone()
                })
            }
        };

        let size = (info.width, info.height);
        let block_size = (size.0 as u32/s, size.1 as u32/s);
        let mut faces = Vec::with_capacity(6);

        if O::pc() || O::ps3() {
            let data_size = (block_size.0 * block_size.1 * d) as usize;
            data1.len().ge(&(data_size*6)).then_some(()).ok_or(anyhow::anyhow!("{:?}, texture data is too small. Expected {} got {}", info.key, data_size*6, data1.len()))?;
            for i in 0..6 {
                faces.push(data1[data_size*i..data_size*i+data_size].to_vec());
            }
        } else {
            let data_size = (block_size.0.max(32) * block_size.1.max(32) * d) as usize;
            data1.len().ge(&(data_size*6)).then_some(()).ok_or(anyhow::anyhow!("{:?}, texture data is too small. Expected {} got {}", info.key, data_size*6, data1.len()))?;
            for i in 0..6 {
                faces.push(conv_img(&data1[data_size*i..data_size*i+data_size], size.1 as usize, size.0 as usize, info.format).0);
            }
        }

        Ok(Self::CubeTexture { faces, info: info.clone() })
    }
}

pub fn get_img(data: &[u8], height: usize, width: usize, format: u32) -> Result<(Vec<u8>, usize, usize, u32)> {
    Ok((match format {
        10 | 0xb | 0xc | 0x11 => bcndecode::decode(
            data,
            width, 
            height,
            bcndecode::BcnEncoding::Bc3,
            bcndecode::BcnDecoderFormat::RGBA,
        )?,
        7 | 8 => bcndecode::decode(
            data, 
            width, 
            height,
            bcndecode::BcnEncoding::Bc1,
            bcndecode::BcnDecoderFormat::RGBA,
        )?,
        _ => data.to_vec()
    }, height, width, format))
}

pub fn get_stride_width(format: u32) -> Option<(u32, u32)> {
    match format {
        10 | 0xb | 0xc | 0x11 => Some((4, 16)),
        7 | 8 | 13 => Some((4, 8)),
        3 | 4 => Some((1, 4)),
        6 => Some((1, 1)),
        _ => None,
    }
}

pub fn get_format(format: u32) -> Option<ddsfile::D3DFormat> {
    match format {
        10 | 0xb | 0xc | 0x11 => Some(ddsfile::D3DFormat::DXT5),
        7 | 8 | 13  => Some(ddsfile::D3DFormat::DXT1),
        4 => Some(ddsfile::D3DFormat::X8R8G8B8),
        3 => Some(ddsfile::D3DFormat::A8R8G8B8),
        6 => Some(ddsfile::D3DFormat::A8),
        _ => None,
    }
}

pub fn conv_img_a8(
    data: &[u8],
    h: usize,
    w: usize,
    y_off: usize,
    x_off: usize,
    ys: usize,
    xs: usize,
) -> Vec<u8> {
    let mut dst = vec![0u8; h*w];
    let w = w >> 5;
    for j in 0..((h*w) >> 4).max(1) {
        let off = (j & 3) + ((j >> 1) & !3);
        let x = (off % w) << 5;
        let y = ((off / w) << 5) + ((j << 2) & 16);
        for i in 0..512 {
            let k = i + (j << 9);
            if k >= dst.len() { break; }
            let x = x + ((((i >> 4) & 16) + (i >> 3)) & 24) + (i & 7);
            let y = y + ((i >> 5) & 8) + ((i >> 3) & 4) + ((i >> 2) & 2) + ((i >> 4) & 1);
            if x < x_off || x >= x_off + xs || y < y_off || y >= y_off + ys {
                continue;
            }
            let j = (y - y_off) * xs + (x - x_off);
            dst[j] = data[k]
        }
    }
    dst
}

pub fn conv_img_argb8(
    data: &[u8],
    h: usize,
    w: usize,
    y_off: usize,
    x_off: usize,
    ys: usize,
    xs: usize,
) -> Vec<u8> {
    let mut dst = vec![0u8; h*w*4];
    let w = w >> 5;
    for j in 0..((h*w) >> 5).max(1) {
        let x = (j % w) << 5;
        let y = (j / w) << 5;
        for i in 0..1024 {
            let k = (i + (j << 10)) << 2;
            if k >= dst.len() { break; }
            let x = x + ((((i >> 4) & 16) + (i >> 1)) & 24) + ((i >> 1) & 4) + (i & 3);
            let y = y + ((i >> 5) & 8) + (((i >> 5) & 16)) + ((i >> 5) & 6) + ((i >> 2) & 1);
            if x < x_off || x >= x_off + xs || y < y_off || y >= y_off + ys {
                continue;
            }
            let j = ((y - y_off) * xs + (x - x_off)) << 2;
            dst[j] = data[k+3];
            dst[j+1] = data[k+2];
            dst[j+2] = data[k+1];
            dst[j+3] = data[k];
        }
    }
    dst
}

pub fn conv_img_dxt1(
    data: &[u8],
    h: usize,
    w: usize,
    y_off: usize,
    x_off: usize,
    ys: usize,
    xs: usize,
) -> Vec<u8> {
    let mut dst = vec![0u8; xs*ys*8];
    let w = w >> 5;
    for j in 0..((h*w) >> 5).max(1) {
        let x = (j % w) << 5;
        let y = (j / w) << 5;
        for i in 0..1024 {
            let mut k = i + (j << 10) << 3;
            if k >= data.len() { break; }
            let x = x + ((((i >> 5) & 16) + i) & 24) + ((i >> 3) & 4) + ((i >> 1) & 2) + (i & 1);
            let y = y + ((i >> 6) & 8) + (((i >> 4) & 16)) + ((i >> 5) & 6) + ((i >> 1) & 1);
            if x < x_off || x >= x_off + xs || y < y_off || y >= y_off + ys {
                continue;
            }
            let mut j = ((y - y_off) * xs + (x - x_off)) << 3;
            for _ in 0..4 {
                dst[j] = data[k+1];
                dst[j+1] = data[k];
                j += 2;
                k += 2;
            }
        }
    }
    dst
}

pub fn conv_img_dxt5(
    data: &[u8],
    h: usize,
    w: usize,
    y_off: usize,
    x_off: usize,
    ys: usize,
    xs: usize,
) -> Vec<u8> {
    let mut dst = vec![0u8; xs*ys*16];
    let w = w >> 5;
    for j in 0..((h*w) >> 5).max(1) {
        let x = (j % w) << 5;
        let y = (j / w) << 5;
        for i in 0..1024 {
            let mut k = (i + (j << 10)) << 4;
            if k > data.len() { break; }
            let x = x + ((((i >> 5) & 16) + (i << 1)) & 24) + ((i >> 3) & 6) + ((i >> 1) & 1);
            let y = y + ((i >> 6) & 8) + (((i >> 3) & 16)) + ((i >> 6) & 4) + ((i >> 5) & 2) + (i & 1);
            if x < x_off || x >= x_off + xs || y < y_off || y >= y_off + ys {
                continue;
            }
            let mut j = ((y - y_off) * xs + (x - x_off)) << 4;
            for _ in 0..8 {
                dst[j] = data[k+1];
                dst[j+1] = data[k];
                j += 2;
                k += 2;
            }
        }
    }
    dst
}

pub fn conv_img(data: &[u8], height: usize, width: usize, f: u32) -> (Vec<u8>, usize, usize, usize) {
    match f {
        10 | 0xb | 0xc | 0x11 => {
            let h = height >> 2;
            let w = width >> 2;
            (conv_img_dxt5(data, h.max(32), w.max(32), 0, 0, h, w), 16, w, h)
        },
        7 | 8 | 13 => {
            let h = height >> 2;
            let w = width >> 2;
            (conv_img_dxt1(data, h.max(32), w.max(32), 0, 0, h, w), 8, w, h)
        },
        3 | 4 => (conv_img_argb8(data, height, width, 0, 0, height, width), 4, width, height),
        _ => (conv_img_a8(data, height, width, 0, 0, height, width), 1, width, height)
    }
}

fn bin_mip(arr: &[u8], w: usize) -> Vec<u8> {
    arr.chunks(w).step_by(2).flat_map(|x| x.iter().step_by(2)).cloned().collect()
}

fn decomp_bc4(arr: &[u8], w: usize, h: usize) -> Vec<u8> {
    bcndecode::decode(arr, w, h, bcndecode::BcnEncoding::Bc4, bcndecode::BcnDecoderFormat::LUM).unwrap()
}
