#pragma once

#include "ui_pressets.hpp"

#include <array>
#include <functional>
#include <string>

namespace dfdh {

/**
 * "Network" settings window (Settings -> Network): the port the hosted
 * servers listen on. A valid value is saved on enter, when the field loses
 * the focus or the window closes (not a half-typed one). The UI
 * does not know about the settings: the actions are callbacks
 */
class network_ui {
public:
    struct actions_t {
        /* The server port when the window opens */
        std::function<u16()> load;
        /* A new valid server port */
        std::function<void(u16)> save;
    };

    explicit network_ui(actions_t actions): _act(std::move(actions)) {}

    void update(ui_ctx& ui, const vec2f& window_size) {
        if (!_show)
            return;

        if (_reopen) {
            /* Nuklear keeps a closed window hidden */
            ui.window_show(wnd_name, NK_SHOWN);
            _reopen = false;
        }

        auto rect = nk_rect((window_size.x - wnd_w) * 0.5f, (window_size.y - wnd_h) * 0.5f, wnd_w, wnd_h);
        ui.window_set_bounds(wnd_name, rect);
        if (ui.begin_titled(wnd_name, "Network", rect, NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR)) {
            ui_layout_row(ui, row_height, {110.f, ui_row::dynamic});
            ui.label("server port", NK_TEXT_LEFT);
            auto res = ui.edit_string_zero_terminated(nk_flags(NK_EDIT_FIELD) | nk_flags(NK_EDIT_SIG_ENTER), _port.data(),
                                                      int(_port.size()), nk_filter_decimal);
            _valid = ui_parse_port(_port.data()).has_value();
            if (res & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED))
                port_save();

            ui.layout_row_dynamic(row_height, 1);
            if (_valid)
                ui.spacing(1);
            else
                ui.label_colored("1..65535 (not saved)", NK_TEXT_LEFT, sf::Color(230, 80, 80));
        }
        else {
            close();
        }
        ui.end();
    }

    void show(bool value) {
        if (value && !_show) {
            _reopen = true;
            _saved  = _act.load ? _act.load() : u16(0);
            ui_set_text(_port, std::to_string(u32(_saved)).data());
            _valid = true;
        }
        _show = value;
    }

    /* The same as the window's close button */
    void close() {
        if (_show)
            port_save();
        show(false);
    }

    [[nodiscard]]
    bool shown() const {
        return _show;
    }

private:
    static constexpr const char* wnd_name   = "network";
    static constexpr float       wnd_w      = 360.f;
    static constexpr float       wnd_h      = 120.f;
    static constexpr float       row_height = 25.f;

    /* Save a changed valid port */
    void port_save() {
        auto port = ui_parse_port(_port.data());
        if (port && *port != _saved) {
            _saved = *port;
            if (_act.save)
                _act.save(*port);
        }
    }

    actions_t            _act;
    std::array<char, 6>  _port{};
    u16                  _saved  = 0;
    bool                 _valid  = true;
    bool                 _show   = false;
    bool                 _reopen = false;
};

}
