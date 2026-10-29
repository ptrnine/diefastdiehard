#pragma once

#include "player_configurator.hpp"
#include "player.hpp"
#include "ui_pressets.hpp"

#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/RenderTexture.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace dfdh {

class player_configurator_ui {
public:
    /* name_edit: the nickname is editable, another name loads that player's settings */
    player_configurator_ui(const player_name_t& name, bool name_edit = false):
        pconf(name), name_edit(name_edit), player_size(player::default_sprite_size() * 1.5f) {

        for (auto& face_path : pconf.available_face_textures())
            faces.push_back({face_path, load_nk_image(face_path), i32(player_configurator::texture_path_to_index(face_path))});

        hats.push_back({"", {}, player_configurator::no_hat}); /* "none" */
        for (auto& hat_path : pconf.available_hat_textures())
            hats.push_back({hat_path, load_nk_image(hat_path), player_configurator::hat_texture_path_to_id(hat_path)});

        for (auto& pistol_sect : weapon::get_secondary_sections()) {
            pistols.emplace_back();
            auto& back   = pistols.back();
            back.wpn     = &weapon_mgr().load(pistol_sect);

            auto& target = back.icon_trgt;
            float size_factor = 1.f;
            for (auto& layer : back.wpn->layers()) {
                if (!layer.has_texture())
                    continue;
                if (!back.trgt_created) {
                    auto size = layer.getTexture().getSize();
                    auto sizef = vec2f(float(size.x), float(size.y));
                    size_factor = wpn_icon_h / sizef.y;
                    if (!target->resize({u32(sizef.x * size_factor), u32(sizef.y * size_factor)}))
                        glog().error("cannot create a weapon icon render texture");
                    /* New FBO texture contents are undefined until cleared (driver may keep stale pixels) */
                    target->clear({0, 0, 0, 0});
                    target->setView(sf::View(sf::FloatRect({0.f, float(sizef.y * size_factor)},
                                                           {float(sizef.x * size_factor), -float(sizef.y * size_factor)})));
                    back.trgt_created = true;
                }
                auto copy_layer = layer;
                copy_layer.setOrigin({0.f, 0.f});
                copy_layer.setPosition({0.f, 0.f});
                copy_layer.setScale({size_factor, size_factor});
                target->draw(copy_layer);
            }
            target->display();
            back.icon = make_nk_image(*target);
        }

        load(name);
    }

    /* Show the settings of the player */
    void load(const player_name_t& name) {
        /* Read only: written on changes */
        auto conf            = player_configurator(name);
        conf.write_on_delete = false;
        pconf                = conf;
        wnd_title             = name_edit ? std::string("player settings") : "configure player " + std::string(name);
        set_name_buf(name);
        for (auto& pistol : pistols)
            pistol.choosed = pistol.wpn->section() == pconf.pistol;
        reload();
    }

    void reload() {
        txtr_body = texture_mgr().load(pconf.body_texture_path());
        txtr_head = texture_mgr().load(pconf.head_texture_path());
        txtr_face = texture_mgr().load(pconf.face_texture_path());
        if (!pconf.hat_texture_path().empty()) {
            txtr_hat = texture_mgr().load(pconf.hat_texture_path());
            sprt_hat.setTexture(txtr_hat, true);
        }
        sprt_body.setTexture(txtr_body, true);
        sprt_head.setTexture(txtr_head, true);
        sprt_face.setTexture(txtr_face, true);
        sprt_body.setColor(pconf.body_color);
        sprt_head.setColor(pconf.body_color);
        hand_or_leg.setFillColor(pconf.body_color);
        player_icon.clear({0, 0, 0, 0});
        wpn = &weapon_mgr().load(pconf.pistol);
        player::draw_to_texture_target(
            player_icon, player_size, 1.5f, sprt_body, sprt_head, sprt_face, pconf.hat_texture_path().empty() ? nullptr : &sprt_hat, hand_or_leg, wpn);
        player_icon_img = make_nk_image(player_icon);
    }

