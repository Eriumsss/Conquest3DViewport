#!/usr/bin/env python3
"""
Fix spawn_node Transform values based on WorldTransform and chunk system.

The Transform field should store coordinates relative to the chunk origin.
Formula:
  Transform[12] = WorldTransform[12] - (ChunkX * 250)
  Transform[13] = WorldTransform[13] - (ChunkZ * 250)
  Transform[14] = WorldTransform[14] (Y is not chunked)
  Rotation: Same as WorldTransform
"""

import json
import sys

def calculate_chunk(x, z):
    """Calculate chunk coordinates from world position."""
    chunk_x = int(x // 250)
    chunk_z = int(z // 250)
    return chunk_x, chunk_z

def calculate_transform(world_transform):
    """
    Calculate Transform values from WorldTransform.
    
    Args:
        world_transform: 16-element list [rotation (12 elements), position (4 elements)]
    
    Returns:
        16-element list with corrected Transform values
    """
    # Extract position from WorldTransform
    world_x = world_transform[12]
    world_z = world_transform[13]
    world_y = world_transform[14]
    
    # Calculate chunk
    chunk_x, chunk_z = calculate_chunk(world_x, world_z)
    
    # Calculate chunk origin
    chunk_origin_x = chunk_x * 250
    chunk_origin_z = chunk_z * 250
    
    # Calculate Transform position (relative to chunk origin)
    transform_x = world_x - chunk_origin_x
    transform_z = world_z - chunk_origin_z
    transform_y = world_y  # Y is not chunked
    
    # Create Transform with same rotation as WorldTransform
    transform = world_transform.copy()
    transform[12] = transform_x
    transform[13] = transform_z
    transform[14] = transform_y
    
    return transform, chunk_x, chunk_z

def process_level_json(input_file, output_file):
    """Process level.json and fix all spawn_node Transform values."""
    
    print(f"Loading {input_file}...")
    with open(input_file, 'r') as f:
        data = json.load(f)
    
    spawn_nodes_fixed = 0
    spawn_points_fixed = 0
    
    print("\nProcessing blocks...")
    
    for block in data.get('blocks', []):
        block_type = block.get('type')
        fields = block.get('fields', {})
        
        if block_type == 'spawn_node':
            world_transform = fields.get('WorldTransform')
            if world_transform and len(world_transform) == 16:
                # Calculate correct Transform
                transform, chunk_x, chunk_z = calculate_transform(world_transform)
                
                # Get old Transform for comparison
                old_transform = fields.get('Transform', [])
                
                # Update Transform
                fields['Transform'] = transform
                
                guid = fields.get('GUID')
                print(f"  spawn_node {guid}: Chunk ({chunk_x}, {chunk_z})")
                print(f"    WorldTransform: ({world_transform[12]:.2f}, {world_transform[13]:.2f}, {world_transform[14]:.2f})")
                print(f"    Old Transform:  ({old_transform[12]:.2f}, {old_transform[13]:.2f}, {old_transform[14]:.2f})")
                print(f"    New Transform:  ({transform[12]:.2f}, {transform[13]:.2f}, {transform[14]:.2f})")
                
                spawn_nodes_fixed += 1
        
        elif block_type == 'spawn_point':
            world_transform = fields.get('WorldTransform')
            if world_transform and len(world_transform) == 16:
                # For spawn_points, Transform should match WorldTransform
                # (they have identity rotation or their own rotation)
                transform, chunk_x, chunk_z = calculate_transform(world_transform)
                
                old_transform = fields.get('Transform', [])
                fields['Transform'] = transform
                
                guid = fields.get('GUID')
                name = fields.get('Name', 'Unknown')
                print(f"  spawn_point {guid} ({name}): Chunk ({chunk_x}, {chunk_z})")
                
                spawn_points_fixed += 1
    
    print(f"\nSaving corrected data to {output_file}...")
    with open(output_file, 'w') as f:
        json.dump(data, f, indent=2)
    
    print(f"\n Fixed {spawn_nodes_fixed} spawn_nodes")
    print(f" Fixed {spawn_points_fixed} spawn_points")
    print(f" Saved to {output_file}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python fix_spawn_node_transforms.py <input_level.json> [output_level.json]")
        print("\nExample:")
        print("  python fix_spawn_node_transforms.py level.json level_fixed.json")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else input_file.replace('.json', '_fixed.json')
    
    try:
        process_level_json(input_file, output_file)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

