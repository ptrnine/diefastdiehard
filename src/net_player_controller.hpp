#pragma once

#include <SFML/Window/Event.hpp>

#include "player_controller.hpp"
#include "net/net_protocol.hpp"

namespace dfdh {

/**
 * Client-side player controller for network play.
 *
 * Reuses the `player_controller_<id>` keymap settings; tracks key
 * events and yields a per-frame `player_input_state` that the network
 * client sends to the server.
 */
struct net_player_controller : public player_controller {
    using player_controller::player_controller;

    ~net_player_controller() override = default;

    /**
     * Feed SFML key events (mirrors player::update_input handling)
     */
    void track_event(const sf::Event& evt) {
        if (evt.type == sf::Event::KeyPressed) {
            auto c = evt.key.code;
            if (c == up) {
                _held_jump = true;
                _edge_jump = true;
            }
            else if (c == down) {
                _held_jump_down = true;
                _edge_jump_down = true;
            }
            else if (c == left)
                _held_left = true;
            else if (c == right)
                _held_right = true;
            else if (c == shot)
                _held_shot = true;
            else if (c == drop)
                _edge_drop = true;
            else if (c == adjust_up)
                _held_long_shot = true;
        }
        else if (evt.type == sf::Event::KeyReleased) {
            auto c = evt.key.code;
            if (c == up)
                _held_jump = false;
            else if (c == down)
                _held_jump_down = false;
            else if (c == left)
                _held_left = false;
            else if (c == right)
                _held_right = false;
            else if (c == shot)
                _held_shot = false;
            else if (c == adjust_up)
                _held_long_shot = false;
        }
    }

    /**
     * Get the per-tick input state; edges are consumed
     */
    [[nodiscard]]
    net::player_input_state take_input_state() {
        net::player_input_state in{
            .left         = _held_left,
            .right        = _held_right,
            .jump         = _edge_jump,
            .jump_down    = _edge_jump_down,
            .shot         = _held_shot,
            .long_shot    = _held_long_shot,
            .drop         = _edge_drop,
        };

        _edge_jump      = false;
        _edge_jump_down = false;
        _edge_drop      = false;

        return in;
    }

private:
    bool _held_left        = false;
    bool _held_right       = false;
    bool _held_jump        = false;
    bool _held_jump_down   = false;
    bool _held_shot        = false;
    bool _held_long_shot   = false;

    bool _edge_jump      = false;
    bool _edge_jump_down = false;
    bool _edge_drop      = false;
};

} // namespace dfdh
