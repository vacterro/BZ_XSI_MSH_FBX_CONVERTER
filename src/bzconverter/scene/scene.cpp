#include "bzconverter/scene/scene.hpp"
#include <cmath>
#include <functional>

namespace bz {

void Scene::update_world_matrices() {
    std::vector<uint8_t> state(nodes.size(), 0); // 0=unvisited, 1=visiting, 2=done
    std::function<void(int)> compute = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(nodes.size())) return;
        if (state[idx] == 2) return;
        if (state[idx] == 1) {
            add_diag(DiagSeverity::Warning, "HIERARCHY_CYCLE", "Cycle detected at node '" + nodes[idx].name + "', breaking link");
            nodes[idx].parent_index = -1;
            nodes[idx].world_matrix = nodes[idx].local_matrix;
            state[idx] = 2;
            return;
        }
        state[idx] = 1;
        int p = nodes[idx].parent_index;
        if (p >= 0 && p < static_cast<int>(nodes.size()) && p != idx) {
            compute(p);
            // Row-vector composition: v_world = (v_local * M_local) * M_parent_world
            nodes[idx].world_matrix = nodes[idx].local_matrix * nodes[p].world_matrix;
        } else {
            nodes[idx].world_matrix = nodes[idx].local_matrix;
        }
        state[idx] = 2;
    };
    for (size_t i = 0; i < nodes.size(); ++i) compute(static_cast<int>(i));
}

void Scene::normalize_skin_weights() {
    for (auto& mesh : meshes) {
        if (!mesh.is_skinned) continue;
        if (mesh.skin_weights.size() < mesh.vertices.size()) {
            mesh.skin_weights.resize(mesh.vertices.size());
        }
        size_t zero_count = 0;
        for (auto& wlist : mesh.skin_weights) {
            // Remove tiny/invalid weights and merge duplicates for same bone
            std::vector<BoneWeight> merged;
            for (const auto& bw : wlist) {
                if (bw.bone_node_index < 0 || bw.bone_node_index >= static_cast<int>(nodes.size())) continue;
                if (!std::isfinite(bw.weight) || bw.weight <= 1e-6f) continue;
                bool found = false;
                for (auto& m : merged) {
                    if (m.bone_node_index == bw.bone_node_index) { m.weight += bw.weight; found = true; break; }
                }
                if (!found) merged.push_back(bw);
            }
            std::sort(merged.begin(), merged.end(), [](const BoneWeight& a, const BoneWeight& b) { return a.weight > b.weight; });
            // Do not truncate skin influences in canonical scene (exporters handle target constraints)
            float sum = 0.0f;
            for (const auto& bw : merged) sum += bw.weight;
            if (sum > 1e-6f) {
                for (auto& bw : merged) bw.weight /= sum;
            } else {
                ++zero_count;
            }
            wlist = std::move(merged);
        }
        if (zero_count > 0) {
            add_diag(DiagSeverity::Info, "SKIN_UNWEIGHTED_VERTS",
                "Mesh '" + mesh.name + "' has " + std::to_string(zero_count) + " vertices with zero bone weight");
        }
    }
}

void Scene::ensure_inverse_bind_matrices() {
    update_world_matrices();
    // Compute base-pose world matrices if base_pose_matrix is present on bone nodes
    std::vector<Mat4> bind_world(nodes.size(), Mat4::identity());
    std::vector<uint8_t> done(nodes.size(), 0);
    std::function<void(int)> compute_bind = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(nodes.size()) || done[idx]) return;
        done[idx] = 1;
        const Mat4& loc = nodes[idx].has_base_pose ? nodes[idx].base_pose_matrix : nodes[idx].local_matrix;
        int p = nodes[idx].parent_index;
        if (p >= 0 && p < static_cast<int>(nodes.size()) && p != idx) {
            compute_bind(p);
            bind_world[idx] = loc * bind_world[p];
        } else {
            bind_world[idx] = loc;
        }
    };
    for (size_t i = 0; i < nodes.size(); ++i) compute_bind(static_cast<int>(i));

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].has_inverse_bind) {
            Mat4 inv;
            if (bind_world[i].invert_affine(inv)) {
                nodes[i].inverse_bind_matrix = inv;
                nodes[i].has_inverse_bind = true;
            }
        }
    }
}

SceneStats Scene::compute_stats() const {
    SceneStats st;
    st.node_count = nodes.size();
    st.mesh_count = meshes.size();
    st.material_count = materials.size();
    st.animation_clip_count = animations.size();
    for (const auto& a : animations) st.animation_track_count += a.tracks.size();

    for (const auto& n : nodes) {
        if (n.is_bone) ++st.bone_count;
        if (n.is_hardpoint) ++st.hardpoint_count;
        if (n.local_matrix.det3x3() < -1e-6f) ++st.negative_det_nodes;
    }

    size_t agree = 0, tested = 0;
    for (const auto& n : nodes) {
        if (n.mesh_index < 0 || n.mesh_index >= static_cast<int>(meshes.size())) continue;
        const auto& m = meshes[n.mesh_index];
        const Mat4& wm = m.vertices_in_model_space ? Mat4::identity() : n.world_matrix;
        bool flipped_det = wm.det3x3() < 0.0f;
        for (const auto& v : m.vertices) {
            Vec3 wp = wm.transform_point(v.pos);
            st.world_bbox.expand(wp);
        }
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            uint32_t i0 = m.indices[i], i1 = m.indices[i+1], i2 = m.indices[i+2];
            if (i0 >= m.vertices.size() || i1 >= m.vertices.size() || i2 >= m.vertices.size()) continue;
            Vec3 a = wm.transform_point(m.vertices[i0].pos);
            Vec3 b = wm.transform_point(m.vertices[i1].pos);
            Vec3 c = wm.transform_point(m.vertices[i2].pos);
            Vec3 g = (b - a).cross(c - a);
            float gl = g.length();
            if (gl < 1e-8f) { ++st.degenerate_triangles; continue; }
            if (m.has_normals) {
                Vec3 na = wm.transform_vector(m.vertices[i0].normal);
                Vec3 nb = wm.transform_vector(m.vertices[i1].normal);
                Vec3 nc = wm.transform_vector(m.vertices[i2].normal);
                Vec3 avg_n = na + nb + nc;
                float nl = avg_n.length();
                if (nl > 1e-8f) {
                    ++tested;
                    float sign = flipped_det ? -1.0f : 1.0f;
                    if (g.dot(avg_n) * sign > 0.0f) ++agree;
                }
            }
        }
    }

    for (const auto& m : meshes) {
        st.total_vertices += m.vertices.size();
        st.total_triangles += m.indices.size() / 3;
        st.source_polygons += m.source_polygon_count;
        if (m.is_skinned) ++st.skinned_mesh_count;
        if (m.has_uvs) st.has_uvs = true;
        if (m.has_normals) st.has_normals = true;
        if (m.has_vertex_colors) st.has_vertex_colors = true;
    }

    st.normal_face_agreement = tested ? (static_cast<double>(agree) / static_cast<double>(tested)) : 1.0;
    return st;
}

} // namespace bz
