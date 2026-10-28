#pragma once
/*
 * Builds a dfdh::efx::efx from a JSON description.
 *
 * Effect files live in data/efx/<name>.json (efx_builder::effect_path), e.g.:
 * {
 *     "name": "test_effect",
 *     "duration": 4.0,
 *     "animations": [
 *         { "name": "scale0", "type": "scale", "apply_to": "all", "keys": [
 *             { "value": [ 1, 1 ], "time": 0, "out": [ 0.47, 1.64 ] },
 *             { "value": [ 2, 2 ], "time": 0.5, "in": [ 0.41, 0.8 ] }
 *         ] }
 *     ],
 *     "textures": { "ball": { "path": "ball.png", "smooth": true } },
 *     "templates": {
 *         "square1": { "type": "rect", "fill_color": "#ffffffff",
 *                      "position": [ 0, 0 ], "size": [ 100, 100 ],
 *                      "origin": [ 50, 50 ], "rotation": 0 },
 *         "ball": { "type": "sprite", "texture": "ball",
 *                  "source_rect": [ 0, 0, 32, 32 ], "origin": [ 16, 16 ] }
 *     },
 *     "primitives": [ { "template": "square1" }, { "template": "ball" } ]
 * }
 *
 * - animations: position/scale/rotation/opacity (keyed), velocity_random
 *   (angle/speed/acceleration) and position_random (x/y ranges). Keyed ones
 *   have keys with value/time and optional type ("hold"/"linear"/"bezier"),
 *   in/out bezier handles; scale additionally takes an optional anchor
 *   ([0-1] point of the drawable's bounds to scale around); apply_to is
 *   "all" or an array of drawable indices.
 * - texture paths are relative to data/textures (dfdh::texture_mgr).
 * - templates: sprite/circle/rect with color/fill_color/position/origin/size/
 *   radius/rotation/source_rect/texture/point_count.
 *
 * Adopted from fever_dream's src/grx/efx_editor.hpp.
 */

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "base/color.hpp"
#include "base/types.hpp"
#include "efx.hpp"
#include "sfml_types.hpp"
#include "texture_mgr.hpp"

namespace dfdh {
/* nlohmann json binding for dfdh::vec2 (ADL) */
template <typename T>
inline void from_json(const nlohmann::json& j, vec2<T>& v) {
    v = {j[0].get<T>(), j[1].get<T>()};
}
} // namespace dfdh

namespace dfdh::efx_editor
{
namespace json = nlohmann;

class efx_builder_error : public std::runtime_error {
public:
    efx_builder_error(std::string msg): std::runtime_error(std::move(msg)) {}
};

class efx_builder {
public:
    using object_t = json::json::object_t;
    using array_t  = json::json::array_t;

    efx_builder(texture_mgr_singleton& texture_manager, const std::string& effect_path): tx_mgr(&texture_manager) {
        std::ifstream ifs(effect_path);
        if (!ifs.is_open())
            throw efx_builder_error("Cannot open effect file '" + effect_path + "'");

        auto b   = std::istreambuf_iterator<char>(ifs);
        auto e   = std::istreambuf_iterator<char>();
        efx_json = json::json::parse(b, e);
    }

    /* Effects live in data/efx/<name>.json */
    static std::filesystem::path effect_path(std::string_view name) {
        namespace fs = std::filesystem;
        return fs::current_path() / "data" / "efx" / (std::string(name) + ".json");
    }

    struct build_result_t {
        std::string name;
        efx::efx effect;
    };

    build_result_t build() const {
        return build(efx_json);
    }

private:
    template <typename T>
    efx::anim_key<T> parse_key(const object_t& obj) const {
        auto time    = obj.at("time").get<float>();
        auto value_j = obj.at("value");
        auto type_p  = obj.find("type");

        auto type = efx::interpolation_not_set;
        if (type_p != obj.end()) {
            auto type_str = type_p->second.get<std::string>();
            if (type_str == "bezier")
                type = efx::interpolation_t::bezier;
            else if (type_str == "hold")
                type = efx::interpolation_t::hold;
            else if (type_str == "linear")
                type = efx::interpolation_t::linear;
            else
                throw efx_builder_error("Invalid interpolation type '" + type_str + "'");
        }

        auto in_p  = obj.find("in");
        auto out_p = obj.find("out");

        vec2f in, out;

        if (in_p != obj.end()) {
            if (type == efx::interpolation_not_set)
                type = efx::interpolation_t::bezier;
            in = in_p->second.get<vec2f>();
        }

        if (out_p != obj.end()) {
            if (type == efx::interpolation_not_set)
                type = efx::interpolation_t::bezier;
            out = out_p->second.get<vec2f>();
        }

        if (type == efx::interpolation_not_set)
            type = efx::interpolation_t::linear;

        return efx::anim_key<T>{value_j.get<T>(), time, type, in, out};
    }

    template <typename T>
    struct keys_result_t {
        efx::anim_key_sequence<T> keys;
        std::vector<efx::efx::handler_t::index_t> affected_indices;
        bool apply_to_all = true;
    };

