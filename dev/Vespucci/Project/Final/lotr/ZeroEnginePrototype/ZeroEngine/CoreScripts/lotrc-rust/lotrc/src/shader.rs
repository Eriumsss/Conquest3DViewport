use std::{fs, path::Path};
use log::info;
use serde::{Serialize, Deserialize};
use indexmap::IndexMap;
use lotrc_proc::{OrderedData, basicpymethods, PyMethods};
use anyhow::{Result, Context};
use pyo3::prelude::*;
use super::{
    types::{Crc, Version, PC, XBOX, from_bytes, dump_bytes, AsData, NoArgs, Strings},
    read_write::{Reader, Writer, PathStuff},
};

#[basicpymethods]
#[pyclass(module="shader", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Header {
    pub constx1: u32,
    pub vertex_shader_num: u32,
    pub fragment_shader_num: u32,
    pub strings_offset: u32,
    pub strings_size: u32,
    pub strings_num: u32,
    pub shaders_offset: u32,
    pub shaders_size: u32,
}

#[basicpymethods]
#[pyclass(module="shader", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct ShaderHeader {
    pub key: Crc,
    pub offset: u32,
    pub flags: u64,
}

#[pyclass(module="shader", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct Shaders {
    #[serde(skip)]
    pub header: Header,
    pub vertex_headers: Vec<ShaderHeader>,
    pub fragment_headers: Vec<ShaderHeader>,
    pub vertex_shaders: IndexMap<Crc, Vec<u8>>,
    pub fragment_shaders: IndexMap<Crc, Vec<u8>>,
    pub strings: Strings 
}

#[basicpymethods]
#[pymethods]
impl Shaders {
    #[staticmethod]
    fn load(path: String) -> Result<Self> {
        let path = std::path::PathBuf::from(path);
        if path.with_extension("zip").is_file() {
            Self::from_file(Reader::new(path, true)?)
        } else if path.is_dir() {
            Self::from_file(Reader::new(path, false)?)
        } else {
            Self::parse(path)
        }
    }

    fn dump_files(&self, path: String, zip: bool) -> Result<()> {
        self.to_file(Writer::new(path, zip)?)
    }

    fn dump_pc(&self, path: String) -> Result<()> {
        self.dump::<PC, _>(path)
    }
}


impl AsData<'_, '_> for Shaders {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;
    fn from_bytes<O: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let header: Header = from_bytes!(O, &data)?;
        let strings: Strings = from_bytes!(O, &data[header.strings_offset as usize..], header.strings_num as usize)?;
        crate::types::update_strings(&strings.strings);
        let mut offset = header.size::<O>();
        let vertex_headers: Vec<ShaderHeader> = from_bytes!(O, &data[offset..], header.vertex_shader_num as usize)?;
        offset += vertex_headers.size::<O>();
        let fragment_headers: Vec<ShaderHeader> = from_bytes!(O, &data[offset..], header.fragment_shader_num as usize)?;
        let fragment_offsets: Vec<_> = fragment_headers.iter()
            .map(|x| (x.offset + header.shaders_offset) as usize)
            .chain(std::iter::once((header.shaders_size + header.shaders_offset) as usize))
            .collect();
        let vertex_offsets: Vec<_> = vertex_headers.iter()
            .map(|x| (x.offset + header.shaders_offset) as usize)
            .chain(std::iter::once(fragment_offsets[0]))
            .collect();
        let vertex_shaders: IndexMap<_, _> = vertex_headers.iter().enumerate()
            .map(|(i, info)| (info.key.clone(), data[vertex_offsets[i]..vertex_offsets[i+1]].to_vec()))
            .collect();
        let fragment_shaders: IndexMap<_, _> = fragment_headers.iter().enumerate()
            .map(|(i, info)| (info.key.clone(), data[fragment_offsets[i]..fragment_offsets[i+1]].to_vec()))
            .collect();

