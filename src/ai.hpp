#pragma once

#include <map>
#include <set>
#include <mutex>
#include <queue>
#include <atomic>
#include <memory>
#include <thread>
#include <optional>
#include <filesystem>

#include <SFML/System/Vector2.hpp>
#include <SFML/Graphics/Rect.hpp>
#include <SFML/System/Clock.hpp>

#include "base/log.hpp"
#include "base/fixed_string.hpp"
#include "base/types.hpp"
#include "base/vec_math.hpp"
#include "base/profiler.hpp"
#include "script/lua.hpp"
#include "ai_types.hpp"

namespace dfdh
{
namespace fs = std::filesystem;

class ai_mgr_singleton;

enum class ai_action {
    move_left = 0,
    move_right,
    stop,
    shot,
    relax,
    jump,
    jump_down,
    enable_long_shot,
    disable_long_shot,
    COUNT
};

class ai_operator_base {
public:
    static std::unique_ptr<ai_operator_base> create(const player_name_t& player_name, const std::string& difficulty);

    ai_operator_base(const player_name_t&);
    virtual ~ai_operator_base();

    const player_name_t& player_name() const {
        return _player_name;
    }

    std::optional<ai_action> consume_action() {
        std::lock_guard lock{mtx};

        if (_actions.empty())
            return {};
        else {
            auto t = _actions.front();
            _actions.pop();
            return t;
        }
    }

    void produce_action(ai_action action) {
        std::lock_guard lock{mtx};
        _actions.push(action);
    }

    template <typename... Acts>
    void unsafe_produce_actions(Acts... actions) {
        (_actions.push(actions), ...);
    }

    auto unsafe_steal_actions(ai_operator_base& from) {
        _actions = std::move(from._actions);
    }

    auto& mutex() {
        return mtx;
    }

    virtual std::string difficulty() const = 0;
    virtual void work(const ai_data_t& ai_data) = 0;

private:
    const player_name_t   _player_name;
    std::queue<ai_action> _actions;
    mutable std::mutex    mtx;
};

ai_operator_base::ai_operator_base(const player_name_t& iplayer_name): _player_name(iplayer_name) {}
ai_operator_base::~ai_operator_base() = default;

class ai_operator {
public:
    class ctor_access {
        friend ai_operator;
        ctor_access() = default;
    };

    ai_operator(ctor_access&, const player_name_t& player_name, const std::string& difficulty);
    ~ai_operator();

    static std::shared_ptr<ai_operator> create(const player_name_t& player_name, const std::string& difficulty) {
        ctor_access ca;
        return std::make_shared<ai_operator>(ca, player_name, difficulty);
    }

    std::optional<ai_action> consume_action() {
        return oper->consume_action();
    }

    void produce_action(ai_action action) {
        oper->produce_action(action);
    }

    [[nodiscard]]
    const player_name_t& player_name() const {
        return oper->player_name();
    }

    [[nodiscard]]
    std::string difficulty() const {
        return oper->difficulty();
    }

    void set_difficulty(const std::string& difficulty);

    void work(const ai_data_t& ai_data) {
        oper->work(ai_data);
    }

private:
    std::unique_ptr<ai_operator_base> oper;
};

class ai_mgr_singleton {
public:
    static ai_mgr_singleton& instance() {
        static ai_mgr_singleton inst;
        return inst;
    }

    ai_mgr_singleton(const ai_mgr_singleton&)            = delete;
    ai_mgr_singleton& operator=(const ai_mgr_singleton&) = delete;

    void reset_all() {
        std::lock_guard lock{mtx};

        _data.players.clear();
        _data.bullets.clear();
        _data.platforms.clear();
        _data.platform_map.clear();
    }

    template <typename C, typename F>
    void provide_players(const C& c, F get_adapter) {
        std::lock_guard lock{mtx};

        /* The provided players only: removed (or eliminated) ones are gone */
        _data.players.clear();
        for (auto& [_, p] : c) {
            auto insert_player = get_adapter(p);
            _data.players.insert_or_assign(insert_player.name, std::move(insert_player));
        }
    }

    void provide_physic_sim(const vec2f&  gravity,
                            float         time_speed,
                            u32           last_rps,
                            bool          enable_gravity_for_bullets,
                            float         bullet_gravity = 0.f) {
        std::lock_guard lock{mtx};

        _data.physic_sim.gravity                    = gravity;
        _data.physic_sim.time_speed                 = time_speed;
        _data.physic_sim.last_rps                   = last_rps;
        _data.physic_sim.enable_gravity_for_bullets = enable_gravity_for_bullets;
        _data.physic_sim.bullet_gravity             = bullet_gravity;
    }

    void provide_level(const vec2f& level_size) {
        std::lock_guard lock{mtx};

        _data.level.level_size = level_size;
    }

    template <typename C, typename F>
    void provide_bullets(const C& c, F get_adapter) {
        std::lock_guard lock{mtx};

        _data.bullets.resize(c.size());
        size_t i = 0;
        for (auto it = c.begin(); it != c.end(); ++it) _data.bullets[i++] = get_adapter(*it);
    }

    template <typename C, typename F>
    void provide_platforms(const C& c, F get_adapter) {
        std::lock_guard lock{mtx};

        _data.platforms.resize(c.size());
        for (size_t i = 0; i < _data.platforms.size(); ++i) _data.platforms[i] = get_adapter(c[i]);

        rebuild_platform_map();
    }

    bool running() const {
        return _work;
    }

    void worker_start() {
        _work    = true;
        _stopped = false;

        _thread = std::thread(&ai_mgr_singleton::worker, this);
    }

    void worker_stop() {
        _work = false;
        while (!_stopped)
            ;

        _thread.join();
    }

    void worker() {
        while (_work) {
            auto step = std::chrono::microseconds(u64(1000000) / _data.physic_sim.last_rps);

            auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard lock{mtx};
                worker_op();
            }
            auto prof = _prof.scope("wait");
            auto elapsed = std::chrono::steady_clock::now() - now;
            if (elapsed < step)
                std::this_thread::sleep_for(step - elapsed);
        }
        _stopped = true;
    }

    void add_ai_operator(ai_operator* oprt) {
        std::lock_guard lock{mtx};

        _operators.emplace(oprt->player_name(), oprt);
    }

    void remove_ai_operator(const player_name_t& name) {
        std::lock_guard lock{mtx};

        _operators.erase(name);
    }

    const ai_data_t& data() const {
        return _data;
    }

    template <typename F>
    void profiler_print(F&& print_callback) {
        std::lock_guard lock{_prof_mtx};
        _ts_prof.print_once(std::forward<F>(print_callback));
    }

