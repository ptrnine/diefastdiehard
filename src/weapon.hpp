#pragma once

#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Sprite.hpp>
#include <utility>

#include "base/rand_pool.hpp"
#include "base/cfg.hpp"
#include "base/log.hpp"
#include "bullet.hpp"
#include "texture_mgr.hpp"
#include "physic/instant_kick.hpp"
#include "sound_mgr.hpp"

namespace dfdh
{
struct weapon_anim_frame {
    enum interpl_type {
        linear = 0,
        quadratic,
        square
    };

    static interpl_type interpl_from_str(const std::string& str) {
        if (str == "linear")
            return linear;
        if (str == "quadratic")
            return quadratic;
        if (str == "square")
            return square;
        throw std::runtime_error("Unknown interpolation type: " + str);
    }

    struct key_t {
        vec2f position = {0.f, 0.f};
        vec2f scale    = {1.f, 1.f};
        float angle    = 0.f;
    };

    std::vector<key_t>         _layers;
    interpl_type               _intrpl;
    float                      _time;
};

struct weapon_anim_sound_key {
    std::string sound;
    u32         frame;
    float       time;
};

struct weapon_anim {
    std::vector<weapon_anim_frame>     _frames;
    std::vector<weapon_anim_sound_key> _sounds;
    float                              _duration;
};

class weapon {
public:
    friend class weapon_instance;

    enum weapon_class {
        pistol = 0,
        rifle,
        shotgun
    };

    static std::vector<std::string> get_pistol_sections() {
        std::vector<std::string> result;

        for (auto& [sect_name, sect] : cfg::global().get_sections()) {
            if (sect_name.starts_with("wpn_")) {
                auto wpn_class = static_cast<const cfg_section<true>&>(sect).try_get<std::string>("class");
                if (wpn_class /* && wpn_class->second == "pistol"*/)
                    result.push_back(sect_name);
            }
        }

        return result;
    }

    static weapon_class weapon_class_from_str(const std::string& str) {
        if (str == "pistol")
            return pistol;
        else if (str == "rifle")
            return rifle;
        else if (str == "shotgun")
            return shotgun;
        throw std::runtime_error("Invalid weapon class: " + str);
    }

    static float tier_to_bullet_vel(u32 tier) {
        switch (tier) {
            case 0: return 1200.f;
            case 1: return 1500.f;
            case 2: return 1800.f;
            case 3: return 2100.f;
            case 4: return 10000.f;
            default: return 1500.f;
        }
    }

    void cfg_set() {
        auto& sect = cfg::global().get_section(cfg_section_name{_section});

        _hit_mass        = sect.value<float>("hit_mass");
        _dispersion      = sect.value<float>("dispersion");
        _fire_rate       = sect.value<float>("fire_rate");
        _recoil          = sect.value<float>("recoil");
        _buckshot        = sect.value<u32>("buckshot");
        _mag_size        = sect.value<u32>("mag_size");
        _size            = sect.value<vec2f>("size");
        _arm_joint       = sect.value<vec2f>("arm_joint");
        _arm_bone        = sect.value<u32>("arm_bone");
        _arm2_bone       = sect.value<u32>("arm2_bone");
        _arm_idle_pos    = sect.value<vec2f>("arm_idle_pos");
        _arm2_idle_pos   = sect.value<vec2f>("arm2_idle_pos");
        _arm_pos_f       = sect.value<vec2f>("arm_pos_f");
        _barrel          = sect.value<vec2f>("barrel");
        _shot_flash      = sect.value_or_default("shot_flash", false);
        _eject_shell     = sect.value_or_default("eject_shell", false);
        _wpn_class       = weapon_class_from_str(sect.value<std::string>("class"));
        _bullet_vel_tier = sect.value<u32>("bullet_velocity_tier");
        _long_shot_angle = sect.value<float>("long_shot_angle");
        _shot_snd_path   = sect.value_or_default("shot_sound", "none"s);
        _mass            = sect.value_or_default("mass", 0.5f);

        /* Dropped gun parameters */
        _dropped_bb          = sect.value_or_default<vec2f>("dropped_bb", _size);
        _drop_velocity       = sect.value_or_default("drop_velocity", 0.f);
        _drop_angle_velocity = sect.value_or_default("drop_angle_velocity", 0.f);

        if (_eject_shell) {
            _shell_pos   = sect.value<vec2f>("shell_pos");
            _shell_dir   = normalize(sect.get<vec2f>("shell_dir").value());
            _shell_frame = sect.value<u32>("shell_frame");
            auto& txtr   = texture_mgr().load(sect.get<std::string>("shell_txtr").value());
            _shell_sprite = sf::Sprite(txtr);
            _shell_sprite.setOrigin(float(txtr.getSize().x) * 0.5f, float(txtr.getSize().y) * 0.5f);
            auto sz = sect.value<vec2f>("shell_size");
            _shell_sprite.setScale(sz.x / float(txtr.getSize().x), sz.y / float(txtr.getSize().y));
            _shell_vel = sect.value<float>("shell_vel");
        }
    }