    void update(ui_ctx& ui) {
        if (!wnd_show)
            return;

        if (reopen) {
            /* Nuklear keeps a closed window hidden */
            ui.window_show(wnd_name, NK_SHOWN);
            reopen = false;
        }

        bool update_was = false;

        nk_flags wnd_flags = NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE |
                             NK_WINDOW_SCALABLE | NK_WINDOW_CLOSABLE;

        if (ui.begin_titled(wnd_name, wnd_title.data(), wnd_rect, wnd_flags)) {
            if (name_edit)
                name_draw(ui);

            ui_layout_row(ui,
                          float(player_icon.getSize().y),
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

            i32 face_id = i32(pconf.face_id);
            if (image_choice_row(ui, "Faces", faces, face_id)) {
                update_was    = true;
                pconf.face_id = u32(face_id);
            }
            if (image_choice_row(ui, "Hats", hats, pconf.hat_id))
                update_was = true;

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
            if (name_edit)
                name_apply();
            wnd_show = false;
            if (on_close)
                on_close();
        }
        ui.end();

        /* Saved on any change */
        if (update_was) {
            pconf.write();
            reload();
        }
    }

    struct image_choice {
        std::string     path;
        struct nk_image icon        = {};
        i32             texture_idx = 0; /* no_hat: none (no icon) */
    };

    /* A centered row of selectable images, chosen: texture_idx of the selected one. Returns true if changed */
    bool image_choice_row(ui_ctx& ui, const char* label, const std::vector<image_choice>& choices, i32& chosen) {
        auto contents_size = ui.window_get_content_region_size();
        bool changed       = false;

        ui.layout_row_dynamic(25.f, 1);
        ui.label(label, NK_TEXT_CENTERED);

        auto icons_width   = float(choices.size()) * (face_icon_size + 5.f);
        auto icons_spacing = icons_width < contents_size.x ? icons_width / contents_size.x : 1.f;
        auto spacing       = (1.f - icons_spacing) * 0.5f;
        icons_spacing      = 1.f - spacing;
        float ratio[]      = {spacing, icons_spacing};
        ui.layout_row(NK_DYNAMIC, face_icon_size + 20.f, 2, ratio);
        ui.spacing(1);
        if (ui.group_begin(label, 0)) {
            ui.layout_row_begin(NK_STATIC, face_icon_size, int(choices.size()));
            for (auto& choice : choices) {
                ui.layout_row_push(face_icon_size);
                nk_bool selected = choice.texture_idx == chosen;
                bool    clicked  = choice.path.empty() ? ui.selectable_label("none", NK_TEXT_CENTERED, &selected)
                                                       : ui.selectable_image(choice.icon, &selected);
                if (clicked && choice.texture_idx != chosen) {
                    chosen  = choice.texture_idx;
                    changed = true;
                }
            }
            ui.layout_row_end();
            ui.group_end();
        }
        return changed;
    }

    void show(bool value) {
        if (value && !wnd_show)
            reopen = true;
        wnd_show = value;
    }

    void toggle() {
        show(!wnd_show);
    }

    [[nodiscard]]
    bool shown() const {
        return wnd_show;
    }

    /* Called when the window is closed */
    std::function<void()> on_close;
    /* name_edit: another player's settings are shown */
    std::function<void(const player_name_t&)> on_name_change;

private:
    static constexpr const char* wnd_name = "player_configurator";

    /* player_name_t: 22 chars + nul */
    using name_buf_t = std::array<char, 23>;

    void set_name_buf(const player_name_t& name) {
        std::string str = name;
        std::strncpy(name_buf.data(), str.data(), name_buf.size() - 1);
        name_buf.back() = '\0';
    }

    void name_draw(ui_ctx& ui) {
        ui_layout_row(ui, 25.f, {110.f, ui_row::dynamic});
        ui.label("nickname", NK_TEXT_LEFT);
        auto res = ui.edit_string_zero_terminated(nk_flags(NK_EDIT_FIELD) | nk_flags(NK_EDIT_SIG_ENTER), name_buf.data(),
                                                  int(name_buf.size()), nk_filter_ascii);
        /* Enter or the focus is lost: not per typed char (no settings for half-typed names) */
        if (res & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
            name_apply();
    }

    void name_apply() {
        auto name = player_name_t(std::string(name_buf.data()));
        if (std::string_view(name).empty()) {
            set_name_buf(pconf.name);
            return;
        }
        if (name == pconf.name)
            return;
        load(name);
        if (on_name_change)
            on_name_change(name);
    }

    player_configurator pconf;
    bool                name_edit;
    name_buf_t          name_buf{};
    std::string         wnd_title;
    bool                reopen = false;

    struct nk_rect wnd_rect{200, 60, 640, 700}; /* the whole content without scrolling */
    bool           wnd_show = false;

    sprite_t          sprt_body, sprt_head, sprt_face, sprt_hat;
    sf::CircleShape   hand_or_leg;
    sf::Texture       txtr_body, txtr_head, txtr_face, txtr_hat;
    sf::RenderTexture player_icon;
    struct nk_image   player_icon_img;
    vec2f             player_size;
    weapon*           wpn            = nullptr;
    float             wpn_icon_h     = 70.f;
    float             face_icon_size = 80.f;

    std::vector<image_choice> faces;
    std::vector<image_choice> hats;

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
