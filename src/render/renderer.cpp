#include "renderer.hpp"
#include "canvas/icanvas.hpp"
#include "document/document.hpp"
#include "document/entity/all_entities.hpp"
#include "document/group/group_extrude.hpp"
#include "document/constraint/all_constraints.hpp"
#include "document/solid_model.hpp"
#include "canvas/selectable_ref.hpp"
#include "idocument_view.hpp"
#include "util/util.hpp"
#include "util/glm_util.hpp"
#include "icon_texture_id.hpp"
#include <iostream>
#include <array>
#include <format>
#include <ranges>
#include <glm/gtx/io.hpp>

namespace dune3d {

Renderer::Renderer(ICanvas &ca) : m_ca(ca)
{
}

bool Renderer::group_is_visible(const UUID &uu) const
{
    if (m_current_group->m_uuid == uu)
        return true;
    auto &group = m_doc->get_group(uu);
    if (group.get_index() > m_current_group->get_index())
        return false;
    if (!m_doc_view->group_is_visible(uu))
        return false;
    auto body = group.find_body(*m_doc);
    if (m_current_body_group != &body.group && !m_doc_view->body_is_visible(body.group.m_uuid))
        return false;
    return true;
}

void Renderer::render(const Document &doc, const UUID &current_group, const IDocumentView &doc_view)
{
    m_doc = &doc;
    m_doc_view = &doc_view;
    m_current_group = &doc.get_group(current_group);
    m_current_body_group = &m_current_group->find_body(doc).group;


    if (m_solid_model_edge_select_mode) {
        auto last_solid_model = SolidModel::get_last_solid_model(*m_doc, *m_current_group);
        if (last_solid_model) {
            m_ca.add_face_group(last_solid_model->m_faces, {0, 0, 0}, glm::quat_identity<float, glm::defaultp>(),
                                ICanvas::FaceColor::SOLID_MODEL);
            for (const auto &[edge_idx, path] : last_solid_model->m_edges) {
                for (size_t i = 1; i < path.size(); i++) {
                    m_ca.add_selectable(
                            m_ca.draw_line(path.at(i - 1), path.at(i)),
                            SelectableRef{m_document_uuid, SelectableRef::Type::SOLID_MODEL_EDGE, UUID(), edge_idx});
                }
            }
        }

        m_doc = nullptr;
        m_doc_view = nullptr;
        m_current_group = nullptr;
        return;
    }

    for (auto group : doc.get_groups_sorted() | std::views::reverse) {
        if (!group_is_visible(group->m_uuid))
            continue;
        for (const auto &[uu, el] : doc.m_entities) {
            if (el->m_group == group->m_uuid)
                render(*el);
        }
    }


    auto groups_by_body = doc.get_groups_by_body();
    for (auto body_groups : groups_by_body) {
        if (!m_doc_view->body_solid_model_is_visible(body_groups.get_group().m_uuid))
            continue;
        const SolidModel *last_solid_model = nullptr;
        for (auto group : body_groups.groups) {
            if (!group_is_visible(group->m_uuid))
                continue;
            if (auto gr = dynamic_cast<const IGroupSolidModel *>(group)) {
                if (gr->get_solid_model())
                    last_solid_model = gr->get_solid_model();
                if (group->m_uuid == current_group)
                    break;
            }
        }

        if (last_solid_model) {
            const auto is_current = std::ranges::any_of(
                    body_groups.groups, [current_group](auto group) { return group->m_uuid == current_group; });
            const auto color =
                    is_current ? ICanvas::FaceColor::SOLID_MODEL : ICanvas::FaceColor::OTHER_BODY_SOLID_MODEL;
            m_ca.add_face_group(last_solid_model->m_faces, {0, 0, 0}, glm::quat_identity<float, glm::defaultp>(),
                                color);
        }
    }


    for (const auto &[uu, el] : doc.m_constraints) {
        if (m_current_group->m_uuid != el->m_group)
            continue;
        el->accept(*this);
    }

    draw_constraints();

    m_ca.update_bbox();

    m_doc = nullptr;
    m_doc_view = nullptr;
    m_current_group = nullptr;
}

void Renderer::render(const Entity &entity)
{
    if (!entity.m_visible)
        return;
    if (entity.m_construction && entity.m_group != m_current_group->m_uuid)
        return;
    m_ca.set_vertex_inactive(entity.m_group != m_current_group->m_uuid);
    m_ca.set_selection_invisible(entity.m_selection_invisible);
    m_ca.set_vertex_construction(entity.m_construction);
    entity.accept(*this);
    m_ca.set_selection_invisible(false);
    m_ca.set_vertex_inactive(false);
    m_ca.set_vertex_construction(false);
}

void Renderer::visit(const EntityLine3D &line)
{
    m_ca.add_selectable(m_ca.draw_line(line.m_p1, line.m_p2),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, line.m_uuid, 0});
    if (line.m_no_points)
        return;

