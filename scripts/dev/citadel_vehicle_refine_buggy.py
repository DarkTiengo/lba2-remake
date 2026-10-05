"""Concept-B buggy refinement; run after citadel_vehicle_refine_common.py."""
Y = material("Buggy B enamel yellow", (0.94, 0.52, 0.012), .62, .24)
R = material("Buggy B enamel red", (0.69, 0.025, 0.035), .57, .25)
K = material("Buggy B rubber", (0.025, 0.027, 0.031), 0, .77)
S = material("Buggy B satin alloy", (0.51, 0.54, 0.55), .83, .26)
D = material("Buggy B dark hub", (0.09, 0.10, 0.11), .58, .37)
L = material("Buggy B lamp lens", (0.84, 0.91, 0.88), .08, .13)
GLASS = material("Transparent cyan glazing", (.12, .47, .55), .05, .11,
                 .32)
remove_matching(lambda n: n.startswith("B "))

obsolete = {
    "Continuous tapered nose shell", "Rounded rear engine deck",
    "Front headlamp", "Recessed nose lamp", "Yellow cockpit side shell",
    "Red upper side band", "Machined wheel rim", "Hub bolt",
}
remove_matching(
    lambda n: any(n == a or n.startswith(a + ".") for a in obsolete)
    or any(w in n.lower() for w in (
        " tire", " rubber sidewall", " metal hub")))

# A pointed long-nose shell and rounded engine body replace the old capsules.
# 64 radial sections plus subdivision preserve the distinctive Citadel profile.
loft_x("B aerodynamic tapered nose", [
    (-1.58, 0, .56, .035, .045),
    (-1.48, 0, .56, .20, .10),
    (-1.25, 0, .57, .34, .15),
    (-.95, 0, .58, .46, .18),
    (-.58, 0, .60, .49, .19),
    (-.26, 0, .61, .45, .17),
], Y, 64, 2)
loft_x("B integrated rear engine cowling", [
    (.46, 0, .58, .45, .23),
    (.70, 0, .62, .54, .29),
    (1.00, 0, .64, .53, .31),
    (1.23, 0, .63, .40, .28),
    (1.38, 0, .61, .14, .13),
], Y, 64, 2)

# Real body panels: hollow cockpit rather than exposed thin colored pipes.
def side_panel(side, red=False):
    stations = [
        (-.34, .29, .46, .46, .77),
        (-.12, .30, .52, .45, .83),
        (.37, .33, .56, .43, .96),
        (.74, .30, .54, .46, .97),
        (1.12, .28, .43, .50, .79),
    ]
    vertices = []
    for x, yi, yo, bottom, top in stations:
        yi, yo = side*yi, side*yo
        if red:
            top += .018
            bottom = top - .115
            yo += side*.016
            yi += side*.006
        vertices.extend([
            (x, yo, bottom), (x, yo, top),
            (x, yi, top), (x, yi, bottom)])
    faces = []
    for i in range(len(stations)-1):
        b = 4*i
        c = 4*(i+1)
        for j in range(4):
            faces.append((b+j, c+j, c+(j+1)%4, b+(j+1)%4))
    faces.extend([(3, 2, 1, 0),
                  tuple(4*(len(stations)-1)+j for j in range(4))])
    obj = mesh("B red sculpted shoulder" if red else "B formed side fairing",
               vertices, faces, R if red else Y)
    bevel = obj.modifiers.new("rounded sheet edges", "BEVEL")
    bevel.width = .028 if red else .04
    bevel.segments = 3
    obj.modifiers.new("weighted normals", "WEIGHTED_NORMAL")
for sign in (-1, 1):
    side_panel(sign)
    side_panel(sign, True)

# Make the safety cage a recognizable rounded yellow metal hoop.
remove_matching(lambda n: n.startswith("Yellow cage ")
                or n == "Yellow rollbar crossmember")
for sign in (-1, 1):
    tubing("B roll cage side", [
        (.25, sign*.47, .82), (.31, sign*.45, 1.20),
        (.45, sign*.44, 1.34), (.77, sign*.40, 1.34),
        (.95, sign*.38, 1.13), (1.10, sign*.37, .79)], .045, Y)
tubing("B roll cage crown", [
    (.43, -.44, 1.34), (.39, 0, 1.37), (.43, .44, 1.34)], .042, Y)

# Wide, visibly treaded rubber and machined exposed hubs.
for axle_x, label in ((-.99, "front"), (.94, "rear")):
    for sign in (-1, 1):
        tire("B %s %s" % (label, sign), axle_x, sign*.88, .41,
             .37, .28, K, S, D, rim_ratio=.42)
        if label == "front":
            a = (axle_x+.10, sign*.43, .67)
            b = (axle_x+.02, sign*.73, .42)
        else:
            a = (axle_x-.10, sign*.45, .73)
            b = (axle_x-.01, sign*.73, .42)
        spring("B visible coil shock", a, b, .055, 7, S)
        rod("B dark damper", a, b, .018, D)

# Recessed wedge-shaped lenses in the outer nose, not central white dots.
for sign in (-1, 1):
    outer = [
        (-1.42, sign*.24, .57), (-1.19, sign*.41, .650),
        (-1.01, sign*.49, .605), (-1.12, sign*.46, .530)]
    inner = [
        (-1.39, sign*.26, .576), (-1.19, sign*.435, .635),
        (-1.05, sign*.515, .595), (-1.13, sign*.485, .545)]
    mesh("B wedge front lamp dark housing", outer,
         [(0, 1, 2, 3)], D, smooth=False)
    mesh("B wedge front lamp clear lens", inner,
         [(0, 1, 2, 3)], L, smooth=False)
tubing("B yellow front crash hoop", [
    (-.77, -.46, .47), (-1.36, -.45, .44),
    (-1.62, -.28, .43),
    (-1.64, 0, .42), (-1.62, .28, .43),
    (-1.36, .45, .44), (-.77, .46, .47)],
    .018, Y)

# Small but physical cockpit hardware and rear deck panel breaks.
rounded_box("B instrument cluster", (-.28, 0, .85),
            (.22, .37, .055), D, .025)
for sign in (-1, 1):
    tubing("B rear cowling seam", [
        (.68, sign*.46, .76), (.91, sign*.48, .88),
        (1.16, sign*.39, .80)], .009, D)
    ellipsoid("B rear red running lamp", (1.34, sign*.20, .66),
              (.02, .09, .055), R, 32, 16)

scene = bpy.context.scene
scene.camera.data.ortho_scale = 4.05
scene.render.film_transparent = False
scene.world.use_nodes = True
scene.world.node_tree.nodes["Background"].inputs["Color"].default_value = (
    .56, .56, .55, 1)
scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = .45
for name, energy in (("Key", 560), ("Fill", 340), ("Rim", 220)):
    light = bpy.data.objects.get(name)
    if light and light.type == "LIGHT":
        light.data.energy = energy
result = {"mesh_parts": len([o for o in bpy.data.objects if o.type == "MESH"]),
          "base_triangles": triangle_count(),
          "note": "Subdivision on aerodynamic body and rear cowl is evaluated on export"}
