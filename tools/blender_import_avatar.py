#!/usr/bin/env python3
"""
Xbox 360 AvatarExtract -> Blender importer. Works two ways:

1) STANDALONE SCRIPT - opens a fresh scene with the avatar in it:

    blender --python blender_import_avatar.py
    blender --python blender_import_avatar.py -- /path/to/AvatarExtract
    blender --background --python blender_import_avatar.py -- /path/to/AvatarExtract --render preview.png
    blender --background --python blender_import_avatar.py -- /path/to/AvatarExtract --save avatar_preview.blend

   No path needed in the common case: the script looks for avatar.gltf next
   to itself, in its own subfolders, and in the current working directory.

2) INSTALLED ADD-ON - import into a project you're already working on, any
   time, no command line needed, ever again:

    Blender > Edit > Preferences > Add-ons > Install from Disk... > pick this
    file > enable "Import: Xbox 360 Avatar". Then, in any .blend:
    File > Import > Xbox 360 Avatar (avatar.gltf) - pick the AvatarExtract
    folder's avatar.gltf and it drops the avatar into your CURRENT scene
    (nothing gets cleared), with the same picker panel and keyframeable face
    properties set up as the standalone path gives you. Import more than one
    avatar and the panel follows whichever one you've got selected (or pick
    explicitly from the "Avatar" dropdown that appears once there's >1).

The face system: rather than swapping a single pre-baked head texture, the
Head material is a real shader node graph that composites the console's raw
mouth/eyebrow/eye layer textures live, using the exact tint math the Xbox's
own avatar shader uses (head_material.txt has the tone colors). Three
independent, keyframeable properties on the armature (Mouth/Eyebrow/Eye) pick
which layer of each is showing - so ANY combination works, not just the ones
that happened to appear in an existing animation, and the console never has
to pre-bake a texture for every possible expression combination.

What it does either way:
  - imports avatar.gltf (mesh + skeleton + baked system animations)
  - finds the armature and lists every imported animation action
  - binds one action directly onto the armature so it plays immediately
  - builds the live-compositing Head material and sets up the Avatar tab in
    the 3D viewport's N-panel: click-to-pick animation/preset thumbnails,
    plus keyframeable Mouth/Eyebrow/Eye properties settable on the timeline
  - (standalone script only) frames a camera, adds simple 3-point lighting,
    and can render a still / save a .blend for a provable, shareable result
"""
import bpy, sys, os, math, re, struct
import bmesh
from bpy_extras.io_utils import ImportHelper
from mathutils import Vector
from mathutils.bvhtree import BVHTree

bl_info = {
    "name": "Xbox 360 Avatar Importer",
    "author": "AvatarExporter",
    "version": (2, 0, 0),
    "blender": (4, 0, 0),
    "location": "File > Import > Xbox 360 Avatar (.gltf)",
    "description": "Import an AvatarExtract export (mesh, rig, animations, live-composited "
                    "face) into the current scene, with a clickable animation/preset picker "
                    "and keyframeable Mouth/Eyebrow/Eye properties",
    "category": "Import-Export",
}

# Per-avatar runtime state, keyed by armature.name (unique within a scene, so
# this is safe with multiple imported avatars). Holds things that can't be
# saved into the .blend itself (node/preview-icon handles) or that are
# simplest to keep alongside them.
#   { armature_name: {
#       "out_dir": str,
#       "tones": {toneName: (r,g,b)},                       # head_material.txt
#       "presets": {presetName: (mouth,eyebrow,eye)},        # face_presets.txt
#       "face_keyframes": {animName: [(frame,mouth,eyebrow,eye), ...]},
#       "layers": {"mouth"/"eyebrow"/"eye": [(idx,label,path), ...]},
#       "nodes": {"mouth"/"eyebrow"/"eye": ShaderNodeTexImage},
#       "action_items": [(id, name, desc, icon_id, index), ...],
#       "preset_items": [(id, name, desc, icon_id, index), ...],
#       "action_pcoll":, "preset_pcoll":,   # bpy.utils.previews collections (must outlive the items)
#   } }
_avatar_data = {}
_face_follows_animation = True  # False once a face is hand-picked/keyframed; True lets the console's own track drive it

# Friendly names for each layer index, straight from the XDK's
# XAVATAR_ANIMATED_TEXTURE_LAYER_* enums. An avatar's actual layer COUNT can
# be smaller (or, in theory, larger) than these lists - only indices that
# have a real exported file are ever offered, using "Layer N" as a fallback
# label for anything past what's named here.
MOUTH_LAYER_NAMES = ["Neutral", "Sad", "Angry", "Confused", "Laughing", "Shocked", "Happy",
                      "Phonetic O", "Phonetic AI", "Phonetic EE", "Phonetic FV", "Phonetic W",
                      "Phonetic L", "Phonetic DTH"]
EYEBROW_LAYER_NAMES = ["Neutral", "Sad", "Angry", "Confused", "Raised"]
EYE_LAYER_NAMES = ["Neutral", "Sad", "Angry", "Confused", "Laughing", "Shocked", "Happy",
                    "Yawning", "Sleeping", "Look Up", "Look Down", "Look Outer", "Look Inner", "Blink"]
LAYER_NAME_TABLES = {"mouth": MOUTH_LAYER_NAMES, "eyebrow": EYEBROW_LAYER_NAMES, "eye": EYE_LAYER_NAMES}
# usage label the console export uses in filenames for each feature slot
FEATURE_FILE_USAGE = {"mouth": "mouth", "eyebrow": "browR", "eye": "eyeR"}
FEATURES = ("mouth", "eyebrow", "eye")


def log(*a):
    print("[avatar-import]", *a)


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    path = None
    render_out = None
    save_out = None
    action_name = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--render" and i + 1 < len(argv):
            render_out = argv[i + 1]; i += 2
        elif a == "--save" and i + 1 < len(argv):
            save_out = argv[i + 1]; i += 2
        elif a == "--action" and i + 1 < len(argv):
            action_name = argv[i + 1]; i += 2
        elif not a.startswith("--"):
            path = a; i += 1
        else:
            i += 1
    return path, render_out, save_out, action_name


def find_gltf(hint):
    """Locate avatar.gltf: explicit hint (file or folder), else search near the script / cwd."""
    candidates = []
    if hint:
        hint = os.path.abspath(hint)
        if os.path.isfile(hint):
            return hint
        candidates.append(hint)

    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = os.getcwd()
    candidates += [here, os.path.join(here, "AvatarExtract"), os.path.dirname(here)]
    candidates.append(os.getcwd())

    for base in candidates:
        direct = os.path.join(base, "avatar.gltf")
        if os.path.isfile(direct):
            return direct
        if os.path.isdir(base):
            for name in os.listdir(base):
                sub = os.path.join(base, name, "avatar.gltf")
                if os.path.isfile(sub):
                    return sub
    return None


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for coll in list(bpy.data.collections):
        if coll.users == 0:
            bpy.data.collections.remove(coll)


def _base_name(name):
    return re.sub(r"\.\d+$", "", name)


def _declip_open_edges(fix_obj, against_obj, margin=0.003):
    """Push fix_obj's open-edge (boundary loop) vertices that end up on the
    inside of against_obj's surface back out along that surface's normal, by
    the penetration depth plus margin. Some avatar shell meshes (e.g. the
    cap's open bottom edge vs the hair underneath it) are authored with real,
    small geometric overlap right at that cut edge - the console's own
    renderer never gets a camera close enough for it to matter, but
    Blender's z-buffer turns it into a jagged seam. Restricted to the open
    (non-manifold) edge loop, never the whole shell: a full-mesh nearest-
    surface test is unreliable on a close-fitting concave shell like this and
    was verified (see project history) to badly distort the mesh when tried;
    the open edge loop is a small, well-defined set where "inside the other
    surface" unambiguously means "authored to overlap slightly"."""
    against_bm = bmesh.new()
    against_bm.from_mesh(against_obj.data)
    against_bm.transform(against_obj.matrix_world)
    against_bvh = BVHTree.FromBMesh(against_bm)

    bm = bmesh.new()
    bm.from_mesh(fix_obj.data)
    boundary_verts = {v for e in bm.edges if e.is_boundary for v in e.verts}
    mat = fix_obj.matrix_world
    inv = mat.inverted()
    moved = 0
    for v in boundary_verts:
        wco = mat @ v.co
        loc, norm, idx, dist = against_bvh.find_nearest(wco)
        if loc is None:
            continue
        to_v = wco - loc
        s = to_v.dot(norm)
        if s < 0:
            new_wco = loc + norm * (-s + margin)
            v.co = inv @ new_wco
            moved += 1
    bm.to_mesh(fix_obj.data)
    bm.free()
    against_bm.free()
    fix_obj.data.update()
    return moved


