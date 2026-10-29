#pragma once

#include "game_state.hpp"
#include "ui_pressets.hpp"

#include <SFML/Graphics/RenderTexture.hpp>
#include <SFML/Graphics/Sprite.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dfdh {

/**
 * Multiplayer lobby window: every player in a row (name, sprite, group,
 * ready), the level and game mode choice, the start button. The server owner
 * edits everything, a client its own player's group and ready flag only.
 * The UI does not know about the network: the actions are callbacks
 */
class lobby_ui {
public:
    enum class role_t { owner, client };

    struct actions_t {
        std::function<void(const std::string&)>          set_level;
        std::function<void(const std::string&)>          set_mode;
        std::function<void(const player_name_t&, int)>   set_group;
        std::function<void(bool)>                        set_ready;  /* client: the own player */
        std::function<bool()>                            own_ready;  /* client: the own player's requested flag */
        std::function<void(const player_name_t&)>        customize;  /* opens the player configurator */
        std::function<void()>                            start;          /* owner */
        std::function<void()>                            cancel_lobby;   /* owner: close the lobby without starting */
        std::function<void()>                            leave;          /* client: leave the game */
        std::function<std::vector<player_name_t>()>      local_players; /* players with the customize button */
    };

    lobby_ui(role_t role, actions_t actions): _role(role), _act(std::move(actions)) {
        if (_role == role_t::owner) {
            _levels = level::list_available_levels();
            _modes  = game_mode::list_available();
        }
    }

    void update(ui_ctx& ui, game_state& gs, const vec2f& window_size) {
        /* One column per player: as wide as the columns need (up to the screen, at least the settings row) */
        auto cols  = std::max<size_t>(gs.players.size(), 1);
        auto wnd_w = std::min(window_size.x - 40.f, std::max(640.f, float(cols) * column_width + 40.f));
        auto wnd_h = std::min(window_size.y - 40.f, 520.f);
        auto rect  = nk_rect((window_size.x - wnd_w) * 0.5f, (window_size.y - wnd_h) * 0.5f, wnd_w, wnd_h);

        if (ui.begin_titled("lobby", owner() ? "lobby (server)" : "lobby", rect, NK_WINDOW_TITLE | NK_WINDOW_BORDER)) {
            settings_draw(ui, gs);
            players_draw(ui, gs);
            bottom_draw(ui, gs);
        }
        ui.end();

        /* Forget the icons of the players that are gone */
        std::erase_if(_icons, [&](auto& icon) { return !gs.players.contains(icon.first); });
    }

private:
    static constexpr float column_width = 200.f; /* the icons are this wide too */
    static constexpr float row_height       = 25.f;
    static constexpr float sprite_scale = 1.5f;

    [[nodiscard]]
    bool owner() const {
        return _role == role_t::owner;
    }

    static std::string group_name(int group) {
        return group < 0 ? std::string("none") : "team " + std::to_string(group);
    }

    /* Level and game mode: combos for the owner, labels for clients */
    void settings_draw(ui_ctx& ui, game_state& gs) {
        ui_layout_row(ui, 25.f, {90.f, {200.f, ui_row::variable}, 110.f, {200.f, ui_row::variable}});

        ui.label("map", NK_TEXT_LEFT);
        choice_draw(ui, gs.lobby.level, _levels, _act.set_level);
        ui.label("game mode", NK_TEXT_LEFT);
        choice_draw(ui, gs.lobby.mode, _modes, _act.set_mode);
    }

    void choice_draw(ui_ctx&                                        ui,
                     const std::string&                             current,
                     const std::vector<std::string>&                items,
                     const std::function<void(const std::string&)>& set) {
        auto text = current.empty() ? std::string("-") : current;
        if (!owner()) {
            ui.label(text.data(), NK_TEXT_LEFT);
            return;
        }

        if (ui.combo_begin_label(text.data(), {ui.widget_width(), 300.f})) {
            ui.layout_row_dynamic(25.f, 1);
            for (auto& item : items)
                if (ui.combo_item_label(item.data(), NK_TEXT_LEFT) && set)
                    set(item);
            ui.combo_end();
        }
    }

