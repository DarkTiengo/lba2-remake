"""Finish the existing refined Scooter B scene; run after the common helpers.

Preserves the editable wheels, frame and suspension. The GLB uses constant
Principled materials so the same colors and roughness reach the game.
"""
BLUE = material('Scooter B blue enamel', (.045, .30, .52), .35, .26)
STEEL = material('Scooter B polished stainless trim', (.52, .57, .61), .85, .23)
DARK = material('Scooter B charcoal leather', (.025, .028, .032), 0, .62)
HUB = material('Scooter B dark alloy hub', (.055, .065, .075), .65, .30)
remove_matching(lambda n: n.startswith((
    'Subtle seat seam', 'Rear motor cooling slot', 'Front shield satin',
    'Passenger grab rail crossbar', 'Rear steel grab rail',
    'B curved rear passenger grab', 'B curved rear panel seam',
    'B formed semicircular front fender', 'B chrome fender centre ridge',
    'B saddle central strap', 'B seat stitched edge', 'B rear panel screw',
    'Amber side turn signal', 'Under-seat bridge')))

# Rounded, deeper rear pressed-metal shell meets the saddle and wheel cover.
shell = bpy.data.objects['B pressed blue rear enclosure']
shell.location.z = .65
shell.scale = (.57, .37, .40)
seat = bpy.data.objects['B black two-rider saddle']
seat.location.z = 1.06
seat.dimensions = (1.05, .53, .18)
rounded_box('C saddle leather retaining strap', (.55, 0, 1.154),
            (.055, .48, .015), DARK, .007, 3)
for sign in (-1, 1):
    tubing('C saddle stitched piping', [(-.06, sign*.19, 1.08),
        (.10, sign*.255, 1.125), (.74, sign*.255, 1.125),
        (.97, sign*.19, 1.08)], .006, DARK)
    tubing('C rear passenger loop', [(.91, sign*.24, 1.04),
        (1.10, sign*.24, 1.12), (1.14, sign*.17, 1.25),
        (1.14, 0, 1.27)], .018, STEEL)

# A convex transverse profile keeps the fender from reading as a flat slab.
verts, faces = [], []
for i in range(49):
    a = math.radians(4+i*172/48)
    for j in range(17):
        t = -1+j/8
        radius = .385 + .04*(1-t*t)
        verts.append((-.8+radius*math.cos(a), t*.185,
                      .34+radius*math.sin(a)-.028*t*t))
for i in range(48):
    for j in range(16):
        k = i*17+j
        faces.append((k, k+1, k+18, k+17))
fender = mesh('C crowned pressed-metal front fender', verts, faces, BLUE)
fender.modifiers.new('sheet thickness', 'SOLIDIFY').thickness = .012
tubing('C front fender chrome spine', [
    (-.8+.429*math.cos(math.radians(5+i*170/16)), 0,
     .34+.429*math.sin(math.radians(5+i*170/16)))
    for i in range(17)], .010, STEEL)
tubing('C front shield chrome spine', [(-.56,0,.39),
    (-.68,0,.50),(-.74,0,.68),(-.72,0,.88),
    (-.65,0,1.08),(-.59,0,1.23)], .019, STEEL)

# The step-through frame supports the seat instead of a floating blue block.
loft_x('C dark pressed seat support', [
    (.10,0,.43,.22,.04), (.18,0,.51,.20,.11),
    (.26,0,.70,.19,.22), (.40,0,.84,.22,.20)], HUB,
    sides=32, subdivision=1)
for o in bpy.data.objects:
    if 'alloy rim' in o.name:
        o.data.materials.clear()
        o.data.materials.append(HUB)
    if o.name.startswith('Black handle grip'):
        o.scale.z *= 1.35

scene = bpy.context.scene
scene.camera.data.ortho_scale = 3.05
for name, energy in (('Key', 600), ('Fill', 280)):
    light = bpy.data.objects[name]
    light.data.type = 'POINT'
    light.data.energy = energy
    light.data.shadow_soft_size = 1.8
result = {'parts': len([o for o in scene.objects if o.type in {'MESH','CURVE'}]),
          'triangles': triangle_count()}

# Close the tire sidewalls between tread shoulders and hub disks.
RUBBER = material('Scooter B tire rubber', (.022, .024, .028), 0, .78)
remove_matching(lambda n: n.startswith(('B chrome step-through outline',
                                        'Chrome step perimeter')))
for x, label in ((-.8, 'front'), (.79, 'rear')):
    bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=.278, depth=.21,
        location=(x, 0, .34), rotation=(math.pi/2, 0, 0))
    assign(bpy.context.object, 'C '+label+' rubber sidewall', RUBBER)
for sign in (-1, 1):
    tubing('C chrome footboard edge', [(-.27, sign*.24, .44),
        (-.16, sign*.255, .41), (.19, sign*.255, .41),
        (.35, sign*.25, .44)], .017, STEEL)
