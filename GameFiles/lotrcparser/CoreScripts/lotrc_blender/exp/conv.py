import bpy
import numpy as np
from mathutils import Matrix, Vector, Quaternion
    
def pos_to_blender_alt(pos):
    return [[i, -k, j] for i,j,k in pos]

def size_from_blender(s):
    return [s[0], s[2], s[1]]
    
def pos_from_blender(pos):
    return [[i, k, -j] for i,j,k in zip(pos[0], pos[1], pos[2])]

def pos_from_blender_single(pos):
    return [pos[0], pos[2], -pos[1]]

def quat_from_blender(q):
    return Quaternion([q[1], q[3], -q[2], q[0]])

def mat_to_blender(m):
    return Matrix([
        [m[0][0], -m[2][0], m[1][0], m[3][0]],
        [-m[0][2], m[2][2], -m[1][2], -m[3][2]],
        [m[0][1], -m[2][1], m[1][1], m[3][1]],
        [m[0][3], -m[2][3], m[1][3], m[3][3]],
    ])
    
def mat_from_blender(m):
    return (
        (m[0][0], m[2][0], -m[1][0], m[3][0]),
        (m[0][2], m[2][2], -m[1][2], m[3][2]),
        (-m[0][1], -m[2][1], m[1][1], -m[3][1]),
        (m[0][3], m[2][3], -m[1][3], m[3][3]),
    )
