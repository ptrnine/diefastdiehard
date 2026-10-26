/* Exact float checks are intended here: serialization round trips and deterministic sim state */
#pragma GCC diagnostic ignored "-Wfloat-equal"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "net/net_protocol.hpp"
#include "net/net_udp.hpp"
#include "net/net_server.hpp"
#include "net/net_client.hpp"
#include "net_player_controller.hpp"
#include "game_state.hpp"
#include "game_commands.hpp"
#include "script/lua_init.hpp" /* luacpp pushers for dfdh types */

using namespace dfdh;

/* One simulation tick (inputs are applied by the network layer on ticks) */
static void sim_tick(game_state& gs) {
    gs.sim.update_immediate(1.f / 60.f, game_clock::now());
}

static u16 free_udp_port() {
    net::udp_channel ch;
    REQUIRE(ch.bind(0));
    auto port = ch.local_port();
    return port;
}

TEST_CASE("protocol: message round-trips") {
    using namespace net;

    SECTION("hello") {
        hello h;
        h.name         = "tester";
        h.pistol       = "wpn_glk17";
        h.face_id      = 1;
        h.body_id      = 2;
        h.body_color   = 0x11223344;
        h.tracer_color = 0x55667788;

        auto data = encode_msg(msg_type::hello, h);
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::hello);

        hello r = read_msg<hello>(msg.payload);
        CHECK(r.version == protocol_version);
        CHECK(std::string(r.name) == "tester");
        CHECK(r.pistol == "wpn_glk17");
        CHECK(r.face_id == 1);
        CHECK(r.body_id == 2);
        CHECK(r.body_color == 0x11223344);
        CHECK(r.tracer_color == 0x55667788);
    }

    SECTION("welcome") {
        auto data = encode_msg(msg_type::welcome, welcome{"john"});
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::welcome);

        auto w = read_msg<welcome>(msg.payload);
        CHECK(std::string(w.name) == "john");
    }

    SECTION("input") {
        input in;
        in.last_seq       = 42;
        in.client_time_ms = 1234;
        in.rtt_ms         = 250;
        in.interp_ms      = 90;
        in.bits           = {in_bit_left, in_bit_left | in_bit_shot};

        auto data = encode_msg(msg_type::input, in);
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::input);

        auto r = read_msg<input>(msg.payload);
        CHECK(r.last_seq == 42);
        CHECK(r.first_seq() == 41);
        CHECK(r.client_time_ms == 1234);
        CHECK(r.rtt_ms == 250);
        CHECK(r.interp_ms == 90);
        REQUIRE(r.bits.size() == 2);
        CHECK(r.bits[1] == (in_bit_left | in_bit_shot));

        /* A window reaching before seq 1 is malformed */
        in.last_seq = 1;
        auto bad    = encode_msg(msg_type::input, in);
        input r_bad;
        CHECK_FALSE(try_read_msg(decode_msg({bad.data(), bad.size()}).payload, r_bad));
    }

    SECTION("state") {
        state st;
        st.tick                = 7;
        st.server_time_ms      = 5000;
        st.ack_seq             = 77;
        st.echo_client_time_ms = 900;
        st.echo_hold_ms        = 16;
        st.own_weapon          = weapon_sync{.ammo = 5, .anim = 2, .anim_time = 0.25f, .since_shot = 1.5f,
                                             .flags = weapon_sync::flag_reloading, .shot_index = 3, .seed_base = 99};
        st.configs.push_back(player_config{.id = 2, .name = "p2", .pistol = "wpn_m40", .face_id = 3,
                                           .tracer_color = 0xFF00FF00, .group = -1});
        st.players.push_back(state_player{.id = 1, .pos_x = 10.f, .pos_y = 20.f, .vel_x = -3.f, .vel_y = 4.f,
                                          .flags = state_player::flag_lock_y | state_player::flag_on_left |
                                                   state_player::flag_shooting,
                                          .jumps_left = 1, .accel_lf = pack_unit(0.5f), .teleports = 9,
                                          .score = -3, .hits = {4, 5}});
        st.players.push_back(state_player{.id = 2});

        auto data = encode_msg(msg_type::state, st);
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::state);

        auto r = read_msg<state>(msg.payload);
        CHECK(r.tick == 7);
        CHECK(r.server_time_ms == 5000);
        CHECK(r.ack_seq == 77);
        CHECK(r.echo_client_time_ms == 900);
        CHECK(r.echo_hold_ms == 16);

        REQUIRE(r.own_weapon);
        CHECK(r.own_weapon->ammo == 5);
        CHECK(r.own_weapon->anim == 2);
        CHECK(r.own_weapon->anim_time == 0.25f);
        CHECK(r.own_weapon->since_shot == 1.5f);
        CHECK(r.own_weapon->flags == weapon_sync::flag_reloading);
        CHECK(r.own_weapon->shot_index == 3);
        CHECK(r.own_weapon->seed_base == 99);

        REQUIRE(r.configs.size() == 1);
        CHECK(r.configs[0] == st.configs[0]);

        REQUIRE(r.players.size() == 2);
        CHECK(r.players[0].id == 1);
        CHECK(r.players[0].pos_x == 10.f); /* full precision: the client reconciles with it */
        CHECK(r.players[0].pos_y == 20.f);
        CHECK(r.players[0].vel_x == -3.f);
        CHECK(r.players[0].vel_y == 4.f);
        CHECK(r.players[0].flags == (state_player::flag_lock_y | state_player::flag_on_left | state_player::flag_shooting));
        CHECK(r.players[0].jumps_left == 1);
        CHECK(unpack_unit(r.players[0].accel_lf) == Catch::Approx(0.5f).margin(1e-4));
        CHECK(unpack_unit(r.players[0].accel_rf) == 1.f);
        CHECK(r.players[0].teleports == 9);
        CHECK(r.players[0].score == -3);
        CHECK(r.players[0].hits == std::vector<u32>{4, 5});
        CHECK(r.players[1].id == 2);
        CHECK_FALSE(r.players[1].flags);
    }

    SECTION("state with the game mode") {
        state st;
        st.mode = game_mode_info{"gm_ballistic", 500.f, 5.f, 3};

        auto data = encode_msg(msg_type::state, st);
        auto r    = read_msg<state>(decode_msg({data.data(), data.size()}).payload);
        REQUIRE(r.mode);
        CHECK(r.mode->section == "gm_ballistic");
        CHECK(r.mode->bullet_gravity == 500.f);
        CHECK(r.mode->crate_drop_period == 5.f);
        CHECK(r.mode->magazines_count == 3);

        st.mode.reset();
        data = encode_msg(msg_type::state, st);
        CHECK_FALSE(read_msg<state>(decode_msg({data.data(), data.size()}).payload).mode);
    }

    SECTION("state with shots and the alive bullets") {
        state st;
        st.tick = 5;
        st.shots.push_back(shot_info{.owner = 1, .first_id = 10, .pull_seq = 30, .shot_index = 2, .first_pellet = 0,
                                     .start_x = -1.f, .start_y = 2.f, .forward = shot_info::pack_forward(0.125f),
                                     .hit_mass = 1.5f, .flags = shot_info::flag_gravity,
                                     .pellets = {{100.f, 0.f}, {90.f, 10.f}}});
        st.alive_bullets = alive_set::from_ids(std::vector<u32>{11, 3, 20});

        auto data = encode_msg(msg_type::state, st);
        auto msg  = decode_msg({data.data(), data.size()});
        auto r    = read_msg<state>(msg.payload);
        CHECK(r.tick == 5);
        REQUIRE(r.shots.size() == 1);
        CHECK(r.shots[0].owner == 1);
        CHECK(r.shots[0].first_id == 10);
        CHECK(r.shots[0].pull_seq == 30);
        CHECK(r.shots[0].shot_index == 2);
        CHECK(r.shots[0].start_x == -1.f);
        CHECK(r.shots[0].forward_s() == Catch::Approx(0.125f).margin(1e-4));
        CHECK(r.shots[0].hit_mass == 1.5f);
        CHECK(r.shots[0].flags == shot_info::flag_gravity);
        REQUIRE(r.shots[0].pellets.size() == 2);
        CHECK(r.shots[0].pellets[1].vel_x == 90.f);
        CHECK(r.shots[0].pellets[1].vel_y == 10.f);

        for (u32 id = 0; id < 30; ++id)
            CHECK(r.alive_bullets.contains(id) == (id == 3 || id == 11 || id == 20));
        CHECK_FALSE(alive_set{}.contains(0));
    }

    SECTION("a typical state fits in a few hundred bytes") {
        /* 4 players, a 7 pellet shot, 30 bullets alive, 2 crates, 1 dropped gun (1131 bytes in protocol v6) */
        state st;
        st.own_weapon = weapon_sync{};
        for (u16 id = 1; id <= 4; ++id)
            st.players.push_back(state_player{.id = id, .hits = {101}});
        st.shots.push_back(shot_info{.owner = 2, .first_id = 200, .pellets = std::vector<pellet_info>(7)});
        st.crates.resize(2);
        st.guns.push_back(gun_info{.section = "wpn_ak9"});
        std::vector<u32> alive;
        for (u32 id = 170; id < 200; ++id)
            alive.push_back(id);
        st.alive_bullets = alive_set::from_ids(alive);

        auto size = encode_msg(msg_type::state, st).size();
        UNSCOPED_INFO("state datagram: " << size << " bytes");
        CHECK(size <= 370);

        /* Configs ride along only when a client may not know them */
        for (u16 id = 1; id <= 4; ++id)
            st.configs.push_back(player_config{.id = id, .name = "client" + std::to_string(u32(id)), .pistol = "wpn_1887"});
        CHECK(encode_msg(msg_type::state, st).size() <= 360 + 4 * 40);
    }

    SECTION("leave") {
        auto data = encode_msg(msg_type::leave, leave{"p1"});
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::leave);

        auto r = read_msg<leave>(msg.payload);
        CHECK(std::string(r.name) == "p1");
    }

    SECTION("error") {
        auto data = encode_msg(msg_type::error, error{"player already exists"});
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::error);

        auto r = read_msg<error>(msg.payload);
        CHECK(r.message == "player already exists");
    }
}

TEST_CASE("protocol: malformed datagrams") {
    using namespace net;

    SECTION("too short") {
        std::vector<u8> data{1, 2};
        REQUIRE_THROWS_AS(decode_msg({data.data(), data.size()}), std::out_of_range);
    }

    SECTION("truncated payload") {
        hello h;
        h.name   = "tester";
        h.pistol = "wpn_glk17";
        auto data = encode_msg(msg_type::hello, h);
        REQUIRE(data.size() > net_header_size + 1);
        data.resize(net_header_size + 1);
        REQUIRE_THROWS_AS(decode_msg({data.data(), data.size()}), std::out_of_range);
    }

    SECTION("header/payload size mismatch") {
        auto data = encode_msg(msg_type::input, input{.last_seq = 1, .bits = {0}});
        /* claim a bigger payload than actually present */
        auto claimed = u16(data.size() - net_header_size + 4);
        std::memcpy(data.data() + 2, &claimed, sizeof(claimed));
        REQUIRE_THROWS_AS(decode_msg({data.data(), data.size()}), std::out_of_range);

        /* truncated payload read */
        std::vector<u8> short_payload(2);
        input r;
        CHECK_FALSE(try_read_msg(std::span<const u8>(short_payload.data(), short_payload.size()), r));
    }
}

TEST_CASE("protocol: input bits mapping") {
    using namespace net;

    player_input_state in{
        .left   = true,
        .jump   = true,
        .shot   = true,
        .long_shot = true,
    };

    auto bits = in.to_bits();
    CHECK(bits == (in_bit_left | in_bit_jump | in_bit_shot | in_bit_long_shot));

    auto back = player_input_state::from_bits(bits);
    CHECK(back.left == true);
    CHECK(back.right == false);
    CHECK(back.jump == true);
    CHECK(back.jump_down == false);
    CHECK(back.shot == true);
    CHECK(back.long_shot == true);

    /* Round-trip through the wire */
    auto data = encode_msg(msg_type::input, input{.last_seq = 1, .bits = {bits}});
    auto msg  = decode_msg({data.data(), data.size()});
    auto wire = read_msg<input>(msg.payload);
    back = player_input_state::from_bits(wire.bits.at(0));
    CHECK(back.left == in.left);
    CHECK(back.right == in.right);
    CHECK(back.jump == in.jump);
    CHECK(back.jump_down == in.jump_down);
    CHECK(back.shot == in.shot);
    CHECK(back.long_shot == in.long_shot);
}

TEST_CASE("protocol: dropped gun state sync") {
    using namespace net;

    /* drop input bit round-trips */
    player_input_state in{.drop = true};
    auto bits = in.to_bits();
    CHECK(bits == in_bit_drop);
    auto back = player_input_state::from_bits(bits);
    CHECK(back.drop == true);
    CHECK(back.shot == false);
    CHECK(back.left == false);

    /* state with a gun round-trips over the wire */
    state st;
    st.tick = 42;
    st.guns.push_back(gun_info{7, "wpn_test", 10.f, 20.f, 3.f, 4.f, 45.f, 1, 0, 3, 250});
    auto data = encode_msg(msg_type::state, st);
    auto msg  = decode_msg({data.data(), data.size()});
    auto wire = read_msg<state>(msg.payload);
    REQUIRE(wire.guns.size() == 1);
    CHECK(wire.tick == 42);
    CHECK(wire.guns[0].id == 7);
    CHECK(wire.guns[0].section == "wpn_test");
    CHECK(wire.guns[0].pos_x == 10.f);
    CHECK(wire.guns[0].pos_y == 20.f);
    CHECK(wire.guns[0].vel_x == 3.f);
    CHECK(wire.guns[0].vel_y == 4.f);
    CHECK(wire.guns[0].angle == 45.f);
    CHECK(wire.guns[0].on_left == 1);
    CHECK(wire.guns[0].long_shot == 0);
    CHECK(wire.guns[0].owner == 3);
    CHECK(wire.guns[0].age_ms == 250);
}