    void reload_layers() {
        decltype(_layers) new_layers;

        int i = 0;
        while (auto layer_txtr = cfg::global()
                                     .get_section(cfg_section_name(_section))
                                     .try_get<std::string>("layer" + std::to_string(i))) {
            if (!layer_txtr->has_value()) {
                new_layers.push_back(sf::Sprite());
                ++i;
                continue;
            }

            auto& txtr = texture_mgr().load(layer_txtr->value());
            sf::Sprite sprite;
            sprite.setTexture(txtr);
            auto txtr_sz = txtr.getSize();
            _xf = _size.x / float(txtr_sz.x);
            _yf = _size.y / float(txtr_sz.y);

            sprite.setOrigin(_arm_joint.x / _xf, _arm_joint.y / _yf);
            sprite.setScale(_xf, _yf);

            new_layers.push_back(std::move(sprite));

            ++i;
        }

        if (new_layers.empty())
            throw cfg_exception("Weapon " + _section + " has no layers");

        auto new_animations = load_animations(new_layers, _section);

        _layers     = std::move(new_layers);
        _animations = std::move(new_animations);
    }

    weapon(std::string section): _section(std::move(section)) {
        cfg_set();
        reload_layers();
    }

     [[nodiscard]]
    vec2f arm_position_factors(bool lefty) const {
            return lefty ? vec2f{1.f - _arm_pos_f.x, _arm_pos_f.y} : _arm_pos_f;
    }

    vec2f long_shot_dir(const vec2f& dir) const {
        auto shot_angle = dir.x < 0.f ? -_long_shot_angle : _long_shot_angle;
        shot_angle *= M_PIf32 / 180.f;
        return rotate_vec(dir, shot_angle);
    }

public:
    void draw(const vec2f& start_pos, bool left_dir, float shot_angle_degree, sf::RenderTarget& wnd, float scale = 1.f) {
        float invert = left_dir ? -1.f : 1.f;
        shot_angle_degree *= invert;
        for (auto& sprite : _layers) {
            sprite.setPosition(start_pos);
            sprite.setScale(_xf * invert * scale, _yf * scale);
            sprite.setRotation(shot_angle_degree);
            wnd.draw(sprite);
        }
    }

private:
    static std::map<std::string, weapon_anim> load_animations(const std::vector<sf::Sprite>& layers,
                                                              const std::string&             section) {
        decltype(_animations) new_animations;

        auto anims = cfg::global().get_section(section).get<std::vector<std::string>>("animations").value();

        if (anims.empty())
            throw cfg_exception("Weapon " + section + " must have at least one animation");

        for (auto& anim : anims)
            new_animations.emplace(anim, load_anim(layers, section + "_" += anim));

        return new_animations;
    }

    static weapon_anim load_anim(const std::vector<sf::Sprite>& layers, const std::string& section) {
        auto sect = cfg::global().get_section(section);

        auto frames = sect.value<u32>("frames");

        std::vector<weapon_anim_frame>     _animation;
        std::vector<weapon_anim_sound_key> sounds;

        auto duration = sect.value<float>("duration");

        for (u32 frame = 0; frame < frames; ++frame) {
            auto frame_str = std::to_string(frame);
            weapon_anim_frame fr;
            fr._time   = sect.value<float>(frame_str + "_time");
            fr._intrpl = weapon_anim_frame::interpl_from_str(sect.value<std::string>(frame_str + "_intrpl"));
            if (auto snd = sect.value_or_default(frame_str + "_sound", std::optional<std::string>{}))
                sounds.push_back(weapon_anim_sound_key{*snd, frame, fr._time});

            for (size_t layer = 0; layer < layers.size(); ++layer) {
                auto layer_str = "_layer" + std::to_string(layer) + "_";
                auto pos_key   = sect.value_or_default<vec2f>(frame_str + layer_str + "pos", {0.f, 0.0001f});
                auto scale_key = sect.value_or_default<vec2f>(frame_str + layer_str + "scale", {1.f, 1.f});
                auto rot_key   = sect.value_or_default<float>(frame_str + layer_str + "rot", 0.f);

                fr._layers.push_back(weapon_anim_frame::key_t{pos_key, scale_key, rot_key});
            }

            _animation.push_back(std::move(fr));
        }
        return {std::move(_animation), sounds, duration};
    }

private:
    std::string _section;

