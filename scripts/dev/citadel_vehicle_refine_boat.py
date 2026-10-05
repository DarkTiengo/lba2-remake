"""Concept-B Citadel working-boat refinement; append after common helpers."""
CREAM = material("Boat B warm cream metal", (.86, .66, .34), .43, .31)
RED = material("Boat B enamel red", (.60, .012, .018), .18, .40)
TEAL = material("Boat B deep teal bottom", (.035, .29, .29), .43, .32)
GLASS = material("Boat B real transparent turquoise glass",
                 (.055, .24, .28), .04, .17, .52)
STEEL = material("Boat B satin stainless railing", (.53, .57, .58), .84, .23)
CRANE = material("Boat B graphite crane steel", (.07, .075, .09), .70, .28)
WOOD = material("Boat B sealed deck timber", (.34, .19, .085), 0, .53)
WHITE = material("Boat B safety-ring white", (.92, .91, .85), .08, .32)
remove_matching(lambda n: n.startswith("B "))

remove_exact = {
    "Red roof left slope", "Red roof right slope", "Roof ridge",
    "Angled crane boom", "Dark stern crane column", "Crane cable",
    "Small hanging ochre cargo bucket",
    "Tall side window top frame", "Transparent blue-green cabin side window",
    "Transparent front wheelhouse glass", "Cabin vertical window post",
    "Thin red painted hull accent", "Red hull belt",
}
remove_matching(lambda n: any(n == a or n.startswith(a + ".")
                              for a in remove_exact))

# Make the existing displacement hull more finely curved without changing the
# model's draft/scale. The top red belt then follows its raked bow and stern.
hull = bpy.data.objects.get("Cream faceted-curved displacement hull")
if hull:
    for old in list(hull.modifiers):
        if old.type == "SUBSURF" and old.name.startswith("denser smoothed hull"):
            hull.modifiers.remove(old)
    for face in hull.data.polygons:
        face.use_smooth = True
    mod = hull.modifiers.new("denser smoothed hull", "SUBSURF")
    mod.levels = 2
    mod.render_levels = 2
    hull.data.materials.clear()
    hull.data.materials.append(CREAM)

for sign in (-1, 1):
    stations = [
        (-2.03, .24), (-1.82, .72), (-1.43, .96),
        (-.6, 1.04), (.64, 1.03), (1.53, .92), (1.91, .67)]
    vertices = []
    for x, y in stations:
        vertices.extend([(x, sign*y, .83), (x, sign*y, 1.04)])
    faces = [(2*i, 2*i+1, 2*i+3, 2*i+2)
             for i in range(len(stations)-1)]
    band = mesh("B continuous red rub rail", vertices, faces, RED)
    band.modifiers.new("red rail thickness", "SOLIDIFY").thickness = .038
    b = band.modifiers.new("rounded rail ends", "BEVEL")
    b.width = .02
    b.segments = 3

# Low-profile hipped canopy: broad roof-top, raked windshield end,
# rounded edge lip. It replaces the previous flat doubled panels.
roof_vertices = [
    (-1.46, -.96, 2.18), (-1.46, .96, 2.18),
    (1.30, -.96, 2.18), (1.30, .96, 2.18),
    (-1.04, -.68, 2.43), (-1.04, .68, 2.43),
    (1.02, -.68, 2.43), (1.02, .68, 2.43),
]
roof_faces = [
    (0, 2, 6, 4), (1, 5, 7, 3), (0, 4, 5, 1),
    (2, 3, 7, 6), (4, 6, 7, 5), (0, 1, 3, 2)]
roof = mesh("B one-piece hipped red cabin roof",
            roof_vertices, roof_faces, RED, smooth=False)
solid = roof.modifiers.new("roof shell thickness", "SOLIDIFY")
solid.thickness = .045
bevel = roof.modifiers.new("formed roof seam", "BEVEL")
bevel.width = .035
bevel.segments = 4
roof.modifiers.new("weighted roof normals", "WEIGHTED_NORMAL")
tubing("B red roof perimeter", [
    (-1.46, -.96, 2.18), (-1.52, -.55, 2.17),
    (-1.54, 0, 2.17), (-1.52, .55, 2.17),
    (-1.46, .96, 2.18), (.2, .98, 2.18), (1.30, .96, 2.18)],
    .018, RED)

