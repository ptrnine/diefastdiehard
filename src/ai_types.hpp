#pragma once

#include <map>

#include "base/vec2.hpp"
#include "base/fixed_string.hpp"

namespace dfdh
{
struct ai_player_t {
    vec2f         pos;
    vec2f         dir;
    vec2f         size;
    vec2f         vel;
    vec2f         acceleration;
    vec2f         barrel_pos;
    player_name_t name;
    u32           available_jumps;
    float         x_accel;
    float         x_slowdown;
    float         jump_speed;
    float         max_jump_dist;
    float         max_vel_x;
    float         gun_dispersion;
    int           group;
    float         gun_bullet_vel;
    float         gun_hit_power;
    float         gun_fire_rate;
    uint          gun_mag_size;
    uint          gun_mag_elapsed;
    bool          on_left;
    bool          is_y_locked;
    bool          long_shot_enabled;
    bool          is_walking;
    vec2f         long_shot_dir;
};

struct ai_physic_sim_t {
    vec2f gravity;
    float time_speed;
    u32   last_rps;
    bool  enable_gravity_for_bullets;
    /* y gravity value used for bullets, 0 when bullet gravity is off */
    float bullet_gravity = 0.f;
};

struct ai_level_t {
    vec2f level_size;
    float platforms_bound_start_x;
    float platforms_bound_end_x;
};

struct ai_bullet_t {
    vec2f pos;
    vec2f vel;
    float hit_mass;
    int   group;
};

struct ai_platform_t {
    vec2f pos1;
    vec2f pos2;
};

using ai_plat_dist_t       = vec2f;
using ai_plat_neighbours_t = std::vector<ai_plat_dist_t>;
using ai_plat_map_t        = std::vector<ai_plat_neighbours_t>;

struct ai_data_t {
    std::map<player_name_t, ai_player_t> players;
    std::vector<ai_bullet_t>             bullets;
    ai_physic_sim_t                      physic_sim;
    std::vector<ai_platform_t>           platforms;
    ai_plat_map_t                        platform_map;
    ai_level_t                           level;
};
} // namespace dfdh