    template <typename T>
    keys_result_t<T> parse_keys(const object_t& obj) const {
        keys_result_t<T> result;

        for (auto&& key : obj.at("keys").get<array_t>())
            result.keys.push(parse_key<T>(key));

        if (auto apply_to_p = obj.find("apply_to"); apply_to_p != obj.end()) {
            if (apply_to_p->second.is_string() && apply_to_p->second.get<std::string>() == "all")
                result.apply_to_all = true;
            else if (apply_to_p->second.is_array()) {
                result.affected_indices = apply_to_p->second.get<std::vector<efx::efx::handler_t::index_t>>();
                result.apply_to_all = false;
            }
        }

        return result;
    }

    bool parse_set_source_rect(const object_t& obj, auto& drawable) const {
        if (auto value = obj.find("source_rect"); value != obj.end()) {
            auto rect = value->second.get<std::array<int, 4>>();
            drawable.setTextureRect(sf::IntRect({rect[0], rect[1]}, {rect[2], rect[3]}));
            return true;
        }
        return false;
    }

    bool
    parse_set_texture(const object_t& obj, auto& drawable, const std::map<std::string, sf::Texture*>& textures) const {
        if (auto value = obj.find("texture"); value != obj.end()) {
            auto name      = value->second.get<std::string>();
            auto texture_p = textures.find(name);

            if (texture_p == textures.end())
                throw efx_builder_error("Cannot find texture '" + name + "'");

            if constexpr (std::is_same_v<std::decay_t<decltype(drawable)>, sprite_t>)
                drawable.setTexture(*texture_p->second);
            else
                drawable.setTexture(texture_p->second);

            return true;
        }
        return false;
    }

#define DEF_PARSE_SET(what, sfml_method, cast)                                                                         \
    bool parse_set_##what(const object_t& obj, auto& drawable) const {                                                 \
        if (auto value = obj.find(#what); value != obj.end()) {                                                        \
            drawable.sfml_method(cast);                                                                                \
            return true;                                                                                               \
        }                                                                                                              \
        return false;                                                                                                  \
    }

    DEF_PARSE_SET(color, setColor, rgba_t::from(value->second.get<std::string>()))
    DEF_PARSE_SET(fill_color, setFillColor, rgba_t::from(value->second.get<std::string>()))
    DEF_PARSE_SET(position, setPosition, value->second.get<vec2f>())
    DEF_PARSE_SET(size, setSize, value->second.get<vec2f>())
    DEF_PARSE_SET(origin, setOrigin, value->second.get<vec2f>())
    DEF_PARSE_SET(radius, setRadius, value->second.get<float>())
    DEF_PARSE_SET(rotation, setRotation, sf::degrees(value->second.get<float>()))
    DEF_PARSE_SET(point_count, setPointCount, value->second.get<unsigned>())

#undef DEF_PARSE_SET

    efx::drawable_t parse_template(const object_t& obj, const std::map<std::string, sf::Texture*>& textures) const {
        auto type = obj.at("type").get<std::string>();

        if (type == "sprite") {
            sprite_t drawable;
            parse_set_texture(obj, drawable, textures);
            parse_set_color(obj, drawable);
            parse_set_source_rect(obj, drawable);
            parse_set_position(obj, drawable);
            parse_set_origin(obj, drawable);
            parse_set_rotation(obj, drawable);
            return drawable;
        }
        else if (type == "circle") {
            sf::CircleShape drawable;
            parse_set_texture(obj, drawable, textures);
            parse_set_fill_color(obj, drawable);
            parse_set_position(obj, drawable);
            parse_set_origin(obj, drawable);
            parse_set_radius(obj, drawable);
            parse_set_rotation(obj, drawable);
            parse_set_point_count(obj, drawable);
            return drawable;
        }
        else if (type == "rect") {
            sf::RectangleShape drawable;
            parse_set_texture(obj, drawable, textures);
            parse_set_fill_color(obj, drawable);
            parse_set_position(obj, drawable);
            parse_set_size(obj, drawable);
            parse_set_origin(obj, drawable);
            parse_set_rotation(obj, drawable);
            return drawable;
        }
        else {
            throw efx_builder_error("Invalid template type: '" + type + "'");
        }
    }

    std::map<std::string, sf::Texture*> parse_textures(const object_t& obj) const {
        std::map<std::string, sf::Texture*> textures;

        if (obj.contains("textures")) {
            for (auto&& [texture_name, texture_j] : obj.at("textures").get<object_t>()) {
                auto tx_obj     = texture_j.get<object_t>();
                auto path       = tx_obj.at("path").get<std::string>();
                auto smooth_p   = tx_obj.find("smooth");
                auto srgb_p     = tx_obj.find("srgb");
                auto repeated_p = tx_obj.find("repeated");

                bool smooth   = true;
                bool srgb     = false;
                bool repeated = false;

                if (smooth_p != tx_obj.end())
                    smooth = smooth_p->second.get<bool>();
                if (srgb_p != tx_obj.end())
                    srgb = srgb_p->second.get<bool>();
                if (repeated_p != tx_obj.end())
                    repeated = repeated_p->second.get<bool>();

                textures.emplace(texture_name, &tx_mgr->load(texture_def{path, smooth, srgb, repeated}));
            }
        }

        return textures;
    }