def import_gltf(path):
    before_mats = set(bpy.data.materials.keys())
    before_objs = set(bpy.data.objects.keys())
    bpy.ops.import_scene.gltf(filepath=path, import_shading="NORMALS")
    # Every avatar texture has alpha=255 throughout (see _coverage_gate's docstring
    # for the same "shape on black" convention) - none of these materials are ever
    # meant to be translucent. Blender's glTF importer still defaults new materials
    # to blend_method='HASHED' just because the base color texture carries an alpha
    # channel, which makes thin near-coincident shells (like the cap's brim, where
    # the outer and inner wall are almost touching) dither/z-fight into a jagged
    # black seam - or, on the head, a dithered/stochastic-looking translucency
    # across the whole face. blend_method is legacy EEVEE and does NOTHING in
    # Blender 4.2+'s EEVEE Next (verified: setting it doesn't even change what
    # it reports back) - surface_render_method is the property that actually
    # controls this now, and its default 'DITHERED' is exactly the stochastic
    # alpha-test behavior causing the see-through look. 'BLENDED' is proper
    # back-to-front alpha compositing, which for these always-opaque materials
    # just renders solid.
    for name in set(bpy.data.materials.keys()) - before_mats:
        mat = bpy.data.materials[name]
        mat.surface_render_method = "BLENDED"
        mat.show_transparent_back = False

    new_objs = {name: bpy.data.objects[name] for name in set(bpy.data.objects.keys()) - before_objs}
    by_base = {}
    for name, obj in new_objs.items():
        by_base.setdefault(_base_name(name), obj)
    hat = by_base.get("Hat_0")
    hair = by_base.get("Hair_2")
    head = by_base.get("Head_7")
    if hat and hair:
        n = _declip_open_edges(hat, hair, margin=0.003)
        if n:
            log(f"de-clipped {n} cap open-edge vert(s) that overlapped the hair mesh")
    if hat and head:
        n = _declip_open_edges(hat, head, margin=0.003)
        if n:
            log(f"de-clipped {n} cap open-edge vert(s) that overlapped the head mesh")


def _armature_enum_items(self, context):
    return [(o.name, o.name, "") for o in context.scene.objects if o.type == "ARMATURE"]


def find_armature(context=None):
    """Which avatar the panel/operators act on, for scenes with more than one:
    whatever's currently selected wins; otherwise the "Avatar" dropdown (only
    shown when there's >1 armature); otherwise just the first armature found
    (fine for the common single-avatar case)."""
    context = context or bpy.context
    scene = context.scene

    ao = context.active_object
    if ao is not None:
        if ao.type == "ARMATURE":
            return ao
        if ao.type == "MESH":
            if ao.parent is not None and ao.parent.type == "ARMATURE":
                return ao.parent
            for mod in ao.modifiers:
                if mod.type == "ARMATURE" and mod.object is not None:
                    return mod.object

    picked = getattr(scene, "avatar_active_armature", None)
    if picked:
        obj = bpy.data.objects.get(picked)
        if obj is not None and obj.type == "ARMATURE":
            return obj

    for o in scene.objects:
        if o.type == "ARMATURE":
            return o
    return None


def scene_bounds(objects=None):
    """Bounds over `objects` (mesh only), or every mesh in the scene if not
    given - pass the just-imported objects when merging into an existing
    project so an unrelated object elsewhere in the scene can't skew it."""
    mn = Vector((1e9, 1e9, 1e9))
    mx = Vector((-1e9, -1e9, -1e9))
    found = False
    for o in (objects if objects is not None else bpy.context.scene.objects):
        if o.type != "MESH":
            continue
        found = True
        for corner in o.bound_box:
            wc = o.matrix_world @ Vector(corner)
            mn.x, mn.y, mn.z = min(mn.x, wc.x), min(mn.y, wc.y), min(mn.z, wc.z)
            mx.x, mx.y, mx.z = max(mx.x, wc.x), max(mx.y, wc.y), max(mx.z, wc.z)
    if not found:
        return Vector((0, 0, 0)), 1.0
    center = (mn + mx) * 0.5
    radius = (mx - mn).length * 0.5
    return center, max(radius, 0.2)


def look_at(obj, target):
    direction = target - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def setup_camera_and_lights(center, radius):
    cam_data = bpy.data.cameras.new("AvatarCam")
    cam = bpy.data.objects.new("AvatarCam", cam_data)
    bpy.context.collection.objects.link(cam)
    dist = radius / math.tan(math.radians(35))
    cam.location = center + Vector((0, -dist * 1.15, radius * 0.15))
    look_at(cam, center)
    bpy.context.scene.camera = cam

    # Energies tuned against the console's own portrait bake (faces/*.png,
    # from BakeFace() on the Xbox itself) - the original 3.0/1.0 pair left
    # skin and especially the iris tones looking notably dimmer/less
    # saturated than that reference (a Principled BSDF's diffuse response
    # doesn't return anywhere near its full albedo under a moderate-strength
    # Sun; this isn't specific to eyes, they're just the most visibly
    # saturated color on the head so under-lighting shows up there first).
    key_data = bpy.data.lights.new("Key", "SUN")
    key_data.energy = 6.5
    key = bpy.data.objects.new("Key", key_data)
    key.location = center + Vector((-radius * 2, -radius * 2, radius * 3))
    look_at(key, center)
    bpy.context.collection.objects.link(key)

    fill_data = bpy.data.lights.new("Fill", "SUN")
    fill_data.energy = 2.2
    fill = bpy.data.objects.new("Fill", fill_data)
    fill.location = center + Vector((radius * 2, -radius, radius * 1.5))
    look_at(fill, center)
    bpy.context.collection.objects.link(fill)

    world = bpy.context.scene.world
    if world is None:
        world = bpy.data.worlds.new("World")
        bpy.context.scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    if bg:
        bg.inputs[0].default_value = (0.05, 0.06, 0.08, 1.0)

    # AgX (Blender's default since 4.0) applies a filmic-style rolloff meant
    # for photographic HDR scenes; the console has no such tonemapping, so
    # 'Standard' is the more faithful comparison against its own renders.
    bpy.context.scene.view_settings.view_transform = "Standard"


def bind_action(armature, preferred_name):
    actions = [a for a in bpy.data.actions if a.users >= 0]
    if not actions:
        log("no animation actions found in the file")
        return None
    chosen = None
    if preferred_name:
        for a in actions:
            if a.name == preferred_name:
                chosen = a
                break
        if not chosen:
            log("--action '%s' not found; available: %s" % (preferred_name, ", ".join(a.name for a in actions)))
    if not chosen:
        # prefer something visibly alive over the plain idle stands
        for want in ("male_wave", "generic_wave", "male_laugh", "generic_celebrate", "female_laugh"):
            for a in actions:
                if want in a.name:
                    chosen = a
                    break
            if chosen:
                break
    if not chosen:
        chosen = actions[0]

    if armature.animation_data is None:
        armature.animation_data_create()
    armature.animation_data.action = chosen
    fr = chosen.frame_range
    bpy.context.scene.frame_start = int(fr[0])
    bpy.context.scene.frame_end = int(fr[1])
    bpy.context.scene.frame_set(int((fr[0] + fr[1]) * 0.5))
    log("bound action:", chosen.name, "frames", fr[0], "-", fr[1])
    log("all available actions (%d):" % len(actions), ", ".join(a.name for a in actions))
    return chosen


def find_head_object(armature=None):
    """Locate the Head_* mesh. Scoped to `armature`'s own children when given
    (so a second imported avatar's head isn't picked up by mistake); falls
    back to an unscoped scene-wide search if that comes up empty."""
    def _search(objects):
        for o in objects:
            if o.type == "MESH" and o.name.startswith("Head_"):
                return o
        return None
    if armature is not None:
        result = _search([o for o in bpy.context.scene.objects if o.parent == armature])
        if result is not None:
            return result
    return _search(bpy.context.scene.objects)


