use std::any::TypeId;
use std::{collections::{HashMap, HashSet}, iter::zip, mem::size_of};
use flate2::Decompress;
use log::warn;
use serde_json::{Value, json, to_vec_pretty, from_slice, Map};
use zerocopy::{Immutable, KnownLayout, IntoBytes, Unaligned, ByteOrder, FromBytes, BE, F32, LE, U16, U32, U64, I32, I16};
use flate2::read::ZlibDecoder;
use flate2::write::ZlibEncoder;
use serde::{Serialize, Deserialize};
use std::sync::Mutex;
use std::io::prelude::*;
use anyhow::{anyhow, Context, Result};
use indicatif::ProgressBar;
use indexmap::IndexMap;
use pyo3::prelude::*;

use crate::{
    lua_stuff as lua,
    read_write::{Reader, Writer, PathStuff},
    pak::PFieldInfo,
};

use lotrc_proc::{OrderedData, basicpymethods, PyMethods};

pub trait PyMethods<'a> where Self: std::fmt::Debug + Sized + Deserialize<'a> {
    fn from_json(s: &'a str) -> Result<Self>;
    fn to_json(&self) -> Result<String>;
}

pub struct PC;
pub struct XBOX;
pub struct PS3;
pub trait Version {
    fn from_bytes<T: OrderedData>(data: &[u8]) -> Result<T>;
    fn to_bytes<T: OrderedData>(val: &T, data: &mut [u8]) -> Result<()>;
    fn dump_bytes<T: OrderedData>(val: &T) -> Vec<u8>;
    fn size<T: OrderedData>() -> usize;
    fn from_bytes_vec<T: OrderedData>(data: &[u8], num: usize) -> Result<Vec<T>>;
    fn to_bytes_vec<T: OrderedData>(val: &Vec<T>, data: &mut [u8]) -> Result<()>;
    fn dump_bytes_vec<T: OrderedData>(val: &Vec<T>) -> Vec<u8>;
    #[inline]
    fn size_vec<T: OrderedData>(val: &Vec<T>) -> usize {
        val.len() * Self::size::<T>()
    }
    #[inline]
    fn pc() -> bool { false }
    #[inline]
    fn ps3() -> bool { false }
    #[inline]
    fn xbox() -> bool { false }
}

impl Version for PC {
    #[inline]
    fn pc() -> bool { true }
    #[inline]
    fn from_bytes<T: OrderedData>(data: &[u8]) -> Result<T> {
        Ok(T::PC::read_from_prefix(data).map_err(|e| anyhow!("{}", e))?.0.into())
    }
    #[inline]
    fn to_bytes<T: OrderedData>(val: &T, data: &mut [u8]) -> Result<()> {
        T::PC::write_to_prefix(&val.clone().into(), data).map_err(|e| anyhow!("{:?}", e))?;
        Ok(())
    }
    #[inline]
    fn dump_bytes<T: OrderedData>(val: &T) -> Vec<u8> {
        T::PC::as_bytes(&val.clone().into()).to_vec()
    }
    #[inline]
    fn size<T: OrderedData>() -> usize {
        size_of::<T::PC>()
    }
    #[inline]
    fn from_bytes_vec<T: OrderedData>(data: &[u8], num: usize) -> Result<Vec<T>> {
       Ok(<[T::PC]>::ref_from_prefix_with_elems(data, num).map_err(|e| anyhow!("{}", e))?.0.iter().map(|x| x.clone().into()).collect())
    }
    #[inline]
    fn to_bytes_vec<T: OrderedData>(val: &Vec<T>, data: &mut [u8]) -> Result<()> {
        val.iter().cloned().map(|x| T::PC::from(x)).collect::<Vec<_>>().as_slice().write_to_prefix(data).map_err(|e| anyhow!("{:?}", e))?;
        Ok(())
    }
    #[inline]
    fn dump_bytes_vec<T: OrderedData>(val: &Vec<T>) -> Vec<u8> {
        val.iter().cloned().flat_map(|x| T::PC::from(x).as_bytes().iter().cloned().collect::<Vec<_>>()).collect()
    }
}

impl Version for XBOX {
    #[inline]
    fn xbox() -> bool { true }
    #[inline]
    fn from_bytes<T: OrderedData>(data: &[u8]) -> Result<T> {
        Ok(T::XBOX::read_from_prefix(data).map_err(|e| anyhow!("{}", e))?.0.into())
    }
    #[inline]
    fn to_bytes<T: OrderedData>(val: &T, data: &mut [u8]) -> Result<()> {
        T::XBOX::write_to_prefix(&val.clone().into(), data).map_err(|e| anyhow!("{:?}", e))?;
        Ok(())
    }
    #[inline]
    fn dump_bytes<T: OrderedData>(val: &T) -> Vec<u8> {
        T::XBOX::as_bytes(&val.clone().into()).to_vec()
    }
    #[inline]
    fn size<T: OrderedData>() -> usize {
        size_of::<T::XBOX>()
    }
    #[inline]
    fn from_bytes_vec<T: OrderedData>(data: &[u8], num: usize) -> Result<Vec<T>> {
       Ok(<[T::XBOX]>::ref_from_prefix_with_elems(data, num).map_err(|e| anyhow!("{}", e))?.0.iter().map(|x| x.clone().into()).collect())
    }
    #[inline]
    fn to_bytes_vec<T: OrderedData>(val: &Vec<T>, data: &mut [u8]) -> Result<()> {
        val.iter().cloned().map(|x| T::XBOX::from(x)).collect::<Vec<_>>().as_slice().write_to_prefix(data).map_err(|e| anyhow!("{:?}", e))?;
        Ok(())
    }
    #[inline]
    fn dump_bytes_vec<T: OrderedData>(val: &Vec<T>) -> Vec<u8> {
        val.iter().cloned().flat_map(|x| T::XBOX::from(x).as_bytes().iter().cloned().collect::<Vec<_>>()).collect()
    }
}

impl Version for PS3 {
    #[inline]
    fn ps3() -> bool { true }
    #[inline]
    fn from_bytes<T: OrderedData>(data: &[u8]) -> Result<T> {
        Ok(T::PS3::read_from_prefix(data).map_err(|e| anyhow!("{}", e))?.0.into())
    }
    #[inline]
    fn to_bytes<T: OrderedData>(val: &T, data: &mut [u8]) -> Result<()> {
        T::PS3::write_to_prefix(&val.clone().into(), data).map_err(|e| anyhow!("{:?}", e))?;
        Ok(())
    }
    #[inline]
    fn dump_bytes<T: OrderedData>(val: &T) -> Vec<u8> {
        T::PS3::as_bytes(&val.clone().into()).to_vec()
    }
    #[inline]
    fn size<T: OrderedData>() -> usize {
        size_of::<T::PS3>()
    }
    #[inline]
    fn from_bytes_vec<T: OrderedData>(data: &[u8], num: usize) -> Result<Vec<T>> {
       Ok(<[T::PS3]>::ref_from_prefix_with_elems(data, num).map_err(|e| anyhow!("{}", e))?.0.iter().map(|x| x.clone().into()).collect())
    }
    #[inline]
    fn to_bytes_vec<T: OrderedData>(val: &Vec<T>, data: &mut [u8]) -> Result<()> {
        val.iter().cloned().map(|x| T::PS3::from(x)).collect::<Vec<_>>().as_slice().write_to_prefix(data).map_err(|e| anyhow!("{:?}", e))?;
        Ok(())
    }
    #[inline]
    fn dump_bytes_vec<T: OrderedData>(val: &Vec<T>) -> Vec<u8> {
        val.iter().cloned().flat_map(|x| T::PS3::from(x).as_bytes().iter().cloned().collect::<Vec<_>>()).collect()
    }
}

pub trait OrderedData where Self: Sized + Clone + Default {
    type PC: Into<Self> + From<Self> + Immutable + KnownLayout + FromBytes + IntoBytes + Unaligned + Clone + std::fmt::Debug + Default;
    type XBOX: Into<Self> + From<Self> + Immutable + KnownLayout + FromBytes + IntoBytes + Unaligned + Clone + std::fmt::Debug + Default;
    type PS3: Into<Self> + From<Self> + Immutable + KnownLayout + FromBytes + IntoBytes + Unaligned + Clone + std::fmt::Debug + Default;
}

impl OrderedData for f32 { type PC = F32<LE>; type XBOX = F32<BE>; type PS3 = F32<BE>; }
impl OrderedData for u64 { type PC = U64<LE>; type XBOX = U64<BE>; type PS3 = U64<BE>; }
impl OrderedData for u32 { type PC = U32<LE>; type XBOX = U32<BE>; type PS3 = U32<BE>; }
impl OrderedData for i32 { type PC = I32<LE>; type XBOX = I32<BE>; type PS3 = I32<BE>; }
impl OrderedData for u16 { type PC = U16<LE>; type XBOX = U16<BE>; type PS3 = U16<BE>; }
impl OrderedData for i16 { type PC = I16<LE>; type XBOX = I16<BE>; type PS3 = I16<BE>; }
impl OrderedData for u8 { type PC = u8; type XBOX = u8; type PS3 = u8; }
impl OrderedData for i8 { type PC = i8; type XBOX = i8; type PS3 = i8; }

pub struct NoArgs;

impl <'py> IntoPyObject<'py> for NoArgs {
    type Target = pyo3::types::PyTuple;
    type Output = Bound<'py, Self::Target>;
    type Error = std::convert::Infallible;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        Ok(pyo3::types::PyTuple::empty(py))
    }
}

impl <'py> FromPyObject<'py> for NoArgs {
    fn extract_bound(_ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self)
    }
}

pub trait AsData<'a, 'b> where Self: Sized {
    type InArgs;
    type OutArgs;
    fn from_bytes<V: Version>(data: &[u8], args: Self::InArgs) -> Result<Self>;
    fn dump_bytes<V: Version>(&self, args: Self::OutArgs) -> Vec<u8>;
    fn size<V: Version>(&self) -> usize;
    #[inline]
    fn to_bytes<V: Version>(&self, data: &mut [u8], args: Self::OutArgs) -> Result<()> {
        let vals = self.dump_bytes::<V>(args);
        data[..vals.len()].copy_from_slice(&vals[..]);
        Ok(())
    }
}

#[macro_export]
macro_rules! wrap_args {
    () => { crate::types::NoArgs };
    ( $a:expr ) => { $a };
    ( $($a:expr),* ) => { ($($a),*) };
}
#[macro_export]
macro_rules! from_bytes {
    ($t:ty, $q:ty, $d:expr $( ,$x:expr )*) => { <$q as AsData<'_, '_>>::from_bytes::<$t>($d, crate::types::wrap_args!($($x),*)) };
    ($t:ty, $d:expr $( ,$x:expr )*) => { AsData::<'_, '_>::from_bytes::<$t>($d, crate::types::wrap_args!($($x),*)) };
}
#[macro_export]
macro_rules! to_bytes {
    ($t:ty, $v:expr, $d:expr $( ,$x:expr )*) => { $v.to_bytes::<$t>($d, crate::types::wrap_args!($($x),*)) }
}
#[macro_export]
macro_rules! dump_bytes {
    ($t:ty, $v:expr $( ,$x:expr )*) => { $v.dump_bytes::<$t>(crate::types::wrap_args!($($x),*)) }
}
pub use wrap_args;
pub use from_bytes;
pub use to_bytes;
pub use dump_bytes;

impl <T: OrderedData> AsData<'_, '_> for Vec<T> {
    type InArgs = usize;
    type OutArgs = NoArgs;
    #[inline]
    fn from_bytes<V: Version>(data: &[u8], num: usize) -> Result<Self> { V::from_bytes_vec(data, num) }
    #[inline]
    fn to_bytes<V: Version>(&self, data: &mut [u8], _args: NoArgs) -> Result<()> { V::to_bytes_vec(self, data) }
    #[inline]
    fn dump_bytes<V: Version>(&self, _args: NoArgs) -> Vec<u8> { V::dump_bytes_vec(self) }
    #[inline]
    fn size<V: Version>(&self) -> usize { V::size_vec(self) }
}

impl <T: OrderedData> AsData<'_, '_> for T {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;
    #[inline]
    fn from_bytes<V: Version>(data: &[u8], _args: NoArgs) -> Result<Self> { V::from_bytes(data) }
    #[inline]
    fn to_bytes<V: Version>(&self, data: &mut [u8], _args: NoArgs) -> Result<()> { V::to_bytes(self, data) }
    #[inline]
    fn dump_bytes<V: Version>(&self, _args: NoArgs) -> Vec<u8> { V::dump_bytes(self) }
    #[inline]
    fn size<V: Version>(&self) -> usize { V::size::<Self>() }
}

