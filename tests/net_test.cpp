#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "net/net_protocol.hpp"
#include "net/net_udp.hpp"
#include "net/net_server.hpp"
#include "net/net_client.hpp"
#include "net_player_controller.hpp"
#include "game_state.hpp"
#include "script/lua_init.hpp" /* luacpp pushers for dfdh types */

using namespace dfdh;

/* One simulation tick (inputs are applied by the network layer on ticks) */
static void sim_tick(game_state& gs) {
    gs.sim.update_immediate(1.f / 60.f, std::chrono::steady_clock::now());
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
        st.players.push_back(state_player{.name = "p1", .pos_x = 10.f, .pos_y = 20.f,
                                          .vel_x = -3.f, .vel_y = 4.f, .jumps_left = 1, .lock_y = 1,
                                          .accel_lf = 0.5f, .on_left = 1, .shooting = 1});
        st.players.push_back(state_player{.name = "p2", .pistol = "wpn_m40", .face_id = 3});
        st.bullets.push_back(bullet_info{.owner = "p1", .pos_x = 11.f, .pos_y = 21.f,
                                         .vel_x = 900.f, .vel_y = -10.f, .hit_mass = 2.f,
                                         .tracer_color = 0xFF00FF00});

        auto data = encode_msg(msg_type::state, st);
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::state);

        auto r = read_msg<state>(msg.payload);
        CHECK(r.tick == 7);
        CHECK(r.server_time_ms == 5000);
        CHECK(r.ack_seq == 77);
        CHECK(r.echo_client_time_ms == 900);
        CHECK(r.echo_hold_ms == 16);
        REQUIRE(r.players.size() == 2);
        CHECK(r.players[0].lock_y == 1);
        CHECK(r.players[0].accel_lf == Catch::Approx(0.5f));
        CHECK(r.players[0].accel_rf == Catch::Approx(1.f));
        CHECK(std::string(r.players[0].name) == "p1");
        CHECK(r.players[0].pos_x == Catch::Approx(10.f));
        CHECK(r.players[0].pos_y == Catch::Approx(20.f));
        CHECK(r.players[0].vel_x == Catch::Approx(-3.f));
        CHECK(r.players[0].vel_y == Catch::Approx(4.f));
        CHECK(r.players[0].jumps_left == 1);
        CHECK(r.players[0].on_left == 1);
        CHECK(r.players[0].shooting == 1);
        CHECK(std::string(r.players[1].name) == "p2");
        CHECK(r.players[1].pistol == "wpn_m40");
        CHECK(r.players[1].face_id == 3);
        REQUIRE(r.bullets.size() == 1);
        CHECK(r.bullets[0].owner == "p1");
        CHECK(r.bullets[0].pos_x == Catch::Approx(11.f));
        CHECK(r.bullets[0].pos_y == Catch::Approx(21.f));
        CHECK(r.bullets[0].vel_x == Catch::Approx(900.f));
        CHECK(r.bullets[0].vel_y == Catch::Approx(-10.f));
        CHECK(r.bullets[0].hit_mass == Catch::Approx(2.f));
        CHECK(r.bullets[0].tracer_color == 0xFF00FF00);
    }

    SECTION("state with bullet ids") {
        state st;
        st.tick = 5;
        st.bullets.push_back(bullet_info{.owner = "p1", .id = 1, .start_x = -1.f, .start_y = 2.f,
                                         .pos_x = 1.f, .pos_y = 2.f, .vel_x = 100.f, .vel_y = 0.f,
                                         .hit_mass = 1.f, .tracer_color = 0x11223344});
        st.alive_bullet_ids.push_back(1);
        st.alive_bullet_ids.push_back(2);

        auto data = encode_msg(msg_type::state, st);
        auto msg  = decode_msg({data.data(), data.size()});
        auto r    = read_msg<state>(msg.payload);
        CHECK(r.tick == 5);
        REQUIRE(r.bullets.size() == 1);
        CHECK(r.bullets[0].id == 1);
        CHECK(r.bullets[0].start_x == Catch::Approx(-1.f));
        REQUIRE(r.alive_bullet_ids.size() == 2);
        CHECK(r.alive_bullet_ids[1] == 2);
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
    st.guns.push_back(gun_info{7, "wpn_test", 10.f, 20.f, 3.f, 4.f, 45.f, 1, 0});
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
        gs.sim.update_immediate(1.f / 60.f, std::chrono::steady_clock::now());
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

    SECTION("drop gun") {
        REQUIRE(!pl->pistol_section().empty());
        auto before = gs.dropped_guns.size();

        pl->apply_input(net::player_input_state{.drop = true});
        CHECK(pl->pistol_section().empty());
        CHECK(gs.dropped_guns.size() == before + 1);
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
    auto t = std::chrono::steady_clock::now();
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
    auto t0 = std::chrono::steady_clock::now();
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

        auto window = [](u32 last_seq, std::vector<u16> bits) {
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
                                         net::input{.last_seq = 3, .bits = {R | net::in_bit_jump, R, R}})));
        srv.update(gs);
        sim_tick(gs);
        CHECK(pl.is_walking());

        /* Not enough queued after running dry (jitter buffer): the held movement repeats */
        sim_tick(gs);
        sim_tick(gs);

        sim_tick(gs);
        CHECK(pl.is_walking());

        std::this_thread::sleep_for(net::net_server::input_timeout + std::chrono::milliseconds(50));
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
        std::vector<u16> bits(10, 0);
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

        srv.update(gs_srv); /* broadcast state */
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
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            gs_srv.game_update();
        }

        REQUIRE(!gs_srv.cur_frame_bullets.empty());
        CHECK(std::string(gs_srv.cur_frame_bullets.front().owner) == "me");

        srv.update(gs_srv); /* broadcast state with the bullet */
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
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            gs_srv.game_update();
        }

        REQUIRE(!gs_srv.cur_frame_bullets.empty());
        CHECK(std::string(gs_srv.cur_frame_bullets.front().owner) == "hostp");

        srv.update(gs_srv); /* broadcast state with the bullet */
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
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            gs_srv.game_update();
        }
        REQUIRE(!gs_srv.cur_frame_bullets.empty());

        srv.update(gs_srv); /* broadcast the bullet with its id */
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
        srv.update(gs_srv);
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
            static val interpolate(const val& a, const val& b, float f) {
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
        constexpr float slowdown = 700.f, gravity = 1200.f, dt = 1.f / 120.f;
        auto run = [&](knockback_offset<int>& kb, float seconds) {
            for (float t = 0.f; t < seconds; t += dt)
                kb.step(dt, slowdown, gravity);
        };

        /* A grounded push slides and stops: v^2 / (2 * slowdown) */
        knockback_offset<int> push;
        push.predict(1, {300.f, 0.f}, true);
        run(push, 1.f);
        CHECK(push.offset().x == Catch::Approx(300.f * 300.f / (2.f * slowdown)).margin(3.f));
        CHECK(push.velocity().x == Catch::Approx(0.f));

        /* A lift arcs back to the ground */
        knockback_offset<int> lift;
        lift.predict(1, {0.f, -600.f}, true);
        run(lift, 0.3f);
        CHECK(lift.offset().y < -50.f);
        run(lift, 1.f);
        CHECK(lift.offset().y == Catch::Approx(0.f));

        /* Predicted + confirmed: the opposite motion cancels it out, nothing stays */
        knockback_offset<int> confirmed;
        confirmed.predict(7, {300.f, 0.f}, true);
        run(confirmed, 0.2f);
        auto mid = confirmed.offset().x;
        CHECK(mid > 30.f);
        confirmed.confirm([](int ref) { return ref == 7; }, true);
        run(confirmed, 0.02f);
        CHECK(confirmed.offset().x < mid); /* hands over continuously, no jump */
        CHECK(confirmed.offset().x > mid - 10.f);
        run(confirmed, 2.f);
        CHECK(confirmed.empty());

        /* Not confirmed in time: fades out */
        knockback_offset<int> missed;
        missed.predict(1, {300.f, 0.f}, true);
        run(missed, 0.3f);
        missed.miss_older_than(0.25f);
        run(missed, 1.f);
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
    u16             port = free_udp_port();
    game_state      gs_srv;
    game_state      gs_cli;
    net::net_server srv{port};
    std::optional<net::net_client> cli;
    net::player_input_state       input;

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
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gs_cli.game_update();
        cli->update(gs_cli);
        gs_srv.game_update();
        srv.update(gs_srv);
    }

    void run_for(std::chrono::milliseconds duration, auto&& on_frame) {
        auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end) {
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
    float prev_x      = remote.get_position().x;
    float window_x    = prev_x;
    float max_speed   = 0.f;
    float min_step    = 0.f;
    auto  window_from = std::chrono::steady_clock::now();
    f.run_for(1500ms, [&] {
        auto x   = remote.get_position().x;
        min_step = std::min(min_step, x - prev_x);
        prev_x   = x;

        /* Speed over ~50ms windows: never faster than the max speed (no catch-up jumps) */
        auto now = std::chrono::steady_clock::now();
        auto dt  = std::chrono::duration<float>(now - window_from).count();
        if (dt >= 0.05f) {
            max_speed   = std::max(max_speed, (x - window_x) / dt);
            window_x    = x;
            window_from = now;
        }
    });

    CHECK(max_speed < bot->get_max_speed() * 1.3f);
    CHECK(min_step > -0.5f); /* never moves backwards */
    CHECK(remote.get_position().x > 400.f + 100.f);

    /* The remote player is shown in the past, by about the interpolation delay */
    CHECK(remote.get_position().x < bot->get_position().x);
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
    auto t = std::chrono::steady_clock::now();
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

    hit_fixture(): net_game_fixture(80, 170) {
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

    auto t = std::chrono::steady_clock::now();
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
    CHECK(net::remote_player_snapshot::interpolate(a, b, 0.5f).ph.pos.x == Catch::Approx(500.f));

    b.teleports = 1;
    CHECK(net::remote_player_snapshot::interpolate(a, b, 0.5f).ph.pos.x == Catch::Approx(1000.f));
    CHECK(net::remote_player_snapshot::interpolate(a, b, 1.f).ph.pos.x == Catch::Approx(0.f));
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

    using clock = std::chrono::steady_clock;
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
    /* Without prediction the knockback shows up one way trip (160..340 ms) after the server hit */
    CHECK(*cli_hit - *srv_hit < 100ms);

    /* Settled: confirmed hits were not applied twice */
    f.run_for(2500ms);
    CHECK(f.local().get_position().x == Catch::Approx(f.server_side().get_position().x).margin(1.f));
}

TEST_CASE("net: own bullet hits on remote players are predicted") {
    using namespace std::chrono_literals;
    hit_fixture f;

    using clock = std::chrono::steady_clock;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

    udp_channel srv;
    REQUIRE(srv.bind(0));
    auto port = srv.local_port();
    srv.set_simulated_latency(80, 170);

    udp_channel cli;
    REQUIRE(cli.bind(0));
    cli.set_simulated_latency(80, 170);

    const std::vector<u8> data = {1, 2, 3, 4};
    auto t0 = std::chrono::steady_clock::now();

    REQUIRE(cli.send_to(sf::IpAddress("127.0.0.1"), port, data));

    /*
     * The datagram must not arrive instantly: each one-way hop is delayed
     * by >= 80ms, so the earliest delivery is ~160ms after the send.
     * Both channels are pumped every frame, like in the game
     */
    bool arrived = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(40)) {
        cli.recv(std::chrono::milliseconds(5));
        if (!srv.recv(std::chrono::milliseconds(5)).empty()) {
            arrived = true;
            break;
        }
    }
    REQUIRE(!arrived);

    /* ...and it must arrive within the worst case (170ms + 170ms) plus margin */
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(600)) {
        cli.recv(std::chrono::milliseconds(10));
        if (!srv.recv(std::chrono::milliseconds(10)).empty()) {
            arrived = true;
            break;
        }
    }
    REQUIRE(arrived);
}
