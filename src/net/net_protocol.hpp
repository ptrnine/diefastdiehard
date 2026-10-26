#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
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
 *   payload... (ss::serializer, little endian, u16 string/vector lengths)
 */

inline constexpr size_t net_header_size = 2 * sizeof(u16);

/* Bumped on every incompatible protocol change; HELLO with another version is rejected */
inline constexpr u16 protocol_version = 13;
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
inline constexpr u8  in_bit_left         = 1 << 0;
inline constexpr u8  in_bit_right        = 1 << 1;
inline constexpr u8  in_bit_jump         = 1 << 2;
inline constexpr u8  in_bit_jump_down    = 1 << 3;
inline constexpr u8  in_bit_shot         = 1 << 4;
inline constexpr u8  in_bit_long_shot    = 1 << 6;
inline constexpr u8  in_bit_drop         = 1 << 7;
inline constexpr u8  in_edge_bits        = in_bit_jump | in_bit_jump_down | in_bit_drop;

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
    u8 to_bits() const {
        u8 bits = 0;
        if (left)         bits |= in_bit_left;
        if (right)        bits |= in_bit_right;
        if (jump)         bits |= in_bit_jump;
        if (jump_down)    bits |= in_bit_jump_down;
        if (shot)         bits |= in_bit_shot;
        if (long_shot)    bits |= in_bit_long_shot;
        if (drop)         bits |= in_bit_drop;
        return bits;
    }

    static player_input_state from_bits(u8 bits) {
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
 * Client input: a window of the per-tick input states the server did not
 * acknowledge yet, up to the last 32 (redundant, so a lost or reordered
 * datagram costs nothing: every datagram repeats them until they are
 * acknowledged). bits[i] is the input of tick seq first_seq() + i
 */
struct input {
    static constexpr size_t max_window = 32;

    u32              last_seq       = 0;
    u32              client_time_ms = 0; // echoed back in the state for RTT measurement
    u16              rtt_ms         = 0; // client-measured round-trip time
    u16              interp_ms      = 0; // client remote-player interpolation delay
    std::vector<u8>  bits;

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

/**
 * The static part of a player (appearance, gun, group), sent only when a
 * client may not know it yet (see net_server). Players are referenced by
 * their id everywhere else
 */
struct player_config {
    u16           id = 0;
    player_name_t name;
    std::string   pistol; // empty: no gun
    u16           face_id      = 0;
    u16           body_id      = 0;
    u32           body_color   = 0; // packed 0xRRGGBBAA
    u32           tracer_color = 0; // packed 0xRRGGBBAA
    i16           group        = 0;

    bool operator==(const player_config&) const = default;

    void serialize(auto& s) const {
        s.write(id, std::string(name), pistol, face_id, body_id, body_color, tracer_color, group);
    }

    void deserialize(auto& d) {
        std::string n;
        d.read(id, n, pistol, face_id, body_id, body_color, tracer_color, group);
        name = n;
    }
};

/* Accel factors ([0, 1]) on the wire */
inline u16 pack_unit(float v) {
    return u16(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f));
}

inline float unpack_unit(u16 v) {
    return float(v) / 65535.f;
}

/* The per-tick part of a player */
struct state_player {
    static constexpr u8 flag_lock_y   = 1 << 0;
    static constexpr u8 flag_on_left  = 1 << 1;
    static constexpr u8 flag_shooting = 1 << 2; // trigger held
    static constexpr u8 flag_throwing = 1 << 3; // throwing the gun away (player::throwing)

    u16 id = 0;

    /* Physic state (see player::physic_state), full precision: the client reconciles with it */
    float pos_x      = 0.f;
    float pos_y      = 0.f;
    float vel_x      = 0.f;
    float vel_y      = 0.f;
    u8    flags      = 0;
    u8    jumps_left = 0;
    u16   accel_lf   = 65535; // pack_unit
    u16   accel_rf   = 65535;
    u16   teleports  = 0; // player::teleports: a change means "jump there, don't interpolate"
    i16   score      = 0; // player::score

    std::vector<u32> hits = {}; // ids of the bullets that hit the player in the last second

    void serialize(auto& s) const {
        s.write(id, pos_x, pos_y, vel_x, vel_y, flags, jumps_left, accel_lf, accel_rf, teleports, score, hits);
    }

    void deserialize(auto& d) {
        d.read(id, pos_x, pos_y, vel_x, vel_y, flags, jumps_left, accel_lf, accel_rf, teleports, score, hits);
    }
};

/* Weapon logic state of the receiving client's own player (see weapon_instance::sync_state) */
struct weapon_sync {
    static constexpr u8 flag_reloading = 1 << 0;
    static constexpr u8 flag_trigger   = 1 << 1;
    static constexpr u16 unlimited_reserve = 0xffff;

    u16   ammo       = 0;
    u16   reserve    = unlimited_reserve; // ammo left beyond the magazine
    u8    anim       = 0;
    float anim_time  = 0.f;
    float since_shot = 0.f;
    u8    flags      = 0;
    u16   shot_index = 0;
    u32   seed_base  = 0;

    void serialize(auto& s) const {
        s.write(ammo, reserve, anim, anim_time, since_shot, flags, shot_index, seed_base);
    }

    void deserialize(auto& d) {
        d.read(ammo, reserve, anim, anim_time, since_shot, flags, shot_index, seed_base);
    }
};

/* A bullet's muzzle velocity */
struct pellet_info {
    float vel_x = 0.f;
    float vel_y = 0.f;

    void serialize(auto& s) const {
        s.write(vel_x, vel_y);
    }

    void deserialize(auto& d) {
        d.read(vel_x, vel_y);
    }
};

/**
 * The bullets of one shot spawned on the server (for client-side spawning):
 * pellet i has the id first_id + i and the shot key (pull_seq, shot_index,
 * first_pellet + i). Every pellet starts at the muzzle; the server forwarded
 * them by forward seconds along their ballistic path (lag compensation, 0
 * for a bullet consumed by the server's spawn hit-scan). The server tests
 * them against the players as they were rewind seconds ago (the shooter's
 * view, 0 for a player without lag compensation)
 */
struct shot_info {
    static constexpr u8    flag_gravity  = 1 << 0;
    static constexpr float forward_scale = 32768.f; // forward units per second (< 2 s)

    u16   owner        = 0; // player id
    u32   first_id     = 0;
    u32   pull_seq     = 0;
    u16   shot_index   = 0;
    u8    first_pellet = 0;
    float start_x      = 0.f;
    float start_y      = 0.f;
    u16   forward      = 0;
    u16   rewind       = 0; // same units as forward
    float hit_mass     = 0.f;
    u8    flags        = 0;

    std::vector<pellet_info> pellets;

    [[nodiscard]]
    float forward_s() const {
        return float(forward) / forward_scale;
    }

    [[nodiscard]]
    float rewind_s() const {
        return float(rewind) / forward_scale;
    }

    static u16 pack_forward(float seconds) {
        return u16(std::lround(std::clamp(seconds, 0.f, 65535.f / forward_scale) * forward_scale));
    }

    void serialize(auto& s) const {
        s.write(owner, first_id, pull_seq, shot_index, first_pellet, start_x, start_y, forward, rewind, hit_mass, flags, pellets);
    }

    void deserialize(auto& d) {
        d.read(owner, first_id, pull_seq, shot_index, first_pellet, start_x, start_y, forward, rewind, hit_mass, flags, pellets);
    }
};

/**
 * A set of bullet ids as a bitmap from the smallest one: bullet ids are
 * sequential and short-lived, so the live ones are close together
 */
struct alive_set {
    u32             base = 0;
    std::vector<u8> bits;

    template <typename R>
    static alive_set from_ids(const R& ids) {
        alive_set res;
        if (std::ranges::empty(ids))
            return res;
        auto [min, max] = std::ranges::minmax(ids);
        res.base = min;
        res.bits.resize((max - min) / 8 + 1);
        for (u32 id : ids)
            res.bits[(id - min) / 8] |= u8(1 << ((id - min) % 8));
        return res;
    }

    [[nodiscard]]
    bool contains(u32 id) const {
        if (id < base || (id - base) / 8 >= bits.size())
            return false;
        return (bits[(id - base) / 8] >> ((id - base) % 8)) & 1;
    }

    void serialize(auto& s) const {
        s.write(base, bits);
    }

    void deserialize(auto& d) {
        d.read(base, bits);
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
    u16         owner     = 0; // id of the player who threw it, 0: none
    u16         age_ms    = 0; // flight time since the throw (clamped)

    void serialize(auto& s) const {
        s.write(id, section, pos_x, pos_y, vel_x, vel_y, angle, on_left, long_shot, owner, age_ms);
    }

    void deserialize(auto& d) {
        d.read(id, section, pos_x, pos_y, vel_x, vel_y, angle, on_left, long_shot, owner, age_ms);
    }
};

/**
 * The server's game mode (see dfdh::game_mode), sent only when a client may
 * not know it (see net_server)
 */
struct game_mode_info {
    std::string section;
    float       bullet_gravity    = 0.f; // 0: off
    float       crate_drop_period = 0.f; // 0: crates never drop
    u32         magazines_count   = 0;   // 0: unlimited

    void serialize(auto& s) const {
        s.write(section, bullet_gravity, crate_drop_period, magazines_count);
    }

    void deserialize(auto& d) {
        d.read(section, bullet_gravity, crate_drop_period, magazines_count);
    }
};

struct state {
    u32 tick           = 0; // broadcast sequence number
    u32 server_time_ms = 0; // server clock at the simulation tick of this state

    /* Per receiving client */
    u32 ack_seq             = 0; // last input tick applied to the client's player
    u32 echo_client_time_ms = 0; // client_time_ms of the client's newest input
    u16 echo_hold_ms        = 0; // time since that input was received
    std::optional<weapon_sync> own_weapon; // the client's player's weapon (if it has a gun)
    std::optional<game_mode_info> mode;    // only when the client may not know it (see net_server)

    std::vector<player_config> configs; // only those the client may not know (see net_server)
    std::vector<state_player>  players;
    std::vector<shot_info>     shots;
    std::vector<crate_info>    crates;
    std::vector<gun_info>      guns;
    /*
     * Every bullet id currently alive on the server: clients drop their local
     * bullets that are no longer listed (e.g. hit a player whose local
     * position diverged)
     */
    alive_set alive_bullets;

    void serialize(auto& s) const {
        s.write(tick, server_time_ms, ack_seq, echo_client_time_ms, echo_hold_ms, own_weapon, mode,
                configs, players, shots, crates, guns, alive_bullets);
    }

    void deserialize(auto& d) {
        d.read(tick, server_time_ms, ack_seq, echo_client_time_ms, echo_hold_ms, own_weapon, mode,
               configs, players, shots, crates, guns, alive_bullets);
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

/* Length prefix of strings and vectors */
inline constexpr auto wire_size_type = ss::policy::size_type<u16>;

/**
 * Encode a message to a datagram (header + payload)
 */
template <typename Msg>
auto encode_msg(msg_type type, const Msg& msg) -> std::vector<u8> {
    auto ser = ss::serializer(std::vector<u8>{}, wire_size_type);
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
    auto deser = ss::deserializer(payload, wire_size_type);
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
