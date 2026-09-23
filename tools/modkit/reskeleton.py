"""Put a mod body's geometry onto another look's retail skeleton.

Each look of a character (Ada's dress / trench coat / bandaged, Sherry's uniform / jacket /
classic) has its OWN skeleton - different joint counts, different joint order - and every
asset of that look (figure motion, cutscene motion lists, jcns rig, GPU cloth) is authored
against it.  A body mesh copied into another look's slot keeps the skeleton of the look it
came from; the engine binds it by name the first time and gets it right, then scrambles the
upper joints whenever the look is re-enabled.  So a transplant must carry the TARGET slot's
skeleton, exactly as retail has it.

Geometry is untouched.  Bind poses of shared joints are identical across a character's looks,
so skinning is exact for every joint both skeletons have; a vertex weighted to a joint the
target lacks is handed to the nearest ancestor that exists there (thigh-muscle helpers, which
hang off the hips but shape the thigh, are shared between the hips and the femur).
"""
import sys, os, re, copy, collections
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _remesh
_lib = _remesh.load()
readREMesh, writeREMesh, getPaddedPos, AABB = _lib.readREMesh, _lib.writeREMesh, _lib.getPaddedPos, _lib.AABB

BONE_INFO_SIZE, MATRIX_SIZE, AABB_SIZE = 16, 64, 32
MUSCLE = re.compile(r"^([lr])_leg_(front|back|side)_muscle$", re.I)


def bone_names(mesh):
    return [mesh.rawNameList[mesh.boneNameRemapList[i]] for i in range(mesh.skeletonHeader.boneCount)]