TEST_CASE("dropped guns: unique ids and the thrower") {
    game_state gs;
    auto       pl = gs.player_create("thrower");
    REQUIRE(pl != nullptr);

    auto* a = gs.dropped_guns.spawn("wpn_glk17", {0.f, 0.f}, false, false, -1, {0.f, 0.f}, 0.f);
    auto* b = gs.dropped_guns.spawn("wpn_glk17", {0.f, 0.f}, false, false, -1, {0.f, 0.f}, 0.f);
    CHECK(a->id() != 0);
    CHECK(b->id() != 0);
    CHECK(a->id() != b->id());

    gs.game_run(true);
    pl->setup_pistol("wpn_ak9");
    REQUIRE(pl->drop_gun());
    for (int i = 0; i < 60 && pl->pistol_section() == "wpn_ak9"; ++i)
        sim_tick(gs);
    REQUIRE(gs.dropped_guns.size() == 3);
    auto& thrown = *gs.dropped_guns.guns().back();
    CHECK(thrown.owner() == pl->name());
    CHECK_FALSE(thrown.predicted());
    CHECK(thrown.id() != b->id());
}

TEST_CASE("weapon slots: secondary guns, respawn with the secondary gun") {
    game_state gs;
    REQUIRE(gs.level_current("lvl_aes"));
    auto pl = gs.player_create("slots");
    REQUIRE(pl != nullptr);

    for (auto wpn : {"wpn_glk17", "wpn_brn10", "wpn_de", "wpn_sw500"})
        CHECK(weapon::is_secondary_section(wpn));
    for (auto wpn : {"wpn_1887", "wpn_416", "wpn_ak9", "wpn_ss50", "wpn_m40", "wpn_vss", "wpn_vssk"})
        CHECK_FALSE(weapon::is_secondary_section(wpn));
    CHECK_FALSE(weapon::is_secondary_section("wpn_missing"));

    /* A secondary pickup becomes the secondary gun, a primary one does not */
    pl->setup_pistol("wpn_de");
    pl->setup_pistol("wpn_ak9");
    CHECK(pl->secondary_section() == "wpn_de");
    CHECK_FALSE(pl->holds_secondary());

    gs.player_on_dead(pl);
    CHECK(pl->pistol_section() == "wpn_de");
    CHECK(pl->holds_secondary());
}

TEST_CASE("game mode: limited magazines, an emptied gun is dropped") {
    game_state gs;
    REQUIRE(gs.level_current("lvl_aes"));
    auto pl = gs.player_create("shooter");
    REQUIRE(pl != nullptr);
    gs.game_run(true);
    pl->teleport({1300.f, 1040.f});

    SECTION("secondary guns are unlimited") {
        auto wpn = GENERATE(as<std::string>{}, "wpn_glk17", "wpn_sw500");
        pl->setup_pistol(wpn, 2);
        CHECK(pl->get_gun().ammo_reserve() == weapon_instance::unlimited_ammo);
    }

    SECTION("no magazines count: unlimited") {
        pl->setup_pistol("wpn_ak9", 0);
        CHECK(pl->get_gun().ammo_reserve() == weapon_instance::unlimited_ammo);
    }

    SECTION("mag_size * magazines_count rounds, then the gun is dropped and the secondary one is taken") {
        auto wpn = GENERATE(as<std::string>{}, "wpn_ak9", "wpn_1887"); /* magazine and shell by shell reloads */
        pl->setup_pistol(wpn, 2);
        auto mag_size = pl->get_gun().get_weapon()->mag_size();
        CHECK(pl->get_gun().ammo_elapsed() == mag_size);
        CHECK(pl->get_gun().ammo_reserve() == mag_size);

        u32 shots = 0;
        pl->shot();
        for (int i = 0; i < 60 * 60 && pl->pistol_section() == wpn; ++i) {
            sim_tick(gs);
            if (pl->pistol_section() == wpn)
                shots = pl->get_gun().get_sync_state().shot_index;
        }
        CHECK(pl->pistol_section() == "wpn_glk17");
        CHECK(shots == mag_size * 2);
        CHECK(gs.dropped_guns.size() == 1);
    }
}

TEST_CASE("udp channel: loopback") {
    using namespace net;

    auto port = free_udp_port();

    udp_channel srv;
    REQUIRE(srv.bind(port));

    udp_channel cli;
    REQUIRE(cli.bind(0));

    SECTION("single datagram") {
        auto data = encode_msg(msg_type::welcome, welcome{"test"});
        REQUIRE(cli.send_to(sf::IpAddress("127.0.0.1"), port, data));

        auto recvs = srv.recv(std::chrono::milliseconds(50));
        REQUIRE(recvs.size() == 1);
        CHECK(recvs[0].address.toString() == "127.0.0.1");
        CHECK(recvs[0].port == cli.local_port());
        CHECK(recvs[0].data == data);
    }

    SECTION("drain multiple datagrams") {
        auto data = encode_msg(msg_type::input, input{.last_seq = 1, .bits = {in_bit_left}});
        for (int i = 0; i < 5; ++i)
            REQUIRE(cli.send_to(sf::IpAddress("127.0.0.1"), port, data));

        auto recvs = srv.recv(std::chrono::milliseconds(50));
        REQUIRE(recvs.size() == 5);
    }

    SECTION("timeout with nothing to read") {
        auto recvs = srv.recv(std::chrono::milliseconds(20));
        REQUIRE(recvs.empty());
    }

    SECTION("connected send") {
        udp_channel cli2;
        REQUIRE(cli2.connect(sf::IpAddress("127.0.0.1"), port));
        auto data = encode_msg(msg_type::leave, leave{"x"});
        REQUIRE(cli2.send(data));
        auto recvs = srv.recv(std::chrono::milliseconds(50));
        REQUIRE(recvs.size() == 1);
    }
}

TEST_CASE("net_player_controller: key events to input state") {
    net_player_controller ctrl{0}; /* use the existing player_controller_0 section */

    auto keypress = [](sf::Keyboard::Key k) {
        sf::Event evt;
        evt.type  = sf::Event::KeyPressed;
        evt.key.code = k;
        return evt;
    };
    auto keyrelease = [](sf::Keyboard::Key k) {
        sf::Event evt;
        evt.type  = sf::Event::KeyReleased;
        evt.key.code = k;
        return evt;
    };

    SECTION("move keys") {
        ctrl.track_event(keypress(sf::Keyboard::Left));
        auto in = ctrl.take_input_state();
        CHECK(in.left == true);
        CHECK(in.right == false);

        ctrl.track_event(keypress(sf::Keyboard::Right));
        in = ctrl.take_input_state();
        CHECK(in.left == true);
        CHECK(in.right == true);

        ctrl.track_event(keyrelease(sf::Keyboard::Left));
        in = ctrl.take_input_state();
        CHECK(in.left == false);
        CHECK(in.right == true);
    }

    SECTION("jump is an edge") {
        ctrl.track_event(keypress(sf::Keyboard::Up));
        auto in = ctrl.take_input_state();
        CHECK(in.jump == true);

        in = ctrl.take_input_state();
        CHECK(in.jump == false);

        ctrl.track_event(keyrelease(sf::Keyboard::Up));
        ctrl.track_event(keypress(sf::Keyboard::Up));
        in = ctrl.take_input_state();
        CHECK(in.jump == true);
    }

    SECTION("jump down is an edge") {
        ctrl.track_event(keypress(sf::Keyboard::Down));
        auto in = ctrl.take_input_state();
        CHECK(in.jump_down == true);
        in = ctrl.take_input_state();
        CHECK(in.jump_down == false);
    }

    SECTION("shot held and released") {
        ctrl.track_event(keypress(sf::Keyboard::Comma));
        auto in = ctrl.take_input_state();
        CHECK(in.shot == true);
        in = ctrl.take_input_state();
        CHECK(in.shot == true); /* held, not consumed */

        ctrl.track_event(keyrelease(sf::Keyboard::Comma));
        in = ctrl.take_input_state();
        CHECK(in.shot == false);
    }

    SECTION("long shot is held") {
        ctrl.track_event(keypress(sf::Keyboard::M));
        auto in = ctrl.take_input_state();
        CHECK(in.long_shot == true);
        in = ctrl.take_input_state();
        CHECK(in.long_shot == true);

        ctrl.track_event(keyrelease(sf::Keyboard::M));
        in = ctrl.take_input_state();
        CHECK(in.long_shot == false);
    }

    SECTION("drop is an edge") {
        auto drop_key = sf::Keyboard::Key(ctrl.drop);

        ctrl.track_event(keypress(drop_key));
        auto in = ctrl.take_input_state();
        CHECK(in.drop == true);
        in = ctrl.take_input_state();
        CHECK(in.drop == false);

        ctrl.track_event(keyrelease(drop_key));
        ctrl.track_event(keypress(drop_key));
        in = ctrl.take_input_state();
        CHECK(in.drop == true);
    }
}

TEST_CASE("player: apply_input") {
    game_state gs;
    auto pl = gs.player_create("app_input_test");
    REQUIRE(pl != nullptr);

    SECTION("movement") {
        pl->position({0.f, -100.f});
        pl->velocity({0.f, 0.f});

        pl->apply_input(net::player_input_state{.left = true});
        CHECK(pl->is_walking());

        pl->apply_input(net::player_input_state{});
        CHECK_FALSE(pl->is_walking());

        pl->apply_input(net::player_input_state{.right = true});
        CHECK(pl->is_walking());
    }

    SECTION("jump") {
        pl->position({0.f, -100.f}); /* in the air */
        pl->velocity({0.f, 0.f});

        pl->apply_input(net::player_input_state{.jump = true});
        gs.sim.update_immediate(1.f / 60.f, game_clock::now());
        CHECK(pl->get_velocity().y < -500.f);
    }

    SECTION("long shot held") {
        pl->apply_input(net::player_input_state{.long_shot = true});
        CHECK(pl->long_shot_enabled());
        pl->apply_input(net::player_input_state{});
        CHECK_FALSE(pl->long_shot_enabled());
    }

    SECTION("shot") {
        pl->apply_input(net::player_input_state{.shot = true});
        CHECK(pl->get_gun().pull_trigger(std::nullopt) == false); /* trigger is already held */
        pl->apply_input(net::player_input_state{});
        CHECK(pl->get_gun().pull_trigger(std::nullopt) == true);  /* trigger is free again */
    }

    SECTION("the secondary gun is never dropped") {
        REQUIRE(pl->pistol_section() == "wpn_glk17");
        gs.game_run(true);
        pl->apply_input(net::player_input_state{.drop = true});
        CHECK_FALSE(pl->throwing());
        CHECK_FALSE(pl->drop_gun());
    }

    SECTION("drop gun") {
        pl->setup_pistol("wpn_ak9");
        auto before = gs.dropped_guns.size();

        gs.game_run(true);

        /* The arm winds up and throws: the gun is released at the end of the throw */
        pl->apply_input(net::player_input_state{.drop = true});
        CHECK(pl->throwing());
        CHECK(pl->throw_duration() >= 0.2f);
        CHECK(pl->throw_duration() <= 0.5f);
        CHECK_FALSE(pl->shot()); /* no shots while throwing */
        CHECK_FALSE(pl->drop_gun()); /* already throwing */

        auto ticks = int(pl->throw_duration() * 60.f);
        for (int i = 0; i < ticks - 1; ++i)
            sim_tick(gs);
        CHECK(pl->throwing());
        CHECK(gs.dropped_guns.size() == before);

        for (int i = 0; i < 2 && pl->throwing(); ++i)
            sim_tick(gs);
        CHECK_FALSE(pl->throwing());
        CHECK(pl->pistol_section() == "wpn_glk17"); /* the secondary gun right away */
        CHECK(gs.dropped_guns.size() == before + 1);
    }

    SECTION("throw pose") {
        pl->setup_pistol("wpn_ak9");
        REQUIRE(pl->drop_gun());
        auto d      = pl->throw_duration();
        auto windup = pl->throw_pose(d * 0.75f);
        auto end    = pl->throw_pose(d);
        auto dir    = pl->facing_dir().x;

        /* Wound up: the arm is back and up, the muzzle raised */
        CHECK(windup.first.x * dir < 0.f);
        CHECK(windup.first.y < 0.f);
        CHECK(windup.second < -45.f);
        /* Released: the arm is in front, the muzzle tips down */
        CHECK(end.first.x * dir > 0.f);
        CHECK(end.second > 0.f);
    }
}

TEST_CASE("player: physic state snapshot and isolated re-simulation") {
    game_state gs;
    auto pl = gs.player_create("resim_test");
    REQUIRE(pl != nullptr);
    gs.sim.gravity({0.f, 1500.f});

    pl->position({100.f, -300.f});
    pl->velocity({0.f, 0.f});
    auto start = pl->get_physic_state();

    std::vector<net::player_input_state> inputs;
    for (int i = 0; i < 40; ++i)
        inputs.push_back({.right = i < 30, .jump = i == 5});

    /* Regular simulation */
    auto t = game_clock::now();
    for (auto& in : inputs) {
        pl->apply_movement_input(in);
        t += std::chrono::milliseconds(16);
        gs.sim.update_immediate(1.f / 60.f, t);
    }
    auto simulated = pl->get_physic_state();

    /* Restore and re-simulate the player alone */
    pl->restore_physic_state(start);
    pl->replaying(true);
    for (auto& in : inputs) {
        pl->apply_movement_input(in);
        gs.sim.step_isolated(pl->collision_box(), 1.f / 60.f, [&](float dt) { pl->physic_update(gs.sim, dt); });
    }
    pl->replaying(false);
    auto resimulated = pl->get_physic_state();

    CHECK(resimulated.pos.x == Catch::Approx(simulated.pos.x));
    CHECK(resimulated.pos.y == Catch::Approx(simulated.pos.y));
    CHECK(resimulated.vel.x == Catch::Approx(simulated.vel.x));
    CHECK(resimulated.vel.y == Catch::Approx(simulated.vel.y));
    CHECK(resimulated.jumps_left == simulated.jumps_left);
}

TEST_CASE("player: server-driven bullet hit gives feedback without knockback") {
    game_state gs;
    auto pl = gs.player_create("hit_test");
    REQUIRE(pl != nullptr);
    pl->velocity({0.f, 0.f});

    pl->set_server_driven(true);
    pl->on_bullet_hit({1000.f, 0.f}, pl->get_position());
    CHECK(pl->get_velocity().x == 0.f);
    CHECK(pl->pop_on_hit_event());

    pl->set_server_driven(false);
    pl->on_bullet_hit({1000.f, 0.f}, pl->get_position());
    CHECK(pl->get_velocity().x > 0.f);
}

