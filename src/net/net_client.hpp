#pragma once

#include <algorithm>
#include <chrono>
#include <string>

#include <SFML/Network/IpAddress.hpp>

#include "base/types.hpp"
#include "base/log.hpp"
#include "base/signals.hpp"
#include "net/net_protocol.hpp"
#include "net/net_udp.hpp"
#include "game_state.hpp"

namespace dfdh::net {

/**
 * Multiplayer client.
 *
 * Sends HELLO on creation, call update(game_state) once per game frame
 * (from the main thread).
 */
class net_client {
public:
    net_client(const sf::IpAddress& server_ip,
               u16                  server_port,
               const player_name_t& name,
               const std::string&   pistol,
               u32                  face_id,
               u32                  body_id,
               const sf::Color&     body_color,
               const sf::Color&     tracer_color):
        _server_ip(server_ip), _server_port(server_port), _name(name) {
        if (!_sock.connect(server_ip, server_port)) {
            glog().error("net client: can't connect to {}:{} (udp)", server_ip, server_port);
            return;
        }

        glog().info("net client: connecting to {}:{} as {}", server_ip, server_port, name);
#if defined(DFDH_DEBUG_PING)
        glog().info("net client: DFDH_DEBUG_PING is enabled, simulating ~250ms ping (~125ms one-way)");
#endif

        hello h;
        h.name         = name;
        h.pistol       = pistol;
        h.face_id      = face_id;
        h.body_id      = body_id;
        h.body_color   = pack_color(body_color);
        h.tracer_color = pack_color(tracer_color);
        _sock.send(encode_msg(msg_type::hello, h));
    }

    ~net_client() {
        leave();
    }

    [[nodiscard]]
    bool is_ready() const {
        return _sock.is_open();
    }

    [[nodiscard]]
    bool joined() const {
        return _joined;
    }

    [[nodiscard]]
    const player_name_t& name() const {
        return _name;
    }

    /** Fired (deferred) when the server accepted the client */
    signal<void(player_name_t)> sig_joined;
    /** Fired (deferred) when the server rejected the client */
    signal<void(std::string)>    sig_error;

    void send_input(const player_input_state& in) {
        if (!is_ready() || !_joined)
            return;

        input msg;
        msg.seq  = ++_input_seq;
        msg.bits = in.to_bits();
        _sock.send(encode_msg(msg_type::input, msg));
    }

    void update(game_state& gs) {
        if (!is_ready())
            return;

        for (auto& datagram : _sock.recv(std::chrono::milliseconds(2))) {
            net_msg msg;
            try {
                msg = decode_msg({datagram.data.data(), datagram.data.size()});
            }
            catch (const std::exception& e) {
                glog().warn("net client: bad datagram: {}", e.what());
                continue;
            }

            switch (msg.type) {
            case msg_type::welcome: handle_welcome(msg); break;
            case msg_type::state:   handle_state(gs, msg); break;
            case msg_type::error:   handle_error(msg); break;
            default: break;
            }
        }
    }

    void leave() {
        if (!is_ready() || !leave_sent)
            return;

        leave_sent = true;
        _sock.send(encode_msg(msg_type::leave, net::leave{_name}));
        glog().info("net client: left the game");
    }

private:
    void handle_welcome(const net_msg& msg) {
        welcome w;
        if (!try_read_msg(msg.payload, w))
            return;

        _joined = true;
        glog().info("net client: joined the game as {}", w.name);
        sig_joined.emit_deferred(w.name);
    }

    void handle_error(const net_msg& msg) {
        error e;
        if (!try_read_msg(msg.payload, e))
            return;

        glog().error("net client: server error: {}", e.message);
        sig_error.emit_deferred(e.message);
    }