    m_ca.add_selectable(m_ca.draw_point(line.m_p1),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, line.m_uuid, 1});
    m_ca.add_selectable(m_ca.draw_point(line.m_p2),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, line.m_uuid, 2});
}

void Renderer::visit(const EntityLine2D &line)
{
    auto &wrkpl = dynamic_cast<const EntityWorkplane &>(*m_doc->m_entities.at(line.m_wrkpl));
    const auto p1 = wrkpl.transform(line.m_p1);
    const auto p2 = wrkpl.transform(line.m_p2);
    m_ca.add_selectable(m_ca.draw_line(p1, p2),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, line.m_uuid, 0});
    m_ca.add_selectable(m_ca.draw_point(p1),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, line.m_uuid, 1});
    m_ca.add_selectable(m_ca.draw_point(p2),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, line.m_uuid, 2});
}

void Renderer::visit(const EntityPoint2D &point)
{
    auto &wrkpl = dynamic_cast<const EntityWorkplane &>(*m_doc->m_entities.at(point.m_wrkpl));
    const auto p = wrkpl.transform(point.m_p);
    m_ca.add_selectable(m_ca.draw_point(p),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, point.m_uuid, 0});
}
static double angle(const glm::dvec2 &v)
{
    return glm::atan(v.y, v.x);
}

template <typename T> T c2pi(T x)
{
    while (x < 0)
        x += 2 * M_PI;

    while (x > 2 * M_PI)
        x -= 2 * M_PI;
    return x;
}

static glm::dvec2 euler(double r, double phi)
{
    return {r * cos(phi), r * sin(phi)};
}

void Renderer::visit(const EntityArc2D &arc)
{
    auto &wrkpl = dynamic_cast<const EntityWorkplane &>(*m_doc->m_entities.at(arc.m_wrkpl));
    auto center = arc.m_center;

    {
        const auto radius0 = glm::length(center - arc.m_from);
        const auto a0 = c2pi(angle(arc.m_from - center));
        const auto a1 = c2pi(angle(arc.m_to - center));
        unsigned int segments = 64;

        float dphi = c2pi(a1 - a0);
        if (dphi < 1e-2)
            dphi = 2 * M_PI;
        dphi /= segments;
        float a = a0;
        while (segments--) {
            const auto p0 = center + euler(radius0, a);
            const auto p1 = center + euler(radius0, a + dphi);
            m_ca.add_selectable(m_ca.draw_line(wrkpl.transform(p0), wrkpl.transform(p1)),
                                SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 0});
            a += dphi;
        }
    }

    m_ca.add_selectable(m_ca.draw_point(wrkpl.transform(arc.m_from)),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 1});
    m_ca.add_selectable(m_ca.draw_point(wrkpl.transform(arc.m_to)),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 2});
    m_ca.add_selectable(m_ca.draw_point(wrkpl.transform(arc.m_center)),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 3});
}

void Renderer::visit(const EntityCircle2D &circle)
{
    auto &wrkpl = dynamic_cast<const EntityWorkplane &>(*m_doc->m_entities.at(circle.m_wrkpl));

    {
        unsigned int segments = 64;

        float dphi = 2 * M_PI;
        dphi /= segments;
        float a = 0;
        while (segments--) {
            const auto p0 = circle.m_center + euler(circle.m_radius, a);
            const auto p1 = circle.m_center + euler(circle.m_radius, a + dphi);
            m_ca.add_selectable(m_ca.draw_line(wrkpl.transform(p0), wrkpl.transform(p1)),
                                SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, circle.m_uuid, 0});
            a += dphi;
        }
    }

    m_ca.add_selectable(m_ca.draw_point(wrkpl.transform(circle.m_center)),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, circle.m_uuid, 1});
}
void Renderer::visit(const EntityCircle3D &circle)
{
    {
        unsigned int segments = 64;

        float dphi = 2 * M_PI;
        dphi /= segments;
        float a = 0;
        while (segments--) {
            const auto p0 = circle.m_center + glm::rotate(circle.m_normal, glm::dvec3(euler(circle.m_radius, a), 0));
            const auto p1 =
                    circle.m_center + glm::rotate(circle.m_normal, glm::dvec3(euler(circle.m_radius, a + dphi), 0));
            m_ca.add_selectable(m_ca.draw_line(p0, p1),
                                SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, circle.m_uuid, 0});
            a += dphi;
        }
    }

    m_ca.add_selectable(m_ca.draw_point(circle.m_center),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, circle.m_uuid, 1});
}