TEST_CASE("player: position_trace_lookup") {
    using namespace std::chrono_literals;

    game_state gs;
    auto pl = gs.player_create("trace_test");
    REQUIRE(pl != nullptr);

    /* Samples at t0, t0+100ms, t0+200ms with x = 0, 100, 200 */
    auto t0 = game_clock::now();
    for (int i = 0; i < 3; ++i) {
        pl->position({float(i) * 100.f, 0.f});
        pl->velocity({0.f, 0.f});
        gs.sim.update_immediate(1.f / 60.f, t0 + i * 100ms); /* records a trace sample */
    }

    auto now = t0 + 200ms;

    auto mid = pl->position_trace_lookup(now, 0.15f); /* t0+50ms: between the first two samples */
    REQUIRE(mid);
    CHECK(mid->x == Catch::Approx(50.f).margin(1.f));

    auto late = pl->position_trace_lookup(now, 0.025f); /* t0+175ms */
    REQUIRE(late);
    CHECK(late->x == Catch::Approx(175.f).margin(1.f));

    CHECK_FALSE(pl->position_trace_lookup(now, 0.f)); /* newer than every sample */
}

TEST_CASE("net_server: handshake, rename and leave") {
    game_clock_manual_guard clock_guard; /* the waits advance the game time by hand */
    auto port = free_udp_port();
    game_state gs;
    net::net_server srv{port};
    REQUIRE(srv.is_ready());

    net::udp_channel cli;
    REQUIRE(cli.connect(sf::IpAddress("127.0.0.1"), port));

    SECTION("join") {
        net::hello h;
        h.name   = "test1";
        h.pistol = "wpn_glk17";
        h.face_id = 1;
        h.body_id = 0;
        h.body_color   = net::pack_color(sf::Color(1, 2, 3, 4));
        h.tracer_color = net::pack_color(sf::Color(5, 6, 7, 8));
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));

        srv.update(gs);

        auto recvs = cli.recv(std::chrono::milliseconds(50));
        /* The server answers WELCOME and also broadcasts a STATE snapshot */
        REQUIRE(recvs.size() >= 1);
        auto msg = net::decode_msg({recvs[0].data.data(), recvs[0].data.size()});
        REQUIRE(msg.type == net::msg_type::welcome);
        auto w = net::read_msg<net::welcome>(msg.payload);
        CHECK(std::string(w.name) == "test1");

        /* Player created with the requested settings */
        REQUIRE(gs.players.contains("test1"));
        auto& pl = *gs.players.at("test1");
        CHECK(pl.face_texture_path() == "player/face1.png");
        CHECK(pl.pistol_section() == "wpn_glk17");
        CHECK(pl.body_color() == sf::Color(1, 2, 3, 4));
        CHECK(pl.tracer_color() == sf::Color(5, 6, 7, 8));
        CHECK(srv.client_count() == 1);
    }

    SECTION("join with a primary gun: the default secondary one") {
        net::hello h;
        h.name   = "test1";
        h.pistol = "wpn_ak9";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));

        srv.update(gs);

        REQUIRE(gs.players.contains("test1"));
        CHECK(gs.players.at("test1")->pistol_section() == "wpn_glk17");
    }

    SECTION("rename on rejoin") {
        net::hello h1;
        h1.name = "test1";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h1)));
        srv.update(gs);
        REQUIRE(gs.players.contains("test1"));

        net::hello h2;
        h2.name = "test2";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h2)));
        srv.update(gs);

        auto recvs = cli.recv(std::chrono::milliseconds(50));
        REQUIRE(recvs.size() >= 1);
        REQUIRE(!gs.players.contains("test1"));
        CHECK(gs.players.contains("test2"));
        CHECK(srv.client_count() == 1);
    }

    SECTION("input drives the player") {
        net::hello h;
        h.name = "test1";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));
        srv.update(gs);
        cli.recv(std::chrono::milliseconds(50)); /* consume welcome */

        REQUIRE(gs.players.contains("test1"));
        auto& pl = *gs.players.at("test1");

        /* Fill the input jitter buffer */
        auto L = net::in_bit_left;
        net::input in;
        in.last_seq = 3;
        in.bits     = {L, L, L};
        REQUIRE(cli.send(net::encode_msg(net::msg_type::input, in)));

        srv.update(gs);
        sim_tick(gs);
        CHECK(pl.is_walking());

        in.last_seq = 6;
        in.bits     = {L, L, L, 0, 0, 0};
        REQUIRE(cli.send(net::encode_msg(net::msg_type::input, in)));
        srv.update(gs);
        for (int i = 0; i < 3; ++i)
            sim_tick(gs);
        CHECK_FALSE(pl.is_walking());
    }

    SECTION("protocol version mismatch is rejected") {
        net::hello h;
        h.version = net::protocol_version + 1;
        h.name    = "test1";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));
        srv.update(gs);

        auto recvs = cli.recv(std::chrono::milliseconds(50));
        REQUIRE(recvs.size() == 1);
        auto msg = net::decode_msg({recvs[0].data.data(), recvs[0].data.size()});
        CHECK(msg.type == net::msg_type::error);
        CHECK_FALSE(gs.players.contains("test1"));
    }

    SECTION("inputs apply once, in seq order, whatever the datagram order") {
        net::hello h;
        h.name = "test1";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));
        srv.update(gs);
        cli.recv(std::chrono::milliseconds(50));
        auto& pl = *gs.players.at("test1");

        auto window = [](u32 last_seq, std::vector<u8> bits) {
            return net::encode_msg(net::msg_type::input, net::input{.last_seq = last_seq, .bits = std::move(bits)});
        };
        auto S = net::in_bit_shot;

        /* Ticks 1-2 shoot, tick 3 releases the trigger */
        auto press   = window(2, {S, S});
        auto release = window(3, {S, S, 0});

        /* The release window arrives first, the stale press window arrives late */
        REQUIRE(cli.send(release));
        srv.update(gs);
        REQUIRE(cli.send(press));
        srv.update(gs);

        sim_tick(gs);
        CHECK(pl.shotting());
        sim_tick(gs);
        CHECK(pl.shotting());
        sim_tick(gs);
        CHECK_FALSE(pl.shotting());

        /* A duplicate of an old window is ignored: the trigger stays released */
        REQUIRE(cli.send(press));
        srv.update(gs);
        sim_tick(gs);
        CHECK_FALSE(pl.shotting());
    }

    SECTION("starving server holds the last input, timeout releases it") {
        net::hello h;
        h.name = "test1";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));
        srv.update(gs);
        cli.recv(std::chrono::milliseconds(50));
        auto& pl = *gs.players.at("test1");

        auto R = net::in_bit_right;
        REQUIRE(cli.send(net::encode_msg(net::msg_type::input,
                                         net::input{.last_seq = 3, .bits = {u8(R | net::in_bit_jump), R, R}})));
        srv.update(gs);
        sim_tick(gs);
        CHECK(pl.is_walking());

        /* Not enough queued after running dry (jitter buffer): the held movement repeats */
        sim_tick(gs);
        sim_tick(gs);

        sim_tick(gs);
        CHECK(pl.is_walking());

        game_clock::advance(net::net_server::input_timeout + std::chrono::milliseconds(50));
        sim_tick(gs);
        CHECK_FALSE(pl.is_walking());
    }

    SECTION("input queue overflow merges the oldest ticks, keeping their edges") {
        net::hello h;
        h.name = "test1";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));
        srv.update(gs);
        cli.recv(std::chrono::milliseconds(50));
        auto& pl = *gs.players.at("test1");
        pl.velocity({0.f, 0.f});

        /* 10 ticks queued at once, a jump edge in the oldest one */
        std::vector<u8> bits(10, 0);
        bits[0] = net::in_bit_jump;
        REQUIRE(cli.send(net::encode_msg(net::msg_type::input, net::input{.last_seq = 10, .bits = bits})));
        srv.update(gs);

        sim_tick(gs);
        CHECK(pl.get_velocity().y < -500.f); /* the merged jump edge was applied */
    }

    SECTION("leave removes the player") {
        net::hello h;
        h.name = "test1";
        REQUIRE(cli.send(net::encode_msg(net::msg_type::hello, h)));
        srv.update(gs);
        REQUIRE(gs.players.contains("test1"));

        REQUIRE(cli.send(net::encode_msg(net::msg_type::leave, net::leave{"test1"})));
        srv.update(gs);

        CHECK_FALSE(gs.players.contains("test1"));
        CHECK(srv.client_count() == 0);
    }

    SECTION("bad datagrams are ignored") {
        std::vector<u8> garbage{0, 0};
        REQUIRE(cli.send(garbage));
        srv.update(gs); /* must not crash */
        CHECK(srv.client_count() == 0);
        CHECK(gs.players.empty());
    }
}

TEST_CASE("net_client: join and state application") {
    game_clock_manual_guard clock_guard; /* the waits advance the game time by hand */
    auto port = free_udp_port();
    game_state gs_srv;
    game_state gs_cli;
    net::net_server srv{port};
    REQUIRE(srv.is_ready());

    /* A bot on the server, so the client has a remote player to sync */
    auto other = gs_srv.player_create("remote_bot");
    REQUIRE(other != nullptr);
    other->position({123.5f, 45.5f});
    other->velocity({10.f, -2.f});

    /* The local player on the client side (created like the game does on join) */
    auto local = gs_cli.player_create("me");
    REQUIRE(local != nullptr);
    local->position({999.f, 999.f});

    bool joined = false;
    net::net_client cli{
        sf::IpAddress("127.0.0.1"), port, "me", "wpn_glk17", 0, 0,
        sf::Color(255, 255, 255), sf::Color(255, 255, 0)};
    REQUIRE(cli.is_ready());
    cli.sig_joined.attach_function("mp_joined_test", [&](const player_name_t& name) {
        joined = true;
        CHECK(std::string(name) == "me");
    });

    SECTION("welcome handshake") {
        srv.update(gs_srv); /* process HELLO, send WELCOME */

        cli.update(gs_cli);
        signal_slot_event_updater::instance().operate_tasks();

        CHECK(joined);
        CHECK(cli.joined());
        CHECK(gs_srv.players.contains("me"));
    }

    net::player_input_state cli_input;
    cli.input_source([&] { return cli_input; });

    /* The server broadcasts once per tick: without ticking (players would move), wait for the idle broadcast */
    auto broadcast = [&] {
        game_clock::advance(net::net_server::idle_broadcast);
        srv.update(gs_srv);
    };

    /* Sample the client input on a client tick, send it and apply it on a server tick */
    auto send_input = [&](const net::player_input_state& in) {
        cli_input = in;
        cli.update(gs_cli);
        for (size_t i = 0; i < net::net_server::target_queued_inputs; ++i)
            sim_tick(gs_cli);
        cli.update(gs_cli);
        srv.update(gs_srv);
        sim_tick(gs_srv);
    };

    SECTION("client sends input to the server") {
        srv.update(gs_srv);
        cli.update(gs_cli);

        send_input({.left = true});
        REQUIRE(gs_srv.players.contains("me"));
        CHECK(gs_srv.players.at("me")->is_walking());
        /* ...and applies it locally as well */
        CHECK(local->is_walking());
    }

    SECTION("server state is applied to remote and local players") {
        srv.update(gs_srv); /* welcome */
        cli.update(gs_cli);

        REQUIRE(gs_srv.players.contains("me"));
        gs_srv.players.at("me")->position({55.f, 66.f});

        /* Local prediction moved the local player — it must snap to the server state */
        local->position({999.f, 999.f});

        broadcast(); /* broadcast state */
        cli.update(gs_cli);

        /* Remote player was created on the client with the server position */
        REQUIRE(gs_cli.players.contains("remote_bot"));
        auto& pl = *gs_cli.players.at("remote_bot");
        CHECK(pl.get_position().x == Catch::Approx(123.5f));
        CHECK(pl.get_position().y == Catch::Approx(45.5f));
        CHECK(pl.get_velocity().x == Catch::Approx(10.f));
        CHECK(pl.get_velocity().y == Catch::Approx(-2.f));

        /* The local player position is overwritten by the server state */
        auto& me = *gs_cli.players.at("me");
        CHECK(me.get_position().x == Catch::Approx(55.f));
        CHECK(me.get_position().y == Catch::Approx(66.f));
    }

    SECTION("the server's game mode is applied on the client") {
        srv.update(gs_srv); /* welcome */
        cli.update(gs_cli);

        /* Both start with the default game mode */
        CHECK(gs_srv.cur_game_mode.section == "gm_default");
        CHECK_FALSE(gs_srv.gravity_for_bullets);
        CHECK_FALSE(gs_srv.cur_game_mode.crates_enabled());

        REQUIRE(gs_srv.game_mode_apply("ballistic"));
        CHECK(gs_srv.cur_game_mode.section == "gm_ballistic");
        CHECK(gs_srv.gravity_for_bullets);
        CHECK(gs_srv.sim.bullet_gravity().y == 500.f);

        broadcast();
        cli.update(gs_cli);
        CHECK(gs_cli.game_mode_networked);
        CHECK(gs_cli.cur_game_mode == gs_srv.cur_game_mode);
        CHECK(gs_cli.gravity_for_bullets);
        CHECK(gs_cli.sim.bullet_gravity().y == 500.f);

        /* A later change reaches the client as well */
        REQUIRE(gs_srv.game_mode_apply("gm_default"));
        broadcast();
        cli.update(gs_cli);
        CHECK(gs_cli.cur_game_mode.section == "gm_default");
        CHECK_FALSE(gs_cli.gravity_for_bullets);
        CHECK(gs_cli.sim.bullet_gravity().y == 0.f);

        /* An unknown game mode is not applied */
        CHECK_FALSE(gs_srv.game_mode_apply("no_such_mode"));
        CHECK(gs_srv.cur_game_mode.section == "gm_default");
    }

    SECTION("server broadcasts bullets fired by network players") {
        srv.update(gs_srv); /* welcome */
        cli.update(gs_cli);

        /* Hold the trigger on the client */
        send_input({.shot = true}); /* apply shot to the server player */
        CHECK(gs_srv.players.at("me")->net_controlled());
        CHECK(gs_srv.players.at("me")->shotting());

        gs_srv.game_run(true);

        /*
         * The pistol shot timer is real-time based (fire_rate 400 => 150ms delay),
         * so step frames with a small sleep until the first shot is fired
         */
        for (int i = 0; i < 300 && gs_srv.cur_frame_bullets.empty(); ++i) {
            game_clock::advance(std::chrono::milliseconds(5));
            gs_srv.game_update();
        }

        REQUIRE(!gs_srv.cur_frame_bullets.empty());
        CHECK(std::string(gs_srv.cur_frame_bullets.front().owner) == "me");

        broadcast(); /* broadcast state with the bullet */
        cli.update(gs_cli);

        /* The client spawned the broadcast bullet in its own simulation */
        CHECK(!gs_cli.blt_mgr.bullets().empty());
    }

    SECTION("server broadcasts bullets fired by its own local player") {
        srv.update(gs_srv); /* welcome */
        cli.update(gs_cli);

        /*
         * A player that exists only on the server (like the host's local
         * player or a server bot): attach a controller so it is simulated
         */
        auto host = gs_srv.player_create("hostp");
        REQUIRE(host != nullptr);
        host->setup_pistol("wpn_glk17");
        auto ctrl = std::make_shared<player_controller>(5);
        ctrl->player_name = "hostp";
        REQUIRE(gs_srv.controller_attach(5, ctrl));
        gs_srv.rebuild_controlled_players();

        REQUIRE(host->shot());

        gs_srv.game_run(true);

        for (int i = 0; i < 300 && gs_srv.cur_frame_bullets.empty(); ++i) {
            game_clock::advance(std::chrono::milliseconds(5));
            gs_srv.game_update();
        }

        REQUIRE(!gs_srv.cur_frame_bullets.empty());
        CHECK(std::string(gs_srv.cur_frame_bullets.front().owner) == "hostp");

        broadcast(); /* broadcast state with the bullet */
        cli.update(gs_cli);

        /*
         * The client created the server-local player from the state and
         * spawned its broadcast bullet in the local simulation
         */
        REQUIRE(gs_cli.players.contains("hostp"));
        CHECK(gs_cli.players.at("hostp")->server_driven());
        CHECK(!gs_cli.blt_mgr.bullets().empty());
    }

    SECTION("client drops bullets the server already removed") {
        srv.update(gs_srv); /* welcome */
        cli.update(gs_cli);

        /* Hold the trigger so the server spawns an authoritative bullet */
        send_input({.shot = true});

        gs_srv.game_run(true);

        for (int i = 0; i < 300 && gs_srv.cur_frame_bullets.empty(); ++i) {
            game_clock::advance(std::chrono::milliseconds(5));
            gs_srv.game_update();
        }
        REQUIRE(!gs_srv.cur_frame_bullets.empty());

        broadcast(); /* broadcast the bullet with its id */
        cli.update(gs_cli);

        /* The client spawned the bullet and tagged it with the server id */
        REQUIRE(gs_cli.blt_mgr.bullets().size() == 1);
        CHECK(gs_cli.blt_mgr.bullets().front().net_id() != 0);

        /*
         * The bullet hit something on the server and was removed there, but
         * missed locally (the client sim is not stepped, so it can't die
         * locally either): the client must drop it on the next state
         */
        gs_srv.blt_mgr.bullets().front().physic()->delete_later();
        gs_srv.blt_mgr.remove_dead();
        CHECK(gs_srv.blt_mgr.bullets().empty());

        /*
         * Step a frame like the game does (game_update clears
         * cur_frame_bullets, then the net update broadcasts)
         */
        gs_srv.game_update();
        broadcast();
        cli.update(gs_cli);

        CHECK(gs_cli.blt_mgr.bullets().empty());
    }
}