    build_result_t build(const object_t& obj) const {
        build_result_t result;

        result.name = obj.at("name").get<std::string>();
        result.effect.set_duration(obj.at("duration").get<float>());

        /*
         * Parse animations
         */
        for (auto&& anim_j : obj.at("animations").get<array_t>()) {
            auto anim_obj = anim_j.get<object_t>();
            auto name     = anim_obj.at("name").get<std::string>();
            auto type     = anim_obj.at("type").get<std::string>();

            /* TODO: refactor */
            if (type == "position") {
                auto keys_result = parse_keys<vec2f>(anim_obj);
                auto& handler    = result.effect.add_handler(name, efx::efx_handlers::position(keys_result.keys));
                if (keys_result.apply_to_all)
                    handler.set_affects_all(true);
                else
                    handler.set_affected_indices(keys_result.affected_indices);
            }
            else if (type == "scale") {
                auto keys_result = parse_keys<vec2f>(anim_obj);
                vec2f anchor{0.5f, 0.5f};
                if (auto anchor_p = anim_obj.find("anchor"); anchor_p != anim_obj.end())
                    anchor = anchor_p->second.get<vec2f>();
                auto& handler = result.effect.add_handler(name, efx::efx_handlers::scale(keys_result.keys, anchor));
                if (keys_result.apply_to_all)
                    handler.set_affects_all(true);
                else
                    handler.set_affected_indices(keys_result.affected_indices);
            }
            else if (type == "rotation") {
                auto keys_result = parse_keys<float>(anim_obj);
                auto& handler    = result.effect.add_handler(name, efx::efx_handlers::rotation(keys_result.keys));
                if (keys_result.apply_to_all)
                    handler.set_affects_all(true);
                else
                    handler.set_affected_indices(keys_result.affected_indices);
            }
            else if (type == "position_random") {
                auto x_range = anim_obj.at("x").get<std::array<float, 2>>();
                auto y_range = anim_obj.at("y").get<std::array<float, 2>>();
                auto& handler = result.effect.add_handler(name, efx::efx_handlers::position_random(x_range, y_range));

                if (auto apply_to_p = anim_obj.find("apply_to"); apply_to_p != anim_obj.end()) {
                    if (apply_to_p->second.is_string() && apply_to_p->second.get<std::string>() == "all")
                        handler.set_affects_all(true);
                    else
                        handler.set_affected_indices(
                            apply_to_p->second.get<std::vector<efx::efx::handler_t::index_t>>());
                }
            }
            else if (type == "opacity") {
                auto keys_result = parse_keys<float>(anim_obj);
                auto& handler    = result.effect.add_handler(name, efx::efx_handlers::opacity(keys_result.keys));
                if (keys_result.apply_to_all)
                    handler.set_affects_all(true);
                else
                    handler.set_affected_indices(keys_result.affected_indices);
            }
            else if (type == "velocity_random") {
                auto  angle = anim_obj.at("angle").get<std::array<float, 2>>();
                auto  speed = anim_obj.at("speed").get<std::array<float, 2>>();
                vec2f acceleration{};
                if (auto p = anim_obj.find("acceleration"); p != anim_obj.end())
                    acceleration = p->second.get<vec2f>();

                auto& handler = result.effect.add_handler(
                    name, efx::efx_handlers::velocity_random(angle[0], angle[1], speed[0], speed[1], acceleration));

                if (auto apply_to_p = anim_obj.find("apply_to"); apply_to_p != anim_obj.end()) {
                    if (apply_to_p->second.is_string() && apply_to_p->second.get<std::string>() == "all")
                        handler.set_affects_all(true);
                    else
                        handler.set_affected_indices(
                            apply_to_p->second.get<std::vector<efx::efx::handler_t::index_t>>());
                }
            }
            else {
                throw efx_builder_error("Invalid animation type '" + type + "'");
            }
        }

        /*
         * Parse textures
         */
        auto textures = parse_textures(obj);

        /*
         * Parse templates
         */
        std::map<std::string, efx::drawable_t> templates;
        for (auto&& [template_name, template_j] : obj.at("templates").get<object_t>())
            templates.emplace(template_name, parse_template(template_j.get<object_t>(), textures));

        /* Parse primitives */
        for (auto&& prim_j : obj.at("primitives").get<array_t>()) {
            auto prim_obj      = prim_j.get<object_t>();
            auto template_name = prim_obj.at("template").get<std::string>();
            auto template_p    = templates.find(template_name);
            if (template_p == templates.end())
                throw efx_builder_error("Cannot find template '" + template_name + "'");

            size_t count = 1;
            auto count_p = prim_obj.find("count");
            if (count_p != prim_obj.end()) {
                count = count_p->second.get<size_t>();
            }

            // TODO: optimize this
            for (size_t i = 0; i < count; ++i) {
                result.effect.push_element(template_p->second);
            }
        }

        return result;
    }

private:
    texture_mgr_singleton* tx_mgr;
    json::json             efx_json;
};
} // namespace dfdh::efx_editor
