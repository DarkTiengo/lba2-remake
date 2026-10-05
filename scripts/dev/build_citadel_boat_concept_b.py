"""Build an editable Citadel boat from the approved concept-B reference.

Run in Blender 5.2. The scene uses metres, Z up, and a bow facing -X.
It contains original geometry only and replaces the current draft scene.
Studio ground and softbox lights belong to the separate preview query.
"""
import bpy
import bmesh
import math
from mathutils import Vector

BLENDER_MIN_VERSION = (5, 2, 0)
if bpy.app.version < BLENDER_MIN_VERSION:
    raise RuntimeError('This scene builder requires Blender %s or newer' %
                       '.'.join(map(str, BLENDER_MIN_VERSION)))

for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)


def material(name, color, metallic=0.0, roughness=.35, alpha=1, coat=0):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    p = mat.node_tree.nodes.get('Principled BSDF')
    p.inputs['Base Color'].default_value = (*color, 1)
    p.inputs['Metallic'].default_value = metallic
    p.inputs['Roughness'].default_value = roughness
    p.inputs['Alpha'].default_value = alpha
    p.inputs['Coat Weight'].default_value = coat
    p.inputs['Coat Roughness'].default_value = .24
    mat.diffuse_color = (*color, alpha)
    mat.use_backface_culling = False
    if alpha < 1:
        mat.surface_render_method = 'DITHERED'
    return mat


CREAM = material('CB cream painted aluminium', (.82, .64, .32), .10, .32, coat=.18)
RED = material('CB red marine enamel', (.45, .010, .016), .04, .40, coat=.10)
RED.node_tree.nodes.get('Principled BSDF').inputs['Specular IOR Level'].default_value = .20
TEAL = material('CB dark teal antifouling', (.015, .145, .148), .08, .43)
CHROME = material('CB brushed stainless steel', (.39, .42, .44), .82, .28)
DARK_METAL = material('CB crane graphite steel', (.048, .052, .065), .62, .33)
RUBBER = material('CB charcoal window seals', (.020, .024, .027), 0, .64)
GLASS = material('CB blue green transparent glazing', (.40, .72, .74), 0, .09)
glass_shader = GLASS.node_tree.nodes.get('Principled BSDF')
glass_shader.inputs['Transmission Weight'].default_value = .92
glass_shader.inputs['IOR'].default_value = 1.46
WHITE = material('CB lifebuoy ivory', (.84, .83, .73), 0, .39)
WOOD = material('CB varnished warm teak', (.30, .135, .040), 0, .38, coat=.14)
WOOD_DARK = material('CB teak end grain and seams', (.115, .047, .016), 0, .57)
SEAT = material('CB dark blue upholstery', (.031, .051, .059), 0, .59)
CONSOLE = material('CB helm console grey', (.10, .115, .12), .18, .46)
BRONZE = material('CB ochre cargo bucket', (.35, .145, .027), .40, .35)
SEAM = material('CB warm panel seams', (.30, .235, .12), .1, .48)


def finish(obj, name, mat, smooth=False):
    obj.name = name
    if mat:
        obj.data.materials.append(mat)
    if obj.type == 'MESH':
        for face in obj.data.polygons:
            face.use_smooth = smooth
    return obj


