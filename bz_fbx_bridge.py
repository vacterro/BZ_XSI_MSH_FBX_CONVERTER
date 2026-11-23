import argparse
import json
import math
import os
import struct
import sys
import traceback

import bpy
from mathutils import Matrix, Quaternion, Vector


# Right-handed basis change C = Rx(+90 deg): BZ(X=right, Y=up, Z=fwd) -> Blender(X=right, Y=-Z=fwd, Z=Y=up)
# det(C) = +1 (pure rotation, preserves CCW triangle winding & handedness)
C_MAT = Matrix((
    (1.0,  0.0, 0.0, 0.0),
    (0.0,  0.0, -1.0, 0.0),
    (0.0,  1.0, 0.0, 0.0),
    (0.0,  0.0, 0.0, 1.0),
))
C_INV = C_MAT.transposed()


def bz_vec_to_bl(v):
    return Vector((float(v[0]), -float(v[2]), float(v[1])))


def bz_quat_col_to_bl(q):
    # q is standard column-vector quaternion (w, x, y, z) in BZ space -> rotated by C: (w, x, -z, y)
    return Quaternion((float(q[0]), float(q[1]), -float(q[3]), float(q[2]))).normalized()


def bz_row_mat_to_bl_col(m16):
    # Canonical Mat4 is row-vector (v_out = v_in * M_row); column-vector M_col = M_row^T
    m_row = Matrix((
        (m16[0],  m16[1],  m16[2],  m16[3]),
        (m16[4],  m16[5],  m16[6],  m16[7]),
        (m16[8],  m16[9],  m16[10], m16[11]),
        (m16[12], m16[13], m16[14], m16[15]),
    ))
    m_col = m_row.transposed()
    return C_MAT @ m_col @ C_INV


def load_image_safe(path, colorspace="sRGB"):
    if not path or not os.path.isfile(path):
        return None
    try:
        img = bpy.data.images.load(path, check_existing=True)
        try:
            img.colorspace_settings.name = colorspace
        except Exception:
            pass
        return img
    except Exception:
        return None


def build_material(mdata, include_textures):
    mat = bpy.data.materials.new(name=mdata.get("name") or "bz_material")
    mat.use_nodes = True
    mat.use_backface_culling = not bool(mdata.get("doubleSided", False))
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    if bsdf is None:
        bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")

    diff = mdata.get("diffuse", [0.7, 0.7, 0.7, 1.0])
    spec = mdata.get("specular", [0.5, 0.5, 0.5, 1.0])
    emis = mdata.get("emissive", [0.0, 0.0, 0.0, 1.0])
    spow = float(mdata.get("specularPower", 32.0))
    alpha = float(diff[3]) if len(diff) >= 4 else 1.0

    bsdf.inputs["Base Color"].default_value = (diff[0], diff[1], diff[2], 1.0)
    if "Alpha" in bsdf.inputs:
        bsdf.inputs["Alpha"].default_value = max(0.0, min(1.0, alpha))
    rough = max(0.15, min(0.95, math.sqrt(2.0 / (max(1.0, spow) + 2.0))))
    if "Roughness" in bsdf.inputs:
        bsdf.inputs["Roughness"].default_value = rough
    if "Specular IOR Level" in bsdf.inputs:
        bsdf.inputs["Specular IOR Level"].default_value = max(0.0, min(1.0, (spec[0] + spec[1] + spec[2]) / 3.0))

    if mdata.get("emissiveFlag", False):
        if "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = (diff[0], diff[1], diff[2], 1.0)
        if "Emission Strength" in bsdf.inputs:
            bsdf.inputs["Emission Strength"].default_value = float(mdata.get("emissiveStrength", 1.0))
    elif emis[0] > 0.001 or emis[1] > 0.001 or emis[2] > 0.001:
        if "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = (emis[0], emis[1], emis[2], 1.0)
        if "Emission Strength" in bsdf.inputs:
            bsdf.inputs["Emission Strength"].default_value = 1.0

    if include_textures:
        diff_path = mdata.get("diffuseTex", "")
        img = load_image_safe(diff_path, "sRGB")
        if img is not None:
            tn = nt.nodes.new("ShaderNodeTexImage")
            tn.image = img
            tn.location = (-400, 200)
            nt.links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
            if mdata.get("hasAlpha", False) and "Alpha" in bsdf.inputs:
                nt.links.new(tn.outputs["Alpha"], bsdf.inputs["Alpha"])

        emis_path = mdata.get("emissiveTex", "")
        eimg = load_image_safe(emis_path, "sRGB")
        if eimg is not None and "Emission Color" in bsdf.inputs:
            en = nt.nodes.new("ShaderNodeTexImage")
            en.image = eimg
            en.location = (-400, -100)
            nt.links.new(en.outputs["Color"], bsdf.inputs["Emission Color"])
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = 1.0

        norm_path = mdata.get("normalTex", "")
        nimg = load_image_safe(norm_path, "Non-Color")
        if nimg is not None and "Normal" in bsdf.inputs:
            nn = nt.nodes.new("ShaderNodeTexImage")
            nn.image = nimg
            nn.location = (-600, -400)
            nmap = nt.nodes.new("ShaderNodeNormalMap")
            nmap.location = (-300, -400)
            nt.links.new(nn.outputs["Color"], nmap.inputs["Color"])
            nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])

        spec_path = mdata.get("specularTex", "")
        simg = load_image_safe(spec_path, "Non-Color")
        if simg is not None and "Specular IOR Level" in bsdf.inputs:
            sn = nt.nodes.new("ShaderNodeTexImage")
            sn.image = simg
            sn.location = (-400, -650)
            nt.links.new(sn.outputs["Color"], bsdf.inputs["Specular IOR Level"])

    # Preserve BZCC metadata without contaminating diffuse
    if mdata.get("teamColorTex"):
        mat["bz_teamColorTexture"] = mdata["teamColorTex"]
    if mdata.get("materialFileRef"):
        mat["bz_materialFile"] = mdata["materialFileRef"]
    mat["bz_specularPower"] = spow
    return mat