    /* One column per player: the rows of names, sprites, groups, ready flags and customize buttons */
    void players_draw(ui_ctx& ui, game_state& gs) {
        auto local    = _act.local_players ? _act.local_players() : std::vector<player_name_t>{};
        auto is_local = [&](const player_name_t& name) {
            return std::ranges::find(local, name) != local.end();
        };

        std::vector<std::pair<player*, icon_t*>> columns;
        for (auto& [_, pl] : gs.players)
            columns.emplace_back(pl.get(), &icon_for(*pl));
        auto col_w    = column_width;
        auto sprite_h = icon_height();
        auto cols = int(std::max<size_t>(columns.size(), 1));

        ui.layout_row_dynamic(sprite_h + 5.f * row_height + 40.f, 1);
        if (!ui.group_begin("lobby_players", NK_WINDOW_BORDER))
            return;

        /* Names (the local ones are marked, they are the only ones with controls) */
        ui.layout_row_static(row_height, int(col_w), cols);
        for (auto& [pl, _] : columns) {
            if (is_local(pl->name()))
                ui.label_colored((std::string(pl->name()) + "  (you)").data(), NK_TEXT_CENTERED, sf::Color(255, 220, 120));
            else
                ui.label(std::string(pl->name()).data(), NK_TEXT_CENTERED);
        }

        /* Sprites (column wide icons) */
        ui.layout_row_static(sprite_h, int(col_w), cols);
        for (auto& [_, icon] : columns)
            ui.image(icon->img);

        /* Groups: the owner sets everyone's, a client its own */
        ui.layout_row_static(row_height, int(col_w), cols);
        for (auto& [pl, _] : columns) {
            auto group_label = group_name(pl->get_group());
            if (!owner() && !is_local(pl->name()))
                ui.label(group_label.data(), NK_TEXT_CENTERED);
            else if (ui.combo_begin_label(group_label.data(), {col_w, 250.f})) {
                ui.layout_row_dynamic(22.f, 1);
                for (int g = -1; g <= lobby_state::max_group; ++g)
                    if (ui.combo_item_label(group_name(g).data(), NK_TEXT_LEFT) && _act.set_group)
                        _act.set_group(pl->name(), g);
                ui.combo_end();
            }
        }

        /* Ready: the own checkbox on a client, the others' flags (the server owner's players are always ready) */
        ui.layout_row_static(row_height, int(col_w), cols);
        for (auto& [pl, _] : columns) {
            if (!owner() && is_local(pl->name())) {
                nk_bool ready = _act.own_ready && _act.own_ready() ? nk_true : nk_false;
                if (ui.checkbox_label("ready", &ready) && _act.set_ready)
                    _act.set_ready(ready == nk_true);
            }
            else if (owner() && !pl->net_controlled())
                ui.label_colored("host", NK_TEXT_CENTERED, sf::Color(200, 200, 200));
            else
                ready_label_draw(ui, gs.lobby.is_ready(pl->name()));
        }

        /* Customize: the local players only */
        ui.layout_row_static(row_height, int(col_w), cols);
        for (auto& [pl, _] : columns) {
            if (!is_local(pl->name()))
                ui.spacing(1);
            else if (ui.button_label("customize") && _act.customize)
                _act.customize(pl->name());
        }

        ui.group_end();
    }

    static void ready_label_draw(ui_ctx& ui, bool ready) {
        if (ready)
            ui.label_colored("ready", NK_TEXT_CENTERED, sf::Color(80, 220, 80));
        else
            ui.label_colored("not ready", NK_TEXT_CENTERED, sf::Color(230, 80, 80));
    }