    float _hit_mass;
    float _dispersion;
    float _fire_rate;
    float _recoil;
    u32   _buckshot;
    u32   _mag_size;
    u32   _arm_bone;
    u32   _arm2_bone;
    u32   _bullet_vel_tier;
    float _long_shot_angle;

    float _xf, _yf;

    vec2f      _arm_joint;
    vec2f      _arm_pos_f;
    vec2f      _size;
    vec2f      _arm_idle_pos;
    vec2f      _arm2_idle_pos;
    vec2f      _barrel;
    vec2f      _shell_pos;
    vec2f      _shell_dir;
    float      _shell_vel;
    u32        _shell_frame;
    sf::Sprite _shell_sprite;
    float      _mass;

    vec2f _dropped_bb;
    float _drop_velocity       = 0.f;
    float _drop_angle_velocity = 0.f;

    std::string _shot_snd_path;

    weapon_class _wpn_class;
    bool         _shot_flash;
    bool         _eject_shell;

    std::vector<sf::Sprite> _layers;
    std::map<std::string, weapon_anim> _animations;

public:
    [[nodiscard]]
    float hit_power() const {
        return _hit_mass * bullet_scalar_velocity() * 0.001f;
    }

    [[nodiscard]]
    float fire_rate() const {
        return _fire_rate;
    }

    [[nodiscard]]
    float recoil() const {
        return _recoil;
    }

    [[nodiscard]]
    float accuracy() const {
        constexpr float pi_4 = 0.7853981633974483f; // M_PI_4f32 as plain float (no _Float32 deduction clash)
        auto v = (pi_4 - std::min(_dispersion, pi_4)) / pi_4;
        return v * v;
    }

    [[nodiscard]]
    float bullet_scalar_velocity() const {
        return tier_to_bullet_vel(_bullet_vel_tier);
    }

    const std::vector<sf::Sprite>& layers() const {
        return _layers;
    }

    [[nodiscard]]
    const vec2f& arm_idle_pos() const {
        return _arm_idle_pos;
    }

    [[nodiscard]]
    const vec2f& arm2_idle_pos() const {
        return _arm2_idle_pos;
    }

    [[nodiscard]]
    const vec2f& barrel_pos() const {
        return _barrel;
    }

    [[nodiscard]]
    weapon_class wpn_class() const {
        return _wpn_class;
    }

    [[nodiscard]]
    u32 mag_size() const {
        return _mag_size;
    }

    [[nodiscard]]
    float get_dispersion() const {
        return _dispersion;
    }

    [[nodiscard]]
    float get_fire_rate() const {
        return _fire_rate;
    }

    [[nodiscard]]
    const std::string& section() const {
        return _section;
    }

    [[nodiscard]]
    float long_shot_angle() const {
        return _long_shot_angle;
    }

    [[nodiscard]]
    const vec2f& dropped_bb() const {
        return _dropped_bb;
    }

    [[nodiscard]]
    float drop_velocity() const {
        return _drop_velocity;
    }

    [[nodiscard]]
    float drop_angle_velocity() const {
        return _drop_angle_velocity;
    }

    [[nodiscard]]
    float mass() const {
        return _mass;
    }
};


class weapon_storage_singleton {
public:
    static weapon_storage_singleton& instance() {
        static weapon_storage_singleton inst;
        return inst;
    }

    static bool is_exists(const std::string& wpn_section_name) {
        if (!wpn_section_name.starts_with("wpn_"))
            return false;

        auto sect = cfg::global().try_get_section(wpn_section_name);
        if (sect) {
            auto wpn_class = sect->try_get<std::string>("class");
            if (wpn_class && *wpn_class)
                return true;
        }

        return false;
    }