    struct ai_luactx_mtxlock {
    public:
        ai_luactx_mtxlock(luactx_mgr& ilua, std::mutex& imtx): lua(ilua), mtx(imtx) {
            mtx.lock();
        }
        ~ai_luactx_mtxlock() {
            mtx.unlock();
        }

        luactx_mgr* operator->() {
            return &lua;
        }

        const luactx_mgr* operator->() const {
            return &lua;
        }

    private:
        luactx_mgr& lua;
        std::mutex& mtx;
    };

    ai_luactx_mtxlock luactx() {
        return {lua, mtx};
    }

private:
    ai_mgr_singleton() = default;
    ~ai_mgr_singleton() {
        if (_work)
            worker_stop();
    }

    void worker_op();

    ai_data_t _data;

public:
    std::map<player_name_t, ai_operator*> _operators;
    std::atomic<bool>                     _work;
    std::atomic<bool>                     _stopped;
    //timer                                 _timer;
    //std::chrono::steady_clock::time_point _next_time = std::chrono::steady_clock::now();
    std::thread                           _thread;
    luactx_mgr                            lua = luactx_mgr::ai();

    profiler   _prof;
    profiler   _ts_prof;
    std::mutex _prof_mtx;

private:
    friend class ai_operator;
    mutable std::mutex mtx;

private:
    void rebuild_platform_map() {
        _data.level.platforms_bound_start_x = std::numeric_limits<float>::max();
        _data.level.platforms_bound_end_x   = std::numeric_limits<float>::lowest();

        _data.platform_map.clear();
        _data.platform_map.resize(_data.platforms.size());

        for (size_t i = 0; i != _data.platforms.size(); ++i) {
            if (_data.platforms[i].pos1.x < _data.level.platforms_bound_start_x)
                _data.level.platforms_bound_start_x = _data.platforms[i].pos1.x;

            if (_data.platforms[i].pos2.x > _data.level.platforms_bound_end_x)
                _data.level.platforms_bound_end_x = _data.platforms[i].pos2.x;

            auto neighbours = ai_plat_neighbours_t(_data.platforms.size());

            for (size_t j = 0; j != _data.platforms.size(); ++j) {
                if (i == j) {
                    neighbours[j] = {0.f, 0.f};
                    continue;
                }

                vec2f dist{0.f, 0.f};

                auto& current   = _data.platforms[i];
                auto& neighbour = _data.platforms[j];
                float cl        = current.pos1.x;
                float cr        = current.pos2.x;
                float cy        = current.pos1.y;

                float nl = neighbour.pos1.x;
                float nr = neighbour.pos2.x;
                float ny = neighbour.pos1.y;

                if ((cl <= nr && cr > nl) || (cl < nr && cr >= nl))
                    dist.y = neighbour.pos1.y - current.pos1.y;
                else if (cl > nr)
                    dist = vec2f{nr - cl, ny - cy};
                else
                    dist = vec2f{nl - cr, ny - cy};

                neighbours[j] = dist;
            }

            _data.platform_map[i] = std::move(neighbours);
        }
    }
};

inline ai_mgr_singleton& ai_mgr() {
    return ai_mgr_singleton::instance();
}

void ai_operator::set_difficulty(const std::string& difficulty) {
    if (difficulty == oper->difficulty())
        return;

    auto new_oper = ai_operator_base::create(oper->player_name(), difficulty);

    std::lock_guard lock{ai_mgr().mtx};

    /* Steal actions from old operator and stop&relax */
    new_oper->unsafe_steal_actions(*oper);
    new_oper->unsafe_produce_actions(ai_action::relax, ai_action::stop, ai_action::disable_long_shot);

    oper = std::move(new_oper);
}

ai_operator::ai_operator(ctor_access&, const player_name_t& player_name, const std::string& difficulty):
    oper(ai_operator_base::create(player_name, difficulty)) {
    ai_mgr().add_ai_operator(this);
}

ai_operator::~ai_operator() {
    ai_mgr().remove_ai_operator(player_name());
}

struct platform_res_t {
    const ai_platform_t *stand_on = nullptr, *can_stand_on = nullptr, *nearest = nullptr;
};

enum class ai_native_difficulty { easy = 0, medium, hard, sniper, invalid };

std::string ai_native_difficulty_to_string(ai_native_difficulty value) {
    switch (value) {
    case ai_native_difficulty::easy: return "easy";
    case ai_native_difficulty::medium: return "medium";
    case ai_native_difficulty::hard: return "hard";
    case ai_native_difficulty::sniper: return "sniper";
    default: return "invalid";
    }
}

ai_native_difficulty ai_native_difficulty_from_string(const std::string& value) {
    if (value == "easy")
        return ai_native_difficulty::easy;
    if (value == "medium")
        return ai_native_difficulty::medium;
    if (value == "hard")
        return ai_native_difficulty::hard;
    if (value == "sniper")
        return ai_native_difficulty::sniper;
    return ai_native_difficulty::invalid;
}


class ai_operator_native : public ai_operator_base {
public:
    ai_operator_native(const player_name_t& player_name, ai_native_difficulty difficulty):
        ai_operator_base(player_name), _difficulty(difficulty) {
    }

    void move_left() {
        produce_action(ai_action::move_left);
        _mov_left  = true;
        _mov_right = false;
    }

    void move_right() {
        produce_action(ai_action::move_right);
        _mov_right = true;
        _mov_left  = false;
    }

    void stop() {
        produce_action(ai_action::stop);
        _mov_left  = false;
        _mov_right = false;
    }

    void jump() {
        if (!_jump_was) {
            _delayed_jump_delay = 0.0f;
            produce_action(ai_action::jump);
            _jump_was = true;
        }
    }

    void delayed_jump(float delay) {
        if (_delayed_jump_delay > 0.001f)
            return;

        _delayed_jump_delay = delay;
        _start_delayed_jump.restart();
    }

    void jump_down() {
        produce_action(ai_action::jump_down);
    }

    void shot() {
        _shot = true;
        produce_action(ai_action::shot);
        _timer.restart();
    }

    void relax() {
        produce_action(ai_action::relax);
        _shot = false;
    }

    [[nodiscard]] bool is_on_shot() const {
        return _shot;
    }

    std::string difficulty() const final {
        return ai_native_difficulty_to_string(_difficulty);
    }

    [[nodiscard]] ai_native_difficulty native_difficulty() const {
        return _difficulty;
    }

