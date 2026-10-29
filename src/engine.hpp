#pragma once

#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Window/Event.hpp>

#include "base/args_view.hpp"
#include "base/cfg.hpp"
#include "base/profiler.hpp"

#include "ui/nuklear.hpp"
#include "ui/devconsole.hpp"
#include "ui/main_menu.hpp"

#include "command_buffer.hpp"

namespace dfdh {

namespace defaults {
    static constexpr auto window_size = vec2u{800, 600};
}

class engine {
public:
    static constexpr auto config_path = "data/settings/game_settings.cfg";

    static sf::VideoMode video_mode(vec2u wnd_size) {
        return sf::VideoMode(wnd_size);
    }

    engine():
        _conf(config_path, cfg_mode::create_if_not_exists | cfg_mode::commit_at_destroy | cfg_mode::autocreate_dir),
        _game_settings(_conf.get_or_create("game_settings"_sect)),
        _wnd(video_mode(_game_settings.value_or_default_and_set("window_size", defaults::window_size)), "diefastdiehard"),
        _devcons(ui) {

        if (!_wnd.setActive(true))
            glog().error("cannot activate the window context");
        ui = ui_ctx(_wnd);
    }

    virtual ~engine() {
        _conf.commit();
    }

    int run(args_view args) {
        init_window();
        on_init(std::move(args));

        auto wnd_size = window_size_float();
        _devcons.place_into_window(wnd_size);
        _main_menu.set_size(wnd_size);

        _wnd.setVerticalSyncEnabled(_game_settings.value_or_default_and_set("vsync", true));
        _wnd.setFramerateLimit(_game_settings.value_or_default_and_set("framerate_limit", 60U));

        profiler_print = _game_settings.value_or_default_and_set("profiler", false);

        while (_wnd.isOpen()) {
            {
                auto prof = loop_prof.scope("logic");
                game_update();
            }

            {
                auto prof = loop_prof.scope("events");
                ui.input_begin();
                while (auto event = _wnd.pollEvent()) {
                    auto& evt = *event;
                    ui.handle_event(evt);
                    _devcons.handle_event(evt);

                    if (evt.is<sf::Event::Closed>()) {
                        _wnd.close();
                        goto end;
                    }
                    else if (auto resized = evt.getIf<sf::Event::Resized>()) {
                        on_window_resize(resized->size.x, resized->size.y);
                        _main_menu.set_size({float(resized->size.x), float(resized->size.y)});
                    }
                    else {
                        if (!_devcons.is_active())
                            handle_event(evt);
                    }
                }
                ui.input_end();
            }

            {
                auto prof = loop_prof.scope("ui", false);
                ui_update();
            }

            /* The window may be closed mid-frame (quit from a UI callback); do not render to it */
            if (_wnd.isOpen())
            {
                {
                    auto prof = loop_prof.scope("render");
                    _wnd.clear();
                    render_update(_wnd);
                }

                {
                    auto prof = loop_prof.scope("ui");
                    ui.render();
                }

                {
                    auto prof = loop_prof.scope("swapbuffers");
                    _wnd.display();
                }
            }

            fps_count_frame();

            {
                auto prof = loop_prof.scope("commands");
                command_buffer().run_handlers();
            }

            if (profiler_print) {
                loop_prof.try_print(
                    [](auto& prof) { glog().detail(prof.short_print_format() ? "{}" : "min|max|avg: {}", prof); });
            }
        }

end:
        on_destroy();
        return EXIT_SUCCESS;
    }

    sf::RenderWindow& window() {
        return _wnd;
    }

    devconsole& devcons() {
        return _devcons;
    }

    main_menu& mainmenu() {
        return _main_menu;
    }

    virtual void ui_update() {
        if (fps_show)
            fps_draw();
        devcons().update_internal();
    }

    virtual void on_init(args_view args) = 0;
    virtual void on_destroy() = 0;
    virtual void handle_event(const sf::Event& event) = 0;
    virtual void render_update(sf::RenderWindow& wnd) = 0;
    virtual void game_update() = 0;
    virtual void post_update() = 0;

    virtual void on_window_resize(u32 width, u32 height) {
        _game_settings.set("window_size", vec2u{width, height});
        _game_settings.set("window_pos", window_position());
    }

    /* The [game_settings] section of data/settings/game_settings.cfg (window, connect window fields) */
    cfg_section<false>& game_settings() {
        return _game_settings;
    }

    /* Write the settings now (they are written on exit anyway) */
    void game_settings_commit() {
        _conf.commit();
    }

    void enable_profiler_print(bool value) {
        profiler_print = value;
    }

    void enable_fps_counter(bool value) {
        fps_show = value;
    }

    [[nodiscard]]
    bool is_fps_counter_enabled() const {
        return fps_show;
    }

    vec2u window_size() const {
        return _wnd.getSize();
    }

    vec2i window_position() const {
        return _wnd.getPosition();
    }

    vec2f window_size_float() const {
        auto sz = window_size();
        return {float(sz.x), float(sz.y)};
    }

    vec2u screen_size() const {
        auto desktop_mode = sf::VideoMode::getDesktopMode();
        return desktop_mode.size;
    }

    void resize_window(vec2u size) {
        _wnd.setSize(size);
    }

private:
    void init_window() {
        auto wnd_size  = window_size();
        auto screen_sz = screen_size();

        if (screen_sz.x < wnd_size.x)
            wnd_size.x = screen_sz.x;
        if (screen_sz.y < wnd_size.y)
            wnd_size.y = screen_sz.y;
        if (sf::Vector2u(wnd_size) != _wnd.getSize())
            resize_window(wnd_size);

        auto wnd_pos =
            _game_settings.value_or_default_and_set("window_pos", static_cast<vec2i>((screen_sz / 2) - (wnd_size / 2)));
        _wnd.setPosition(wnd_pos);
    }

    /* Wall-clock rendering rate, not game time: averaged over fps_period */
    void fps_count_frame() {
        auto now = std::chrono::steady_clock::now();
        ++fps_frames;
        if (now - fps_start >= fps_period) {
            fps_value  = float(fps_frames) / std::chrono::duration<float>(now - fps_start).count();
            fps_frames = 0;
            fps_start  = now;
        }
    }

    void fps_draw() {
        auto style = &ui.nk_ctx()->style;
        ui.style_push_style_item(&style->window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 0)));

        if (ui.begin("fps_counter", {4.f, 4.f, 100.f, 32.f}, NK_WINDOW_NO_INPUT | NK_WINDOW_NO_SCROLLBAR)) {
            ui.layout_row_dynamic(20.f, 1);
            auto text = "FPS: " + std::to_string(int(fps_value + 0.5f));
            ui.label_colored(text.data(), NK_TEXT_LEFT, sf::Color(255, 255, 0));
        }
        ui.end();

        ui.style_pop_style_item();
    }

protected:
    ui_ctx ui;

private:
    cfg                 _conf;
    cfg_section<false>  _game_settings;
    sf::ContextSettings _settings{24, 8, 4, 3, 3};
    sf::RenderWindow    _wnd;
    devconsole          _devcons;
    main_menu           _main_menu;
    profiler            loop_prof;
    bool                profiler_print = false;

    static constexpr auto                 fps_period = std::chrono::milliseconds(500);
    bool                                  fps_show   = false;
    std::chrono::steady_clock::time_point fps_start  = std::chrono::steady_clock::now();
    u32                                   fps_frames = 0;
    float                                 fps_value  = 0.f;
};
}