# ---------------------------------------------------------------- data files
def parse_head_material(out_dir):
    """head_material.txt: '<name> <r> <g> <b>' per line - the 8 tone colors
    the console's own avatar shader tints each feature layer with."""
    path = os.path.join(out_dir, "head_material.txt")
    tones = {}
    if not os.path.isfile(path):
        return tones
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            try:
                tones[parts[0]] = (float(parts[1]), float(parts[2]), float(parts[3]))
            except ValueError:
                continue
    return tones


def parse_body_material(out_dir):
    """body_material.txt: '<component_name> c0r c0g c0b c1r c1g c1b c2r c2g c2b'
    per line - the 3 CustomColor constants psBody tints a part's 'intensity'
    mask with (see main.cpp's WriteBodyMaterial_worker). Only components that
    actually use an intensity map get a line."""
    path = os.path.join(out_dir, "body_material.txt")
    result = {}
    if not os.path.isfile(path):
        return result
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 10:
                continue
            try:
                vals = [float(x) for x in parts[1:]]
            except ValueError:
                continue
            result[parts[0]] = (tuple(vals[0:3]), tuple(vals[3:6]), tuple(vals[6:9]))
    return result


def parse_face_presets(out_dir):
    """face_presets.txt: '<name> <mouth> <eye> <eyebrow>' per line."""
    path = os.path.join(out_dir, "face_presets.txt")
    presets = {}
    if not os.path.isfile(path):
        return presets
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            try:
                presets[parts[0]] = {"mouth": int(parts[1]), "eye": int(parts[2]), "eyebrow": int(parts[3])}
            except ValueError:
                continue
    return presets


def parse_face_keyframes(out_dir):
    """face_keyframes.txt: '<animName> <frame> <mouth> <eyebrow> <eye>' per
    line - one line per point a system animation's facial track actually
    changes (the console samples XAvatarAnimation::GetPose()'s texture-layer
    output alongside the joint pose - the same data the real Xbox dashboard
    uses to animate eyebrows/eyes/mouth during these clips)."""
    path = os.path.join(out_dir, "face_keyframes.txt")
    data = {}
    if not os.path.isfile(path):
        return data
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 5:
                continue
            anim, frame, mouth, eyebrow, eye = parts
            try:
                data.setdefault(anim, []).append((int(frame), int(mouth), int(eyebrow), int(eye)))
            except ValueError:
                continue
    for anim in data:
        data[anim].sort(key=lambda t: t[0])
    log("loaded facial-expression tracks for %d animation(s)" % len(data))
    return data


def list_feature_layers(out_dir, feature):
    """Which layer indices actually have an exported file for this feature,
    as (index, label, abs_path) triples sorted by index."""
    usage = FEATURE_FILE_USAGE[feature]
    tex_dir = os.path.join(out_dir, "textures")
    if not os.path.isdir(tex_dir):
        return []
    # .dds, not .png: the console's own PNG writer silently drops alpha for
    # these (confirmed via on-console debug dump), and TGA save turned out
    # to be unimplemented in this SDK (D3DERR_INVALIDCALL) - DDS is what
    # actually round-trips alpha correctly, parsed manually on load.
    pat = re.compile(r"^Head_%s_(\d+)\.dds$" % re.escape(usage), re.IGNORECASE)
    names = LAYER_NAME_TABLES[feature]
    found = []
    for fn in os.listdir(tex_dir):
        m = pat.match(fn)
        if m:
            idx = int(m.group(1))
            label = names[idx] if idx < len(names) else ("Layer %d" % idx)
            found.append((idx, label, os.path.join(tex_dir, fn)))
    found.sort(key=lambda t: t[0])
    return found


def _bakedface_eye_path(out_dir, layer_idx):
    """Path to the whole-head unlit bake bounds for one eye layer (main.cpp's
    BakeHeadEyeVariants: 'Head_bakedface_eye_<layer>_<component>.png'), or
    None if this export doesn't have it (older export, or no head)."""
    tex_dir = os.path.join(out_dir, "textures")
    if not os.path.isdir(tex_dir):
        return None
    pat = re.compile(r"^Head_bakedface_eye_%02d_\d+\.png$" % layer_idx, re.IGNORECASE)
    for fn in os.listdir(tex_dir):
        if pat.match(fn):
            return os.path.join(tex_dir, fn)
    return None


def _load_bake_image(path):
    """Loads a whole-face bake PNG as a plain sRGB color image (unlike
    _load_shape_masked_image, this is already-final console-rendered color
    data, not a mask layer - no alpha forcing, no Non-Color colorspace).

    No hand-rolled cache here (that's what check_existing=True is for) and,
    critically, .pack() the result: an un-packed image.load()'d from disk can
    drop to 0 users when a picker swap moves a node off it (every eye layer
    swap does exactly that) and Blender is then free to unload its underlying
    data - a STALE reference kept anywhere (a naive path->Image cache
    included) then points at freed memory, corrupting Blender's ID
    refcounting on the next reassignment ('ID user decrement error: 0 <= 0',
    reproducibly crashing on 'Let Animation Drive Face Again' - confirmed via
    /tmp/blender.crash.txt). Packing embeds the pixels so the datablock stays
    valid at 0 users, matching _load_shape_masked_image's already-safe
    pattern."""
    img = bpy.data.images.load(path, check_existing=True)
    if not img.packed_file:
        img.pack()
    return img


# ---------------------------------------------------------------- head material (live compositing)
def _node(nodes, links, type_, **kw):
    n = nodes.new(type_)
    for k, v in kw.items():
        setattr(n, k, v)
    return n


def _scale_color(nodes, links, color_rgb, scalar_socket):
    """color_rgb (constant) * scalar_socket -> Vector output, via VectorMath SCALE."""
    n = nodes.new("ShaderNodeVectorMath")
    n.operation = "SCALE"
    n.inputs[0].default_value = (color_rgb[0], color_rgb[1], color_rgb[2])
    links.new(scalar_socket, n.inputs["Scale"])
    return n.outputs[0]


def _add_colors(nodes, links, a_socket, b_socket):
    n = nodes.new("ShaderNodeVectorMath")
    n.operation = "ADD"
    links.new(a_socket, n.inputs[0])
    links.new(b_socket, n.inputs[1])
    return n.outputs[0]


def _weighted_sum3(nodes, links, sep, c1, c2, c3):
    """r*c1 + g*c2 + b*c3, matching FaceShader's InterpolateIntensity() stack."""
    t1 = _scale_color(nodes, links, c1, sep.outputs["Red"])
    t2 = _scale_color(nodes, links, c2, sep.outputs["Green"])
    t3 = _scale_color(nodes, links, c3, sep.outputs["Blue"])
    return _add_colors(nodes, links, _add_colors(nodes, links, t1, t2), t3)


def _mix_rgba(nodes, links, fac_socket, a_socket, b_socket):
    """lerp(a, b, fac) using the unified Mix node's RGBA sockets (index-based:
    named-socket lookup is ambiguous since Mix has float/vector/rgba variants
    all sharing input/output names)."""
    n = nodes.new("ShaderNodeMix")
    n.data_type = "RGBA"
    links.new(fac_socket, n.inputs[0])       # Factor (float)
    links.new(a_socket, n.inputs[6])         # A (RGBA)
    links.new(b_socket, n.inputs[7])         # B (RGBA)
    return n.outputs[2]                      # Result (RGBA)


