"""Rebuild the Buggy B draft geometry after the common Blender helpers.

Only mesh/curve objects in the dedicated vehicle draft are replaced; camera
and lights remain editable. This is an authored model, not retail mesh data.
The geometry uses metres, +Z up, -X forward, with separate named wheel parts.
"""
remove_matching(lambda name: True)
YELLOW = material('C buggy clean yellow enamel', (.96, .60, .015), .42, .25)
RED = material('C buggy red cockpit surround', (.64, .016, .023), .3, .26)
RUBBER = material('C buggy tire rubber', (.021, .024, .028), 0, .78)
LEATHER = material('C buggy charcoal upholstery', (.018, .021, .025), 0, .52)
STEEL = material('C buggy satin machined alloy', (.48, .52, .55), .83, .26)
DARK = material('C buggy dark suspension metal', (.06, .07, .075), .7, .32)
LENS = material('C buggy headlamp lens', (.85, .91, .94), .1, .17)
GLASS = material('C buggy transparent cyan windshield', (.25, .64, .71), 0, .12, .3)

# Continuous compound curvature joins a low nose to the wider cockpit sill.
loft_x('C tapered yellow bonnet', [
    (-1.61, 0, .48, .025, .035), (-1.54, 0, .49, .17, .08),
    (-1.33, 0, .52, .30, .15), (-1.05, 0, .57, .42, .22),
    (-.72, 0, .60, .48, .28), (-.43, 0, .61, .49, .30),
    (-.33, 0, .61, .46, .28)], YELLOW, 48, 2)
loft_x('C rounded rear engine cover', [
    (.76, 0, .65, .45, .24), (.90, 0, .66, .52, .29),
    (1.13, 0, .65, .48, .29), (1.35, 0, .60, .36, .23),
    (1.43, 0, .57, .12, .10)], YELLOW, 48, 2)

for sign in (-1, 1):
    stations = [(-.46, .29, .47, .37, .86),
                (-.30, .30, .51, .34, .90),
                (.0, .32, .55, .32, .93),
                (.40, .33, .56, .33, 1.02),
                (.73, .31, .53, .37, 1.07),
                (.98, .27, .47, .40, 1.00),
                (1.19, .24, .37, .45, .86)]
    vertices, faces = [], []
    for x, inner, outer, bottom, top in stations:
        # Rounded closed cross-section: broad external skin, hollow cockpit.
        section = [(inner, bottom+.06), (outer-.045, bottom),
                   (outer+.01, bottom+.15), (outer, top-.13),
                   (outer-.055, top), (inner+.02, top),
                   (inner, top-.07), (inner, bottom+.16)]
        vertices.extend((x, sign*y, z) for y, z in section)
    for i in range(len(stations)-1):
        for j in range(8):
            faces.append((i*8+j, i*8+(j+1)%8,
                          (i+1)*8+(j+1)%8, (i+1)*8+j))
    faces.extend([tuple(reversed(range(8))), tuple(range(48, 56))])
    if sign < 0:
        faces = [tuple(reversed(f)) for f in faces]
    fairing = mesh('C sculpted yellow side fairing', vertices, faces, YELLOW)
    mod = fairing.modifiers.new('compound body curvature', 'SUBSURF')
    mod.levels = mod.render_levels = 2
    # A raised ribbon follows the sill, not a rectangular rail.
    loft_x('C red cockpit shoulder', [
        (x, sign*(inner+outer-.035)/2, top-.025,
         (outer-inner)/2+.005, .055)
        for x, inner, outer, bottom, top in stations], RED, 24, 2)

rounded_box('C cockpit footwell', (.08, 0, .43), (1.1, .66, .14), DARK, .06)
rounded_box('C bucket seat cushion', (.35, 0, .67), (.50, .50, .16), LEATHER, .065)
back = rounded_box('C bucket seat back', (.69, 0, .96),
                   (.16, .50, .62), LEATHER, .07)
back.rotation_euler.y = -.14
for sign in (-1, 1):
    ellipsoid('C seat side bolster', (.59, sign*.225, .92),
              (.12, .075, .29), LEATHER, 24, 16)
for y in (-.14, -.07, 0, .07, .14):
    tubing('C seat stitched channel', [(.585,y,.76), (.58,y,.98),
           (.61,y,1.18)], .004, DARK, 2)

