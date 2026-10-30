#pragma once

#include "player_controller.hpp"
#include "ui_pressets.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dfdh {

/**
 * "Controls" window (Settings -> Controls): the key bindings of the player
 * controllers, one column each. A clicked binding waits for a key
 * (handle_key), saved right away. Keys bound more than once are red, unset
 * ones dim. Controllers can be added and deleted (not controller 0)
 */
class controls_ui {
public:
    /* The bindings of the controller changed (rebound, deleted) */
    std::function<void(u32 id)> on_change;

    void update(ui_ctx& ui, const vec2f& window_size) {
        if (!_show)
            return;

        if (_reopen) {
            /* Nuklear keeps a closed window hidden */
            ui.window_show(wnd_name, NK_SHOWN);
            _reopen = false;
        }

        auto cols  = float(_ctrls.size());
        auto content_w = std::max(400.f, cols * column_w + 40.f);
        auto wnd_w     = std::min(window_size.x - 40.f, content_w);
        /* The bindings, the header and the bottom rows (with the row spacing), the two section gaps, the title and the padding */
        auto wnd_h = std::min(window_size.y - 40.f,
                              (row_height + row_spacing) * float(std::size(player_controller::bindings) + 2) +
                                  2.f * (section_gap + row_spacing) + 64.f);
        auto rect  = nk_rect((window_size.x - wnd_w) * 0.5f, (window_size.y - wnd_h) * 0.5f, wnd_w, wnd_h);
        ui.window_set_bounds(wnd_name, rect);
        auto style = &ui.nk_ctx()->style;
        ui.style_push_vec2(&style->window.spacing, {style->window.spacing.x, row_spacing});
        /* A scrollbar only when the columns don't fit across (nuklear draws a vertical one anyway) */
        nk_flags flags = NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_CLOSABLE | (content_w > wnd_w ? 0 : NK_WINDOW_NO_SCROLLBAR);
        if (ui.begin_titled(wnd_name, "Controls", rect, flags)) {
            header_draw(ui);
            /* The delete buttons apart from the bindings */
            ui.layout_row_dynamic(section_gap, 1);
            ui.label("", NK_TEXT_LEFT);
            bindings_draw(ui);
            /* add controller apart from the bindings */
            ui.layout_row_dynamic(section_gap, 1);
            ui.label("", NK_TEXT_LEFT);
            bottom_draw(ui);
        }
        else {
            close();
        }
        ui.end();
        ui.style_pop_vec2();
    }

    /**
     * A key press while a binding waits for a key: binds it (Esc cancels).
     * Returns true if the key was taken
     */
    bool handle_key(sf::Keyboard::Key key) {
        if (!_show || !_capture)
            return false;

        auto [id, binding] = *_capture;
        _capture.reset();
        if (key == sf::Keyboard::Key::Escape || key == sf::Keyboard::Key::Unknown)
            return true;

        player_controller::set_key(id, player_controller::bindings[binding].cfg_key, int(key));
        reload();
        if (on_change)
            on_change(id);
        return true;
    }

    void show(bool value) {
        if (value && !_show) {
            _reopen = true;
            reload();
        }
        if (!value)
            _capture.reset();
        _show = value;
    }

    /* The same as the window's close button */
    void close() {
        show(false);
    }

    [[nodiscard]]
    bool shown() const {
        return _show;
    }

private:
    static constexpr const char* wnd_name   = "controls";
    static constexpr float       row_height  = 25.f;
    static constexpr float       row_spacing = 8.f;  /* between the button rows */
    static constexpr float       section_gap = 10.f; /* header / bindings / bottom */
    static constexpr float       label_w    = 90.f;
    static constexpr float       key_w      = 130.f;
    static constexpr float       gap_w      = 24.f;
    static constexpr float       column_w   = label_w + key_w + gap_w + 12.f; /* with nuklear's item spacing */

    static constexpr sf::Color conflict_color = {230, 80, 80};
    static constexpr sf::Color unset_color    = {150, 150, 120};
    static constexpr sf::Color capture_color  = {255, 190, 60};