def reskeleton(mod_path, target_path, out_path, quiet=False):
    m, t = readREMesh(mod_path), readREMesh(target_path)
    mn, tn = bone_names(m), bone_names(t)
    tset = set(tn)
    # the target mesh's own weighted-joint table: the slot layout every asset of the look was built around
    ttable = [tn[i] for i in t.skeletonHeader.boneRemapList]
    ttable_set = set(ttable)
    parent = {mn[i]: (mn[b.boneParent] if 0 <= b.boneParent < len(mn) else None)
              for i, b in enumerate(m.skeletonHeader.boneInfoList)}

    def targets(bone):
        if bone in tset:
            return [(bone, 1.0)]
        mm = MUSCLE.match(bone)
        if mm and "hips" in tset:
            # prefer a thigh joint the target mesh itself weights, so its table needs no additions
            for thigh in (mm.group(1) + "_leg_femur_twist_0_H", mm.group(1) + "_leg_femur"):
                if thigh in ttable_set:
                    return [("hips", 0.5), (thigh, 0.5)]
            if (mm.group(1) + "_leg_femur") in tset:
                return [("hips", 0.5), (mm.group(1) + "_leg_femur", 0.5)]
        a = parent.get(bone)
        while a is not None and a not in tset:
            a = parent.get(a)
        assert a is not None, "no ancestor of %s exists in the target skeleton" % bone
        return [(a, 1.0)]

    old_weighted = [mn[i] for i in m.skeletonHeader.boneRemapList]
    moved = {b: targets(b) for b in old_weighted if b not in tset}

    # ---- re-express every vertex's influences in target joints
    mb = m.meshBufferHeader
    els = {e.typing: e for e in mb.vertexElementList}
    # every vertex in the buffer - lower LODs and shadow meshes index the same table as LOD0
    n = (els[1].posStartOffset - els[0].posStartOffset) // els[0].stride
    vb = bytearray(mb.vertexBuffer)
    w0, stride = els[4].posStartOffset, els[4].stride
    per_vertex, used = [], set()
    touched = overflow = 0
    for v in range(n):
        o = w0 + v * stride
        infl = collections.OrderedDict()
        hit = False
        for k in range(8):
            w = vb[o + 8 + k]
            if not w:
                continue
            tg = targets(old_weighted[vb[o + k]])
            if len(tg) == 1:
                infl[tg[0][0]] = infl.get(tg[0][0], 0) + w
            else:
                a = w // 2
                infl[tg[0][0]] = infl.get(tg[0][0], 0) + a
                infl[tg[1][0]] = infl.get(tg[1][0], 0) + (w - a)
            hit |= old_weighted[vb[o + k]] not in tset
        items = sorted(((b, w) for b, w in infl.items() if w), key=lambda x: -x[1])
        if len(items) > 8:                       # fold the smallest into the largest
            overflow += 1
            extra = sum(w for _, w in items[8:])
            items = [(items[0][0], items[0][1] + extra)] + items[1:8]
        touched += hit
        used.update(b for b, _ in items)
        per_vertex.append(items)

    # The weighted table IS the skinning palette layout, and the look's GPU cloth writes its driven
    # joints into the slots retail gave them.  So: retail's table first, entry for entry, then anything
    # extra this body needs.  (A shorter table put `head` where the cloth expects its first coat joint
    # and let the rest of its writes run off the end, into the head and hair parts' palettes.)
    extras = [b for b in tn if b in used and b not in ttable_set]
    new_weighted = ttable + extras
    slot = {b: i for i, b in enumerate(new_weighted)}
    assert len(new_weighted) <= 256
    for v, items in enumerate(per_vertex):
        o = w0 + v * stride
        for k in range(8):
            vb[o + k], vb[o + 8 + k] = (slot[items[k][0]], items[k][1]) if k < len(items) else (0, 0)
    mb.vertexBuffer = vb

    # ---- the target's skeleton, verbatim, with this body's weighted-joint table
    sk = copy.deepcopy(t.skeletonHeader)
    sk.boneRemapList = [tn.index(b) for b in new_weighted]
    sk.remapCount = len(new_weighted)
    m.skeletonHeader = sk
    mats = [m.rawNameList[i] for i in m.materialNameRemapList]
    m.rawNameList = mats + tn
    m.materialNameRemapList = list(range(len(mats)))
    m.boneNameRemapList = [len(mats) + i for i in range(len(tn))]
    m.fileHeader.nameCount = len(m.rawNameList)

    # ---- bone boxes: extent of the vertices each joint influences, relative to the joint
    pos = np.frombuffer(bytes(vb[els[0].posStartOffset:els[0].posStartOffset + n * 12]), dtype="<f4").reshape(n, 3)
    members = collections.defaultdict(list)
    for v, items in enumerate(per_vertex):
        for b, _ in items:
            members[b].append(v)
    boxes = []
    tboxes = {ttable[i]: bx for i, bx in enumerate(t.boneBoundingBoxHeader.bboxList)} if t.boneBoundingBoxHeader else {}
    for b in new_weighted:
        if not members.get(b):                  # a joint only the look's cloth/rig uses: keep retail's box
            boxes.append(copy.deepcopy(tboxes[b]))
            continue
        head = np.array(sk.worldMatList[tn.index(b)].matrix[3][:3], dtype="f4")
        p = pos[members[b]]
        lo, hi = p.min(axis=0) - head, p.max(axis=0) - head
        box = AABB()
        box.min.x, box.min.y, box.min.z, box.min.w = float(lo[0]), float(lo[1]), float(lo[2]), 1.0
        box.max.x, box.max.y, box.max.z, box.max.w = float(hi[0]), float(hi[1]), float(hi[2]), 1.0
        boxes.append(box)
    m.boneBoundingBoxHeader.bboxList = boxes
    m.boneBoundingBoxHeader.count = len(boxes)

    # ---- every offset the layout implies (the writer pads to 16 before each buffer)
    fh = m.fileHeader
    sk.boneHeaderOffset = getPaddedPos(fh.skeletonOffset + 48 + 2 * sk.remapCount, 16)
    sk.boneLocalMatrixOffset = sk.boneHeaderOffset + sk.boneCount * BONE_INFO_SIZE
    sk.boneWorldMatrixOffset = sk.boneLocalMatrixOffset + sk.boneCount * MATRIX_SIZE
    sk.boneInverseMatrixOffset = sk.boneWorldMatrixOffset + sk.boneCount * MATRIX_SIZE
    fh.materialNameRemapOffset = sk.boneInverseMatrixOffset + sk.boneCount * MATRIX_SIZE
    fh.boneNameRemapOffset = getPaddedPos(fh.materialNameRemapOffset + 2 * len(m.materialNameRemapList), 16)
    nxt = getPaddedPos(fh.boneNameRemapOffset + 2 * len(m.boneNameRemapList), 16)
    fh.nameOffsetsOffset = getPaddedPos(nxt + 2 * len(m.blendShapeNameRemapList), 16)
    p = getPaddedPos(fh.nameOffsetsOffset + 8 * len(m.rawNameList), 16)
    m.rawNameOffsetList = []
    for name in m.rawNameList:
        m.rawNameOffsetList.append(p)
        p += len(name.encode("utf-8")) + 1
    fh.aabbOffset = getPaddedPos(p, 16)
    m.boneBoundingBoxHeader.offset = fh.aabbOffset + 16
    fh.meshOffset = getPaddedPos(fh.aabbOffset + 16 + len(boxes) * AABB_SIZE, 16)
    mb.vertexElementOffset = fh.meshOffset + 64
    mb.vertexBufferOffset = getPaddedPos(mb.vertexElementOffset + len(mb.vertexElementList) * 8, 16)
    mb.faceBufferOffset = getPaddedPos(mb.vertexBufferOffset + len(mb.vertexBuffer), 16)
    fh.fileSize = getPaddedPos(mb.faceBufferOffset + len(mb.faceBuffer), 16)

    writeREMesh(m, out_path)
    if fh.fileSize != os.path.getsize(out_path):
        fh.fileSize = os.path.getsize(out_path)
        writeREMesh(m, out_path)
    if not quiet:
        print("   skeleton %d -> %d joints; weighted table %d -> %d (retail's %d, entry for entry%s); "
              "%d of %d vertices re-targeted (%d needed folding)"
              % (len(mn), len(tn), len(old_weighted), len(new_weighted), len(ttable),
                 (" + %d appended: %s" % (len(extras), extras)) if extras else "", touched, n, overflow))
        for b, tg in moved.items():
            print("      %-28s -> %s" % (b, " + ".join("%s %d%%" % (x, f * 100) for x, f in tg)))
    return moved


if __name__ == "__main__":
    reskeleton(sys.argv[1], sys.argv[2], sys.argv[3])
