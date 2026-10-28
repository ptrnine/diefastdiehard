#pragma once

#include "usertypes.hpp"
#include <luaffi/luacpp_ctx.hpp>

namespace dfdh
{
inline void lua_sfml_event_init(luacpp::luactx& ctx) {
    ctx.annotate({.explicit_type = "string"});
    ctx.annotate({.explicit_type = "string"});
    ctx.set_member_table(luacpp::member_table<sf::Event>{
        {"type",
         {[](const sf::Event& e, luacpp::luactx& ctx) { ctx.push(sfml_event_type_to_str(e)); },
          [](sf::Event& e, luacpp::luactx& ctx) {
              std::string event_type;
              ctx.get_new(event_type);
              if (auto event = sfml_str_to_event(event_type))
                  e = *event;
          }}},
        {"keycode",
         {[](const sf::Event& e, luacpp::luactx& ctx) {
              if (auto pressed = e.getIf<sf::Event::KeyPressed>())
                  ctx.push(sfml_key_to_str(pressed->code));
              else if (auto released = e.getIf<sf::Event::KeyReleased>())
                  ctx.push(sfml_key_to_str(released->code));
              else
                  ctx.push(std::string{});
          },
          [](sf::Event& e, luacpp::luactx& ctx) {
              std::string key;
              ctx.get_new(key);
              if (auto pressed = e.getIf<sf::Event::KeyPressed>())
                  pressed->code = sfml_str_to_key(key);
              else if (auto released = e.getIf<sf::Event::KeyReleased>())
                  released->code = sfml_str_to_key(key);
          }}},
    });
}
} // namespace dfdh
