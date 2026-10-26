#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/types.hpp"
#include "base/serialization.hpp"
#include "base/fixed_string.hpp"

namespace dfdh::net {

/*
 * Datagram format:
 *   u16 type
 *   u16 payload size (bytes)
 *   payload...
 */

inline constexpr size_t net_header_size = 2 * sizeof(u16);

/* Bumped on every incompatible protocol change; HELLO with another version is rejected */
inline constexpr u16 protocol_version = 4;
inline constexpr size_t net_max_msg_size = 1024;

enum class msg_type : u16 {
    hello   = 1,
    welcome = 2,
    input   = 3,
    state   = 4,
    leave   = 5,
    error   = 6,
};

/* Input bits */
inline constexpr u16 in_bit_left         = 1 << 0;
inline constexpr u16 in_bit_right        = 1 << 1;
inline constexpr u16 in_bit_jump         = 1 << 2;
inline constexpr u16 in_bit_jump_down    = 1 << 3;
inline constexpr u16 in_bit_shot         = 1 << 4;
inline constexpr u16 in_bit_long_shot    = 1 << 6;
inline constexpr u16 in_bit_drop         = 1 << 7;
inline constexpr u16 in_edge_bits        = in_bit_jump | in_bit_jump_down | in_bit_drop;

/**
 * Per-tick player input state (held state + one-tick edges)
 */
struct player_input_state {
    bool left         = false; // held
    bool right        = false; // held
    bool jump         = false; // edge
    bool jump_down    = false; // edge
    bool shot         = false; // held
    bool long_shot    = false; // held
    bool drop         = false; // edge

    [[nodiscard]]
    u16 to_bits() const {
        u16 bits = 0;
        if (left)         bits |= in_bit_left;
        if (right)        bits |= in_bit_right;
        if (jump)         bits |= in_bit_jump;
        if (jump_down)    bits |= in_bit_jump_down;
        if (shot)         bits |= in_bit_shot;
        if (long_shot)    bits |= in_bit_long_shot;
        if (drop)         bits |= in_bit_drop;
        return bits;
    }

    static player_input_state from_bits(u16 bits) {
        return {
            .left         = (bits & in_bit_left) != 0,
            .right        = (bits & in_bit_right) != 0,
            .jump         = (bits & in_bit_jump) != 0,
            .jump_down    = (bits & in_bit_jump_down) != 0,
            .shot         = (bits & in_bit_shot) != 0,
            .long_shot    = (bits & in_bit_long_shot) != 0,
            .drop         = (bits & in_bit_drop) != 0,
        };
    }

    void serialize(auto& s) const {
        s.write(left, right, jump, jump_down, shot, long_shot, drop);
    }

    void deserialize(auto& d) {
        d.read(left, right, jump, jump_down, shot, long_shot, drop);
    }
};

struct hello {
    u16           version = protocol_version;
    player_name_t name;
    std::string   pistol;
    u32           face_id      = 0;
    u32           body_id      = 0;
    u32           body_color   = 0; // packed 0xRRGGBBAA
    u32           tracer_color = 0; // packed 0xRRGGBBAA

    void serialize(auto& s) const {
        s.write(version, std::string(name), pistol, face_id, body_id, body_color, tracer_color);
    }

    void deserialize(auto& d) {
        std::string n;
        d.read(version);
        if (version != protocol_version)
            return; /* the rest of the layout is unknown */
        d.read(n, pistol, face_id, body_id, body_color, tracer_color);
        name = n;
    }
};

struct welcome {
    player_name_t name;

    void serialize(auto& s) const {
        s.write(std::string(name));
    }

    void deserialize(auto& d) {
        std::string n;
        d.read(n);
        name = n;
    }
};

/**
 * Client input: a window of the last per-tick input states (redundant, so a
 * lost or reordered datagram costs nothing: every datagram carries the
 * recent history). bits[i] is the input of tick seq first_seq() + i
 */
struct input {
    static constexpr size_t max_window = 32;

