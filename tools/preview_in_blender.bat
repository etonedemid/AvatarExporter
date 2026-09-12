@echo off
rem Double-click this to open Blender with the avatar imported, rigged, lit,
rem and posed with an animation already bound. Drag an AvatarExtract folder
rem onto it if the script can't auto-find avatar.gltf on its own.
set DIR=%~dp0
blender --python "%DIR%blender_import_avatar.py" -- %*
