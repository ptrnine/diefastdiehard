#pragma once

#include "base/types.hpp"
#include "base/vec2.hpp"
#include "base/vec_math.hpp"
#include "nuklear.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dfdh {

/**
 * A column of borderless text options: a hovered option grows and changes its
 * color, the column fades to the alpha it is given
 */
class menu_column {
public:
    static constexpr float row_height = 64.f; /* fits the hovered size: rows don't move */
    static constexpr float row_gap    = 8.f;

    menu_column(const char* wnd_name, std::vector<std::string> options):
        _wnd_name(wnd_name), _options(std::move(options)), _fonts(_options.size()), _hover(_options.size(), 0.f) {}

    [[nodiscard]]
    float height() const {
        return float(_options.size()) * row_height + float(_options.size() - 1) * row_gap;
    }

    [[nodiscard]]
    float alpha() const {
        return _alpha;
    }

    /* Clickable at the full alpha only. Returns the clicked option */
    std::optional<size_t> update(ui_ctx& ui, struct nk_rect bounds, float alpha_target, float dt) {
        _alpha = approach(_alpha, alpha_target, dt / fade_s);
        if (_alpha <= 0.f) {
            std::ranges::fill(_hover, 0.f);
            return {};
        }

        /* Fading column is not clickable */
        bool interactive = alpha_target >= 1.f && _alpha >= 1.f;

        auto ctx   = ui.nk_ctx();
        auto style = &ctx->style;
        ui.style_push_style_item(&style->window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        ui.style_push_vec2(&style->window.padding, {0.f, 0.f});
        ui.style_push_vec2(&style->window.spacing, {0.f, row_gap});

        std::optional<size_t> clicked;
        /* Nuklear takes the bounds of a new window only: keep it in place on resize */
        ui.window_set_bounds(_wnd_name, bounds);
        nk_flags flags = NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR | (interactive ? 0 : NK_WINDOW_NO_INPUT);
        if (ui.begin(_wnd_name, bounds, flags)) {
            /* Covered by another window (e.g. the dev console): no hover */
            bool rom = ctx->current->flags & NK_WINDOW_ROM;
            ui.layout_row_dynamic(row_height, 1);
            for (size_t i = 0; i < _options.size(); ++i) {
                bool hovered = interactive && !rom && nk_input_is_mouse_hovering_rect(&ctx->input, ui.widget_bounds());
                _hover[i]    = approach(_hover[i], hovered ? 1.f : 0.f, dt / hover_s);
                if (option_draw(ui, i) && interactive)
                    clicked = i;
            }
        }
        ui.end();

        ui.style_pop_vec2();
        ui.style_pop_vec2();
        ui.style_pop_style_item();
        return clicked;
    }

private:
    static constexpr float fade_s     = 0.3f;  /* fade in / out time */
    static constexpr float hover_s    = 0.15f; /* hover grow / shrink time */
    static constexpr float font_size  = 40.f;
    static constexpr float font_hover = 52.f; /* <= ui_ctx::menu_font_size: the baked font is only downscaled */

    static constexpr sf::Color text_color  = {200, 200, 200};
    static constexpr sf::Color hover_color = {255, 190, 60};

    static float approach(float value, float target, float step) {
        return value < target ? std::min(value + step, target) : std::max(value - step, target);
    }

    static float smoothstep(float t) {
        return t * t * (3.f - 2.f * t);
    }

    /* Returns true if the option was clicked */
    bool option_draw(ui_ctx& ui, size_t i) {
        auto ctx = ui.nk_ctx();
        auto btn = &ctx->style.button;
        auto h   = smoothstep(_hover[i]);

        auto color = sf::Color(u8(lerp(float(text_color.r), float(hover_color.r), h)),
                               u8(lerp(float(text_color.g), float(hover_color.g), h)),
                               u8(lerp(float(text_color.b), float(hover_color.b), h)),
                               u8(255.f * _alpha));
        auto nk_col = sfml_color_to_nk(color);

        /*
         * The baked font scales by its height (nk_font_text_width / query use
         * height / info.height). The text command keeps the font pointer until
         * ui.render(): the copy lives in a member
         */
        _fonts[i]        = *ui.menu_font();
        _fonts[i].height = lerp(font_size, font_hover, h);

        auto none = nk_style_item_color(nk_rgba(0, 0, 0, 0));
        ui.style_push_style_item(&btn->normal, none);
        ui.style_push_style_item(&btn->hover, none);
        ui.style_push_style_item(&btn->active, none);
        nk_style_push_color(ctx, &btn->border_color, nk_rgba(0, 0, 0, 0));
        nk_style_push_color(ctx, &btn->text_background, nk_rgba(0, 0, 0, 0));
        nk_style_push_color(ctx, &btn->text_normal, nk_col);
        nk_style_push_color(ctx, &btn->text_hover, nk_col);
        nk_style_push_color(ctx, &btn->text_active, nk_col);
        ui.style_push_float(&btn->border, 0.f);
        ui.style_push_font(&_fonts[i]);

        bool clicked = ui.button_label(_options[i].data());

        ui.style_pop_font();
        ui.style_pop_float();
        for (int c = 0; c < 5; ++c)
            ui.style_pop_color();
        for (int s = 0; s < 3; ++s)
            ui.style_pop_style_item();
        return clicked;
    }

    const char*               _wnd_name;
    std::vector<std::string>  _options;
    std::vector<nk_user_font> _fonts; /* sized once: the draw commands point into it */
    std::vector<float>        _hover;
    float                     _alpha = 0.f;
};

/**
 * Main menu: a column of options in the center of the screen, the settings
 * submenu to the right of it (the main column dims meanwhile). The menu fades
 * in / out when shown / hidden. The UI does not know about the game: the
 * actions are callbacks
 */
class main_menu {
public:
    struct actions_t {
        std::function<void()> host;    /* start a server and open its lobby */
        std::function<void()> connect; /* open the connect window */
        std::function<void()> player;  /* settings: open the player configurator */
        std::function<void()> controls; /* settings: open the key bindings */
        std::function<void()> network; /* settings: open the network settings */
        std::function<void()> exit;
    };

