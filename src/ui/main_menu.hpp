#pragma once

#include "base/types.hpp"
#include "base/vec2.hpp"
#include "base/vec_math.hpp"
#include "nuklear.hpp"

#include <array>
#include <functional>

namespace dfdh {

/**
 * Main menu: a column of borderless text options in the center of the screen.
 * A hovered option grows and changes its color, the menu fades in / out when
 * shown / hidden. The UI does not know about the game: the actions are callbacks
 */
class main_menu {
public:
    struct actions_t {
        std::function<void()> host;    /* start a server and open its lobby */
        std::function<void()> connect; /* open the connect window */
        std::function<void()> exit;
    };

    explicit main_menu(actions_t actions): _act(std::move(actions)) {}

    void update(ui_ctx& ui, const vec2f& window_size) {
        auto dt = _frame_timer.elapsed();
        _frame_timer.restart();

        _alpha = approach(_alpha, _show ? 1.f : 0.f, dt / fade_s);
        if (_alpha <= 0.f) {
            _hover.fill(0.f);
            return;
        }

        /* Fading menu is not clickable */
        bool interactive = _show && _alpha >= 1.f;

        auto ctx   = ui.nk_ctx();
        auto style = &ctx->style;
        ui.style_push_style_item(&style->window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        ui.style_push_vec2(&style->window.padding, {0.f, 0.f});
        ui.style_push_vec2(&style->window.spacing, {0.f, row_gap});

        auto height = float(options.size()) * row_height + float(options.size() - 1) * row_gap;
        auto bounds = nk_rect((window_size.x - width) * 0.5f, (window_size.y - height) * 0.5f, width, height);
        /* Nuklear takes the bounds of a new window only: keep it centered on resize */
        ui.window_set_bounds(wnd_name, bounds);
        nk_flags flags = NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR | (interactive ? 0 : NK_WINDOW_NO_INPUT);
        if (ui.begin(wnd_name, bounds, flags)) {
            /* Covered by another window (e.g. the dev console): no hover */
            bool rom = ctx->current->flags & NK_WINDOW_ROM;
            ui.layout_row_dynamic(row_height, 1);
            for (size_t i = 0; i < options.size(); ++i) {
                bool hovered = interactive && !rom && nk_input_is_mouse_hovering_rect(&ctx->input, ui.widget_bounds());
                _hover[i]    = approach(_hover[i], hovered ? 1.f : 0.f, dt / hover_s);
                if (option_draw(ui, i) && interactive)
                    option_action(i);
            }
        }
        ui.end();

        ui.style_pop_vec2();
        ui.style_pop_vec2();
        ui.style_pop_style_item();
    }

    void show(bool value) {
        _show = value;
    }

    [[nodiscard]]
    bool shown() const {
        return _show;
    }

private:
    static constexpr const char* wnd_name = "main_menu";

    static constexpr std::array options = {"Host Lobby", "Connect Lobby", "Exit"};

    static constexpr float fade_s      = 0.3f;  /* fade in / out time */
    static constexpr float hover_s     = 0.15f; /* hover grow / shrink time */
    static constexpr float font_size   = 40.f;
    static constexpr float font_hover  = 52.f;  /* <= ui_ctx::menu_font_size: the baked font is only downscaled */
    static constexpr float row_height  = 64.f;  /* fits the hovered size: rows don't move */
    static constexpr float row_gap     = 8.f;
    static constexpr float width       = 480.f;

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

        bool clicked = ui.button_label(options[i]);

        ui.style_pop_font();
        ui.style_pop_float();
        for (int c = 0; c < 5; ++c)
            ui.style_pop_color();
        for (int s = 0; s < 3; ++s)
            ui.style_pop_style_item();
        return clicked;
    }

    void option_action(size_t i) {
        auto& action = i == 0 ? _act.host : i == 1 ? _act.connect : _act.exit;
        if (action)
            action();
    }

    actions_t                                _act;
    std::array<nk_user_font, options.size()> _fonts{};
    std::array<float, options.size()>        _hover{};
    timer                                    _frame_timer;
    float                                    _alpha = 0.f;
    bool                                     _show  = false;
};

}
