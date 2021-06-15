#pragma once

#include "log.hpp"
#include "nuklear.hpp"
#include "command_buffer.hpp"

namespace dfdh {
class devconsole {
public:
    static constexpr size_t HISTORY_LEN = 1024;
    static constexpr size_t COMMAND_LEN = 512;

    devconsole() {
        log_buf = log_fixed_buffer::create(200);
        log().add_output_stream("devconsole", log_buf);
    }

    ~devconsole() {
        log().remove_output_stream("devconsole");
    }

    devconsole(const devconsole&) = delete;
    devconsole& operator=(const devconsole&) = delete;
    devconsole(devconsole&&) = default;
    devconsole& operator=(devconsole&&) = default;

    void handle_event(ui_ctx& ui, const sf::Event& evt) {
        if (evt.type == sf::Event::Resized) {
            update_size(ui, {float(evt.size.width), float(evt.size.height)});
        }
        else if (evt.type == sf::Event::KeyPressed) {
            switch (evt.key.code) {
            case sf::Keyboard::Tilde:
                toggle();
                break;

            case sf::Keyboard::Up:
                if (!_edit_active)
                    return;
                history_up();
                break;

            case sf::Keyboard::Down:
                if (!_edit_active)
                    return;
                history_down();

            default:
                break;
            }
        }
    }

    void history_up() {
        if (_history_pos == HISTORY_LEN) {
            _current_command = std::string(_command_edit.data(), size_t(_command_len));
            _history_pos = 0;
        } else
            ++_history_pos;

        if (_history_pos >= _history.size()) {
            _history_pos = _history.size() == 0 ? 0 : _history.size() - 1;
            return;
        }

        auto& cmd = *(_history.end() - ssize_t(_history_pos) - 1);
        auto min_sz = std::min(cmd.size(), COMMAND_LEN);
        std::memcpy(_command_edit.data(), cmd.data(), min_sz);
        _command_len = int(min_sz);
    }

    void history_down() {
        if (_history_pos == HISTORY_LEN) {
            return;
        } else if (_history_pos == 0) {
            _command_len = int(_current_command.size());
            std::memcpy(_command_edit.data(), _current_command.data(), _current_command.size());
            _history_pos = HISTORY_LEN;
            return;
        } else if (_history.size() != 0) {
            --_history_pos;
            auto pos = std::min(_history.size() - 1, _history_pos);
            auto& cmd = *(_history.end() - ssize_t(pos) - 1);
            auto min_sz = std::min(cmd.size(), COMMAND_LEN);
            std::memcpy(_command_edit.data(), cmd.data(), min_sz);
            _command_len = int(min_sz);
        }
    }