void Renderer::visit(const EntityArc3D &arc)
{
    auto un = glm::rotate(arc.m_normal, glm::dvec3(1, 0, 0));
    auto vn = glm::rotate(arc.m_normal, glm::dvec3(0, 1, 0));

    auto project = [&un, &vn, &arc](const glm::dvec3 &p) -> glm::dvec2 {
        auto v = p - arc.m_center;
        return {glm::dot(un, v), glm::dot(vn, v)};
    };
    auto transform = [&arc](const glm::dvec2 &p) { return arc.m_center + glm::rotate(arc.m_normal, glm::dvec3(p, 0)); };

    auto from2 = project(arc.m_from);
    auto to2 = project(arc.m_to);


    {
        const auto radius0 = glm::length(from2);
        const auto a0 = c2pi(angle(from2));
        const auto a1 = c2pi(angle(to2));
        unsigned int segments = 64;

        float dphi = c2pi(a1 - a0);
        if (dphi < 1e-2)
            dphi = 2 * M_PI;
        dphi /= segments;
        float a = a0;
        while (segments--) {
            const auto p0 = euler(radius0, a);
            const auto p1 = euler(radius0, a + dphi);
            m_ca.add_selectable(m_ca.draw_line(transform(p0), transform(p1)),
                                SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 0});
            a += dphi;
        }
    }

    m_ca.add_selectable(m_ca.draw_point(arc.m_from),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 1});
    m_ca.add_selectable(m_ca.draw_point(arc.m_to),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 2});
    m_ca.add_selectable(m_ca.draw_point(arc.m_center),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, arc.m_uuid, 3});
}

void Renderer::visit(const EntityWorkplane &wrkpl)
{
    m_ca.add_selectable(m_ca.draw_point(wrkpl.m_origin),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, wrkpl.m_uuid, 1});
    glm::vec2 sz = wrkpl.m_size / 2.;
    std::array<glm::vec2, 4> pts = {
            glm::vec2(-sz),
            glm::vec2(sz * glm::vec2(1, -1)),
            glm::vec2(sz),
            glm::vec2(sz * glm::vec2(-1, 1)),
    };
    const auto sr = SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, wrkpl.m_uuid, 0};

    for (size_t i = 0; i < pts.size(); i++) {
        const auto p1 = wrkpl.transform(pts.at(i));
        const auto p2 = wrkpl.transform(pts.at((i + 1) % (pts.size())));
        m_ca.add_selectable(m_ca.draw_line(p1, p2), sr);
    }

    if (wrkpl.m_uuid == m_current_group->m_active_wrkpl) {
        for (size_t i = 0; i < pts.size(); i++) {
            const auto p1 = wrkpl.transform(pts.at(i) * .99f);
            const auto p2 = wrkpl.transform(pts.at((i + 1) % (pts.size())) * .99f);
            m_ca.add_selectable(m_ca.draw_line(p1, p2), sr);
        }
    }

    auto normal = wrkpl.get_normal_vector() * .05;
    m_ca.add_selectable(m_ca.draw_screen_line(wrkpl.m_origin, normal), sr);

    // draw bottom left chamfer
    {
        auto s = std::min(sz.x, sz.y) / 5;
        auto p1 = wrkpl.transform(-sz + glm::vec2(s, 0));
        auto p2 = wrkpl.transform(-sz + glm::vec2(0, s));
        m_ca.add_selectable(m_ca.draw_line(p1, p2), sr);

        add_selectables(sr, m_ca.draw_bitmap_text_3d(wrkpl.transform(-sz + glm::vec2(s, s * .25)), wrkpl.m_normal,
                                                     s / 2, wrkpl.m_name));
    }
}

void Renderer::visit(const EntitySTEP &en)
{
    m_ca.add_selectable(m_ca.draw_point(en.m_origin),
                        SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, en.m_uuid, 1});

    if (!en.m_show_points) {
        for (const auto &[idx, p] : en.m_anchors) {
            m_ca.add_selectable(m_ca.draw_point(en.transform(p)),
                                SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, en.m_uuid, idx});
        }
    }

    if (en.m_imported) {
        m_ca.add_selectable(
                m_ca.add_face_group(en.m_imported->result.faces, en.m_origin, en.m_normal, ICanvas::FaceColor::AS_IS),
                SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, en.m_uuid, 0});
        if (en.m_show_points) {
            unsigned int idx = EntitySTEP::s_imported_point_offset;
            for (auto &pt : en.m_imported->result.points) {
                m_ca.add_selectable(m_ca.draw_point(en.transform({pt.x, pt.y, pt.z})),
                                    SelectableRef{m_document_uuid, SelectableRef::Type::ENTITY, en.m_uuid, idx++});
            }
        }
    }
}

