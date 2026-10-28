#pragma once

#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>

namespace dfdh {

/*
 * sf::Sprite with the SFML 2 behavior: default constructible (no texture, draws nothing), and the
 * first texture set resets the texture rect to the texture's size
 */
class sprite_t : public sf::Sprite {
public:
    sprite_t(): sf::Sprite(no_texture()) {}
    sprite_t(const sf::Texture& texture): sf::Sprite(texture) {}

    void setTexture(const sf::Texture& texture, bool reset_rect = false) {
        sf::Sprite::setTexture(texture, reset_rect || !has_texture());
    }

    [[nodiscard]]
    bool has_texture() const {
        return &getTexture() != &no_texture();
    }

private:
    static const sf::Texture& no_texture() {
        static const sf::Texture texture;
        return texture;
    }
};

} // namespace dfdh