# Large, actually translucent wheelhouse panes with trim matching the image.
# The front face is raked back toward the roof and split by one mullion.
for sign in (-1, 1):
    y0 = 0 if sign < 0 else .005
    y1 = sign*.76
    vertices = [
        (-1.40, y0, 1.62), (-1.40, y1, 1.62),
        (-1.05, sign*.72, 2.12), (-1.05, y0, 2.12)]
    mesh("B raked panoramic windscreen", vertices, [(0, 1, 2, 3)],
         GLASS, smooth=False)
    rod("B windscreen lower sill", vertices[0], vertices[1], .018, STEEL)
    rod("B windscreen outside edge", vertices[1], vertices[2], .018, STEEL)
    rod("B windscreen upper sill", vertices[2], vertices[3], .018, STEEL)
    for left, right in ((-1.01, -.22), (-.14, .59)):
        yy = sign*.785
        corners = [
            (left, yy, 1.61), (right, yy, 1.61),
            (right, yy, 2.10), (left, yy, 2.10)]
        mesh("B clear side window", corners, [(0, 1, 2, 3)],
             GLASS, smooth=False)
        tubing("B rounded side window frame", [
            corners[0], corners[1], corners[2], corners[3], corners[0]],
            .017, STEEL)
    rod("B cream cabin B pillar", (-.17, sign*.80, 1.60),
        (-.17, sign*.80, 2.16), .035, CREAM)
rod("B front screen centre mullion",
    (-1.40, 0, 1.62), (-1.05, 0, 2.12), .020, STEEL)
rounded_box("B brown starboard access door", (.84, -.813, 1.81),
            (.31, .026, .57), WOOD, .022, 3)
ellipsoid("B polished access-door latch", (.94, -.836, 1.79),
          (.014, .010, .020), STEEL, 24, 12)

# Functional aft davit, lifting cable, open cargo bucket and hydraulic ram.
def beam(name, a, b, width, depth, mat):
    a, b = Vector(a), Vector(b)
    direction = b-a
    obj = rounded_box(name, (a+b)/2, (width, depth, direction.length),
                      mat, min(width, depth)*.20, 4)
    obj.rotation_euler = direction.to_track_quat("Z", "Y").to_euler()
    return obj

beam("B crane pedestal", (1.49, .50, 1.15), (1.49, .50, 2.05),
     .18, .21, CRANE)
beam("B crane rising arm", (1.49, .50, 2.00), (1.87, .50, 2.84),
     .19, .19, CRANE)
beam("B crane horizontal jib", (1.86, .50, 2.84), (2.42, .50, 2.84),
     .16, .16, CRANE)
rod("B hydraulic ram", (1.47, .40, 1.67), (1.79, .40, 2.55),
    .034, STEEL)
rod("B bucket steel cable", (2.41, .50, 2.82), (2.41, .50, 2.36),
    .013, STEEL)
for x, z in ((1.49, 2.02), (1.87, 2.82)):
    ellipsoid("B crane pivot pin", (x, .37, z),
              (.075, .025, .075), STEEL, 32, 16)
bucket_vertices = [
    (2.23, .30, 2.26), (2.59, .30, 2.26),
    (2.59, .70, 2.26), (2.23, .70, 2.26),
    (2.28, .35, 2.02), (2.54, .35, 2.02),
    (2.54, .65, 2.02), (2.28, .65, 2.02)]
mesh("B open cargo bucket", bucket_vertices, [
    (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6),
    (3, 0, 4, 7), (4, 5, 6, 7)], WOOD, smooth=False)
for y in (.31, .69):
    rod("B bucket lifting strop", (2.41, .50, 2.36),
        (2.41, y, 2.25), .010, STEEL)

# Bow timber strips, windowsill, red ring markings and practical railing.
for i in range(5):
    x = -1.91 + i*.18
    rounded_box("B individual bow deck plank", (x, 0, 1.101),
                (.16, 1.70, .014), WOOD, .009, 2)
for sign in (-1, 1):
    tubing("B bow stainless safety rail", [
        (-2.04, 0, 1.29), (-1.94, sign*.48, 1.29),
        (-1.77, sign*.86, 1.29), (-1.32, sign*.93, 1.29)],
        .017, STEEL)

scene = bpy.context.scene
scene.camera.data.ortho_scale = 6.4
scene.render.film_transparent = False
scene.world.use_nodes = True
scene.world.node_tree.nodes["Background"].inputs["Color"].default_value = (
    .56, .56, .55, 1)
scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = .48
for name, energy in (("Key", 700), ("Fill", 380)):
    light = bpy.data.objects.get(name)
    if light and light.type == "LIGHT":
        light.data.energy = energy
result = {"mesh_parts": len([o for o in bpy.data.objects if o.type == "MESH"]),
          "base_triangles": triangle_count(),
          "note": "Hull subdivision evaluated in GLB export"}