def _load_dds_argb(path):
    """Manually parses an uncompressed 32bpp DDS (as the console's own
    D3DXSaveTextureToFileA(..., D3DXIFF_DDS, ...) writes for these) into a
    (width, height, rgba float32 numpy array) triple. Doesn't depend on
    Blender having DDS codec support built in - D3DXIFF_TGA turned out to
    be a valid enum value the SDK doesn't actually implement saving for
    (D3DERR_INVALIDCALL on every attempt), and PNG silently drops alpha on
    this SDK (see build_head_material's docstring) - DDS is the only format
    that actually saved successfully. The standard DDS header layout is
    little-endian regardless of the Xbox 360's own big-endian CPU (it's a
    fixed file-format spec, not a platform-native dump)."""
    import numpy as np
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"DDS ":
        raise ValueError("not a DDS file: %s" % path)
    header = data[4:4 + 124]
    height = struct.unpack_from("<I", header, 8)[0]
    width = struct.unpack_from("<I", header, 12)[0]
    pixel_start = 4 + 124
    pixels = np.frombuffer(data, dtype=np.uint8, count=width * height * 4, offset=pixel_start)
    pixels = pixels.reshape(height, width, 4).astype(np.float32) / 255.0
    # D3DFMT_A8R8G8B8 little-endian DWORD -> byte order B,G,R,A in memory
    b, g, r, a = pixels[..., 0], pixels[..., 1], pixels[..., 2], pixels[..., 3]
    rgba = np.stack([r, g, b, a], axis=-1)
    return width, height, rgba


def _load_shape_masked_image(path):
    """Loads an avatar face-feature layer texture from its .dds file (see
    _load_dds_argb) into a Blender image, then hard-thresholds alpha: any
    pixel with real signal (>2%) becomes fully opaque (1.0), anything else
    fully transparent (0.0). The console's real per-pixel alpha is smoothly
    graded rather than binary (mean ~0.21 on the eye layer, with plenty of
    intermediate values well below 1.0 even off the antialiased edges), so
    using it directly as a lerp factor still visibly dilutes the shape
    toward skin - forcing it binary here means coverage is strictly on/off,
    and the shape's own r/g/b weighted-tone mix (via _weighted_sum3) is what
    decides how dark/light/skin-like a covered pixel looks, not a partial
    alpha blend. Caches by path so re-selecting the same layer in the picker
    doesn't re-parse the file every time."""
    cache = _load_shape_masked_image._cache
    img = cache.get(path)
    if img is not None and img.name in bpy.data.images:
        return img
    w, h, rgba = _load_dds_argb(path)
    rgba[:, :, 3] = (rgba[:, :, 3] > 0.02).astype(rgba.dtype)
    name = os.path.basename(path)
    img = bpy.data.images.new(name, width=w, height=h, alpha=True, float_buffer=False)
    img.colorspace_settings.name = "Non-Color"
    # DDS rows are top-to-bottom; Blender's pixel buffer is bottom-to-top.
    img.pixels.foreach_set(rgba[::-1].reshape(-1))
    img.pack()
    cache[path] = img
    return img


_load_shape_masked_image._cache = {}


def _load_dds_image(path, non_color=False):
    """Loads a DDS (see _load_dds_argb) into a Blender image with its real,
    continuous alpha intact - unlike _load_shape_masked_image, no binary
    thresholding. That thresholding exists specifically for the head's
    eye/mouth/eyebrow COVERAGE masks (a shape either is or isn't there); body
    decal/intensity textures are soft-edged prints/tints meant to blend
    smoothly, so the real alpha should be used as-is."""
    cache = _load_dds_image._cache
    img = cache.get(path)
    if img is not None and img.name in bpy.data.images:
        return img
    w, h, rgba = _load_dds_argb(path)
    name = os.path.basename(path)
    img = bpy.data.images.new(name, width=w, height=h, alpha=True, float_buffer=False)
    if non_color:
        img.colorspace_settings.name = "Non-Color"
    img.pixels.foreach_set(rgba[::-1].reshape(-1))
    img.pack()
    cache[path] = img
    return img


_load_dds_image._cache = {}


def build_head_material(armature, head_obj, out_dir):
    """Rebuilds the Head mesh's material as a node graph that composites the
    raw skin-features/facial-hair/eyebrow/eye/mouth/eyeshadow layers live,
    using the console's own tint math (see AvatarCommon.hlsl's FaceShader).
    Returns {feature: ShaderNodeTexImage} for the 3 swappable layers."""
    tones = parse_head_material(out_dir)
    white = (1.0, 1.0, 1.0)

    def _srgb_to_linear(v):
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

    def tone(name):
        # head_material.txt's values are plain display/gamma-space numbers
        # (same space the game itself authors them in) - Blender's own color
        # nodes expect linear, so decode here or every tone renders lighter/
        # less saturated than intended (this is the same fix already applied
        # to the console's glTF baseColorFactor export, recurring here).
        r, g, b = tones.get(name, (0.8, 0.8, 0.8))
        r, g, b = _srgb_to_linear(r), _srgb_to_linear(g), _srgb_to_linear(b)
        return (r, g, b)

    mat = head_obj.active_material
    if mat is None:
        mat = bpy.data.materials.new(head_obj.name + "_face")
        head_obj.data.materials.append(mat)
    mat.use_nodes = True
    tree = mat.node_tree
    nodes, links = tree.nodes, tree.links

    output_name = None
    for n in nodes:
        if n.type == "OUTPUT_MATERIAL":
            output_name = n.name
            break
    if output_name is None:
        output_name = nodes.new("ShaderNodeOutputMaterial").name
    # `n is not output` is NOT reliable here: bpy hands back a fresh Python
    # wrapper on each access, so identity comparison silently fails to
    # recognize the same underlying node and ends up deleting it too. Compare
    # by name instead, and re-fetch `output` fresh afterward.
    for n in list(nodes):
        if n.name != output_name:
            nodes.remove(n)
    output = nodes[output_name]

    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
    bsdf.inputs["Roughness"].default_value = 0.6

    def sample(uv_index, usage, layer_idx, label):
        layers = list_feature_layers(out_dir, usage) if usage in FEATURE_FILE_USAGE else []
        # usage may be a raw file-usage string ("skinfeat" etc) not one of the
        # 3 keyframeable features - look those up directly instead
        if usage not in FEATURE_FILE_USAGE:
            tex_dir = os.path.join(out_dir, "textures")
            path = None
            if os.path.isdir(tex_dir):
                cand = os.path.join(tex_dir, "Head_%s_%02d.dds" % (usage, layer_idx))
                if os.path.isfile(cand):
                    path = cand
        else:
            match = next((p for (i, _, p) in layers if i == layer_idx), None)
            path = match if match else (layers[0][2] if layers else None)

        uvnode = nodes.new("ShaderNodeUVMap")
        if uv_index < len(head_obj.data.uv_layers):
            uvnode.uv_map = head_obj.data.uv_layers[uv_index].name
        img_node = nodes.new("ShaderNodeTexImage")
        img_node.label = label
        # The console packs each feature's UV island into a small corner of
        # its own TEXCOORD set, so the rest of the head legitimately maps
        # outside [0,1] (eye/mouth go as far as -0.29..1.32). Blender's
        # default 'REPEAT' extension tiles the small shape across that whole
        # out-of-range area, showing duplicate ghost copies of the eye/mouth
        # at the sides of the face. 'CLIP' samples as transparent (0,0,0,0)
        # outside the image instead, matching "nothing here" the same way
        # a genuine background texel does.
        img_node.extension = "CLIP"
        # 'Closest', not 'Linear': alpha is forced to a hard 0/1 in the
        # loaded image data (_load_shape_masked_image), but that's a CPU-side
        # preprocessing step - it doesn't stop the GPU's OWN bilinear
        # filtering from happening at sample time. A pixel that samples
        # between a foreground and an adjacent background texel still gets
        # an interpolated (non-binary) alpha there, letting skin bleed
        # through right at the shape's edge - subtle, but real ("eyes mix
        # with skin a bit" at the boundary, not a bulk transparency, which
        # is why it was easy to miss in a full-image comparison). 'Closest'
        # samples exactly one texel with no interpolation at all, so there's
        # no GPU-side blending left to reintroduce it.
        img_node.interpolation = "Closest"
        if path and os.path.isfile(path):
            img_node.image = _load_shape_masked_image(path)
        links.new(uvnode.outputs["UV"], img_node.inputs["Vector"])
        return img_node

    # slot order matches the console's h0..h5 samplers exactly (see psHeadAlbedo)
    sf_img = sample(0, "skinfeat", 0, "Skin Features")
    fh_img = sample(1, "facialhair", 0, "Facial Hair")
    eb_img = sample(2, "eyebrow", 0, "Eyebrow")
    ey_img = sample(3, "eye", 0, "Eye")
    mo_img = sample(4, "mouth", 0, "Mouth")
    es_img = sample(5, "eyeshadow", 0, "Eye Shadow")

    # Eye colour comes from the console's own whole-face bake (uv0 - the same
    # UV space Head_bakedface_NN.png uses, see main.cpp's BakeHeadEyeVariants),
    # not from IrisTone recomposited through Blender's lit shading. IrisTone is
    # a low-saturation colour, and lit diffuse shading compresses a pale
    # colour's contrast far more visibly than it does eyebrow's near-black -
    # that mismatch (not alpha, not UV, not colour-space) is why the eye
    # always looked washed out next to eyebrow/mouth no matter how alpha or
    # tone were tuned. Sampling the console's already-correct pixels for this
    # region sidesteps the mismatch instead of chasing it with more tuning.
    ey_bake_uv = nodes.new("ShaderNodeUVMap")
    if 0 < len(head_obj.data.uv_layers):
        ey_bake_uv.uv_map = head_obj.data.uv_layers[0].name
    ey_bake_img = nodes.new("ShaderNodeTexImage")
    ey_bake_img.label = "Eye (console bake)"
    ey_bake_img.extension = "EXTEND"
    ey_bake_img.interpolation = "Closest"
    ey_bake_path0 = _bakedface_eye_path(out_dir, 0)
    if ey_bake_path0:
        ey_bake_img.image = _load_bake_image(ey_bake_path0)
    links.new(ey_bake_uv.outputs["UV"], ey_bake_img.inputs["Vector"])

    def separated(img_node):
        sep = nodes.new("ShaderNodeSeparateColor")
        links.new(img_node.outputs["Color"], sep.inputs["Color"])
        return sep

    skin = tone("skin")
    sf_sep = separated(sf_img)
    sf_rgb = _weighted_sum3(nodes, links, sf_sep, tone("skinfeature1"), tone("skinfeature2"), skin)

    fh_sep = separated(fh_img)
    fh_rgb = _weighted_sum3(nodes, links, fh_sep, tone("facialhair"), white, skin)

    eb_sep = separated(eb_img)
    eb_rgb = _weighted_sum3(nodes, links, eb_sep, tone("eyebrow"), white, skin)

    # Coverage still comes from the raw eye layer's own alpha (ey_img, its own
    # small-island UV) - only the RGB source changed to the bake above.
    ey_rgb = ey_bake_img.outputs["Color"]

    mo_sep = separated(mo_img)
    mo_rgb = _weighted_sum3(nodes, links, mo_sep, tone("mouth"), white, skin)

    es_sep = separated(es_img)
    es_rgb = _scale_color(nodes, links, tone("eyeshadow"), es_sep.outputs["Red"])

    skin_rgb_node = nodes.new("ShaderNodeRGB")
    skin_rgb_node.outputs[0].default_value = (*skin, 1.0)
    c = skin_rgb_node.outputs[0]
    # Every one of these six textures now carries a real, computed alpha
    # (see _load_shape_masked_image) instead of the console's always-255
    # export, so this is the plain, direct lerp(previous, feature.rgb,
    # feature.a) from the original shader - no coverage-reconstruction
    # heuristics needed at all.
    c = _mix_rgba(nodes, links, sf_img.outputs["Alpha"], c, sf_rgb)
    c = _mix_rgba(nodes, links, es_img.outputs["Alpha"], c, es_rgb)
    c = _mix_rgba(nodes, links, mo_img.outputs["Alpha"], c, mo_rgb)
    c = _mix_rgba(nodes, links, ey_img.outputs["Alpha"], c, ey_rgb)
    c = _mix_rgba(nodes, links, fh_img.outputs["Alpha"], c, fh_rgb)
    c = _mix_rgba(nodes, links, eb_img.outputs["Alpha"], c, eb_rgb)
    links.new(c, bsdf.inputs["Base Color"])

    # lay the graph out left-to-right instead of stacked on top of itself
    for i, n in enumerate(nodes):
        n.location = (i * 180 - 1800, (i % 5) * 160)
    output.location = (400, 0)

    return {"mouth": mo_img, "eyebrow": eb_img, "eye": ey_img, "eye_bake": ey_bake_img}