    explicit main_menu(actions_t actions): _act(std::move(actions)) {}

    void update(ui_ctx& ui, const vec2f& window_size) {
        auto dt = _frame_timer.elapsed();
        _frame_timer.restart();

        bool settings = _page == page_t::settings;

        auto main_bounds = nk_rect((window_size.x - main_width) * 0.5f,
                                   (window_size.y - _main.height()) * 0.5f,
                                   main_width,
                                   _main.height());
        auto main_target = _show ? (settings ? dim_alpha : 1.f) : 0.f;
        if (auto clicked = _main.update(ui, main_bounds, main_target, dt)) {
            switch (*clicked) {
            case 0: call(_act.host); break;
            case 1: call(_act.connect); break;
            case 2: _page = page_t::settings; break;
            default: call(_act.exit); break;
            }
        }

        /* Just right of the main column */
        auto sub_bounds = nk_rect(main_bounds.x + main_bounds.w,
                                  (window_size.y - _settings.height()) * 0.5f,
                                  settings_width,
                                  _settings.height());
        if (auto clicked = _settings.update(ui, sub_bounds, _show && settings ? 1.f : 0.f, dt)) {
            switch (*clicked) {
            case 0: call(_act.player); break;
            case 1: call(_act.controls); break;
            case 2: call(_act.network); break;
            default: _page = page_t::main; break;
            }
        }
    }

    /* Hiding keeps the page: the settings submenu is back after the player configurator */
    void show(bool value) {
        _show = value;
    }

    [[nodiscard]]
    bool shown() const {
        return _show;
    }

    /* Close the settings submenu: false if it is not open */
    bool back() {
        if (!_show || _page != page_t::settings)
            return false;
        _page = page_t::main;
        return true;
    }

private:
    enum class page_t { main, settings };

    static constexpr float main_width     = 480.f;
    static constexpr float settings_width = 280.f;
    static constexpr float dim_alpha      = 0.35f; /* the main column while the submenu is open */

    static void call(const std::function<void()>& action) {
        if (action)
            action();
    }

    actions_t   _act;
    menu_column _main{"main_menu", {"Host Lobby", "Connect Lobby", "Settings", "Exit"}};
    menu_column _settings{"main_menu_settings", {"Player", "Controls", "Network", "Back"}};
    timer       _frame_timer;
    page_t      _page = page_t::main;
    bool        _show = false;
};

}
