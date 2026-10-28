#pragma once

#include <map>
#include <string>
#include <filesystem>
#include <list>

#include <SFML/Audio/SoundBuffer.hpp>
#include <SFML/Audio/Sound.hpp>

#include "base/log.hpp"
#include "base/vec2.hpp"

namespace dfdh {

namespace fs = std::filesystem;

class sound_mgr_singleton {
public:
    static constexpr size_t max_sounds    = 128;
    static constexpr float  position_coef = 0.0002f;

    static sound_mgr_singleton& instance() {
        /*
         * Intentionally leaked: destroying the OpenAL-backed sf::Sound/sf::SoundBuffer
         * objects at process exit races with the audio device/library teardown.
         */
        static sound_mgr_singleton* inst = new sound_mgr_singleton; // NOLINT
        return *inst;
    }

    sf::Sound& get(std::string path, int id) {
        auto [sound_pos, was_insert] = sounds.emplace(sound_key{path, id}, sound_data_t{});
        if (was_insert) {
            auto [sample_pos, was_insert] = samples.emplace(path, sf::SoundBuffer{});
            if (was_insert &&
                !sample_pos->second.loadFromFile((std::filesystem::current_path() / "data/sounds" / path).string())) {
                if (path == "dummy.wav")
                    throw std::runtime_error("Sound "s +
                                             (std::filesystem::current_path() / "dummy.wav does not exists").string());
                glog().error("Cannot load sound file: {}", path);
                samples.erase(sample_pos);
                sounds.erase(sound_pos);
                return get("dummy.wav", id);
            }

            if (sounds.size() == max_sounds) {
                sounds.erase(sound_use.back());
                sound_use.pop_back();
            }

            sound_use.push_front({path, id});
            sound_pos->second.sound.setBuffer(sample_pos->second);
            sound_pos->second.use_i = sound_use.begin();
        }

        auto& snd = sound_pos->second;
        if (snd.use_i != sound_use.begin())
            sound_use.splice(sound_use.begin(), sound_use, snd.use_i, std::next(snd.use_i));
        return snd.sound;
    }

    void play(const std::string& path, int id, vec2f cam_relative_position, float pitch, float volume = 100.f) {
        auto& snd = get(path, id);
        snd.setPosition({cam_relative_position.x * position_coef, cam_relative_position.y * position_coef, 0.1f});
        snd.setPitch(pitch);
        snd.setVolume(volume * (volume_level * 0.01f));
        snd.play();
    }

    sound_mgr_singleton(const sound_mgr_singleton&) = delete;
    sound_mgr_singleton& operator=(const sound_mgr_singleton&) = delete;

    float volume() const {
        return volume_level;
    }

    void volume(float value) {
        volume_level = value;
    }

private:
    sound_mgr_singleton() = default;
    ~sound_mgr_singleton() = default;

    struct sound_key {
        std::string path;
        int         id;
        auto operator<=>(const sound_key&) const = default;
    };

    struct sound_data_t {
        sf::Sound                      sound;
        std::list<sound_key>::iterator use_i;
    };

private:
    std::map<std::string, sf::SoundBuffer> samples;
    std::map<sound_key, sound_data_t>      sounds;
    std::list<sound_key>                   sound_use;
    float                                  volume_level = 100.f;
};

inline sound_mgr_singleton& sound_mgr() {
    return sound_mgr_singleton::instance();
}

}