    weapon& load(const std::string& section) {
        auto found = _wpns.find(section);
        if (found != _wpns.end())
            return found->second;
        return _wpns.emplace(section, weapon(section)).first->second;
    }

    void reload() {
        for (auto& [_, wpn] : _wpns) {
            wpn.cfg_set();
            wpn.reload_layers();
        }
    }

    void reload(std::string wpn_section) {
        auto found = _wpns.find(wpn_section);
        if (found != _wpns.end()) {
            found->second.cfg_set();
            found->second.reload_layers();
            return;
        }

        /* Animation section changed: reload the layers (and animations) of its weapon */
        for (std::string_view anim_suffix : {"_reload", "_shot", "_load_start", "_load_end", "_shell"}) {
            if (wpn_section.ends_with(anim_suffix)) {
                wpn_section.resize(wpn_section.size() - anim_suffix.size());
                break;
            }
        }

        found = _wpns.find(wpn_section);
        if (found != _wpns.end())
            found->second.reload_layers();
        /* TODO: log if section not found? */
    }

    weapon_storage_singleton(const weapon_storage_singleton&) = delete;
    weapon_storage_singleton& operator=(const weapon_storage_singleton&) = delete;

private:
    weapon_storage_singleton() = default;
    ~weapon_storage_singleton() = default;

    std::map<std::string, weapon> _wpns;
};

inline weapon_storage_singleton& weapon_mgr() {
    return weapon_storage_singleton::instance();
}


class weapon_instance {
public:
    static constexpr vec2f shot_flash_scale = {55.f, 55.f};

    struct remote_shot_params_t {
        instant_kick_mgr* kick_mgr;
        float             lastency;
    };

    struct anim_spec_t {
        std::string _name;
        timer   _timer = {};
        bool        _stop_at_end = true;
    };

    weapon_instance() = default;
    weapon_instance(weapon* wpn): _ammo_elapsed(wpn->mag_size()), _wpn(wpn) {
        if (_wpn->_shot_flash) {
            auto& txtr = texture_mgr().load("wpn/shot.png");
            auto  txtr_sz = txtr.getSize();
            auto  scale   = vec2f{shot_flash_scale.x / float(txtr_sz.x), shot_flash_scale.y / float(txtr_sz.y)};

            _shot_flash.setTexture(txtr);
            _shot_flash.setOrigin({float(txtr.getSize().x) * 0.5f, float(txtr.getSize().y) * 0.5f});
            _shot_flash.setScale(scale.x, scale.y);
            _shot_flash.setColor({255, 255, 255, 0});
            _shot_flash_intensity = 0.f;
        }
    }

    weapon_instance(const std::string& section): weapon_instance(&weapon_mgr().load(section)) {}

    [[nodiscard]]
    weapon* wpn() {
        return _wpn;
    }

    [[nodiscard]]
    bool empty() const {
        return _wpn == nullptr;
    }

    vec2f arm_position_factors(bool lefty, const vec2f& bobbing = {0.f, 0.f}) const {
        if (_wpn)
            return _wpn->arm_position_factors(lefty) + bobbing * _wpn->_mass;
        else
            return (lefty ? vec2f(0.f, -0.4f) : vec2f(1.f, -0.4f)) + bobbing * 0.01f;
    }

    operator bool() const {
        return !empty();
    }

    void play_animation(const std::string& name) {
        if (!_wpn)
            return;

        auto found_anim = _wpn->_animations.find(name);
        if (found_anim == _wpn->_animations.end()) {
            std::cerr << "Animation " << name << " not found" << std::endl;
            return;
        }

        _current_anim = anim_spec_t{name, {}};
        _anim_played_sounds.clear();
    }

