import sys, os, re, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _remesh
_lib = _remesh.load()
readREMesh = _lib.readREMesh
mod_p, tgt_p, out_p = sys.argv[1:4]
a, t, b = readREMesh(mod_p), readREMesh(tgt_p), readREMesh(out_p)
names = lambda m: [m.rawNameList[m.boneNameRemapList[i]] for i in range(m.skeletonHeader.boneCount)]
na, nt, nb = names(a), names(t), names(b)
ok = True
def check(label, cond, extra=""):
    global ok; ok &= bool(cond); print("  %-58s %s %s" % (label, "ok" if cond else "FAIL", extra))

check("joint list is the target slot's, same order (%d)" % len(nt), nb == nt)
mat = lambda m, which: np.array([x.matrix for x in getattr(m.skeletonHeader, which)], dtype="f4")
check("local/world/inverse matrices identical to the target's",
      all(np.array_equal(mat(b, w), mat(t, w)) for w in ("localMatList", "worldMatList", "inverseMatList")))
info = lambda m: [(x.boneParent, x.boneChild, x.boneSibling) for x in m.skeletonHeader.boneInfoList]
check("joint hierarchy identical to the target's", info(b) == info(t))
tt = list(t.skeletonHeader.boneRemapList); bt = list(b.skeletonHeader.boneRemapList)
check("weighted table begins with retail's, entry for entry (%d of %d)" % (len(tt), len(bt)), bt[:len(tt)] == tt)

def buffers(m):
    mb = m.meshBufferHeader; els = {e.typing: e for e in mb.vertexElementList}
    n = (els[1].posStartOffset - els[0].posStartOffset) // els[0].stride      # all LODs
    raw = bytes(mb.vertexBuffer)
    w = np.frombuffer(raw[els[4].posStartOffset:els[4].posStartOffset + n*16], dtype=np.uint8).reshape(n, 16)
    pos = np.frombuffer(raw[:n*12], dtype="<f4").reshape(n, 3)
    return n, raw, els, w[:, :8], w[:, 8:], pos
n, ra, ea, ia, wa, pa = buffers(a); n2, rb, eb, ib, wb, pb = buffers(b)
check("vertex count unchanged (%d)" % n, n == n2)
check("positions, normals/tangents, both UV sets byte-identical", ra[:ea[4].posStartOffset] == rb[:eb[4].posStartOffset])
check("face buffer byte-identical", bytes(a.meshBufferHeader.faceBuffer) == bytes(b.meshBufferHeader.faceBuffer))
check("every vertex's weights still total what they did", np.array_equal(wa.sum(axis=1), wb.sum(axis=1)))
check("every bone index within the weighted table (%d)" % b.skeletonHeader.remapCount, int(ib[wb > 0].max()) < b.skeletonHeader.remapCount)

# independent re-derivation of what each vertex should now be weighted to
tset = set(nt); par = {na[i]: (na[x.boneParent] if 0 <= x.boneParent < len(na) else None) for i, x in enumerate(a.skeletonHeader.boneInfoList)}
def expect(bone, w):
    if bone in tset: return {bone: w}
    mm = re.match(r"^([lr])_leg_(front|back|side)_muscle$", bone)
    if mm:
        tt = {nt[i] for i in t.skeletonHeader.boneRemapList}
        thigh = next(j for j in (mm.group(1) + "_leg_femur_twist_0_H", mm.group(1) + "_leg_femur") if j in tt or j.endswith("femur"))
        return {"hips": w // 2, thigh: w - w // 2}
    p = par[bone]
    while p not in tset: p = par[p]
    return {p: w}
wa_names = [na[i] for i in a.skeletonHeader.boneRemapList]; wb_names = [nb[i] for i in b.skeletonHeader.boneRemapList]
bad = 0
for v in range(n):
    want = {}
    for k in range(8):
        if wa[v, k]:
            for bn, w in expect(wa_names[ia[v, k]], int(wa[v, k])).items(): want[bn] = want.get(bn, 0) + w
    got = {}
    for k in range(8):
        if wb[v, k]: got[wb_names[ib[v, k]]] = got.get(wb_names[ib[v, k]], 0) + int(wb[v, k])
    bad += ({k: v_ for k, v_ in want.items() if v_} != got)
check("all %d vertices carry exactly the re-derived influences" % n, bad == 0, "(%d mismatches)" % bad if bad else "")

# bind-pose skinning with the NEW skeleton must reproduce every vertex position
W, I = mat(b, "worldMatList"), mat(b, "inverseMatList")
remap = np.array(b.skeletonHeader.boneRemapList)
hp = np.concatenate([pb, np.ones((n, 1), "f4")], axis=1)
skinned = np.zeros((n, 3), "f8")
for k in range(8):
    bones = remap[ib[:, k]]
    M = np.einsum("nij,njk->nik", I[bones], W[bones])        # row-vector convention: p * inverse * world
    skinned += (wb[:, k, None] / wb.sum(axis=1, keepdims=True)) * np.einsum("ni,nij->nj", hp, M)[:, :3]
err = float(np.abs(skinned - pb).max())
check("bind-pose skinning reproduces every vertex (max error %.2e)" % err, err < 1e-3)
check("declared file size matches", b.fileHeader.fileSize == os.path.getsize(out_p))
print("\n%s" % ("ALL CHECKS PASSED" if ok else "FAILED - do not install"))