# ---------------------------------------------------------------- body material (color+intensity+decal)

def _find_raw_texture(out_dir, comp_name, usage, ext, layer=0):
    path = os.path.join(out_dir, "textures", "%s_%s_%02d.%s" % (comp_name, usage, layer, ext))
    return path if os.path.isfile(path) else None


def build_body_material(obj, out_dir, custom_colors):
    """Rebuilds one non-head avatar part's material as color+intensity-tint+
    decal, matching psBody's compositing (main.cpp's g_shader/IntensityMap):
    an 'intensity' mask tinted per-channel by 3 CustomColor constants, then a
    'decal' graphic (logo/print/pattern) lerped on top by its own alpha. The
    glTF import only ever wires up the flat 'color' texture (see imgJS in
    main.cpp's export worker), so team-color tinting and any decal graphic
    are otherwise completely missing - not a UV bug, the data just was not
    part of the material at all until this.

    `custom_colors` is (c0, c1, c2) RGB triples from parse_body_material(),
    or None if this component never had an intensity map on console (then
    only the decal, if any, gets composited on top of the plain color)."""
    comp_name = obj.name
    # "color" doesn't need alpha, so the plain (freshly-regenerated, see
    # main.cpp's ResolveAndSaveTexture) .png is fine. "intensity"/"decal" are
    # lerp FACTORS - their alpha channel is the actual shape/coverage, and
    # D3DXSaveTextureToFileA silently flattens alpha to opaque when saving
    # PNG on this SDK (confirmed: Shirt_decal_00.png has zero alpha
    # variation while the .dds has a real soft-edged mask) - so those two
    # must come from the alpha-correct .dds instead.
    color_path = _find_raw_texture(out_dir, comp_name, "color", "png")
    intensity_path = _find_raw_texture(out_dir, comp_name, "intensity", "dds")
    decal_path = _find_raw_texture(out_dir, comp_name, "decal", "dds")
    if not color_path or (not intensity_path and not decal_path):
        return  # nothing extra to composite - leave the glTF-imported material alone

    def _srgb_to_linear(v):
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

    def _lin(rgb):
        return tuple(_srgb_to_linear(c) for c in rgb)

    mat = obj.active_material
    if mat is None:
        mat = bpy.data.materials.new(obj.name + "_mat")
        obj.data.materials.append(mat)
    mat.use_nodes = True
    tree = mat.node_tree
    nodes, links = tree.nodes, tree.links

    output_name = None
    for n in nodes:
        if n.type == "OUTPUT_MATERIAL":
            output_name = n.name
            break
    if output_name is None:
        output_name = nodes.new("ShaderNodeOutputMaterial").name
    for n in list(nodes):
        if n.name != output_name:
            nodes.remove(n)
    output = nodes[output_name]

    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])

    def tex_node(uv_index, path, label, non_color):
        uvnode = nodes.new("ShaderNodeUVMap")
        if uv_index < len(obj.data.uv_layers):
            uvnode.uv_map = obj.data.uv_layers[uv_index].name
        img_node = nodes.new("ShaderNodeTexImage")
        img_node.label = label
        img_node.extension = "CLIP"
        # 'Linear', not 'Closest': unlike the head's per-feature mask layers
        # (which force Closest to avoid edge-bleed on a binary alpha mask -
        # see build_head_material), these color/intensity/decal textures
        # carry no such forced-binary alpha and are often genuinely
        # low-resolution (a small logo/print, not a full-detail photo).
        # Closest turned every texel into a big flat visible square here.
        img_node.interpolation = "Linear"
        if path.lower().endswith(".dds"):
            img = _load_dds_image(path, non_color=non_color)
        else:
            img = bpy.data.images.load(path, check_existing=True)
            if non_color:
                img.colorspace_settings.name = "Non-Color"
            # pack: see _load_bake_image's docstring - an un-packed, disk-
            # linked image that drops to 0 users (rebuilding this node tree
            # a second time does exactly that) is unsafe to keep any
            # reference to, crashing Blender's ID refcounting later.
            if not img.packed_file:
                img.pack()
        img_node.image = img
        links.new(uvnode.outputs["UV"], img_node.inputs["Vector"])
        return img_node

    color_node = tex_node(0, color_path, "Color", False)
    c = color_node.outputs["Color"]

    if intensity_path and custom_colors:
        intensity_node = tex_node(1, intensity_path, "Intensity", True)
        sep = nodes.new("ShaderNodeSeparateColor")
        links.new(intensity_node.outputs["Color"], sep.inputs["Color"])
        cc0, cc1, cc2 = (_lin(custom_colors[0]), _lin(custom_colors[1]), _lin(custom_colors[2]))
        mapped = _weighted_sum3(nodes, links, sep, cc0, cc1, cc2)
        c = _mix_rgba(nodes, links, intensity_node.outputs["Alpha"], c, mapped)

    if decal_path:
        decal_node = tex_node(2, decal_path, "Decal", False)
        c = _mix_rgba(nodes, links, decal_node.outputs["Alpha"], c, decal_node.outputs["Color"])

    links.new(c, bsdf.inputs["Base Color"])
    for i, n in enumerate(nodes):
        n.location = (i * 180 - 1200, (i % 4) * 160)
    output.location = (300, 0)