    template <typename F = int, typename F2 = void (*)(const vec2f&, const vec2f&, float)>
    std::optional<float> update(const vec2f&       position,
                                const vec2f&       cam_position,
                                const vec2f&       direction,
                                bool               enable_long_shot,
                                bullet_mgr&        bm,
                                physic_simulation& sim,
                                bool               gravity_for_bullets   = false,
                                bool               spawn_bullet          = true,
                                F2&&               bullet_spawn_callback = nullptr,
                                int                group                 = -1,
                                F                  player_group_getter   = -1,
                                rand_float_pool*   rand_pool             = nullptr) {
        if (!_wpn)
            return {};

        if (_ammo_elapsed == 0 && !_current_anim && !_on_reload) {
            _on_reload = true;
            if (_wpn->_wpn_class == weapon::shotgun)
                play_animation("load_start");
            else
                play_animation("reload");
            return {};
        }

        /*
         * Fire rate in simulation time, updated once per tick. A network player's
         * weapon counts its applied input ticks instead (the server can tick
         * without a new input while waiting for one), so the shots land on the
         * same input ticks on the server and in the client's prediction
         */
        if (_input_tick) {
            auto ticks       = _counted_input_tick ? *_input_tick - *_counted_input_tick : 1u;
            _counted_input_tick = _input_tick;
            _since_shot += sim.last_timestep() * float(ticks);
        }
        else
            _since_shot += sim.last_timestep();
        if (_on_shot && _ammo_elapsed && _since_shot > 60.f / _wpn->_fire_rate) {
            shot(position,
                 cam_position,
                 direction,
                 enable_long_shot,
                 bm,
                 sim,
                 _last_tracer_color,
                 gravity_for_bullets,
                 spawn_bullet,
                 std::forward<F2>(bullet_spawn_callback),
                 std::is_same_v<F, int> ? -1 : group,
                 std::move(player_group_getter),
                 rand_pool);
            _on_reload = false;
            _since_shot = 0.f;
            return _wpn->_recoil;
        }

        if (_on_reload && !_current_anim) {
            _ammo_elapsed = _wpn->mag_size();
            _on_reload = false;
        }

        if (_anim_sound_to_play) {
            sound_mgr().play(*_anim_sound_to_play, group, position - cam_position, sim.last_speed());
            _anim_sound_to_play.reset();
        }

        return {};
    }

    /* Returs false if trigger has been already held down */
    bool pull_trigger(std::optional<sf::Color> tracer_color = std::nullopt) {
        if (tracer_color)
            _last_tracer_color = *tracer_color;

        return !std::exchange(_on_shot, true);
    }

    /* Returns false if trigger has been already relaxed */
    bool relax_trigger() {
        return std::exchange(_on_shot, false);
    }

    /**
     * Deterministic shots (network play): from now on the dispersion of shot
     * number n is generated from (base, n), so the client prediction and the
     * server fire the very same directions. Called on every trigger pull with
     * the input tick of the pull
     */
    void seed_shots(u32 base) {
        _seed_base  = base;
        _shot_index = 0;
    }

    [[nodiscard]]
    const std::optional<u32>& seed_base() const {
        return _seed_base;
    }

    /* Network input tick currently applied to the owner (its fire rate counts input ticks) */
    void input_tick(u32 seq) {
        _input_tick = seq;
    }

    /* Index of the shot being fired (inside the bullet spawn callback) or of the next one */
    [[nodiscard]]
    u32 shot_index() const {
        return _shot_index;
    }

