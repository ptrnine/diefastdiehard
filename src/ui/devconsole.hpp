#pragma once

#include "base/log.hpp"
#include "base/vec2.hpp"
#include "nuklear.hpp"
#include "command_buffer.hpp"
#include "basic_view.hpp"

namespace dfdh {
class devconsole : public basic_view {
public:
    static constexpr size_t HISTORY_LEN = 1024;
    static constexpr size_t COMMAND_LEN = 512;

    devconsole(ui_ctx& ui): basic_view(ui, "devconsole") {
        auto ring = log_acceptor_ring_buffer::create(200);
        log_ring  = ring.get();
        glog().add_stream("devconsole", std::move(ring));
    }

    ~devconsole() override {
        glog().remove_stream("devconsole");
    }

    devconsole(const devconsole&) = delete;
    devconsole& operator=(const devconsole&) = delete;
    devconsole(devconsole&&) = default;
    devconsole& operator=(devconsole&&) = default;

    void handle_event(const sf::Event& evt) final {
        if (auto resized = evt.getIf<sf::Event::Resized>()) {
            try_place_into_window({float(resized->size.x), float(resized->size.y)});
        }
        else if (auto pressed = evt.getIf<sf::Event::KeyPressed>()) {
            switch (pressed->code) {
            case sf::Keyboard::Key::Grave:
                toggle();
                break;

            case sf::Keyboard::Key::Up:
                if (!is_active())
                    return;
                history_up();
                break;

            case sf::Keyboard::Key::Down:
                if (!is_active())
                    return;
                history_down();

            default:
                break;
            }
        }
    }

    void on_show() final {}

    void on_close() final {
        _first_run = true;
        _history_pos = HISTORY_LEN;
    }