def build_scene_from_payload(payload, bin_bytes):
    global CANONICAL_NODES
    CANONICAL_NODES = payload.get("nodes", [])
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 20

    opts = payload.get("options", {})
    inc_mats = bool(opts.get("includeMaterials", True))
    inc_tex = bool(opts.get("includeTextures", True))
    inc_anim = bool(opts.get("includeAnimations", True))
    preserve_hier = bool(opts.get("preserveHierarchy", True))

    bl_materials = []
    if inc_mats:
        for mdata in payload.get("materials", []):
            bl_materials.append(build_material(mdata, inc_tex))

    nodes_data = payload.get("nodes", [])
    meshes_data = payload.get("meshes", [])
    anims_data = payload.get("animations", [])

    # Check if scene has skinned meshes
    has_skin = any(m.get("isSkinned", False) for m in meshes_data)
    bone_indices = [i for i, nd in enumerate(nodes_data) if nd.get("isBone", False)]
    if has_skin and not bone_indices:
        has_skin = False

    # Precompute Blender local & world matrices for every node
    local_mats = [bz_row_mat_to_bl_col(nd["localMatrix"]) for nd in nodes_data]
    base_mats = [
        bz_row_mat_to_bl_col(nd["basePoseMatrix"]) if nd.get("hasBasePose") else local_mats[i]
        for i, nd in enumerate(nodes_data)
    ]
    world_mats = [Matrix.Identity(4)] * len(nodes_data)
    bind_world_mats = [Matrix.Identity(4)] * len(nodes_data)
    computed = [False] * len(nodes_data)

    def compute_world(idx):
        if computed[idx]:
            return
        computed[idx] = True
        p = nodes_data[idx].get("parent", -1)
        if p >= 0 and p != idx:
            compute_world(p)
            world_mats[idx] = world_mats[p] @ local_mats[idx]
            bind_world_mats[idx] = bind_world_mats[p] @ base_mats[idx]
        else:
            world_mats[idx] = local_mats[idx]
            bind_world_mats[idx] = base_mats[idx]

    for i in range(len(nodes_data)):
        compute_world(i)

    # Create Armature if skinned
    arm_obj = None
    bone_name_by_node = {}
    bone_rest_local = {}
    if has_skin:
        arm_data = bpy.data.armatures.new(payload.get("sourceName", "BZ_Armature") + "_Armature")
        arm_obj = bpy.data.objects.new(payload.get("sourceName", "BZ_Armature") + "_Armature", arm_data)
        scene.collection.objects.link(arm_obj)
        bpy.context.view_layer.objects.active = arm_obj
        bpy.ops.object.mode_set(mode="EDIT")

        edit_bones = {}
        # Compute bone head positions in bind/world space
        for bi in bone_indices:
            nd = nodes_data[bi]
            # Prefer exact inverse(inverseBindMatrix) if provided, else bind_world_mats[bi]
            if nd.get("hasInverseBind"):
                ibm = bz_row_mat_to_bl_col(nd["inverseBindMatrix"])
                try:
                    bw = ibm.inverted()
                except Exception:
                    bw = bind_world_mats[bi]
            else:
                bw = bind_world_mats[bi]
            bind_world_mats[bi] = bw

        for bi in bone_indices:
            nd = nodes_data[bi]
            eb = arm_data.edit_bones.new(nd["name"])
            bone_name_by_node[bi] = eb.name
            edit_bones[bi] = eb
            head = bind_world_mats[bi].to_translation()
            eb.head = head

            # Choose a clean non-zero tail pointing toward children average or along local +Y
            child_bones = [c for c in nd.get("children", []) if c in bone_indices]
            tail = None
            if child_bones:
                avg = Vector((0.0, 0.0, 0.0))
                for c in child_bones:
                    avg += bind_world_mats[c].to_translation()
                avg /= len(child_bones)
                if (avg - head).length > 1e-4:
                    tail = avg
            if tail is None:
                axis = (bind_world_mats[bi].to_3x3() @ Vector((0.0, 0.25, 0.0)))
                if axis.length < 1e-4:
                    axis = Vector((0.0, 0.0, 0.25))
                tail = head + axis
            eb.tail = tail

        for bi in bone_indices:
            p = nodes_data[bi].get("parent", -1)
            if p in edit_bones:
                edit_bones[bi].parent = edit_bones[p]

        # Capture rest local matrices of edit bones for exact pose_basis conversion!
        for bi in bone_indices:
            eb = edit_bones[bi]
            if eb.parent is not None:
                bone_rest_local[bi] = eb.parent.matrix.inverted() @ eb.matrix
            else:
                bone_rest_local[bi] = eb.matrix.copy()

        bpy.ops.object.mode_set(mode="OBJECT")

    # Helper to construct a Blender Mesh from binary slices
    def make_bl_mesh(mdata):
        nv = int(mdata["vertexCount"])
        ni = int(mdata["indexCount"])
        p_off = int(mdata["posOffset"])
        n_off = int(mdata["normOffset"])
        uv_off = int(mdata["uvOffset"])
        c_off = int(mdata["colOffset"])
        i_off = int(mdata["idxOffset"])
        tm_off = int(mdata["triMatOffset"])

        pos_raw = struct.unpack_from(f"<{nv * 3}f", bin_bytes, p_off)
        norm_raw = struct.unpack_from(f"<{nv * 3}f", bin_bytes, n_off)
        uv_raw = struct.unpack_from(f"<{nv * 2}f", bin_bytes, uv_off)
        col_raw = struct.unpack_from(f"<{nv * 4}f", bin_bytes, c_off)
        idx_raw = struct.unpack_from(f"<{ni}I", bin_bytes, i_off)
        ntris = ni // 3
        tmat_raw = struct.unpack_from(f"<{ntris}i", bin_bytes, tm_off) if ntris > 0 else ()

        verts = [(pos_raw[i*3], -pos_raw[i*3+2], pos_raw[i*3+1]) for i in range(nv)]
        faces = [(idx_raw[t*3], idx_raw[t*3+1], idx_raw[t*3+2]) for t in range(ntris)]

        me = bpy.data.meshes.new(mdata.get("name") or "mesh")
        me.from_pydata(verts, [], faces)
        me.update()

        # Assign materials per polygon
        if inc_mats and bl_materials:
            used_mats = []
            used_map = {}
            for mi in tmat_raw:
                if 0 <= mi < len(bl_materials) and mi not in used_map:
                    used_map[mi] = len(used_mats)
                    used_mats.append(mi)
                    me.materials.append(bl_materials[mi])
            for poly_i, poly in enumerate(me.polygons):
                if poly_i < len(tmat_raw) and tmat_raw[poly_i] in used_map:
                    poly.material_index = used_map[tmat_raw[poly_i]]

        # UVs (per loop)
        if mdata.get("hasUvs", False) and len(me.loops) > 0:
            uv_layer = me.uv_layers.new(name="UVMap")
            uv_data = uv_layer.data
            for li, loop in enumerate(me.loops):
                vi = loop.vertex_index
                uv_data[li].uv = (uv_raw[vi*2], uv_raw[vi*2+1])

        # Vertex Colors (Blender 4.x/5.x Color Attribute Byte Color on Corner)
        if mdata.get("hasVertexColors", False) and len(me.loops) > 0:
            ca = me.color_attributes.new(name="Col", type="BYTE_COLOR", domain="CORNER")
            for li, loop in enumerate(me.loops):
                vi = loop.vertex_index
                ca.data[li].color = (col_raw[vi*4], col_raw[vi*4+1], col_raw[vi*4+2], col_raw[vi*4+3])

        # Custom split normals (Blender 4.1+ / 5.0 API)
        if mdata.get("hasNormals", False) and len(me.loops) > 0:
            loop_normals = []
            for loop in me.loops:
                vi = loop.vertex_index
                nx, ny, nz = norm_raw[vi*3], -norm_raw[vi*3+2], norm_raw[vi*3+1]
                ln = math.sqrt(nx*nx + ny*ny + nz*nz)
                if ln > 1e-8:
                    loop_normals.append((nx/ln, ny/ln, nz/ln))
                else:
                    loop_normals.append((0.0, 0.0, 1.0))
            try:
                me.normals_split_custom_set(loop_normals)
            except Exception:
                pass
        return me

    # Create Blender Objects for nodes
    bl_objs = [None] * len(nodes_data)
    for ni, nd in enumerate(nodes_data):
        mi = nd.get("meshIndex", -1)
        is_bone = nd.get("isBone", False)
        # Skip empty pure-bone nodes if represented in Armature (keep mesh nodes and hardpoints!)
        if has_skin and is_bone and mi < 0 and not nd.get("isHardpoint", False):
            continue

        obj_data = None
        mdata = None
        if mi >= 0 and mi < len(meshes_data):
            mdata = meshes_data[mi]
            obj_data = make_bl_mesh(mdata)

        obj = bpy.data.objects.new(nd["name"], obj_data)
        scene.collection.objects.link(obj)
        if obj_data is None:
            obj.empty_display_type = "ARROWS"
            obj.empty_display_size = 0.25

        if mdata and mdata.get("verticesInModelSpace", False):
            obj.matrix_world = Matrix.Identity(4)
        elif not preserve_hier:
            obj.matrix_world = world_mats[ni]
        else:
            obj.matrix_local = local_mats[ni]

        # Bind skinning weights & Armature modifier
        if has_skin and arm_obj is not None and mdata and mdata.get("isSkinned", False):
            obj.parent = arm_obj
            obj.matrix_parent_inverse = Matrix.Identity(4)
            nv = int(mdata["vertexCount"])
            j_off = int(mdata["jointOffset"])
            w_off = int(mdata["weightOffset"])
            inf_counts = struct.unpack_from(f"<{nv}I", bin_bytes, int(mdata["influenceCountOffset"]))
            inf_offsets = struct.unpack_from(f"<{nv}I", bin_bytes, int(mdata["influenceOffsetOffset"]))
            vgroups = {}
            max_influences = 0
            for vi in range(nv):
                count = int(inf_counts[vi])
                base = int(inf_offsets[vi])
                max_influences = max(max_influences, count)
                for k in range(count):
                    bn = struct.unpack_from("<i", bin_bytes, j_off + (base + k) * 4)[0]
                    wt = struct.unpack_from("<f", bin_bytes, w_off + (base + k) * 4)[0]
                    if wt > 1e-6 and bn in bone_name_by_node:
                        bname = bone_name_by_node[bn]
                        vg = vgroups.get(bname)
                        if vg is None:
                            vg = obj.vertex_groups.new(name=bname)
                            vgroups[bname] = vg
                        vg.add([vi], wt, "ADD")
            mod = obj.modifiers.new(name="Armature", type="ARMATURE")
            mod.object = arm_obj

        bl_objs[ni] = obj

    # Wire parent-child hierarchy for non-skinned objects
    if preserve_hier:
        for ni, nd in enumerate(nodes_data):
            obj = bl_objs[ni]
            if obj is None or obj.parent is not None:
                continue
            p = nd.get("parent", -1)
            if p >= 0 and bl_objs[p] is not None:
                obj.parent = bl_objs[p]
                obj.matrix_local = local_mats[ni]
            elif p >= 0 and has_skin and arm_obj is not None and p in bone_name_by_node:
                obj.parent = arm_obj
                obj.parent_type = "BONE"
                obj.parent_bone = bone_name_by_node[p]
                obj.matrix_world = world_mats[ni]
            else:
                obj.matrix_world = world_mats[ni]

    # Build Animations (Actions + NLA strips so multiple clips export to FBX!)
    if inc_anim and anims_data:
        min_f = 0
        max_f = 1
        for clip_idx, clip in enumerate(anims_data):
            cname = clip.get("name") or f"Anim_{clip_idx}"
            fps = float(clip.get("fps", 20.0))
            scene.render.fps = max(1, int(round(fps)))
            start_f = int(round(float(clip.get("startFrame", 0.0))))
            end_f = int(round(float(clip.get("endFrame", 1.0))))
            min_f = min(min_f, start_f)
            max_f = max(max_f, end_f)

            arm_action = None
            if arm_obj is not None:
                arm_action = bpy.data.actions.new(name=cname)
                if arm_obj.animation_data is None:
                    arm_obj.animation_data_create()
                arm_obj.animation_data.action = arm_action

            for tr in clip.get("tracks", []):
                ni = int(tr["nodeIndex"])
                t_keys = {float(k[0]): bz_vec_to_bl(k[1]) for k in tr.get("translations", [])}
                r_keys = {float(k[0]): bz_quat_col_to_bl(k[1]) for k in tr.get("rotations", [])}
                s_keys = {float(k[0]): Vector((float(k[1][0]), float(k[1][2]), float(k[1][1]))) for k in tr.get("scales", [])}
                all_frames = sorted(set(t_keys.keys()) | set(r_keys.keys()) | set(s_keys.keys()))
                if not all_frames:
                    continue

                if has_skin and arm_obj is not None and ni in bone_name_by_node:
                    pbone = arm_obj.pose.bones.get(bone_name_by_node[ni])
                    if pbone is None:
                        continue
                    pbone.rotation_mode = "QUATERNION"
                    rest_inv = bone_rest_local[ni].inverted()
                    loc0, rot0, sca0 = local_mats[ni].decompose()
                    for fr in all_frames:
                        lt = t_keys.get(fr, loc0)
                        lr = r_keys.get(fr, rot0)
                        ls = s_keys.get(fr, sca0)
                        m_target = Matrix.LocRotScale(lt, lr, ls)
                        m_basis = rest_inv @ m_target
                        bt, br, bs = m_basis.decompose()
                        pbone.location = bt
                        pbone.rotation_quaternion = br
                        pbone.scale = bs
                        pbone.keyframe_insert(data_path="location", frame=fr)
                        pbone.keyframe_insert(data_path="rotation_quaternion", frame=fr)
                        if s_keys:
                            pbone.keyframe_insert(data_path="scale", frame=fr)
                elif bl_objs[ni] is not None:
                    obj = bl_objs[ni]
                    obj.rotation_mode = "QUATERNION"
                    if obj.animation_data is None:
                        obj.animation_data_create()
                    oact = bpy.data.actions.new(name=f"{cname}_{obj.name}")
                    obj.animation_data.action = oact
                    loc0, rot0, sca0 = local_mats[ni].decompose()
                    for fr in all_frames:
                        if fr in t_keys:
                            obj.location = t_keys[fr]
                            obj.keyframe_insert(data_path="location", frame=fr)
                        elif not t_keys:
                            obj.location = loc0
                        if fr in r_keys:
                            obj.rotation_quaternion = r_keys[fr]
                            obj.keyframe_insert(data_path="rotation_quaternion", frame=fr)
                        if fr in s_keys:
                            obj.scale = s_keys[fr]
                            obj.keyframe_insert(data_path="scale", frame=fr)
                    if len(anims_data) > 1:
                        trk = obj.animation_data.nla_tracks.new()
                        trk.name = cname
                        trk.strips.new(cname, start_f, oact)

            if arm_obj is not None and arm_action is not None and len(anims_data) > 1:
                trk = arm_obj.animation_data.nla_tracks.new()
                trk.name = cname
                trk.strips.new(cname, start_f, arm_action)

        scene.frame_start = min_f
        scene.frame_end = max(min_f + 1, max_f)
        scene.frame_set(min_f)