TEST_CASE("net_sync: server clock and snapshot interpolation") {
    using namespace net;

    SECTION("server clock follows the least delayed state, interpolation covers the jitter") {
        server_clock clk;
        /* Server clock = local - 10s, one-way delay 80..170ms */
        for (int i = 0; i < 60; ++i) {
            double local = 100.0 + i * 0.016;
            double delay = 0.08 + 0.09 * double((i * 7) % 10) / 9.0;
            clk.on_state(local, local - 10.0 - delay);
        }
        auto now = 100.0 + 60 * 0.016;
        CHECK(clk.server_now(now) == Catch::Approx(now - 10.0 - 0.08).margin(0.001));
        CHECK(clk.interp_delay() == Catch::Approx(0.09 + server_clock::interp_margin_s).margin(0.001));

        /* Render time is monotonic and moves with the local clock */
        double prev = clk.render_time(now);
        for (int i = 1; i < 100; ++i) {
            double t = clk.render_time(now + i * 0.01);
            CHECK(t >= prev);
            CHECK(t - prev == Catch::Approx(0.01).margin(0.0011));
            prev = t;
        }
    }

    SECTION("snapshot buffer interpolates and clamps") {
        struct val {
            float x;
            static val interpolate(const val& a, const val& b, float f, float) {
                return {a.x + (b.x - a.x) * f};
            }
        };

        snapshot_buffer<val> buf;
        CHECK_FALSE(buf.sample(1.0));

        buf.push(2.0, {20.f});
        buf.push(1.0, {10.f}); /* out of order */
        buf.push(1.0, {99.f}); /* duplicate time is ignored */

        CHECK(buf.sample(0.5)->x == Catch::Approx(10.f));
        CHECK(buf.sample(1.25)->x == Catch::Approx(12.5f));
        CHECK(buf.sample(3.0)->x == Catch::Approx(20.f));
    }

    SECTION("knockback offset") {
        /* Toy physics: a platform x < 100 at y 0, friction on it, gravity off it */
        struct st_t {
            vec2f pos;
            vec2f vel;
            bool  lock_y;
        };
        using kb_t        = knockback_offset<int, st_t>;
        constexpr float dt = 1.f / 60.f;
        auto stepper = [](st_t& st, float ts) {
            st.pos += st.vel * ts;
            st.lock_y = st.lock_y && st.pos.x < 100.f;
            if (st.lock_y) {
                auto dec = 700.f * ts;
                st.vel.x = std::abs(st.vel.x) <= dec ? 0.f : st.vel.x - std::copysign(dec, st.vel.x);
            }
            else
                st.vel.y += 1200.f * ts;
        };
        auto apply_hit = [](st_t& st, const vec2f& impulse) { st.vel += impulse; };
        auto run = [&](kb_t& kb, float seconds, float timeout = 10.f) {
            for (float t = 0.f; t < seconds; t += dt)
                kb.step(dt, dt, timeout, stepper);
        };
        auto standing = st_t{{0.f, 0.f}, {0.f, 0.f}, true};

        /* A push slides on the platform and stops: v^2 / (2 * friction) */
        kb_t push;
        push.predict(1, {200.f, 0.f}, standing, apply_hit);
        run(push, 1.f);
        CHECK(push.offset().x == Catch::Approx(200.f * 200.f / 1400.f).margin(4.f));
        CHECK(push.offset().y == Catch::Approx(0.f));

        /* A hard push leaves the platform past its edge and falls (geometry from the stepper) */
        kb_t off_edge;
        off_edge.predict(1, {600.f, 0.f}, standing, apply_hit);
        run(off_edge, 0.35f); /* passes the edge at ~0.17 s */
        CHECK(off_edge.offset().x > 100.f);
        CHECK(off_edge.offset().y > 5.f);
        CHECK(off_edge.grounded() == false);
        run(off_edge, 1.f);
        CHECK(off_edge.offset().y > 100.f); /* not limited: keeps falling like the player would */

        /* Hits in a row accumulate on one episode (one ghost) */
        kb_t burst;
        burst.predict(1, {100.f, 0.f}, standing, apply_hit);
        burst.predict(2, {100.f, 0.f}, standing, apply_hit);
        run(burst, 1.f);
        CHECK(burst.offset().x == Catch::Approx(200.f * 200.f / 1400.f).margin(4.f));

        /* Confirmed hits leave the prediction (the snapshots show them); the rest restarts from the snapshot */
        kb_t confirmed;
        confirmed.predict(1, {100.f, 0.f}, standing, apply_hit);
        confirmed.predict(2, {100.f, 0.f}, standing, apply_hit);
        run(confirmed, 0.1f);
        confirmed.confirm([](int ref) { return ref == 1; }, standing, apply_hit);
        CHECK_FALSE(confirmed.empty());
        run(confirmed, 1.f);
        CHECK(confirmed.offset().x == Catch::Approx(100.f * 100.f / 1400.f).margin(3.f));
        confirmed.confirm([](int ref) { return ref == 2; }, standing, apply_hit);
        CHECK(confirmed.empty());

        /* Not confirmed in time: fades out */
        kb_t missed;
        missed.predict(1, {200.f, 0.f}, standing, apply_hit);
        run(missed, 1.5f, 0.3f);
        CHECK(missed.empty());
    }

    SECTION("input history") {
        input_history<int, int, int> h;
        for (u32 seq = 1; seq <= 10; ++seq)
            h.push({seq, int(seq), 0, 1.f / 60.f});
        REQUIRE(h.find(5));
        CHECK(h.find(5)->input == 5);
        CHECK_FALSE(h.find(11));

        h.drop_up_to(7);
        CHECK(h.size() == 3);
        CHECK_FALSE(h.find(7));
        CHECK(h.find(8)->input == 8);
    }
}

/*
 * Real-time loopback game: a server and a client game_state, both on the same
 * level, stepped like the game main loop
 */
struct net_game_fixture {
    /* Real-time code paths on manual game time: a frame takes no wall time */
    game_clock_manual_guard clock_guard;
    u16             port = free_udp_port();
    game_state      gs_srv;
    game_state      gs_cli;
    net::net_server srv{port};
    std::optional<net::net_client> cli;
    net::player_input_state       input;
    std::chrono::milliseconds     frame_period{5}; /* frame rate of both games (render frames, not ticks) */

    net_game_fixture(double latency_min_ms, double latency_max_ms) {
        REQUIRE(gs_srv.level_current("lvl_aes"));
        REQUIRE(gs_cli.level_current("lvl_aes"));
        srv.channel().set_simulated_latency(latency_min_ms, latency_max_ms);

        cli.emplace(sf::IpAddress("127.0.0.1"), port, "me", "wpn_glk17", 0, 0,
                    sf::Color(255, 255, 255), sf::Color(255, 255, 0));
        cli->channel().set_simulated_latency(latency_min_ms, latency_max_ms);
        cli->input_source([this] { return input; });

        /* Join: create the local player like the game does on WELCOME */
        for (int i = 0; i < 400 && !cli->joined(); ++i)
            frame();
        REQUIRE(cli->joined());
        if (!gs_cli.players.contains("me"))
            REQUIRE(gs_cli.player_create("me"));
        gs_cli.players.at("me")->set_server_driven(true);
        gs_cli.crates_networked = true;

        gs_srv.game_run(true);
        gs_cli.game_run(true);
    }

    ~net_game_fixture() {
        cli.reset();
    }

    void frame() {
        game_clock::advance(frame_period);
        gs_cli.game_update();
        cli->update(gs_cli);
        gs_srv.game_update();
        srv.update(gs_srv);
    }

    void run_for(std::chrono::milliseconds duration, auto&& on_frame) {
        auto end = game_clock::now() + duration;
        while (game_clock::now() < end) {
            frame();
            on_frame();
        }
    }

    void run_for(std::chrono::milliseconds duration) {
        run_for(duration, [] {});
    }

    player& local() {
        return *gs_cli.players.at("me");
    }

    player& server_side() {
        return *gs_srv.players.at("me");
    }
};

TEST_CASE("net: client-side prediction under ~500ms ping") {
    using namespace std::chrono_literals;
    net_game_fixture f{80, 170};

    /* Settle on the spawn platform */
    f.run_for(3000ms);
    REQUIRE(f.local().collision_box()->is_lock_y());

    /* Input applies locally immediately: the client moves before the server even got the input */
    auto x0 = f.local().get_position().x;
    f.input  = {.right = true};
    f.run_for(50ms);
    CHECK(f.local().get_position().x > x0 + 0.5f);
    CHECK(f.server_side().get_position().x == Catch::Approx(x0).margin(0.01f));

    /* Hold right, jump, keep running: no visible jumps of the local player */
    f.cli->reset_correction_stats();
    vec2f prev_visual = f.local().get_position() + f.local().render_offset();
    float max_step    = 0.f;
    int   frames      = 0;
    f.run_for(1500ms, [&] {
        if (++frames == 30)
            f.input.jump = true;
        else
            f.input.jump = false;

        auto visual = f.local().get_position() + f.local().render_offset();
        max_step    = std::max(max_step, std::abs(visual.x - prev_visual.x));
        prev_visual = visual;
    });

    /* A 5ms frame can contain at most a couple of 60Hz ticks of movement at max speed */
    CHECK(max_step < 2.f * f.local().get_max_speed() / 60.f + 1.f);
    /*
     * Mispredictions are rare and small: a few ticks of movement when the
     * server input queue ran dry or merged ticks after a burst. They are
     * smoothed out (well below the render offset snap threshold)
     */
    CHECK(f.cli->max_correction() < 64.f);
    CHECK(f.cli->corrections() < 20);

    /* Release and let the server catch up: prediction converges to the server state */
    f.input = {};
    f.run_for(1000ms);
    CHECK(f.local().get_position().x == Catch::Approx(f.server_side().get_position().x).margin(1.f));
    CHECK(f.local().get_position().y == Catch::Approx(f.server_side().get_position().y).margin(1.f));
    UNSCOPED_INFO("corrections: " << f.cli->corrections() << ", max " << f.cli->max_correction() << " px");
}