const HASHING_ARRAY: [u32; 256] = [
    0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9, 0x130476dc, 0x17c56b6b, 0x1a864db2, 0x1e475005, 
    0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61, 0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd, 
    0x4c11db70, 0x48d0c6c7, 0x4593e01e, 0x4152fda9, 0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75, 
    0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011, 0x791d4014, 0x7ddc5da3, 0x709f7b7a, 0x745e66cd, 
    0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039, 0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5, 
    0xbe2b5b58, 0xbaea46ef, 0xb7a96036, 0xb3687d81, 0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d, 
    0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49, 0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95, 
    0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1, 0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d, 
    0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae, 0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072, 
    0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16, 0x018aeb13, 0x054bf6a4, 0x0808d07d, 0x0cc9cdca, 
    0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde, 0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02, 
    0x5e9f46bf, 0x5a5e5b08, 0x571d7dd1, 0x53dc6066, 0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba, 
    0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e, 0xbfa1b04b, 0xbb60adfc, 0xb6238b25, 0xb2e29692, 
    0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6, 0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a, 
    0xe0b41de7, 0xe4750050, 0xe9362689, 0xedf73b3e, 0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2, 
    0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686, 0xd5b88683, 0xd1799b34, 0xdc3abded, 0xd8fba05a, 
    0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637, 0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb, 
    0x4f040d56, 0x4bc510e1, 0x46863638, 0x42472b8f, 0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53, 
    0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47, 0x36194d42, 0x32d850f5, 0x3f9b762c, 0x3b5a6b9b, 
    0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff, 0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623, 
    0xf12f560e, 0xf5ee4bb9, 0xf8ad6d60, 0xfc6c70d7, 0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b, 
    0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f, 0xc423cd6a, 0xc0e2d0dd, 0xcda1f604, 0xc960ebb3, 
    0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7, 0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b, 
    0x9b3660c6, 0x9ff77d71, 0x92b45ba8, 0x9675461f, 0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3, 
    0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640, 0x4e8ee645, 0x4a4ffbf2, 0x470cdd2b, 0x43cdc09c, 
    0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8, 0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24, 
    0x119b4be9, 0x155a565e, 0x18197087, 0x1cd86d30, 0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec, 
    0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088, 0x2497d08d, 0x2056cd3a, 0x2d15ebe3, 0x29d4f654, 
    0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0, 0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c, 
    0xe3a1cbc1, 0xe760d676, 0xea23f0af, 0xeee2ed18, 0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4, 
    0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0, 0x9abc8bd5, 0x9e7d9662, 0x933eb0bb, 0x97ffad0c, 
    0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668, 0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4 
];

const INDEX_ARRAY: [usize; 256] = [
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 
    0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff 
];

#[pyfunction]
#[pyo3(signature = (string, mask=None))]
pub const fn hash_string(string: &[u8], mask: Option<u32>) -> u32 {
    let mut h = !match mask {
        Some(val) => val,
        None => 0,
    };
    let mut i: usize = 0;
    loop {
        if i >= string.len() { break; }
        h = (h << 8) ^ HASHING_ARRAY[INDEX_ARRAY[string[i] as usize] ^ (h >> 24) as usize];
        i += 1;
    }
    !h
}

lazy_static::lazy_static! {
    pub static ref STRING_LOOKUP: Mutex<HashMap<u32, String>> = {
        const CONQUEST_STRINGS: &str = include_str!("../res/conquest_strings.txt");
        Mutex::new(CONQUEST_STRINGS.replace("\r\n", "\n").split('\n').map(|x| (hash_string(x.as_bytes(), None), String::from(x))).collect())
    };

    pub static ref ANIMATION_EVENTS: HashMap<Crc, Vec<u32>> = {
        serde_json::from_str::<HashMap<String, Vec<String>>>(include_str!("../res/animation_events.json")).unwrap().into_iter().map(|(k, val)| (
            Crc::Str(k), val.into_iter().map(|x| hash_string(x.as_bytes(), None)).collect()
        )).collect()
    };

    pub static ref DECOMP_LUA: Mutex<bool> = Mutex::new(false);

    pub static ref RECOMP_LUA: Mutex<bool> = Mutex::new(false);

    pub static ref UNLUAC: Mutex<String> = Mutex::new("unluac.jar".to_string());

    pub static ref COMPRESSION: Mutex<flate2::Compression> = Mutex::new(flate2::Compression::default());

    pub static ref ANIM_TABLES: Mutex<bool> = Mutex::new(true);

    pub static ref ZIP: Mutex<bool> = Mutex::new(false);

    pub static ref GLTF: Mutex<bool> = Mutex::new(false);

    pub static ref ALT_OBJS: Mutex<bool> = Mutex::new(false);
}

#[pyfunction]
#[pyo3(signature = (val=None))]
pub fn decomp_lua(val: Option<bool>) -> bool {
    let mut global = DECOMP_LUA.lock().unwrap();
    if let Some(val) = val {
        *global = val;
    }
    *global
}

#[pyfunction]
#[pyo3(signature = (val=None))]
pub fn recomp_lua(val: Option<bool>) -> bool {
    let mut global = RECOMP_LUA.lock().unwrap();
    if let Some(val) = val {
        *global = val;
    }
    *global
}

#[pyfunction]
#[pyo3(signature = (val=None))]
pub fn unluac(val: Option<String>) -> String {
    let mut global = UNLUAC.lock().unwrap();
    if let Some(val) = val {
        *global = val;
    }
    (*global).clone()
}

#[pyfunction]
#[pyo3(signature = (val=None))]
pub fn compression(val: Option<u32>) -> u32 {
    let mut global = COMPRESSION.lock().unwrap();
    if let Some(val) = val {
        *global = flate2::Compression::new(val);
    }
    global.level()
}

#[pyfunction]
#[pyo3(signature = (val=None))]
pub fn anim_tables(val: Option<bool>) -> bool {
    let mut global = ANIM_TABLES.lock().unwrap();
    if let Some(val) = val {
        *global = val;
    }
    *global
}

#[pyfunction]
#[pyo3(name = "zip", signature = (val=None))]
pub fn zip_(val: Option<bool>) -> bool {
    let mut global = ZIP.lock().unwrap();
    if let Some(val) = val {
        *global = val;
    }
    *global
}

#[pyfunction]
#[pyo3(signature = (val=None))]
pub fn gltf(val: Option<bool>) -> bool {
    let mut global = GLTF.lock().unwrap();
    if let Some(val) = val {
        *global = val;
    }
    *global
}

#[pyfunction]
#[pyo3(signature = (val=None))]
pub fn alt_objs(val: Option<bool>) -> bool {
    let mut global = ALT_OBJS.lock().unwrap();
    if let Some(val) = val {
        *global = val;
    }
    *global
}

#[pyfunction]
pub fn crc_string(val: u32) -> Option<String> {
    STRING_LOOKUP.lock().unwrap().get(&val).cloned()
}

pub fn update_strings(vals: &[String]) {
    let mut strings = STRING_LOOKUP.lock().unwrap();
    let new_strings: Vec<_> = vals.iter().filter_map(|x| {
        let key = hash_string(x.as_bytes(), None);
        if strings.contains_key(&key) {
            None
        } else {
            Some((key, x.clone()))
        }
    }).collect();
    strings.extend(new_strings);
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(from = "U32String", into = "String")]
pub enum Crc {
    Str(String),
    Key(u32)
}

impl <'py> IntoPyObject<'py> for Crc {
    type Target = <String as IntoPyObject<'py>>::Target;
    type Output = <String as IntoPyObject<'py>>::Output;
    type Error = <String as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.to_string().into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Crc {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self::from_string(String::extract_bound(ob)?.as_str()))
    }
}

impl Crc {
    pub fn key(&self) -> u32 {
        match self {
            Self::Key(val) => *val,
            Self::Str(val) => hash_string(val.as_bytes(), None)
        }
    }

    pub fn str(&self) -> Option<&str> {
        match self {
            Self::Key(_) => None,
            Self::Str(val) => Some(val)
        }
    }

    pub fn to_string(&self) -> String {
        match self {
            Self::Key(val) => format!("0x{val:08X}"),
            Self::Str(val) => val.to_string()
        }
    }

    pub fn from_key(val: u32) -> Self {
        match STRING_LOOKUP.lock().ok().and_then(|m| m.get(&val).map(|x| x.clone())) {
            Some(str) => Self::Str(str),
            None => Self::Key(val)
        }
    }

    pub fn from_string(val: &str) -> Self {
        if val.starts_with("0x") {
            Self::from_key(u32::from_str_radix(&val[2..], 16).unwrap())
        } else {
            Self::Str(val.into())
        }
    }
}

impl PartialEq for Crc {
    fn eq(&self, other: &Self) -> bool {
        self.key() == other.key()
    }
}

impl PartialOrd for Crc {
    fn partial_cmp(&self, other: &Self) -> std::option::Option<std::cmp::Ordering> {
        self.key().partial_cmp(&other.key())
    }

}

impl std::hash::Hash for Crc {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.key().hash(state)
    }
}

impl Eq for Crc {}
impl Ord for Crc {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        self.key().cmp(&other.key())
    }
}

impl Default for Crc {
    fn default() -> Self {
        Self::Key(0)
    }
}

impl<O: ByteOrder> From<U32<O>> for Crc {
    fn from(value: U32<O>) -> Self {
        Self::from_key(value.into())
    }
}

impl<O: ByteOrder> From<Crc> for U32<O> {
    fn from(value: Crc) -> Self {
        match value {
            Crc::Str(val) => hash_string(val.as_bytes(), None),
            Crc::Key(val) => val,
        }.into()
    }
}

impl From<&str> for Crc {
    fn from(value: &str) -> Self {
        Crc::from_string(value)
    }
}

impl From<Crc> for String {
    fn from(value: Crc) -> Self {
        value.to_string()
    }
}

impl From<U32String> for Crc {
    fn from(value: U32String) -> Self {
        match value {
            U32String::U32(x) => Crc::from_key(x),
            U32String::String(x) => Crc::from_string(&x),
        }
    }
}

impl OrderedData for Crc { type PC = U32<LE>; type XBOX = U32<BE>; type PS3 = U32<BE>; }

#[derive(Serialize, Deserialize)]
#[serde(untagged)]
enum U32String {
    U32(u32),
    String(String),
}

#[derive(Default, Debug, Clone, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Strings {
    pub strings: Vec<String>,
}

impl <'py> IntoPyObject<'py> for Strings {
    type Target = <Vec<String> as IntoPyObject<'py>>::Target;
    type Output = <Vec<String> as IntoPyObject<'py>>::Output;
    type Error = <Vec<String> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.strings.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Strings {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { strings: Vec::extract_bound(ob)? })
    }
}

impl AsData<'_, '_> for Strings {
    type InArgs = usize;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], num: Self::InArgs) -> Result<Self> {
        let mut strings = Vec::with_capacity(num);
        let mut offset = 0;
        for _ in 0..num {
            let k = from_bytes!(V, u32, &data[offset..])? as usize;
            offset += 4;
            strings.push(String::from_utf8_lossy(&data[offset..offset+k]).to_string());
            offset += k;
        }
        Ok(Self { strings, ..Default::default() })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        self.strings.iter().flat_map(|string| {
            dump_bytes!(V, (string.len() as u32)).into_iter().chain(string.as_bytes().iter().cloned()).collect::<Vec<_>>()
        }).collect()
    }

    fn size<V: Version>(&self) -> usize {
        self.strings.iter().map(|x| x.len()).sum::<usize>() + 4 * self.strings.len()
    }
}

impl Strings {
    pub fn len(&self) -> usize {
        self.strings.len()
    }

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.with_extension("json").write(&to_vec_pretty(&json!(self.strings))?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        let name = reader.path().display().to_string();
        let strings = from_slice::<Vec<String>>(&reader.with_extension("json").read()?).context(format!("{}", name))?;
        Ok(Self { strings })
    }
}

#[derive(Default, Debug, Clone, Serialize, Deserialize)]
pub struct CompressedBlock {
    pub data: Vec<u8>,
}

impl <'py> IntoPyObject<'py> for CompressedBlock {
    type Target = <Vec<u8> as IntoPyObject<'py>>::Target;
    type Output = <Vec<u8> as IntoPyObject<'py>>::Output;
    type Error = <Vec<u8> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.data.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for CompressedBlock {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { data: Vec::extract_bound(ob)? })
    }
}