    std::array<vec2f, 2> draw(const vec2f&      position,
                              bool              left_dir,
                              bool              enable_long_shot,
                              sf::RenderWindow& wnd,
                              const vec2f&      shell_additional_vel = {0.f, 0.f}) {
        float                 LF       = left_dir ? -1.f : 1.f;
        auto shot_angle_deg = enable_long_shot ? _wpn->_long_shot_angle : 0.f;
        auto shot_angle_rad = shot_angle_deg * M_PIf32 / 180.f;

        static constexpr auto leftyfix = [](const vec2f& v, float invert) {
            return vec2f(v.x * invert, v.y);
        };

        static constexpr auto make_return = [](const vec2f& pos, weapon* wpn, float LF, float rotate_angle) {
            return std::array<vec2f, 2>{pos + leftyfix(wpn->_arm_idle_pos, LF),
                                        pos + leftyfix(wpn->_arm_idle_pos, LF) + rotate_vec(leftyfix(wpn->_arm2_idle_pos, LF), rotate_angle)};
        };

        auto rotvec = [angl = shot_angle_rad * LF](const vec2f& v) {
            return rotate_vec(v, angl);
        };

        if (auto c = _shot_flash.getColor(); _wpn->_shot_flash && c.a != 0) {
            _shot_flash.setPosition(position + rotvec(shot_displacement(vec2f(left_dir ? -1.f : 1.f, 0.f))));
            wnd.draw(_shot_flash);
            _shot_flash_intensity -= _shot_flash_timer.restart().asSeconds() * 18.f;
            if (_shot_flash_intensity < 0.f)
                _shot_flash_intensity = 0.f;

            c.a = static_cast<u8>(_shot_flash_intensity * 255.f);
            _shot_flash.setColor(c);
        }

        if (!_current_anim) {
            _wpn->draw(position, left_dir, shot_angle_deg, wnd);
            draw_shells(wnd);
            return make_return(position, _wpn, LF, shot_angle_rad * LF);
        }

        weapon_anim* anim;
        std::vector<weapon_anim_frame>* frames;
        float max_frame;
        float cur_frame_time;

        for (bool repeat = true; repeat;) {
            repeat = false;

            auto found_anim = _wpn->_animations.find(_current_anim->_name);
            if (found_anim == _wpn->_animations.end()) {
                glog().error("Animation '{}' not found in weapon [{}]", _current_anim->_name, _wpn->_section);
                _ammo_elapsed = _wpn->_mag_size;
                if (_current_anim->_name == "shell")
                    _current_anim = anim_spec_t{"load_end"};
                else
                    _current_anim.reset();
                return make_return(position, _wpn, LF, shot_angle_rad * LF);
            }
            anim = &found_anim->second;

            frames = &anim->_frames;

            if (frames->empty()) {
                _current_anim.reset();
                draw(position, left_dir, enable_long_shot, wnd);
                return make_return(position, _wpn, LF, shot_angle_rad * LF);
            }

            auto  dur      = anim->_duration;
            float time     = _current_anim->_timer.elapsed(_last_time_speed);
            max_frame      = frames->back()._time;
            cur_frame_time = max_frame / dur * time;

            if (cur_frame_time > max_frame && _current_anim->_stop_at_end) {
                if (_current_anim->_name == "load_start") {
                    _current_anim = anim_spec_t{"shell"};
                    repeat = true;
                }
                else if (_current_anim->_name == "shell") {
                    ++_ammo_elapsed;
                    if (_ammo_elapsed == _wpn->_mag_size)
                        _current_anim = anim_spec_t{"load_end"};
                    else
                        _current_anim = anim_spec_t{"shell"};
                    repeat = true;
                }
                else {
                    _current_anim.reset();
                    draw(position, left_dir, enable_long_shot, wnd);
                    return make_return(position, _wpn, LF, shot_angle_rad * LF);
                }
            }
        }

        cur_frame_time = std::fmod(cur_frame_time, max_frame);

        auto beg = frames->begin();
        auto end = frames->begin() + 1;
        for (auto i = frames->begin(); i != frames->end() - 1; ++i) {
            if (i->_time <= cur_frame_time && std::next(i)->_time >= cur_frame_time) {
                beg = i;
                end = i + 1;
                break;
            }
        }

        /* Play sound */
        {
            auto frame_i = u32(beg - frames->begin());
            for (size_t i = 0; i < anim->_sounds.size(); ++i) {
                auto& snd = anim->_sounds[i];
                if (snd.frame == frame_i && snd.time <= cur_frame_time &&
                    _anim_played_sounds.emplace(played_sound_info{i, frame_i}).second) {
                    _anim_sound_to_play = snd.sound;
                    break;
                }
            }
        }


        if (_wpn->_eject_shell && !_shell_ejected && _current_anim->_name == "shot" &&
            beg - frames->begin() == _wpn->_shell_frame) {
            _shell_ejected = true;

            auto vel = _wpn->_shell_vel + rand_float(-_wpn->_shell_vel * 0.1f, _wpn->_shell_vel * 0.1f);
            auto dir = left_dir ? vec2f{-_wpn->_shell_dir.x, _wpn->_shell_dir.y} : _wpn->_shell_dir;

            _active_shells.push_back(shell_data{
                vel * dir + shell_additional_vel,
                position + rotvec(shell_displacement(left_dir)),
                rand_float(-40.f, 40.f),
                rand_float(-360.f, 360.f),
                left_dir});
        }

        constexpr auto interpl = [](auto v1, auto v2, float f, weapon_anim_frame::interpl_type it) {
            switch (it) {
                case weapon_anim_frame::linear:
                    return lerp(v1, v2, f);
                case weapon_anim_frame::quadratic:
                    return lerp(v1, v2, f * f);
                case weapon_anim_frame::square:
                    return lerp(v1, v2, std::sqrt(f));
            }
            throw std::runtime_error("WTF");
        };

        float factor = inverse_lerp(beg->_time, end->_time, cur_frame_time);
        std::vector<weapon_anim_frame::key_t> keys;
        keys.reserve(beg->_layers.size());
        for (u32 i = 0; i < beg->_layers.size(); ++i) {
            auto& b = beg->_layers[i];
            auto& e = end->_layers[i];
            keys.push_back({interpl(b.position, e.position, factor, end->_intrpl),
                            interpl(b.scale, e.scale, factor, end->_intrpl),
                            interpl(b.angle, e.angle, factor, end->_intrpl)});
        }

        auto& layers = _wpn->_layers;
        auto& main_layer_keys = keys[_wpn->_arm_bone];
        auto xf = _wpn->_xf;
        auto yf = _wpn->_yf;
        for (u32 i = 0; i < layers.size(); ++i) {
            if (i == _wpn->_arm_bone) {
                layers[i].setPosition(position + leftyfix(main_layer_keys.position, LF));
                layers[i].setScale(
                    {LF * xf * main_layer_keys.scale.x, yf * main_layer_keys.scale.y});
                layers[i].setRotation(main_layer_keys.angle * LF + shot_angle_deg * LF);
            }
            else {
                auto addition = i == _wpn->_arm2_bone ? _wpn->_arm2_idle_pos : vec2f(0.f, 0.f);
                layers[i].setPosition(position + leftyfix(main_layer_keys.position, LF) +
                                      rotate_vec(leftyfix(keys[i].position, LF) + leftyfix(addition, LF),
                                                 M_PIf32 * main_layer_keys.angle * LF / 180.f + shot_angle_rad * LF));
                /* TODO: fix scale with rotations */
                layers[i].setScale({LF * xf * main_layer_keys.scale.x * keys[i].scale.x,
                                    yf * main_layer_keys.scale.y * keys[i].scale.y});
                layers[i].setRotation(main_layer_keys.angle * LF + keys[i].angle * LF + shot_angle_deg * LF);
            }
            wnd.draw(layers[i]);
        }

        draw_shells(wnd);

        return {layers[_wpn->_arm_bone].getPosition(), layers[_wpn->_arm2_bone].getPosition()};
    }