TEST_CASE("net: remote players are interpolated smoothly") {
    using namespace std::chrono_literals;
    net_game_fixture f{80, 170};

    /* A player that exists on the server only (host player / bot) */
    auto bot = f.gs_srv.player_create("bot");
    REQUIRE(bot != nullptr);
    f.run_for(3000ms);
    REQUIRE(f.gs_cli.players.contains("bot"));
    auto& remote = *f.gs_cli.players.at("bot");

    bot->move_right();
    /* What is drawn this frame */
    auto drawn = [&] { return remote.drawn_position(f.gs_cli.sim.interpolation_factor(), f.gs_cli.sim.last_timestep()).x; };

    float prev_x      = remote.get_position().x;
    float window_x    = prev_x;
    float max_speed   = 0.f;
    float min_step    = 0.f;
    float prev_drawn  = drawn();
    float drawn_min   = 0.f, drawn_max = 0.f;
    auto  window_from = game_clock::now();
    f.run_for(1500ms, [&] {
        auto x   = remote.get_position().x;
        min_step = std::min(min_step, x - prev_x);
        prev_x   = x;

        drawn_min  = std::min(drawn_min, drawn() - prev_drawn);
        drawn_max  = std::max(drawn_max, drawn() - prev_drawn);
        prev_drawn = drawn();

        /* Speed over ~50ms windows: never faster than the max speed (no catch-up jumps) */
        auto now = game_clock::now();
        auto dt  = std::chrono::duration<float>(now - window_from).count();
        if (dt >= 0.05f) {
            max_speed   = std::max(max_speed, (x - window_x) / dt);
            window_x    = x;
            window_from = now;
        }
    });

    CHECK(max_speed < bot->get_max_speed() * 1.3f);
    CHECK(min_step > -0.5f); /* never moves backwards */
    /* ...also as drawn in between the ticks (no extrapolation on top of the frame's snapshot: it saw-toothed) */
    CHECK(drawn_min > -0.5f);
    CHECK(drawn_max < bot->get_max_speed() * 0.005f * 2.f + 0.5f);
    CHECK(remote.get_position().x > 400.f + 100.f);

    /* The remote player is shown in the past, by about the interpolation delay */
    CHECK(remote.get_position().x < bot->get_position().x);
}

TEST_CASE("net: weapon recoil is predicted") {
    using namespace std::chrono_literals;
    net_game_fixture f{80, 170};

    f.server_side().setup_pistol("wpn_ss50"); /* strongest recoil */
    f.server_side().teleport({1300.f, 1040.f}); /* middle of the floor platform */
    f.run_for(3000ms);
    REQUIRE(f.local().pistol_section() == "wpn_ss50");
    REQUIRE(f.local().collision_box()->is_lock_y());

    using clock = game_clock;
    std::optional<clock::time_point> srv_recoil, cli_recoil;

    f.cli->reset_correction_stats();
    f.input = {.shot = true};
    f.run_for(2500ms, [&] {
        if (!srv_recoil && std::abs(f.server_side().get_velocity().x) > 100.f)
            srv_recoil = clock::now();
        if (!cli_recoil && std::abs(f.local().get_velocity().x) > 100.f)
            cli_recoil = clock::now();
    });
    f.input = {};

    /* The recoil moves the local player right away, not a round trip later */
    REQUIRE(srv_recoil);
    REQUIRE(cli_recoil);
    CHECK(*cli_recoil < *srv_recoil);

    /* ...exactly like on the server: no corrections while shooting */
    UNSCOPED_INFO("corrections: " << f.cli->corrections() << ", max " << f.cli->max_correction() << " px");
    CHECK(f.cli->max_correction() < 2.f);

    f.run_for(1500ms);
    CHECK(f.local().get_position().x == Catch::Approx(f.server_side().get_position().x).margin(1.f));
}

TEST_CASE("net: ammo and reloads are predicted and synced") {
    using namespace std::chrono_literals;
    auto wpn = GENERATE(as<std::string>{}, "wpn_glk17", "wpn_1887"); /* magazine and shell by shell reloads */
    /*
     * Steady latency (~400ms RTT): every input tick reaches the server. A
     * jitter burst can overflow its input queue, merging ticks: then the
     * weapon is corrected (the section below)
     */
    net_game_fixture f{100, 100};

    f.server_side().setup_pistol(wpn);
    f.server_side().teleport({1300.f, 1040.f});
    f.run_for(2000ms);
    REQUIRE(f.local().pistol_section() == wpn);

    auto& srv_gun = f.server_side().get_gun();
    auto& cli_gun = f.local().get_gun();

    /* Through several reloads, also pulling the trigger right when they end */
    f.cli->reset_correction_stats();
    bool reloaded = false;
    int  frames   = 0;
    f.run_for(6000ms, [&] {
        f.input.shot = (++frames / 7) % 4 != 0;
        if (cli_gun.ammo_elapsed() == 0)
            reloaded = true;
    });
    f.input = {};

    /* Deterministic: the prediction never needed the server's weapon state */
    CHECK(reloaded);
    CHECK(f.cli->weapon_corrections() == 0);

    /* (the client is ahead by the latency: compare once the reload is over on both sides) */
    auto settle = [&] {
        f.run_for(5000ms);
        CHECK(cli_gun.ammo_elapsed() == srv_gun.ammo_elapsed());
        CHECK(cli_gun.get_sync_state().shot_index == srv_gun.get_sync_state().shot_index);
        CHECK_FALSE(cli_gun.get_sync_state().on_reload);
        CHECK_FALSE(srv_gun.get_sync_state().on_reload);
    };
    settle();

    SECTION("a diverged weapon is corrected") {
        cli_gun.ammo_elapsed(1);
        f.input.shot = true;
        f.run_for(300ms);
        f.input = {};
        settle();
        CHECK(f.cli->weapon_corrections() > 0);
    }
}

TEST_CASE("net: limited ammo is synced and an emptied gun is dropped") {
    using namespace std::chrono_literals;
    auto wpn = GENERATE(as<std::string>{}, "wpn_ak9", "wpn_1887");
    net_game_fixture f{100, 100};

    auto gm            = f.gs_srv.cur_game_mode;
    gm.magazines_count = 2;
    f.gs_srv.game_mode_set(gm);
    f.server_side().setup_pistol(wpn, gm.magazines_count);
    f.server_side().teleport({1300.f, 1040.f});
    f.run_for(2000ms);
    REQUIRE(f.gs_cli.cur_game_mode.magazines_count == 2);
    REQUIRE(f.local().pistol_section() == wpn);
    CHECK(f.local().get_gun().ammo_reserve() == f.server_side().get_gun().ammo_reserve());

    /* (the dropped gun may fly off the level later: watch for it while shooting; the client predicts its throw) */
    size_t srv_dropped = 0;
    bool   cli_confirmed = false;
    f.cli->reset_correction_stats();
    f.input = {.shot = true};
    f.run_for(15000ms, [&] {
        srv_dropped = std::max(srv_dropped, f.gs_srv.dropped_guns.size());
        for (auto& g : f.gs_cli.dropped_guns.guns())
            cli_confirmed = cli_confirmed || (g->predicted() && g->confirmed());
    });
    f.input = {};

    CHECK(f.server_side().pistol_section() == "wpn_glk17");
    CHECK(f.local().pistol_section() == "wpn_glk17");
    CHECK(f.cli->weapon_corrections() == 0);
    CHECK(srv_dropped == 1);
    CHECK(cli_confirmed);
}

TEST_CASE("net: server-only knockback is reconciled on the client") {
    using namespace std::chrono_literals;
    net_game_fixture f{80, 170};

    f.run_for(3000ms);
    REQUIRE(f.local().collision_box()->is_lock_y());
    auto corrections = f.cli->corrections();

    /* An impulse the client could not predict (e.g. a hit on the server) */
    f.server_side().apply_impulse(vec2f{300.f, -500.f} * f.server_side().collision_box()->get_mass());
    f.run_for(1500ms);

    CHECK(f.cli->corrections() > corrections);
    CHECK(f.local().get_position().x == Catch::Approx(f.server_side().get_position().x).margin(1.f));
    CHECK(f.local().get_position().y == Catch::Approx(f.server_side().get_position().y).margin(1.f));
}

TEST_CASE("game_state: lag-compensated bullets") {
    game_state gs;
    auto shooter = gs.player_create("shooter");
    auto target  = gs.player_create("target");
    REQUIRE(shooter);
    REQUIRE(target);
    shooter->group(1);
    target->group(2);

    shooter->position({0.f, 100.f});
    shooter->velocity({0.f, 0.f});

    /* The target moves down along x = 500 at 600 px/s; 30 ticks of position trace */
    auto t = game_clock::now();
    for (int i = 0; i < 30; ++i) {
        target->position({500.f, float(i) * 10.f});
        target->velocity({0.f, 0.f});
        t += std::chrono::microseconds(16667);
        gs.sim.update_immediate(1.f / 60.f, t);
    }
    /* Now at y ~= 290, 0.25 s ago at y ~= 140 (box spans y - 90 .. y) */
    auto now_y = target->get_position().y;
    REQUIRE(now_y > 280.f);

    auto start = vec2f{0.f, 120.f};
    auto end   = vec2f{1000.f, 120.f};

    SECTION("hit-scan tests the players where the shooter saw them") {
        CHECK_FALSE(gs.hitscan(start, end, 0.f, *shooter));

        auto hit = gs.hitscan(start, end, 0.25f, *shooter);
        REQUIRE(hit);
        CHECK(hit->target == target);
        CHECK(hit->position.x == Catch::Approx(500.f).margin(0.01f));
    }

    SECTION("the owner and its group are never hit") {
        target->group(1);
        CHECK_FALSE(gs.hitscan(start, end, 0.25f, *shooter));
        CHECK_FALSE(gs.hitscan({-100.f, 50.f}, {100.f, 50.f}, 0.f, *shooter));
    }

    SECTION("a lag-compensated bullet spawns forwarded by vel * lag") {
        shooter->lag_compensation({0.2f, 0.f});
        gs.spawn_bullet(*shooter, {0.f, -500.f}, {1500.f, 0.f}, 1.f, false);

        REQUIRE(gs.blt_mgr.bullets().size() == 1);
        auto& blt = gs.blt_mgr.bullets().front();
        CHECK(blt.physic()->get_position().x == Catch::Approx(300.f));
        CHECK(blt.net_id() != 0);
        REQUIRE(gs.cur_frame_bullets.size() == 1);
        CHECK(gs.cur_frame_bullets[0].start.x == Catch::Approx(0.f));
        CHECK(gs.cur_frame_bullets[0].position.x == Catch::Approx(300.f));
        CHECK(gs.cur_frame_bullets[0].id == blt.net_id());
    }

    SECTION("the skipped segment is a hit-scan against the rewound players") {
        shooter->lag_compensation({0.5f, 0.25f});
        target->velocity({0.f, 0.f});
        gs.spawn_bullet(*shooter, start, {1500.f, 0.f}, 1.f, false);

        /* Hit 0.25 s in the past: no bullet, the target got the impulse */
        CHECK(gs.blt_mgr.bullets().empty());
        CHECK(target->get_velocity().x > 0.f);
        CHECK(target->pop_on_hit_event());
        REQUIRE(gs.cur_frame_bullets.size() == 1);
    }

    SECTION("no lag: a plain bullet at the muzzle") {
        gs.spawn_bullet(*shooter, start, {1500.f, 0.f}, 1.f, false);
        REQUIRE(gs.blt_mgr.bullets().size() == 1);
        CHECK(gs.blt_mgr.bullets().front().physic()->get_position().x == Catch::Approx(0.f));
    }
}

TEST_CASE("bullets never hit their owner nor the owner's group") {
    game_state gs;
    auto owner = gs.player_create("owner");
    auto mate  = gs.player_create("mate");
    auto enemy = gs.player_create("enemy");
    REQUIRE(owner);
    REQUIRE(mate);
    REQUIRE(enemy);
    for (auto pl : {owner, mate, enemy})
        pl->collision_box()->enable_gravity(false);

    /* All on one line, the bullet starts inside the owner */
    owner->position({0.f, 100.f});
    mate->position({200.f, 100.f});
    enemy->position({400.f, 100.f});

    auto group = GENERATE(-1, 1);
    CAPTURE(group);
    owner->group(group);
    mate->group(group);
    enemy->group(2);

    auto t = game_clock::now();
    auto run = [&] {
        for (int i = 0; i < 30; ++i) {
            t += std::chrono::microseconds(16667);
            gs.sim.update_immediate(1.f / 60.f, t);
        }
    };

    SECTION("server/game spawn") {
        gs.spawn_bullet(*owner, {25.f, 55.f}, {1500.f, 0.f}, 1.f, false);
    }
    SECTION("client spawn of a server bullet") {
        gs.blt_mgr.shot(gs.sim, {25.f, 55.f}, 1.f, {1500.f, 0.f}, sf::Color::White, false, owner->get_group(),
                        player::player_group_getter, player::owner_checker(owner));
    }
    run();

    CHECK_FALSE(owner->pop_on_hit_event());
    if (group != -1)
        CHECK_FALSE(mate->pop_on_hit_event()); /* same group */
    CHECK((group == -1 ? mate->pop_on_hit_event() : enemy->pop_on_hit_event()));
}

TEST_CASE("game_state: retroactive hits rewrite the victim's history") {
    /*
     * Two identical worlds: in one the victim is hit live at tick 30, in the
     * other the same hit arrives at tick 45 for time of tick 30 (lag
     * compensated). Both must end in the same state
     */
    struct world {
        game_state gs;
        player*    victim;
        game_clock::time_point t;

        world(game_clock::time_point t0): t(t0) {
            REQUIRE(gs.level_current("lvl_aes"));
            gs.game_run(true);
            victim = gs.player_create("victim");
            REQUIRE(victim);
            victim->teleport({1200.f, 1040.f});
        }

        void tick(int i) {
            /* The victim runs right, then jumps: its controls must be replayed */
            if (i == 10)
                victim->move_right();
            if (i == 40)
                victim->jump();
            t += std::chrono::microseconds(16667);
            gs.sim.update_immediate(1.f / 60.f, t);
        }
    };

    auto t0 = game_clock::now();
    world live{t0}, retro{t0};
    game_clock::time_point hit_time;

    for (int i = 0; i < 70; ++i) {
        live.tick(i);
        retro.tick(i);
        if (i == 30) {
            hit_time = live.t;
            live.victim->on_bullet_hit({-900.f, -400.f}, live.victim->get_position());
        }
        if (i == 45)
            retro.gs.apply_hit_at(*retro.victim, {-900.f, -400.f}, retro.victim->get_position(), {}, hit_time);
    }

    CHECK(retro.victim->get_position().x == Catch::Approx(live.victim->get_position().x).margin(0.01f));
    CHECK(retro.victim->get_position().y == Catch::Approx(live.victim->get_position().y).margin(0.01f));
    CHECK(retro.victim->get_velocity().x == Catch::Approx(live.victim->get_velocity().x).margin(0.01f));
    CHECK(retro.victim->get_velocity().y == Catch::Approx(live.victim->get_velocity().y).margin(0.01f));

    /* The lag compensation history (position trace) is rewritten as well */
    auto past_live  = live.victim->position_trace_lookup(live.t, 0.2f);
    auto past_retro = retro.victim->position_trace_lookup(retro.t, 0.2f);
    REQUIRE(past_live);
    REQUIRE(past_retro);
    CHECK(past_retro->x == Catch::Approx(past_live->x).margin(0.01f));
}

