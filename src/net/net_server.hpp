#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <SFML/Network/IpAddress.hpp>


#include "base/types.hpp"
#include "base/log.hpp"
#include "net/net_protocol.hpp"
#include "net/net_udp.hpp"
#include "game_state.hpp"

namespace dfdh::net {

inline u32 pack_color(const sf::Color& c) {
    return (u32(c.r) << 24) | (u32(c.g) << 16) | (u32(c.b) << 8) | u32(c.a);
}

inline sf::Color unpack_color(u32 v) {
    return sf::Color(u8((v >> 24) & 0xff), u8((v >> 16) & 0xff), u8((v >> 8) & 0xff), u8(v & 0xff));
}

/**
 * Multiplayer server.
 *
 * Call update(game_state) once per game frame (from the main thread).
 */
class net_server {
public:
    struct client_t {
        sf::IpAddress    address;
        u16              port         = 0;
        player_name_t    player_name;
        bool             joined       = false;
        u16              input_seq    = 0;
        u16              latest_bits  = 0;
        std::vector<u16> pending_bits; // input edges accumulated since the last tick

        [[nodiscard]]
        static std::string key(const sf::IpAddress& addr) {
            return addr.toString();
        }
    };

    net_server(u16 port): _port(port) {
        if (!_sock.bind(port))
            glog().error("net server: can't bind port {}", port);
        else {
            glog().info("net server: listening on udp port {}", port);
#if defined(DFDH_DEBUG_PING)
            glog().info("net server: DFDH_DEBUG_PING is enabled, simulating ~250ms ping (~125ms one-way)");
#endif
        }
    }

    [[nodiscard]]
    bool is_ready() const {
        return _sock.is_open();
    }

    u16 port() const {
        return _port;
    }

    size_t client_count() const {
        return _clients.size();
    }

    [[nodiscard]]
    client_t* find_client(const sf::IpAddress& addr) {
        auto found = _clients.find(client_t::key(addr));
        return found == _clients.end() ? nullptr : &found->second;
    }

    void update(game_state& gs) {
        if (!_sock.is_open())
            return;

        for (auto& datagram : _sock.recv(std::chrono::milliseconds(2)))
            handle_msg(gs, datagram);

        /* Apply inputs and broadcast state once per frame */
        if (!_clients.empty()) {
            net::state st;
            st.tick = ++_tick;
            for (auto& [name, pl] : gs.players)
                st.players.push_back(make_state_player(*pl));

            /*
             * Broadcast all bullets spawned this frame (network players, the
             * host's local player and bots). The client disables local bullet
             * spawning for every server-known player, so there are no
             * duplicates.
             */
            for (auto& be : gs.cur_frame_bullets) {
                st.bullets.push_back(bullet_info{std::string(be.owner),
                                                 be.position.x,
                                                 be.position.y,
                                                 be.velocity.x,
                                                 be.velocity.y,
                                                 be.hit_mass,
                                                 pack_color(be.tracer_color),
                                                 be.gravity});
            }

            for (auto& c : gs.crates) {
                auto pos = c->get_position();
                auto vel = c->get_velocity();
                st.crates.push_back(crate_info{c->id(), pos.x, pos.y, vel.x, vel.y,
                                               c->landed() ? 1 : 0});
            }

            auto data = encode_msg(msg_type::state, st);

            for (auto& [key, client] : _clients) {
                if (!client.joined)
                    continue;

                auto found = gs.players.find(client.player_name);
                if (found != gs.players.end() && found->second)
                    apply_input_per_tick(*found->second, client);

                _sock.send_to(client.address, client.port, data);
            }
        }

        for (auto& [key, client] : _clients)
            client.pending_bits.clear();
    }

private:
    static state_player make_state_player(const player& pl) {
        state_player sp;
        sp.name         = pl.name();
        sp.pistol       = pl.pistol_section();
        sp.face_id      = player_configurator::texture_path_to_index(pl.face_texture_path());
        sp.body_id      = player_configurator::texture_path_to_index(pl.body_texture_path());
        sp.body_color   = pack_color(pl.body_color());
        sp.tracer_color = pack_color(pl.tracer_color());
        auto pos        = pl.get_position();
        auto vel        = pl.get_velocity();
        sp.pos_x        = pos.x;
        sp.pos_y        = pos.y;
        sp.vel_x        = vel.x;
        sp.vel_y        = vel.y;
        sp.jumps_left   = u8(pl.get_available_jumps());
        sp.on_left      = pl.get_on_left() ? 1 : 0;
        sp.shooting     = pl.shotting() ? 1 : 0;
        return sp;
    }