impl CompressedBlock {
    pub fn from_data<O: Version + 'static>(data: &[u8], size: usize, size_comp: usize, offset: usize) -> Self {
        let data = match size_comp {
            0 => data[offset..offset + size].to_vec(),
            _ => if TypeId::of::<O>() == TypeId::of::<PS3>() {
                let mut out = Vec::with_capacity(size);
                let data = &data[offset..offset+size_comp];
                let mut s = 2;
                while s < size_comp {
                    let mut decoder = ZlibDecoder::new_with_decompress(&data[s..], Decompress::new(false));
                    decoder.read_to_end(&mut out).unwrap();
                    s += decoder.total_in() as usize + 2;
                }
                out
            } else {
                let mut out = Vec::with_capacity(size);
                ZlibDecoder::new(&data[offset..offset+size_comp]).read_to_end(&mut out).unwrap();
                out
            }
        };
        Self { data }
    }

    pub fn dump(&self, zero_alt: bool) -> Result<Vec<u8>> {
        if self.data.is_empty() { return Ok(Vec::new()); }
        let mut c = COMPRESSION.lock().unwrap().clone();
        Ok(if c.level() == 0 && zero_alt {
            self.data.clone()
        } else {
            // let mut z = ZlibEncoder::new(Vec::new(), flate2::Compression::fast());
            let mut z = ZlibEncoder::new(Vec::new(), c);
            z.write_all(self.data.as_slice())?;
            let mut data = z.finish()?;
            while data.len() > 0xffffff {
                if c.level() == 9 {
                    return Err(anyhow!("Bin Asset could not be compressed to a small enough size"))
                }
                c = flate2::Compression::new(c.level() + 1);
                z = ZlibEncoder::new(Vec::new(), c);
                z.write_all(self.data.as_slice())?;
                data = z.finish()?;
            }
            data
        })
    }
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct List {
    pub num: u16,
    pub offset: u16,
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct Vector2 {
    pub x: f32,
    pub y: f32,
}

impl From<(f32, f32)> for Vector2 {
    fn from((x,y): (f32, f32)) -> Self {
        Self { x, y }
    }
}

impl <'py> IntoPyObject<'py> for Vector2 {
    type Target = <(f32, f32) as IntoPyObject<'py>>::Target;
    type Output = <(f32, f32) as IntoPyObject<'py>>::Output;
    type Error = <(f32, f32) as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        (self.x, self.y).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Vector2 {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(<(f32, f32)>::extract_bound(ob)?.into())
    }
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct Vector3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl From<(f32, f32, f32)> for Vector3 {
    fn from((x,y,z): (f32, f32, f32)) -> Self {
        Self { x, y, z }
    }
}

impl <'py> IntoPyObject<'py> for Vector3 {
    type Target = <(f32, f32, f32) as IntoPyObject<'py>>::Target;
    type Output = <(f32, f32, f32) as IntoPyObject<'py>>::Output;
    type Error = <(f32, f32, f32) as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        (self.x, self.y, self.z).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Vector3 {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(<(f32, f32, f32)>::extract_bound(ob)?.into())
    }
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct Vector4 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

impl From<(f32, f32, f32, f32)> for Vector4 {
    fn from((x,y,z,w): (f32, f32, f32, f32)) -> Self {
        Self { x, y, z, w }
    }
}

impl <'py> IntoPyObject<'py> for Vector4 {
    type Target = <(f32, f32, f32, f32) as IntoPyObject<'py>>::Target;
    type Output = <(f32, f32, f32, f32) as IntoPyObject<'py>>::Output;
    type Error = <(f32, f32, f32, f32) as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        (self.x, self.y, self.z, self.w).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Vector4 {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(<(f32, f32, f32, f32)>::extract_bound(ob)?.into())
    }
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct Matrix4x4 {
    pub x: Vector4,
    pub y: Vector4,
    pub z: Vector4,
    pub w: Vector4,
}

impl <'py> IntoPyObject<'py> for Matrix4x4 {
    type Target = <[Vector4; 4] as IntoPyObject<'py>>::Target;
    type Output = <[Vector4; 4] as IntoPyObject<'py>>::Output;
    type Error = <[Vector4; 4] as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        [self.x, self.y, self.z, self.w].into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Matrix4x4 {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        let [x,y,z,w] = <[Vector4; 4]>::extract_bound(ob)?;
        Ok(Self { x, y, z, w })
    }
}

impl From<&[f32; 16]> for Matrix4x4 {
    fn from(x: &[f32; 16]) -> Self {
        Self {
            x: Vector4 { x: x[0], y: x[1], z: x[2], w: x[3] },
            y: Vector4 { x: x[4], y: x[5], z: x[6], w: x[7] },
            z: Vector4 { x: x[8], y: x[9], z: x[10], w: x[11] },
            w: Vector4 { x: x[12], y: x[13], z: x[14], w: x[15] },
        }
    }
}

impl std::convert::TryFrom<&[f32]> for Matrix4x4 {
    type Error = ();
    fn try_from(x: &[f32]) -> Result<Self, ()> {
        if x.len() >= 16 { Ok(Self {
            x: Vector4 { x: x[0], y: x[1], z: x[2], w: x[3] },
            y: Vector4 { x: x[4], y: x[5], z: x[6], w: x[7] },
            z: Vector4 { x: x[8], y: x[9], z: x[10], w: x[11] },
            w: Vector4 { x: x[12], y: x[13], z: x[14], w: x[15] },
        }) } else { Err(()) }
    }
}

impl From<&Matrix4x4> for [f32; 16] {
    fn from(mat: &Matrix4x4) -> Self { [
        mat.x.x, mat.x.y, mat.x.z, mat.x.w,
        mat.y.x, mat.y.y, mat.y.z, mat.y.w,
        mat.z.x, mat.z.y, mat.z.z, mat.z.w,
        mat.w.x, mat.w.y, mat.w.z, mat.w.w
    ]}
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
#[serde(from = "bool", into = "bool")]
pub struct Bool {
    pub val: u8,
    pub _pad1: u8,
    pub _pad2: u8,
    pub _pad3: u8,
}

impl From<bool> for Bool {
    fn from(value: bool) -> Self {
        Self {
            val: if value { 1 } else { 0 },
            ..Default::default()
        }
    }
}

impl From<Bool> for bool {
    fn from(value: Bool) -> Self {
        value.val != 0
    }
}

impl <'py> IntoPyObject<'py> for Bool {
    type Target = <bool as IntoPyObject<'py>>::Target;
    type Output = <bool as IntoPyObject<'py>>::Output;
    type Error = <bool as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        bool::from(self).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Bool {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self::from(bool::extract_bound(ob)?))
    }
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct Weight {
    pub x: u32,
    pub a: u8,
    pub b: u8,
    pub c: u8,
    pub d: u8,
}

impl From<(u32, u8, u8, u8, u8)> for Weight {
    fn from((x, a, b, c, d): (u32, u8, u8, u8, u8)) -> Self {
        Self { x, a, b, c, d }
    }
}

impl <'py> IntoPyObject<'py> for Weight {
    type Target = <(u32, u8, u8, u8, u8) as IntoPyObject<'py>>::Target;
    type Output = <(u32, u8, u8, u8, u8) as IntoPyObject<'py>>::Output;
    type Error = <(u32, u8, u8, u8, u8) as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        (self.x, self.a, self.b, self.c, self.d).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Weight {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(<(u32, u8, u8, u8, u8)>::extract_bound(ob)?.into())
    }
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
#[serde(try_from = "&str", into = "String")]
pub struct Color(pub u32);

impl From<Color> for String {
    fn from(value: Color) -> Self {
        format!("0x{:08X}", value.0)
    }
}

impl TryFrom<&str> for Color  {
    type Error = std::num::ParseIntError;
    fn try_from(value: &str) -> Result<Self, Self::Error> {
        u32::from_str_radix(&value[2..], 16).map(|x| Self(x))
    }
}

impl <'py> IntoPyObject<'py> for Color {
    type Target = <String as IntoPyObject<'py>>::Target;
    type Output = <String as IntoPyObject<'py>>::Output;
    type Error = <String as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        format!("0x{:08X}", self.0).into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Color {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        let val: String = ob.extract()?;
        Ok(Self(u32::from_str_radix(&val[2..], 16)?))
    }
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum BaseTypes {
    CRC(Crc),
    GUID(u32),
    Color(Color),
    Vector2(Vector2),
    Vector3(Vector3),
    Vector4(Vector4),
    Matrix4x4(Matrix4x4),
    Float(f32),
    Int(i32),
    Bool(Bool),
    String(String),
    StringList(Vec<String>),
    ObjectList(Vec<u32>),
    NodeList(Vec<Vector4>),
    IntList(Vec<i32>),
    CRCList(Vec<Crc>),
    WeightList(Vec<Weight>),
    MatrixList(Vec<Matrix4x4>),
    Byte(u8),
}

impl BaseTypes {
    pub const CRC_KEY: u32 = hash_string("CRC".as_bytes(), None);
    pub const GUID_KEY: u32 = hash_string("GUID".as_bytes(), None);
    pub const COLOR_KEY: u32 = hash_string("Color".as_bytes(), None);
    pub const VECTOR2_KEY: u32 = hash_string("Vector2".as_bytes(), None);
    pub const VECTOR3_KEY: u32 = hash_string("Vector3".as_bytes(), None);
    pub const VECTOR4_KEY: u32 = hash_string("Vector4".as_bytes(), None);
    pub const MATRIX4X4_KEY: u32 = hash_string("Matrix4x4".as_bytes(), None);
    pub const FLOAT_KEY: u32 = hash_string("Float".as_bytes(), None);
    pub const INT_KEY: u32 = hash_string("Int".as_bytes(), None);
    pub const BOOL_KEY: u32 = hash_string("Bool".as_bytes(), None);
    pub const BYTE_KEY: u32 = hash_string("Byte".as_bytes(), None);
    pub const STRING_KEY: u32 = hash_string("String".as_bytes(), None);
    pub const STRINGLIST_KEY: u32 = hash_string("StringList".as_bytes(), None);
    pub const OBJECTLIST_KEY: u32 = hash_string("ObjectList".as_bytes(), None);
    pub const NODELIST_KEY: u32 = hash_string("NodeList".as_bytes(), None);
    pub const INTLISTS_KEY: u32 = hash_string("IntList".as_bytes(), None);
    pub const CRCLIST_KEY: u32 = hash_string("CRCList".as_bytes(), None);
    pub const WEIGHTLIST_KEY: u32 = hash_string("WeightList".as_bytes(), None);
    pub const MATRIXLIST_KEY: u32 = hash_string("MatrixList".as_bytes(), None);
    pub fn from_data<O: Version>(data: &[u8], kind: u32) -> Result<Self> {
        Ok(match kind {
            Self::CRC_KEY => Self::CRC(from_bytes!(O, data)?),
            Self::GUID_KEY => Self::GUID(from_bytes!(O, data)?),
            Self::COLOR_KEY => Self::Color(from_bytes!(O, data)?),
            Self::VECTOR2_KEY => Self::Vector2(from_bytes!(O, data)?),
            Self::VECTOR3_KEY => Self::Vector3(from_bytes!(O, data)?),
            Self::VECTOR4_KEY => Self::Vector4(from_bytes!(O, data)?),
            Self::MATRIX4X4_KEY => Self::Matrix4x4(from_bytes!(O, data)?),
            Self::FLOAT_KEY => Self::Float(from_bytes!(O, data)?),
            Self::INT_KEY  => Self::Int(from_bytes!(O, data)?),
            Self::BOOL_KEY => Self::Bool(from_bytes!(O, data)?),
            Self::BYTE_KEY => Self::Byte(from_bytes!(O, data)?),
            Self::STRING_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::String(String::from_utf8(vals).unwrap())
            },
            Self::STRINGLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals: Vec<List> = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                let valss = vals.iter().enumerate().map(|(i, v)| { Ok(String::from_utf8(
                    from_bytes!(O, &data[val.offset as usize + val.size::<O>() * (i + 2) + v.offset as usize..], v.num as usize)?
                ).unwrap())}).collect::<Result<Vec<_>>>()?;
                Self::StringList(valss)
            },
            Self::OBJECTLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::ObjectList(vals)
            },
            Self::NODELIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::NodeList(vals)
            },
            Self::INTLISTS_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::IntList(vals)
            },
            Self::CRCLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::CRCList(vals)
            },
            Self::WEIGHTLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::WeightList(vals)
            },
            Self::MATRIXLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::MatrixList(vals)
            },
            _ => panic!("Unkown Type {:?}", kind)
        })
    }

    pub fn into_data<O: Version>(&self, data: &mut [u8], off: &mut usize) -> Result<()> {
        match self {
            Self::CRC(val) => to_bytes!(O, val, data)?,
            Self::GUID(val) => to_bytes!(O, val, data)?,
            Self::Color(val) => to_bytes!(O, val, data)?,
            Self::Vector2(val) => to_bytes!(O, val, data)?,
            Self::Vector3(val) => to_bytes!(O, val, data)?,
            Self::Vector4(val) => to_bytes!(O, val, data)?,
            Self::Matrix4x4(val) => to_bytes!(O, val, data)?,
            Self::Float(val) => to_bytes!(O, val, data)?,
            Self::Int(val) => to_bytes!(O, val, data)?,
            Self::Bool(val) => to_bytes!(O, val, data)?,
            Self::Byte(val) => to_bytes!(O, val, data)?,
            Self::String(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                data[*off..*off + vals.len()].copy_from_slice(vals.as_bytes());
                if vals.len() != 0 { *off += vals.len() + 1 };
            },
            Self::StringList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                let mut off_ = *off;
                *off += vals.len() * O::size::<List>();
                for v in vals {
                    to_bytes!(O, List{ num: v.len() as u16, offset: (*off - off_ - O::size::<List>()) as u16}, data)?;
                    data[*off..*off + v.len()].copy_from_slice(v.as_bytes());                    
                    if v.len() != 0 {
                        *off += v.len() + 1;
                    }
                    off_ += 4;
                }
            },
            Self::ObjectList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                to_bytes!(O, vals, &mut data[*off..])?;
                *off += vals.size::<O>();
            },
            Self:: NodeList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                to_bytes!(O, vals, &mut data[*off..])?;
                *off += vals.size::<O>();
            },
            Self::IntList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                to_bytes!(O, vals, &mut data[*off..])?;
                *off += vals.size::<O>();
            },
            Self::CRCList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                to_bytes!(O, vals, &mut data[*off..])?;
                *off += vals.size::<O>();
            },
            Self::WeightList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                to_bytes!(O, vals, &mut data[*off..])?;
                *off += vals.size::<O>();
            },
            Self::MatrixList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (*off - O::size::<List>()) as u16}, data)?;
                to_bytes!(O, vals, &mut data[*off..])?;
                *off += vals.size::<O>();
            },
        };
        Ok(())
    }

    pub fn dump<O: Version>(&self, data: &mut Vec<u8>, off: usize) -> Result<()> {
        match self {
            Self::CRC(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::GUID(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Color(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Vector2(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Vector3(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Vector4(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Matrix4x4(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Float(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Int(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Bool(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::Byte(val) => to_bytes!(O, val, &mut data[off..])?,
            Self::String(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                data.extend(vals.as_bytes());
                if vals.len() != 0 { data.push(0) };
            },
            Self::StringList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                let mut off_ = data.len();
                data.extend(vec![0u8; vals.len() * O::size::<List>()]);
                for v in vals {
                    to_bytes!(O, List{ num: v.len() as u16, offset: (data.len() - off_ - O::size::<List>()) as u16}, &mut data[off_..])?;
                    data.extend(v.as_bytes());
                    if v.len() != 0 { data.push(0) }
                    off_ += 4;
                }
            },
            Self::ObjectList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                data.extend(dump_bytes!(O, vals));
            },
            Self:: NodeList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                data.extend(dump_bytes!(O, vals));
            },
            Self::IntList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                data.extend(dump_bytes!(O, vals));
            },
            Self::CRCList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                data.extend(dump_bytes!(O, vals));
            },
            Self::WeightList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                data.extend(dump_bytes!(O, vals));
            },
            Self::MatrixList(vals) => {
                to_bytes!(O, List{ num: vals.len() as u16, offset: (data.len() - off - O::size::<List>()) as u16}, &mut data[off..])?;
                data.extend(dump_bytes!(O, vals));
            },
        }
        Ok(())
    }

    pub fn off_size<O: Version>(&self) -> usize {
        match self {
            Self::String(vals) => {
                if vals.len() != 0 { vals.len() + 1 } else { 0 }
            },
            Self::StringList(vals) => {
                let mut s = vals.len() * O::size::<List>();
                for v in vals {
                    if v.len() != 0 {
                        s += v.len() + 1;
                    }
                }
                s
            },
            Self::ObjectList(vals) => {
                vals.size::<O>()
            },
            Self::NodeList(vals) => {
                vals.size::<O>()
            },
            Self::IntList(vals) => {
                vals.size::<O>()
            },
            Self::CRCList(vals) => {
                vals.size::<O>()
            },
            Self::WeightList(vals) => {
                vals.size::<O>()
            },
            Self::MatrixList(vals) => {
                vals.size::<O>()
            },
            _ => 0,
        }
    }


    pub fn to_json(&self) -> Value {
        match self {
            Self::CRC(val) => json!(val.to_string()),
            Self::GUID(val) => json!(val),
            Self::Color(val) => json!(String::from(val.clone())),
            Self::Vector2(val) => json!([val.x, val.y]),
            Self::Vector3(val) => json!([val.x, val.y, val.z]),
            Self::Vector4(val) => json!([val.x, val.y, val.z, val.w]),
            Self::Matrix4x4(val) => json!([
                val.x.x, val.x.y, val.x.z, val.x.w,
                val.y.x, val.y.y, val.y.z, val.y.w,
                val.z.x, val.z.y, val.z.z, val.z.w,
                val.w.x, val.w.y, val.w.z, val.w.w,
            ]),
            Self::Float(val) => json!(val),
            Self::Int(val) => json!(val),
            Self::Bool(val) => json!(val.val != 0),
            Self::Byte(val) => json!(val),
            Self::String(vals) => json!(vals),
            Self::StringList(vals) => json!(vals.iter().map(|x| json!(x)).collect::<Vec<_>>()),
            Self::ObjectList(vals) => json!(vals.iter().map(|x| json!(x)).collect::<Vec<_>>()),
            Self::NodeList(vals) => json!(vals.iter().map(|x| json!([x.x, x.y, x.z, x.w])).collect::<Vec<_>>()),
            Self::IntList(vals) => json!(vals.iter().map(|x| json!(x)).collect::<Vec<_>>()),
            Self::CRCList(vals) => json!(vals.iter().map(|x| json!(x.to_string())).collect::<Vec<_>>()),
            Self::WeightList(vals) => json!(vals.iter().map(|x| json!([x.x, x.a, x.b, x.c, x.d])).collect::<Vec<_>>()),
            Self::MatrixList(vals) => json!(vals.iter().map(|val| json!([
                val.x.x, val.x.y, val.x.z, val.x.w,
                val.y.x, val.y.y, val.y.z, val.y.w,
                val.z.x, val.z.y, val.z.z, val.z.w,
                val.w.x, val.w.y, val.w.z, val.w.w,
            ])).collect::<Vec<_>>()),
        }
    }

    pub fn from_json(val: Value, kind: u32) -> Result<Self> {
        Ok(match kind {
            Self::CRC_KEY => Self::CRC(Crc::from_string(&serde_json::from_value::<String>(val)?)),
            Self::GUID_KEY => Self::GUID(serde_json::from_value::<u32>(val)?),
            Self::COLOR_KEY => Self::Color(Color::try_from(serde_json::from_value::<String>(val)?.as_str())?),
            Self::VECTOR2_KEY => Self::Vector2(serde_json::from_value::<(f32, f32)>(val)?.into()),
            Self::VECTOR3_KEY => Self::Vector3(serde_json::from_value::<(f32, f32, f32)>(val)?.into()),
            Self::VECTOR4_KEY => Self::Vector4(serde_json::from_value::<(f32, f32, f32, f32)>(val)?.into()),
            Self::MATRIX4X4_KEY => Self::Matrix4x4((&serde_json::from_value::<[f32; 16]>(val)?).into()),
            Self::FLOAT_KEY => Self::Float(serde_json::from_value::<f32>(val)?),
            Self::INT_KEY  => Self::Int(serde_json::from_value::<i32>(val)?),
            Self::BOOL_KEY => Self::Bool(serde_json::from_value::<bool>(val)?.into()),
            Self::BYTE_KEY => Self::Byte(serde_json::from_value::<u8>(val)?),
            Self::STRING_KEY => Self::String(serde_json::from_value::<String>(val)?),
            Self::STRINGLIST_KEY => Self::StringList(serde_json::from_value::<Vec<String>>(val)?),
            Self::OBJECTLIST_KEY => Self::ObjectList(serde_json::from_value::<Vec<u32>>(val)?),
            Self::NODELIST_KEY => Self::NodeList(serde_json::from_value::<Vec<(f32, f32, f32, f32)>>(val)?.into_iter().map(|x| x.into()).collect()),
            Self::INTLISTS_KEY => Self::IntList(serde_json::from_value::<Vec<i32>>(val)?),
            Self::CRCLIST_KEY => Self::CRCList(serde_json::from_value::<Vec<String>>(val)?.into_iter().map(|x| Crc::from_string(&x)).collect()),
            Self::WEIGHTLIST_KEY => Self::WeightList(serde_json::from_value::<Vec<(u32, u8, u8, u8, u8)>>(val)?.into_iter().map(|x| x.into()).collect()),
            Self::MATRIXLIST_KEY => Self::MatrixList(serde_json::from_value::<Vec<[f32; 16]>>(val)?.iter().map(|x| x.into()).collect()),
            _ => Err(anyhow!("Unkown Type {:?}", kind))?
        })
    }
}

impl AsData<'_, '_> for BaseTypes {
    type InArgs = u32;
    type OutArgs = NoArgs;

    fn from_bytes<O: Version>(data: &[u8], kind: u32) -> Result<Self> {
        Ok(match kind {
            Self::CRC_KEY => Self::CRC(from_bytes!(O, data)?),
            Self::GUID_KEY => Self::GUID(from_bytes!(O, data)?),
            Self::COLOR_KEY => Self::Color(from_bytes!(O, data)?),
            Self::VECTOR2_KEY => Self::Vector2(from_bytes!(O, data)?),
            Self::VECTOR3_KEY => Self::Vector3(from_bytes!(O, data)?),
            Self::VECTOR4_KEY => Self::Vector4(from_bytes!(O, data)?),
            Self::MATRIX4X4_KEY => Self::Matrix4x4(from_bytes!(O, data)?),
            Self::FLOAT_KEY => Self::Float(from_bytes!(O, data)?),
            Self::INT_KEY  => Self::Int(from_bytes!(O, data)?),
            Self::BOOL_KEY => Self::Bool(from_bytes!(O, data)?),
            Self::BYTE_KEY => Self::Byte(from_bytes!(O, data)?),
            Self::STRING_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::String(String::from_utf8(vals).unwrap())
            },
            Self::STRINGLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals: Vec<List> = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                let valss = vals.iter().enumerate().map(|(i, v)| { Ok(String::from_utf8(
                    from_bytes!(O, &data[val.offset as usize + val.size::<O>() * (i + 2) + v.offset as usize..], v.num as usize)?
                ).unwrap())}).collect::<Result<Vec<_>>>()?;
                Self::StringList(valss)
            },
            Self::OBJECTLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::ObjectList(vals)
            },
            Self::NODELIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::NodeList(vals)
            },
            Self::INTLISTS_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::IntList(vals)
            },
            Self::CRCLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::CRCList(vals)
            },
            Self::WEIGHTLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::WeightList(vals)
            },
            Self::MATRIXLIST_KEY => {
                let val: List = from_bytes!(O, data)?;
                let vals = from_bytes!(O, &data[val.offset as usize + val.size::<O>()..], val.num as usize)?;
                Self::MatrixList(vals)
            },
            _ => panic!("Unkown Type {:?}", kind)
        })
    }

    fn dump_bytes<O: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        match self {
            Self::CRC(val) => dump_bytes!(O, val),
            Self::GUID(val) => dump_bytes!(O, val),
            Self::Color(val) => dump_bytes!(O, val),
            Self::Vector2(val) => dump_bytes!(O, val),
            Self::Vector3(val) => dump_bytes!(O, val),
            Self::Vector4(val) => dump_bytes!(O, val),
            Self::Matrix4x4(val) => dump_bytes!(O, val),
            Self::Float(val) => dump_bytes!(O, val),
            Self::Int(val) => dump_bytes!(O, val),
            Self::Bool(val) => dump_bytes!(O, val),
            Self::Byte(val) => dump_bytes!(O, val),
            _ => panic!("Not implemented for this type"),
        }
    }

    fn size<O: Version>(&self) -> usize {
        match self {
            Self::CRC(..) => O::size::<u32>(),
            Self::GUID(..) => O::size::<u32>(),
            Self::Color(..) => O::size::<u32>(),
            Self::Vector2(..) => O::size::<Vector2>(),
            Self::Vector3(..) => O::size::<Vector3>(),
            Self::Vector4(..) => O::size::<Vector4>(),
            Self::Matrix4x4(..) => O::size::<Matrix4x4>(),
            Self::Float(..) => O::size::<f32>(),
            Self::Int(..) => O::size::<u32>(),
            Self::Bool(..) => O::size::<Bool>(),
            Self::Byte(..) => O::size::<u8>(),
            Self::String(..) => O::size::<List>(),
            Self::StringList(..) => O::size::<List>(),
            Self::ObjectList(..) => O::size::<List>(),
            Self::NodeList(..) => O::size::<List>(),
            Self::IntList(..) => O::size::<List>(),
            Self::CRCList(..) => O::size::<List>(),
            Self::WeightList(..) => O::size::<List>(),
            Self::MatrixList(..) => O::size::<List>(),
        }
    }
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
pub enum SubBlock {
    LangStrings(LangStrings),
    Data(Data),
    Spray(Spray),
    PFields(PFields),
    Crowd(Crowd),
    GameObjs(GameObjs),
    AtlasUV(AtlasUV),
    Lua(Lua),
    SSA(SSA),
}