TEST_CASE("game_state: lag-compensated bullets hit rewound players for their whole flight") {
    /*
     * A target falls (no platforms); the shooter, 0.3 s behind in its view,
     * aims at where it sees the target when the bullet arrives, far beyond
     * the initial hit-scan segment (vel * lag = 180 px)
     */
    game_state gs;
    gs.sim.gravity({0.f, 1200.f});
    auto shooter = gs.player_create("shooter");
    auto target  = gs.player_create("target");
    REQUIRE(shooter);
    REQUIRE(target);
    shooter->group(1);
    target->group(2);
    shooter->collision_box()->enable_gravity(false);
    shooter->position({0.f, -1000.f});
    target->position({600.f, 0.f});
    target->velocity({0.f, 0.f});

    auto t    = game_clock::now();
    auto tick = [&] {
        t += std::chrono::microseconds(16667);
        gs.sim.update_immediate(1.f / 60.f, t);
    };
    for (int i = 0; i < 40; ++i)
        tick();

    /* Bullet arrives in ~14 ticks: the target as seen 0.3 s (18 ticks) earlier is ~4 ticks back in its fall */
    auto fall_y = [](int n) { return float(n * (n - 1)) / 6.f; }; /* y after n ticks of 1200 px/s^2 */
    auto aim_y  = fall_y(40 + 14 - 18) - 45.f;                     /* middle of its body */
    REQUIRE(target->get_position().y > aim_y + 90.f);              /* the present target is already below */

    SECTION("lag compensated: hits where the shooter saw the target") {
        shooter->lag_compensation({0.1f, 0.3f});
        gs.spawn_bullet(*shooter, {0.f, aim_y}, {1800.f, 0.f}, 1.f, false);
        for (int i = 0; i < 30; ++i)
            tick();
        CHECK(target->pop_on_hit_event());
        CHECK(target->get_velocity().x > 0.f);
    }

    SECTION("not compensated: misses the present target") {
        gs.spawn_bullet(*shooter, {0.f, aim_y}, {1800.f, 0.f}, 1.f, false);
        for (int i = 0; i < 30; ++i)
            tick();
        CHECK_FALSE(target->pop_on_hit_event());
    }
}

TEST_CASE("net: clients see server bullets in present time") {
    using namespace std::chrono_literals;
    net_game_fixture f{80, 170};
    f.run_for(3000ms);

    /* A server-side player (like the host's) fires */
    auto host = f.gs_srv.player_create("hostp");
    REQUIRE(host != nullptr);
    host->setup_pistol("wpn_glk17");
    auto ctrl = std::make_shared<player_controller>(5);
    ctrl->player_name = "hostp";
    REQUIRE(f.gs_srv.controller_attach(5, ctrl));
    f.gs_srv.rebuild_controlled_players();
    f.run_for(500ms);

    REQUIRE(host->shot());
    f.run_for(400ms, [&] {
        if (f.gs_cli.blt_mgr.bullets().size() == 1)
            host->relax();
    });

    REQUIRE(f.gs_cli.blt_mgr.bullets().size() >= 1);
    auto id = f.gs_cli.blt_mgr.bullets().front().net_id();
    auto srv_blt = std::ranges::find_if(f.gs_srv.blt_mgr.bullets(), [&](const bullet& b) { return b.net_id() == id; });
    REQUIRE(srv_blt != f.gs_srv.blt_mgr.bullets().end());

    /*
     * Forwarded by the client's one-way lag estimate: the client bullet is
     * where the server bullet is now (within the latency jitter of ~45 ms)
     */
    auto cli_x = f.gs_cli.blt_mgr.bullets().front().physic()->get_position().x;
    auto srv_x = srv_blt->physic()->get_position().x;
    auto speed = srv_blt->physic()->scalar_velocity();
    CHECK(std::abs(cli_x - srv_x) < speed * 0.07f);
}

TEST_CASE("net: own bullets are predicted and confirmed by the server") {
    using namespace std::chrono_literals;
    net_game_fixture f{80, 170};
    f.run_for(3000ms);
    REQUIRE(f.local().pistol_section() == "wpn_glk17");

    /* The shot shows up locally right away, long before the server could answer */
    f.input = {.shot = true};
    bool predicted_early = false;
    f.run_for(60ms, [&] {
        predicted_early = predicted_early || !f.gs_cli.blt_mgr.bullets().empty();
    });
    CHECK(predicted_early);

    f.run_for(600ms);
    f.input = {};
    f.run_for(1500ms);

    /* Every prediction was confirmed (or expired): no pending ones */
    CHECK(f.cli->pending_predicted_bullets() == 0);

    /* Same shot key, same seeded dispersion: the prediction flew the authoritative trajectory */
    CHECK(f.cli->max_bullet_correction() < 2.f);

    /* Exactly the server's bullets exist on the client: no duplicates, no orphan predictions */
    std::vector<u32> srv_ids, cli_ids;
    for (auto& b : f.gs_srv.blt_mgr.bullets())
        srv_ids.push_back(b.net_id());
    for (auto& b : f.gs_cli.blt_mgr.bullets())
        if (!b.physic()->ready_delete_later())
            cli_ids.push_back(b.net_id());
    std::ranges::sort(srv_ids);
    std::ranges::sort(cli_ids);
    REQUIRE(srv_ids.size() > 1);
    CHECK(cli_ids == srv_ids);
}

/*
 * Two players standing on the lvl_aes floor (pl0: y 1050, x 150..2450), "me"
 * (the client, group 0) on the left facing right, a server-side "bot"
 * (group 1, simulated like the host's player) on the right facing left
 */
struct hit_fixture : net_game_fixture {
    player* bot;

    hit_fixture(double latency_min_ms = 80, double latency_max_ms = 170):
        net_game_fixture(latency_min_ms, latency_max_ms) {
        using namespace std::chrono_literals;
        bot = gs_srv.player_create("bot");
        REQUIRE(bot);
        bot->setup_pistol("wpn_glk17");
        bot->group(1);
        auto ctrl         = std::make_shared<player_controller>(5);
        ctrl->player_name = "bot";
        REQUIRE(gs_srv.controller_attach(5, ctrl));
        gs_srv.rebuild_controlled_players();

        server_side().position({700.f, 1040.f});
        server_side().velocity({0.f, 0.f});
        bot->position({1100.f, 1040.f});
        bot->velocity({0.f, 0.f});
        bot->on_left(true);
        run_for(3000ms);
        REQUIRE(gs_cli.players.contains("bot"));
        REQUIRE(local().collision_box()->is_lock_y());
        REQUIRE(gs_cli.players.at("bot")->get_group() == 1);
    }

    player& remote_bot() {
        return *gs_cli.players.at("bot");
    }
};

TEST_CASE("game_state: local key events drive controlled players only") {
    game_state gs;
    gs.game_run(true);
    auto pl = gs.player_create("lol");
    REQUIRE(pl);
    REQUIRE(gs.controller_bind(0, "lol"));
    gs.rebuild_controlled_players();

    sf::Event press;
    press.type     = sf::Event::KeyPressed;
    press.key.code = sf::Keyboard::Key(gs.controllers.at(0)->shot);
    sf::Event release = press;
    release.type      = sf::Event::KeyReleased;

    gs.handle_event(press);
    CHECK(pl->shotting());
    gs.handle_event(release);
    CHECK_FALSE(pl->shotting());

    SECTION("a server-driven player ignores local keys (its input comes from the server)") {
        pl->set_server_driven(true);
        gs.handle_event(press);
        CHECK_FALSE(pl->shotting());
    }

    SECTION("a deleted controller drives nothing") {
        REQUIRE(gs.controller_delete(0));
        CHECK_FALSE(gs.controlled_players.contains("lol"));
        CHECK_FALSE(gs.locally_controlled(*pl));
        gs.handle_event(press);
        CHECK_FALSE(pl->shotting());
    }

    SECTION("the network client's predicted player is controlled from this PC (camera follows it)") {
        REQUIRE(gs.controller_delete(0));
        auto me = gs.player_create("me");
        REQUIRE(me);
        CHECK_FALSE(gs.locally_controlled(*me));
        me->set_server_driven(true);
        me->set_predicted(true);
        CHECK(gs.locally_controlled(*me));
    }
}

TEST_CASE("player: teleport") {
    game_state gs;
    auto pl = gs.player_create("tp");
    REQUIRE(pl);

    auto t = game_clock::now();
    for (int i = 0; i < 10; ++i) {
        t += std::chrono::milliseconds(16);
        gs.sim.update_immediate(1.f / 60.f, t);
    }
    REQUIRE(pl->position_trace_lookup(gs.sim.current_update_time(), 0.05f));

    auto before = pl->teleports();
    pl->velocity({100.f, -50.f});
    pl->add_render_offset({10.f, 0.f});
    pl->teleport({300.f, 400.f});

    CHECK(pl->teleports() == u16(before + 1));
    CHECK(pl->get_position().x == 300.f);
    CHECK(pl->get_position().y == 400.f);
    CHECK(pl->get_velocity().x == 0.f);
    CHECK(pl->render_offset().x == 0.f);
    /* No lag compensation path across the teleport */
    CHECK_FALSE(pl->position_trace_lookup(gs.sim.current_update_time(), 0.05f));

    /* Spawning teleports */
    REQUIRE(gs.level_current("lvl_aes"));
    gs.player_spawn(pl);
    CHECK(pl->teleports() == u16(before + 2));
}

TEST_CASE("net: remote snapshots are not interpolated across a teleport") {
    net::remote_player_snapshot a, b;
    a.ph.pos = {1000.f, 0.f};
    b.ph.pos = {0.f, 0.f};
    CHECK(net::remote_player_snapshot::interpolate(a, b, 0.5f, 0.016f).ph.pos.x == Catch::Approx(500.f));

    b.teleports = 1;
    CHECK(net::remote_player_snapshot::interpolate(a, b, 0.5f, 0.016f).ph.pos.x == Catch::Approx(1000.f));
    CHECK(net::remote_player_snapshot::interpolate(a, b, 1.f, 0.016f).ph.pos.x == Catch::Approx(0.f));
}

TEST_CASE("net: remote snapshots are not interpolated across a retroactive hit") {
    /* b reports a hit: the server rewrote the path (the victim was already flying since before a) */
    net::remote_player_snapshot a, b;
    a.ph.pos = {1000.f, 0.f};
    b.ph.pos = {1400.f, 0.f};
    b.ph.vel = {3000.f, 0.f};
    b.hits   = {7};

    auto mid = net::remote_player_snapshot::interpolate(a, b, 0.5f, 0.016f);
    CHECK(mid.ph.pos.x == Catch::Approx(1400.f - 3000.f * 0.008f)); /* on b's path, not half way from a */
    CHECK(mid.hits == std::vector<u32>{7});                         /* confirms the prediction right away */

    /* Hits known to both: a regular interpolation */
    a.hits = {7};
    CHECK(net::remote_player_snapshot::interpolate(a, b, 0.5f, 0.016f).ph.pos.x == Catch::Approx(1200.f));
}

TEST_CASE("net: teleported players jump on the client") {
    using namespace std::chrono_literals;
    hit_fixture f;

    /* Every frame the client shows a player either at the old or at the new position, nothing in between */
    auto watch = [&](auto&& visual_x, float from, float to) {
        int  between = 0;
        bool arrived = false;
        f.run_for(1500ms, [&] {
            auto x = visual_x();
            if (std::abs(x - from) > 10.f && std::abs(x - to) > 10.f)
                ++between;
            arrived = arrived || std::abs(x - to) < 1.f;
        });
        CHECK(between == 0);
        CHECK(arrived);
    };

    SECTION("remote player") {
        f.bot->teleport({300.f, 1040.f});
        watch([&] { return f.remote_bot().get_position().x; }, 1100.f, 300.f);
    }

    SECTION("local player") {
        /* A short teleport: a regular prediction correction of this size would be smoothed */
        f.server_side().teleport({800.f, 1040.f});
        watch([&] { return f.local().get_position().x + f.local().render_offset().x; }, 700.f, 800.f);
        CHECK(f.local().render_offset().x == 0.f);
    }
}

TEST_CASE("net: bullet hits on the local player are predicted") {
    using namespace std::chrono_literals;
    hit_fixture f;

    using clock = game_clock;
    std::optional<clock::time_point> srv_hit, cli_hit;

    REQUIRE(f.bot->shot());
    f.run_for(1500ms, [&] {
        if (!srv_hit && f.server_side().get_velocity().x < -50.f)
            srv_hit = clock::now();
        if (!cli_hit && f.local().get_velocity().x < -50.f)
            cli_hit = clock::now();
        if (srv_hit && cli_hit)
            f.bot->relax();
    });
    f.bot->relax();

    REQUIRE(srv_hit);
    REQUIRE(cli_hit);
    /*
     * Without prediction the knockback shows up one way trip (160..340 ms)
     * after the server hit; predicted, it differs by the error of the
     * client's estimate of the bullet's age (0..~100 ms with this jitter)
     */
    CHECK(*cli_hit - *srv_hit < 150ms);

    /* Settled: confirmed hits were not applied twice */
    f.run_for(2500ms);
    CHECK(f.local().get_position().x == Catch::Approx(f.server_side().get_position().x).margin(1.f));
}