    /* Hard logic: sniper behaves exactly like hard, except it shoots with the
       lua insane shooter (insane_shooter) instead of hard_shooter */
    [[nodiscard]] bool is_hardlike() const {
        return _difficulty == ai_native_difficulty::hard || _difficulty == ai_native_difficulty::sniper;
    }

    void set_native_difficulty(ai_native_difficulty value) {
        _difficulty = value;
    }

    void shot_next_frame(float shot_delay = 0.f) {
        if (!_shot_next_frame) {
            _shot_delay      = shot_delay;
            _shot_next_frame = true;
            _start_shot_timer.restart();
        }
    }

    void update_shot_next_frame(const ai_physic_sim_t& sim) {
        _jump_was = false;

        if (_delayed_jump_delay > 0.001f && _start_delayed_jump.elapsed(sim.time_speed) > _delayed_jump_delay)
            jump();

        if (_shot_next_frame && _start_shot_timer.elapsed(sim.time_speed) > _shot_delay) {
            shot();
            _shot_next_frame = false;
        }
    }

    [[nodiscard]] auto& operated_platforms() const {
        return _operated_platforms;
    }

    [[nodiscard]] auto& operated_platforms() {
        return _operated_platforms;
    }

    [[nodiscard]] float jump_x_max() const {
        return _jump_x_max;
    }

    [[nodiscard]] auto last_stand_on_plat() const {
        return _last_stand_on_plat;
    }

    static constexpr player_name_t no_target = player_name_t{};

    void work(const ai_data_t& ai_data) final;

    player_name_t        _target_id;
    player_name_t        _last_target_id;
    /* insane_shooter state (sniper difficulty) */
    player_name_t        _insane_target_name{};
    timer                _insane_relax_timer;
    bool                 _insane_relax_active = false;
    float                _jump_x_max         = 0.f;
    const ai_platform_t* _last_stand_on_plat = nullptr;
    platform_res_t       _operated_platforms;

private:
    ai_native_difficulty   _difficulty;

    float _shot_delay = 0.f;
    timer _start_shot_timer;
    float _delayed_jump_delay = 0.f;
    timer _start_delayed_jump;
    float _last_plat_path_delay = 0.f;
    timer _start_plat_path;

    timer _timer;
    bool  _mov_left        = false;
    bool  _mov_right       = false;
    bool  _shot            = false;
    bool  _shot_next_frame = false;
    bool  _jump_was        = false;
};

class ai_operator_lua : public ai_operator_base {
public:
    using update_func_t = void(ai_operator_base*, const ai_data_t*);
    using init_func_t   = void(ai_operator_base*);

    ai_operator_lua(const player_name_t& player_name, std::string operator_name):
        ai_operator_base(player_name), _operator_name(std::move(operator_name)) {

        auto lua       = ai_mgr().luactx();
        auto init_func = lua->get_caller_type_erased<init_func_t>("AI." + _operator_name + ".init");
        _update_func   = lua->get_caller_type_erased<update_func_t>("AI." + _operator_name + ".update", 2s);

        init_func(this);
    }

    std::string difficulty() const final {
        return _operator_name;
    }

    void work(const ai_data_t& ai_data) final {
        _update_func(this, &ai_data);
    }

private:
    const std::string            _operator_name;
    std::function<update_func_t> _update_func;
};

std::unique_ptr<ai_operator_base> ai_operator_base::create(const player_name_t& player_name,
                                                           const std::string&   difficulty) {
    if (auto d = ai_native_difficulty_from_string(difficulty); d != ai_native_difficulty::invalid)
        return std::make_unique<ai_operator_native>(player_name, d);
    else
        return std::make_unique<ai_operator_lua>(player_name, difficulty);
}

inline bool overlap(float a1, float a2, float b1, float b2) {
    return (a2 > b1 && a1 < b2) || (b2 > a1 && b1 < a2);
}

inline constexpr float falling_dist = 350.f;

inline bool
hard_i_see_you(const ai_player_t& it, const ai_player_t& pl, const ai_level_t& level, float gun_dispersion) {
    if (it.group != -1 && it.group == pl.group)
        return false;

    if (gun_dispersion > M_PI)
        return true;

    if (std::fabs(pl.pos.x) - level.level_size.x > falling_dist)
        return false;

    auto disp   = gun_dispersion * 0.35f;
    auto x_dist = std::fabs(pl.pos.x - it.pos.x);
    auto y_add  = std::sin(disp) * x_dist;

    return overlap(
        pl.pos.y, pl.pos.y + pl.size.y, it.pos.y - y_add + it.size.y * 0.5f, it.pos.y + y_add + it.size.y * 0.5f);
}

inline bool i_see_you(const ai_player_t& it, const ai_player_t& pl) {
    if (it.group != -1 && it.group == pl.group)
        return false;
    return overlap(it.pos.y, it.pos.y + it.size.y, pl.pos.y, pl.pos.y + pl.size.y);
}

inline std::optional<std::pair<player_name_t, vec2f>>
easy_ai_find_nearest(const ai_player_t&                          it,
                     const std::map<player_name_t, ai_player_t>& players,
                     const ai_level_t&                           level,
                     ai_operator_native&                                oper) {
    static constexpr auto cmp = [](const std::pair<player_name_t, vec2f>& lhs,
                                   const std::pair<player_name_t, vec2f>& rhs) {
        return magnitude2(lhs.second) < magnitude2(rhs.second);
    };

    std::set<std::pair<player_name_t, vec2f>, decltype(cmp)> nearests;
    for (auto& [_, pl] : players) {
        auto iseeyou =
            oper.is_hardlike() ? hard_i_see_you(it, pl, level, it.gun_dispersion) : i_see_you(it, pl);
        if (pl.name != it.name && iseeyou)
            nearests.emplace(pl.name, pl.pos - it.pos);
    }

    if (nearests.empty())
        return {};
    else
        return *nearests.begin();
}

inline auto calc_dist_to_platform(const ai_platform_t& plat, const ai_player_t& plr) {
    if (plr.pos.x > plat.pos2.x)
        return plat.pos2 - plr.pos;
    else if (plr.pos.x + plr.size.x < plat.pos1.x)
        return plat.pos1 - (plr.pos + plr.size.x);
    else
        return vec2f(0.f, plat.pos1.y - plr.pos.y);
}

