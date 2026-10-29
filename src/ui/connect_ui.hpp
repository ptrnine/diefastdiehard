#pragma once

#include "base/game_clock.hpp"
#include "base/ston.hpp"
#include "ui_pressets.hpp"

#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <string>

namespace dfdh {

/**
 * "Connect to lobby" window: the server address and port (the player is the
 * current one, see player_configurator::current_player), a loader while the client waits for the server and the error of the last
 * attempt. The UI does not know about the network: the actions are callbacks,
 * the owner reports the outcome with connected() / failed()
 */
class connect_ui {
public:
    /* The server did not answer: the attempt fails after this long */
    static constexpr float timeout_s = 5.f;

    /* The fields' text */
    struct params_t {
        std::string host = "127.0.0.1";
        std::string port = "10443";
    };

    struct actions_t {
        /* Start a connection: an error message if it can't start */
        std::function<std::optional<std::string>(const std::string& host, u16 port)> connect;
        /* Abort the connection in progress */
        std::function<void()> cancel;
        /* The fields when the window opens (the last saved ones) */
        std::function<params_t()> load;
        /* The window closed (for any reason): keep the fields */
        std::function<void(const params_t&)> save;
    };

    explicit connect_ui(actions_t actions): _act(std::move(actions)) {
        params(params_t{});
    }

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
        if (ui.begin_titled(wnd_name, "Connect to lobby", rect, NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR)) {
            bool submit = fields_draw(ui);
            status_draw(ui);
            submit = buttons_draw(ui) || submit;
            if (submit && !_connecting)
                connect();
        }
        else {
            /* Closed */
            if (_connecting && _act.cancel)
                _act.cancel();
            _connecting = false;
            show(false);
        }
        ui.end();
    }

    void show(bool value) {
        if (value && !_show) {
            _reopen = true;
            if (_act.load)
                params(_act.load());
        }
        else if (!value && _show && _act.save) {
            _act.save(params());
        }
        _show = value;
    }

    [[nodiscard]]
    params_t params() const {
        return {_host.data(), _port.data()};
    }

    void params(const params_t& value) {
        set_text(_host, value.host.data());
        set_text(_port, value.port.data());
    }

    [[nodiscard]]
    bool shown() const {
        return _show;
    }

    [[nodiscard]]
    bool connecting() const {
        return _connecting;
    }

    /* The connection in progress waited for the server longer than timeout_s */
    [[nodiscard]]
    bool timed_out() const {
        return _connecting && _timer.elapsed() > timeout_s;
    }

    /* The server accepted the client: the window closes */
    void connected() {
        _connecting = false;
        _error.clear();
        show(false);
    }

    /* The connection failed (e.g. rejected by the server): the error is shown */
    void failed(std::string error) {
        _connecting = false;
        _error      = std::move(error);
        show(true);
    }

private:
    static constexpr const char* wnd_name   = "connect";
    static constexpr float       wnd_w      = 420.f;
    static constexpr float       wnd_h      = 220.f;
    static constexpr float       row_height = 25.f;

    using host_buf_t = std::array<char, 256>;
    using port_buf_t = std::array<char, 6>;

    template <size_t S>
    static void set_text(std::array<char, S>& buf, const char* text) {
        std::strncpy(buf.data(), text, S - 1);
        buf[S - 1] = '\0';
    }

    template <size_t S>
    static bool edit(ui_ctx& ui, std::array<char, S>& buf, bool read_only, nk_plugin_filter filter) {
        nk_flags flags = nk_flags(NK_EDIT_FIELD) | nk_flags(NK_EDIT_SIG_ENTER) | (read_only ? nk_flags(NK_EDIT_READ_ONLY) : 0);
        return ui.edit_string_zero_terminated(flags, buf.data(), int(S), filter) & NK_EDIT_COMMITED;
    }

    /* Returns true if enter was pressed in a field */
    bool fields_draw(ui_ctx& ui) {
        bool submit = false;
        ui_layout_row(ui, row_height, {90.f, ui_row::dynamic});

        ui.label("address", NK_TEXT_LEFT);
        submit = edit(ui, _host, _connecting, nk_filter_ascii) || submit;
        ui.label("port", NK_TEXT_LEFT);
        submit = edit(ui, _port, _connecting, nk_filter_decimal) || submit;
        return submit;
    }

    /* The loader while connecting, the last error otherwise */
    void status_draw(ui_ctx& ui) {
        if (_connecting) {
            auto elapsed = _timer.elapsed();
            auto dots    = std::string(size_t(elapsed * 3.f) % 4, '.');
            auto text    = "connecting to " + _target + dots;
            ui.layout_row_dynamic(row_height, 1);
            ui.label(text.data(), NK_TEXT_LEFT);
            /* Fills up until the timeout */
            ui.layout_row_dynamic(12.f, 1);
            ui.prog(nk_size(std::min(elapsed / timeout_s, 1.f) * 1000.f), 1000, false);
        }
        else {
            /* As high as the loader's rows (with the row spacing): the buttons stay in place */
            ui.layout_row_dynamic(row_height + 16.f, 1);
            if (_error.empty())
                ui.spacing(1);
            else
                ui.label_colored_wrap(_error.data(), sf::Color(230, 80, 80));
        }
    }

    /* Returns true if connect was clicked */
    bool buttons_draw(ui_ctx& ui) {
        ui_layout_row(ui, 30.f, {ui_row::dynamic, 120.f});
        ui.spacing(1);
        if (_connecting) {
            if (ui.button_label("cancel")) {
                if (_act.cancel)
                    _act.cancel();
                _connecting = false;
                _error      = "cancelled";
            }
            return false;
        }
        return ui.button_label("connect");
    }

    void connect() {
        std::string host = _host.data();
        std::string port = _port.data();

        if (host.empty()) {
            _error = "enter the server address";
            return;
        }
        u16 port_num = 0;
        try {
            auto value = ston<u64>(port);
            if (value == 0 || value > 65535)
                throw std::out_of_range("port");
            port_num = u16(value);
        }
        catch (const std::exception&) {
            _error = "invalid port '" + port + "'";
            return;
        }

        _target = host + ":" + port;
        _error.clear();
        if (auto error = _act.connect ? _act.connect(host, port_num) : std::optional<std::string>("can't connect")) {
            _error = std::move(*error);
            return;
        }
        _connecting = true;
        _timer.restart();
    }

    actions_t   _act;
    host_buf_t  _host{};
    port_buf_t  _port{};
    std::string _target;
    std::string _error;
    game_timer  _timer;
    bool        _connecting = false;
    bool        _show       = false;
    bool        _reopen     = false;
};

}
