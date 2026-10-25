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
        in.seq  = 42;
        in.bits = in_bit_left | in_bit_shot;

        auto data = encode_msg(msg_type::input, in);
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::input);

        auto r = read_msg<input>(msg.payload);
        CHECK(r.seq == 42);
        CHECK(r.bits == (in_bit_left | in_bit_shot));
    }

    SECTION("state") {
        state st;
        st.tick = 7;
        st.players.push_back(state_player{.name = "p1", .pos_x = 10.f, .pos_y = 20.f,
                                          .vel_x = -3.f, .vel_y = 4.f, .jumps_left = 1,
                                          .on_left = 1, .shooting = 1});
        st.players.push_back(state_player{.name = "p2", .pistol = "wpn_m40", .face_id = 3});
        st.bullets.push_back(bullet_info{.owner = "p1", .pos_x = 11.f, .pos_y = 21.f,
                                         .vel_x = 900.f, .vel_y = -10.f, .hit_mass = 2.f,
                                         .tracer_color = 0xFF00FF00});

        auto data = encode_msg(msg_type::state, st);
        auto msg  = decode_msg({data.data(), data.size()});
        REQUIRE(msg.type == msg_type::state);

        auto r = read_msg<state>(msg.payload);
        CHECK(r.tick == 7);
        REQUIRE(r.players.size() == 2);
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
        auto data = encode_msg(msg_type::input, input{.seq = 1, .bits = 0});
        REQUIRE(data.size() == net_header_size + 4);
        /* claim a bigger payload than actually present */
        data[2] = u8(8);
        data[3] = 0;
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
    CHECK(back.shot_release == false);
    CHECK(back.long_shot == true);

    /* Round-trip through the wire */
    auto data = encode_msg(msg_type::input, input{.seq = 1, .bits = bits});
    auto msg  = decode_msg({data.data(), data.size()});
    auto wire = read_msg<input>(msg.payload);
    back = player_input_state::from_bits(wire.bits);
    CHECK(back.left == in.left);
    CHECK(back.right == in.right);
    CHECK(back.jump == in.jump);
    CHECK(back.jump_down == in.jump_down);
    CHECK(back.shot == in.shot);
    CHECK(back.shot_release == in.shot_release);
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

    /* v1 payload (crates, no guns): full read fails, state_v1 fallback works */
    state_v1 v1;
    v1.tick = 7;
    v1.crates.push_back(crate_info{1, 5.f, 6.f, 7.f, 8.f, 1});
    auto v1_data = encode_msg(msg_type::state, v1);
    auto v1_msg  = decode_msg({v1_data.data(), v1_data.size()});
    state full;
    CHECK_FALSE(try_read_msg(v1_msg.payload, full));
    state_v1 v1_back;
    CHECK(try_read_msg(v1_msg.payload, v1_back));
    CHECK(v1_back.tick == 7);
    CHECK(v1_back.crates.size() == 1);

    /* an old client (v1 layout) reads a new payload: trailing guns are ignored */
    state_v1 old_read;
    CHECK(try_read_msg(msg.payload, old_read));
    CHECK(old_read.tick == 42);
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
        auto data = encode_msg(msg_type::input, input{.seq = 1, .bits = in_bit_left});
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
        CHECK(in.shot_release == false);

        ctrl.track_event(keyrelease(sf::Keyboard::Comma));
        in = ctrl.take_input_state();
        CHECK(in.shot == false);
        CHECK(in.shot_release == true);

        in = ctrl.take_input_state();
        CHECK(in.shot_release == false);
    }

    SECTION("long shot is an edge") {
        ctrl.track_event(keypress(sf::Keyboard::M));
        auto in = ctrl.take_input_state();
        CHECK(in.long_shot == true);
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

    SECTION("long shot toggle") {
        pl->apply_input(net::player_input_state{.long_shot = true});
        CHECK(pl->long_shot_enabled());
    }

    SECTION("shot") {
        pl->apply_input(net::player_input_state{.shot = true});
        CHECK(pl->get_gun().pull_trigger(std::nullopt) == false); /* trigger is already held */
        pl->apply_input(net::player_input_state{.shot_release = true});
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

        net::input in;
        in.bits = net::in_bit_left;
        REQUIRE(cli.send(net::encode_msg(net::msg_type::input, in)));

        srv.update(gs);
        CHECK(pl.is_walking());

        in.bits = 0;
        REQUIRE(cli.send(net::encode_msg(net::msg_type::input, in)));
        srv.update(gs);
        CHECK_FALSE(pl.is_walking());
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

    SECTION("client sends input to the server") {
        srv.update(gs_srv);
        cli.update(gs_cli);

        cli.send_input(net::player_input_state{.left = true});
        REQUIRE(gs_srv.players.contains("me"));
        srv.update(gs_srv);
        CHECK(gs_srv.players.at("me")->is_walking());
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
        cli.send_input(net::player_input_state{.shot = true});
        srv.update(gs_srv); /* apply shot to the server player */
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
        CHECK(!gs_cli.players.at("hostp")->spawn_bullets_enabled());
        CHECK(!gs_cli.blt_mgr.bullets().empty());
    }
}

#if defined(DFDH_DEBUG_PING)
TEST_CASE("udp_channel simulates ~250ms ping (DFDH_DEBUG_PING)") {
    using namespace net;

    udp_channel srv;
    REQUIRE(srv.bind(0));
    auto port = srv.local_port();

    udp_channel cli;
    REQUIRE(cli.bind(0));

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
#endif
