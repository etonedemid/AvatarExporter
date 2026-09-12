#!/bin/bash
# Double-click (or run) this to open Blender with the avatar imported, rigged,
# lit, and posed with an animation already bound. Drag an AvatarExtract folder
# onto it if the script can't auto-find avatar.gltf on its own.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
blender --python "$DIR/blender_import_avatar.py" -- "$@"