CANONICAL_NODES = []


def inspect_active_blender_scene():
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mesh_objs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    arm_objs = [o for o in bpy.context.scene.objects if o.type == "ARMATURE"]
    empty_objs = [o for o in bpy.context.scene.objects if o.type == "EMPTY"]

    total_tris = 0
    total_verts = 0
    uv_present = False
    vcol_present = False
    skin_weights = False
    agree = 0
    tested = 0
    lo = [1e30, 1e30, 1e30]
    hi = [-1e30, -1e30, -1e30]
    mat_names = set()

    for obj in mesh_objs:
        ev = obj.evaluated_get(depsgraph)
        me = ev.to_mesh()
        me.calc_loop_triangles()
        wm = obj.matrix_world
        det_neg = wm.determinant() < 0.0
        total_verts += len(me.vertices)
        total_tris += len(me.loop_triangles)
        if len(me.uv_layers) > 0:
            uv_present = True
        if len(me.color_attributes) > 0:
            vcol_present = True
        if len(obj.vertex_groups) > 0 and any(len(v.groups) > 0 for v in obj.data.vertices):
            skin_weights = True
        for m in obj.data.materials:
            if m is not None:
                mat_names.add(m.name)
        for v in me.vertices:
            wp = wm @ v.co
            lo[0] = min(lo[0], wp.x); lo[1] = min(lo[1], wp.y); lo[2] = min(lo[2], wp.z)
            hi[0] = max(hi[0], wp.x); hi[1] = max(hi[1], wp.y); hi[2] = max(hi[2], wp.z)

        nmat = wm.to_3x3()
        for tri in me.loop_triangles:
            p0 = wm @ me.vertices[tri.vertices[0]].co
            p1 = wm @ me.vertices[tri.vertices[1]].co
            p2 = wm @ me.vertices[tri.vertices[2]].co
            g = (p1 - p0).cross(p2 - p0)
            if g.length < 1e-8:
                continue
            n_sum = Vector((0.0, 0.0, 0.0))
            for li in tri.loops:
                n_sum += nmat @ me.loops[li].normal
            if n_sum.length > 1e-8:
                tested += 1
                sign = -1.0 if det_neg else 1.0
                if g.dot(n_sum) * sign > 0.0:
                    agree += 1
        ev.to_mesh_clear()

    bone_count = sum(len(a.data.bones) for a in arm_objs)
    action_count = len(bpy.data.actions)

    hierarchy_signature = []
    def hp(name): return name.lower().startswith("hp_") or "hardpoint" in name.lower()
    for o in bpy.context.scene.objects:
        if hp(o.name):
            continue  # hardpoint attachment links are documented as non-hierarchy
        parent = o.parent.name if o.parent else "-"
        hierarchy_signature.append(f"{o.name}<-{parent}")
    for arm in arm_objs:
        for b in arm.data.bones:
            if hp(b.name):
                continue
            parent = b.parent.name if b.parent else "-"
            hierarchy_signature.append(f"{b.name}<-{parent}")

    # Sample animation at five points per clip; loops may have identical endpoints.
    anim_sampled = False
    anim_motion = 0.0
    if action_count > 0:
        fr0 = bpy.context.scene.frame_start
        fr1 = max(fr0 + 4, bpy.context.scene.frame_end)
        frames = sorted({fr0, fr0 + (fr1-fr0)//4, (fr0+fr1)//2,
                         fr0 + 3*(fr1-fr0)//4, fr1})
        tracked = {}
        for o in bpy.context.scene.objects:
            tracked[o.name] = o
        for arm in arm_objs:
            for pb in arm.pose.bones:
                tracked["bone:" + arm.name + ":" + pb.name] = ("bone", arm, pb.name)
        prev = None
        for fr in frames:
            bpy.context.scene.frame_set(fr)
            bpy.context.view_layer.update()
            cur = {}
            for name, item in tracked.items():
                if isinstance(item, tuple):
                    _, arm, bone_name = item
                    pb = arm.pose.bones.get(bone_name)
                    if pb is None:
                        continue
                    m = arm.matrix_world @ pb.matrix
                else:
                    m = item.matrix_world
                cur[name] = (m.translation.copy(), m.to_scale().copy(), m.to_quaternion().copy())
            if prev is not None:
                for name in cur:
                    if name not in prev:
                        continue
                    p, c = prev[name], cur[name]
                    anim_motion = max(anim_motion, (c[0] - p[0]).length)
                    anim_motion = max(anim_motion, (c[1] - p[1]).length)
                    anim_motion = max(anim_motion, 2.0 * abs(c[2].rotation_difference(p[2]).angle))
            prev = cur
        anim_sampled = True
        bpy.context.scene.frame_set(fr0)

    # Observed-only normalization: FBX collapses bone-parented meshes onto the
    # armature root. This is derived from the imported armature modifier, not
    # from canonical expectations. Both observed and normalized forms are emitted.
    if arm_objs:
        arm = arm_objs[0]
        for obj in bpy.context.scene.objects:
            cur = obj.parent
            reaches_armature = any(m.type == "ARMATURE" and m.object == arm for m in obj.modifiers)
            bone_names = {b.name for b in arm.data.bones}
            hops = 0
            while cur is not None and hops < 64:
                if cur == arm or cur.name in bone_names:
                    reaches_armature = True
                    break
                cur = cur.parent
                hops += 1
            if reaches_armature:
                hierarchy_signature.append(f"{obj.name}<-ARMATURE")
    hierarchy_signature = sorted(set(hierarchy_signature))

    bone_heads_bz = []
    for arm in arm_objs:
        for bone in arm.data.bones:
            head_bl = (arm.matrix_world @ bone.head_local)
            bone_heads_bz.append([head_bl.x, head_bl.z, -head_bl.y])
    bone_heads_bz.sort()

    uv_sum_u = uv_sum_v = uv_min_u = uv_min_v = 1e30
    uv_max_u = uv_max_v = -1e30
    uv_count = 0
    for obj in mesh_objs:
        for layer in obj.data.uv_layers:
            for datum in layer.data:
                u, v = datum.uv
                uv_sum_u += u; uv_sum_v += v
                uv_min_u = min(uv_min_u, u); uv_max_u = max(uv_max_u, u)
                uv_min_v = min(uv_min_v, v); uv_max_v = max(uv_max_v, v)
                uv_count += 1
    uv_checksum = [uv_count, uv_sum_u, uv_sum_v, uv_min_u, uv_max_u, uv_min_v, uv_max_v] if uv_count else [0] * 7

    # Convert Blender bbox back to BZ space (X=X, Y=Z, Z=-Y) for direct comparison!
    if lo[0] <= hi[0]:
        bz_lo = [lo[0], lo[2], -hi[1]]
        bz_hi = [hi[0], hi[2], -lo[1]]
    else:
        bz_lo = [0.0, 0.0, 0.0]
        bz_hi = [0.0, 0.0, 0.0]

    report_data = {
        "objectCount": len(bpy.context.scene.objects),
        "meshCount": len(mesh_objs),
        "emptyCount": len(empty_objs),
        "armatureCount": len(arm_objs),
        "boneCount": bone_count,
        "vertexCount": total_verts,
        "triangleCount": total_tris,
        "materialCount": len(mat_names),
        "uvPresent": uv_present,
        "vertexColorPresent": vcol_present,
        "skinWeightsPresent": skin_weights,
        "actionCount": action_count,
        "animSampled": anim_sampled,
        "animMotion": anim_motion,
        "normalAgreement": (agree / tested) if tested > 0 else 1.0,
        "hierarchySignature": hierarchy_signature,
        "boneHeadsBZ": bone_heads_bz,
        "uvChecksum": uv_checksum,
        "bboxMinBZ": bz_lo,
        "bboxMaxBZ": bz_hi,
        "bboxMinBL": lo if lo[0] <= hi[0] else [0, 0, 0],
        "bboxMaxBL": hi if lo[0] <= hi[0] else [0, 0, 0],
    }
    return report_data


def inspect_uv_corners():
    """Per-vertex UV sets keyed by mesh + BZ position, catching UV permutations."""
    out = {}
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or not obj.data.uv_layers:
            continue
        layer = obj.data.uv_layers.active
        if not layer:
            continue
        uv_by_vertex = {}
        for loop in obj.data.loops:
            v = obj.data.vertices[loop.vertex_index]
            wp = obj.matrix_world @ v.co
            key = f"{obj.name}|{loop.vertex_index}"
            uv_by_vertex.setdefault(key, []).append((round(layer.data[loop.index].uv.x, 4),
                                                      round(layer.data[loop.index].uv.y, 4)))
        for key, vals in uv_by_vertex.items():
            out[key] = sorted(vals)
    return out


def source_uv_corners(payload, bin_bytes):
    out = {}
    for mesh in payload.get("meshes", []):
        if not mesh.get("hasUvs"):
            continue
        nv = int(mesh["vertexCount"])
        pos = struct.unpack_from(f"<{nv*3}f", bin_bytes, int(mesh["posOffset"]))
        uv = struct.unpack_from(f"<{nv*2}f", bin_bytes, int(mesh["uvOffset"]))
        name = mesh.get("name", "mesh")
        for vi in range(nv):
            # canonical V is bottom-left; Blender import uses 1 - V
            key = f"{name}|{vi}"
            out[key] = [(round(uv[vi*2], 4), round(uv[vi*2+1], 4))]
    return out


def compare_uv_corners(src, dst):
    if not src:
        return {"agreement": 1.0, "checked": 0, "mismatches": []}
    matched = 0
    bad = []
    for key, vals in src.items():
        got = dst.get(key, [])
        if got and all(v in got for v in vals):
            matched += 1
        elif len(bad) < 8:
            bad.append(key)
    return {"agreement": matched / len(src), "checked": len(src), "mismatches": bad}


def source_weight_assignments(payload, bin_bytes):
    nodes = payload.get("nodes", [])
    bone_names = {i: nd.get("name", "") for i, nd in enumerate(nodes)}
    per_vertex = {}
    max_inf = 0
    sum_error = 0.0
    weighted = 0
    for mesh in payload.get("meshes", []):
        if not mesh.get("isSkinned"):
            continue
        nv = int(mesh["vertexCount"])
        pos = struct.unpack_from(f"<{nv*3}f", bin_bytes, int(mesh["posOffset"]))
        counts = struct.unpack_from(f"<{nv}I", bin_bytes, int(mesh["influenceCountOffset"]))
        offs = struct.unpack_from(f"<{nv}I", bin_bytes, int(mesh["influenceOffsetOffset"]))
        joints = struct.unpack_from(f"<{sum(counts)}i", bin_bytes, int(mesh["jointOffset"]))
        weights = struct.unpack_from(f"<{sum(counts)}f", bin_bytes, int(mesh["weightOffset"]))
        name = mesh.get("name", "mesh")
        base = 0
        for vi in range(nv):
            sig = []
            total = 0.0
            for k in range(int(counts[vi])):
                bn = bone_names.get(joints[base + k])
                wt = weights[base + k]
                if bn and wt > 1e-5:
                    sig.append((bn, round(wt, 4)))
                    total += wt
            base += int(counts[vi])
            if sig:
                key = f"{name}|{vi}"
                per_vertex[key] = sorted(sig)
                weighted += 1
                max_inf = max(max_inf, len(sig))
                sum_error = max(sum_error, abs(total - 1.0))
    return {"perVertex": per_vertex, "weightedVertices": weighted,
            "maxInfluences": max_inf, "weightSumError": sum_error}


def inspect_weight_assignments():
    """Mesh/vertex-position keyed bone weights. Position key preserves vertex identity."""
    per_vertex = {}
    max_influences = 0
    weighted_vertices = 0
    sum_error = 0.0
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or not obj.vertex_groups:
            continue
        mesh_name = obj.name
        for v in obj.data.vertices:
            assignments = [(obj.vertex_groups[g.group].name, round(g.weight, 4))
                           for g in v.groups if g.weight > 1e-5]
            if not assignments:
                continue
            wp = obj.matrix_world @ v.co
            key = f"{mesh_name}|{v.index}"
            per_vertex[key] = sorted(assignments)
            weighted_vertices += 1
            max_influences = max(max_influences, len(assignments))
            sum_error = max(sum_error, abs(sum(w for _, w in assignments) - 1.0))
    return {
        "perVertex": per_vertex,
        "weightedVertices": weighted_vertices,
        "maxInfluences": max_influences,
        "weightSumError": sum_error,
    }


def compare_weight_assignments(src, dst):
    a = src.get("perVertex", {})
    b = dst.get("perVertex", {})
    if not a:
        return {"agreement": 1.0, "srcMaxInfluences": 0, "dstMaxInfluences": 0,
                "weightSumError": 0.0, "srcWeightedVertices": 0, "dstWeightedVertices": len(b),
                "matchedVertices": 0, "unmatchedSamples": []}
    matched = 0
    unmatched = []
    for key, sig in a.items():
        if key in b and b[key] == sig:
            matched += 1
        elif len(unmatched) < 8:
            unmatched.append(key)
    return {
        "agreement": matched / len(a),
        "srcMaxInfluences": src.get("maxInfluences", 0),
        "dstMaxInfluences": dst.get("maxInfluences", 0),
        "weightSumError": max(src.get("weightSumError", 0.0), dst.get("weightSumError", 0.0)),
        "srcWeightedVertices": src.get("weightedVertices", 0),
        "dstWeightedVertices": dst.get("weightedVertices", 0),
        "matchedVertices": matched,
        "unmatchedSamples": unmatched,
    }


def render_preview_png(out_png_path):
    scene = bpy.context.scene
    # Compute bounds of all meshes
    lo = Vector((1e30, 1e30, 1e30))
    hi = Vector((-1e30, -1e30, -1e30))
    has_geo = False
    for o in scene.objects:
        if o.type == "MESH":
            for corner in o.bound_box:
                wc = o.matrix_world @ Vector(corner)
                lo.x = min(lo.x, wc.x); lo.y = min(lo.y, wc.y); lo.z = min(lo.z, wc.z)
                hi.x = max(hi.x, wc.x); hi.y = max(hi.y, wc.y); hi.z = max(hi.z, wc.z)
                has_geo = True
    if not has_geo:
        return False

    center = (lo + hi) * 0.5
    radius = max(0.5, (hi - lo).length * 0.5)

    cam_data = bpy.data.cameras.new("RegCam")
    cam_obj = bpy.data.objects.new("RegCam", cam_data)
    scene.collection.objects.link(cam_obj)
    scene.camera = cam_obj

    # Fixed deterministic 3/4 front-right-top view
    cam_pos = center + Vector((radius * 1.9, -radius * 2.1, radius * 1.15))
    cam_obj.location = cam_pos
    direction = center - cam_pos
    cam_obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()

    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x = 512
    scene.render.resolution_y = 384
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = out_png_path
    bpy.ops.render.render(write_still=True)

    bpy.data.objects.remove(cam_obj, do_unlink=True)
    bpy.data.cameras.remove(cam_data)
    return os.path.isfile(out_png_path)


def main():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []

    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["export", "validate"], required=True)
    ap.add_argument("--input", help="Canonical scene JSON (for export mode)")
    ap.add_argument("--bin", help="Canonical scene BIN (for export mode)")
    ap.add_argument("--fbx", required=True, help="Target/input FBX path")
    ap.add_argument("--report", required=True, help="Output JSON report path")
    ap.add_argument("--validate-after", action="store_true")
    ap.add_argument("--render-dir", default="")
    args = ap.parse_args(argv)

    report = {"ok": False, "blenderVersion": bpy.app.version_string}
    try:
        if args.mode == "export":
            with open(args.input, "r", encoding="utf-8") as fp:
                payload = json.load(fp)
            with open(args.bin, "rb") as fp:
                bin_bytes = fp.read()

            build_scene_from_payload(payload, bin_bytes)
            src_metrics = inspect_active_blender_scene()
            src_weights = source_weight_assignments(payload, bin_bytes)
            src_uv = source_uv_corners(payload, bin_bytes)
            report["sourceMetrics"] = src_metrics
            report["sourceWeights"] = src_weights
            report["sourceUvCorners"] = src_uv

            if args.render_dir:
                os.makedirs(args.render_dir, exist_ok=True)
                render_preview_png(os.path.join(args.render_dir, "source_scene.png"))

            opts = payload.get("options", {})
            bzcc_axis = bool(opts.get("bzccAxisMode", False))
            inc_anim = bool(opts.get("includeAnimations", True))
            embed_tex = bool(opts.get("embedTextures", True))

            os.makedirs(os.path.dirname(os.path.abspath(args.fbx)), exist_ok=True)
            bpy.ops.export_scene.fbx(
                filepath=args.fbx,
                use_selection=False,
                global_scale=1.0,
                apply_unit_scale=True,
                apply_scale_options="FBX_SCALE_ALL" if bzcc_axis else "FBX_SCALE_NONE",
                use_space_transform=True,
                bake_space_transform=False,
                object_types={"ARMATURE", "EMPTY", "MESH"},
                use_mesh_modifiers=False,
                mesh_smooth_type="FACE",
                colors_type="SRGB",
                add_leaf_bones=True,
                bake_anim=inc_anim,
                bake_anim_use_all_bones=True,
                bake_anim_use_nla_strips=True,
                bake_anim_use_all_actions=True,
                bake_anim_simplify_factor=0.0,
                path_mode="COPY" if embed_tex else "AUTO",
                embed_textures=embed_tex,
                axis_forward="-Y" if bzcc_axis else "-Z",
                axis_up="Z" if bzcc_axis else "Y",
            )
            report["exported"] = os.path.isfile(args.fbx)
            report["fbxBytes"] = os.path.getsize(args.fbx) if os.path.isfile(args.fbx) else 0

            if args.validate_after and report["exported"]:
                bpy.ops.wm.read_factory_settings(use_empty=True)
                bpy.ops.import_scene.fbx(
                    filepath=args.fbx,
                    use_custom_normals=True,
                    use_anim=True,
                    ignore_leaf_bones=False,
                )
                report["reimportMetrics"] = inspect_active_blender_scene()
                dst_weights = inspect_weight_assignments()
                dst_uv = inspect_uv_corners()
                report["reimportWeights"] = dst_weights
                report["reimportUvCorners"] = dst_uv
                report["weightFidelity"] = compare_weight_assignments(src_weights, dst_weights)
                report["uvFidelity"] = compare_uv_corners(src_uv, dst_uv)
                if args.render_dir:
                    render_preview_png(os.path.join(args.render_dir, "reimport_fbx.png"))
            report["ok"] = bool(report["exported"])

        elif args.mode == "validate":
            bpy.ops.wm.read_factory_settings(use_empty=True)
            bpy.ops.import_scene.fbx(
                filepath=args.fbx,
                use_custom_normals=True,
                use_anim=True,
                ignore_leaf_bones=False,
            )
            report["reimportMetrics"] = inspect_active_blender_scene()
            if args.render_dir:
                os.makedirs(args.render_dir, exist_ok=True)
                render_preview_png(os.path.join(args.render_dir, "validate_fbx.png"))
            report["ok"] = True
    except Exception as exc:
        report["ok"] = False
        report["error"] = str(exc)
        report["traceback"] = traceback.format_exc()

    with open(args.report, "w", encoding="utf-8") as fp:
        json.dump(report, fp, indent=2)
    if os.environ.get("BZCONV_DEBUG_REPORT"):
        with open(os.environ["BZCONV_DEBUG_REPORT"], "w", encoding="utf-8") as fp:
            json.dump(report, fp, indent=2)


if __name__ == "__main__":
    main()
