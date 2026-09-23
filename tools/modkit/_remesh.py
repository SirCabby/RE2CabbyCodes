"""Locate NSACloud's RE-Mesh-Editor and import its pure-Python mesh library (no bpy needed).

The addon's own __init__.py imports bpy, so its `modules` folder is imported as a top-level
package instead.  Looked for in $RE_MESH_EDITOR, then any Blender user addons folder.
    git clone https://github.com/NSACloud/RE-Mesh-Editor   (GPL; not vendored here)
"""
import glob, os, sys

def load():
    roots = [os.environ.get("RE_MESH_EDITOR", "")]
    roots += sorted(glob.glob(os.path.expanduser("~/.config/blender/*/scripts/addons/RE*Mesh*Editor*")), reverse=True)
    for root in roots:
        if root and os.path.isdir(os.path.join(root, "modules", "mesh")):
            if root not in sys.path:
                sys.path.insert(0, root)
            from modules.mesh import file_re_mesh
            return file_re_mesh
    sys.exit("RE-Mesh-Editor not found: set RE_MESH_EDITOR to a checkout of github.com/NSACloud/RE-Mesh-Editor")