    [[nodiscard]]
    vec2f shot_displacement(const vec2f& direction) const {
        return direction.x < 0.f ? vec2f(-_wpn->_barrel.x, _wpn->_barrel.y) : _wpn->_barrel;
    }

private:
    void draw_shells(sf::RenderWindow& wnd) {
        auto& sprite = _wpn->_shell_sprite;
        for (auto i = _active_shells.begin(); i != _active_shells.end();) {
            auto elapsed_t = i->_timer.elapsed(_last_time_speed);
            if (elapsed_t > 5.f) {
                _active_shells.erase(i++);
                continue;
            }

            sprite.setPosition(i->_pos);
            sprite.setRotation(i->_angle + (i->_left ? 180.f : 0.f));
            wnd.draw(sprite);

            float timestep = elapsed_t - i->_last_time;
            i->_last_time = elapsed_t;
            i->_vel += _last_gravity * timestep;
            i->_pos += i->_vel * timestep;
            i->_angle = std::fmod(i->_angle + i->_angle_vel * timestep, 360.f);

            ++i;
        }
    }

    [[nodiscard]]
    vec2f shell_displacement(bool on_left) {
        return on_left ? vec2f(-_wpn->_shell_pos.x, _wpn->_shell_pos.y) : _wpn->_shell_pos;
    }