def mesh(name, vertices, faces, mat, smooth=False, bevel=0):
    data = bpy.data.meshes.new(name)
    data.from_pydata(vertices, [], faces)
    data.update()
    bm = bmesh.new()
    bm.from_mesh(data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(data)
    bm.free()
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    finish(obj, name, mat, smooth)
    if bevel:
        mod = obj.modifiers.new('small manufactured edge radius', 'BEVEL')
        mod.width = bevel
        mod.segments = 3
        obj.modifiers.new('weighted face normals', 'WEIGHTED_NORMAL')
    return obj


def box(name, xyz, dims, mat, bevel=.015):
    bpy.ops.mesh.primitive_cube_add(size=1, location=xyz)
    obj = bpy.context.object
    obj.dimensions = dims
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    finish(obj, name, mat)
    if bevel:
        b = obj.modifiers.new('rounded edges', 'BEVEL')
        b.width = bevel
        b.segments = 4
        obj.modifiers.new('weighted normals', 'WEIGHTED_NORMAL')
    return obj


def rod(name, a, b, radius, mat, sides=24):
    a, b = Vector(a), Vector(b)
    delta = b-a
    bpy.ops.mesh.primitive_cylinder_add(vertices=sides, radius=radius,
                                      depth=delta.length, location=(a+b)/2)
    obj = bpy.context.object
    obj.rotation_euler = delta.to_track_quat('Z', 'Y').to_euler()
    return finish(obj, name, mat, True)


def tube(name, points, radius, mat, closed=False):
    data = bpy.data.curves.new(name, 'CURVE')
    data.dimensions = '3D'
    data.resolution_u = 1
    data.bevel_depth = radius
    data.bevel_resolution = 3
    data.use_fill_caps = True
    spline = data.splines.new('POLY')
    spline.points.add(len(points)-1)
    for p, xyz in zip(spline.points, points):
        p.co = (*xyz, 1)
    spline.use_cyclic_u = closed
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    data.materials.append(mat)
    return obj


def outline(xmin, xmax, ymin, ymax, front=.4, rear=.12, steps=10,
            front_y=None):
    ry = front if front_y is None else front_y
    corners = [(xmin+front, ymin+ry, front, ry, math.pi),
               (xmax-rear, ymin+rear, rear, rear, 1.5*math.pi),
               (xmax-rear, ymax-rear, rear, rear, 0),
               (xmin+front, ymax-ry, front, ry, .5*math.pi)]
    points = []
    for cx, cy, rx, ry, start in corners:
        for i in range(steps):
            angle = start + .5*math.pi*i/(steps-1)
            points.append((cx+rx*math.cos(angle), cy+ry*math.sin(angle)))
    return points


def slab(name, perimeter, low, high, mat, bevel=.01):
    count = len(perimeter)
    vertices = [(x, y, z) for z in (low, high) for x, y in perimeter]
    faces = [(i, (i+1)%count, count+(i+1)%count, count+i)
             for i in range(count)]
    faces += [tuple(reversed(range(count))), tuple(count+i for i in range(count))]
    return mesh(name, vertices, faces, mat, bevel=bevel)


def rivet(xyz, axis='Y', mat=CHROME, radius=.010):
    a = Vector(xyz)
    direction = Vector({'X': (1, 0, 0), 'Y': (0, 1, 0), 'Z': (0, 0, 1)}[axis])
    return rod('stainless flush fastener', a-direction*.004,
               a+direction*.004, radius, mat, 12)


# Two raked pontoons and a high bridge deck form the bow tunnel.
for sign, name in ((-1, 'port'), (1, 'starboard')):
    rings = [(.065, -2.06, 2.13, .385),
             (.10, -2.15, 2.18, .415),
             (.215, -2.22, 2.21, .445),
             (.40, -2.31, 2.27, .470),
             (.87, -2.47, 2.39, .520),
             (1.10, -2.51, 2.44, .545)]
    vertices = []
    count = 48
    for z, xmin, xmax, width in rings:
        p = outline(xmin, xmax, sign*.665-width, sign*.665+width,
                    front=.46, rear=.16, steps=12, front_y=width*.85)
        vertices.extend((x, y, z) for x, y in p)
    faces = []
    bands = []
    for k in range(len(rings)-1):
        for i in range(count):
            faces.append((k*count+i, k*count+(i+1)%count,
                          (k+1)*count+(i+1)%count, (k+1)*count+i))
            bands.append(1 if k < 2 else 0)
    faces += [tuple(reversed(range(count))),
              tuple((len(rings)-1)*count+i for i in range(count))]
    hull = mesh(name+' sculpted pontoon', vertices, faces, CREAM, True)
    hull.data.materials.append(TEAL)
    for face, band in zip(hull.data.polygons, bands):
        face.material_index = band
    hull.data.polygons[-2].material_index = 1
    hull.data.polygons[-2].use_smooth = False
    hull.data.polygons[-1].use_smooth = False

box('bridge underside above bow tunnel', (.24, 0, .935),
    (3.93, .55, .30), CREAM, .045)
deck_outline = outline(-2.56, 2.48, -1.23, 1.23, .60, .19, 14, .72)
slab('cream continuous deck coaming', deck_outline, 1.085, 1.205, CREAM, .018)
wood_outline = outline(-2.48, 2.40, -1.155, 1.155, .56, .15, 14, .68)
slab('inset teak deck', wood_outline, 1.205, 1.227, WOOD, .008)

# Red belt is a broad sheet bonded to the hull, with the bow tip left cream.
for sign in (-1, 1):
    sections = [(-1.86, 1.23), (-1.56, 1.23), (-.7, 1.23),
                (.7, 1.23), (2.20, 1.23), (2.43, 1.166)]
    verts = []
    for x, y in sections:
        low_x = min(x, 2.365)
        low_y = 1.188 if x < 2.23 else 1.024+math.sqrt(.16**2-(low_x-2.225)**2)
        verts.extend([(low_x, sign*low_y, .850),
                      (x, sign*(y+.004), 1.170)])
    belt = mesh('red upper hull belt', verts,
                [(i*2, i*2+1, i*2+3, i*2+2)
                 for i in range(len(sections)-1)], RED, bevel=.008)
    solid = belt.modifiers.new('painted metal thickness', 'SOLIDIFY')
    solid.thickness = .018
    for x in (-.70, .82, 2.05):
        tube('vertical hull panel seam',
             [(x, sign*1.117, .28), (x, sign*1.143, .50),
              (x, sign*1.198, .88), (x, sign*1.224, 1.15)], .003, SEAM)
        for z in (.34, .49, .73, .97, 1.13):
            y = 1.11 + (z-.25)*.128
            for dx in (-.025, .025):
                rivet((x+dx, sign*(y+.012), z), radius=.007)

# Long, fine plank gaps follow the exposed bow and stern deck.
for i in range(-7, 8):
    y = i*.145
    bow_x = -2.46 + .30*(abs(y)/1.13)**3
    tube('teak deck caulking', [(bow_x, y, 1.232), (2.38, y, 1.232)], .003, WOOD_DARK)


def map_quad(corners, u, v):
    a, b, c, d = [Vector(p) for p in corners]
    return tuple((1-v)*((1-u)*a+u*b) + v*((1-u)*d+u*c))


def window(name, corners, margin=(.07, .055), radius=.070):
    # Paired rounded loops form real frame geometry and a separate glass pane.
    u, v = margin
    outer = outline(0, 1, 0, 1, .007, .007, 8)
    opening = outline(u, 1-u, v, 1-v, radius, radius, 8)
    inset = outline(u+.015, 1-u-.015, v+.012, 1-v-.012,
                    max(.018, radius-.006), max(.018, radius-.006), 8)
    count = len(outer)
    ring_faces = [(i, (i+1)%count, count+(i+1)%count, count+i)
                  for i in range(count)]
    frame = mesh(name+' cream structural surround',
                 [map_quad(corners, x, y) for x, y in outer+opening],
                 ring_faces, CREAM)
    solid = frame.modifiers.new('folded window panel', 'SOLIDIFY')
    solid.thickness = .024
    solid.offset = 0
    seal = mesh(name+' grey rubber gasket',
                [map_quad(corners, x, y) for x, y in opening+inset],
                ring_faces, RUBBER)
    solid = seal.modifiers.new('window seal depth', 'SOLIDIFY')
    solid.thickness = .010
    solid.offset = 0
    glass = mesh(name+' transparent glass',
                 [map_quad(corners, x, y) for x, y in inset],
                 [tuple(range(count))], GLASS)
    solid = glass.modifiers.new('laminated glazing thickness', 'SOLIDIFY')
    solid.thickness = .006
    solid.offset = 0
    return glass


def side_point(x, z, sign):
    y = 1.035 - (z-1.24)*.135
    return (x, sign*y, z)


# The panoramic windshield wraps round the bow, with a sill under it.
lower = [(-1.10, -1.007), (-1.76, -.62), (-1.81, 0),
         (-1.76, .62), (-1.10, 1.007)]
upper = [(-1.02, -.892), (-1.27, -.55), (-1.31, 0),
         (-1.27, .55), (-1.02, .892)]
for i in range(4):
    corners = [(*lower[i], 1.47), (*lower[i+1], 1.47),
               (*upper[i+1], 2.365), (*upper[i], 2.365)]
    window('panoramic windshield %d' % i, corners, (.032, .025), .055)
    mesh('sloped cream windscreen sill',
         [(*lower[i], 1.47), (*lower[i+1], 1.47),
          (lower[i+1][0]-.05, lower[i+1][1]*1.018, 1.245),
          (lower[i][0]-.05, lower[i][1]*1.018, 1.245)],
         [(0, 1, 2, 3)], CREAM, bevel=.012)

# Tall side panes, narrow sill and an open-looking timber entry recess.
for sign in (-1, 1):
    first = [side_point(-1.10, 1.245, sign), side_point(-.40, 1.245, sign),
             side_point(-.40, 2.365, sign), side_point(-1.02, 2.365, sign)]
    second = [side_point(-.40, 1.245, sign), side_point(.26, 1.245, sign),
              side_point(.26, 2.365, sign), side_point(-.40, 2.365, sign)]
    window('forward side window', first, (.065, .074), .065)
    window('rear side window', second, (.07, .074), .065)
    mesh('sealed windshield side corner',
         [side_point(-1.10, 1.245, sign), side_point(-1.02, 2.365, sign),
          (-1.02, sign*.892, 2.365), (-1.10, sign*1.007, 1.47),
          (-1.15, sign*1.007*1.018, 1.245)],
         [(0, 1, 2, 3, 4)], CREAM)
    if sign > 0:
        third = [side_point(.26, 1.245, sign), side_point(.97, 1.245, sign),
                 side_point(.97, 2.365, sign), side_point(.26, 2.365, sign)]
        window('far cabin window', third, (.08, .08), .075)
    else:
        # Recessed doorway, jamb thickness, timber lining and a physical step.
        for x in (.285, .945):
            rod('entry cream jamb', side_point(x, 1.255, sign),
                side_point(x, 2.365, sign), .022, CREAM)
        box('entry interior dark timber bulkhead', (.60, -.62, 1.795),
            (.66, .04, 1.05), WOOD_DARK, .01)
        box('entry teak door leaf', (.60, -.655, 1.795),
            (.57, .035, .98), WOOD, .015)
        box('entry teak threshold', (.60, -.922, 1.252),
            (.67, .27, .05), WOOD, .009)
        rod('small entry handle', (.82, -.681, 1.69), (.82, -.681, 1.83),
            .014, CHROME)
    mesh('sloped aft cabin wall',
         [side_point(.97, 1.245, sign), side_point(1.68, 1.245, sign),
          side_point(1.36, 2.365, sign), side_point(.97, 2.365, sign)],
         [(0, 1, 2, 3)], CREAM, bevel=.018)
    mesh('red band behind lifebuoy',
         [(x, y+sign*.008, z) for x, y, z in
          [side_point(.98, 1.73, sign), side_point(1.54, 1.73, sign),
           side_point(1.49, 1.90, sign), side_point(.98, 1.90, sign)]],
         [(0, 1, 2, 3)], RED)
    for x in (-.40, .26, .95):
        y = sign*.976
        box('red window pillar strap', (x, y, 1.81), (.115, .050, .17), RED, .008)
        for dx in (-.038, .038):
            for dz in (-.057, .057):
                rivet((x+dx, y+sign*.029, 1.81+dz), radius=.007)

mesh('aft cabin sloping bulkhead',
     [(1.68, -1.035, 1.245), (1.68, 1.035, 1.245),
      (1.36, .884, 2.365), (1.36, -.884, 2.365)],
     [(0, 1, 2, 3)], CREAM, bevel=.015)

# Interior objects have volume and remain visible through the separate panes.
box('cabin floor', (-.12, 0, 1.255), (3.05, 1.81, .05), WOOD_DARK, .01)
box('helm seat cushion', (-.57, -.22, 1.64), (.43, .46, .11), SEAT, .06)
seat = box('helm high backrest', (-.30, -.22, 1.87), (.13, .45, .48), SEAT, .05)
seat.rotation_euler[1] = -.12
rod('helm chair pedestal', (-.52, -.22, 1.30), (-.52, -.22, 1.58), .045, CHROME)
dash = box('main helm dashboard', (-1.21, -.18, 1.65), (.35, .76, .32), CONSOLE, .045)
dash.rotation_euler[1] = -.14
for y in (-.43, -.20, .035):
    box('instrument recessed dial', (-1.17, y, 1.822), (.16, .13, .012), RUBBER, .02)
    box('instrument marking', (-1.17, y, 1.831), (.012, .077, .008), WHITE, .002)
rod('wheel steering shaft', (-1.03, -.22, 1.72), (-.81, -.22, 1.83), .017, CHROME)
bpy.ops.mesh.primitive_torus_add(major_segments=48, minor_segments=10,
    location=(-.81, -.22, 1.84), rotation=(0, math.pi/2-.20, 0),
    major_radius=.11, minor_radius=.012)
finish(bpy.context.object, 'black helm steering wheel', RUBBER, True)
for angle in (0, 2*math.pi/3, 4*math.pi/3):
    rod('steering wheel spoke', (-.81, -.22, 1.84),
        (-.81, -.22+.105*math.cos(angle), 1.84+.105*math.sin(angle)), .006, CHROME)
box('passenger bench cushion', (.30, .63, 1.53), (1.14, .32, .12), SEAT, .055)
box('passenger bench back', (.30, .77, 1.73), (1.14, .09, .36), SEAT, .04)

# Hip roof: substantial sloped faces and a narrow continuous eave.
eave = outline(-1.42, 1.53, -1.035, 1.035, .40, .08, 12, .43)
roof_top = outline(-.93, 1.20, -.73, .73, .18, .08, 12, .20)
slab('red roof eave rolled lip', eave, 2.358, 2.403, RED, .012)
count = len(eave)
verts = [(x, y, 2.400) for x, y in eave] + [(x, y, 2.815) for x, y in roof_top]
faces = [(i, (i+1)%count, count+(i+1)%count, count+i) for i in range(count)]
faces += [tuple(count+i for i in range(count))]
mesh('red four-sided hip roof', verts, faces, RED, bevel=.010)
slab('cream cabin ceiling liner',
     outline(-1.33, 1.42, -.90, .90, .35, .07, 10, .36), 2.333, 2.353, CREAM)
for sign in (-1, 1):
    tube('roof panel seam', [(-1.10, sign*.94, 2.414),
                             (-.84, sign*.70, 2.818)], .003, RED)

# Stainless deck rails with rounded turns and short, physically attached posts.
for sign in (-1, 1):
    p = [(-2.30, sign*.62, 1.46), (-2.27, sign*.84, 1.46),
         (-2.15, sign*1.04, 1.46), (-2.04, sign*1.12, 1.46),
         (-1.62, sign*1.14, 1.46), (-1.31, sign*1.14, 1.46)]
    tube('bow safety railing', p, .017, CHROME)
    for x, y in ((-2.27, .77), (-1.95, 1.14), (-1.37, 1.14)):
        rod('bow railing stanchion', (x, sign*y, 1.233),
            (x, sign*y, 1.46), .014, CHROME)
        box('railing base flange', (x, sign*y, 1.239), (.065, .053, .012), CHROME, .008)
    tube('stern safety railing', [(1.59, sign*1.145, 1.62),
         (2.24, sign*1.145, 1.62), (2.36, sign*1.10, 1.62),
         (2.40, sign*.96, 1.62), (2.40, 0, 1.62)], .018, CHROME)
    for x, y in ((1.66, 1.145), (2.23, 1.145), (2.40, .65)):
        rod('stern railing stanchion', (x, sign*y, 1.235),
            (x, sign*y, 1.62), .017, CHROME)
        box('stern railing foot', (x, sign*y, 1.24), (.064, .055, .015), CHROME, .007)
    # Short bow mooring bollards, with their caps and mounting plates.
    x, y = -2.12, sign*.56
    box('bollard foot', (x, y, 1.243), (.15, .13, .022), CHROME, .012)
    rod('mooring bollard shank', (x, y, 1.25), (x, y, 1.35), .039, CHROME)
    rod('mooring bollard cap', (x, y, 1.34), (x, y, 1.365), .060, CHROME)

# Cargo deck plank walls and an articulated hydraulic davit.
for z in (1.32, 1.48, 1.64):
    box('stern cargo timber slat', (2.23, .21, z), (.075, 1.30, .135), WOOD, .008)
for y in (-.46, .87):
    box('cargo wall corner steel', (2.19, y, 1.47), (.11, .07, .50), CHROME, .009)


def extruded_xz(name, coords, y, depth, mat, bevel=.015):
    n = len(coords)
    verts = [(x, yy, z) for yy in (y-depth/2, y+depth/2) for x, z in coords]
    faces = [tuple(reversed(range(n))), tuple(n+i for i in range(n))]
    faces += [(i, (i+1)%n, n+(i+1)%n, n+i) for i in range(n)]
    return mesh(name, verts, faces, mat, bevel=bevel)


box('crane bolted deck foundation', (1.79, .52, 1.25), (.40, .39, .075), DARK_METAL, .017)
box('crane lower pedestal', (1.77, .52, 1.81), (.22, .24, 1.11), DARK_METAL, .026)
extruded_xz('crane broad articulated lifting arm',
    [(1.61, 2.23), (1.60, 2.50), (2.10, 3.34),
     (2.33, 3.34), (2.29, 3.10), (1.86, 2.34)], .52, .23, DARK_METAL)
extruded_xz('crane horizontal upper jib',
    [(2.13, 3.33), (2.73, 3.28), (2.73, 3.09), (2.24, 3.08)],
    .52, .20, DARK_METAL)
for x, z, radius in ((1.73, 2.43, .105), (2.26, 3.21, .090), (2.69, 3.20, .045)):
    rod('crane hinge bearing', (x, .365, z), (x, .39, z), radius, CHROME, 40)
    rod('crane pivot cap', (x, .350, z), (x, .368, z), radius*.55, DARK_METAL, 32)
rod('hydraulic cylinder housing', (1.78, .34, 1.62), (1.68, .34, 2.11), .045, DARK_METAL)
rod('hydraulic polished piston', (1.68, .34, 2.10), (1.70, .34, 2.40), .023, CHROME)
tube('crane outer plate seam', [(1.64, .392, 2.52), (2.12, .392, 3.30),
                               (2.26, .392, 3.30)], .009, CHROME)
rod('hanging lifting cable', (2.70, .52, 3.12), (2.70, .52, 2.88), .011, DARK_METAL)
tube('lifting hook', [(2.70, .52, 2.90), (2.70, .52, 2.85),
                      (2.72, .52, 2.82), (2.76, .52, 2.83), (2.76, .52, 2.86)], .014, CHROME)
top = [(2.43, .28, 2.61), (2.96, .28, 2.61), (2.96, .77, 2.61), (2.43, .77, 2.61)]
bottom = [(2.52, .36, 2.28), (2.86, .36, 2.28), (2.86, .69, 2.28), (2.52, .69, 2.28)]
bucket = mesh('open ochre work bucket', top+bottom,
              [(0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6),
               (3, 0, 4, 7), (4, 5, 6, 7)], BRONZE, bevel=.006)
bucket.modifiers.new('bucket sheet thickness', 'SOLIDIFY').thickness = .018
tube('bucket folded rim', top, .010, BRONZE, True)
for xyz in (top[0], top[1], top[2], top[3]):
    rod('bucket suspension strap', (2.72, .52, 2.83), xyz, .012, BRONZE)

# White rescue ring with four clean red textile bands and mounting rope.
cx, cy, cz = 1.225, -1.052, 1.84
major, minor = .205, .050
bpy.ops.mesh.primitive_torus_add(major_segments=64, minor_segments=16,
    location=(cx, cy, cz), rotation=(math.pi/2, 0, 0),
    major_radius=major, minor_radius=minor)
finish(bpy.context.object, 'ivory rescue lifebuoy', WHITE, True)
for centre in (0, math.pi/2, math.pi, 1.5*math.pi):
    verts, faces = [], []
    for i in range(9):
        a = centre + (i/8-.5)*.47
        for j in range(16):
            b = 2*math.pi*j/16
            r = major+(minor+.0015)*math.cos(b)
            verts.append((cx+r*math.cos(a), cy+(minor+.0015)*math.sin(b), cz+r*math.sin(a)))
    for i in range(8):
        for j in range(16):
            faces.append((i*16+j, i*16+(j+1)%16,
                          (i+1)*16+(j+1)%16, (i+1)*16+j))
    mesh('red lifebuoy fabric band', verts, faces, RED, True)
tube('lifebuoy mounting line', [(cx-.055, cy+.005, cz+.18),
     (cx, cy+.004, cz+.235), (cx+.055, cy+.005, cz+.18)], .008, WOOD)

# Thin windscreen wipers, side-panel fasteners and restrained surface joints.
for sign in (-1, 1):
    rod('windscreen wiper arm', (-1.77, sign*.25, 1.51),
        (-1.53, sign*.27, 1.91), .006, CHROME)
    rod('windscreen black wiper blade', (-1.62, sign*.28, 1.75),
        (-1.43, sign*.30, 2.08), .007, RUBBER)
    for x in (-1.34, -.39, .96, 1.53):
        for z in (1.28, 2.30):
            if x > 1.40 and z > 2:
                continue
            _, y, _ = side_point(x, z, sign)
            rivet((x, y+sign*.012, z), radius=.008)

# Keep the cabin low and broad; translate round rescue gear without flattening it.
bpy.context.view_layer.update()
for obj in list(bpy.context.scene.objects):
    if obj.type not in {'MESH', 'CURVE'}:
        continue
    matrix = obj.matrix_world.copy()
    inverse = matrix.inverted()
    points = (obj.data.vertices if obj.type == 'MESH' else
              [p for spline in obj.data.splines for p in spline.points])
    for point in points:
        xyz = matrix @ Vector(point.co[:3])
        if 'lifebuoy' in obj.name:
            xyz.z -= (1.84-1.23)*.12
        elif xyz.z > 1.23:
            xyz.z = 1.23+(xyz.z-1.23)*.88
        local = inverse @ xyz
        point.co = local if obj.type == 'MESH' else (*local, 1)

scene = bpy.context.scene
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x = 1152
scene.render.resolution_y = 648
scene.render.resolution_percentage = 100
scene.render.film_transparent = False
scene.world = bpy.data.worlds.new('CB neutral studio environment')
scene.world.use_nodes = True
scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (.53, .51, .48, 1)
scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = .5


def look(obj, target):
    obj.rotation_euler = (Vector(target)-obj.location).to_track_quat('-Z', 'Y').to_euler()


bpy.ops.object.camera_add(location=(-6, -12, 4.5))
camera = bpy.context.object
camera.name = 'Concept B comparison camera'
camera.data.type = 'ORTHO'
camera.data.ortho_scale = 6.9
look(camera, (.05, 0, 1.60))
scene.camera = camera
bpy.context.view_layer.update()
camera_inverse = camera.matrix_world.inverted()
bounds = [camera_inverse @ (obj.matrix_world @ Vector(corner))
          for obj in scene.objects if obj.type in {'MESH', 'CURVE'}
          for corner in obj.bound_box]
xmin, xmax = min(v.x for v in bounds), max(v.x for v in bounds)
ymin, ymax = min(v.y for v in bounds), max(v.y for v in bounds)
aspect = scene.render.resolution_x/scene.render.resolution_y
camera.data.ortho_scale = max(xmax-xmin, (ymax-ymin)*aspect)/.88
camera.location += camera.rotation_euler.to_matrix() @ Vector(
    ((xmin+xmax)/2, (ymin+ymax)/2, 0))
for name, xyz, power, radius in [
    ('portable warm key', (-4, -5, 7), 1600, 2.4),
    ('portable soft fill', (3, -2, 5), 800, 2.0),
    ('portable rim', (1, 4, 6), 1200, 2.2),
]:
    bpy.ops.object.light_add(type='POINT', location=xyz)
    obj = bpy.context.object
    obj.name = name
    obj.data.energy = power
    obj.data.shadow_soft_size = radius

for datablocks in (bpy.data.meshes, bpy.data.curves, bpy.data.materials):
    for data in list(datablocks):
        if data.users == 0:
            datablocks.remove(data)

result = {'mesh_objects': len([o for o in scene.objects if o.type == 'MESH']),
          'curve_objects': len([o for o in scene.objects if o.type == 'CURVE']),
          'features': ['twin-pontoon bow tunnel', 'tall framed windows',
                       'raked panoramic windshield', 'recessed entry',
                       'hip roof', 'articulated davit and open bucket'],
          'front_axis': '-X', 'units': 'metres'}
