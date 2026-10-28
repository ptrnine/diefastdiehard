#pragma once
/*
 * The set of SFML drawables an effect can consist of. Adopted from fever_dream's
 * src/grx/sfml_types.hpp, adapted to SFML 3 (no sf::Text: it is no longer default
 * constructible, and dfdh::sprite_t replaces the default-constructible sf::Sprite).
 */

#include <variant>

#include <SFML/Graphics/CircleShape.hpp>
#include <SFML/Graphics/ConvexShape.hpp>
#include <SFML/Graphics/RectangleShape.hpp>

#include "sprite.hpp"

namespace dfdh::efx {
using drawable_t = std::variant<sprite_t, sf::CircleShape, sf::ConvexShape, sf::RectangleShape>;

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

/*
 * Runs the handler on the actual alternative of the variant (when it accepts it).
 */
template <typename Drawable, typename... Ts>
bool downcast(Drawable&& obj, auto&& handler, Ts&&... args) {
    return std::visit(
        [&, ...args = std::forward<Ts>(args)]<typename T>(T&& obj) mutable {
            if constexpr (requires { handler(std::forward<T>(obj), std::forward<Ts>(args)...); }) {
                handler(std::forward<T>(obj), std::forward<Ts>(args)...);
                return true;
            }
            return false;
        },
        std::forward<Drawable>(obj));
}
} // namespace dfdh::efx