impl AsData<'_, '_> for SubBlock {
    type InArgs = (Crc, usize);
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], (key, size): Self::InArgs) -> Result<Self> {
        Ok(match key.key() {
            LangStrings::KEY_POLISH | LangStrings::KEY_GERMAN | LangStrings::KEY_FRENCH | LangStrings::KEY_SPANISH | LangStrings::KEY_RUSSIAN | LangStrings::KEY_SWEDISH | LangStrings::KEY_ENGLISH | LangStrings::KEY_ITALIAN | LangStrings::KEY_NORWEGIAN => SubBlock::LangStrings(from_bytes!(V, LangStrings, data, size)?),
            Spray::KEY => SubBlock::Spray(from_bytes!(V, Spray, data)?),
            Crowd::KEY => SubBlock::Crowd(from_bytes!(V, Crowd, data)?),
            PFields::KEY => SubBlock::PFields(from_bytes!(V, PFields, data, size)?),
            GameObjs::KEY => SubBlock::GameObjs(from_bytes!(V, GameObjs, data, -1)?),
            AtlasUV::KEY1 | AtlasUV::KEY2 => SubBlock::AtlasUV(from_bytes!(V, AtlasUV, data, size)?),
            _ => match key.str() {
                Some(x) if x.ends_with(".lua") => SubBlock::Lua(from_bytes!(V, Lua, data, size, x.to_string())?),
                Some(x) if x.ends_with(".ssa") => SubBlock::SSA(from_bytes!(V, SSA, data, size)?),
                Some(x) if x.ends_with(".csv") || x.ends_with(".txt") || x.ends_with(".dat") => 
                    SubBlock::Data(from_bytes!(V, Data, data, size)?),
                _ => {
                    warn!("Unknown block type {:?}", key);
                    SubBlock::Data(from_bytes!(V, Data, data, size)?)
                } 
            }
        })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        match self {
            SubBlock::LangStrings(val) => dump_bytes!(V, val),
            SubBlock::Data(val) => dump_bytes!(V, val),
            SubBlock::Spray(val) => dump_bytes!(V, val),
            SubBlock::Crowd(val) => dump_bytes!(V, val),
            SubBlock::PFields(val) => dump_bytes!(V, val),
            SubBlock::GameObjs(val) => dump_bytes!(V, val),
            SubBlock::AtlasUV(val) => dump_bytes!(V, val),
            SubBlock::Lua(val) => dump_bytes!(V, val),
            SubBlock::SSA(val) => dump_bytes!(V, val),
        }
    }

    fn size<V: Version>(&self) -> usize {
        match self {
            SubBlock::LangStrings(val) => val.size::<V>(),
            SubBlock::Data(val) => val.size::<V>(),
            SubBlock::Spray(val) => val.size::<V>(),
            SubBlock::Crowd(val) => val.size::<V>(),
            SubBlock::PFields(val) => val.size::<V>(),
            SubBlock::GameObjs(val) => val.size::<V>(),
            SubBlock::AtlasUV(val) => val.size::<V>(),
            SubBlock::Lua(val) => val.size::<V>(),
            SubBlock::SSA(val) => val.size::<V>(),
        }
    }
}