    void reload() {
        _ctrls.clear();
        for (auto id : player_controller::ids())
            _ctrls.emplace_back(id);

        /* Keys bound more than once (across all controllers: two local players would share them) */
        std::map<int, size_t> uses;
        for (auto& c : _ctrls)
            for (auto& b : player_controller::bindings)
                if (c.*b.key != player_controller::unset)
                    ++uses[c.*b.key];
        _conflicts.clear();
        for (auto [key, count] : uses)
            if (count > 1)
                _conflicts.push_back(key);
    }

    /* label, key and gap per controller */
    void columns_row(ui_ctx& ui) {
        ui.layout_row_begin(NK_STATIC, row_height, int(_ctrls.size()) * 3);
    }

    void header_draw(ui_ctx& ui) {
        std::optional<u32> removed;
        columns_row(ui);
        for (auto& c : _ctrls) {
            ui.layout_row_push(label_w + key_w - 70.f);
            ui.label(("Controller " + std::to_string(c.id() + 1)).data(), NK_TEXT_LEFT);
            ui.layout_row_push(70.f);
            if (c.id() == 0)
                ui.label("", NK_TEXT_LEFT);
            else if (ui.button_label("delete"))
                removed = c.id();
            ui.layout_row_push(gap_w);
            ui.label("", NK_TEXT_LEFT); /* spacing() would wrap the row */
        }
        ui.layout_row_end();

        if (removed) {
            _capture.reset();
            player_controller::remove(*removed);
            reload();
            if (on_change)
                on_change(*removed);
        }
    }

    void bindings_draw(ui_ctx& ui) {
        for (size_t b = 0; b < std::size(player_controller::bindings); ++b) {
            auto& binding = player_controller::bindings[b];
            columns_row(ui);
            for (auto& c : _ctrls) {
                ui.layout_row_push(label_w);
                ui.label(binding.label, NK_TEXT_LEFT);
                ui.layout_row_push(key_w);
                if (key_button(ui, c.id(), b, c.*binding.key))
                    _capture = std::pair{c.id(), b};
                ui.layout_row_push(gap_w);
                ui.label("", NK_TEXT_LEFT);
            }
            ui.layout_row_end();
        }
    }

    /* Returns true if clicked */
    bool key_button(ui_ctx& ui, u32 id, size_t binding, int key) {
        bool capturing = _capture && _capture->first == id && _capture->second == binding;
        bool conflict  = std::ranges::find(_conflicts, key) != _conflicts.end();

        std::optional<sf::Color> color;
        std::string              text;
        if (capturing) {
            text  = "press any key...";
            color = capture_color;
        }
        else if (key == player_controller::unset) {
            text  = "not set";
            color = unset_color;
        }
        else {
            text = key_str_map().to_str(key);
            if (conflict)
                color = conflict_color;
        }

        if (!color)
            return ui.button_label(text.data());

        auto ctx    = ui.nk_ctx();
        auto btn    = &ctx->style.button;
        auto nk_col = sfml_color_to_nk(*color);
        nk_style_push_color(ctx, &btn->text_normal, nk_col);
        nk_style_push_color(ctx, &btn->text_hover, nk_col);
        nk_style_push_color(ctx, &btn->text_active, nk_col);
        /* The border shows a conflict / the waiting binding at a glance */
        bool border = capturing || conflict;
        if (border)
            nk_style_push_color(ctx, &btn->border_color, nk_col);
        bool clicked = ui.button_label(text.data());
        if (border)
            ui.style_pop_color();
        for (int i = 0; i < 3; ++i)
            ui.style_pop_color();
        return clicked;
    }

    void bottom_draw(ui_ctx& ui) {
        ui_layout_row(ui, row_height, {160.f, ui_row::dynamic});
        if (ui.button_label("add controller")) {
            _capture.reset();
            player_controller::create();
            reload();
        }
        if (_capture)
            ui.label_colored("press any key (Esc: cancel)", NK_TEXT_LEFT, capture_color);
        else if (!_conflicts.empty())
            ui.label_colored("red: the key is bound more than once", NK_TEXT_LEFT, conflict_color);
        else
            ui.spacing(1);
    }

    std::vector<player_controller>          _ctrls;
    std::vector<int>                        _conflicts;
    std::optional<std::pair<u32, size_t>>   _capture; /* controller id, bindings index */
    bool                                    _show   = false;
    bool                                    _reopen = false;
};

}