    void style_start() final {
        auto style = &ui().nk_ctx()->style;
        nk_style_push_color(ui().nk_ctx(), &style->window.background, nk_rgba(20, 20, 20, 210));
        ui().style_push_style_item(&style->window.fixed_background, nk_style_item_color(nk_rgba(20, 20, 20, 210)));
        ui().style_push_style_item(&style->edit.normal, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        ui().style_push_style_item(&style->edit.active, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
        ui().style_push_style_item(&style->edit.hover, nk_style_item_color(nk_rgba(0, 0, 0, 0)));
    }

    void style_end() final {
        ui().style_pop_color();
        ui().style_pop_style_item();
        ui().style_pop_style_item();
        ui().style_pop_style_item();
        ui().style_pop_style_item();
    }

    void print_output(float content_h) {
        /* Return if space is not enough */
        content_h -= 9.f;
        if (content_h < 0.f)
            return;

        static sf::Color level_colors[] = {
            sf::Color(0x67, 0x9d, 0xc0),
            sf::Color(220, 220, 220),
            sf::Color(69, 217, 20),
            sf::Color(225, 195, 52),
            sf::Color(255, 56, 56)
        };

        static constexpr std::string_view level_str[] = {
            ": [debug] "sv,
            ": "sv,
            ": [info] "sv,
            ": [warn] "sv,
            ": [error] "sv
        };

        auto scroll_rows      = scroll.y / uint(_row_height + 4.f);
        auto max_content_rows = uint(content_h / (_row_height + 4.f));
        auto row_padding      = content_h - (float(max_content_rows) * (_row_height + 4.f));
        row_padding           = std::clamp(row_padding, 1.f, 100.f);

        auto records = log_ring->get_records(scroll_rows, max_content_rows + 1);

        auto front_space_rows = records.start_pos() == 0 && records.end_pos() < max_content_rows
                                    ? max_content_rows - records.end_pos()
                                    : records.start_pos();
        auto records_rows     = records.end_pos() - records.start_pos();
        auto back_space_rows  = records.max_pos() - records.end_pos();

        size_t total_rows   = front_space_rows + records_rows + back_space_rows;
        float  total_height = row_padding + float(total_rows) * (_row_height + 4.f);

        auto max_scroll = total_height > content_h ? uint(total_height - content_h - 1) : 0;
        if (max_scroll_reached && (total_rows > prev_total_rows || max_scroll > prev_max_scroll))
            scroll.y = max_scroll;

        prev_max_scroll    = max_scroll;
        prev_total_rows    = total_rows;
        max_scroll_reached = max_scroll == scroll.y;

        /* Make ui */
        if (ui().group_scrolled_begin(&scroll, "devconsole_output", NK_WINDOW_BORDER)) {
            ui().layout_row_dynamic(row_padding, 1);
            ui().layout_row_dynamic(_row_height, 1);

            for (size_t i = 0; i < front_space_rows; ++i) ui().spacing(1);
            for (auto& record : records) {
                std::string msg;
                if (_show_time)
                    msg += record.time;
                if (record.wt == log_acceptor_base::write_type::write_same && record.times > 1) {
                    if (_show_level || _show_time)
                        msg += ' ';
                    if (record.times != std::numeric_limits<uint16_t>::max()) {
                        msg += '(';
                        msg += std::to_string(record.times);
                        msg += " times)";
                    }
                    else {
                        msg += "(repeats infinitely)";
                    }

                    if (!_show_level)
                        msg += ' ';
                }
                if (_show_level)
                    msg += level_str[size_t(record.lvl)];
                msg += record.msg;

                ui().text_colored(msg.data(), int(msg.size()), NK_TEXT_LEFT, level_colors[size_t(record.lvl)]);
            }
            for (size_t i = records.end_pos(); i < records.max_pos(); ++i) ui().spacing(1);

            ui().group_scrolled_end();
        }
        /*
        printfln("scroll: {} max scroll: {} reached: {} th: {} h: {}",
                 scroll.y,
                 max_scroll,
                 max_scroll_reached,
                 total_height,
                 content_h);
        */
    }

    void update() final {
        auto wnd_content_h = ui().window_get_content_region_size().y;
        auto edit_height = _row_height * 2.f;
        if (wnd_content_h > edit_height + 12.f) {
            float group_height = wnd_content_h - edit_height - 12.f;
            ui().layout_row_dynamic(group_height, 1);
            print_output(wnd_content_h - _row_height * 2.f - 24.f);
        }

        ui().layout_row_dynamic(_row_height * 2.f, 1);
        auto edit_state = ui().edit_string(NK_EDIT_FIELD | nk_edit_types(NK_EDIT_SIG_ENTER),
                                           _command_edit.data(),
                                           &_command_len,
                                           int(_command_edit.size()),
                                           nk_filter_default);

        /* Keep the field focused while the console has the focus only (not another window's field) */
        if (ui().nk_ctx()->active == ui().nk_ctx()->current && !ui().nk_ctx()->current->edit.active) {
            ui().nk_ctx()->current->edit.active = 1;
            ui().nk_ctx()->current->edit.cursor = _command_len;
        }

        /* Prohibit tab */
        auto cursor_pos = ui().nk_ctx()->current->edit.cursor;
        if (cursor_pos < _command_len) {
            if (_command_edit[size_t(cursor_pos - 1)] == '\t') {
                std::memmove(_command_edit.data() + cursor_pos - 1,
                             _command_edit.data() + cursor_pos,
                             size_t(_command_len - cursor_pos));
                --ui().nk_ctx()->current->edit.cursor;
                --_command_len;
            }
        }

        if (_command_len > 0 && _command_edit[size_t(_command_len - 1)] == '`')
            _command_len = 0;

        /* Autocomplete */
        if (_command_len != 0 && _command_edit[size_t(_command_len - 1)] == '\t') {
            --_command_len;
            auto command_part = std::string(_command_edit.data(), size_t(_command_len));

            auto found = command_buffer().find(command_part);
            if (found) {
                if (command_part == *found) {
                    auto args     = command_part / split_when(skip_whitespace_outside_quotes());
                    auto cmd_view = (*args.begin());
                    auto cmd      = std::string(cmd_view.begin(), cmd_view.end());

                    if (!_last_help || *_last_help != cmd) {
                        command_buffer().push("help '" + cmd + "'");
                        _last_help = cmd;
                    }
                }
                else {
                    _command_len = int(std::min(found->size(), COMMAND_LEN));
                    std::memcpy(_command_edit.data(), found->data(), size_t(_command_len));
                }
            }
            ui().nk_ctx()->current->edit.cursor = _command_len;
        }

        if (_command_len != 0) {
            size_t found = std::string_view::npos;
            while ((found = std::string_view(_command_edit.data(), size_t(_command_len)).find(char(23))) !=
                   std::string_view::npos) {
                auto postfix = found + 1;
                while (found < size_t(_command_len) && _command_edit[found] != ' ')
                    --found;
                bool was_space = found < size_t(_command_len);
                while (found < size_t(_command_len) && _command_edit[found] == ' ')
                    --found;
                if (was_space)
                    ++found;

                if (found > size_t(_command_len))
                    found = 0;
                auto posfix_len = size_t(_command_len) - postfix;
                if (posfix_len)
                    std::memmove(_command_edit.data() + found, _command_edit.data() + postfix, posfix_len);
                _command_len -= int(postfix - found);
            }

            if (ui().nk_ctx()->current->edit.cursor > _command_len)
                ui().nk_ctx()->current->edit.cursor = _command_len;
        }

        if (_command_len > 0 && edit_state & NK_EDIT_COMMITED) {
            auto command     = std::string(_command_edit.data(), size_t(_command_len));
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
            _last_help.reset();
        }
    }

    /* The cursor of the console's edit field (also from handle_event, outside the window's begin/end) */
    void edit_cursor(int value) {
        if (auto* wnd = ui().window_find(name().data()))
            wnd->edit.cursor = value;
    }

    void history_up() {
        if (_history_pos == HISTORY_LEN) {
            _current_command = std::string(_command_edit.data(), size_t(_command_len));
            _history_pos     = 0;
        }
        else
            ++_history_pos;

        if (_history_pos >= _history.size()) {
            _history_pos = _history.size() == 0 ? 0 : _history.size() - 1;
            return;
        }

        auto& cmd    = *(_history.end() - ssize_t(_history_pos) - 1);
        auto  min_sz = std::min(cmd.size(), COMMAND_LEN);
        std::memcpy(_command_edit.data(), cmd.data(), min_sz);
        _command_len                       = int(min_sz);
        edit_cursor(_command_len + 1);
    }

    void history_down() {
        if (_history_pos == HISTORY_LEN) {
            return;
        }
        else if (_history_pos == 0) {
            _command_len                       = int(_current_command.size());
            edit_cursor(_command_len);
            std::memcpy(_command_edit.data(), _current_command.data(), _current_command.size());
            _history_pos = HISTORY_LEN;
            return;
        }
        else if (_history.size() != 0) {
            --_history_pos;
            auto  pos    = std::min(_history.size() - 1, _history_pos);
            auto& cmd    = *(_history.end() - ssize_t(pos) - 1);
            auto  min_sz = std::min(cmd.size(), COMMAND_LEN);
            std::memcpy(_command_edit.data(), cmd.data(), min_sz);
            _command_len                       = int(min_sz);
            edit_cursor(_command_len);
        }
    }

    void try_place_into_window(const vec2f& window_size) {
        auto sz = size();
        if (sz.x > window_size.x || sz.y > window_size.y)
            place_into_window(window_size);
    }

    void show_time(bool value = true) {
        _show_time = value;
    }

    [[nodiscard]]
    bool is_show_time() const {
        return _show_time;
    }

    void show_level(bool value = true) {
        _show_level = value;
    }

    [[nodiscard]]
    bool is_show_level() {
        return _show_level;
    }

    void clear() {
        glog().detail("~~CLEAR~~");
        log_ring->clear();
    }

    [[nodiscard]]
    size_t ring_size() const {
        return log_ring->max_size();
    }

    void ring_size(size_t value) {
        log_ring->max_size(value);
    }

private:
    log_acceptor_ring_buffer* log_ring;
    ring_buffer<std::string>  _history{HISTORY_LEN};
    size_t                    _history_pos = HISTORY_LEN;

    float     _row_height        = 15;
    nk_scroll scroll             = {0, 0};
    bool      max_scroll_reached = true;
    uint      prev_max_scroll    = 0;
    size_t    prev_total_rows   = 0;

    bool      _first_run        = true;
    bool      _show_time        = false;
    bool      _show_level       = false;

    std::string                       _current_command;
    std::array<char, COMMAND_LEN>     _command_edit;
    int                               _command_len = 0;

    std::optional<std::string>        _last_help;
};

} // namespace dfdh