inline platform_res_t find_platform(const std::vector<ai_platform_t>& platforms, const ai_player_t& player) {
    platform_res_t res;
    vec2f          nearest_dist;
    float          nearest_dist_sc = std::numeric_limits<float>::max();

    auto& p = player.pos;
    for (auto& pl : platforms) {
        if (p.x > pl.pos1.x && p.x < pl.pos2.x) {
            if (approx_equal(pl.pos1.y, p.y, 0.0001f)) {
                res.stand_on     = &pl;
                res.can_stand_on = &pl;
                res.nearest      = &pl;
                break;
            }
            else {
                if (pl.pos1.y > player.pos.y &&
                    (!res.can_stand_on || res.can_stand_on->pos1.y - player.pos.y > pl.pos1.y - player.pos.y)) {
                    res.can_stand_on = &pl;
                }
            }
        }

        auto  dist_to_plat    = calc_dist_to_platform(pl, player);
        float dist_to_plat_sc = magnitude(dist_to_plat);
        if (!res.nearest || dist_to_plat_sc < nearest_dist_sc) {
            res.nearest     = &pl;
            nearest_dist    = dist_to_plat;
            nearest_dist_sc = dist_to_plat_sc;
        }
    }

    return res;
}

enum ai_move_type { ai_move_none = 0, ai_move_left, ai_move_right, ai_move_stop };

struct ai_move_spec {
    void update_move(ai_move_type itype, int ipriority) {
        if (ipriority >= priority) {
            type     = itype;
            priority = ipriority;
        }
    }

    void update_apply(ai_operator_native& oper, ai_move_type itype, int ipriority) {
        if (ipriority >= priority) {
            apply(oper);
            type     = itype;
            priority = ipriority;
        }
    }

    void apply(ai_operator_native& oper) {
        switch (type) {
        case ai_move_left: oper.move_left(); break;
        case ai_move_right: oper.move_right(); break;
        case ai_move_stop: oper.stop(); break;
        default: break;
        }
        type     = ai_move_none;
        priority = 0;
    }

    ai_move_type type     = ai_move_none;
    int          priority = 0;
};

inline void easy_ai_platform_actions(const std::vector<ai_platform_t>& platforms,
                                     const platform_res_t&             plat,
                                     const ai_player_t&                player,
                                     ai_operator_native&                      oper,
                                     ai_move_spec&                     move_spec) {
    auto [stand_on, can_stand_on, nearest_plat] = plat;

    if (!stand_on && !can_stand_on) {
        if (nearest_plat) {
            auto dist = calc_dist_to_platform(*nearest_plat, player);
            if (dist.x < 0.f)
                move_spec.update_move(ai_move_left, 200);
            else
                move_spec.update_move(ai_move_right, 200);
            if (dist.y < 0.f && player.vel.y > -0.001f)
                oper.jump();
        }
    }
    else {
        auto           plr   = player;
        constexpr auto solve = [](float x, float v0, float a) {
            float t = v0 / a;
            return x + v0 * t + a * t * t * 0.5f;
        };

        if (player.vel.x > 0.f) {
            plr.pos.x = solve(player.pos.x, player.vel.x, player.x_slowdown + player.x_accel * 0.2f);
            if (find_platform(platforms, plr).can_stand_on == nullptr) {
                plr.pos.x = solve(player.pos.x, player.vel.x, player.x_slowdown + player.x_accel);
                if (find_platform(platforms, plr).can_stand_on == nullptr) {
                    if (player.vel.y > -0.001f)
                        oper.jump();
                }

                move_spec.update_move(ai_move_left, 100);
            }
        }
        else {
            plr.pos.x = solve(player.pos.x, player.vel.x, -(player.x_slowdown + player.x_accel * 0.2f));
            if (find_platform(platforms, plr).can_stand_on == nullptr) {
                plr.pos.x = solve(player.pos.x, player.vel.x, -player.x_slowdown - player.x_accel);
                if (find_platform(platforms, plr).can_stand_on == nullptr) {
                    if (player.vel.y > -0.001f)
                        oper.jump();
                }
                move_spec.update_move(ai_move_right, 100);
            }
        }
    }
}

inline std::vector<const ai_bullet_t*>
find_dangerous_bullets(const std::vector<ai_bullet_t>& bullets, const ai_player_t& player, float bullet_timeshift) {
    std::vector<const ai_bullet_t*> dangerous;

    for (auto& bl : bullets) {
        if (bl.pos.y > player.pos.y || bl.pos.y < player.pos.y - player.size.y)
            continue;

        if (bl.group != -1 && bl.group == player.group)
            continue;

        auto dist_x = player.pos.x - bl.pos.x;
        if ((bl.vel.x > 0.f && dist_x > 0.f) || (bl.vel.x < 0.f && dist_x < 0.f)) {
            float time = dist_x / (bl.vel.x - player.vel.x);
            if (time < std::fabs(bl.vel.x) * 0.00013f + bullet_timeshift)
                dangerous.push_back(&bl);
        }
    }

    return dangerous;
}

inline void dodge_ai(const std::vector<ai_bullet_t>& bullets,
                     const ai_player_t&              player,
                     const ai_physic_sim_t&          physic_sim,
                     ai_move_spec& /*                                  move_spec*/,
                     ai_operator_native& oper) {
    if (oper.native_difficulty() < ai_native_difficulty::medium)
        return;

    if (oper.native_difficulty() == ai_native_difficulty::medium) {
        /* 8% chance */
        bool ok = static_cast<u32>(std::fabs(player.pos.x)) % 12 != 0;
        if (ok)
            return;

        for (auto& bl : bullets) {
            if (bl.pos.y > player.pos.y || bl.pos.y < player.pos.y - player.size.y)
                continue;

            if (bl.group != -1 && bl.group == player.group)
                continue;

            auto dist_x = player.pos.x - bl.pos.x;
            if ((bl.vel.x > 0.f && dist_x > 0.f) || (bl.vel.x < 0.f && dist_x < 0.f)) {
                float time = dist_x / (bl.vel.x - player.vel.x);
                if (time < 0.2f) {
                    if (player.vel.y > -0.001f)
                        oper.jump();
                }
            }
        }
    }

    if (oper.is_hardlike()) {
        constexpr auto calc_hit_mass = [](const std::vector<const ai_bullet_t*>& bullets) {
            float hit_mass = 0;
            for (auto b : bullets) hit_mass += (b->vel.x > 0.f ? b->hit_mass : -b->hit_mass);
            return hit_mass;
        };

        auto full_mass = calc_hit_mass(find_dangerous_bullets(bullets, player, 0.f));
        if (std::fabs(full_mass) < 0.1f)
            return;

        auto afterjump_pos = player.pos + vec2f(player.vel.x, -player.jump_speed) * 0.5f + physic_sim.gravity * 0.125f;
        auto afterjump_player = player;
        afterjump_player.pos  = afterjump_pos;

        float afterjump_full_mass = calc_hit_mass(find_dangerous_bullets(bullets, afterjump_player, 0.5f));

        if (std::fabs(afterjump_full_mass) < std::fabs(full_mass)) {
            if (player.vel.y > -0.001f)
                oper.jump();
        }
    }
}