# One transverse safety hoop, with rear braces anchored in the body.
tubing('C yellow main safety hoop', [(.43,-.39,.88), (.58,-.36,1.26),
       (.64,-.29,1.36), (.64,0,1.38), (.64,.29,1.36),
       (.58,.36,1.26), (.43,.39,.88)], .036, YELLOW)
for sign in (-1, 1):
    rod('C yellow rear cage brace', (.62,sign*.33,1.29),
        (1.04,sign*.32,.88), .027, YELLOW)
    rod('C metallic cage coupling', (.57,sign*.36,1.24),
        (.595,sign*.348,1.29), .039, STEEL)

# Curved windshield with a narrow frame and a real view into the cockpit.
def glass_point(u, v):
    return (-.46+.29*v-.05*(1-u*u), u*(.44-.10*v), .88+.31*v)
vertices = [glass_point(-1+j/12, i/8) for i in range(9) for j in range(25)]
faces = [(i*25+j, i*25+j+1, (i+1)*25+j+1, (i+1)*25+j)
         for i in range(8) for j in range(24)]
mesh('C curved cyan windshield', vertices, faces, GLASS)
for v in (0, 1):
    tubing('C windshield transverse frame',
           [glass_point(-1+j/8, v) for j in range(17)], .012, YELLOW, 3)
for u in (-1, 1):
    tubing('C windshield side frame',
           [glass_point(u, i/8) for i in range(9)], .012, YELLOW, 3)
rounded_box('C recessed dashboard', (-.32, 0, .85), (.14,.64,.09), DARK, .025)
rod('C steering column', (-.25,0,.77), (.02,0,.91), .018, DARK)
direction = Vector((.27, 0, .14)).normalized()
bpy.ops.mesh.primitive_torus_add(major_segments=48, minor_segments=8,
    location=(.02,0,.91), major_radius=.13, minor_radius=.013)
wheel = assign(bpy.context.object, 'C driver steering wheel', LEATHER)
wheel.rotation_euler = direction.to_track_quat('Z','Y').to_euler()
for a in (0, 2*math.pi/3, 4*math.pi/3):
    endpoint = wheel.location + wheel.rotation_euler.to_matrix() @ Vector((.12*math.cos(a),.12*math.sin(a),0))
    rod('C steering wheel spoke', wheel.location, endpoint, .008, STEEL, 12)

# Closed radial profiles eliminate daylight between tread and metal rims.
def revolve_wheel(name, x, y, profile, mat, segments=64):
    vertices = [(x+r*math.cos(2*math.pi*i/segments), y+dy,
                 .445+r*math.sin(2*math.pi*i/segments))
                for dy, r in profile for i in range(segments)]
    faces = []
    for j in range(len(profile)):
        k = (j+1) % len(profile)
        for i in range(segments):
            n = (i+1) % segments
            faces.append((j*segments+i,j*segments+n,k*segments+n,k*segments+i))
    return mesh(name, vertices, faces, mat)

for x, label in ((-1.00, 'front'), (.98, 'rear')):
    for sign in (-1, 1):
        y = sign*.84
        prefix = 'C wheel %s %s ' % (label, sign)
        revolve_wheel(prefix+'closed rubber tire', x, y, [
            (-.15,.235),(-.165,.29),(-.16,.35),(-.13,.397),(-.08,.423),
            (.08,.423),(.13,.397),(.16,.35),(.165,.29),(.15,.235)], RUBBER, 96)
        # Broad staggered blocks, joined in one mesh per wheel.
        vertices, faces = [], []
        for row in (-1, 0, 1):
            for i in range(32):
                a = 2*math.pi*(i+.45*(row % 2))/32
                b = len(vertices)
                for relief in (-.006,.006):
                    for da, dy in ((-.077,-.043),(.077,-.043),(.077,.043),(-.077,.043)):
                        angle = a+da+dy*.38*row
                        radius = .423-.035*(abs(row*.092+dy)/.15)**3+relief
                        vertices.append((x+radius*math.cos(angle), y+row*.092+dy,
                                         .445+radius*math.sin(angle)))
                faces.extend(tuple(b+k for k in f) for f in (
                    (0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)))
        blocks = mesh(prefix+'staggered tread blocks', vertices, faces, RUBBER, False)
        bevel = blocks.modifiers.new('tread edge radius', 'BEVEL')
        bevel.width, bevel.segments = .004, 1
        for face in (-1, 1):
            revolve_wheel(prefix+'dished alloy rim', x, y, [
                (face*.12,.06),(face*.125,.17),(face*.16,.21),
                (face*.166,.236),(face*.154,.243),(face*.141,.224),
                (face*.10,.17),(face*.095,.06)], STEEL)
            rod(prefix+'hub cap', (x,y+face*.10,.445),
                (x,y+face*.15,.445), .075, DARK, 32)
            for i in range(5):
                a = i*2*math.pi/5
                xx, zz = x+.105*math.cos(a), .445+.105*math.sin(a)
                rod(prefix+'hub bolt', (xx,y+face*.125,zz),
                    (xx,y+face*.144,zz), .013, STEEL, 8)
        for dx in (-.16, .16):
            rod('C %s yellow wishbone'%label, (x+dx,sign*.41,.38),
                (x,sign*.70,.445), .024, YELLOW)
        a, b = (x+.06,sign*.43,.73), (x,sign*.68,.45)
        rod('C %s damper piston'%label, a, b, .025, STEEL)
        spring('C %s dark coil spring'%label, a, b, .062, 7, DARK)