    u32              last_seq       = 0;
    u32              client_time_ms = 0; // echoed back in the state for RTT measurement
    u16              rtt_ms         = 0; // client-measured round-trip time
    u16              interp_ms      = 0; // client remote-player interpolation delay
    std::vector<u16> bits;

    [[nodiscard]]
    u32 first_seq() const {
        return last_seq + 1 - u32(bits.size());
    }

    void serialize(auto& s) const {
        s.write(last_seq, client_time_ms, rtt_ms, interp_ms, bits);
    }

    void deserialize(auto& d) {
        d.read(last_seq, client_time_ms, rtt_ms, interp_ms, bits);
        if (bits.size() > max_window || bits.size() > size_t(last_seq))
            throw std::out_of_range("net: bad input window");
    }
};

struct state_player {
    player_name_t name;
    std::string   pistol;
    u32           face_id      = 0;
    u32           body_id      = 0;
    u32           body_color   = 0;
    u32           tracer_color = 0;

    /* Physic state (see player::physic_state) */
    float pos_x      = 0.f;
    float pos_y      = 0.f;
    float vel_x      = 0.f;
    float vel_y      = 0.f;
    u8    jumps_left = 0;
    u8    lock_y     = 0;
    float accel_lf   = 1.f;
    float accel_rf   = 1.f;

    u8  on_left  = 0;
    u8  shooting = 0; // trigger held
    i32 group    = 0;

    std::vector<u32> hits; // ids of the bullets that hit the player in the last second

    void serialize(auto& s) const {
        s.write(std::string(name), pistol, face_id, body_id, body_color, tracer_color,
                pos_x, pos_y, vel_x, vel_y, jumps_left, lock_y, accel_lf, accel_rf, on_left, shooting, group, hits);
    }

    void deserialize(auto& d) {
        std::string n;
        d.read(n, pistol, face_id, body_id, body_color, tracer_color,
               pos_x, pos_y, vel_x, vel_y, jumps_left, lock_y, accel_lf, accel_rf, on_left, shooting, group, hits);
        name = n;
    }
};

/**
 * A bullet spawned on the server this frame (for client-side spawning).
 * start is the muzzle position, pos the (lag-forwarded) server spawn position
 */
struct bullet_info {
    std::string owner; // player name
    u32         id       = 0;
    /* Shot key (matches the owner's predicted bullets): trigger pull input tick, shot index since the pull, pellet */
    u32         pull_seq   = 0;
    u16         shot_index = 0;
    u8          pellet     = 0;
    float       start_x = 0.f;
    float       start_y = 0.f;
    float       pos_x  = 0.f;
    float       pos_y  = 0.f;
    float       vel_x  = 0.f;
    float       vel_y  = 0.f;
    float       hit_mass = 0.f;
    u32         tracer_color = 0; // packed 0xRRGGBBAA
    bool        gravity = false; // gravity flag the bullet was spawned with

    void serialize(auto& s) const {
        s.write(owner, id, pull_seq, shot_index, pellet, start_x, start_y, pos_x, pos_y, vel_x, vel_y, hit_mass, tracer_color, gravity);
    }

    void deserialize(auto& d) {
        d.read(owner, id, pull_seq, shot_index, pellet, start_x, start_y, pos_x, pos_y, vel_x, vel_y, hit_mass, tracer_color, gravity);
    }
};

/**
 * A crate in the server-authoritative simulation
 */
struct crate_info {
    u32  id = 0;
    float pos_x = 0.f;
    float pos_y = 0.f;
    float vel_x = 0.f;
    float vel_y = 0.f;
    u8   landed = 0;

    void serialize(auto& s) const {
        s.write(id, pos_x, pos_y, vel_x, vel_y, landed);
    }