inline void hard_shooter(const std::map<player_name_t, ai_player_t>& players,
                         const ai_player_t&                          plr,
                         ai_operator_native&                                op,
                         const ai_physic_sim_t&                      physic_sim,
                         const ai_level_t&                           level,
                         ai_move_spec&) {
    if (op.is_on_shot())
        return;

    std::map<float, const ai_player_t*> targets;

    for (auto& [_, trg] : players) {
        if (trg.name == plr.name || trg.is_y_locked)
            continue;

        if (trg.group != -1 && trg.group == plr.group)
            continue;

        if (std::fabs(trg.pos.x) - level.level_size.x > falling_dist)
            continue;

        auto a = 0.5f * physic_sim.gravity.y;
        auto b = trg.vel.y;
        auto c = trg.pos.y - plr.pos.y;
        auto d = b * b - 4.f * a * c;

        if (d < 0.f)
            continue;

        auto x1 = (-b + std::sqrt(d)) / (2.f * a);
        auto x2 = (-b - std::sqrt(d)) / (2.f * a);

        float t;
        if (x1 < 0.f && x2 < 0.f)
            continue;
        else {
            if (x1 < 0.f)
                t = x2;
            else if (x2 < 0.f)
                t = x1;
            else
                t = std::min(x1, x2);
        }

        if (t < 1.f) {
            targets.emplace(t, &trg);
        }
    }

    for (auto& [t, trg] : targets) {
        /* TODO: read from gun params */

        auto trg_new_pos = trg->pos + trg->vel * t + physic_sim.gravity * t * t * 0.5f;
        auto dist        = trg_new_pos - (plr.pos - (plr.barrel_pos - plr.pos));
        auto blt_dist    = plr.gun_bullet_vel * t;
        if (((dist.x < 0.f && dist.x < blt_dist) || (dist.x > 0.f && dist.x > blt_dist)) &&
            std::fabs(std::fabs(dist.x) - blt_dist) < 50.f / std::cos(plr.gun_dispersion * 0.5f)) {
            if (trg_new_pos.x - plr.pos.x < 0.f) {
                if (!plr.on_left)
                    op.move_left();
            }
            else {
                if (plr.on_left)
                    op.move_right();
            }

            op.shot_next_frame();
        }
    }
}

/*
 * The C++ ports of the lua "insane" AI shooter (data/scripts/dfdh_ai.lua) and
 * the mod/ai.lua helpers it relies on. Used by the "sniper" native difficulty
 * instead of hard_shooter.
 */