impl SubBlock {
    pub fn to_file(&self, writer: Writer, keys: &StringKeys) -> Result<()> {
        match self {
            SubBlock::LangStrings(val) => val.to_file(writer, keys),
            SubBlock::Data(val) => val.to_file(writer),
            SubBlock::Spray(val) => val.to_file(writer),
            SubBlock::Crowd(val) => val.to_file(writer),
            SubBlock::PFields(val) => val.to_file(writer),
            SubBlock::GameObjs(val) => val.to_file(writer),
            SubBlock::AtlasUV(val) => val.to_file(writer),
            SubBlock::Lua(val) => val.to_file(writer),
            SubBlock::SSA(val) => val.to_file(writer),
        }
    }

    pub fn from_file(reader: Reader, key: &Crc) -> Result<Self> {
        Ok(match key.key() {
            LangStrings::KEY_POLISH | LangStrings::KEY_GERMAN | LangStrings::KEY_FRENCH | LangStrings::KEY_SPANISH | LangStrings::KEY_RUSSIAN | LangStrings::KEY_SWEDISH | LangStrings::KEY_ENGLISH | LangStrings::KEY_ITALIAN | LangStrings::KEY_NORWEGIAN => 
                SubBlock::LangStrings(LangStrings::from_file(reader)?),
            Spray::KEY => SubBlock::Spray(Spray::from_file(reader)?),
            Crowd::KEY => SubBlock::Crowd(Crowd::from_file(reader)?),
            PFields::KEY => SubBlock::PFields(PFields::from_file(reader)?),
            GameObjs::KEY => SubBlock::GameObjs(GameObjs::from_file(reader.with_file_name("level.json"))?),
            AtlasUV::KEY1 | AtlasUV::KEY2 => SubBlock::AtlasUV(AtlasUV::from_file(reader)?),
            _ => match key.str() {
                Some(x) if x.ends_with(".lua") => SubBlock::Lua(Lua::from_file(reader)?),
                Some(x) if x.ends_with(".ssa") => SubBlock::SSA(SSA::from_file(reader)?),
                Some(x) if x.ends_with(".csv") || x.ends_with(".txt") || x.ends_with(".dat") => 
                    SubBlock::Data(Data::from_file(reader)?),
                _ =>  {
                    warn!("Unknown block type {:?}", key);
                    SubBlock::Data(Data::from_file(reader)?)
                } 
            }
        })
    }
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct SubBlocksHeader {
    pub z0: u32,
    pub block_num: u32,
    pub z2: u32,
    pub z3: u32,
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct SubBlocksBlockHeader {
    pub key: Crc,
    pub offset: u32,
    pub size: u32,
}

#[derive(Default, Debug, Clone, Serialize, Deserialize)]
#[serde(transparent)]
pub struct SubBlocks {
    pub blocks: IndexMap<Crc, SubBlock>,
}

impl <'py> IntoPyObject<'py> for SubBlocks {
    type Target = <IndexMap<Crc, SubBlock> as IntoPyObject<'py>>::Target;
    type Output = <IndexMap<Crc, SubBlock> as IntoPyObject<'py>>::Output;
    type Error = <IndexMap<Crc, SubBlock> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.blocks.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for SubBlocks {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { blocks: IndexMap::extract_bound(ob)? })
    }
}


#[pyclass]
#[derive(Debug, Clone)]
pub struct WrappedProgress {
    pub inner: Option<ProgressBar>
}

impl From<Option<ProgressBar>> for WrappedProgress {
    fn from(value: Option<ProgressBar>) -> Self {
        Self { inner: value.clone() }
    }
}

impl AsData<'_, '_> for SubBlocks {
    type InArgs = WrappedProgress;
    type OutArgs = WrappedProgress;
    fn from_bytes<V: Version>(data: &[u8], prog: Self::InArgs) -> Result<Self> {
        let header: SubBlocksHeader = from_bytes!(V, &data[..])?;
        prog.inner.as_ref().map(|x| x.set_length(header.block_num as u64));
        let block_headers: Vec<SubBlocksBlockHeader> = from_bytes!(V, &data[V::size::<SubBlocksHeader>()..], header.block_num as usize)?;
        let blocks = block_headers.into_iter().map(|info| {
            prog.inner.as_ref().map(|x| { x.inc(1); x.set_message(info.key.to_string()) });
            let block = from_bytes!(V, SubBlock, &data[info.offset as usize..], info.key.clone(), info.size as usize)?;
            Ok((info.key, block))
        }).collect::<Result<_>>()?;
        prog.inner.as_ref().map(|x| x.finish());
        Ok(Self { blocks })
    }

    fn dump_bytes<V: Version>(&self, prog: Self::OutArgs) -> Vec<u8> {
        prog.inner.as_ref().map(|x| x.set_length(self.blocks.len() as u64));
        let header = SubBlocksHeader { block_num: self.blocks.len() as u32, ..Default::default() };
        let mut block_headers = Vec::with_capacity(self.blocks.len());
        let mut offset = V::size::<SubBlocksHeader>() + V::size::<SubBlocksBlockHeader>() * self.blocks.len();
        let mut data = vec![];
        let off = (offset + 15) & 0xfffffff0;
        data.extend(vec![0u8; off - offset]);
        offset = off;
        for (key, block) in &self.blocks { 
            prog.inner.as_ref().map(|x| { x.inc(1); x.set_message(key.to_string()) });
            let block_data: Vec<u8> = dump_bytes!(V, block);
            block_headers.push(SubBlocksBlockHeader {
                key: key.clone(),
                offset: offset as u32,
                size: block_data.len() as u32,
            });
            offset += block_data.len();
            data.extend(block_data);
            let off = (offset + 16) & 0xfffffff0;
            data.extend(vec![0u8; off - offset]);
            offset = off;
        }
        prog.inner.as_ref().map(|x| x.finish());
        dump_bytes!(V, header).into_iter()
            .chain(dump_bytes!(V, block_headers))
            .chain(data)
            .collect()
    }

    fn size<V: Version>(&self) -> usize {
        let mut s = V::size::<SubBlocksHeader>() + V::size::<SubBlocksBlockHeader>() * self.blocks.len();
        for block in self.blocks.values() {
            s = (s + 16) & 0xFFFFFFF0;
            s += block.size::<V>();
        }
        s = (s + 16) & 0xFFFFFFF0;
        return s
    }
}

impl SubBlocks {
    pub fn to_file(&self, writer: Writer, keys: &StringKeys, prog: Option<&ProgressBar>) -> Result<()> {
        prog.map(|x| x.set_length(self.blocks.len() as u64));
        writer.join("index.json").write(&to_vec_pretty(&self.blocks.keys().cloned().collect::<Vec<_>>())?)?;
        for (key, block) in &self.blocks {
            prog.map(|x| { x.inc(1); x.set_message(key.to_string()) });
            block.to_file(writer.join(key.str().unwrap()), keys)?
        }
        prog.map(|x| x.finish());
        Ok(())
    }

    pub fn from_file(reader: Reader, prog: Option<&ProgressBar>) -> Result<Self> {
        let keys = from_slice::<Vec<Crc>>(&reader.join("index.json").read()?).context(format!("{}/index.json", reader.path().display()))?;
        prog.map(|x| x.set_length(keys.len() as u64));
        let blocks = keys.into_iter().map(|key| {
            prog.as_ref().map(|x| { x.inc(1); x.set_message(key.to_string())});
            let block = SubBlock::from_file(reader.join(key.str().unwrap()), &key)?;
            Ok((key, block))
        }).collect::<Result<_>>()?;
        prog.map(|x| x.finish());
        Ok(Self { blocks })
    }
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct StringKeysHeader {
    pub num_a: u16,
    pub num_b: u16,
    pub z2: u32,
    pub z3: u32,
    pub z4: u32,
    pub z5: u32,
}

#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize)]
pub struct StringKeysVal {
    pub key: Crc,
    pub offset: u32,
}

#[derive(Default, Debug, Clone, Serialize, Deserialize)]
#[serde(transparent)]
pub struct StringKeys {
    pub vals: Vec<Crc>,
}

impl <'py> IntoPyObject<'py> for StringKeys {
    type Target = <Vec<String> as IntoPyObject<'py>>::Target;
    type Output = <Vec<String> as IntoPyObject<'py>>::Output;
    type Error = <Vec<String> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.vals.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for StringKeys {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { vals: Vec::extract_bound(ob)? })
    }
}

impl AsData<'_, '_> for StringKeys {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let mut offset: usize = 0;
        let header: StringKeysHeader = from_bytes!(V, &data[offset..])?;
        assert!(header.num_a == header.num_b, "Seems to be true");
        offset += V::size::<StringKeysHeader>();
        let vals: Vec<StringKeysVal> = from_bytes!(V, &data[offset..], header.num_a as usize)?;
        let vals = vals.into_iter().map(|x| x.key).collect(); 
        Ok(Self { vals })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let header = StringKeysHeader {
            num_a: self.vals.len() as u16,
            num_b: self.vals.len() as u16,
            ..Default::default()
        };
        let mut off = V::size::<StringKeysHeader>() + self.vals.len() * V::size::<StringKeysVal>();
        let vals = self.vals.iter().map(|key| {
            let val = StringKeysVal { key: key.clone(), offset: off as u32};
            off += 4;
            val
        }).collect::<Vec<_>>();
        
