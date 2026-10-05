"""Concept-B blue scooter refinement; append after common helpers."""
BLUE = material("Scooter B blue enamel", (.045, .43, .70), .50, .22)
DARK = material("Scooter B charcoal leather", (.035, .038, .044), 0, .68)
RUBBER = material("Scooter B tire rubber", (.022, .024, .028), 0, .78)
STEEL = material("Scooter B polished stainless trim", (.65, .69, .71), .88, .19)
HUB = material("Scooter B dark alloy hub", (.11, .12, .13), .61, .32)
LENS = material("Scooter B headlamp lens", (.75, .84, .88), .04, .08)
RED = material("Scooter B red tail lamp", (.75, .025, .025), .05, .12)
remove_matching(lambda n: n.startswith("B "))

obsolete = {
    "Tall iconic blue leg shield", "Bulbous blue rear motor cowling",
    "Blue formed-metal front mudguard", "Painted front lower apron",
    "Long two passenger saddle", "Circular blue lamp pod",
    "Round transparent headlight", "Chrome circular headlamp bezel",
    "Rear drive housing", "Chrome side trim",
}
remove_matching(
    lambda n: any(n == a or n.startswith(a + ".") for a in obsolete)
    or "rubber tire" in n or "black sidewall" in n or "steel hub" in n)

# Tall swept step-through front: stations follow the side silhouette of the
# approved concept instead of being an upright ellipsoid.
stations = [
    (.35, -.34, .21, .29),
    (.45, -.46, .25, .33),
    (.66, -.53, .23, .36),
    (.90, -.54, .18, .34),
    (1.11, -.53, .13, .28),
    (1.24, -.52, .09, .19),
]
sides = 64
vertices = []
for z, xc, rx, ry in stations:
    for i in range(sides):
        a = i*2*math.pi/sides
        vertices.append((xc + rx*math.cos(a), ry*math.sin(a), z))
faces = []
for k in range(len(stations)-1):
    for i in range(sides):
        j = (i+1) % sides
        faces.append((k*sides+i, k*sides+j,
                      (k+1)*sides+j, (k+1)*sides+i))
faces.append(tuple(reversed(range(sides))))
faces.append(tuple((len(stations)-1)*sides+i for i in range(sides)))
shield = mesh("B sculpted swept leg shield", vertices, faces, BLUE)
sub = shield.modifiers.new("continuous pressed-metal curvature", "SUBSURF")
sub.levels = 2
sub.render_levels = 2

# Large smooth rear motor cowling surrounds, but does not fill, the rear wheel.
ellipsoid("B pressed blue rear enclosure", (.61, 0, .67),
          (.58, .41, .35), BLUE, 96, 48)
rounded_box("B black two-rider saddle", (.46, 0, 1.04),
            (1.01, .46, .17), DARK, .085, 10)
rounded_box("B saddle central strap", (.54, 0, 1.133),
            (.055, .42, .012), HUB, .009, 2)
for sign in (-1, 1):
    tubing("B seat stitched edge", [
        (-.035, sign*.19, 1.01), (.15, sign*.22, 1.10),
        (.60, sign*.22, 1.11), (.94, sign*.18, 1.07)],
        .005, HUB)

# Arc-shaped steel fender. It is part of the body, not a detached oval slab.
x0, z0 = -.80, .34
arc = []
steps = 40
for i in range(steps+1):
    angle = math.radians(5 + i*170/steps)
    arc.append((x0+.375*math.cos(angle), z0+.375*math.sin(angle)))
verts = []
for x, z in arc:
    verts.extend([(x, -.215, z), (x, .215, z),
                  (x0+(x-x0)*.86, .215, z0+(z-z0)*.86),
                  (x0+(x-x0)*.86, -.215, z0+(z-z0)*.86)])
fender_faces = []
for i in range(steps):
    b, c = 4*i, 4*(i+1)
    for j in range(4):
        fender_faces.append((b+j, c+j, c+(j+1)%4, b+(j+1)%4))
fender = mesh("B formed semicircular front fender", verts, fender_faces, BLUE)
solid = fender.modifiers.new("fender sheet metal", "SOLIDIFY")
solid.thickness = .014
bevel = fender.modifiers.new("soft fender edges", "BEVEL")
bevel.width = .018
bevel.segments = 3
tubing("B chrome fender centre ridge", [
    (x0+.37*math.cos(math.radians(5+i*170/12)), 0,
     z0+.37*math.sin(math.radians(5+i*170/12)))
    for i in range(13)], .009, STEEL)