    void deserialize(auto& d) {
        d.read(id, pos_x, pos_y, vel_x, vel_y, landed);
    }
};

/**
 * A dropped gun in the server-authoritative simulation
 */
struct gun_info {
    u32         id        = 0;
    std::string section;    // weapon section for drawing
    float       pos_x     = 0.f;
    float       pos_y     = 0.f;
    float       vel_x     = 0.f;
    float       vel_y     = 0.f;
    float       angle     = 0.f; // sprite rotation (degrees, pre-mirror)
    u8          on_left   = 0;
    u8          long_shot = 0;

    void serialize(auto& s) const {
        s.write(id, section, pos_x, pos_y, vel_x, vel_y, angle, on_left, long_shot);
    }

    void deserialize(auto& d) {
        d.read(id, section, pos_x, pos_y, vel_x, vel_y, angle, on_left, long_shot);
    }
};

struct state {
    u32 tick           = 0; // broadcast sequence number (several broadcasts can share a simulation tick)
    u32 server_time_ms = 0; // server clock at the simulation tick of this state

    /* Per receiving client */
    u32 ack_seq             = 0; // last input tick applied to the client's player
    u32 echo_client_time_ms = 0; // client_time_ms of the client's newest input
    u16 echo_hold_ms        = 0; // time since that input was received

    std::vector<state_player> players;
    std::vector<bullet_info>  bullets;
    std::vector<crate_info>   crates;
    std::vector<gun_info>     guns;
    /*
     * Every bullet id currently alive on the server: clients drop their local
     * bullets that are no longer listed (e.g. hit a player whose local
     * position diverged)
     */
    std::vector<u32> alive_bullet_ids;

    void serialize(auto& s) const {
        s.write(tick, server_time_ms, ack_seq, echo_client_time_ms, echo_hold_ms,
                players, bullets, crates, guns, alive_bullet_ids);
    }

    void deserialize(auto& d) {
        d.read(tick, server_time_ms, ack_seq, echo_client_time_ms, echo_hold_ms,
               players, bullets, crates, guns, alive_bullet_ids);
    }
};

struct leave {
    player_name_t name;

    void serialize(auto& s) const {
        s.write(std::string(name));
    }

    void deserialize(auto& d) {
        std::string n;
        d.read(n);
        name = n;
    }
};

struct error {
    std::string message;

    void serialize(auto& s) const {
        s.write(message);
    }

    void deserialize(auto& d) {
        d.read(message);
    }
};

/**
 * Encode a message to a datagram (header + payload)
 */
template <typename Msg>
auto encode_msg(msg_type type, const Msg& msg) -> std::vector<u8> {
    auto ser = ss::serializer(std::vector<u8>{});
    u16 placeholder_size = 0;
    ser.write(static_cast<u16>(type), placeholder_size);
    auto payload_start  = ser.data_holder.size();
    ser.write(msg);
    auto payload_size   = u16(ser.data_holder.size() - payload_start);
    std::memcpy(ser.data_holder.data() + 2, &payload_size, sizeof(u16));
    return ser.data_holder;
}

struct net_msg {
    msg_type             type;
    std::span<const u8>  payload;
};

/**
 * Decode a datagram (header + payload), throws std::out_of_range on error
 */
inline net_msg decode_msg(std::span<const u8> data) {
    if (data.size() < net_header_size)
        throw std::out_of_range("net: datagram too short");

    u16 type_id = 0, size = 0;
    std::memcpy(&type_id, data.data(), sizeof(u16));
    std::memcpy(&size, data.data() + 2, sizeof(u16));

    if (data.size() < net_header_size + size)
        throw std::out_of_range("net: datagram truncated");

    return {msg_type(type_id), std::span<const u8>{data.data() + net_header_size, size}};
}

/**
 * Deserialize a payload into T
 */
template <typename T>
T read_msg(std::span<const u8> payload) {
    T res;
    ss::deserializer<std::span<const u8>> deser{payload};
    deser.read(res);
    return res;
}

template <typename T>
[[nodiscard]]
bool try_read_msg(std::span<const u8> payload, T& res) {
    try {
        res = read_msg<T>(payload);
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

} // namespace dfdh::net