inline float cross2z(const vec2f& lhs, const vec2f& rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

/* x == 0 (an exactly collinear point) without -Wfloat-equal */
inline bool exact_zero(float x) {
    return std::fpclassify(x) == FP_ZERO;
}

inline std::optional<vec2f> fall_on_platform(const vec2f& start_pos, const vec2f& next_pos, const ai_platform_t& platform) {
    auto c1 = next_pos - start_pos;
    auto c2 = platform.pos2 - platform.pos1;

    auto pz1 = cross2z(c1, platform.pos1 - start_pos);
    auto pz2 = cross2z(c1, platform.pos2 - start_pos);

    if ((pz1 < 0.f) == (pz2 < 0.f) || exact_zero(pz1) || exact_zero(pz2))
        return {};

    pz1 = cross2z(c2, start_pos - platform.pos1);
    pz2 = cross2z(c2, next_pos - platform.pos1);

    if ((pz1 < 0.f) == (pz2 < 0.f) || exact_zero(pz1) || exact_zero(pz2))
        return {};

    return vec2f{start_pos.x + c1.x * std::fabs(pz1) / std::fabs(pz2 - pz1),
                 start_pos.y + c1.y * std::fabs(pz1) / std::fabs(pz2 - pz1)};
}

inline std::optional<vec2f> fall_on_platforms(const vec2f& start_pos, const vec2f& next_pos, const ai_data_t& world) {
    std::optional<vec2f> result;
    float                min_y = std::numeric_limits<float>::max();

    for (auto& platform : world.platforms) {
        if (auto res = fall_on_platform(start_pos, next_pos, platform); res && res->y < min_y) {
            result = res;
            min_y  = res->y;
        }
    }

    return result;
}

inline float
get_actual_damage(const ai_player_t& plr, const ai_player_t& target, const ai_data_t& world, float player_size_coef) {
    float dmg = 0.f;
    auto  dir_sign = plr.barrel_pos.x > target.pos.x;
    auto  tpos     = target.pos + vec2f{target.size.x * 0.5f, -target.size.y * 0.5f};

    for (auto& bullet : world.bullets) {
        if (bullet.group == plr.group && dir_sign == (bullet.vel.x < 0.f)) {
            auto  bpos    = bullet.pos;
            auto  bvel    = bullet.vel;
            auto  delta_s = tpos.x - bpos.x;
            auto  delta_v = bvel.x - target.vel.x;
            auto  t       = delta_s / delta_v;

            auto  g_s         = world.physic_sim.gravity * t * t * 0.5f;
            auto  tpos_next   = tpos + target.vel * t + (target.is_y_locked ? vec2f{} : g_s);
            auto  bpos_next_y = bpos.y + bvel.y * t + (world.physic_sim.enable_gravity_for_bullets ? g_s.y : 0.f);

            /* Check for platforms */
            if (tpos_next.y > tpos.y) {
                if (auto fall_position = fall_on_platforms(tpos, tpos_next, world)) {
                    tpos_next.x = fall_position->x + target.size.x * 0.5f;
                    tpos_next.y = fall_position->y - target.size.y * 0.5f;
                }
            }

            /* Distance from meet point to target center relative to y */
            auto y_dist = std::fabs(bpos_next_y - tpos_next.y);

            /* The target becomes archievable if distance to the target center
               less than half height of the target */
            if (y_dist < (target.size.y * 0.5f) * player_size_coef)
                dmg += bullet.hit_mass * std::fabs(bullet.vel.x) * 0.001f;
        }
    }

    return dmg;
}

inline bool is_flying_outside(const ai_player_t& plr, float start_x, float end_x) {
    auto  center = (end_x - start_x) * 0.5f;
    auto  t      = std::fabs(plr.vel.x) / plr.x_accel;
    float pos;

    if (plr.pos.x + plr.size.x * 0.5f < center)
        pos = plr.pos.x + plr.size.x + plr.vel.x * t + plr.x_accel * t * t * 0.5f;
    else
        pos = plr.pos.x + plr.vel.x * t - plr.x_accel * t * t * 0.5f;

    return pos < start_x || pos > end_x;
}

inline bool is_flying_to_death(const ai_player_t& plr, const ai_data_t& world) {
    return is_flying_outside(plr, world.level.platforms_bound_start_x, world.level.platforms_bound_end_x);
}

struct find_victim_res {
    const ai_player_t*  target = nullptr;
    std::optional<float> change_dir;
    std::optional<bool>  long_shot_setting;
    bool                 too_close = false;
};

inline find_victim_res
find_victim(const ai_player_t& plr, const ai_data_t& world, float player_size_coef = 1.f, bool change_dir_allowed = true) {
    find_victim_res  res;
    float            res_dist = std::numeric_limits<float>::max();
    const bool       bullet_gravity_enabled = world.physic_sim.enable_gravity_for_bullets;

    for (auto& [_, target] : world.players) {
        if (target.group == plr.group)
            continue;
        if (is_flying_to_death(target, world))
            continue;
        if (get_actual_damage(plr, target, world, player_size_coef) >= 1.f)
            continue;

        std::optional<float> change_dir;
        auto tpos   = target.pos + vec2f{target.size.x * 0.5f, -target.size.y * 0.5f};
        auto twidth = target.size.x * 0.5f;
        auto ppos   = plr.pos + vec2f{plr.size.x * 0.5f, -plr.size.y * 0.5f};
        auto bpos   = plr.barrel_pos;

        /* Determine direction to target */
        float target_dir_x = ppos.x < tpos.x ? 1.f : -1.f;

        /* Is direction change needed */
        if (target_dir_x < 0.f && !plr.on_left)
            change_dir = -1.f;
        else if (target_dir_x > 0.f && plr.on_left)
            change_dir = 1.f;

        /* Skip if change direction is required and not allowed */
        if (change_dir && !change_dir_allowed)
            continue;

        /* Recalculate barrel position if direction change is needed */
        if (change_dir) {
            auto barrel_displacement = bpos - ppos;
            bpos = vec2f{ppos.x - barrel_displacement.x, ppos.y + barrel_displacement.y};
        }

        /* Is target too close to shot */
        bool too_close =
            (target_dir_x < 0.f && bpos.x < tpos.x - twidth) || (target_dir_x > 0.f && bpos.x > tpos.x + twidth);

        if (!too_close) {
            /* TODO: handle target acceleration (maybe it's useless) */
            std::array<vec2f, 2> dirs;
            size_t               dirs_count = 1;
            dirs[0] = {target_dir_x, 0.f};

            bool   long_shot_iter = false;
            vec2f  long_shot_dir  = plr.long_shot_dir;
            if (bullet_gravity_enabled) {
                long_shot_iter = true;
                /* Fix long_shot_dir if direction changed */
                if (change_dir)
                    long_shot_dir.x = -long_shot_dir.x;
                dirs[1]     = long_shot_dir;
                dirs_count  = 2;
            }

            for (size_t i = 0; i < dirs_count; ++i) {
                auto bdir = dirs[i];

                /* Find meet time relative to x
                   From Sbullet + Vbullet * t = Starget + Vtarget * t ->
                        t = (Starget - Sbullet) / (Vbullet - Vtarget) */
                auto  delta_s = tpos.x - bpos.x;
                auto  delta_v = bdir.x * plr.gun_bullet_vel - target.vel.x;
                auto  t       = delta_s / delta_v;
                auto  g_s     = world.physic_sim.gravity * t * t * 0.5f;

                /* Find meet y positions, pos = S + Vt + gt^2/2 */
                auto  tpos_next   = tpos + target.vel * t + (target.is_y_locked ? vec2f{} : g_s);
                auto  bpos_next_y = bpos.y + bdir.y * plr.gun_bullet_vel * t +
                                    (bullet_gravity_enabled ? g_s.y : 0.f);

                /* Check for platforms */
                if (tpos_next.y > tpos.y) {
                    if (auto fall_position = fall_on_platforms(tpos, tpos_next, world)) {
                        tpos_next.x = fall_position->x + target.size.x * 0.5f;
                        tpos_next.y = fall_position->y - target.size.y * 0.5f;
                    }
                }

                /* Distance from meet point to target center relative to y */
                auto y_dist = std::fabs(bpos_next_y - tpos_next.y);

                /* The target becomes archievable if distance to the target center
                   less than half height of the target */
                if (y_dist < (target.size.y * 0.5f) * player_size_coef) {
                    auto dist = magnitude(tpos - bpos);

                    /* Set the current target if it is closer */
                    if (dist < res_dist) {
                        res.target     = &target;
                        res_dist       = dist;
                        res.change_dir = change_dir;
                        res.too_close  = too_close;

                        if (bullet_gravity_enabled) {
                            if (plr.long_shot_enabled && !long_shot_iter)
                                res.long_shot_setting = false;
                            else if (!plr.long_shot_enabled && long_shot_iter)
                                res.long_shot_setting = true;
                        }

                        break;
                    }
                }

                long_shot_iter = false;
            }
        }
    }

    return res;
}

inline void insane_shooter(ai_operator_native& op,
                           const ai_player_t&   plr,
                           const ai_data_t&     world,
                           bool                 change_dir_allowed) {
    static constexpr float insane_shooter_relax_timeout    = 0.5f;
    static constexpr float insane_shooter_player_size_coef = 1.0f;

    auto vr = find_victim(plr, world, insane_shooter_player_size_coef, change_dir_allowed);

    bool switch_target_required = false;
    /* Switch target if it required */
    if (!vr.target && op._insane_target_name != ai_operator_native::no_target) {
        switch_target_required = true;
        op._insane_target_name = ai_operator_native::no_target;
    }
    else if (vr.target && vr.target->name != op._insane_target_name) {
        switch_target_required = true;
        op._insane_target_name = vr.target->name;
    }

    /* Enable/disable long shot mode if it required */
    if (vr.long_shot_setting)
        op.produce_action(*vr.long_shot_setting ? ai_action::enable_long_shot : ai_action::disable_long_shot);

    /* Change direction if it required */
    if (vr.change_dir) {
        if (*vr.change_dir < 0.f)
            op.move_left();
        else
            op.move_right();
        op.stop();
    }

    if (switch_target_required) {
        if (op._insane_target_name != ai_operator_native::no_target)
            op.shot();
        else {
            /* Schedule relax */
            op._insane_relax_active = true;
            op._insane_relax_timer.restart();
        }
    }

    /* Produce relax on timeout */
    if (op._insane_relax_active &&
        op._insane_relax_timer.elapsed(world.physic_sim.time_speed) >=
            insane_shooter_relax_timeout * (plr.gun_fire_rate / 1000.f)) {
        op._insane_relax_active = false;
        op.relax();
    }
}

inline auto pathfinder(const std::vector<ai_platform_t>& platforms,
                       const ai_plat_map_t&              plat_map,
                       const platform_res_t&             start_plat,
                       const platform_res_t&             target_plat) {
    std::vector<const ai_platform_t*> res;
    auto                              src_plat = start_plat.stand_on;
    auto                              trg_plat = target_plat.stand_on ? target_plat.stand_on : target_plat.nearest;

    if (!src_plat)
        src_plat = start_plat.stand_on;
    if (!src_plat)
        src_plat = start_plat.nearest;

    if (!src_plat || !trg_plat)
        return res;

    if (src_plat == trg_plat)
        return res;

    auto src_plat_idx = static_cast<size_t>(src_plat - platforms.data());
    auto trg_plat_idx = static_cast<size_t>(trg_plat - platforms.data());

    std::vector<size_t> came_from;
    std::vector<float>  cost_so_far;
    came_from.resize(platforms.size(), std::numeric_limits<size_t>::max());
    cost_so_far.resize(platforms.size());

    using pair_t = std::pair<float, size_t>;
    std::priority_queue<pair_t, std::vector<pair_t>, std::greater<>> frontier;
    frontier.push({0.0, src_plat_idx});

    came_from[src_plat_idx]   = src_plat_idx;
    cost_so_far[src_plat_idx] = 0.f;

    while (!frontier.empty()) {
        auto current = frontier.top().second;
        frontier.pop();

        if (current == trg_plat_idx)
            break;

        for (size_t i = 0; i < plat_map[current].size(); ++i) {
            auto& next     = plat_map[current][i];
            float new_cost = cost_so_far[current] + magnitude2(next);

            if (came_from[i] == std::numeric_limits<size_t>::max() || new_cost < cost_so_far[i]) {
                cost_so_far[i] = new_cost;
                came_from[i]   = current;
                frontier.push({new_cost, i});
            }
        }
    }

    if (!frontier.empty()) {
        auto cur = trg_plat_idx;
        while (cur != src_plat_idx) {
            res.push_back(platforms.data() + cur);
            cur = came_from[cur];
        }

        std::reverse(res.begin(), res.end());
    }

    return res;
}

inline void move_to_target(const std::vector<ai_platform_t>& platforms,
                           const ai_plat_map_t&              plat_map,
                           const ai_player_t&                player,
                           ai_operator_native&                      oper,
                           ai_move_spec&                     move_spec,
                           const ai_player_t&                target,
                           const platform_res_t&             target_plat,
                           float                             plats_bound_start,
                           float                             plats_bound_end) {
    auto operated_plats = oper.operated_platforms();
    if (!operated_plats.stand_on)
        operated_plats.stand_on = oper.last_stand_on_plat();

    auto path = pathfinder(platforms, plat_map, operated_plats, target_plat);

    if (path.empty())
        return;

    auto& next_plat = path.front();

    bool ready_to_jump = false;

    float plat_center;
    float plat_min_dist;

    if (path.size() == 1 && oper.is_hardlike() && next_plat->pos1.x < target.pos.x &&
        target.pos.x < next_plat->pos2.x) {
        auto  trg_plat_f    = inverse_lerp(next_plat->pos1.x, next_plat->pos2.x, target.pos.x);
        auto  global_plat_f = inverse_lerp(plats_bound_start, plats_bound_end, target.pos.x);
        float f;
        if (global_plat_f <= 0.5f)
            f = lerp(trg_plat_f, 1.f, 0.5f);
        else
            f = lerp(0.f, trg_plat_f, 0.5f);
        plat_center   = (next_plat->pos2.x - next_plat->pos1.x) * f;
        plat_min_dist = std::min(plat_center - next_plat->pos1.x, next_plat->pos2.x - plat_center) * 0.1f;
    }
    else {
        plat_center   = (next_plat->pos1.x + next_plat->pos2.x) * 0.5f;
        plat_min_dist = (next_plat->pos2.x - next_plat->pos1.x) * 0.1f;
    }

    if (plat_center + plat_min_dist < player.pos.x) {
        move_spec.update_move(ai_move_left, 0);
        ready_to_jump = next_plat->pos2.x > player.pos.x || player.pos.x - next_plat->pos2.x < oper.jump_x_max();
    }
    else if (plat_center - plat_min_dist > player.pos.x) {
        move_spec.update_move(ai_move_right, 0);
        ready_to_jump = next_plat->pos1.x < player.pos.x || next_plat->pos1.x - player.pos.x < oper.jump_x_max();
    }
    else {
        move_spec.update_move(ai_move_stop, 0);
        ready_to_jump = true;
    }

    if (next_plat->pos1.y < player.pos.y) {
        if (ready_to_jump && player.vel.y > -0.001f)
            oper.jump();
    }
    else {
        if (player.is_y_locked)
            oper.jump_down();
    }

    /*
    std::cout << "Path: ";
    for (auto p : path) {
        std::cout << p - platforms.data() << " ";
    }
    std::cout << std::endl;
    */
}

inline std::optional<player_name_t> find_closest_target(const std::map<player_name_t, ai_player_t>& players,
                                                        const ai_player_t&                          player) {
    std::optional<player_name_t> closest;
    float                        dist = std::numeric_limits<float>::max();

    for (auto& [_, trg] : players) {
        if (&player != &trg && (trg.group == -1 || trg.group != player.group)) {
            auto cur_dist = magnitude2(trg.pos - player.pos);
            if (cur_dist < dist) {
                closest = trg.name;
                dist    = cur_dist;
            }
        }
    }

    return closest;
}

inline void stay_away_from_borders(const ai_player_t& player, ai_move_spec& move_spec, float start, float end) {
    if (player.pos.x < start || player.pos.x > end)
        return;

    auto plat_factor = inverse_lerp(start, end, player.pos.x);
    if (plat_factor < 0.2f) {
        move_spec.update_move(ai_move_right, 1);
    }
    else if (plat_factor > 0.8f) {
        move_spec.update_move(ai_move_left, 1);
    }
}

inline void ai_operator_native::work(const ai_data_t& ai_data) {
    auto found = ai_data.players.find(player_name());
    if (found == ai_data.players.end())
        return;
    auto& player = found->second;

    auto last_stand_on  = _operated_platforms.stand_on;
    _operated_platforms = find_platform(ai_data.platforms, player);
    if (_operated_platforms.stand_on != last_stand_on)
        _last_stand_on_plat = last_stand_on;

    _jump_x_max = ((player.jump_speed * 2.f) / ai_data.physic_sim.gravity.y) * player.max_vel_x * 0.7f;



    auto         plat   = _operated_platforms;
    ai_move_spec move_spec;

    update_shot_next_frame(ai_data.physic_sim);

    /* The targets may be gone (removed or eliminated players) */
    if (!ai_data.players.contains(_target_id))
        _target_id = ai_operator_native::no_target;
    if (!ai_data.players.contains(_last_target_id))
        _last_target_id = ai_operator_native::no_target;

    if (_target_id == ai_operator_native::no_target) {
        if (auto id = easy_ai_find_nearest(player, ai_data.players, ai_data.level, *this))
            _target_id = id->first;
    }
    else {
        if (is_hardlike()) {
            if (!hard_i_see_you(player, ai_data.players.at(_target_id), ai_data.level, player.gun_dispersion)) {
                if (auto next_target = find_closest_target(ai_data.players, player))
                    _last_target_id = *next_target;
                else
                    _last_target_id = ai_operator_native::no_target;

                _target_id = ai_operator_native::no_target;
                if (auto id = easy_ai_find_nearest(player, ai_data.players, ai_data.level, *this))
                    _target_id = id->first;
            }
        }
        else {
            if (!i_see_you(player, ai_data.players.at(_target_id))) {
                if (auto next_target = find_closest_target(ai_data.players, player))
                    _last_target_id = *next_target;
                _target_id = ai_operator_native::no_target;
            }
        }
    }

    easy_ai_platform_actions(ai_data.platforms, plat, player, *this, move_spec);

    if (_target_id != ai_operator_native::no_target) {
        if (!_shot) {
            float delay = 0.f;
            if (_difficulty == ai_native_difficulty::easy)
                delay = 0.35f; // rand_float(0.15f, 0.4f);
            else if (_difficulty == ai_native_difficulty::medium)
                delay = 0.15f; // rand_float(0.05f, 0.15f);
            shot_next_frame(delay);
        }

        auto& trg        = ai_data.players.at(_target_id);
        auto  dist       = trg.pos - player.pos;
        auto  mod_dist_x = fabs(dist.x);

        std::array<float, 3> dists; // NOLINT
        if (!is_hardlike()) {
            dists[0] = std::fabs(player.barrel_pos.x - player.pos.x) * 1.3f;
            dists[1] = dists[0] * 0.8f;
            dists[2] = 500.f;
        }
        else {
            dists[0] = std::fabs(player.barrel_pos.x - player.pos.x);
            dists[1] = dists[0];
            dists[2] = 40.f;
        }

        if (mod_dist_x < dists[0]) {
            if (_difficulty >= ai_native_difficulty::hard && player.vel.y > -0.001f)
                jump();
            /* Move from target */
            if (dist.x >= 0.f && !_mov_left)
                move_spec.update_move(ai_move_left, 0);
            else if (dist.x < 0.f && !_mov_right)
                move_spec.update_move(ai_move_right, 0);
        }
        else if (mod_dist_x >= dists[1] && mod_dist_x < dists[2]) {
            if (dist.x >= 0.f && player.on_left)
                move_spec.update_move(ai_move_right, 0);
            else if (dist.x < 0.f && !player.on_left)
                move_spec.update_move(ai_move_left, 0);

            if (_mov_right || _mov_left)
                move_spec.update_apply(*this, ai_move_stop, 0);
        }
        else {
            /* Move to target */
            if (dist.x >= 0.f && !_mov_right)
                move_spec.update_move(ai_move_right, 0);
            else if (dist.x < 0.f && !_mov_left)
                move_spec.update_move(ai_move_left, 0);
        }

        if (is_hardlike() && trg.vel.y < -trg.jump_speed * 0.99f &&
            player.vel.y > -0.001f /* && roll_the_dice(0.5f)*/) {
            /* For 50% chance */
            if (static_cast<int>(std::fabs(player.pos.x)) % 2 == 0)
                delayed_jump(0.15f /*rand_float(0.05f, 0.25f)*/);
        }
    }
    else {
        if (_last_target_id != ai_operator_native::no_target) {
            auto& trg         = ai_data.players.at(_last_target_id);
            auto  target_plat = find_platform(ai_data.platforms, trg);

            if (is_hardlike() && player.pos.y > trg.pos.y &&
                (player.pos.y - trg.pos.y) < player.max_jump_dist * float(player.available_jumps) &&
                player.pos.y > -0.001f) {
                jump();
            }
            else if (!_shot) {
                move_to_target(ai_data.platforms,
                               ai_data.platform_map,
                               player,
                               *this,
                               move_spec,
                               ai_data.players.at(_last_target_id),
                               target_plat,
                               ai_data.level.platforms_bound_start_x,
                               ai_data.level.platforms_bound_end_x);
            }
        }

        /* Relax with delay */
        float relax_delay = player.gun_fire_rate * 0.001f;
        if (_shot && _timer.elapsed(ai_data.physic_sim.time_speed) > relax_delay) {
            if (_shot)
                relax();
        }

        if (is_hardlike()) {
            if (_difficulty == ai_native_difficulty::sniper)
                insane_shooter(*this, player, ai_data, plat.stand_on != nullptr || plat.can_stand_on != nullptr);
            else
                hard_shooter(ai_data.players, player, *this, ai_data.physic_sim, ai_data.level, move_spec);
        }
    }

    if (std::fabs(player.vel.x) > player.max_vel_x * 3.f && plat.stand_on && player.vel.y > -0.001f)
        jump();

    dodge_ai(ai_data.bullets, player, ai_data.physic_sim, move_spec, *this);

    move_spec.apply(*this);
}

inline void ai_mgr_singleton::worker_op() {
    for (auto& [oper_name, oper_ptr] : _operators) {
        auto prof = _prof.scope(oper_name);
        oper_ptr->work(_data);
    }
    if (_prof.print_ready()) {
        {
            std::lock_guard lock{_prof_mtx};
            _ts_prof = _prof;
        }
        _prof.reset();
        _prof.reset_timer();
    }
}

} // namespace dfdh
