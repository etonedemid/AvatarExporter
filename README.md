# AvatarExporter

Xbox 360 homebrew that reads the signed-in profile's Xbox LIVE avatar straight
from the console's own avatar system, previews it in 3D, and exports it for
use elsewhere (Blender, mainly).

It exports:

- avatar.gltf + avatar.bin - skinned, rigged mesh with system animations baked in
- avatar.obj + avatar.mtl - the same mesh without rig/animation
- faces/face_<name>.png - the head fully rendered for each named facial expression
- textures/ - every raw per-part texture layer (color, decal, intensity-tint, and the per-expression eyebrow/eye/mouth/etc. layers), alpha-correct
- head_material.txt, body_material.txt, face_keyframes.txt, face_presets.txt - the tint/expression data a companion tool uses to recomposite faces and clothing live instead of needing a baked texture for every combination

tools/blender_import_avatar.py imports all of this into Blender: rigs and
poses the avatar, live-composites facial expressions from the raw layers
(matching the console's own shader math), and rebuilds clothing materials
with their custom-color tint and decal layers.


## Run

Copy default.xex to your console (FTP or USB) and launch it as an extracted
xex. Sign in a profile with an avatar on controller 0. Press A to export;
output goes to <drive>:\AvatarExtract\.

## Notes

- Textures save as .dds, not .png - this SDK's D3DXSaveTextureToFileA
  silently drops alpha when saving PNG, even from an uncompressed source.
  The Blender tool parses DDS manually, no codec dependency.
- Eye color comes from a per-layer whole-face bake
  (Head_bakedface_eye_NN.png) rather than being recomposited from a tint
  color - the tint math looks visibly different once run through Blender's
  own lighting versus the console's unlit bake, so the eye uses the
  console's actual rendered pixels instead.
