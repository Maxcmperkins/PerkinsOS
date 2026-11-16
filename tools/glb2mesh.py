"""Convert a .glb aircraft into the wallpaper's mesh file (.mesh).

Runs inside Blender, which reads glTF and its textures for us:

  blender --background --python tools/glb2mesh.py -- IN.glb OUT.mesh \
      --length 15.96 --nose -y --up +z \
      --nozzles=-0.5,0,-8:0.5,0,-8 --nozzle-radius 0.44

  blender --background --python tools/glb2mesh.py -- IN.glb --info [same options]
      prints the model's size and where each material sits, in the
      wallpaper's coordinates, to help choose --nose, --up and --nozzles

What it does:
  - turns and scales the model into the wallpaper's coordinates: metres,
    x right, y up, z towards the nose, centred on its bounding box, scaled
    so it is --length metres nose to tail
  - gives every vertex one flat colour, read from the material's texture
    at that vertex (the wallpaper's renderer has no textures)
  - marks see-through materials (canopies) as glass, so model.c gives
    them a glint

File format, little-endian, read by load_mesh() in shell/wallpaper/model.c:
  char     magic[8]          "PKMESH1\\0"
  uint32   vertex_count, triangle_count
  float    nozzles[2][3]     exhaust exits, for the glow
  float    nozzle_radius
  vertex_count   x { float position[3]; float normal[3]; uint8 r, g, b, kind }
  triangle_count x { uint32 a, b, c }
"""
import argparse
import struct
import sys

import bpy
import numpy as np
from mathutils import Vector

KIND_PAINT, KIND_GLASS = 0, 1
AXES = {"+x": (1, 0, 0), "-x": (-1, 0, 0), "+y": (0, 1, 0),
        "-y": (0, -1, 0), "+z": (0, 0, 1), "-z": (0, 0, -1)}

parser = argparse.ArgumentParser(prog="glb2mesh")
parser.add_argument("src")
parser.add_argument("dst", nargs="?")
parser.add_argument("--info", action="store_true")
parser.add_argument("--length", type=float, required=True,
                    help="real length of the aircraft, nose to tail, in metres")
parser.add_argument("--nose", choices=AXES, default="-y",
                    help="Blender axis the nose points along")
parser.add_argument("--up", choices=AXES, default="+z",
                    help="Blender axis that is 'up' for the aircraft")
parser.add_argument("--nozzles", default="0,0,0:0,0,0",
                    help="two exhaust exits x,y,z:x,y,z in wallpaper metres")
parser.add_argument("--nozzle-radius", type=float, default=0.0)
parser.add_argument("--drop-below", type=float, default=None,
                    help="remove triangles reaching below this height in "
                    "wallpaper metres (e.g. lowered landing gear)")
parser.add_argument("--glass", default="",
                    help="comma-separated material names to treat as glass")
args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=args.src)

# The wallpaper's axes, written in Blender's: a pilot facing `forward`
# with `up` overhead has `forward x up` on their right.
forward, up = Vector(AXES[args.nose]), Vector(AXES[args.up])
right = forward.cross(up)


def to_model(v):
    return Vector((v.dot(right), v.dot(up), v.dot(forward)))


# ---- materials: one colour lookup per material ----

def find_image(socket):
    """The first image texture feeding this socket, through any mix nodes."""
    if not socket.is_linked:
        return None
    node = socket.links[0].from_node
    if node.type == "TEX_IMAGE":
        return node.image
    for inp in node.inputs:
        image = find_image(inp)
        if image:
            return image
    return None


class Material:
    def __init__(self, mat):
        self.name = mat.name if mat else "(none)"
        self.factor = np.ones(3)
        self.pixels = None
        self.kind = KIND_PAINT
        bsdf = None
        if mat and mat.use_nodes:
            bsdf = next((n for n in mat.node_tree.nodes
                         if n.type == "BSDF_PRINCIPLED"), None)
        if bsdf is None:
            return
        base = bsdf.inputs["Base Color"]
        image = find_image(base)
        if image is not None and image.size[0] > 0:
            w, h = image.size
            px = np.empty(w * h * 4, np.float32)
            image.pixels.foreach_get(px)
            self.pixels = px.reshape(h, w, 4)
        elif not base.is_linked:
            self.factor = np.array(base.default_value[:3])
        alpha = bsdf.inputs["Alpha"]
        see_through = (not alpha.is_linked and alpha.default_value < 0.9) or \
            mat.blend_method == "BLEND" or \
            any(word in self.name.lower() for word in ("glass", "canopy", "window"))
        if see_through or self.name in args.glass.split(","):
            self.kind = KIND_GLASS

    def colour(self, uv):
        if self.kind == KIND_GLASS:
            # Same tint as the hand-built canopies; the glint does the rest.
            return (44, 66, 92)
        c = self.factor
        if self.pixels is not None:
            h, w = self.pixels.shape[:2]
            x = int((uv[0] % 1.0) * w) % w
            y = int((uv[1] % 1.0) * h) % h
            c = self.pixels[y, x, :3] * self.factor
        return tuple(int(max(0, min(255, round(v * 255)))) for v in c)


