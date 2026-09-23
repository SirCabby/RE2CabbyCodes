# modkit - making a skin mod cover every look of a character

Not part of the cheat mod; tools that came out of fixing the Ada and Sherry nude mods (2026-09-18).

**The rule that took a day to find:** every *look* of a character is its own model slot with its **own
skeleton** and its **own skinning-slot layout**, and every asset of that look - figure and cutscene motion,
the `.jcns` rig, chains, GPU cloth - is authored against them. Ada: cocktail dress `pl2000` 176 joints,
trench coat `pl2001` **246** (120 of them `coat_*`, driven by `pl2001.gpuc`), bandaged `pl2002` 176 but
reordered. Sherry: `pl3000` 209, `pl3001` 212, `pl3002` 177, `pl3003` 159.

A body mesh copied into another look's slot keeps the skeleton it came from. The engine binds it by name
on first load and it looks right; when the look is re-enabled (Model Viewer: view another figure, come
back) the upper joints get garbage while root/hips/legs survive. With only the skeleton fixed, the head
and hair still came apart: the mesh's **weighted-joint table is the skinning palette layout**, and the
coat's cloth writes its 118 joints into the slots retail gave them (93-210) - a shorter table put `head`
at slot 94 and let the rest run off the end into the head and hair parts. So a transplant needs:

1. the target slot's retail skeleton, verbatim (bind poses of shared joints are identical across looks);
2. the target mesh's weighted-joint table, entry for entry, extras appended only if unavoidable;
3. the slot's own prefab / `.jcns` / `.chain` / `.gpuc` left alone - they fit again.

Mods that sit in their native slot (Claire's; Ada's in `pl2000`) never show any of this.

    pak.py probe|extract|refs|recover ...          # find, read and search pak entries by path
    reskeleton.py  MOD.mesh  RETAIL_TARGET.mesh  OUT.mesh
    verify_reskel.py MOD.mesh RETAIL_TARGET.mesh OUT.mesh   # independent per-vertex check + bind-pose skinning

`reskeleton.py` needs NSACloud's RE-Mesh-Editor (GPL, not vendored): its `modules/mesh` library is pure
Python. Set `RE_MESH_EDITOR` or install the addon into Blender. Mesh file names must end in the version
(`.mesh.2109108288`). Get retail meshes with `pak.py extract` *before* installing the mod, or afterwards
with `pak.py recover SIZE` (an installed mod zeroes the entry's path hash but leaves the data).

Dead ends, so nobody repeats them: LOD count (Claire's working mod has 1), `skinWeightCount` 1 vs 18,
splitting the single submesh, padding the weighted table without the right skeleton, swapping the look's
prefab or rig for another look's, and appending a rebuilt LOD table at EOF (crashes: the offset array is
inline and read sequentially).