    template <typename F = int, typename F2 = void (*)(const vec2f&, const vec2f&, float)>
    void shot(const vec2f&       position,
              const vec2f&       cam_position,
              vec2f              direction,
              bool               enable_long_shot,
              bullet_mgr&        bm,
              physic_simulation& sim,
              sf::Color          tracer_color,
              bool               gravity_for_bullets   = false,
              bool               spawn_bullet          = true,
              F2&&               bullet_spawn_callback = nullptr,
              int                group                 = -1,
              F                  player_group_getter   = -1,
              rand_float_pool*   rand_pool             = nullptr) {
        auto shot_angle =
            enable_long_shot ? (direction.x < 0.f ? -_wpn->_long_shot_angle : _wpn->_long_shot_angle) : 0.f;
        shot_angle *= M_PIf32 / 180.f;

        auto pos   = position + rotate_vec(shot_displacement(direction), shot_angle);
        direction  = normalize(rotate_vec(direction, shot_angle));
        auto angl  = _wpn->_buckshot == 1 ? _wpn->_dispersion : _wpn->_dispersion * 0.5f;

        std::optional<std::mt19937> seeded;
        if (_seed_base)
            seeded.emplace(*_seed_base * 2654435761u ^ (_shot_index + 1) * 2246822519u);

        auto rand_dir = [rand_pool, &seeded](const vec2f& dir, float angle) {
            if (seeded)
                return randomize_dir(dir, angle, [&seeded](float min, float max) {
                    return min + (max - min) * float(double((*seeded)()) / double(std::mt19937::max()));
                });
            if (!rand_pool)
                return randomize_dir(dir, angle);
            return randomize_dir(dir, angle, [rand_pool](float min, float max) { return rand_pool->gen(min, max); });
        };

        auto bullet_vel = weapon::tier_to_bullet_vel(_wpn->_bullet_vel_tier);
        auto spawn      = [&](const vec2f& dir) {
            if (bullet_spawn_callback)
                bullet_spawn_callback(pos, dir * bullet_vel, _wpn->_hit_mass);

            if (spawn_bullet) {
                if constexpr (std::is_same_v<F, int>)
                    bm.shot(sim, pos, _wpn->_hit_mass, dir * bullet_vel, tracer_color, gravity_for_bullets);
                else
                    bm.shot(sim,
                            pos,
                            _wpn->_hit_mass,
                            dir * bullet_vel,
                            tracer_color,
                            gravity_for_bullets,
                            group,
                            player_group_getter);
            }
        };

        auto dir = rand_dir(direction, angl);
        if (_wpn->_buckshot == 1)
            spawn(dir);
        else
            for (u32 i = 0; i < _wpn->_buckshot; ++i)
                spawn(rand_dir(dir, _wpn->_dispersion));

        ++_shot_index;
        play_animation("shot");

        if (_wpn->_shot_flash) {
            _shot_flash.setRotation(rand_float(0.f, 360.f));
            _shot_flash.setColor({255, 255, 255, 255});
            _shot_flash_intensity = 1.f;
            _shot_flash_timer.restart();
            _shot_flash.setPosition(pos);
        }

        _last_gravity    = sim.gravity();
        _last_time_speed = sim.last_speed();
        _shell_ejected = false;

        --_ammo_elapsed;
        sound_mgr().play(_wpn->_shot_snd_path, group, position - cam_position, _last_time_speed);
    }

    struct shell_data {
        vec2f _vel;
        vec2f _pos;
        float _angle;
        float _angle_vel;
        bool  _left;
        float _last_time = 0.f;
        timer _timer     = {};
    };

private:
    u32                        _ammo_elapsed = 0;
    weapon*                    _wpn          = nullptr;
    std::optional<anim_spec_t> _current_anim;

    sf::Color                  _last_tracer_color = {255, 255, 255};

    sf::Sprite _shot_flash;
    sf::Clock  _shot_flash_timer;
    float      _shot_flash_intensity = 0.f;

    std::list<shell_data> _active_shells;
    bool                  _shell_ejected   = false;
    vec2f                 _last_gravity    = {0.f, 0.f};
    float                 _last_time_speed = 1.f;

    bool  _on_shot   = false;
    bool  _on_reload = false;
    float _since_shot = std::numeric_limits<float>::max(); /* simulation seconds since the last shot */

    std::optional<u32> _input_tick;
    std::optional<u32> _counted_input_tick;

    std::optional<u32> _seed_base;
    u32                _shot_index = 0;

    struct played_sound_info {
        size_t sound_index;
        u32    frame;
        auto operator<=>(const played_sound_info&) const = default;
    };
    std::set<played_sound_info> _anim_played_sounds;
    std::optional<std::string>  _anim_sound_to_play;

public:
    [[nodiscard]]
    bool on_shot() const {
        return _on_shot;
    }

    [[nodiscard]]
    const weapon* get_weapon() const {
        return _wpn;
    }

    [[nodiscard]]
    float get_bullet_vel() const {
        return weapon::tier_to_bullet_vel(_wpn->_bullet_vel_tier);
    }

    [[nodiscard]]
    u32 ammo_elapsed() const {
        return _ammo_elapsed;
    }

    void ammo_elapsed(u32 value) {
        _ammo_elapsed = value;
    }
};

} // namespace dfdh