    void update(ui_ctx& ui) {
        static constexpr auto push_line = [](auto& ui, auto&& line) {
            constexpr auto warn_pref = "[warn]:"sv;
            constexpr auto err_pref  = "[error]:"sv;
            auto color = sf::Color(220, 220, 220);

            if (line.size() > LOG_TIME_FMT.size() + 1 + err_pref.size()) {
                auto start_msg = line.data() + LOG_TIME_FMT.size() + 1;
                auto end = line.data() + line.size();

                switch (*start_msg) {
                case '(':
                    ++start_msg;
                    while (isdigit(*start_msg))
                        ++start_msg;
                    if (*start_msg == ' ')
                        ++start_msg;
                    if (std::string_view(start_msg, end).starts_with("times) "sv))
                        start_msg += "times) "sv.size();
                    if (size_t(end - start_msg) < err_pref.size())
                        break;
                    [[fallthrough]];

                case '[':
                    if (std::string_view(start_msg, warn_pref.size()) == warn_pref)
                        color = sf::Color(225, 195, 52);
                    else if (std::string_view(start_msg, err_pref.size()) == err_pref)
                        color = sf::Color(255, 56, 56);
                }
            }

            ui.text_colored(
                line.data(), int(line.size()), NK_TEXT_LEFT, color /*sf::Color(69, 217, 20)*/);
        };

        if (!_show)
            return;

        auto style = &ui.nk_ctx()->style;
        nk_style_push_color(ui.nk_ctx(), &style->window.background, nk_rgba(20, 20, 20, 210));
        ui.style_push_style_item(&style->window.fixed_background, nk_style_item_color(nk_rgba(20, 20, 20, 210)));

        ui.style_push_style_item(&style->edit.normal, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        ui.style_push_style_item(&style->edit.active, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        ui.style_push_style_item(&style->edit.hover, nk_style_item_color(nk_rgba(0, 0, 0, 0)));


        static constexpr float lpush = 4.f;

        if (ui.begin("devconsole",
                     {_pos.x, _pos.y, _size.x, _size.y},
                     NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                         NK_WINDOW_CLOSABLE)) {
            if (_first_run)
                ui.window_set_scroll(_x_scroll, _y_scroll);

            _first_run = false;

            _pos  = ui.window_get_position();
            _size = ui.window_get_size();
            ui.window_get_size();

            auto content_region_size = ui.window_get_content_region_size();
            auto labels_in_region = u32(content_region_size.y / (_row_height + lpush));
            auto lines_count = log_buf->size();
            bool space_enable = (lines_count + 2) < labels_in_region;

            float y_acc = 0.f;
            if (space_enable) {
                ui.layout_space_begin(NK_STATIC, 0, int(labels_in_region - 1));
                y_acc = content_region_size.y - float(lines_count + 2) * (_row_height + lpush);


                for (auto& line : log_buf->get_log_locked(labels_in_region)) {
                    ui.layout_space_push(nk_rect(0, y_acc, content_region_size.x, (_row_height + lpush)));
                    y_acc += _row_height + lpush;
                    push_line(ui, line);
                }
                ui.layout_space_push(nk_rect(0, y_acc, content_region_size.x, _row_height * 2.f));
            }
            else {
                ui.layout_row_dynamic(_row_height, 1);
                for (auto& line : log_buf->get_log_locked(labels_in_region * 4))
                    push_line(ui, line);
                ui.layout_row_dynamic(_row_height * 2.f, 1);
            }

            auto edit_state = ui.edit_string(NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
                                             _command_edit.data(),
                                             &_command_len,
                                             int(_command_edit.size()),
                                             nk_filter_default);

            if (edit_state & NK_EDIT_ACTIVATED)
                _edit_active = true;
            if (edit_state & NK_EDIT_DEACTIVATED)
                _edit_active = false;

            if (_command_len > 0 && edit_state & NK_EDIT_COMMITED) {
                auto command = std::string(_command_edit.data(), size_t(_command_len));

                bool only_spaces = true;
                for (auto c : command) {
                    if (c != ' ' && c != '\t' && c != '\n') {
                        only_spaces = false;
                        break;
                    }
                }

                if (!only_spaces) {
                    command_buffer().push(command);
                    if (_history.empty() || _history.back() != command)
                        _history.push(std::move(command));
                }

                _command_len = 0;
                _history_pos = HISTORY_LEN;
            }

            if (space_enable)
                ui.layout_space_end();

            if (lines_count > _lines_count_last) {
                auto& scroll = ui.nk_ctx()->current->scrollbar;
                scroll.y += 100000;
            }
            _lines_count_last = u32(lines_count);

            ui.window_get_scroll(&_x_scroll, &_y_scroll);
        }
        else {
            _show = false;
            _first_run = true;
            _edit_active = false;
        }
        ui.end();

        ui.style_pop_color();
        ui.style_pop_style_item();
        ui.style_pop_style_item();
        ui.style_pop_style_item();
        ui.style_pop_style_item();
    }

    void set_size(ui_ctx& ui, const sf::Vector2f& window_size) {
        auto sz_x = std::max(window_size.x - 40.f, 20.f);
        auto sz_y = std::max(window_size.y - 40.f, 20.f);
        _pos = {20.f, 20.f};
        _size = {sz_x, sz_y};
        ui.window_set_size("devconsole", _size);
    }

    void update_size(ui_ctx& ui, const sf::Vector2f& window_size) {
        if (_size.x > window_size.x || _size.y > window_size.y)
            set_size(ui, window_size);
    }

    void show(bool value) {
        if (_show && !value) {
            _first_run = true;
            _edit_active = false;
        }

        _show = value;
    }

    void toggle() {
        show(!_show);
    }

private:
    std::shared_ptr<log_fixed_buffer> log_buf;
    ring_buffer<std::string>          _history{HISTORY_LEN};
    size_t                            _history_pos = HISTORY_LEN;

    float                             _row_height = 15;
    sf::Vector2f                      _pos;
    sf::Vector2f                      _size;
    u32                               _lines_count_last = 0;
    u32                               _x_scroll = 0;
    u32                               _y_scroll = 0;
    bool                              _show = false;
    bool                              _first_run = true;
    bool                              _edit_active = false;

    std::string                       _current_command;
    std::array<char, COMMAND_LEN>     _command_edit;
    int                               _command_len = 0;
};
}