# Wide tires, alloy rims, exposed front suspension coils and swing arm.
for x, label in ((-.80, "front"), (.79, "rear")):
    tire("B scooter "+label, x, 0, .34, .34, .22,
         RUBBER, STEEL, HUB, rim_ratio=.43)
for sign in (-1, 1):
    a = (-.67, sign*.115, .72)
    b = (-.80, sign*.115, .36)
    rod("B fork inner slider", a, b, .025, STEEL)
    spring("B visible front fork spring", (-.68, sign*.14, .67),
           (-.78, sign*.14, .41), .034, 7, STEEL)
    rod("B rear swing arm", (.49, sign*.22, .49),
        (.79, sign*.13, .34), .031, HUB)
    spring("B exposed rear damper", (.67, sign*.20, .68),
           (.76, sign*.18, .41), .031, 6, STEEL)

# Reflective seams trace the step-through and removable rear shell.
for sign in (-1, 1):
    tubing("B chrome step-through outline", [
        (-.30, sign*.29, .95), (-.27, sign*.33, .66),
        (-.18, sign*.29, .39), (.18, sign*.29, .38),
        (.47, sign*.36, .42)], .014, STEEL)
    tubing("B curved rear panel seam", [
        (.15, sign*.35, .77), (.40, sign*.403, .87),
        (.78, sign*.385, .87), (1.03, sign*.31, .72)],
        .009, STEEL)
    ellipsoid("B rear panel screw", (.20, sign*.35, .69),
              (.014, .009, .014), STEEL, 24, 12)
    ellipsoid("B amber turn indicator", (-.48, sign*.31, .92),
              (.033, .014, .025),
              material("Scooter B amber signal", (.80, .37, .025), 0, .20),
              32, 16)

# Large round headlamp with dark optical centre and polished bezel.
ellipsoid("B deep blue headlamp body", (-.60, 0, 1.36),
          (.19, .21, .20), BLUE, 64, 32)
bpy.ops.mesh.primitive_cylinder_add(
    vertices=64, radius=.152, depth=.028,
    location=(-.779, 0, 1.36), rotation=(0, math.pi/2, 0))
assign(bpy.context.object, "B round lamp lens", LENS)
bpy.ops.mesh.primitive_torus_add(
    major_segments=64, minor_segments=12,
    location=(-.796, 0, 1.36), rotation=(0, math.pi/2, 0),
    major_radius=.150, minor_radius=.016)
assign(bpy.context.object, "B machined chrome headlamp bezel", STEEL)
ellipsoid("B dark central lamp optic", (-.801, 0, 1.36),
          (.013, .065, .065), HUB, 32, 16)

# Brake levers, passenger grip, footboard ribs and rear running lamp.
for sign in (-1, 1):
    tubing("B polished brake lever", [
        (-.56, sign*.35, 1.33), (-.45, sign*.48, 1.30),
        (-.43, sign*.54, 1.27)], .012, STEEL)
    tubing("B curved rear passenger grab", [
        (.93, sign*.24, 1.08), (1.09, sign*.24, 1.10),
        (1.14, sign*.12, 1.14)], .018, STEEL)
for i in range(5):
    x = -.18+i*.10
    rounded_box("B nonslip step rib", (x, 0, .432),
                (.024, .40, .008), HUB, .004, 2)
ellipsoid("B rear red marker light", (1.17, 0, .81),
          (.025, .12, .045), RED, 32, 16)

scene = bpy.context.scene
scene.camera.data.ortho_scale = 3.20
scene.render.film_transparent = False
scene.world.use_nodes = True
scene.world.node_tree.nodes["Background"].inputs["Color"].default_value = (
    .56, .56, .55, 1)
scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = .47
for name, energy in (("Key", 520), ("Fill", 300)):
    light = bpy.data.objects.get(name)
    if light and light.type == "LIGHT":
        light.data.energy = energy
result = {"mesh_parts": len([o for o in bpy.data.objects if o.type == "MESH"]),
          "base_triangles": triangle_count(),
          "note": "Shield subdivision evaluated in GLB export"}