    static void apply_input_per_tick(player& pl, client_t& client) {
        auto in = player_input_state::from_bits(client.latest_bits);
        for (auto bits : client.pending_bits) {
            auto edge = player_input_state::from_bits(bits);
            in.jump         |= edge.jump;
            in.jump_down    |= edge.jump_down;
            in.shot_release |= edge.shot_release;
            in.long_shot    ^= edge.long_shot;
            in.shot         |= edge.shot;
        }

        pl.apply_input(in);
    }

    void handle_msg(game_state& gs, const received_datagram& datagram) {
        net_msg msg;
        try {
            msg = decode_msg({datagram.data.data(), datagram.data.size()});
        }
        catch (const std::exception& e) {
            glog().warn("net server: bad datagram from {}:{}: {}",
                    datagram.address.toString(), datagram.port, e.what());
            return;
        }

        switch (msg.type) {
        case msg_type::hello: handle_hello(gs, datagram.address, datagram.port, msg); break;
        case msg_type::input: handle_input(datagram.address, msg); break;
        case msg_type::leave: handle_leave(gs, datagram.address); break;
        default: break;
        }
    }

    void handle_hello(game_state& gs, const sf::IpAddress& addr, u16 port, const net_msg& msg) {
        hello h;
        if (!try_read_msg(msg.payload, h)) {
            glog().warn("net server: bad HELLO from {}:{}", addr.toString(), port);
            return;
        }

        if (h.name.empty()) {
            glog().error("net server: HELLO with empty player name from {}:{}", addr.toString(), port);
            auto data = encode_msg(msg_type::error, error{"empty player name"});
            _sock.send_to(addr, port, data);
            return;
        }

        /* One client record per address */
        auto& client = _clients[client_t::key(addr)];

        /* Reject duplicate player names from other clients */
        if (client.player_name != h.name) {
            auto found = gs.players.find(h.name);
            if (found != gs.players.end()) {
                glog().error("net server: player {} already exists (hello from {}:{} as '{}')",
                        h.name, addr.toString(), port, h.name);
                auto data = encode_msg(msg_type::error, error{"player already exists"});
                _sock.send_to(addr, port, data);
                return;
            }
        }

        auto was_new = !client.joined;
        auto old_name = client.player_name;
        auto renamed  = !was_new && client.player_name != h.name;
        client.address     = addr;
        client.port        = port;
        client.player_name = h.name;
        client.joined      = true;
        client.latest_bits = 0;
        client.pending_bits.clear();

        /* Create or reconfigure the player */
        player* pl = nullptr;
        auto found = gs.players.find(h.name);
        if (found != gs.players.end())
            pl = found->second.get();
        else
            pl = gs.player_create(h.name);

        if (!pl) {
            glog().error("net server: can't create player {}", h.name);
            client.joined = false;
            return;
        }

        if (renamed && !old_name.empty()) {
            auto old = gs.players.find(old_name);
            if (old != gs.players.end()) {
                gs.players.erase(old);
                glog().info("net server: player {} deleted (renamed to {})", old_name, h.name);
            }
        }

        apply_player_config(*pl, h);
        pl->set_net_controlled(true);
        gs.player_spawn(pl);

        if (was_new)
            glog().info("net server: client {}:{} joined as player {}", addr.toString(), port, h.name);

        auto data = encode_msg(msg_type::welcome, welcome{h.name});
        _sock.send_to(addr, port, data);
    }

    void handle_input(const sf::IpAddress& addr, const net_msg& msg) {
        auto client = find_client(addr);
        if (!client || !client->joined)
            return;

        input in;
        if (!try_read_msg(msg.payload, in))
            return;

        client->input_seq   = in.seq;
        client->latest_bits = in.bits;
        if (client->pending_bits.size() < 32)
            client->pending_bits.push_back(in.bits);
    }

    void handle_leave(game_state& gs, const sf::IpAddress& addr) {
        auto client = find_client(addr);
        if (!client)
            return;

        if (gs.players.contains(client->player_name)) {
            gs.players.erase(gs.players.find(client->player_name));
            glog().info("net server: player {} deleted (client left)", client->player_name);
        }

        _clients.erase(client_t::key(addr));
        glog().info("net server: client {} left", addr.toString());
    }

    static void apply_player_config(player& pl, const hello& h) {
        pl.set_body("player/body" + std::to_string(h.body_id) + ".png", unpack_color(h.body_color));
        pl.set_face("player/face" + std::to_string(h.face_id) + ".png");
        if (!h.pistol.empty())
            pl.setup_pistol(h.pistol);
        pl.tracer_color(unpack_color(h.tracer_color));
    }

    udp_channel                  _sock;
    u16                          _port;
    u16                          _tick = 0;
    std::map<std::string, client_t> _clients;
};

} // namespace dfdh::net