TEST_CASE("net: own bullet hits on remote players are predicted") {
    using namespace std::chrono_literals;
    hit_fixture f;

    using clock = game_clock;
    std::optional<clock::time_point> srv_hit, cli_hit;
    auto srv_x0 = f.bot->get_position().x;
    auto cli_x0 = f.remote_bot().get_position().x;

    f.input = {.shot = true};
    f.run_for(1500ms, [&] {
        if (!srv_hit && f.bot->get_position().x > srv_x0 + 2.f)
            srv_hit = clock::now();
        if (!cli_hit && f.remote_bot().get_position().x > cli_x0 + 2.f)
            cli_hit = clock::now();
        if (srv_hit && cli_hit)
            f.input = {};
    });
    f.input = {};

    REQUIRE(srv_hit);
    REQUIRE(cli_hit);
    /* The client shoots one way trip before the server applies the shot: it sees its hit first */
    CHECK(*cli_hit < *srv_hit);

    /* Settled: the predicted knockback handed over to the server's, no double push */
    f.run_for(2500ms);
    CHECK(f.remote_bot().get_position().x == Catch::Approx(f.bot->get_position().x).margin(1.f));
    CHECK(f.remote_bot().get_position().y == Catch::Approx(f.bot->get_position().y).margin(1.f));
}

TEST_CASE("net: predicted knockback follows the level geometry") {
    using namespace std::chrono_literals;
    hit_fixture f;

    /* "me" shoots the strongest weapon at the bot standing at the right edge of the floor (pl0: x 150..2450, y 1050) */
    f.server_side().setup_pistol("wpn_ss50");
    f.server_side().teleport({2100.f, 1040.f});
    f.bot->teleport({2380.f, 1040.f});
    f.run_for(2000ms);
    REQUIRE(f.local().pistol_section() == "wpn_ss50");
    REQUIRE(f.remote_bot().get_position().x == Catch::Approx(2380.f).margin(1.f));

    bool               server_off_edge = false, client_off_edge = false;
    int                inside_floor    = 0;
    int                detached_legs   = 0;
    int                frames          = 0;
    std::vector<vec2f> cli_path;
    /*
     * The server's path: its per tick history, which the retroactive hit
     * rewrites (the live position jumps over the rewritten part at once)
     */
    std::map<game_clock::time_point, vec2f> srv_arc;
    auto teleports = f.bot->teleports();
    f.input = {.shot = true};
    f.run_for(1500ms, [&] {
        if (++frames == 20)
            f.input = {}; /* a single shot (ss50 fires every 0.86 s) */

        auto srv = f.bot->get_position();
        auto pos = f.remote_bot().get_position();
        auto sz  = f.remote_bot().get_size();
        server_off_edge = server_off_edge || srv.x > 2450.f;
        client_off_edge = client_off_edge || pos.x > 2450.f;
        cli_path.push_back(pos);
        for (auto& rec : f.gs_srv.history.at("bot"))
            if (rec.end && rec.teleports == teleports)
                srv_arc[rec.time] = rec.end->pos;

        /* The drawn bot can leave the floor past its edge, never through it */
        if (pos.x + sz.x > 150.f && pos.x < 2450.f && pos.y > 1050.f + 2.f)
            ++inside_floor;

        /* Its legs stay under it */
        auto [l, r] = f.remote_bot().leg_positions();
        for (auto leg : {l, r})
            if (std::abs(leg.x - (pos.x + sz.x * 0.5f)) > sz.x * 0.5f || std::abs(leg.y - pos.y) > 20.f)
                ++detached_legs;
    });

    REQUIRE(server_off_edge);
    CHECK(client_off_edge);
    CHECK(inside_floor == 0);
    CHECK(detached_legs == 0);

    /*
     * The predicted knockback is ahead in time (the client sees its hit before
     * the server gets the shot), but it follows the server's trajectory: slides
     * off the edge and falls, no floating, no sideways shift
     */
    std::vector<vec2f> srv_path;
    for (auto& [_, pos] : srv_arc)
        srv_path.push_back(pos);
    REQUIRE(srv_path.size() > 30);

    float max_path_error = 0.f;
    for (auto& p : cli_path) {
        if (p.y > 1800.f)
            continue; /* about to die and respawn */
        /* Distance to the server's path (a polyline of its tick positions) */
        float d = std::numeric_limits<float>::max();
        for (size_t i = 1; i < srv_path.size(); ++i) {
            auto a = srv_path[i - 1], ab = srv_path[i] - a;
            auto len2 = ab.x * ab.x + ab.y * ab.y;
            auto k    = len2 > 0.f ? std::clamp(((p - a).x * ab.x + (p - a).y * ab.y) / len2, 0.f, 1.f) : 0.f;
            d         = std::min(d, magnitude(p - (a + ab * k)));
        }
        max_path_error = std::max(max_path_error, d);
    }
    UNSCOPED_INFO("max path error " << max_path_error);
    CHECK(max_path_error < 5.f);
}

/*
 * Distance from p to the server path (a polyline, its position changes once
 * per tick); the path is split at teleports (respawn)
 */
static float distance_to_path(const vec2f& p, const std::vector<std::pair<vec2f, u16>>& path) {
    float d = std::numeric_limits<float>::max();
    for (size_t i = 0; i < path.size(); ++i) {
        auto a = path[i].first;
        if (i == 0 || path[i - 1].second != path[i].second) {
            d = std::min(d, magnitude(p - a));
            continue;
        }
        auto from = path[i - 1].first, ab = a - from;
        auto len2 = ab.x * ab.x + ab.y * ab.y;
        auto k    = len2 > 0.f ? std::clamp(((p - from).x * ab.x + (p - from).y * ab.y) / len2, 0.f, 1.f) : 0.f;
        d         = std::min(d, magnitude(p - (from + ab * k)));
    }
    return d;
}

TEST_CASE("net: remote knockback under sustained fire stays on the server's path (100 ms ping)") {
    using namespace std::chrono_literals;
    hit_fixture f{20, 30};

    auto weapon   = GENERATE(as<std::string>{}, "wpn_ak9", "wpn_sw500", "wpn_ss50");
    auto moving   = GENERATE(false, true);
    auto frame_ms = GENERATE(4, 10, 16); /* frames shorter and as long as a tick */
    CAPTURE(weapon, moving, frame_ms);

    f.server_side().setup_pistol(weapon);
    f.server_side().teleport({1100.f, 1040.f});
    f.bot->teleport({1500.f, 1040.f});
    f.run_for(1500ms);
    REQUIRE(f.local().pistol_section() == weapon);

    std::vector<std::pair<vec2f, u16>> srv_path;
    std::vector<vec2f>                 cli_path;
    size_t                             server_hits = 0;
    auto                               start       = game_clock::now();
    f.frame_period = std::chrono::milliseconds(frame_ms);
    f.input        = {.shot = true};
    f.run_for(3000ms, [&] {
        auto elapsed = game_clock::now() - start;
        if (elapsed >= 1500ms)
            f.input = {};
        if (moving)
            elapsed % 600ms < 300ms ? f.bot->move_left() : f.bot->move_right();
        srv_path.push_back({f.bot->get_position(), f.bot->teleports()});
        /* What is drawn: the position with the smoothing offset */
        cli_path.push_back(f.remote_bot().get_position() + f.remote_bot().render_offset());
        server_hits = std::max(server_hits, f.bot->recent_hit_ids().size());
    });
    REQUIRE(server_hits > 0);

    /*
     * The client shows the hits right away, the server applies them where the
     * shooter saw them: the drawn motion stays on the server's path
     */
    float max_error = 0.f;
    for (auto& p : cli_path)
        if (p.y < 1800.f)
            max_error = std::max(max_error, distance_to_path(p, srv_path));
    UNSCOPED_INFO("max path error " << max_error);
    CHECK(max_error < 40.f);

    /* A standing target is only pushed along the shots: it never moves back against them */
    if (!moving) {
        float max_x = cli_path.front().x, max_back = 0.f;
        for (size_t i = 1; i < cli_path.size(); ++i) {
            auto& p = cli_path[i];
            /* until it falls out of the level (respawn) */
            if (p.y > 1800.f || magnitude(p - cli_path[i - 1]) > 200.f)
                break;
            max_x    = std::max(max_x, p.x);
            max_back = std::max(max_back, max_x - p.x);
        }
        UNSCOPED_INFO("max move against the hits " << max_back);
        CHECK(max_back < 5.f);
    }
}

TEST_CASE("net: traffic in a fight") {
    using namespace std::chrono_literals;
    hit_fixture f{20, 30};
    f.frame_period = 4ms; /* more frames than ticks */
    f.server_side().setup_pistol("wpn_1887");
    f.run_for(1000ms);

    auto& srv_ch    = f.srv.channel();
    auto& cli_ch    = f.cli->channel();
    auto  srv_sent  = srv_ch.datagrams_sent();
    auto  srv_bytes = srv_ch.bytes_sent();
    auto  cli_sent  = cli_ch.datagrams_sent();
    auto  cli_bytes = cli_ch.bytes_sent();
    auto  ticks     = f.gs_srv.sim.tick();

    /* Both shoot (the shotgun: several pellets per shot) */
    REQUIRE(f.bot->shot());
    f.input = {.shot = true};
    f.run_for(2000ms);
    f.bot->relax();
    f.input = {};

    ticks          = f.gs_srv.sim.tick() - ticks;
    auto states    = srv_ch.datagrams_sent() - srv_sent;
    auto inputs    = cli_ch.datagrams_sent() - cli_sent;
    auto state_avg = double(srv_ch.bytes_sent() - srv_bytes) / double(states);
    auto input_avg = double(cli_ch.bytes_sent() - cli_bytes) / double(inputs);
    UNSCOPED_INFO(states << " states, " << state_avg << " bytes avg; " << inputs << " inputs, " << input_avg
                         << " bytes avg; " << ticks << " ticks");

    /* One state per tick (one client), not per frame */
    CHECK(states <= ticks);
    CHECK(states >= ticks * 9 / 10);
    CHECK(state_avg < 200.0);
    /* Inputs carry the unacknowledged ticks only (~100 ms ping: a handful) */
    CHECK(inputs <= ticks);
    CHECK(input_avg < 40.0);
}

TEST_CASE("net: a hard hit on a remote player flies smoothly") {
    using namespace std::chrono_literals;
    auto latency = GENERATE(std::pair{1.0, 2.0}, std::pair{20.0, 30.0}, std::pair{80.0, 170.0});
    CAPTURE(latency.first, latency.second);
    hit_fixture f{latency.first, latency.second};
    f.server_side().setup_pistol("wpn_ss50"); /* the hardest knockback */
    f.run_for(1500ms);
    REQUIRE(f.local().pistol_section() == "wpn_ss50");

    /* One shot at the standing bot */
    /* What is drawn this frame */
    auto drawn = [&] {
        return f.remote_bot().drawn_position(f.gs_cli.sim.interpolation_factor(), f.gs_cli.sim.last_timestep()).x;
    };
    auto x0    = drawn();
    f.input    = {.shot = true};
    std::vector<float> xs;
    int frames = 0;
    f.run_for(1500ms, [&] {
        if (++frames == 3)
            f.input = {};
        if (drawn() > x0 + 2.f && xs.size() < 40) /* 200 ms of flight from the predicted hit */
            xs.push_back(drawn());
    });
    REQUIRE(xs.size() == 40);

    /*
     * ~3500 px/s: ~17 px per 5 ms frame. Neither stopping in mid-air until the
     * server confirms the hit (the prediction used to be limited), nor
     * crossing the rest of the knockback at once when it does
     */
    float min_step = 1e9f, max_step = 0.f;
    for (size_t i = 1; i < xs.size(); ++i) {
        min_step = std::min(min_step, xs[i] - xs[i - 1]);
        max_step = std::max(max_step, xs[i] - xs[i - 1]);
    }
    UNSCOPED_INFO("steps " << min_step << " .. " << max_step << " px");
    CHECK(min_step > 10.f);
    CHECK(max_step < 28.f);
}

TEST_CASE("net: predicted hits the server did not register fade out") {
    using namespace std::chrono_literals;
    hit_fixture f;

    auto x0 = f.remote_bot().get_position().x;
    f.remote_bot().on_bullet_hit({1500.f, 0.f}, f.remote_bot().get_position(), bullet_ref{999999, {}});
    f.run_for(150ms);
    CHECK(f.remote_bot().get_position().x > x0 + 5.f);

    f.run_for(2500ms);
    CHECK(f.remote_bot().get_position().x == Catch::Approx(f.bot->get_position().x).margin(0.5f));
}

TEST_CASE("weapon: seeded shots are deterministic") {
    game_clock_manual_guard clock_guard; /* the waits advance the game time by hand */
    game_state gs;
    auto a = gs.player_create("a");
    auto b = gs.player_create("b");
    REQUIRE(a);
    REQUIRE(b);
    a->setup_pistol("wpn_1887"); /* buckshot: several pellets per shot */
    b->setup_pistol("wpn_1887");
    gs.game_run(true);

    auto fire = [&](player& pl) {
        pl.last_input_seq(42);
        pl.lag_compensation({});
        REQUIRE(pl.shot());
        auto ctrl = std::make_shared<player_controller>(7);
        ctrl->player_name = pl.name();
        gs.controller_attach(7, ctrl);
        gs.rebuild_controlled_players();
        for (int i = 0; i < 200 && gs.cur_frame_bullets.empty(); ++i) {
            game_clock::advance(std::chrono::milliseconds(5));
            gs.game_update();
        }
        pl.relax();
        gs.controller_delete(7);
        gs.rebuild_controlled_players();
        REQUIRE(!gs.cur_frame_bullets.empty());
        return gs.cur_frame_bullets;
    };

    auto shots_a = fire(*a);
    auto shots_b = fire(*b);
    REQUIRE(shots_a.size() == shots_b.size());
    REQUIRE(shots_a.size() > 1);
    for (size_t i = 0; i < shots_a.size(); ++i) {
        CHECK(shots_a[i].key == shots_b[i].key);
        CHECK(shots_a[i].key.pellet == i);
        CHECK(normalize(shots_a[i].velocity).x == Catch::Approx(normalize(shots_b[i].velocity).x));
        CHECK(normalize(shots_a[i].velocity).y == Catch::Approx(normalize(shots_b[i].velocity).y));
    }
}

TEST_CASE("net: unconfirmed bullet predictions expire") {
    using namespace std::chrono_literals;
    net_game_fixture f{80, 170};
    f.run_for(3000ms);

    /* A predicted bullet the server never fired */
    f.gs_cli.spawn_bullet(f.local(), f.local().get_position(), {0.f, -1500.f}, 1.f, false);
    f.cli->update(f.gs_cli);
    REQUIRE(f.cli->pending_predicted_bullets() == 1);
    REQUIRE(f.gs_cli.blt_mgr.bullets().size() == 1);
    CHECK(f.gs_cli.blt_mgr.bullets().front().net_id() == 0);

    f.run_for(2000ms); /* timeout: max(0.5 s, 2 * rtt + 0.25 s) */
    CHECK(f.cli->pending_predicted_bullets() == 0);
    CHECK(f.gs_cli.blt_mgr.bullets().empty());
}

