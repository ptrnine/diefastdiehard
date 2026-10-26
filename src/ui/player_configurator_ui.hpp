#pragma once

#include "player_configurator.hpp"
#include "player.hpp"
#include "ui_pressets.hpp"

#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/RenderTexture.hpp>

#include <functional>
#include <string>
#include <vector>

namespace dfdh {

class player_configurator_ui {
public:
    player_configurator_ui(const player_name_t& name):
        pconf(name),
        wnd_title("configure player " + std::string(name)),
        player_size(player::default_sprite_size() * 1.5f) {

        reload();

        for (auto& face_path : pconf.available_face_textures())
            faces.push_back({face_path});

        for (auto& pistol_sect : weapon::get_secondary_sections()) {
            pistols.emplace_back();
            auto& back   = pistols.back();
            back.wpn     = &weapon_mgr().load(pistol_sect);
            back.choosed = pistol_sect == pconf.pistol;

            auto& target = back.icon_trgt;
            float size_factor = 1.f;
            for (auto& layer : back.wpn->layers()) {
                if (!layer.getTexture())
                    continue;
                if (!back.trgt_created) {
                    auto size = layer.getTexture()->getSize();
                    auto sizef = vec2f(float(size.x), float(size.y));
                    size_factor = wpn_icon_h / sizef.y;
                    target->create(u32(sizef.x * size_factor), u32(sizef.y * size_factor));
                    /* New FBO texture contents are undefined until cleared (driver may keep stale pixels) */
                    target->clear({0, 0, 0, 0});
                    target->setView(sf::View(sf::FloatRect{0.f,
                                                           float(sizef.y * size_factor),
                                                           float(sizef.x * size_factor),
                                                           -float(sizef.y * size_factor)}));
                    back.trgt_created = true;
                }
                auto copy_layer = layer;
                copy_layer.setOrigin(0.f, 0.f);
                copy_layer.setPosition(0.f, 0.f);
                copy_layer.setScale(size_factor, size_factor);
                target->draw(copy_layer);
            }
            target->display();
            back.icon = make_nk_image(target->getTexture());
        }

        for (auto& face : faces) {
            auto img         = load_nk_image(face.path);
            auto sz          = vec2f(float(img.w), float(img.h));
            img.region[0]    = u16(sz.x * 0.13f);
            img.region[1]    = u16(sz.y * 0.03f);
            img.region[2]    = u16(sz.x * 0.85f);
            img.region[3]    = u16(sz.y * 0.54f);
            face.icon        = img;
            face.texture_idx = player_configurator::texture_path_to_index(face.path);
            face.choosed     = face.texture_idx == pconf.face_id;
        }

        pconf.write_on_delete = false;
    }

    void reload() {
        txtr_body = texture_mgr().load(pconf.body_texture_path());
        txtr_face = texture_mgr().load(pconf.face_texture_path());
        sprt_body.setTexture(txtr_body);
        sprt_face.setTexture(txtr_face);
        sprt_body.setColor(pconf.body_color);
        hand_or_leg.setFillColor(pconf.body_color);
        player_icon.clear({0, 0, 0, 0});
        wpn = &weapon_mgr().load(pconf.pistol);
        player::draw_to_texture_target(
            player_icon, player_size, 1.5f, sprt_body, sprt_face, hand_or_leg, wpn);
        player_icon_img = make_nk_image(player_icon.getTexture());
    }