def build_body_materials(armature, out_dir):
    """Runs build_body_material() on every non-head mesh part of this avatar
    that has an exported intensity or decal texture. Call once, right after
    import_gltf() - unlike the head's live-compositing graph (built lazily on
    first expression change), body parts have no swappable layers, so there
    is no reason to defer this."""
    custom = parse_body_material(out_dir)
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or obj.parent != armature or obj.name.startswith("Head_"):
            continue
        build_body_material(obj, out_dir, custom.get(obj.name))


def _apply_layer_sync(armature, feature, layer_idx):
    """Does the actual work: builds the live-composite graph if this is the
    first expression change for this avatar (swapping off the console's
    pixel-perfect bake - see setup_pickers()), then swaps the feature's
    image. May create new Blender data-blocks (the graph's nodes, or a
    not-yet-seen .dds layer via _load_shape_masked_image) - only call this
    directly from a normal script/operator context. From a property
    update= callback or a frame_change_pre handler, call apply_layer()
    instead."""
    data = _avatar_data.get(armature.name)
    if not data:
        return
    if "nodes" not in data:
        head_obj = find_head_object(armature)
        if head_obj is None:
            return
        data["nodes"] = build_head_material(armature, head_obj, data.get("out_dir", ""))
    node = data.get("nodes", {}).get(feature)
    if node is None:
        return
    match = next((p for (i, _, p) in data.get("layers", {}).get(feature, []) if i == layer_idx), None)
    if match and os.path.isfile(match):
        node.image = _load_shape_masked_image(match)
    if feature == "eye":
        bake_node = data.get("nodes", {}).get("eye_bake")
        bake_path = _bakedface_eye_path(data.get("out_dir", ""), layer_idx)
        if bake_node is not None and bake_path:
            bake_node.image = _load_bake_image(bake_path)


def apply_layer(armature, feature, layer_idx):
    """Safe to call from anywhere - property update= callbacks, a
    frame_change_pre handler, or plain scripts/operators. Building the live-
    composite graph and loading a not-yet-seen .dds layer both create new
    Blender data-blocks (see _apply_layer_sync), which isn't safe to do
    directly inside a property update= callback or frame_change_pre handler
    (both call into this) - it crashed Blender. If that work is already
    done, this is just a cheap in-place node.image swap, done synchronously
    right here; otherwise it's deferred to the next event-loop tick (a
    normal script context, where data-block creation is fine)."""
    data = _avatar_data.get(armature.name)
    if not data:
        return
    match = next((p for (i, _, p) in data.get("layers", {}).get(feature, []) if i == layer_idx), None)
    graph_ready = "nodes" in data
    image_cached = match is not None and match in _load_shape_masked_image._cache
    if graph_ready and image_cached:
        _apply_layer_sync(armature, feature, layer_idx)
        return
    armature_name = armature.name
    def _retry():
        arm = bpy.data.objects.get(armature_name)
        if arm is not None:
            _apply_layer_sync(arm, feature, layer_idx)
        return None
    bpy.app.timers.register(_retry, first_interval=0.0)


def apply_face_for_frame(action_name, frame, armature):
    """Pick whichever segment of that action's facial track covers `frame`
    and apply it - called on animation switch and on every frame change."""
    data = _avatar_data.get(armature.name)
    if not data:
        return
    segs = data.get("face_keyframes", {}).get(action_name)
    if not segs:
        return
    chosen = segs[0][1:]
    for start, mouth, eyebrow, eye in segs:
        if start <= frame:
            chosen = (mouth, eyebrow, eye)
        else:
            break
    for feature, idx in zip(FEATURES, chosen):
        apply_layer(armature, feature, idx)


def apply_preset(armature, preset_name):
    data = _avatar_data.get(armature.name)
    if not data:
        return
    preset = data.get("presets", {}).get(preset_name)
    if not preset:
        return
    for feature in FEATURES:
        idx = preset[feature]
        setattr(armature, "avatar_" + feature, str(idx))


def find_action_fcurve(action, data_path):
    """Action.fcurves was removed in Blender's newer layered-action data model
    (4.4+: Action -> layers -> strips -> channelbags -> fcurves). This walks
    that without creating anything (unlike fcurve_ensure_for_datablock)."""
    if action is None:
        return None
    for layer in action.layers:
        for strip in layer.strips:
            for cb in getattr(strip, "channelbags", []):
                fc = cb.fcurves.find(data_path)
                if fc is not None:
                    return fc
    return None


def _on_frame_change(scene, *_args):
    armature = find_armature()
    if not armature or not armature.animation_data or not armature.animation_data.action:
        return
    action = armature.animation_data.action
    # Hand-placed keyframes on avatar_mouth/eyebrow/eye (see
    # AVATAR_OT_insert_face_keyframe) always win, regardless of
    # _face_follows_animation - that flag only gates the auto-derived console
    # track below. Note: a property's own update= callback is NOT a reliable
    # way to catch this - Blender's depsgraph evaluates F-curve-driven
    # property changes without necessarily re-invoking a bpy.props update
    # callback, so playback/scrubbing has to be driven from here, reading
    # whatever value the curve just evaluated to.
    keyframed = any(find_action_fcurve(action, "avatar_" + f) is not None for f in FEATURES)
    if keyframed:
        for feature in FEATURES:
            if find_action_fcurve(action, "avatar_" + feature) is not None:
                val = getattr(armature, "avatar_" + feature, None)
                if val is not None:
                    apply_layer(armature, feature, int(val))
        return
    if not _face_follows_animation:
        return
    apply_face_for_frame(action.name, scene.frame_current, armature)


def register_frame_handler():
    for h in list(bpy.app.handlers.frame_change_pre):
        if getattr(h, "__name__", "") == "_on_frame_change":
            bpy.app.handlers.frame_change_pre.remove(h)
    bpy.app.handlers.frame_change_pre.append(_on_frame_change)


def generate_action_thumbnails(armature, actions, out_dir):
    """Render one small pose thumbnail per action, cached in thumbnails/ next
    to the export so re-opening the file doesn't re-render every time."""
    thumb_dir = os.path.join(out_dir, "thumbnails")
    os.makedirs(thumb_dir, exist_ok=True)
    scene = bpy.context.scene
    if scene.camera is None:
        log("no camera in the scene - skipping thumbnail render (the picker will "
            "still work, just without pictures; add a camera and re-import, or "
            "tick 'Add Camera & Lights', to get them)")
        return thumb_dir
    prev_action = armature.animation_data.action if armature.animation_data else None
    prev_frame = scene.frame_current
    prev_res = (scene.render.resolution_x, scene.render.resolution_y)
    prev_path = scene.render.filepath
    try:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    except TypeError:
        scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 192
    scene.render.resolution_y = 192
    made = 0
    # frame_set() below fires frame_change_pre (_on_frame_change), which
    # calls apply_layer() and would eagerly build the live head material -
    # exactly the "swap off the pixel-perfect bake" apply_layer's own lazy
    # build is meant to defer until an expression is actually changed by the
    # user. Pull the handler out for the duration of thumbnailing (which
    # only cares about the skeletal pose, not facial expression) and restore
    # it after, so cached-thumbnail runs stay a true no-op for the material.
    had_handler = _on_frame_change in bpy.app.handlers.frame_change_pre
    if had_handler:
        bpy.app.handlers.frame_change_pre.remove(_on_frame_change)
    try:
        for a in actions:
            out_path = os.path.join(thumb_dir, a.name + ".png")
            if os.path.isfile(out_path):
                continue
            if armature.animation_data is None:
                armature.animation_data_create()
            armature.animation_data.action = a
            fr = a.frame_range
            scene.frame_set(int((fr[0] + fr[1]) * 0.5))
            scene.render.filepath = out_path
            bpy.ops.render.render(write_still=True)
            made += 1
        if armature.animation_data:
            armature.animation_data.action = prev_action
        scene.frame_set(prev_frame)
    finally:
        if had_handler:
            bpy.app.handlers.frame_change_pre.append(_on_frame_change)
    scene.render.resolution_x, scene.render.resolution_y = prev_res
    scene.render.filepath = prev_path
    log("rendered %d new action thumbnail(s) into %s" % (made, thumb_dir))
    return thumb_dir