        dump_bytes!(V, header).into_iter().chain(dump_bytes!(V, vals).into_iter()).chain(std::iter::repeat_n(0u8, self.vals.len() * V::size::<u32>())).collect()
    }

    fn size<V: Version>(&self) -> usize {
        V::size::<StringKeysHeader>() + self.vals.size::<V>() * 3
    }
}

impl StringKeys {
    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.with_extension("json").write(&to_vec_pretty(&json!(self))?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        let name = reader.path().display().to_string();
        Ok(from_slice::<Self>(&reader.with_extension("json").read()?).context(format!("{}", name))?)
    }
}

#[derive(Debug, Default, Clone, Serialize, Deserialize)]
pub struct LangStrings {
    pub strings: Vec<String>,
}

impl <'py> IntoPyObject<'py> for LangStrings {
    type Target = <Vec<String> as IntoPyObject<'py>>::Target;
    type Output = <Vec<String> as IntoPyObject<'py>>::Output;
    type Error = <Vec<String> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.strings.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for LangStrings {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { strings: Vec::extract_bound(ob)? })
    }
}

impl AsData<'_, '_> for LangStrings {
    type InArgs = usize;
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], size: Self::InArgs) -> Result<Self> {
        let mut val = Self::default();
        let mut offset = 0;
        while offset < size {
            let start = offset;
            while data[offset] != 0 || data[offset+1] != 0 {
                offset += 2;
            }
            let string: Vec<u16> = from_bytes!(V, &data[start..offset], (offset-start)/2)?;
            val.strings.push(String::from_utf16(string.as_slice()).unwrap());
            offset += 2;
        }
        Ok(val)
    }
    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let vals = self.strings.iter().flat_map(|x| x.encode_utf16().chain([0u16])).collect::<Vec<_>>();
        dump_bytes!(V, vals)
    }
    fn size<V: Version>(&self) -> usize {
        self.strings.iter().map(|x| x.encode_utf16().map(|_| 2).sum::<usize>() + 2).sum::<usize>()
    }

}

impl LangStrings {
    pub const KEY_POLISH: u32 = hash_string("Polish".as_bytes(), None);
    pub const KEY_GERMAN: u32 = hash_string("German".as_bytes(), None);
    pub const KEY_FRENCH: u32 = hash_string("French".as_bytes(), None);
    pub const KEY_SPANISH: u32 = hash_string("Spanish".as_bytes(), None);
    pub const KEY_RUSSIAN: u32 = hash_string("Russian".as_bytes(), None);
    pub const KEY_SWEDISH: u32 = hash_string("Swedish".as_bytes(), None);
    pub const KEY_ENGLISH: u32 = hash_string("English".as_bytes(), None);
    pub const KEY_ITALIAN: u32 = hash_string("Italian".as_bytes(), None);
    pub const KEY_NORWEGIAN: u32 = hash_string("Norwegian".as_bytes(), None);

    pub fn to_file(&self, writer: Writer, keys: &StringKeys) -> Result<()> {
        let vals = zip(&keys.vals, &self.strings).map(|(key, string)| (key.clone(), string.clone())).collect::<IndexMap<_,_>>();
        writer.with_extension("json").write(&to_vec_pretty(&vals)?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        let name = reader.path().display().to_string();
        let vals = from_slice::<IndexMap<Crc, String>>(&reader.with_extension("json").read()?).context(format!("{}.json", name))?;
        let strings: Vec<_> = vals.into_iter().map(|(_, val)| val).collect();
        Ok(Self { strings })
    }
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct SSAVal {
    pub t_start: f32,
    pub t_end: f32,
    pub unk_2: u32,
    pub unk_3: u32,
    pub off: u32,
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct SSA {
    pub vals: Vec<SSAVal>,
    pub strings: Vec<String>,
}

impl AsData<'_, '_> for SSA {
    type InArgs = usize;
    type OutArgs = NoArgs;
    fn from_bytes<O: Version>(data: &[u8], size: Self::InArgs) -> Result<Self> {
        let n = from_bytes!(O, u32, &data[..])? as usize;
        let vals: Vec<SSAVal> = from_bytes!(O, &data[4..], n as usize)?;
        let offs = vals.iter().map(|x| x.off as usize).chain([size]).collect::<Vec<_>>();
        let strings = (0..n).map(|i| {
            let s: Vec<u16> = from_bytes!(O, &data[offs[i]..], (offs[i+1] - offs[i])/2)?;
            Ok(String::from_utf16(s.as_slice()).unwrap())
        }).collect::<Result<Vec<_>>>()?;
        Ok(Self { vals, strings })
    }

    fn dump_bytes<O: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let mut data = dump_bytes!(O, (self.vals.len() as u32));
        let mut string_data = vec![];
        let mut vals: Vec<SSAVal> = self.vals.clone();
        let off = 4 + (O::size::<SSAVal>() * self.vals.len()) as u32;
        for (string, val) in zip(&self.strings, &mut vals) {
            val.off = off + string_data.len() as u32;
            string_data.extend(dump_bytes!(O, string.encode_utf16().collect::<Vec<_>>()));
        }
        data.extend(dump_bytes!(O, vals));
        data.extend(string_data);
        data 
    }

    fn size<V: Version>(&self) -> usize {
        self.strings.iter().map(|x| x.encode_utf16().map(|_| 2).sum::<usize>()).sum::<usize>() + 4 + (PC::size::<SSAVal>() * self.vals.len())
    }
}

impl SSA {
    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.with_extension("json").write(&to_vec_pretty(self)?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        from_slice(&reader.with_extension("json").read()?).with_context(|| anyhow!("{}.json", reader.path().display()))
    }
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Default, Debug, Clone, Serialize, Deserialize, PyMethods)]
pub struct Lua {
    pub name: String,
    pub data: Vec<u8>,
}

impl AsData<'_, '_> for Lua {
    type InArgs = (usize, String);
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], (size, name): Self::InArgs) -> Result<Self> {
        let data = if size >= 4 && data[..4] == [27, 76, 117, 97] {
            if *DECOMP_LUA.lock().unwrap() {
                lua::decomp(&data[..size], UNLUAC.lock().unwrap().clone()).context(name.clone())?.as_bytes().to_vec()
            } else if size >= 11 && data[5..11] != [0, 1, 4, 4, 4, 4] {
                lua::convert(&data[..size], &lua::PC_FORMAT).context(name.clone())?
            } else {
                (&data[..size]).to_vec()
            }
        } else {
            if *RECOMP_LUA.lock().unwrap() {
                lua::compile(&data[..size], &name).context(name.clone()).with_context(|| String::from_utf8(data.to_vec()).unwrap_or("Non utf8 shader".to_string()))?
            } else {
                (&data[..size]).to_vec()
            }
        };
        Ok(Self { data, name })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        self.data.clone()
    }

    fn size<V: Version>(&self) -> usize {
        self.data.len()
    }
}

impl Lua {
    pub fn conv(&self, fmt: &lunify::Format) -> Result<Vec<u8>> {
        let val = &self.data;
        Ok(if (val.len() > 3) && (val[0] == 0x1bu8) && (val[1] == 76) && (val[2] == 117) && (val[3] == 97) {
            lua::convert(&val, fmt)?
        } else {
            val.clone()
        })
    }

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.write(&self.data)
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        let name: String = reader.name().to_string();
        let name_path = reader.path().display();
        let data = reader.read()?;
        let size = data.len();
        let data = if size >= 4 && data[..4] == [27, 76, 117, 97] {
            if *DECOMP_LUA.lock().unwrap() {
                lua::decomp(&data, UNLUAC.lock().unwrap().clone()).context(format!("{}", name_path))?.as_bytes().to_vec()
            } else if size >= 11 && data[5..11] != [0, 1, 4, 4, 4, 4] {
                lua::convert(&data, &lua::PC_FORMAT).context(format!("{}", name_path))?
            } else {
                data
            }
        } else {
            if *RECOMP_LUA.lock().unwrap() {
                lua::compile(&data, &name).context(format!("{}", name_path)).with_context(|| String::from_utf8(data.to_vec()).unwrap_or("Non utf8 shader".to_string()))?
            } else {
                data.to_vec()
            }
        };
        
        Ok(Self { name, data })
    }
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct GameObjsHeader {
    pub const_: u32,
    pub types_num: u32,
    pub types_offset: u32,
    pub obj_num: u32,
    pub obj_offset: u32,
    pub z5: u32,
    pub z6: u32,
    pub z7: u32,
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct GameObjsTypeHeader {
    pub key: Crc,
    pub size: u32,
    pub fields: u32,
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct GameObjsTypeField {
    #[serde(rename = "name")]
    pub key: Crc,
    #[serde(rename = "type")]
    pub kind: Crc,
    pub offset: u32,
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct GameObjsObjHeader {
    pub layer: u32,
    pub key: Crc,
    pub size: u16,
    pub z3: u16,
    pub z4: u32,
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct GameObj {
    pub layer: u32,
    pub key: Crc,
    pub fields: IndexMap<Crc, BaseTypes>
}

impl AsData<'_, '_> for GameObj {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(_data: &[u8], _args: Self::InArgs) -> Result<Self> {
        panic!("not implemented")
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        panic!("not implemented")
    }

    fn size<V: Version>(&self) -> usize {
        panic!("not implemented")
    }
}

#[basicpymethods]
#[pyclass(module="types", get_all, set_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct GameObjs {
    pub gamemodemask: i32,
    pub types: IndexMap<Crc, Vec<GameObjsTypeField>>,
    pub objs: IndexMap<u32, GameObj>,
}

impl AsData<'_, '_> for GameObjs {
    type InArgs = i32;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], gamemodemask: Self::InArgs) -> Result<Self> {
        let header: GameObjsHeader = from_bytes!(V, &data[..])?;
        if header.const_ != 1296123652 {
            log::error!("Invalid gameobj block");
        }
        let mut types = IndexMap::with_capacity(header.types_num as usize);
        let mut type_orders = HashMap::with_capacity(header.types_num as usize);
        let mut objs = IndexMap::with_capacity(header.obj_num as usize);
        let mut offset = header.types_offset as usize;
        for _ in 0..header.types_num {
            let info: GameObjsTypeHeader = from_bytes!(V, &data[offset..])?;
            offset += info.size::<V>();
            let fields: Vec<GameObjsTypeField> = from_bytes!(V, &data[offset..], info.size as usize)?;
            offset += fields.size::<V>();
            let mut order: Vec<_> = (0..fields.len()).collect();
            order.sort_by_key(|x| fields[*x].offset);
            type_orders.insert(info.key.clone(), order);
            types.insert(info.key, fields);
        }
        offset = header.obj_offset as usize;
        for _ in 0..header.obj_num {
            //println!("offset {:?}", offset);
            let info: GameObjsObjHeader = from_bytes!(V, &data[offset..])?;
            offset += info.size::<V>();
            let ts = types.get(&info.key).unwrap();
            let order = type_orders.get(&info.key).unwrap();
            let mut fields = IndexMap::with_capacity(ts.len());
            for t in order.iter().map(|i| &ts[*i]) {
                let val = BaseTypes::from_data::<V>(&data[offset + t.offset as usize..], t.kind.key())?;
                //println!("val {:?}", val);
                fields.insert(t.key.clone(), val);
            }
            offset += info.size as usize;
            let guid = if let Some(BaseTypes::GUID(val)) = fields.get(&Crc::Key(3482846511)) {
                Some(*val)
            } else { None }.unwrap();
            objs.insert(guid, GameObj { layer: info.layer, key: info.key, fields });
        }
        Ok(Self { gamemodemask, types, objs })
    }
    
    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let mut type_data = vec![];
        for (key, fields) in &self.types {
            type_data.extend(dump_bytes!(V, GameObjsTypeHeader { key: key.clone(), size: fields.len() as u32, fields: 0 }));
            type_data.extend(dump_bytes!(V, fields));
        }
        type_data.extend(vec![0u8; ((type_data.len() + 15) & 0xFFFFFFF0) - type_data.len()]);
        let mut data = dump_bytes!(V, GameObjsHeader {
            const_: 1296123652,
            types_num: self.types.len() as u32,
            types_offset: V::size::<GameObjsHeader>() as u32,
            obj_num: self.objs.len() as u32,
            obj_offset: (type_data.len() + V::size::<GameObjsHeader>()) as u32,
            z5: 0,
            z6: 0,
            z7: 0
        });
        data.extend(type_data);
        
        for obj in self.objs.values() {
            let ts = self.types.get(&obj.key).unwrap();
            let obj_size = ts.iter().map(|t| {
                let f = obj.fields.get(&t.key).unwrap();
                t.offset as usize + f.size::<V>()
            }).fold(0, usize::max);
            let mut obj_vals = vec![0u8; (obj_size + 15) & 0xFFFFFFF0];
            for t in ts {
                let val = obj.fields.get(&t.key).unwrap();
                val.dump::<V>(&mut obj_vals, t.offset as usize).unwrap();
                if t.kind.key() == BaseTypes::INTLISTS_KEY {
                    // println!("{}", vals.len());
                    obj_vals.extend(vec![0u8; ((obj_vals.len() + 15) & 0xFFFFFFF0) - obj_vals.len()]);
                }
            }
            obj_vals.extend(vec![0u8; ((obj_vals.len() + 15) & 0xFFFFFFF0) - obj_vals.len()]);
            data.extend(dump_bytes!(V, GameObjsObjHeader {
                layer: obj.layer,
                key: obj.key.clone(),
                size: obj_vals.len() as u16,
                z3: 0,
                z4: 0
            }));
            data.extend(obj_vals);
        }
        data
    }

    fn size<V: Version>(&self) -> usize {
        let size = ((self.types.len() * V::size::<GameObjsTypeHeader>()) + (self.types.values().map(|x| x.len() as usize).sum::<usize>() * V::size::<GameObjsTypeField>()) + V::size::<GameObjsHeader>() + 15) & 0xFFFFFFF0;
        size + self.obj_headers::<V>().iter().map(|x| x.size as usize).sum::<usize>() + self.objs.len() * V::size::<GameObjsObjHeader>() 
    }
}