TEST_CASE("udp_channel simulates latency") {
    using namespace net;
    game_clock_manual_guard clock_guard; /* the waits advance the game time by hand */

    udp_channel srv;
    REQUIRE(srv.bind(0));
    auto port = srv.local_port();
    srv.set_simulated_latency(80, 170);

    udp_channel cli;
    REQUIRE(cli.bind(0));
    cli.set_simulated_latency(80, 170);

    const std::vector<u8> data = {1, 2, 3, 4};
    auto t0 = game_clock::now();

    REQUIRE(cli.send_to(sf::IpAddress("127.0.0.1"), port, data));

    /*
     * The datagram must not arrive instantly: each one-way hop is delayed
     * by >= 80ms, so the earliest delivery is ~160ms after the send.
     * Both channels are pumped every frame, like in the game
     */
    bool arrived = false;
    while (game_clock::now() - t0 < std::chrono::milliseconds(40)) {
        game_clock::advance(std::chrono::milliseconds(5));
        cli.recv(std::chrono::milliseconds(5));
        if (!srv.recv(std::chrono::milliseconds(5)).empty()) {
            arrived = true;
            break;
        }
    }
    REQUIRE(!arrived);

    /* ...and it must arrive within the worst case (170ms + 170ms) plus margin */
    while (game_clock::now() - t0 < std::chrono::milliseconds(600)) {
        game_clock::advance(std::chrono::milliseconds(5));
        cli.recv(std::chrono::milliseconds(10));
        if (!srv.recv(std::chrono::milliseconds(10)).empty()) {
            arrived = true;
            break;
        }
    }
    REQUIRE(arrived);
    CHECK(game_clock::now() - t0 >= std::chrono::milliseconds(160));
}

TEST_CASE("net: the hands of standing players keep still on the client") {
    using namespace std::chrono_literals;
    auto frame_ms = GENERATE(4, 7, 16);
    CAPTURE(frame_ms);
    net_game_fixture f{2, 3};
    f.frame_period = std::chrono::milliseconds(frame_ms);

    auto bot = f.gs_srv.player_create("bot");
    REQUIRE(bot);
    bot->teleport({1600.f, 1040.f});
    f.server_side().teleport({1300.f, 1040.f});
    f.run_for(2000ms);
    REQUIRE(f.gs_cli.players.contains("bot"));
    auto& remote = *f.gs_cli.players.at("bot");
    REQUIRE(remote.collision_box()->is_lock_y());
    REQUIRE(f.local().collision_box()->is_lock_y());

    /* The weapon arm joint relative to the body: moved only by the weapon bobbing */
    auto arm = [](const player& pl) { return pl.arm_position(pl.get_position()) - pl.get_position(); };
    auto remote_arm0 = arm(remote), local_arm0 = arm(f.local());
    f.cli->reset_correction_stats();
    float remote_jiggle = 0.f, local_jiggle = 0.f;
    f.run_for(1000ms, [&] {
        remote_jiggle = std::max(remote_jiggle, magnitude(arm(remote) - remote_arm0));
        local_jiggle  = std::max(local_jiggle, magnitude(arm(f.local()) - local_arm0));
    });
    CHECK(remote_jiggle < 0.01f);
    CHECK(local_jiggle < 0.01f);
    /* Standing still is predicted exactly (the hidden vertical velocity included) */
    CHECK(f.cli->corrections() == 0);
}

TEST_CASE("game_state: crates drop by the game mode") {
    game_clock_manual_guard clock_guard;
    game_state              gs;
    REQUIRE(gs.level_current("lvl_aes"));
    gs.game_run(true);

    auto run_for = [&](std::chrono::milliseconds duration) {
        for (auto end = game_clock::now() + duration; game_clock::now() < end;) {
            game_clock::advance(std::chrono::milliseconds(10));
            gs.game_update();
        }
    };

    /* The default game mode never drops crates */
    run_for(std::chrono::seconds(6));
    CHECK(gs.crates.empty());

    REQUIRE(gs.game_mode_apply("crates"));
    CHECK(gs.cur_game_mode.crate_drop_period == 5.f);
    run_for(std::chrono::seconds(4));
    CHECK(gs.crates.empty());
    run_for(std::chrono::seconds(2));
    CHECK(gs.crates.size() == 1);
}

TEST_CASE("game_commands: game mode") {
    game_state    gs;
    game_commands cmds{gs};

    auto run = [](std::string cmd) {
        command_buffer().push(std::move(cmd));
        command_buffer().run_handlers();
    };

    run("game mode list");
    run("game mode");
    CHECK(gs.cur_game_mode.section == "gm_default");

    run("game mode ballistic");
    CHECK(gs.cur_game_mode.section == "gm_ballistic");
    CHECK(gs.gravity_for_bullets);

    run("game mode default");
    CHECK(gs.cur_game_mode.section == "gm_default");
    CHECK_FALSE(gs.gravity_for_bullets);

    /* The network client gets the game mode from the server only */
    gs.game_mode_networked = true;
    run("game mode gm_crates");
    CHECK(gs.cur_game_mode.section == "gm_default");
}

TEST_CASE("net: gun throws are shown on the client") {
    using namespace std::chrono_literals;
    net_game_fixture f{40, 60};

    auto bot = f.gs_srv.player_create("bot");
    REQUIRE(bot != nullptr);
    /* Primary guns: the secondary ones are never dropped */
    bot->setup_pistol("wpn_ak9");
    f.server_side().setup_pistol("wpn_ak9");
    f.run_for(1000ms);
    REQUIRE(f.gs_cli.players.contains("bot"));
    REQUIRE(f.local().pistol_section() == "wpn_ak9");

    SECTION("a remote player throws its gun") {
        auto& remote = *f.gs_cli.players.at("bot");
        REQUIRE(remote.pistol_section() == "wpn_ak9");
        REQUIRE(bot->drop_gun());

        /* The throw is shown with the gun in the hands, the secondary gun replaces it at the release */
        bool   seen_throwing = false, gun_while_throwing = true;
        size_t max_guns      = 0; /* the thrown gun may fly out of the level */
        f.run_for(1500ms, [&] {
            max_guns = std::max(max_guns, f.gs_srv.dropped_guns.size());
            if (remote.throwing()) {
                seen_throwing = true;
                gun_while_throwing = gun_while_throwing && remote.pistol_section() == "wpn_ak9";
            }
        });
        CHECK(seen_throwing);
        CHECK(gun_while_throwing);
        CHECK(bot->pistol_section() == "wpn_glk17");
        CHECK(remote.pistol_section() == "wpn_glk17");
        CHECK(max_guns == 1);
    }

    SECTION("the local player throws its gun") {
        f.input = {.drop = true};
        for (int i = 0; i < 20 && !f.local().throwing(); ++i)
            f.frame();
        f.input = {};
        REQUIRE(f.local().throwing());

        /* Predicted throw: the gun stays in the hands until the server drops it, then never comes back */
        bool   gun_back = false;
        bool   released = false;
        size_t max_guns = 0;
        f.run_for(1500ms, [&] {
            max_guns = std::max(max_guns, f.gs_srv.dropped_guns.size());
            released = released || f.local().pistol_section() == "wpn_glk17";
            gun_back = gun_back || (released && f.local().pistol_section() == "wpn_ak9");
        });
        CHECK(f.server_side().pistol_section() == "wpn_glk17");
        CHECK(f.local().pistol_section() == "wpn_glk17");
        CHECK_FALSE(gun_back);
        CHECK(max_guns == 1);
    }

    SECTION("the local player's thrown gun is predicted") {
        auto predicted_gun = [&]() -> dropped_gun* {
            for (auto& g : f.gs_cli.dropped_guns.guns())
                if (g->predicted())
                    return g.get();
            return nullptr;
        };

        f.input = {.drop = true};
        for (int i = 0; i < 20 && !f.local().throwing(); ++i)
            f.frame();
        f.input = {};
        REQUIRE(f.local().throwing());

        /* The predicted gun flies right after the local release, before the server drops its gun */
        for (int i = 0; i < 200 && !predicted_gun(); ++i)
            f.frame();
        REQUIRE(predicted_gun());
        CHECK(f.gs_srv.dropped_guns.size() == 0);
        CHECK_FALSE(predicted_gun()->confirmed());
        CHECK(f.gs_cli.dropped_guns.size() == 1);

        /* The server's gun takes the prediction over: no second gun on the client */
        size_t max_cli_guns = 0;
        bool   confirmed    = false;
        f.run_for(1000ms, [&] {
            max_cli_guns = std::max(max_cli_guns, f.gs_cli.dropped_guns.size());
            if (auto* g = predicted_gun(); g && g->confirmed()) {
                confirmed = true;
                CHECK(g->id() == f.gs_srv.dropped_guns.guns().front()->id());
            }
        });
        CHECK(confirmed);
        CHECK(max_cli_guns == 1);
        UNSCOPED_INFO("max gun correction: " << f.cli->max_gun_correction() << " px");
        CHECK(f.cli->max_gun_correction() < 10.f);
    }

    SECTION("a predicted gun hits a remote player") {
        /* The bot stands on the long platform right in front of the thrower (another group: guns skip the own one) */
        bot->group(1);
        f.server_side().teleport({1000.f, 900.f});
        bot->teleport({1090.f, 900.f});
        f.run_for(1000ms);
        auto& remote = *f.gs_cli.players.at("bot");
        REQUIRE(remote.get_group() == 1);
        REQUIRE_FALSE(f.local().get_on_left());

        auto predicted_gun = [&]() -> dropped_gun* {
            for (auto& g : f.gs_cli.dropped_guns.guns())
                if (g->predicted())
                    return g.get();
            return nullptr;
        };

        f.input = {.drop = true};
        for (int i = 0; i < 20 && !f.local().throwing(); ++i)
            f.frame();
        f.input = {};
        for (int i = 0; i < 200 && !predicted_gun(); ++i)
            f.frame();
        REQUIRE(predicted_gun());
        REQUIRE(predicted_gun()->get_velocity().x > 0.f);
        auto bot_x = remote.get_position().x;

        /* The gun bounces off the bot where it is shown, the bot is knocked back right away */
        bool bounced = false, server_hit_first = false;
        for (int i = 0; i < 60 && !bounced; ++i) {
            f.frame();
            bounced          = predicted_gun() && predicted_gun()->get_velocity().x < 0.f;
            server_hit_first = server_hit_first || !bot->recent_hit_ids().empty();
        }
        REQUIRE(bounced);
        CHECK_FALSE(server_hit_first);
        CHECK(remote.get_velocity().x > 0.f);

        /* The server registers the hit where the thrower saw it, and reports it */
        bool reported = false;
        f.run_for(1000ms, [&] {
            for (auto id : bot->recent_hit_ids())
                reported = reported || (id & dropped_gun::hit_ref_tag) != 0;
        });
        CHECK(reported);
        CHECK(bot->get_position().x > bot_x + 5.f);
        CHECK(remote.get_position().x > bot_x + 5.f);
        UNSCOPED_INFO("max gun correction: " << f.cli->max_gun_correction() << " px");
        CHECK(f.cli->max_gun_correction() < 20.f);
    }
}

TEST_CASE("game_state: score of kills and suicides") {
    game_clock_manual_guard clock_guard;
    game_state              gs;
    REQUIRE(gs.level_current("lvl_aes"));
    gs.game_run(true);
    auto a = gs.player_create("a");
    auto b = gs.player_create("b");
    auto c = gs.player_create("c");
    REQUIRE((a && b && c));

    auto tick = [&] {
        game_clock::advance(std::chrono::microseconds(16667));
        gs.sim.update_immediate(1.f / 60.f, game_clock::now());
    };
    auto hit_by = [](player* victim, player* attacker, const vec2f& impulse) {
        victim->on_bullet_hit(impulse, victim->get_position(), bullet_ref{1, {}, attacker->name()});
    };

    SECTION("a fall without an attacker is a suicide") {
        gs.player_on_dead(a);
        CHECK(a->score() == -1);
        CHECK(b->score() == 0);
    }

    SECTION("a fall after a hit is a kill: the last hit wins") {
        hit_by(b, a, {-900.f, -400.f});
        CHECK(b->attacker() == a->name());
        hit_by(b, c, {-900.f, -400.f});
        CHECK(b->attacker() == c->name());

        gs.player_on_dead(b);
        CHECK(c->score() == 1);
        CHECK(a->score() == 0);
        CHECK(b->score() == 0);
        CHECK(b->attacker().empty()); /* the respawn forgets it */
    }

    SECTION("own hits are not attacks") {
        hit_by(a, a, {-900.f, 0.f});
        CHECK(a->attacker().empty());
    }

    SECTION("heavier hits take longer to compensate") {
        CHECK(a->hit_cooldown({0.f, -500.f}) == 0.f);
        CHECK(a->hit_cooldown({300.f, 0.f}) < a->hit_cooldown({3000.f, 0.f}));
        CHECK(a->hit_cooldown({-3000.f, 0.f}) == Catch::Approx(a->hit_cooldown({3000.f, 0.f})));
    }

    SECTION("the attacker is forgotten on the ground after the hit cooldown") {
        b->teleport({1200.f, 1040.f});
        for (int i = 0; i < 60 && !b->collision_box()->is_lock_y(); ++i)
            tick();
        REQUIRE(b->collision_box()->is_lock_y());

        hit_by(b, a, {-300.f, 0.f}); /* knockback along the ground */
        auto cooldown = b->hit_cooldown({-300.f, 0.f});
        REQUIRE(cooldown > 0.05f);

        tick();
        REQUIRE(b->collision_box()->is_lock_y());
        CHECK(b->attacker() == a->name());

        auto ticks = int(cooldown * 60.f) + 2;
        for (int i = 0; i < ticks; ++i)
            tick();
        REQUIRE(b->collision_box()->is_lock_y());
        CHECK(b->attacker().empty());

        gs.player_on_dead(b);
        CHECK(b->score() == -1);
        CHECK(a->score() == 0);
    }
}
