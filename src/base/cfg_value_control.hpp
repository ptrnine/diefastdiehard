#pragma once

#include <SFML/Window/Event.hpp>

#include "cfg.hpp"
#include "log.hpp"
#include "ston.hpp"

namespace dfdh {

class cfg_value_control {
public:
    using str_value_t = std::array<std::optional<std::string>, 3>;
    using value_t     = std::array<std::optional<float>, 3>;

    cfg_value_control(std::string isection, std::string ikey, value_t isteps):
        section(std::move(isection)), key(std::move(ikey)), steps(isteps) {}

    void handle_event(const sf::Event& evt) {
        if (auto pressed = evt.getIf<sf::Event::KeyPressed>()) {
            switch (pressed->code) {
            case sf::Keyboard::Key::Z:
                if (steps[0])
                    update(0, -*steps[0]);
                break;
            case sf::Keyboard::Key::X:
                if (steps[0])
                    update(0, *steps[0]);
                break;
            case sf::Keyboard::Key::C:
                if (steps[1])
                    update(1, -*steps[1]);
                break;
            case sf::Keyboard::Key::V:
                if (steps[1])
                    update(1, *steps[1]);
                break;
            case sf::Keyboard::Key::B:
                if (steps[2])
                    update(2, -*steps[2]);
                break;
            case sf::Keyboard::Key::N:
                if (steps[2])
                    update(2, *steps[2]);
                break;
            default:
                break;
            }
        }
    }

    void update(size_t idx, float step) {
        try {
            auto value_opt = cfg::mutable_global().get_section(section).try_get<std::string>(key);
            if (!value_opt) {
                glog().warn("cfg_value_control: key {} was not found in section [{}]", key, section);
                return;
            }

            u32 value_idx = 0;
            value_t values;

            for (auto str_value_view : value_opt->value() / split(' ', '\t')) {
                if (value_idx > 2){
                    glog().warn("cfg_value_control: only 3 values supported");
                    return;
                }
                auto str_value = std::string(str_value_view.begin(), str_value_view.end());
                float num;
                try {
                    num = ston<float>(str_value);
                }
                catch (...) {
                    glog().warn("cfg_value_control: key {} in section [{}] stores not a number value",
                            key, section);
                    return;
                }
                values[value_idx] = num;
                ++value_idx;
            }

            if (values[idx]) {
                *values[idx] += step;
                std::stringstream ss;
                for (auto& v : values)
                    if (v)
                        ss << *v << ' ';

                std::string new_value = ss.str();
                if (!new_value.empty() && new_value.back() == ' ')
                    new_value.pop_back();

                value_opt->set(new_value);
                cfg::mutable_global().commit();
                glog().info_update(__COUNTER__, "cfg_value_control: updated [{}]:{} = {}", section, key, new_value);
                updated = true;
            }
        }
        catch (...) {
            glog().warn("cfg_value_control: section [{}] was not found", section);
        }
    }

    std::optional<std::string> consume_update() {
        if (updated) {
            updated = false;
            return section;
        }
        return {};
    }

private:
    std::string section;
    std::string key;
    value_t     steps;

    bool updated = false;
};

}