    void handle_state(game_state& gs, const net_msg& msg) {
        state st;
        bool bullet_ids = try_read_msg(msg.payload, st);
        if (!bullet_ids) {
            /* Older server without gun sync: guns are appended last, so a
             * full read fails and the crates layout is tried instead */
            state_v1 v1;
            if (try_read_msg(msg.payload, v1)) {
                st.tick     = v1.tick;
                st.players  = std::move(v1.players);
                st.bullets  = std::move(v1.bullets);
                st.crates   = std::move(v1.crates);
            }
            else {
                /* Even older server without crate sync */
                state_legacy legacy;
                if (!try_read_msg(msg.payload, legacy))
                    return;
                st.tick     = legacy.tick;
                st.players  = std::move(legacy.players);
                st.bullets  = std::move(legacy.bullets);
            }
        }

        for (auto& sp : st.players) {
            auto is_local = _joined && sp.name == _name;

            auto found = gs.players.find(sp.name);
            if (found == gs.players.end()) {
                auto pl = gs.player_create(sp.name);
                if (!pl)
                    continue;
                apply_player_config(*pl, sp);
            }
            else {
                apply_player_config_if_changed(*found->second, sp);
            }

            auto pl = gs.players.at(sp.name).get();

            /*
             * Every server-known player is authoritative: bullets for it come
             * from the server broadcast, so never spawn them locally (this
             * also covers locally simulated bots that exist on both sides)
             */
            pl->set_server_driven(true);

            /*
             * Server state overwrites the position for ALL players, including the
             * locally controlled one (local simulation is prediction only)
             */
            pl->position({sp.pos_x, sp.pos_y});
            pl->velocity({sp.vel_x, sp.vel_y});
            pl->on_left(sp.on_left != 0);

            if (!is_local) {
                /*
                 * Drive the remote player's pistol locally (muzzle flash, recoil,
                 * sounds). The actual bullets arrive via the state broadcast
                 */
                if (sp.shooting)
                    pl->shot();
                else
                    pl->relax();
            }
        }

        /* Spawn the server-side bullets in the local simulation */
        for (size_t i = 0; i < st.bullets.size(); ++i) {
            auto& b   = st.bullets[i];
            auto& blt = gs.blt_mgr.shot(gs.sim,
                                        {b.pos_x, b.pos_y},
                                        b.hit_mass,
                                        {b.vel_x, b.vel_y},
                                        unpack_color(b.tracer_color),
                                        b.gravity);

            if (bullet_ids && i < st.spawn_bullet_ids.size())
                blt.net_id(st.spawn_bullet_ids[i]);
        }

        /*
         * Drop local bullets the server no longer lists as alive: they hit
         * something on the server but may have missed locally because the
         * local simulation diverged slightly
         */
        if (bullet_ids)
            gs.blt_mgr.remove_not_in(st.alive_bullet_ids);

        /* Sync server crates: create ghost crates for new ids, drive their
         * position/velocity, remove crates picked up (or gone) on the server */
        std::vector<u32> received_ids;
        for (auto& ci : st.crates) {
            crate* local = nullptr;
            for (auto& c : gs.crates)
                if (c->id() == ci.id) {
                    local = c.get();
                    break;
                }

            if (!local) {
                local = gs.crate_create_net();
                local->id(ci.id);
                glog().info("net client: crate {} synced at ({}, {})", ci.id, ci.pos_x, ci.pos_y);
            }

            local->position({ci.pos_x, ci.pos_y});
            local->velocity({ci.vel_x, ci.vel_y});
            received_ids.push_back(ci.id);
        }

        for (auto it = gs.crates.begin(); it != gs.crates.end();) {
            bool keep = std::find(received_ids.begin(), received_ids.end(), (*it)->id()) != received_ids.end();
            it = keep ? std::next(it) : gs.crates.erase(it);
        }

        /* Sync server dropped guns: create ghost guns for new ids, drive
         * their position/velocity/angle, remove guns gone on the server */
        std::vector<u32> received_gun_ids;
        for (auto& gi : st.guns) {
            dropped_gun* local = nullptr;
            for (auto& g : gs.dropped_guns.guns())
                if (g->id() == gi.id && g->net_synced()) {
                    local = g.get();
                    break;
                }

            if (!local) {
                local = gs.dropped_guns.spawn_net(gi.id, gi.section, {gi.pos_x, gi.pos_y},
                                                  gi.on_left != 0, gi.long_shot != 0);
                glog().info("net client: dropped gun {} synced at ({}, {})", gi.id, gi.pos_x, gi.pos_y);
            }

            local->position({gi.pos_x, gi.pos_y});
            local->velocity({gi.vel_x, gi.vel_y});
            local->sync_angle(gi.angle);
            received_gun_ids.push_back(gi.id);
        }

        for (auto it = gs.dropped_guns.guns().begin(); it != gs.dropped_guns.guns().end();) {
            auto& g = *it;
            bool keep = !g->net_synced() ||
                        std::find(received_gun_ids.begin(), received_gun_ids.end(), g->id()) != received_gun_ids.end();
            it = keep ? std::next(it) : gs.dropped_guns.guns().erase(it);
        }
    }

    static void apply_player_config(player& pl, const state_player& sp) {
        pl.set_body("player/body" + std::to_string(sp.body_id) + ".png", unpack_color(sp.body_color));
        pl.set_face("player/face" + std::to_string(sp.face_id) + ".png");
        /* An empty pistol section means the server-side player has no gun
         * (e.g. dropped it), so clear it locally as well */
        if (sp.pistol.empty())
            pl.clear_pistol();
        else
            pl.setup_pistol(sp.pistol);
        pl.tracer_color(unpack_color(sp.tracer_color));
    }

    void apply_player_config_if_changed(player& pl, const state_player& sp) {
        if (pl.pistol_section() == sp.pistol &&
            pl.face_texture_path() == ("player/face" + std::to_string(sp.face_id) + ".png") &&
            pl.body_texture_path() == ("player/body" + std::to_string(sp.body_id) + ".png") &&
            pl.body_color() == unpack_color(sp.body_color) &&
            pl.tracer_color() == unpack_color(sp.tracer_color))
            return;

        apply_player_config(pl, sp);
    }

    udp_channel     _sock;
    sf::IpAddress   _server_ip;
    u16             _server_port  = 0;
    player_name_t   _name;
    u16             _input_seq    = 0;
    bool            _joined       = false;
    bool            leave_sent    = false;
};

} // namespace dfdh::net