impl GameObjs {
    pub const KEY: u32 = hash_string("Level".as_bytes(), None);

    pub fn obj_headers<V: Version>(&self) -> Vec<GameObjsObjHeader> {
        let mut headers = Vec::with_capacity(self.objs.len());
        for obj in self.objs.values() {
            let ts = self.types.get(&obj.key).unwrap();
            let mut off = ts.iter().map(|t| {
                let f = obj.fields.get(&t.key).unwrap();
                t.offset as usize + f.size::<V>()
            }).fold(0, usize::max);
            for t in ts {
                let val = obj.fields.get(&t.key).unwrap();
                off += val.off_size::<V>();
                if t.key.key() == BaseTypes::INTLISTS_KEY {
                    off = (off + 15) & 0xFFFFFFF0;
                }
            }
            let size = (off + 15) as u16 & 0xFFF0;
            headers.push(GameObjsObjHeader {
                layer: obj.layer,
                key: obj.key.clone(),
                size,
                z3: 0,
                z4: 0
            });
        }
        headers
    }

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        if *ALT_OBJS.lock().unwrap() {
            self.to_file_new(writer)
        } else {
            self.to_file_old(writer)
        }
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        if *ALT_OBJS.lock().unwrap() {
            Self::from_file_new(reader)
        } else {
            Self::from_file_old(reader)
        }
    }
    
    pub fn to_file_old(&self, writer: Writer) -> Result<()> {
        let val = json!({
            "gamemodemask": self.gamemodemask,
            "objs": self.objs.values().map(|GameObj { layer, key, fields }| {
                json!({
                    "type": key.to_string(),
                    "layer": layer,
                    "fields": fields.iter().map(|(key, val)| (key.to_string(), val.to_json())).collect::<Map<_,_>>()
                })
            }).collect::<Vec<_>>(),
            "types": self.types.iter().map(|(key, fs)| {
                json!({
                    "name": key.to_string(),
                    "fields": fs.iter().map(|f| {
                        json!({
                            "name": f.key.to_string(),
                            "type": f.kind.to_string(),
                            "offset": f.offset
                        })
                    }).collect::<Vec<_>>()
                })
            }).collect::<Vec<_>>(),
        });
        writer.with_extension("json").write(&to_vec_pretty(&val)?)?;
        Ok(())
    }

    pub fn from_file_old(reader: Reader) -> Result<Self> {
        let name = reader.path().display();
        let mut val = from_slice::<Value>(&reader.with_extension("json").read()?).with_context(|| format!("{}", name))?;
        let ts = val.get("types").ok_or(anyhow!("{} missing types", name))?.as_array().ok_or(anyhow!("{} types is not array", name))?;
        let mut types = IndexMap::with_capacity(ts.len());
        for (i, t) in ts.into_iter().enumerate() {
            let ty_name = t.get("name").ok_or(anyhow!("{} type {} missing name", name, i))?.as_str().ok_or(anyhow!("{} type {} name is not string", name, i))?;
            types.insert(
                Crc::from_string(ty_name),
                t.get("fields").ok_or(anyhow!("{} type {} missing fields", name, ty_name))?.as_array().ok_or(anyhow!("{} type {} fields is not array", name, ty_name))?.iter().enumerate().map(|(i, v)| {
                    let f_name = v.get("name").ok_or(anyhow!("{} type {} field {} missing name", name, ty_name, i))?.as_str().ok_or(anyhow!("{} type {} field {} name is not string", name, ty_name, i))?;
                    Ok(GameObjsTypeField {
                        key: Crc::from_string(f_name),
                        kind: Crc::from_string(v.get("type").ok_or(anyhow!("{} type {} field {} missing kind", name, ty_name, f_name))?.as_str().ok_or(anyhow!("{} type {} field {} kind is not string", name, ty_name, f_name))?),
                        offset: v.get("offset").ok_or(anyhow!("{} type {} field {} missing offset", name, ty_name, f_name))?.as_u64().and_then(|x| u32::try_from(x).ok()).ok_or(anyhow!("{} type {} field {} offset not u32", name, ty_name, f_name))?,
                    })
                }).collect::<Result<Vec<_>>>()?,
            );
        }
        let types_order: HashMap<_, _> = types.iter().map(|(key, fields)| {
            let mut order: Vec<_> = (0..fields.len()).collect();
            order.sort_by_key(|x| fields[*x].offset);
            let mut order = fields.clone();
            order.sort_by_key(|x| x.offset);
            //let order: Vec<_> = order.into_iter().map(|x| x.kind.key()).collect();
            (key.clone(), order)
        }).collect();

        let os = val.get_mut("objs").ok_or(anyhow!("{} missing objs", name))?.as_array_mut().ok_or(anyhow!("{} obs is not array", name))?;
        let mut objs = IndexMap::with_capacity(os.len());

        for (i, o) in os.into_iter().enumerate() {
            let o_name = o.get("type").ok_or(anyhow!("{} obj index {} missing type", name, i))?.as_str().ok_or(anyhow!("{} obj index {} type is not string", name, i))?;
            let key = Crc::from_string(o_name);
            let order = types_order.get(&key).ok_or(anyhow!("{} type {} not in types", name, o_name))?;
            let mut o_: HashMap<_,_> = o.get_mut("fields").ok_or(anyhow!("{} obj index {} missing fields", name, i))?.as_object_mut().ok_or(anyhow!("{} obj index {} fields not map", name, i))?.into_iter().map(|(k,v)| (Crc::from_string(k), v.take())).collect();
            let fields: IndexMap<Crc, BaseTypes> = order.iter().map(|t| Ok((
                    t.key.clone(), 
                    BaseTypes::from_json(o_.remove(&t.key).ok_or_else(|| anyhow!("{} obj index {} missing field {}", name, i, t.key.to_string()))?, t.kind.key()).with_context(|| format!("{} obj index {} field {}", name, i, t.key.to_string()))?
            ))).collect::<Result<_>>()?;
            let guid = if let BaseTypes::GUID(val) = fields.get(&Crc::Key(3482846511)).ok_or(anyhow!("{} obj index {} missing guid field", name, i))? {
                *val
            } else {
                panic!("Parsing BaseType returned incorrect type")
            };
            objs.insert(guid, GameObj { 
                layer: o.get("layer").ok_or(anyhow!("{} obj {} missing layer", name, guid))?.as_u64().and_then(|x| u32::try_from(x).ok()).ok_or(anyhow!("{} obj {} layer is not u32", name, guid))?,
                key,
                fields
            });
        }
        let gamemodemask = val["gamemodemask"].as_i64().ok_or(anyhow!("{}", name))? as i32;
        Ok(Self { gamemodemask, types, objs })
    }

    pub fn to_file_new(&self, writer: Writer) -> Result<()> {
        let val = json!({
            "gamemodemask": self.gamemodemask,
            "objs": self.objs.iter().map(|(guid, GameObj { layer, key, fields })| (
                guid.to_string(),
                json!({
                    "type": key.to_string(),
                    "layer": layer,
                    "fields": fields.iter().filter(|(key, _)| key.key() != 3482846511).map(|(key, val)| (key.to_string(), val.to_json())).collect::<Map<_,_>>()
                })
            )).collect::<Map<_, _>>(),
            "types": self.types
        });
        writer.with_extension("json").write(&to_vec_pretty(&val)?)?;
        Ok(())
    }

    pub fn from_file_new(reader: Reader) -> Result<Self> {
        let name = reader.path().display();
        let mut val = from_slice::<Value>(&reader.with_extension("json").read()?).with_context(|| format!("{}", name))?;

        let types = serde_json::from_value::<IndexMap<Crc, Vec<GameObjsTypeField>>>(val.get("types").ok_or(anyhow!("{} missing types", name))?.clone()).with_context(|| format!("{} types", name))?;
        
        let types_order: HashMap<_, _> = types.iter().map(|(key, fields)| {
            let mut order: Vec<_> = (0..fields.len()).collect();
            order.sort_by_key(|x| fields[*x].offset);
            let mut order = fields.clone();
            order.sort_by_key(|x| x.offset);
            //let order: Vec<_> = order.into_iter().map(|x| x.kind.key()).collect();
            (key.clone(), order)
        }).collect();

        let os = val.get_mut("objs").ok_or(anyhow!("{} missing objs", name))?.as_object_mut().ok_or(anyhow!("{} obs is not map", name))?;
        let mut objs = IndexMap::with_capacity(os.len());

        for (guid, o) in os.into_iter() {
            let guid = guid.parse::<u32>()?;
            let o_name = o.get("type").ok_or(anyhow!("{} obj {} missing type", name, guid))?.as_str().ok_or(anyhow!("{} obj {} type is not string", name, guid))?;
            let key = Crc::from_string(o_name);
            let order = types_order.get(&key).ok_or(anyhow!("{} type {} not in types", name, o_name))?;
            let mut o_: HashMap<_,_> = o.get_mut("fields").ok_or(anyhow!("{} obj {} missing fields", name, guid))?.as_object_mut().ok_or(anyhow!("{} obj {} fields not map", name, guid))?.into_iter().map(|(k,v)| (Crc::from_string(k), v.take())).collect();
            let fields: IndexMap<Crc, BaseTypes> = order.iter().map(|t| Ok((
                    t.key.clone(),
                    if t.key.key() == 3482846511 {
                        BaseTypes::GUID(guid)
                    } else {
                        BaseTypes::from_json(o_.remove(&t.key).ok_or_else(|| anyhow!("{} obj {} missing field {}", name, guid, t.key.to_string()))?, t.kind.key()).with_context(|| format!("{} obj {} field {}", name, guid, t.key.to_string()))?
                    }
            ))).collect::<Result<_>>()?;
            let guid = if let BaseTypes::GUID(val) = fields.get(&Crc::Key(3482846511)).ok_or(anyhow!("{} obj index {} missing guid field", name, guid))? {
                *val
            } else {
                panic!("Parsing BaseType returned incorrect type")
            };
            objs.insert(guid, GameObj { 
                layer: o.get("layer").ok_or(anyhow!("{} obj {} missing layer", name, guid))?.as_u64().and_then(|x| u32::try_from(x).ok()).ok_or(anyhow!("{} obj {} layer is not u32", name, guid))?,
                key,
                fields
            });
        }
        let gamemodemask = val["gamemodemask"].as_i64().ok_or(anyhow!("{}", name))? as i32;
        Ok(Self { gamemodemask, types, objs })
    }
}