# ---- collect every triangle corner ----

materials = {}
positions, normals, colours, kinds = [], [], [], []
triangles = []
corner_index = {}
per_material = {}
depsgraph = bpy.context.evaluated_depsgraph_get()

for oi, obj in enumerate(o for o in bpy.context.scene.objects if o.type == "MESH"):
    mesh = obj.evaluated_get(depsgraph).to_mesh()
    mesh.calc_loop_triangles()
    world = obj.matrix_world
    turn = world.to_3x3().inverted().transposed()
    uvs = mesh.uv_layers.active.data if mesh.uv_layers.active else None
    for tri in mesh.loop_triangles:
        slot = obj.material_slots[tri.material_index].material \
            if obj.material_slots else None
        key = slot.name if slot else None
        if key not in materials:
            materials[key] = Material(slot)
        material = materials[key]
        corners = []
        for loop in tri.loops:
            vi = mesh.loops[loop].vertex_index
            uv = tuple(uvs[loop].uv) if uvs else (0.0, 0.0)
            n = mesh.corner_normals[loop].vector
            # Corners that share position, normal and colour become one vertex.
            ident = (oi, vi, round(uv[0], 4), round(uv[1], 4),
                     round(n.x, 3), round(n.y, 3), round(n.z, 3), key)
            if ident not in corner_index:
                corner_index[ident] = len(positions)
                positions.append(to_model(world @ mesh.vertices[vi].co))
                normals.append(to_model((turn @ n).normalized()))
                colours.append(material.colour(uv))
                kinds.append(material.kind)
            corners.append(corner_index[ident])
        triangles.append(corners)
        per_material.setdefault(material.name, []).extend(corners)
    obj.evaluated_get(depsgraph).to_mesh_clear()

# ---- centre and scale to real size ----

lo = Vector([min(p[i] for p in positions) for i in range(3)])
hi = Vector([max(p[i] for p in positions) for i in range(3)])
centre = (lo + hi) / 2
scale = args.length / (hi.z - lo.z)
positions = [(p - centre) * scale for p in positions]

# Drop low triangles, then the vertices nothing uses any more. The centre
# stays where it was, so --nozzles measured before dropping still fit.
if args.drop_below is not None:
    triangles = [t for t in triangles
                 if min(positions[i].y for i in t) >= args.drop_below]
    used = sorted({i for t in triangles for i in t})
    renumber = {old: new for new, old in enumerate(used)}
    positions = [positions[i] for i in used]
    normals = [normals[i] for i in used]
    colours = [colours[i] for i in used]
    kinds = [kinds[i] for i in used]
    triangles = [[renumber[i] for i in t] for t in triangles]
    per_material = {name: [renumber[i] for i in corners if i in renumber]
                    for name, corners in per_material.items()}

if args.info:
    size = (hi - lo) * scale
    print(f"INFO size: span {size.x:.2f} m, height {size.y:.2f} m, "
          f"length {size.z:.2f} m; {len(positions)} vertices, "
          f"{len(triangles)} triangles")
    for name, corners in sorted(per_material.items()):
        if not corners:
            continue
        pts = [positions[c] for c in set(corners)]
        mid = sum(pts, Vector()) / len(pts)
        zmin = min(p.z for p in pts)
        kind = "glass" if kinds[corners[0]] == KIND_GLASS else "paint"
        rgb = colours[corners[0]]
        print(f"INFO material {name!r}: {kind}, {len(corners) // 3} triangles, "
              f"centre x={mid.x:.2f} y={mid.y:.2f} z={mid.z:.2f}, "
              f"rearmost z={zmin:.2f}, sample colour {rgb}")
    sys.exit(0)

nozzles = [[float(v) for v in n.split(",")] for n in args.nozzles.split(":")]
with open(args.dst, "wb") as f:
    f.write(b"PKMESH1\0")
    f.write(struct.pack("<II", len(positions), len(triangles)))
    f.write(struct.pack("<7f", *nozzles[0], *nozzles[1], args.nozzle_radius))
    for p, n, c, k in zip(positions, normals, colours, kinds):
        f.write(struct.pack("<6f4B", p.x, p.y, p.z, n.x, n.y, n.z, *c, k))
    for t in triangles:
        f.write(struct.pack("<3I", *t))
print(f"GLB2MESH wrote {args.dst}: {len(positions)} vertices, "
      f"{len(triangles)} triangles")