    void update(ui_ctx& ui) {
        if (!wnd_show)
            return;

        bool update_was = false;

        nk_flags wnd_flags = NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE |
                             NK_WINDOW_SCALABLE | NK_WINDOW_CLOSABLE;

        if (ui.begin(wnd_title.data(), wnd_rect, wnd_flags)) {
            ui_layout_row(ui,
                          player_size.y,
                          {20.f, float(player_icon.getSize().x), 20.f, {200.f, ui_row::variable}});

            auto contents_size = ui.window_get_content_region_size();
            auto contents_pos = ui.window_get_content_region_min();

            ui.spacing(1);
            ui.image(player_icon_img);
            ui.spacing(1);

            if (ui.group_begin("plr_params", NK_WINDOW_NO_SCROLLBAR)) {
                ui_layout_row(ui, 25.f, {110.f, {50.f, ui_row::variable}});

                ui.label("body color", NK_TEXT_LEFT);
                if (ui.combo_begin_color(pconf.body_color, {ui.widget_width(), 400.f})) {
                    if (auto color = ui_color_picker(ui, pconf.body_color, 150.f)) {
                        update_was       = true;
                        pconf.body_color = *color;
                    }
                    ui.combo_end();
                }
                ui.label("tracer color", NK_TEXT_LEFT);
                if (ui.combo_begin_color(pconf.tracer_color, {200, 400})) {
                    if (auto color = ui_color_picker(ui, pconf.tracer_color, 150.f)) {
                        update_was         = true;
                        pconf.tracer_color = *color;
                    }
                    ui.combo_end();
                }
                ui.group_end();
            }

            ui.layout_row_dynamic(25.f, 1);
            ui.label("Faces", NK_TEXT_CENTERED);
            {
                auto icons_width = float(faces.size()) * (face_icon_size + 5.f);
                auto icons_spacing =
                    icons_width < contents_size.x ? icons_width / contents_size.x : 1.f;
                auto spacing  = (1.f - icons_spacing) * 0.5f;
                icons_spacing = 1.f - spacing;
                float ratio[] = {spacing, icons_spacing};
                ui.layout_row(NK_DYNAMIC, face_icon_size + 20.f, 2, ratio);
                ui.spacing(1);
                if (ui.group_begin("faces", 0)) {
                    ui.layout_row_begin(NK_STATIC, face_icon_size, int(faces.size()));
                    for (auto& face : faces) {
                        ui.layout_row_push(face_icon_size);
                        auto choosed = face.choosed;
                        if (ui.selectable_image(face.icon, &choosed)) {
                            if (!face.choosed) {
                                update_was = true;
                                for (auto& f : faces)
                                    if (f.texture_idx == pconf.face_id)
                                        f.choosed = false;
                                pconf.face_id = face.texture_idx;
                                face.choosed = choosed;
                            }
                        }
                    }
                    ui.layout_row_end();
                    ui.group_end();
                }
            }

            ui.layout_row_dynamic(25.f, 1);
            ui.label("Pistols", NK_TEXT_CENTERED);
            {
                ui.layout_row_dynamic(wpn_icon_h + 20.f, 1);
                if (ui.group_begin("pistols", 0)) {
                    ui.layout_row_begin(NK_STATIC, wpn_icon_h, int(pistols.size()));
                    for (auto& pistol : pistols) {
                        ui.layout_row_push(float(pistol.icon.w));
                        auto bounds = ui.widget_bounds();
                        auto local_bound_x = bounds.x - contents_pos.x;
                        if (local_bound_x + bounds.w > contents_size.x) {
                            if (local_bound_x < contents_size.x)
                                bounds.w = contents_pos.x + contents_size.x - bounds.x;
                            else
                                bounds.w = 0.f;
                        }
                        auto in = &ui.nk_ctx()->input;
                        if (nk_input_is_mouse_hovering_rect(in, bounds)) {
                            if (ui.tooltip_begin(300.f)) {
                                ui_layout_row(ui, 25.f, {80.f, ui_row::dynamic, 40.f});
                                ui_weapon_property(ui, "hit power", pistol.wpn->hit_power(), 100, 60.f);
                                ui_weapon_property(ui, "fire rate", u32(pistol.wpn->fire_rate()), 1000);
                                ui_weapon_property(ui, "recoil", u32(pistol.wpn->recoil()), 400);
                                ui_weapon_property(ui, "accuracy", u32(pistol.wpn->accuracy() * 100.f), 100);

                                ui.tooltip_end();
                            }
                        }
                        auto choosed = pistol.choosed;
                        if (ui.selectable_image(pistol.icon, &choosed)) {
                            if (!pistol.choosed) {
                                for (auto& p : pistols)
                                    if (p.wpn->section() == pconf.pistol)
                                        p.choosed = false;
                                update_was = true;
                                pconf.pistol = pistol.wpn->section();
                                pistol.choosed = true;
                            }
                        }
                    }

                    ui.group_end();
                }
            }
        }
        else {
            pconf.write();
            wnd_show = false;
            if (on_close)
                on_close();
        }
        ui.end();

        if (update_was)
            reload();
    }

    void show(bool value) {
        wnd_show = value;
        sf::Sprite sp;
    }

    void toggle() {
        show(!wnd_show);
    }

    [[nodiscard]]
    bool shown() const {
        return wnd_show;
    }

    /* Called when the window is closed (the config is written) */
    std::function<void()> on_close;

private:
    player_configurator pconf;
    std::string         wnd_title;

    struct nk_rect wnd_rect{200, 100, 500, 500};
    bool           wnd_show = false;

    sf::Sprite        sprt_body, sprt_face;
    sf::CircleShape   hand_or_leg;
    sf::Texture       txtr_body, txtr_face;
    sf::RenderTexture player_icon;
    struct nk_image   player_icon_img;
    vec2f             player_size;
    weapon*           wpn            = nullptr;
    float             wpn_icon_h     = 70.f;
    float             face_icon_size = 80.f;

    struct face_param {
        std::string     path;
        struct nk_image icon        = {};
        u32             texture_idx = 0;
        nk_bool         choosed     = false;
    };
    std::vector<face_param> faces;

    struct pistol_param {
        pistol_param(): icon_trgt(std::make_shared<sf::RenderTexture>()) {}
        weapon*                            wpn;
        std::shared_ptr<sf::RenderTexture> icon_trgt;
        struct nk_image                    icon;
        bool                               trgt_created = false;
        nk_bool                            choosed      = false;
    };
    std::vector<pistol_param> pistols;
};

}
