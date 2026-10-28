#pragma once

#include <map>
#include <string>
#include <filesystem>

#include <SFML/Graphics/Texture.hpp>

#include "base/log.hpp"

namespace dfdh {

struct texture_def {
    std::string path;
    bool smooth = true;
    bool srgb = false;
    bool repeated = false;

    auto operator<=>(const texture_def&) const = default;
};

class texture_mgr_singleton {
public:
    static texture_mgr_singleton& instance() {
        /*
         * Intentionally leaked: destroying the Texture map at process exit runs
         * sf::Texture destructors after the window/GL context is already gone,
         * which crashes in the GL driver (on Windows, inside gdi32full.dll).
         */
        static texture_mgr_singleton* inst = new texture_mgr_singleton; // NOLINT
        return *inst;
    }

    sf::Texture& load(const texture_def& def) {
        auto p = (std::filesystem::current_path() / "data/textures" / def.path).string();
        auto [pos, was_insert] = _textures.emplace(def, sf::Texture());
        if (was_insert) {
            if (!pos->second.loadFromFile(p, def.srgb)) {
                glog().error("Cannot load texture {}", p);
                _textures.erase(pos);
                return _textures.emplace(texture_def{"!!/dummy/!!"}, sf::Texture()).first->second;
            }
            pos->second.setSmooth(def.smooth);
            pos->second.setRepeated(def.repeated);
        }
        return pos->second;
    }

    sf::Texture& load(const std::string& path) {
        return load(texture_def{path});
    }

    texture_mgr_singleton(const texture_mgr_singleton&) = delete;
    texture_mgr_singleton& operator=(const texture_mgr_singleton&) = delete;

private:
    texture_mgr_singleton() = default;
    ~texture_mgr_singleton() = default;

    std::map<texture_def, sf::Texture> _textures;
};

inline texture_mgr_singleton& texture_mgr() {
    return texture_mgr_singleton::instance();
}

}