#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct SprayInstance {
    pub key: Crc,
    pub tex1: Crc,
    pub tex2: Crc,
    pub unk_3: u32,
    pub width: u32,
    pub height: u32,
    pub unk_6: f32,
    pub unk_7: f32,
    pub size_w: u32,
    pub size_h: u32,
    pub scale_w: f32,
    pub scale_h: f32,
    pub delay: u32,
    pub stride_x: f32,
    pub stride_y: f32,
    pub unk_15: u32,
    pub unk_16: u32,
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct SprayVal {
    pub position: Vector3,
    pub scale: f32,
    pub instance: u16,
    pub rotation: u16,
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct Spray {
    instances: Vec<SprayInstance>,
    vals: Vec<SprayVal>,
}

impl AsData<'_, '_> for Spray {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let mut offset = 0;
        let n = from_bytes!(V, u32, &data[offset..])? as usize;
        offset += V::size::<u32>();
        let instances: Vec<SprayInstance> = from_bytes!(V, &data[offset..], n)?;
        offset += instances.size::<V>();
        let n = from_bytes!(V, u32, &data[offset..])? as usize;
        offset += V::size::<u32>();
        let vals: Vec<SprayVal> = from_bytes!(V, &data[offset..], n)?;
        Ok(Self { instances, vals })
    }
    fn dump_bytes<O: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(O, (self.instances.len() as u32)).into_iter()
            .chain(dump_bytes!(O, self.instances))
            .chain(dump_bytes!(O, (self.vals.len() as u32)))
            .chain(dump_bytes!(O, self.vals))
            .collect()
    }
    fn size<V: Version>(&self) -> usize {
        self.instances.size::<V>() + self.vals.size::<V>() + (V::size::<u32>() * 2)
    }
}

impl Spray {
    pub const KEY: u32 = hash_string("Spray".as_bytes(), None);

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.with_extension("json").write(&to_vec_pretty(self)?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        from_slice(&reader.with_extension("json").read()?).with_context(|| format!("{}.json", reader.path().display()))
    }
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct CrowdHeader {
    pub key: Crc,
    pub key_main: Crc,
    pub key_right: Crc,
    pub key_left: Crc,
    pub unk_4: f32,
    pub animation_num: u32,
    pub instance_num: u32,
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct CrowdVal {
    pub position: Vector3,
    pub rotation: f32,
    pub lod: f32,
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
pub struct CrowdItem {
    pub header: CrowdHeader,
    pub animations: Vec<Crc>,
    pub instances: Vec<CrowdVal>,
}

impl AsData<'_, '_> for CrowdItem {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;
    fn from_bytes<V: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let mut offset = 0;
        let header: CrowdHeader = from_bytes!(V, &data[offset..])?;
        offset += header.size::<V>();
        let animations: Vec<Crc> = from_bytes!(V, &data[offset..], header.animation_num as usize)?;
        offset += animations.size::<V>();
        let instances = from_bytes!(V, &data[offset..], header.instance_num as usize)?;
        Ok(Self { header, animations, instances })
    }
    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(V, self.header).into_iter()
            .chain(dump_bytes!(V, self.animations))
            .chain(dump_bytes!(V, self.instances))
            .collect()
    }
    fn size<V: Version>(&self) -> usize {
        self.header.size::<V>() + self.animations.size::<V>() + self.instances.size::<V>()
    }
}

#[derive(Debug, Default, Clone, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Crowd {
    pub vals: Vec<CrowdItem>
}

impl <'py> IntoPyObject<'py> for Crowd {
    type Target = <Vec<CrowdItem> as IntoPyObject<'py>>::Target;
    type Output = <Vec<CrowdItem> as IntoPyObject<'py>>::Output;
    type Error = <Vec<CrowdItem> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.vals.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Crowd {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { vals: Vec::extract_bound(ob)? })
    }
}

impl AsData<'_, '_> for Crowd {
    type InArgs = NoArgs;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], _args: Self::InArgs) -> Result<Self> {
        let val: u32 = from_bytes!(V, data)?;
        if val != 0x65 { return Err(anyhow!("Invalid Block Data for Crowd Block")); }
        let n: u32 = from_bytes!(V, &data[V::size::<u32>()..])?;
        let offs: Vec<u32> = from_bytes!(V, &data[V::size::<u32>() * 2..], n as usize)?;
        let vals = offs.into_iter()
            .map(|off| from_bytes!(V, CrowdItem, &data[off as usize..]))
            .collect::<Result<Vec<_>>>()?;
        Ok(Self { vals })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        let n = self.vals.len() as u32;

        let mut vals = vec![];
        let mut offs = vec![];
        let off = (n + 2) * (V::size::<u32>() as u32);
        for CrowdItem { header, animations, instances } in &self.vals {
            offs.push(off + (vals.len() as u32));
            vals.extend(dump_bytes!(V, header));
            vals.extend(dump_bytes!(V, animations));
            vals.extend(dump_bytes!(V, instances));
        }

        let mut data = Vec::with_capacity(off as usize + vals.len());
        data.extend(dump_bytes!(V, 0x65u32));
        data.extend(dump_bytes!(V, n));
        data.extend(dump_bytes!(V, offs));
        data.extend(vals);
        data
    }

    fn size<V: Version>(&self) -> usize {
        let n = (2 + self.vals.len()) * V::size::<u32>();
        let m: usize = self.vals.iter().map(|CrowdItem { animations, instances, .. }| {
            V::size::<CrowdHeader>() + animations.size::<V>() + instances.size::<V>()
        }).sum();
        n + m
    }
}

impl Crowd {
    pub const KEY: u32 = hash_string("3dCrowd".as_bytes(), None);

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.with_extension("json").write(&to_vec_pretty(self)?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        from_slice(&reader.with_extension("json").read()?).with_context(|| format!("{}.json", reader.path().display()))
    }
}

#[basicpymethods]
#[pyclass(module="types", set_all, get_all)]
#[derive(Debug, Default, Clone, OrderedData, Serialize, Deserialize, PyMethods)]
pub struct AtlasUVVal {
    pub key: Crc,
    pub vals: Vector4
}

#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
#[serde(transparent)]
pub struct AtlasUV {
    pub vals: Vec<AtlasUVVal>,
}

impl <'py> IntoPyObject<'py> for AtlasUV {
    type Target = <Vec<AtlasUVVal> as IntoPyObject<'py>>::Target;
    type Output = <Vec<AtlasUVVal> as IntoPyObject<'py>>::Output;
    type Error = <Vec<AtlasUVVal> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.vals.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for AtlasUV {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { vals: Vec::extract_bound(ob)? })
    }
}

impl AsData<'_, '_> for AtlasUV {
    type InArgs = usize;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], size: Self::InArgs) -> Result<Self> {
        if size%V::size::<AtlasUVVal>() != 0 {
            return Err(anyhow!("Invalid UV Atlas size {}", size))
        }
        let num = size / V::size::<AtlasUVVal>();
        let vals = from_bytes!(V, &data[..], num)?;
        Ok(Self { vals })
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        dump_bytes!(V, self.vals)
    }

    fn size<V: Version>(&self) -> usize {
        self.vals.size::<V>()
    }
}

impl AtlasUV {
    pub const KEY1: u32 = hash_string("atlas_1.uv".as_bytes(), None);
    pub const KEY2: u32 = hash_string("atlas_2.uv".as_bytes(), None);

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.with_extension("json").write(&to_vec_pretty(self)?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        from_slice(&reader.with_extension("json").read()?).with_context(|| format!("{}.json", reader.path().display()))
    }
}

#[basicpymethods(no_bytes)]
#[derive(Debug, Default, Clone, Serialize, Deserialize, PyMethods)]
#[pyclass(module="types", get_all, set_all)]
pub struct PFieldVal {
    pub link_guid: u32,
    pub vals: Vec<(HashSet<u32>, Vec<u8>)>,
    pub width: u32,
    pub height: u32,
}

use serde_with::serde_as;
#[serde_as]
#[basicpymethods]
#[derive(Debug, Clone, Serialize, Deserialize, PyMethods)]
#[pyclass(module="types", get_all, set_all)]
pub enum PFields {
    Vals(IndexMap<u32, PFieldVal>),
    RawData(
        #[serde_as(as = "serde_with::hex::Hex")]
        Vec<u8>
    ),
}

impl PFields {
    pub const KEY: u32 = hash_string("PFields".as_bytes(), None);
    pub fn parse(&mut self, infos: Vec<PFieldInfo>) {
        match self {
            Self::RawData(data) => {
                let mut offset_maps: HashMap<u32, HashMap<u32, usize>> = HashMap::new();
                let mut pfields = IndexMap::new();
                for info in infos {
                    if !pfields.contains_key(&info.link_guid) {
                        pfields.insert(info.link_guid, PFieldVal {
                            link_guid: info.link_guid,
                            width: info.width,
                            height: info.height,
                            ..Default::default()
                        });
                        offset_maps.insert(info.link_guid, HashMap::new());
                    }
                    let val = pfields.get_mut(&info.link_guid).unwrap();
                    let offset_map = offset_maps.get_mut(&info.link_guid).unwrap();
                    if let Some(&index) = offset_map.get(&info.offset) {
                        val.vals[index].0.insert(info.gamemode_guid);
                    } else {
                        offset_map.insert(info.offset, val.vals.len());
                        let mut gamemodes = HashSet::new();
                        gamemodes.insert(info.gamemode_guid);
                        let offset = info.offset as usize;
                        let size = (info.width * info.height) as usize;
                        let vals = data[offset..offset+size].to_vec();
                        val.vals.push((gamemodes, vals));
                    }
                }
                *self = Self::Vals(pfields);
            },
            _ => ()
        }
    }

    pub fn infos(&self) -> Vec<PFieldInfo> {
        match self {
            Self::Vals(vals) => {
                let mut infos = vec![];
                let mut offset = 0u32;
                for pfield in vals.values() {
                    for (gamemodes, _) in &pfield.vals {
                        for &gamemode_guid in gamemodes {
                            infos.push(PFieldInfo {
                                link_guid: pfield.link_guid,
                                width: pfield.width,
                                height: pfield.height,
                                gamemode_guid, offset
                            });
                        }
                        offset += pfield.width * pfield.height;
                    }
                }
                infos
            },
            _ => vec![]
        }
    }

    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.with_extension("json").write(&to_vec_pretty(self)?)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        from_slice(&reader.with_extension("json").read()?).with_context(|| format!("{}.json", reader.path().display()))
    }
}

impl <'a, 'b> AsData<'a, 'b> for PFields {
    type InArgs = usize;
    type OutArgs = NoArgs; 

    fn from_bytes<V: Version>(data: &[u8], size: Self::InArgs) -> Result<Self> {
        Ok(Self::RawData(data[..size].to_vec()))
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        match self {
            Self::RawData(data) => data.clone(),
            Self::Vals(vals) => {
                let mut data = Vec::with_capacity(self.size::<V>());
                for pfield in vals.values() {
                    for (_, vals) in &pfield.vals {
                        data.extend(dump_bytes!(V, vals));
                    }
                }
                data
            }
        }
    }

    fn size<V: Version>(&self) -> usize {
        match self {
            Self::Vals(vals) => vals.values().flat_map(|x| x.vals.iter()).map(|(_, vals)| vals.size::<V>()).sum::<usize>(),
            Self::RawData(data) => data.len()
        }
    }
}


#[derive(Debug, Default, Clone, Serialize, Deserialize)]
pub struct Data {
    pub data: Vec<u8>,
}

impl <'py> IntoPyObject<'py> for Data {
    type Target = <Vec<u8> as IntoPyObject<'py>>::Target;
    type Output = <Vec<u8> as IntoPyObject<'py>>::Output;
    type Error = <Vec<u8> as IntoPyObject<'py>>::Error;

    fn into_pyobject(self, py: Python<'py>) -> Result<Self::Output, Self::Error> {
        self.data.into_pyobject(py)
    }
}

impl <'py> FromPyObject<'py> for Data {
    fn extract_bound(ob: &Bound<'py, PyAny>) -> PyResult<Self> {
        Ok(Self { data: Vec::extract_bound(ob)? })
    }
}

impl AsData<'_, '_> for Data {
    type InArgs = usize;
    type OutArgs = NoArgs;

    fn from_bytes<V: Version>(data: &[u8], size: Self::InArgs) -> Result<Self> {
        Ok(Self { data: data[..size].to_vec()})
    }

    fn dump_bytes<V: Version>(&self, _args: Self::OutArgs) -> Vec<u8> {
        self.data.clone()
    }

    fn size<V: Version>(&self) -> usize {
        self.data.len()
    }
}

impl Data {
    pub fn to_file(&self, writer: Writer) -> Result<()> {
        writer.write(&self.data)?;
        Ok(())
    }

    pub fn from_file(reader: Reader) -> Result<Self> {
        Ok(Self { data: reader.read()? })
    }
}

