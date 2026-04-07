use std::fs;
use serde::{Serialize, Deserialize};
use std::path::Path;
use log::info;
use anyhow::Result;
use pyo3::prelude::*;

use lotrc_proc::{OrderedData, basicpymethods, PyMethods};
use super::types::{Crc, Version, PC, XBOX, from_bytes, dump_bytes, AsData, NoArgs};

#[basicpymethods]
#[pyclass(module="audio", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Header {
    pub const0x2: u32,
    pub n1: u32,
    pub n2: u32,
    pub n3: u32,
    pub n4: u32,
    pub n5: u32,
    pub n6: u32,
    pub n7: u32,
}

#[basicpymethods]
#[pyclass(module="audio", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Obj1 {
    pub key: Crc,
    pub val: u32,
}

#[basicpymethods]
#[pyclass(module="audio", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct Obj2 {
    pub unk_0: u32,
    pub unk_1: u32,
    pub n: u32,
}

#[pyclass(module="audio", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct AudioTable {
    #[serde(skip)]
    pub header: Header,
    pub obj1s: Vec<Obj1>,
    pub obj2s: Vec<(Obj2, Vec<Obj1>)>,
    pub obj3s: Vec<(Obj2, Vec<Obj1>)>,
    pub obj4s: Vec<Obj1>,
    pub obj5s: Vec<Obj1>,
    pub obj6s: Vec<Obj1>,
    pub obj7s: Vec<Obj1>,
    pub extra: Vec<Crc>,
}

#[basicpymethods]
#[pymethods]
impl AudioTable {
    #[staticmethod]
    fn load(path: String) -> Result<Self> {
        let path = std::path::PathBuf::from(path);
        if path.with_extension("json").is_file() {
            Self::from_file(path)
        } else {
            Self::parse(path)
        }
    }

    fn dump_files(&self, path: String) -> () {
        self.to_file(path)
    }

    fn dump_pc(&self, path: String) {
        self.dump::<PC, _>(path)
    }
}

impl AsData<'_, '_> for AudioTable {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;
    fn from_bytes<O: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let header: Header = from_bytes!(O, &data)?;
        let mut offset = header.size::<O>();
        let obj1s: Vec<Obj1> = from_bytes!(O, &data[offset..], header.n1 as usize)?;
        offset += obj1s.size::<O>();
        let mut obj2s = Vec::with_capacity(header.n2 as usize);
        for _ in 0..header.n2 {
            let obj: Obj2 = from_bytes!(O, &data[offset..])?;
            offset += obj.size::<O>();
            let objs: Vec<Obj1> = from_bytes!(O, &data[offset..], obj.n as usize)?;
            offset += objs.size::<O>();
            obj2s.push((obj, objs));
        }
        let mut obj3s = Vec::with_capacity(header.n3 as usize);
        for _ in 0..header.n3 {
            let obj: Obj2 = from_bytes!(O, &data[offset..])?;
            offset += obj.size::<O>();
            let objs: Vec<Obj1> = from_bytes!(O, &data[offset..], obj.n as usize)?;
            offset += objs.size::<O>();
            obj3s.push((obj, objs));
        }
        let obj4s: Vec<Obj1> = from_bytes!(O, &data[offset..], header.n4 as usize)?;
        offset += obj4s.size::<O>();
        let obj5s: Vec<Obj1> = from_bytes!(O, &data[offset..], header.n5 as usize)?;
        offset += obj5s.size::<O>();
        let obj6s: Vec<Obj1> = from_bytes!(O, &data[offset..], header.n6 as usize)?;
        offset += obj6s.size::<O>();
        let obj7s: Vec<Obj1> = from_bytes!(O, &data[offset..], header.n7 as usize)?;
        offset += obj7s.size::<O>();
        let n = (data.len() - offset) / 4;
        let extra: Vec<Crc> = from_bytes!(O, &data[offset..], n)?;

        Ok(Self {
            header,
            obj1s,
            obj2s,
            obj3s,
            obj4s,
            obj5s,
            obj6s,
            obj7s,
            extra
        })
    }

    fn dump_bytes<O: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(O, self.header).into_iter()
        .chain(dump_bytes!(O, self.obj1s))
        .chain(self.obj2s.iter().flat_map(|(obj, objs)| dump_bytes!(O, obj).into_iter().chain(dump_bytes!(O, objs))))
        .chain(self.obj3s.iter().flat_map(|(obj, objs)| dump_bytes!(O, obj).into_iter().chain(dump_bytes!(O, objs))))
        .chain(dump_bytes!(O, self.obj4s))
        .chain(dump_bytes!(O, self.obj5s))
        .chain(dump_bytes!(O, self.obj6s))
        .chain(dump_bytes!(O, self.obj7s))
        .chain(dump_bytes!(O, self.extra))
        .collect()
    }

    fn size<V: Version>(&self) -> usize {
        self.header.size::<V>()
+ self.obj1s.size::<V>()
            + self.obj2s.iter().map(|(obj, objs)| obj.size::<V>() + objs.size::<V>()).sum::<usize>()
            + self.obj3s.iter().map(|(obj, objs)| obj.size::<V>() + objs.size::<V>()).sum::<usize>()
            + self.obj4s.size::<V>()
            + self.obj5s.size::<V>()
            + self.obj6s.size::<V>()
            + self.obj7s.size::<V>()
            + self.extra.size::<V>()
    }
}

impl AudioTable {
    pub fn parse<P: AsRef<Path>>(path: P) -> Result<Self> {
        let path = path.as_ref();
        info!("Parsing audio table {}", path.file_stem().unwrap().to_str().unwrap());   
        let data = fs::read(path).unwrap();
        if data[0] == 2 {
            from_bytes!(PC, &data[..])
        } else if data[3] == 2 {
            from_bytes!(XBOX, &data[..])
        } else {
            Err(anyhow::anyhow!("Invalid audio table data"))
        }
    }

    pub fn dump<O: Version + 'static, P: AsRef<Path>>(&self, path: P) {
        path.as_ref().parent().map(fs::create_dir_all);
        fs::write(path.as_ref().with_extension("bin"), dump_bytes!(O, self)).unwrap();
    }

    pub fn to_file<P: AsRef<Path>>(&self, path: P) {
        if let Some(path) = path.as_ref().parent() {
            fs::create_dir_all(path).ok();
        }
        fs::write(path.as_ref().with_extension("audio.json"), serde_json::to_string_pretty(&self).unwrap()).unwrap();
    }

    pub fn from_file<P: AsRef<Path>>(path: P) -> Result<Self> {
        let mut val = serde_json::from_slice::<Self>(&fs::read(path.as_ref().with_extension("json"))?)?;
        val.header = Header {
            const0x2: 2,
            n1: val.obj1s.len() as u32,
            n2: val.obj2s.len() as u32,
            n3: val.obj3s.len() as u32,
            n4: val.obj4s.len() as u32,
            n5: val.obj5s.len() as u32,
            n6: val.obj6s.len() as u32,
            n7: val.obj7s.len() as u32
        };
        Ok(val)
    }
}
