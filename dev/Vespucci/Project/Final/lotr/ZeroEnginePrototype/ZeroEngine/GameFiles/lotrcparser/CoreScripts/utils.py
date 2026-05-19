import zipfile
import string
import struct
import json
import copy
import os
from operator import attrgetter

class FileWriter:
    def __init__(self, src):
        self.files_ = []
        self.src = src
        q = [src]
        while q != []:
            f = q.pop()
            for p in os.listdir(f):
                p = f"{f}/{p}"
                if os.path.isdir(p):
                    q.append(p)
                else:
                    self.files_.append(p.split(src)[1].lstrip('/').lstrip('\\'))

    def files(self):
        return self.files_
                    
    def read(self, path):
        with open(f"{self.src}/{path}", "rb") as f:
            return f.read()

    def remove(self, *paths):
        for path in paths:
            os.remove(f"{self.src}/{path}")

    def writestr(self, path, data):
        f = 'w' if isinstance(data, str) else 'wb'
        with open(f"{self.src}/{path}", f) as f:
            f.write(data)

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        pass

class ZipWriter(zipfile.ZipFile):
    """
        Modified zipfile to allow for removing files. 
        Uses slightly modified code from https://github.com/python/cpython/blob/659eb048cc9cac73c46349eb29845bc5cd630f09/Lib/zipfile.py
    """
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def files(self):
        return [i.filename for i in self.filelist]
        
    def remove(self, *members):
        """Remove a file from the archive. The archive must be open with mode 'a'"""

        if self.mode != 'a':
            raise RuntimeError("remove() requires mode 'a'")
        if not self.fp:
            raise ValueError(
                "Attempt to write to ZIP archive that was already closed")
        if self._writing:
            raise ValueError(
                "Can't write to ZIP archive while an open writing handle exists."
            )

        zinfos = []
        for member in members:
            # Make sure we have an info object
            if isinstance(member, zipfile.ZipInfo):
                # 'member' is already an info object
                zinfo = member
            else:
                # get the info object
                zinfo = self.getinfo(member)
            zinfos.append(zinfo)

        return self._remove_member(*zinfos)

    def _remove_member(self, *members):
        # get a sorted filelist by header offset, in case the dir order
        # doesn't match the actual entry order
        fp = self.fp
        entry_offset = 0
        filelist = sorted(self.filelist, key=attrgetter('header_offset'))
        min_header_offset = min(i.header_offset for i in members)
        members = set(members)
        for i in range(len(filelist)):
            info = filelist[i]
            # find the target member
            if info.header_offset < min_header_offset:
                continue

            # get the total size of the entry
            entry_size = None
            if i == len(filelist) - 1:
                entry_size = self.start_dir - info.header_offset
            else:
                entry_size = filelist[i + 1].header_offset - info.header_offset

            # found the member, set the entry offset
            if info in members:
                entry_offset += entry_size
                continue

            # Move entry
            # read the actual entry data
            fp.seek(info.header_offset)
            entry_data = fp.read(entry_size)

            # update the header
            info.header_offset -= entry_offset

            # write the entry to the new position
            fp.seek(info.header_offset)
            fp.write(entry_data)
            fp.flush()

        # update state
        self.start_dir -= entry_offset
        for member in members:
            self.filelist.remove(member)
            del self.NameToInfo[member.filename]
        self._didModify = True

        # seek to the start of the central dir
        fp.seek(self.start_dir)

def writer(path, *args, **kwargs):
    if path.endswith(".zip"):    
        if 'compression' not in kwargs:
            kwargs['compression'] = zipfile.ZIP_DEFLATED
        return ZipWriter(path, "a", *args, **kwargs)
    else:
        return FileWriter(path, *args, **kwargs)

def reader(path, *args, **kwargs):
    if path.endswith(".zip"):
        return ZipWriter(path, "r", *args, **kwargs)
    else:
        return FileWriter(path, *args, **kwargs)
        
def get_gamemodemask(obj):
    a = obj.find(b'"gamemodemask": ') + len(b'"gamemodemask": ')
    b = obj.find(b',', a)
    return int(obj[a:b]), a, b

def get_model(files, model):
    return files.get(
        f"models/{model.casefold()}.json",
        files.get(f"models/{model.casefold()}.glb")
    )


# some utilities for getting things from dumped level file
def find_obj(vals, guid):
    for obj in vals['objs']:
        if obj['fields']['GUID'] == guid:
            return obj

def get_layer(vals, *layers):
    layers = set(layers)
    return [i for i in vals['objs'] if i['layer'] in layers]

def find_type(vals, name):
    for ty in vals['types']:
        if ty['name'] == name:
            return ty
    
# grabs an object and all sub objects from a dumped level file
# parts can be uncommented to print some stuff about 
#    meshes, effects and scripts that are needed for the objects to work propoerly (or you can try to find everything in a dumped json file
def copy_tree(vals, guid, gamemodemask=None, processed=None, infos=None):
    if processed is None:
        processed = set()
    if infos is None:
        infos = set()
    elif guid in processed:
        return []
    processed.add(guid)
    obj = find_obj(vals, guid)
    ty = find_type(vals, obj['type'])
    objs = [obj]
    if gamemodemask is not None and 'GameModeMask' in obj['fields']:
        obj['fields']['GameModeMask'] |= gamemodemask
    for t in ty['fields']:
        if t['type'] == 'GUID':
            val = obj['fields'][t['name']]
            if val != 0:
                objs.extend(copy_tree(vals, val, processed=processed, gamemodemask=gamemodemask, infos=infos))
        elif t['type'] == 'objectlist':
            for val in obj['fields'][t['name']]:
                objs.extend(copy_tree(vals, val, processed=processed, gamemodemask=gamemodemask, infos=infos))
        elif (t['type'] == 'crc' or t['type'] == 'string') and (val:=obj['fields'][t['name']]) != '':
            infos.add(val.casefold())
        elif t['type'] == 'crclist' or t['type'] == 'stringlist':
            infos.update([i.casefold() for i in obj['fields'][t['name']]])
    return objs

