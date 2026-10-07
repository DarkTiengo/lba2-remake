#!/usr/bin/env python3
"""Bake authored boat/scooter GLBs into C++98 vertex data (no retail data).

The runtime poses the boat door or scooter steering from the actor's animation.
Only embedded, uncompressed triangle primitives with constant PBR materials
are supported. Unsupported data fails explicitly instead of losing geometry.
"""
import argparse
import json
import math
from pathlib import Path
import struct


IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def multiply(a, b):
    return [sum(a[k*4+r]*b[c*4+k] for k in range(4))
            for c in range(4) for r in range(4)]


def transform(m, v, point=True):
    return [sum(m[c*4+r]*v[c] for c in range(3)) + (m[12+r] if point else 0)
            for r in range(3)]


def node_matrix(node):
    if 'matrix' in node:
        return node['matrix']
    x, y, z, w = node.get('rotation', [0, 0, 0, 1])
    sx, sy, sz = node.get('scale', [1, 1, 1])
    tx, ty, tz = node.get('translation', [0, 0, 0])
    return [(1-2*y*y-2*z*z)*sx, (2*x*y+2*z*w)*sx, (2*x*z-2*y*w)*sx, 0,
            (2*x*y-2*z*w)*sy, (1-2*x*x-2*z*z)*sy, (2*y*z+2*x*w)*sy, 0,
            (2*x*z+2*y*w)*sz, (2*y*z-2*x*w)*sz, (1-2*x*x-2*y*y)*sz, 0,
            tx, ty, tz, 1]


def read_glb(path):
    data = Path(path).read_bytes()
    if len(data) < 20 or struct.unpack_from('<4sII', data) != (b'glTF', 2, len(data)):
        raise ValueError('Invalid GLB header')
    chunks = {}
    offset = 12
    while offset < len(data):
        size, kind = struct.unpack_from('<II', data, offset)
        offset += 8
        if offset+size > len(data):
            raise ValueError('Truncated GLB chunk')
        chunks[kind] = data[offset:offset+size]
        offset += size
    doc = json.loads(chunks[0x4e4f534a])
    binary = chunks[0x004e4942]
    if len(doc['buffers']) != 1 or 'uri' in doc['buffers'][0]:
        raise ValueError('Only embedded GLB buffers are supported')
    return doc, binary


def accessor(doc, binary, index):
    a = doc['accessors'][index]
    if 'sparse' in a or a.get('normalized'):
        raise ValueError('Sparse/normalized accessors are unsupported')
    view = doc['bufferViews'][a['bufferView']]
    code = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}[a['componentType']]
    width = {'SCALAR': 1, 'VEC3': 3}[a['type']]
    fmt = '<'+code*width
    size = struct.calcsize(fmt)
    stride = view.get('byteStride', size)
    start = view.get('byteOffset', 0)+a.get('byteOffset', 0)
    end = start+(a['count']-1)*stride+size
    if stride < size or end > view.get('byteOffset', 0)+view['byteLength'] or end > len(binary):
        raise ValueError('Accessor exceeds buffer view')
    return [struct.unpack_from(fmt, binary, start+i*stride) for i in range(a['count'])]


def srgb(value):
    return 12.92*value if value <= .0031308 else 1.055*value**(1/2.4)-.055


def scooter_front(name):
    return name.startswith(('B scooter front', 'B fork inner', 'B visible front',
                            'Front telescopic', 'Steering column', 'Wide handlebars',
                            'Black handle grip', 'B deep blue headlamp',
                            'B round lamp', 'B machined chrome headlamp',
                            'B dark central lamp', 'B polished brake',
                            'C crowned pressed-metal front fender',
                            'C front rubber sidewall',
                            'C front fender chrome'))


