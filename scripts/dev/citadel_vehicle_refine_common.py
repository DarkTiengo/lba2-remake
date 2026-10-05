"""Shared Blender 5.2 geometry helpers for the Citadel concept-B drafts.

This file is prepended to one vehicle script and executed in an editable
3D scene. It never reads or embeds the original game's proprietary meshes.
"""
import bpy
import math
from mathutils import Vector


def material(name, rgb, metallic=0.0, roughness=0.35, alpha=1.0):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.diffuse_color = (*rgb, alpha)
    m.use_nodes = True
    p = m.node_tree.nodes.get("Principled BSDF")
    p.inputs["Base Color"].default_value = (*rgb, 1.0)
    p.inputs["Metallic"].default_value = metallic
    p.inputs["Roughness"].default_value = roughness
    p.inputs["Alpha"].default_value = alpha
    if alpha < 1:
        m.surface_render_method = "DITHERED"
    return m


def assign(obj, name, mat, smooth=True):
    obj.name = name
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    if smooth:
        for face in obj.data.polygons:
            face.use_smooth = True
    return obj


def remove_matching(predicate):
    for obj in list(bpy.data.objects):
        if obj.type in {"MESH", "CURVE"} and predicate(obj.name):
            bpy.data.objects.remove(obj, do_unlink=True)


def mesh(name, vertices, faces, mat, smooth=True):
    data = bpy.data.meshes.new(name)
    data.from_pydata(vertices, [], faces)
    data.update()
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    return assign(obj, name, mat, smooth)


def rounded_box(name, center, dimensions, mat, bevel=0.05, segments=4):
    bpy.ops.mesh.primitive_cube_add(size=1, location=center)
    obj = bpy.context.object
    obj.dimensions = dimensions
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    assign(obj, name, mat, smooth=False)
    if bevel:
        b = obj.modifiers.new("manufactured edge radius", "BEVEL")
        b.width = bevel
        b.segments = segments
        obj.modifiers.new("weighted face normals", "WEIGHTED_NORMAL")
    return obj


def ellipsoid(name, center, scale, mat, segments=64, rings=32):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings, location=center)
    obj = bpy.context.object
    obj.scale = scale
    return assign(obj, name, mat)


def rod(name, a, b, radius, mat, sides=24):
    a = Vector(a)
    b = Vector(b)
    direction = b - a
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=sides, radius=radius, depth=direction.length,
        location=(a + b) / 2,
    )
    obj = bpy.context.object
    obj.rotation_euler = direction.to_track_quat("Z", "Y").to_euler()
    return assign(obj, name, mat)


def tubing(name, points, radius, mat, resolution=4):
    data = bpy.data.curves.new(name, "CURVE")
    data.dimensions = "3D"
    data.resolution_u = 16
    data.bevel_depth = radius
    data.bevel_resolution = resolution
    spline = data.splines.new("BEZIER")
    spline.bezier_points.add(len(points) - 1)
    for control, p in zip(spline.bezier_points, points):
        control.co = p
        control.handle_left_type = "AUTO"
        control.handle_right_type = "AUTO"
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(mat)
    return obj


def loft_x(name, sections, mat, sides=64, subdivision=1):
    """Closed oval hull along X: (x, ycentre, zcentre, y_radius, z_radius)."""
    verts = []
    for x, yc, zc, ry, rz in sections:
        for i in range(sides):
            angle = 2 * math.pi * i / sides
            verts.append((x, yc + ry * math.cos(angle), zc + rz * math.sin(angle)))
    faces = []
    for k in range(len(sections) - 1):
        for i in range(sides):
            j = (i + 1) % sides
            faces.append((k * sides + i, k * sides + j,
                          (k + 1) * sides + j, (k + 1) * sides + i))
    faces.append(tuple(reversed(range(sides))))
    faces.append(tuple((len(sections) - 1) * sides + i for i in range(sides)))
    obj = mesh(name, verts, faces, mat)
    if subdivision:
        mod = obj.modifiers.new("smooth body curvature", "SUBSURF")
        mod.levels = subdivision
        mod.render_levels = subdivision
    return obj


def tire(name, x, y, z, radius, width, rubber, metal, dark,
         rim_ratio=0.53):
    """Dense tire with circumferential tread, metal rim and centre cap."""
    sides = 96
    rings = 14
    vertices = []
    for j in range(rings + 1):
        v = j / rings
        yy = y + (v - 0.5) * width
        shoulder = math.sin(math.pi * v) ** 0.45
        for i in range(sides):
            angle = 2 * math.pi * i / sides
            zigzag = math.sin(30 * angle + 6 * v)
            lug = max(0.0, zigzag) * 0.012 * shoulder
            rr = radius * (0.79 + 0.22 * shoulder) + lug
            vertices.append((x + rr * math.cos(angle), yy,
                             z + rr * math.sin(angle)))
    faces = []
    for j in range(rings):
        for i in range(sides):
            ni = (i + 1) % sides
            faces.append((j*sides+i, j*sides+ni,
                          (j+1)*sides+ni, (j+1)*sides+i))
    body = mesh(name + " detailed tread", vertices, faces, rubber)
    for side in (-1, 1):
        yy = y + side * width * 0.52
        bpy.ops.mesh.primitive_cylinder_add(
            vertices=64, radius=radius*rim_ratio, depth=0.035,
            location=(x, yy, z), rotation=(math.pi/2, 0, 0))
        assign(bpy.context.object, name + " alloy rim", metal)
        bpy.ops.mesh.primitive_torus_add(
            major_segments=64, minor_segments=12,
            location=(x, yy+side*0.025, z), rotation=(math.pi/2, 0, 0),
            major_radius=radius*(rim_ratio-0.10),
            minor_radius=radius*0.028)
        assign(bpy.context.object, name + " rim lip", metal)
        bpy.ops.mesh.primitive_cylinder_add(
            vertices=48, radius=radius*0.20, depth=0.05,
            location=(x, yy+side*0.04, z), rotation=(math.pi/2, 0, 0))
        assign(bpy.context.object, name + " dark hub", dark)
    return body


def spring(name, start, end, radius, turns, material):
    start = Vector(start)
    end = Vector(end)
    axis = end - start
    u = axis.normalized().cross(Vector((0, 1, 0))).normalized()
    v = axis.normalized().cross(u).normalized()
    points = []
    for i in range(turns*16 + 1):
        t = i / (turns*16)
        a = t * turns * 2 * math.pi
        p = start + axis*t + radius * (u*math.cos(a) + v*math.sin(a))
        points.append(tuple(p))
    data = bpy.data.curves.new(name, "CURVE")
    data.dimensions = "3D"
    data.bevel_depth = radius*0.13
    data.bevel_resolution = 3
    spline = data.splines.new("POLY")
    spline.points.add(len(points)-1)
    for point, p in zip(spline.points, points):
        point.co = (*p, 1)
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    data.materials.append(material)
    return obj


def triangle_count():
    # Evaluated mesh count is intentionally omitted: subdivision is noted
    # separately because it is exported on the committed GLB.
    return sum(len(face.vertices)-2
               for obj in bpy.data.objects if obj.type == "MESH"
               for face in obj.data.polygons)