def scan(vals, guid, infos=None):
    if infos is None:
        infos = set()
    obj = find_obj(vals, guid)
    ty = find_type(vals, obj['type'])
    objs = [obj]
    for t in ty['fields']:
        if (t['type'] == 'crc' or t['type'] == 'string') and (val:=obj['fields'][t['name']]) != '':
            infos.add(val.casefold())
        elif t['type'] == 'crclist' or t['type'] == 'stringlist':
            infos.update([i.casefold() for i in obj['fields'][t['name']]])

def get_lua_strings(data):
    if len(data) < 4 or data[0] != 0x1b or data[1] != 76 or data[2] != 117 or data[3] != 97:
        data = data.decode()
        strings = []
        i = data.find('"')
        while i != -1:
            j = data.find('"', i+1)
            strings.append(data[i+1:j].casefold())
            i = data.find('"', j+1)
        return strings
    valid_chars = set(string.printable.encode())
    strings = []
    off = 0
    while off < len(data):
        if data[off-1] == 0 and data[off] in valid_chars:
            valid = True
            l = struct.unpack_from("I", data, off-4)[0] - 1
            # print(l, data[off:off+l])
            if l > len(data) or l <= 1:
                valid = False
            else:
                for i in range(l):
                    if data[off+i] not in valid_chars:
                        valid = False
                        break
            if valid:
                strings.append(data[off:off+l].decode().casefold())
                off += l
        off += 1
    return strings



class Glb:
    @classmethod
    def empty(Self):
        gltf = {i: [] for i in [
            'accessors', 'asset', 'buffers', 
            'bufferViews', 'extras', 'extensionsUsed', 
            'extensionsRequired', 'meshes', 'nodes', 
            'scenes', 'skins'
        ]}
        gltf['asset'] = {'version': '2.0'}
        return Self(gltf, bytearray())

    @classmethod
    def from_data(Self, data):
        (_, _, size, json_len, _) = struct.unpack_from("5I", data)
        off = 20
        gltf = json.loads(data[off:off+json_len])
        off += json_len
        (bin_len, _) = struct.unpack_from("2I", data, off)
        off += 8
        buff = bytearray(data[off:off+bin_len])
        return Self(gltf, buff)

    def __init__(self, gltf, buff):
        self.gltf = gltf
        self.buff = buff

    def splice_accessors(self, buff, gltf, *accessors):
        self.gltf['extensionsUsed'].extend(
            i for i in gltf['extensionsUsed'] if i not in self.gltf['extensionsUsed']
        )
        self.gltf['extensionsRequired'].extend(
            i for i in gltf['extensionsRequired'] if i not in self.gltf['extensionsRequired']
        )
        inds = list(set(accessors))
        accessor_map = {j: i + len(self.gltf['accessors']) for i,j in enumerate(inds)}
        accessors = [copy.copy(gltf['accessors'][i]) for i in accessor_map]
        inds = list(set([i['bufferView'] for i in accessors]))
        view_map = {j: i + len(self.gltf['bufferViews']) for i,j in enumerate(inds)}
        views = [copy.copy(gltf['bufferViews'][i]) for i in view_map]

        # lazy and wasteful splicing
        self.buff += bytes(((len(self.buff) + 3) & 0xFFFFFFFC) - len(self.buff))
        for view in views:
            if 'byteOffset' in view:
                view['byteOffset'] += len(self.buff)
        self.buff += buff

        for accessor in accessors: 
            accessor['bufferView'] = view_map[accessor['bufferView']]

        self.gltf['bufferViews'].extend(views)
        self.gltf['accessors'].extend(accessors)
        return accessor_map        
        
    def dump(self):
        self.gltf['buffers'] = [{'byteLength': len(self.buff)}]
        json_data = json.dumps(self.gltf).encode()
        json_data += b' ' * (((len(json_data) + 3) & 0xFFFFFFFC) - len(json_data))
        size = 28 + len(self.buff) + len(json_data)
        return b''.join([
            struct.pack("5I", 1179937895, 2, size, len(json_data), 1313821514),
            json_data,
            struct.pack("2I", len(self.buff), 5130562),
            self.buff
        ])

def splice_lod0(base, glb):
    prims = []
    for i in range(
        glb.gltf['extras']['info']['lod0']['start'], 
        glb.gltf['extras']['info']['lod1']['start']
    ):
        prims.extend(glb.gltf['meshes'][i]['primitives'])
    accessors = [j for i in prims for j in list(i['attributes'].values()) + [i['indices']]]
    m = base.splice_accessors(glb.buff, glb.gltf, *accessors)
    for prim in prims:
        prim['indices'] = m[prim['indices']]
        for k in prim['attributes']:
            prim['attributes'][k] = m[prim['attributes'][k]]
    return prims

