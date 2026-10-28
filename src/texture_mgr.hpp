#pragma once

#include <map>
#include <string>
#include <filesystem>

#include <SFML/Graphics/Texture.hpp>

#include "base/log.hpp"

namespace dfdh {

class texture_mgr_singleton {
public:
    static texture_mgr_singleton& instance() {
        static texture_mgr_singleton inst;
        return inst;
    }

    sf::Texture& load(const std::string& path) {
        auto p = (std::filesystem::current_path() / "data/textures" / path).string();
        auto [pos, was_insert] = _textures.emplace(p, sf::Texture());
        if (was_insert) {
            if (!pos->second.loadFromFile(p)) {
                glog().error("Cannot load texture {}", p);
                _textures.erase(pos);
                return _textures.emplace("!!/dummy/!!", sf::Texture()).first->second;
            }
            pos->second.setSmooth(true);
        }
        return pos->second;
    }

    texture_mgr_singleton(const texture_mgr_singleton&) = delete;
    texture_mgr_singleton& operator=(const texture_mgr_singleton&) = delete;

private:
    texture_mgr_singleton() = default;
    ~texture_mgr_singleton() = default;

    std::map<std::string, sf::Texture> _textures;
};

inline texture_mgr_singleton& texture_mgr() {
    return texture_mgr_singleton::instance();
}

}