bpy.context.view_layer.update()
bonnet = bpy.data.objects['C tapered yellow bonnet'].evaluated_get(
    bpy.context.evaluated_depsgraph_get())
def on_bonnet(x, y, offset=.009):
    hit, p, normal, face = bonnet.ray_cast(Vector((x,y,3)), Vector((0,0,-1)))
    if not hit:
        raise ValueError('Headlamp lies outside the bonnet')
    return tuple(p+normal*offset)

for sign in (-1, 1):
    # Project the full lens and perimeter onto the evaluated curved skin.
    corners = [(-1.44,sign*.10),(-1.22,sign*.26),
               (-1.10,sign*.32),(-1.14,sign*.13)]
    vertices, faces, outline = [], [], []
    for i in range(9):
        u = i/8
        for j in range(9):
            v = j/8
            xy = [((1-u)*(1-v)*corners[0][k]+u*(1-v)*corners[1][k]
                   +u*v*corners[2][k]+(1-u)*v*corners[3][k]) for k in (0,1)]
            vertices.append(on_bonnet(*xy))
    for i in range(8):
        for j in range(8):
            k = i*9+j
            faces.append((k,k+9,k+10,k+1))
    if sign < 0:
        faces = [tuple(reversed(f)) for f in faces]
    lamp = mesh('C fitted front lamp lens', vertices, faces, LENS)
    lamp.modifiers.new('lens thickness', 'SOLIDIFY').thickness = .015
    for i in range(4):
        a, b = corners[i], corners[(i+1)%4]
        outline.extend(on_bonnet(a[0]+(b[0]-a[0])*j/8,
                                a[1]+(b[1]-a[1])*j/8, .014) for j in range(8))
    tubing('C yellow headlamp surround', outline+[outline[0]], .008, YELLOW, 2)
    ellipsoid('C red rear tail lamp', (1.366,sign*.21,.69),
              (.035,.09,.045), RED, 24, 12)
tubing('C yellow nose bumper', [(-.80,-.47,.39),(-1.42,-.40,.385),
       (-1.67,-.17,.385),(-1.68,0,.385),(-1.67,.17,.385),
       (-1.42,.40,.385),(-.80,.47,.39)], .024, YELLOW, 3)

scene = bpy.context.scene
scene.camera.location = (-3.7, -4.3, 3.3)
scene.camera.rotation_euler = (Vector((-.08,0,.64))-scene.camera.location).to_track_quat('-Z','Y').to_euler()
scene.camera.data.ortho_scale = 3.95
for name, energy in (('Key', 850), ('Fill', 400), ('Rim', 450)):
    light = bpy.data.objects[name]
    light.data.type = 'POINT'
    light.data.energy = energy
    light.data.shadow_soft_size = 1.7
scene.world.use_nodes = True
scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (.35,.35,.35,1)
scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = .5
result = {'parts': len([o for o in scene.objects if o.type in {'MESH','CURVE'}]),
          'base_triangles': triangle_count(), 'forward': '-X',
          'wheel_centres': [[x,y,.445] for x in (-1,.98) for y in (-.84,.84)]}