    void bottom_draw(ui_ctx& ui, game_state& gs) {
        ui_layout_row(ui, 30.f, {ui_row::dynamic, 200.f, 150.f});
        if (owner()) {
            size_t ready = 0, clients = 0;
            for (auto& [name, pl] : gs.players)
                if (pl->net_controlled()) {
                    ++clients;
                    if (gs.lobby.is_ready(name))
                        ++ready;
                }
            auto text = "ready: " + std::to_string(ready) + "/" + std::to_string(clients);
            ui.label(text.data(), NK_TEXT_LEFT);

            if (gs.lobby.level.empty())
                ui.label("choose a map", NK_TEXT_CENTERED);
            else if (ui.button_label("Start") && _act.start)
                _act.start();

            if (ui.button_label("cancel lobby") && _act.cancel_lobby)
                _act.cancel_lobby();
        }
        else {
            ui.label("waiting for the host to start the game", NK_TEXT_LEFT);
            ui.spacing(1);

            if (ui.button_label("leave") && _act.leave)
                _act.leave();
        }
    }

    /* Player sprite (the player configurator's look) centered in a column wide icon, redrawn when the appearance changes */
    struct icon_t {
        std::string                        key;
        std::unique_ptr<sf::RenderTexture> sprite = std::make_unique<sf::RenderTexture>();
        std::unique_ptr<sf::RenderTexture> target = std::make_unique<sf::RenderTexture>();
        struct nk_image                    img    = {};
    };

    static float icon_height() {
        return std::round(player::default_sprite_size().y * (1.f + player::hat_overflow_factors().y) * sprite_scale);
    }

    icon_t& icon_for(const player& pl) {
        auto color = pl.body_color();
        auto key   = pl.body_texture_path() + "|" + pl.head_texture_path() + "|" + pl.face_texture_path() + "|" + pl.hat_texture_path() + "|" + pl.pistol_section() + "|" +
                   std::to_string(color.toInteger());

        auto& icon = _icons[pl.name()];
        if (icon.key == key)
            return icon;
        icon.key = key;

        sf::Sprite      body(texture_mgr().load(pl.body_texture_path()));
        sf::Sprite      head(texture_mgr().load(pl.head_texture_path()));
        sf::Sprite      face(texture_mgr().load(pl.face_texture_path()));
        std::optional<sf::Sprite> hat;
        if (!pl.hat_texture_path().empty())
            hat.emplace(texture_mgr().load(pl.hat_texture_path()));
        sf::CircleShape hand_or_leg;
        body.setColor(color);
        head.setColor(color);
        hand_or_leg.setFillColor(color);
        weapon* wpn = pl.pistol_section().empty() ? nullptr : &weapon_mgr().load(pl.pistol_section());

        player::draw_to_texture_target(
            *icon.sprite, player::default_sprite_size() * sprite_scale, sprite_scale, body, head, face, hat ? &*hat : nullptr, hand_or_leg, wpn);

        /* Centered (a long gun is scaled down to fit) */
        auto w = u32(column_width), h = u32(icon_height());
        if (!icon.target->resize({w, h}))
            glog().error("cannot create a player icon render texture");
        icon.target->clear({0, 0, 0, 0});
        sf::Sprite sprite(icon.sprite->getTexture());
        auto       size  = icon.sprite->getSize();
        auto       scale = std::min({1.f, float(w) / float(size.x), float(h) / float(size.y)});
        sprite.setScale({scale, scale});
        sprite.setPosition({(float(w) - float(size.x) * scale) * 0.5f, float(h) - float(size.y) * scale});
        icon.target->draw(sprite);
        icon.target->display();
        icon.img = make_nk_image(*icon.target);
        return icon;
    }

    role_t                           _role;
    actions_t                        _act;
    std::vector<std::string>         _levels;
    std::vector<std::string>         _modes;
    std::map<player_name_t, icon_t>  _icons;
};

}
