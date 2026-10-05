# Citadel: original object material pack

These PNGs are new project artwork; none is an atlas or texture exported
from the retail game. The `*-source.png` files preserve the generated artwork.
The corresponding 512 × 512 PNGs are repeatable, game-ready albedo tiles made
by `scripts/dev/build_citadel_metal_tiles.py`. That script also stacks the
yellow, red, galvanized, bark, foliage, neutral and panel painted-steel tiles into
`vehicle-materials.png`, embedded into the GPU renderer at build time.

| Tile | Intended use | Suggested material response |
| --- | --- | --- |
| `vehicle-painted-steel.png` | Teal paint source for later Citadel vehicles | Metallic substrate under worn paint; medium roughness |
| `vehicle-yellow-painted-steel.png` | Retained color variant for later vehicle materials | Weathered ochre paint; medium roughness |
| `vehicle-red-painted-steel.png` | Retained color variant for later vehicle materials | Weathered oxblood paint; medium roughness |
| `vehicle-neutral-painted-steel.png` | Retained neutral source (runtime uses the clean panel tile for frames) | No longer selected at runtime |
| `vehicle-panel-painted-steel.png` | Buggy, boat, bins, railings and lamp frames | Clean satin-metal grain; each object's original palette supplies its color |
| `utility-galvanized-steel.png` | Retained galvanized source (runtime uses the clean panel tile for fixtures) | No longer selected at runtime |
| `tree-bark.png` | Citadel and post-storm Citadel tree trunks | Fine vertical ridges; matte |
| `tree-foliage.png` | Citadel and post-storm Citadel crowns and bushes | Overlapping leaves; matte |

The teal and galvanized source artwork was generated from text alone with the
built-in image generator, without retail art as input. The yellow and red
sources are edits of the teal source: their prompts requested only a change
of painted base color to warm golden ochre and deep muted oxblood,
respectively, preserving its fine grain, restrained scuffs and sparse chips.
The neutral steel source was generated independently from text alone by the
built-in image generator. No retail image was given to the generator. Original prompts:

The current panel-steel source was generated from text alone using the built-in
image generator. Its smooth silver-gray satin grain replaces the earlier aged,
scuffed version. Shader layers 2 (fixtures), 5 (frames) and 6 (vehicles) all
sample this clean tile, then retain each object's original palette colour. The
neutral and galvanized images remain in the atlas for asset compatibility but
are not selected by those runtime materials.

- Vehicle: “Create one brand-new square seamless tileable albedo texture for painted sheet steel used on small retro-futurist island vehicles. Stylized realism, muted desaturated ocean teal paint, fine orange-peel grain, sparse subtle scratches exposing charcoal primer and tiny oxidized chips, gentle broad value variation. Orthographic material swatch, even lighting, no vehicle, panels, rivets, perspective, border, text, logos, trademarks, watermark, or existing-game reference.”
- Neutral steel: “One entirely new square seamless tileable albedo texture of neutral light-gray painted sheet steel for retro-adventure vehicle parts, designed to be tinted in software to each object's original color. Restrained photorealism, orthographic flat material swatch, uniform micro-detail, diffuse even light, subtle orange-peel grain, faint brushed metal undercoat, sparse tiny wear scratches and pinprick chips, moderate roughness, nearly achromatic stable midtones. No vehicle, object, panels, seams, rivets, text, symbols, logo, watermark, perspective, background scene, colored paint, strong dark patches or high-contrast grunge.”
- Current panel steel: “Clean light silver-gray satin painted steel, fine even brushed grain, subtle soft highlights, nearly achromatic and suitable for tinting by the original palette. Seamless orthographic material swatch; no wear, rust, scratches, chips, grime, panels, seams, objects, text or perspective.”
- Utility: “Create one brand-new square seamless tileable albedo texture of aged galvanized steel for outdoor trash bins, utility boxes, and small street fixtures on a whimsical retro-futurist island. Stylized realism, matte zinc spangle, gray-blue patina, fine abrasions, sparse tiny rust freckles, modest contrast. Orthographic material swatch, even lighting, no object, seams, bolts, panels, perspective, border, text, logos, trademarks, watermark, or existing-game reference.”
- Bark: “Generate one brand-new square seamless tileable game albedo texture, photorealistic but restrained, of weathered bark for the trunks of stylized Citadel Island trees. Fine vertical fibrous ridges, subtle warm grey-brown and muted umber variation, a few natural hairline cracks, no large knots or high-contrast scars. Strict orthographic flat material swatch, diffuse even lighting with no directional shadows or specular highlights, uniform scale across whole image. No tree silhouette, branch, leaves, background scene, perspective, border, text, logo, watermark, or reference to a copyrighted game. Asset must be usable as a repeating tile on low-poly 3D trees.”
- Foliage: “Generate one brand-new square seamless tileable game albedo texture, photorealistic but gently stylized, for the living leafy canopy of Citadel Island trees and shrubs. Dense overlapping small green leaves with naturally irregular clusters, muted olive-to-deep-forest-green hues, a few warmer sunlit leaf edges, restrained contrast and fine organic detail. Strictly orthographic flat material swatch, even diffuse lighting, uniform scale, no hard cast shadows, no visible sky or background gaps, no large individual branches, no isolated tree silhouette, no perspective, no border, no text, logo, watermark, or existing-game reference. It will be blended with the original low-poly leaf colors rather than defining a new silhouette.”

The parked buggy (Citadel model 85, near Dino-Fly) keeps its retail geometry,
placement and collision. With the GPU renderer and `gfx_texturedetail 2`,
the original model's yellow and red paint and gray chassis receive a new
palette-tinted metal overlay; wheels, cabin and other surfaces keep their authored colors. The same
palette selection reaches the quest buggy when parked or driven on Citadel.
The harbour boat uses the panel steel on its untextured painted hull, roof,
trim and exhaust. In harbour cube 43 the visible boat is actor 10, body 165;
decor model 56 is its flag-hidden alternative, and both paths are covered.
The overlay keeps the boat's own cream, red and dark hues; timber and
image-mapped details stay original. Its palette-only gray window polygons are
drawn as translucent blue-green glass over a simple recessed cabin view with
a seat and rail. This interior is a render-only backdrop, not navigable 3D
geometry. The glazing is limited to those window polygons, including the
smaller door pane stored at a lower model-space height.
Modes 0–1 and the classic renderer keep the complete original appearance. The
galvanized tile also covers non-luminous, untextured surfaces of Citadel street
lamp model 26 and street-object model 28; the lamp's gold frame keeps its gold
through neutral steel. Identified railing and bench frames (models 27 and
51–54) receive neutral steel only on gray untextured parts; red bench slats,
glass and existing graphics remain original. Tree bodies identified in
`TREES.CPP` use bark on untextured trunk
polygons and foliage on leaf polygons; bushes have foliage only. This includes
the post-storm `citabau` file. Texture detail modes 0–1 and the classic renderer
keep the complete original appearance. No broad palette substitution is used,
so stonework and characters stay unaffected.

The optional GPU draw also adds render-only triangles to curved decor throughout
the `citadel` and `citabau` areas. It uses each model's own vertex normals:
smoothly shaded curved triangles split into four; flat walls, signs, floors and
hard-edged panels stay planar. The parked buggy 85 additionally rounds its
painted facets, splitting each near-view triangle into nine (four in the
distance). Both near and retained distant decors are covered, with a mesh
budget to avoid overloading large models. Original corners, placement,
collision, silhouette and the classic renderer are unchanged. This improves
the existing models but is not a full high-poly remodel.