        Ok(Self {
            header,
            vertex_headers,
            fragment_headers,
            vertex_shaders,
            fragment_shaders,
            strings
        })
    }

    fn dump_bytes<O: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let mut shaders_size = 0;
        let vertex_headers: Vec<_> = self.vertex_headers.iter().zip(self.vertex_shaders.values()).map(|(info, shader)| {
            let mut info = info.clone();
            info.offset = shaders_size;
            shaders_size += shader.len() as u32;
            info
        }).collect();
        let fragment_headers: Vec<_> = self.fragment_headers.iter().zip(self.fragment_shaders.values()).map(|(info, shader)| {
            let mut info = info.clone();
            info.offset = shaders_size;
            shaders_size += shader.len() as u32;
            info
        }).collect();
        let shaders_offset = (self.header.size::<O>() + vertex_headers.size::<O>() + fragment_headers.size::<O>()) as u32;
        
        let header = Header {
            constx1: 1,
            vertex_shader_num: self.vertex_headers.len() as u32,
            fragment_shader_num: self.fragment_shaders.len() as u32,
            strings_offset: shaders_size + shaders_offset,
            strings_size: self.strings.size::<O>() as u32,
            strings_num: self.strings.strings.len() as u32,
            shaders_offset,
            shaders_size,
        };
        
        dump_bytes!(O, header).into_iter()
        .chain(dump_bytes!(O, vertex_headers))
        .chain(dump_bytes!(O, fragment_headers))
        .chain(vertex_headers.iter().flat_map(|x| self.vertex_shaders.get(&x.key).unwrap()).cloned())
        .chain(fragment_headers.iter().flat_map(|x| self.fragment_shaders.get(&x.key).unwrap()).cloned())
        .chain(dump_bytes!(O, self.strings))
        .collect()
    }

    fn size<V: Version>(&self) -> usize {
        self.header.size::<V>()
            + self.fragment_headers.size::<V>()
            + self.vertex_headers.size::<V>()
            + self.vertex_headers.iter().map(|x| self.vertex_shaders.get(&x.key).unwrap().len()).sum::<usize>()
            + self.fragment_headers.iter().map(|x| self.fragment_shaders.get(&x.key).unwrap().len()).sum::<usize>()
            + self.strings.size::<V>()
    }
}

impl Shaders {
    pub fn parse<P: AsRef<Path>>(path: P) -> Result<Self> {
        let path = path.as_ref();
        info!("Parsing shaders {}", path.file_stem().unwrap().to_str().unwrap());   
        let data = fs::read(path).unwrap();
        if data[0] == 1 {
            from_bytes!(PC, &data[..])
        } else if data[3] == 1 {
            from_bytes!(XBOX, &data[..])
        } else {
            Err(anyhow::anyhow!("Invalid audio table data"))
        }
    }

    pub fn dump<O: Version + 'static, P: AsRef<Path>>(&self, path: P) -> Result<()> {
        path.as_ref().parent().map(fs::create_dir_all);
        fs::write(path.as_ref().with_extension("bin"), dump_bytes!(O, self))?;
        Ok(())
    }

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        self.strings.to_file(writer.join("debug_strings"))?;
        writer.join("vertex_headers.json").write(&serde_json::to_vec_pretty(&self.vertex_headers)?)?;
        writer.join("fragment_headers.json").write(&serde_json::to_vec_pretty(&self.fragment_headers)?)?;
        for (name, shader) in self.vertex_shaders.iter() {
            writer.join("vertex_shaders").join(name.to_string()).with_extension("vso").write(shader)?;
        }
        for (name, shader) in self.fragment_shaders.iter() {
            writer.join("fragment_shaders").join(name.to_string()).with_extension("pso").write(shader)?;
        }
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        let header = Header::default();
        let strings = Strings::from_file(reader.join("debug_strings")).context("debug_strings")?;
        crate::types::update_strings(&strings.strings);
        let vertex_headers = serde_json::from_slice::<Vec<ShaderHeader>>(&reader.join("vertex_headers.json").read()?).context("vertex_headers.json")?;
        let fragment_headers = serde_json::from_slice::<Vec<ShaderHeader>>(&reader.join("fragment_headers.json").read()?).context("vertex_headers.json")?;

        let vertex_shaders: IndexMap<_, _> = vertex_headers.iter().map(|info| Ok((
                info.key.clone(),
                reader.join("vertex_shaders").join(info.key.to_string()).with_extension("vso").read()?
        ))).collect::<Result<_>>()?;

        let fragment_shaders: IndexMap<_, _> = fragment_headers.iter().map(|info| Ok((
                info.key.clone(),
                reader.join("fragment_shaders").join(info.key.to_string()).with_extension("pso").read()?
        ))).collect::<Result<_>>()?;

        Ok(Self {
            header,
            vertex_headers,
            fragment_headers,
            vertex_shaders,
            fragment_shaders,
            strings
        })
    }
}