def bake(path, vehicle='boat'):
    doc, binary = read_glb(path)
    vertices, indices, lookup = [], [], {}
    door_nodes = 0

    def visit(index, parent):
        nonlocal door_nodes
        node = doc['nodes'][index]
        matrix = multiply(parent, node_matrix(node))
        name = node.get('name', '')
        # The entry must open onto the modeled cabin, not an opaque backing.
        if vehicle == 'boat' and name == 'entry interior dark timber bulkhead':
            return
        moving = (name in ('entry teak door leaf', 'small entry handle')
                  if vehicle == 'boat' else scooter_front(name))
        if moving:
            door_nodes += 1
        if 'skin' in node:
            raise ValueError('Skinned input needs an explicit bake')
        if 'mesh' in node:
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                if primitive.get('mode', 4) != 4 or 'extensions' in primitive:
                    raise ValueError('Only uncompressed triangles are supported')
                mat = doc['materials'][primitive['material']]
                pbr = mat.get('pbrMetallicRoughness', {})
                if any('Texture' in k for k in pbr):
                    raise ValueError('Texture baking is not supported')
                color = tuple(srgb(v) for v in pbr.get('baseColorFactor', [1, 1, 1, 1])[:3])
                roughness = pbr.get('roughnessFactor', 1)
                metallic = pbr.get('metallicFactor', 1)
                glass = 'KHR_materials_transmission' in mat.get('extensions', {})
                pos = accessor(doc, binary, primitive['attributes']['POSITION'])
                normals = accessor(doc, binary, primitive['attributes']['NORMAL'])
                order = accessor(doc, binary, primitive['indices'])
                if len(order) % 3:
                    raise ValueError('Incomplete triangle')
                remap = []
                # Inverse transpose for normals, including non-uniform scale.
                a, b, c = matrix[0:3], matrix[4:7], matrix[8:11]
                def cross(u, v):
                    return [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]]
                cof = [cross(b, c), cross(c, a), cross(a, b)]
                det = sum(a[i]*cof[0][i] for i in range(3))
                if abs(det) < 1e-12:
                    raise ValueError('Singular node transform')
                for p, n in zip(pos, normals):
                    p = transform(matrix, p)
                    n = [sum(cof[j][i]*n[j] for j in range(3))/det for i in range(3)]
                    length = math.sqrt(sum(v*v for v in n))
                    if length < 1e-9:
                        raise ValueError('Zero normal')
                    n = [v/length for v in n]
                    if moving and vehicle == 'boat':
                        p[2] += .325  # place the door at the entry, not behind it
                    # glTF Y-up -> engine Y-up; forward faces +Z.
                    if vehicle == 'boat':
                        p = (p[2]*1100, (p[1]-.5)*1100, -p[0]*1100)
                    else:
                        # Chassis group 2 rests at Y=416; steering group 20
                        # is offset (0,-250,250) from that chassis.
                        p = (p[2]*680, p[1]*680-416, -p[0]*680)
                        if moving:
                            p = (p[0], p[1]+250, p[2]-250)
                    n = (n[2], n[1], -n[0])
                    key = tuple(round(v, 6) for v in (*p, *n, *color, roughness, metallic)) + (int(moving), int(glass))
                    if not all(math.isfinite(v) for v in key):
                        raise ValueError('Non-finite vertex')
                    if key not in lookup:
                        lookup[key] = len(vertices)
                        vertices.append(key)
                    remap.append(lookup[key])
                for i in range(0, len(order), 3):
                    tri = [remap[order[i+j][0]] for j in range(3)]
                    if det < 0:
                        tri[1], tri[2] = tri[2], tri[1]
                    indices.extend(tri)
        for child in node.get('children', []):
            visit(child, matrix)

    for root in doc['scenes'][doc.get('scene', 0)]['nodes']:
        visit(root, IDENTITY)
    if not vertices or not indices or (vehicle == 'boat' and door_nodes != 2) or (vehicle == 'scooter' and not door_nodes):
        raise ValueError('Expected vehicle geometry and its moving parts')
    return vertices, indices


def write_header(path, vertices, indices, vehicle='boat'):
    def number(v):
        text = f'{v:.6f}'.rstrip('0').rstrip('.')
        return (text if '.' in text else text+'.0')+'f'
    with Path(path).open('w', encoding='utf-8') as out:
        out.write('// Generated by export_citadel_boat.py; original project geometry.\n')
        out.write(f'static const T_CITADEL_{vehicle.upper()}_VERTEX s_{vehicle}Vertices[] = {{\n')
        for v in vertices:
            out.write('    {{'+','.join(number(x) for x in v[:3])+'},{'+
                      ','.join(number(x) for x in v[3:6])+'},{'+
                      ','.join(number(x) for x in v[6:9])+'},'+
                      number(v[9])+','+number(v[10])+','+str(v[11])+','+str(v[12])+'},\n')
        out.write(f'}};\nstatic const U32 s_{vehicle}Indices[] = {{\n')
        for i in range(0, len(indices), 18):
            out.write('    '+','.join(map(str, indices[i:i+18]))+',\n')
        out.write('};\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input')
    parser.add_argument('output')
    parser.add_argument('--vehicle', choices=('boat', 'scooter'), default='boat')
    args = parser.parse_args()
    vertices, indices = bake(args.input, args.vehicle)
    write_header(args.output, vertices, indices, args.vehicle)
    print(f'{args.vehicle}: {len(vertices)} vertices, {len(indices)//3} triangles')


if __name__ == '__main__':
    main()