static glm::vec3 project_point_onto_plane(const glm::vec3 &plane_origin, const glm::vec3 &plane_normal,
                                          const glm::vec3 &point)
{
    const auto v = point - plane_origin;
    const auto dist = glm::dot(plane_normal, v);
    return point - dist * plane_normal;
}

void Renderer::visit(const ConstraintPointDistance &constr)
{
    m_ca.set_vertex_constraint(true);
    glm::vec3 from = m_doc->get_point(constr.m_entity1);
    glm::vec3 to = m_doc->get_point(constr.m_entity2);
    auto p = constr.get_origin(*m_doc) + constr.m_offset;
    glm::vec3 fallback_normal = {NAN, NAN, NAN};
    if (constr.m_wrkpl) {
        auto &wrkpl = m_doc->get_entity<EntityWorkplane>(constr.m_wrkpl);
        p = wrkpl.project3(p);
        fallback_normal = wrkpl.get_normal_vector();
        from = wrkpl.project3(from);
        to = wrkpl.project3(to);
    }

    draw_distance_line(from, to, p, constr.m_distance, constr.m_uuid, fallback_normal);

    m_ca.set_vertex_constraint(false);
}


static const float constraint_arrow_scale = .015;
static const float constraint_arrow_aspect = 1.5;
static const float constraint_line_extension = 2.0;