def update_action_pick(self, context):
    global _face_follows_animation
    armature = find_armature(context)
    action = bpy.data.actions.get(self.avatar_action_pick)
    if not armature or not action:
        return
    if armature.animation_data is None:
        armature.animation_data_create()
    armature.animation_data.action = action
    fr = action.frame_range
    context.scene.frame_start = int(fr[0])
    context.scene.frame_end = int(fr[1])
    mid = int((fr[0] + fr[1]) * 0.5)
    context.scene.frame_set(mid)
    # picking a pose hands the face back to whatever that animation drives -
    # matches the console, where this animation would be changing expression too
    _face_follows_animation = True
    apply_face_for_frame(action.name, mid, armature)


def update_preset_pick(self, context):
    armature = find_armature(context)
    if armature:
        apply_preset(armature, self.avatar_preset_pick)


def _make_layer_update(feature):
    def _update(self, context):
        global _face_follows_animation
        apply_layer(self, feature, int(getattr(self, "avatar_" + feature)))
        _face_follows_animation = False  # a manual pick sticks until a new pose is chosen
    return _update


def _make_layer_items(feature):
    def _items(self, context):
        data = _avatar_data.get(self.name)
        if not data:
            return []
        return [(str(i), label, "", i) for (i, label, _p) in data.get("layers", {}).get(feature, [])]
    return _items


class AVATAR_OT_insert_face_keyframe(bpy.types.Operator):
    bl_idname = "avatar.insert_face_keyframe"
    bl_label = "Insert Face Keyframe"
    bl_description = ("Keyframe the current Mouth/Eyebrow/Eye values at this frame, "
                       "in the current action - shows up as real keyframes in the "
                       "Dope Sheet, just like any other animated property")

    def execute(self, context):
        armature = find_armature(context)
        if not armature:
            self.report({"WARNING"}, "No armature found")
            return {"CANCELLED"}
        if armature.animation_data is None or armature.animation_data.action is None:
            self.report({"WARNING"}, "Pick a pose/action first")
            return {"CANCELLED"}
        action = armature.animation_data.action
        for feature in FEATURES:
            if not hasattr(armature, "avatar_" + feature):
                continue
            armature.keyframe_insert(data_path="avatar_" + feature, frame=context.scene.frame_current)
            fc = find_action_fcurve(action, "avatar_" + feature)
            if fc:
                for kp in fc.keyframe_points:
                    kp.interpolation = "CONSTANT"
        return {"FINISHED"}


def _action_enum_items(self, context):
    armature = find_armature(context)
    if armature is None:
        return []
    data = _avatar_data.get(armature.name)
    return data["action_items"] if data else []


def _preset_enum_items(self, context):
    armature = find_armature(context)
    if armature is None:
        return []
    data = _avatar_data.get(armature.name)
    return data["preset_items"] if data else []


class AVATAR_OT_follow_animation(bpy.types.Operator):
    bl_idname = "avatar.follow_animation"
    bl_label = "Let Animation Drive Face Again"
    bl_description = ("Undo a manual face pick and let the current pose's "
                       "baked expression track control the face again")

    def execute(self, context):
        global _face_follows_animation
        _face_follows_animation = True
        armature = find_armature(context)
        if armature and armature.animation_data and armature.animation_data.action:
            apply_face_for_frame(armature.animation_data.action.name, context.scene.frame_current, armature)
        return {"FINISHED"}


class AVATAR_PT_picker(bpy.types.Panel):
    bl_label = "Avatar Exporter"
    bl_idname = "AVATAR_PT_picker"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Avatar"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        armature = find_armature(context)

        armatures = [o for o in scene.objects if o.type == "ARMATURE"]
        if len(armatures) > 1 and hasattr(scene, "avatar_active_armature"):
            layout.prop(scene, "avatar_active_armature", text="Avatar")
            layout.separator()

        if armature is None:
            layout.label(text="No avatar armature in scene", icon="ERROR")
            return
        data = _avatar_data.get(armature.name)
        if not data:
            layout.label(text="Not set up for this avatar yet", icon="ERROR")
            return

        if hasattr(scene, "avatar_action_pick"):
            layout.label(text="Animation")
            layout.template_icon_view(scene, "avatar_action_pick", show_labels=True, scale=5.0)
            act_name = armature.animation_data.action.name if (armature.animation_data and armature.animation_data.action) else None
            if act_name in data.get("face_keyframes", {}):
                note = layout.row()
                note.enabled = not _face_follows_animation
                note.operator("avatar.follow_animation", icon="FILE_REFRESH")
                layout.label(text=("pose drives the face automatically" if _face_follows_animation
                                    else "face pick overrides this pose"), icon="INFO")

        if hasattr(scene, "avatar_preset_pick") and data.get("preset_items"):
            layout.separator()
            layout.label(text="Preset")
            layout.template_icon_view(scene, "avatar_preset_pick", show_labels=True, scale=5.0)

        if all(hasattr(armature, "avatar_" + f) for f in FEATURES):
            layout.separator()
            box = layout.box()
            box.label(text="Mouth / Eyebrow / Eye", icon="KEYTYPE_KEYFRAME_VEC")
            for feature in FEATURES:
                box.prop(armature, "avatar_" + feature, text=feature.capitalize())
            box.operator("avatar.insert_face_keyframe", icon="KEY_HLT", text="Insert Keyframe Here")