void Renderer::draw_distance_line(const glm::vec3 &from, const glm::vec3 &to, const glm::vec3 &text_p, double distance,
                                  const UUID &uu, const glm::vec3 &fallback_normal)
{
    auto n = glm::normalize(from - to);
    auto p1 = project_point_onto_plane(from, n, text_p);
    auto p2 = project_point_onto_plane(to, n, text_p);

    SelectableRef sr{m_document_uuid, SelectableRef::Type::CONSTRAINT, uu, 0};
    m_ca.add_selectable(m_ca.draw_line(p1, p2), sr);
    m_ca.add_selectable(m_ca.draw_line(from, p1), sr);
    m_ca.add_selectable(m_ca.draw_line(to, p2), sr);

    std::string label = std::format(" {:.3f}", distance);
    add_selectables(sr, m_ca.draw_bitmap_text(text_p, 1, label));

    const float scale = constraint_arrow_scale;
    const float aspect = constraint_arrow_aspect;
    const float ext = constraint_line_extension;
    auto d = p1 - from;
    if (glm::length(d) > 1e-6) {
        d = glm::normalize(d);
    }
    else {
        if (std::isnan(fallback_normal.x))
            return;
        d = glm::normalize(glm::cross(n, fallback_normal));
    }

    m_ca.add_selectable(m_ca.draw_screen_line(p1, d * ext * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(p2, d * ext * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(p1, (+d - n * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(p1, (-d - n * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(p2, (+d + n * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(p2, (-d + n * aspect) * scale), sr);
}

void Renderer::add_selectables(const SelectableRef &sr, const std::vector<ICanvas::VertexRef> &vrs)
{
    for (const auto &vr : vrs) {
        m_ca.add_selectable(vr, sr);
    }
}

void Renderer::visit(const ConstraintPointLineDistance &constr)
{
    m_ca.set_vertex_constraint(true);

    auto pp = m_doc->get_point(constr.m_point);
    auto pproj = constr.get_projected(*m_doc);
    auto p = constr.get_origin(*m_doc) + constr.m_offset;
    glm::vec3 fallback_normal = {NAN, NAN, NAN};
    if (constr.m_wrkpl) {
        auto &wrkpl = m_doc->get_entity<EntityWorkplane>(constr.m_wrkpl);
        p = wrkpl.project3(p);
        fallback_normal = wrkpl.get_normal_vector();
        pproj = wrkpl.project3(pproj);
        pp = wrkpl.project3(pp);
    }

    draw_distance_line(pproj, pp, p, std::abs(constr.m_distance), constr.m_uuid, fallback_normal);

    m_ca.set_vertex_constraint(false);
}

void Renderer::visit(const ConstraintPointPlaneDistance &constr)
{
    m_ca.set_vertex_constraint(true);

    const auto pp = m_doc->get_point(constr.m_point);
    const auto pproj = constr.get_projected(*m_doc);
    auto p = constr.get_origin(*m_doc) + constr.m_offset;

    const auto &l1 = m_doc->get_entity(constr.m_line1);
    const auto fallback_normal = l1.get_point(2, *m_doc) - l1.get_point(1, *m_doc);

    draw_distance_line(pproj, pp, p, std::abs(constr.m_distance), constr.m_uuid, fallback_normal);

    m_ca.set_vertex_constraint(false);
}

void Renderer::visit(const ConstraintDiameterRadius &constr)
{
    m_ca.set_vertex_constraint(true);
    auto &en = m_doc->get_entity(constr.m_entity);
    auto &en_radius = dynamic_cast<const IEntityRadius &>(en);
    auto &en_wrkpl = dynamic_cast<const IEntityInWorkplane &>(en);
    auto &wrkpl = m_doc->get_entity<EntityWorkplane>(en_wrkpl.get_workplane());
    const auto center = en_radius.get_center();
    const auto radius = en_radius.get_radius();
    const auto offset_norm = glm::normalize(constr.m_offset);


    glm::vec3 from = wrkpl.transform(center);
    if (constr.get_type() == Constraint::Type::DIAMETER)
        from = wrkpl.transform(center + offset_norm * -radius);
    glm::vec3 to = wrkpl.transform(center + offset_norm * radius);
    auto p = wrkpl.transform(center + constr.m_offset);


    auto l = glm::normalize(from - to);
    glm::vec3 n = wrkpl.transform_relative(glm::vec2(-offset_norm.y, offset_norm.x));

    SelectableRef sr{m_document_uuid, SelectableRef::Type::CONSTRAINT, constr.m_uuid, 0};
    m_ca.add_selectable(m_ca.draw_line(from, to), sr);
    const auto scale = constraint_arrow_scale;
    const auto aspect = constraint_arrow_aspect;
    if (constr.get_type() == Constraint::Type::DIAMETER) {
        m_ca.add_selectable(m_ca.draw_screen_line(from, (+n - l * aspect) * scale), sr);
        m_ca.add_selectable(m_ca.draw_screen_line(from, (-n - l * aspect) * scale), sr);
    }
    m_ca.add_selectable(m_ca.draw_screen_line(to, (+n + l * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(to, (-n + l * aspect) * scale), sr);

    std::string label = std::format(" {:.3f}", constr.m_distance);
    add_selectables(sr, m_ca.draw_bitmap_text(p, 1, label));

    m_ca.set_vertex_constraint(false);
}

void Renderer::visit(const ConstraintPointDistanceHV &constr)
{
    m_ca.set_vertex_constraint(true);
    auto &wrkpl = m_doc->get_entity<EntityWorkplane>(constr.m_wrkpl);
    auto from = wrkpl.project(m_doc->get_point(constr.m_entity1));
    auto to = wrkpl.project(m_doc->get_point(constr.m_entity2));
    glm::vec2 mid = (from + to) / 2.;
    auto p = wrkpl.project(wrkpl.transform(mid) + constr.m_offset);
    const double scale = constraint_arrow_scale;
    const double aspect = constraint_arrow_aspect;
    const double ext = constraint_line_extension;
    SelectableRef sr{m_document_uuid, SelectableRef::Type::CONSTRAINT, constr.m_uuid, 0};

    glm::dvec2 pf, pt;
    if (constr.get_type() == Constraint::Type::POINT_DISTANCE_HORIZONTAL) {
        pf = {from.x, p.y};
        pt = {to.x, p.y};
    }
    else {
        pf = {p.x, from.y};
        pt = {p.x, to.y};
    }

    const auto v = glm::normalize(pf - pt);
    const auto d = wrkpl.transform_relative(v);
    const auto dnf = wrkpl.transform_relative(glm::normalize(pf - from));
    const auto dnt = wrkpl.transform_relative(glm::normalize(pt - to));
    const auto pft = wrkpl.transform(pf);
    const auto ptt = wrkpl.transform(pt);
    m_ca.add_selectable(m_ca.draw_line(wrkpl.transform(from), pft), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(pft, dnf * scale * ext), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(pft, (+dnf - d * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(pft, (-dnf - d * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_line(pft, ptt), sr);


    m_ca.add_selectable(m_ca.draw_screen_line(ptt, dnt * scale * ext), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(ptt, (+dnt + d * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_screen_line(ptt, (-dnt + d * aspect) * scale), sr);
    m_ca.add_selectable(m_ca.draw_line(ptt, wrkpl.transform(to)), sr);

    std::string label = std::format(" {:.3f}", constr.m_distance);
    add_selectables(sr, m_ca.draw_bitmap_text(wrkpl.transform(p), 1, label));

    m_ca.set_vertex_constraint(false);
}

using IconID = IconTexture::IconTextureID;

void Renderer::visit(const ConstraintPointsCoincident &constraint)
{
    auto p1 = m_doc->get_point(constraint.m_entity1);
    auto p2 = m_doc->get_point(constraint.m_entity2);
    add_constraint(p1, IconID::CONSTRAINT_POINTS_COINCIDENT, constraint.m_uuid);
    if (glm::length(p2 - p1) > 1e-6)
        add_constraint(p2, IconID::CONSTRAINT_POINTS_COINCIDENT, constraint.m_uuid);
}

void Renderer::visit(const ConstraintHV &constraint)
{
    auto p1 = m_doc->get_point(constraint.m_entity1);
    auto p2 = m_doc->get_point(constraint.m_entity2);
    auto icon = IconID::CONSTRAINT_HORIZONTAL;
    add_constraint((p1 + p2) / 2., icon, constraint.m_uuid, p2 - p1);
}

void Renderer::visit(const ConstraintSymmetricHV &constraint)
{
    auto p1 = m_doc->get_point(constraint.m_entity1);
    auto p2 = m_doc->get_point(constraint.m_entity2);
    auto icon = IconID::CONSTRAINT_SYMMETRIC_VERTICAL;
    add_constraint((p1 + p2) / 2., icon, constraint.m_uuid, p2 - p1);
}

void Renderer::visit(const ConstraintSymmetricLine &constraint)
{
    auto p1 = m_doc->get_point(constraint.m_entity1);
    auto p2 = m_doc->get_point(constraint.m_entity2);
    auto icon = IconID::CONSTRAINT_SYMMETRIC_LINE;
    add_constraint((p1 + p2) / 2., icon, constraint.m_uuid, p2 - p1);
}

static glm::dvec3 get_vec(const UUID &uu, const Document &doc)
{
    return doc.get_point({uu, 2}) - doc.get_point({uu, 1});
}

void Renderer::visit(const ConstraintPointOnLine &constraint)
{
    const auto pt = m_doc->get_point(constraint.m_point);
    const auto v = get_vec(constraint.m_line, *m_doc);
    add_constraint(pt, IconID::CONSTRAINT_POINT_ON_LINE, constraint.m_uuid, v);
}

void Renderer::visit(const ConstraintPointOnCircle &constraint)
{
    const auto pt = m_doc->get_point(constraint.m_point);
    const auto &en = m_doc->get_entity(constraint.m_circle);
    glm::dvec3 center;
    if (en.of_type(Entity::Type::CIRCLE_2D, Entity::Type::CIRCLE_3D))
        center = en.get_point(1, *m_doc);
    else
        center = en.get_point(3, *m_doc);
    const auto v = pt - center;

    glm::dquat normal;
    if (auto iw = dynamic_cast<const IEntityInWorkplane *>(&en))
        normal = m_doc->get_entity<EntityWorkplane>(iw->get_workplane()).get_normal();
    else if (auto arc = dynamic_cast<const EntityArc3D *>(&en))
        normal = arc->m_normal;
    else if (auto circle = dynamic_cast<const EntityCircle3D *>(&en))
        normal = circle->m_normal;
    auto normal_vec = glm::rotate(normal, glm::dvec3(0, 0, 1));
    auto ortho = glm::cross(v, normal_vec);

    add_constraint(pt, IconID::CONSTRAINT_POINT_ON_CIRCLE, constraint.m_uuid, ortho);
}

void Renderer::visit(const ConstraintWorkplaneNormal &constraint)
{
    auto pt = m_doc->get_entity<EntityWorkplane>(constraint.m_wrkpl).m_origin;
    add_constraint(pt, IconID::CONSTRAINT_WORKPLANE_NORMAL, constraint.m_uuid);
}

void Renderer::visit(const ConstraintMidpoint &constraint)
{
    auto pt = m_doc->get_point(constraint.m_point);
    const auto v = get_vec(constraint.m_line, *m_doc);
    add_constraint(pt, IconID::CONSTRAINT_MIDPOINT, constraint.m_uuid, v);
}

static glm::vec3 get_center(const Entity &entity, const Document &doc)
{
    if (auto wrkpl = dynamic_cast<const EntityWorkplane *>(&entity)) {
        return wrkpl->m_origin;
    }
    else if (entity.get_type() == Entity::Type::CIRCLE_2D) {
        return entity.get_point(1, doc);
    }
    else {
        return (entity.get_point(1, doc) + entity.get_point(2, doc)) / 2.;
    }
}

void Renderer::visit(const ConstraintParallel &constraint)
{
    auto c1 = get_center(m_doc->get_entity(constraint.m_entity1), *m_doc);
    auto c2 = get_center(m_doc->get_entity(constraint.m_entity2), *m_doc);
    const auto v1 = get_vec(constraint.m_entity1, *m_doc);
    const auto v2 = get_vec(constraint.m_entity2, *m_doc);
    add_constraint(c1, IconID::CONSTRAINT_PARALLEL, constraint.m_uuid, v1);
    add_constraint(c2, IconID::CONSTRAINT_PARALLEL, constraint.m_uuid, v2);
}

void Renderer::visit(const ConstraintEqualLength &constraint)
{
    auto c1 = get_center(m_doc->get_entity(constraint.m_entity1), *m_doc);
    auto c2 = get_center(m_doc->get_entity(constraint.m_entity2), *m_doc);
    const auto v1 = get_vec(constraint.m_entity1, *m_doc);
    const auto v2 = get_vec(constraint.m_entity2, *m_doc);
    add_constraint(c1, IconID::CONSTRAINT_EQUAL_LENGTH, constraint.m_uuid, v1);
    add_constraint(c2, IconID::CONSTRAINT_EQUAL_LENGTH, constraint.m_uuid, v2);
}

void Renderer::visit(const ConstraintEqualRadius &constraint)
{
    auto c1 = get_center(m_doc->get_entity(constraint.m_entity1), *m_doc);
    auto c2 = get_center(m_doc->get_entity(constraint.m_entity2), *m_doc);
    add_constraint(c1, IconID::CONSTRAINT_EQUAL_LENGTH, constraint.m_uuid);
    add_constraint(c2, IconID::CONSTRAINT_EQUAL_LENGTH, constraint.m_uuid);
}

void Renderer::visit(const ConstraintSameOrientation &constraint)
{
    auto pt1 = m_doc->get_point({constraint.m_entity1, 1});
    auto pt2 = m_doc->get_point({constraint.m_entity2, 1});
    add_constraint(pt1, IconID::CONSTRAINT_SAME_ORIENTATION, constraint.m_uuid);
    add_constraint(pt2, IconID::CONSTRAINT_SAME_ORIENTATION, constraint.m_uuid);
}

void Renderer::visit(const ConstraintLockRotation &constraint)
{
    auto pt = m_doc->get_point({constraint.m_entity, 1});
    add_constraint(pt, IconID::CONSTRAINT_LOCK_ROTATION, constraint.m_uuid);
}

void Renderer::visit(const ConstraintArcArcTangent &constraint)
{
    auto p1 = m_doc->get_point(constraint.m_arc1);
    const auto &arc = m_doc->get_entity<EntityArc2D>(constraint.m_arc1.entity);
    const auto &wrkpl = m_doc->get_entity<EntityWorkplane>(arc.m_wrkpl);
    const auto v = wrkpl.transform_relative(arc.get_tangent_at_point(constraint.m_arc1.point));
    add_constraint(p1, IconID::CONSTRAINT_ARC_ARC_TANGENT, constraint.m_uuid, v);
}

void Renderer::visit(const ConstraintLinePointsPerpendicular &constraint)
{
    auto p1 = m_doc->get_point(constraint.m_point_line);
    add_constraint(p1, IconID::CONSTRAINT_PERPENDICULAR, constraint.m_uuid);
}

void Renderer::visit(const ConstraintLinesPerpendicular &constraint)
{
    auto c1 = get_center(m_doc->get_entity(constraint.m_entity1), *m_doc);
    auto c2 = get_center(m_doc->get_entity(constraint.m_entity2), *m_doc);
    const auto v1 = get_vec(constraint.m_entity1, *m_doc);
    const auto v2 = get_vec(constraint.m_entity2, *m_doc);
    add_constraint(c1, IconID::CONSTRAINT_PERPENDICULAR, constraint.m_uuid, v1);
    add_constraint(c2, IconID::CONSTRAINT_PERPENDICULAR, constraint.m_uuid, v2);
}


void Renderer::visit(const ConstraintLinesAngle &constr)
{
    m_ca.set_vertex_constraint(true);

    auto is = constr.get_origin(*m_doc);

    const auto vecs = constr.get_vectors(*m_doc);

    auto p = is + constr.m_offset;
    if (constr.m_wrkpl) {
        auto &wrkpl = m_doc->get_entity<EntityWorkplane>(constr.m_wrkpl);
        p = wrkpl.project3(p);
    }
    else {
        p = project_point_onto_plane(is, vecs.n, p);
    }

    auto vp = p - is;
    auto r = glm::length(vp);
    auto vpu = glm::dot(vecs.u, vp);

    auto transform = [&](const glm::dvec2 &p) { return is + p.x * vecs.u + p.y * vecs.v; };

    SelectableRef sr{m_document_uuid, SelectableRef::Type::CONSTRAINT, constr.m_uuid, 0};

    {
        float a0 = 0;
        if (vpu < 0)
            a0 = M_PI;
        unsigned int segments = 64;

        auto l1vp = glm::dvec2(glm::dot(vecs.u, vecs.l1v), glm::dot(vecs.v, vecs.l1v));
        auto l2vp = glm::dvec2(glm::dot(vecs.u, vecs.l2v), glm::dot(vecs.v, vecs.l2v));

        float dphi = angle(l2vp) - angle(l1vp);
        if (dphi > M_PI)
            dphi = 2 * M_PI - dphi;

        dphi /= segments;
        float a = a0;
        while (segments--) {
            const auto p0 = euler(r, a);
            const auto p1 = euler(r, a + dphi);
            m_ca.add_selectable(m_ca.draw_line(transform(p0), transform(p1)), sr);
            a += dphi;
        }
    }

    std::string label = std::format(" {:.1f}°", constr.m_angle);
    add_selectables(sr, m_ca.draw_bitmap_text(p, 1, label));

    m_ca.set_vertex_constraint(false);
}

void Renderer::visit(const ConstraintArcLineTangent &constraint)
{
    const auto p1 = m_doc->get_point(constraint.m_arc);
    const auto &arc = m_doc->get_entity<EntityArc2D>(constraint.m_arc.entity);
    const auto &wrkpl = m_doc->get_entity<EntityWorkplane>(arc.m_wrkpl);
    const auto v = wrkpl.transform_relative(arc.get_tangent_at_point(constraint.m_arc.point));
    add_constraint(p1, IconID::CONSTRAINT_ARC_LINE_TANGENT, constraint.m_uuid, v);
}

void Renderer::visit(const ConstraintPointInPlane &constraint)
{
    auto pt = m_doc->get_point(constraint.m_point);
    add_constraint(pt, IconID::CONSTRAINT_POINT_IN_PLANE, constraint.m_uuid);
}

void Renderer::visit(const ConstraintPointInWorkplane &constraint)
{
    auto pt = m_doc->get_point(constraint.m_point);
    add_constraint(pt, IconID::CONSTRAINT_POINT_IN_PLANE, constraint.m_uuid);
}

void Renderer::add_constraint_icons(glm::vec3 p, glm::vec3 v, const std::vector<ConstraintType> &constraints)
{
    using CT = Constraint::Type;
    static const std::map<ConstraintType, IconID> constraint_icon_map = {
            {CT::HORIZONTAL, IconID::CONSTRAINT_HORIZONTAL},
            {CT::VERTICAL, IconID::CONSTRAINT_VERTICAL},
            {CT::POINTS_COINCIDENT, IconID::CONSTRAINT_POINTS_COINCIDENT},
            {CT::POINT_ON_LINE, IconID::CONSTRAINT_POINT_ON_LINE},
            {CT::POINT_ON_CIRCLE, IconID::CONSTRAINT_POINT_ON_CIRCLE},
            {CT::MIDPOINT, IconID::CONSTRAINT_MIDPOINT},
    };
    for (const auto constraint : constraints) {
        if (!constraint_icon_map.contains(constraint))
            continue;
        auto icon = constraint_icon_map.at(constraint);
        if (!std::isnan(v.x) && constraint == CT::VERTICAL)
            icon = IconID::CONSTRAINT_HORIZONTAL;
        add_constraint(p, icon, {}, v);
    }
}


void Renderer::add_constraint(const glm::vec3 &pos, IconTexture::IconTextureID icon, const UUID &constraint,
                              const glm::vec3 &v)
{
    for (auto &[p, l] : m_constraints) {
        if (glm::length(p - pos) < 1e-6) {
            l.push_back({icon, v, constraint});
            return;
        }
    }
    m_constraints.emplace_back();
    m_constraints.back().first = pos;
    m_constraints.back().second.push_back(ConstraintInfo{icon, v, constraint});
}

void Renderer::draw_constraints()
{
    m_ca.set_vertex_constraint(true);
    for (const auto &[pos, constraints] : m_constraints) {
        double n = constraints.size();
        double spacing = 1;
        double offset = -(n - 1) / 2 * spacing;
        glm::vec3 v = {NAN, NAN, NAN};
        for (const auto &constraint : constraints) {
            if (!std::isnan(constraint.v.x) && (glm::length(constraint.v) != 0)) {
                v = constraint.v;
                break;
            }
        }
        for (const auto &constraint : constraints) {
            m_ca.set_selection_invisible(!constraint.constraint);
            const auto vr = m_ca.draw_icon(constraint.icon, pos, glm::vec2(offset, -.9), v);
            m_ca.set_selection_invisible(false);
            if (constraint.constraint)
                m_ca.add_selectable(
                        vr, SelectableRef{m_document_uuid, SelectableRef::Type::CONSTRAINT, constraint.constraint, 0});
            offset += spacing;
        }
    }
    m_ca.set_vertex_constraint(false);
}

} // namespace dune3d