def setup_pickers(armature, actions, out_dir):
    """Registers a Blender-side panel (3D viewport > N-panel > Avatar tab):
    clickable animation/preset thumbnail grids, and keyframeable Mouth/
    Eyebrow/Eye properties on the armature. Scoped to this specific armature
    so multiple imported avatars don't clobber each other's data.
    Interactive-mode only - meaningless in --background runs since there is
    no UI to show it in.

    Deliberately does NOT touch the Head material here. The glTF the console
    exports already ships a Head_bakedface_*.png texture - a real GPU render
    from the console's own shader, pixel-correct by construction - and that's
    exactly what's showing right out of the import. build_head_material()'s
    live node-graph is a reconstruction of that same shader math meant only
    to let mouth/eyebrow/eye be swapped afterward (the bake is frozen at
    whatever expression it was baked with); it's a strictly worse
    approximation of the *default* look, so it's only built lazily, in
    apply_layer(), the first time something actually swaps an expression."""
    import bpy.utils.previews

    head_obj = find_head_object(armature)
    data = _avatar_data.setdefault(armature.name, {})
    data["out_dir"] = out_dir
    armature["avatar_out_dir"] = out_dir
    if "face_keyframes" not in data:
        data["face_keyframes"] = parse_face_keyframes(out_dir)
    if data["face_keyframes"]:
        register_frame_handler()
    data["presets"] = parse_face_presets(out_dir)
    data["layers"] = {f: list_feature_layers(out_dir, f) for f in FEATURES}

    if head_obj is not None and any(data["layers"].values()):
        for feature in FEATURES:
            if not hasattr(bpy.types.Object, "avatar_" + feature):
                setattr(bpy.types.Object, "avatar_" + feature, bpy.props.EnumProperty(
                    items=_make_layer_items(feature), update=_make_layer_update(feature),
                    name=feature.capitalize()))
    else:
        log("no per-feature face textures found for this export - skipping the live head material")

    for key in ("action_pcoll", "preset_pcoll"):
        if key in data:
            bpy.utils.previews.remove(data[key])

    thumb_dir = generate_action_thumbnails(armature, actions, out_dir)
    apcoll = bpy.utils.previews.new()
    items = []
    for i, a in enumerate(actions):
        thumb_path = os.path.join(thumb_dir, a.name + ".png")
        icon_id = 0
        if os.path.isfile(thumb_path):
            icon_id = apcoll.load(a.name, thumb_path, "IMAGE").icon_id
        items.append((a.name, a.name, "", icon_id, i))
    data["action_pcoll"] = apcoll
    data["action_items"] = items

    faces_dir = os.path.join(out_dir, "faces")
    if data["presets"] and os.path.isdir(faces_dir):
        ppcoll = bpy.utils.previews.new()
        pitems = []
        for i, name in enumerate(data["presets"]):
            icon_path = os.path.join(faces_dir, "face_%s.png" % name)
            icon_id = ppcoll.load(name, icon_path, "IMAGE").icon_id if os.path.isfile(icon_path) else 0
            pitems.append((name, name, "", icon_id, i))
        data["preset_pcoll"] = ppcoll
        data["preset_items"] = pitems
        if not hasattr(bpy.types.Scene, "avatar_preset_pick"):
            bpy.types.Scene.avatar_preset_pick = bpy.props.EnumProperty(
                items=_preset_enum_items, update=update_preset_pick, name="Preset")
    else:
        data["preset_items"] = []

    if not hasattr(bpy.types.Scene, "avatar_action_pick"):
        bpy.types.Scene.avatar_action_pick = bpy.props.EnumProperty(
            items=_action_enum_items, update=update_action_pick, name="Animation")
    if not hasattr(bpy.types.Scene, "avatar_active_armature"):
        bpy.types.Scene.avatar_active_armature = bpy.props.EnumProperty(
            items=_armature_enum_items, name="Active Avatar")

    for cls in (AVATAR_OT_follow_animation, AVATAR_OT_insert_face_keyframe, AVATAR_PT_picker):
        try:
            bpy.utils.register_class(cls)
        except ValueError:
            pass  # already registered from a previous run in this session

    log("picker panel ready for '%s': 3D viewport > N-panel > 'Avatar' tab" % armature.name)


def main():
    hint, render_out, save_out, action_name = parse_args()
    gltf_path = find_gltf(hint)
    if not gltf_path:
        log("ERROR: could not find avatar.gltf. Pass a path: blender --python %s -- /path/to/AvatarExtract" % os.path.basename(__file__))
        sys.exit(1)
    log("importing", gltf_path)

    out_dir = os.path.dirname(gltf_path)

    clear_scene()
    import_gltf(gltf_path)

    armature = find_armature()
    chosen = None
    if armature:
        _avatar_data[armature.name] = {"face_keyframes": parse_face_keyframes(out_dir), "out_dir": out_dir}
        chosen = bind_action(armature, action_name)
        build_body_materials(armature, out_dir)
    else:
        log("no armature found (static mesh import?) - skipping animation binding")

    # bound_box can be stale/unevaluated until the depsgraph refreshes at least
    # once (frame_set() inside bind_action() happens to trigger that when an
    # action was bound, but with no animations in the file - e.g. "Anims: Off"
    # exports - nothing forces it, and camera framing reads garbage bounds).
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()

    center, radius = scene_bounds()
    setup_camera_and_lights(center, radius)

    # frame all mesh + armature objects for a 3D-viewport-free "it worked" summary
    verts = sum(len(o.data.vertices) for o in bpy.context.scene.objects if o.type == "MESH")
    log("scene ready: %d mesh objects, %d verts, armature=%s" % (
        sum(1 for o in bpy.context.scene.objects if o.type == "MESH"),
        verts, "yes" if armature else "no"))

    if armature and bpy.data.actions:
        setup_pickers(armature, list(bpy.data.actions), out_dir)
        if chosen:
            apply_face_for_frame(chosen.name, bpy.context.scene.frame_current, armature)

    if save_out:
        save_path = save_out if os.path.isabs(save_out) else os.path.join(out_dir, save_out)
        bpy.ops.wm.save_as_mainfile(filepath=save_path)
        log("saved", save_path)

    if render_out:
        render_path = render_out if os.path.isabs(render_out) else os.path.join(out_dir, render_out)
        scene = bpy.context.scene
        try:
            scene.render.engine = "BLENDER_EEVEE_NEXT"      # Blender 4.2+
        except TypeError:
            scene.render.engine = "BLENDER_EEVEE"            # older versions
        scene.render.resolution_x = 1280
        scene.render.resolution_y = 720
        scene.render.filepath = render_path
        bpy.ops.render.render(write_still=True)
        log("rendered", render_path)

    log("done.")


# ---------------------------------------------------------------- add-on path
class AVATAR_OT_import_gltf(bpy.types.Operator, ImportHelper):
    """Import an Xbox 360 AvatarExtract export into the CURRENT scene - unlike
    the standalone script, this never clears anything. Pick avatar.gltf inside
    the AvatarExtract folder pulled off the console. Import more than one and
    the Avatar tab's "Avatar" dropdown lets you switch which one it's editing."""
    bl_idname = "avatar.import_gltf"
    bl_label = "Import Xbox 360 Avatar"
    bl_description = "Import an AvatarExtract export (mesh, rig, animations, live-composited face) into the current scene"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".gltf"
    filter_glob: bpy.props.StringProperty(default="avatar.gltf;*.gltf", options={"HIDDEN"})

    bind_pose: bpy.props.BoolProperty(
        name="Bind a Pose", default=True,
        description="Bind one system animation onto the armature immediately after import")
    add_camera_light: bpy.props.BoolProperty(
        name="Add Camera && Lights", default=False,
        description="Add a framed camera and simple 3-point lighting - usually leave this off, your project likely already has its own")
    setup_avatar_pickers: bpy.props.BoolProperty(
        name="Set Up Picker Panel", default=True,
        description="Register the Avatar tab (3D viewport N-panel): clickable animation/preset thumbnails, the live head material, and keyframeable Mouth/Eyebrow/Eye properties")

    def execute(self, context):
        gltf_path = self.filepath
        if not os.path.isfile(gltf_path):
            self.report({"ERROR"}, "No such file: %s" % gltf_path)
            return {"CANCELLED"}
        out_dir = os.path.dirname(gltf_path)

        before = set(bpy.data.objects)
        import_gltf(gltf_path)
        imported = [o for o in bpy.data.objects if o not in before]
        if not imported:
            self.report({"WARNING"}, "Import produced no new objects")
            return {"CANCELLED"}

        for o in context.selected_objects:
            o.select_set(False)
        for o in imported:
            o.select_set(True)
        context.view_layer.objects.active = next((o for o in imported if o.type == "ARMATURE"), imported[0])

        armature = next((o for o in imported if o.type == "ARMATURE"), None)
        chosen = None
        if armature:
            _avatar_data[armature.name] = {"face_keyframes": parse_face_keyframes(out_dir), "out_dir": out_dir}
            if self.bind_pose:
                chosen = bind_action(armature, None)
            build_body_materials(armature, out_dir)

        if armature and self.add_camera_light:
            context.view_layer.update()
            center, radius = scene_bounds(imported)
            setup_camera_and_lights(center, radius)

        if armature and bpy.data.actions and self.setup_avatar_pickers:
            setup_pickers(armature, list(bpy.data.actions), out_dir)
            if chosen:
                apply_face_for_frame(chosen.name, context.scene.frame_current, armature)

        self.report({"INFO"}, "Imported %d object(s) from %s" % (len(imported), out_dir))
        log("imported into existing scene:", len(imported), "objects from", out_dir)
        return {"FINISHED"}


def menu_func_import(self, context):
    self.layout.operator(AVATAR_OT_import_gltf.bl_idname, text="Xbox 360 Avatar (.gltf)")


_addon_classes = (
    AVATAR_OT_follow_animation,
    AVATAR_OT_insert_face_keyframe,
    AVATAR_PT_picker,
    AVATAR_OT_import_gltf,
)


def register():
    for cls in _addon_classes:
        try:
            bpy.utils.register_class(cls)
        except ValueError:
            pass
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    for cls in reversed(_addon_classes):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass


if __name__ == "__main__":
    main()
