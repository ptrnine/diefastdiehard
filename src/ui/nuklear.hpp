#pragma once

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT

#include <map>
#include <memory>
#include <cstring>

#include <nuklear.h>
#include "nuklear_sfml_gl3.h"

#include "nuklear_binding.hpp"
#include "texture_mgr.hpp"

#define UI_NK_MAX_VERTEX_BUFFER 512 * 1024
#define UI_NK_MAX_ELEMENT_BUFFER 128 * 1024

namespace dfdh {
class ui_ctx : public ui_nuklear_base {
public:
    ui_ctx() = default;

    ui_ctx(sf::Window& window): ui_nuklear_base() {
        std::memset(&_sfml, 0, sizeof(_sfml));

        _sfml.window = &window;
        nk_sfml_init(&_sfml);
        _ctx = &_sfml.ctx;

        nk_sfml_font_stash_begin(&_sfml);

        auto cfg = nk_font_config(17);
        /*
         * The infinity sign (unlimited ammo) and the cyrillic ranges (see
         * nk_font_cyrillic_glyph_ranges). The infinity goes first: the glyph
         * lookup indexes the ranges as if they had no gaps, but the baker
         * skips the codepoints missing in the font
         */
        static const nk_rune glyph_ranges[] = {
            0x221E, 0x221E, 0x0020, 0x00FF, 0x0400, 0x052F, 0x2DE0, 0x2DFF, 0xA640, 0xA69F, 0};
        cfg.range = glyph_ranges;
        fonts.pt17 = nk_font_atlas_add_from_file(&_sfml.atlas, "data/font/RobotoMono-Regular.ttf", 17.f, &cfg);

        nk_sfml_font_stash_end(&_sfml);
        //nk_style_load_all_cursors(&_sfml.ctx, _sfml.atlas.cursors);
        nk_style_set_font(&_sfml.ctx, &fonts.pt17->handle);
    }

    ~ui_ctx() {
        if (_sfml.window)
            nk_sfml_shutdown(&_sfml);
    }

    ui_ctx(ui_ctx&& ui) noexcept: ui_nuklear_base(), _sfml(ui._sfml) {
        std::memset(&_sfml, 0, sizeof(_sfml));

        _ctx = &_sfml.ctx;
        ui._sfml.window = nullptr;
    }

    ui_ctx& operator=(ui_ctx&& ui) noexcept {
        std::memset(&_sfml, 0, sizeof(_sfml));

        _sfml = ui._sfml;
        _ctx = &_sfml.ctx;
        ui._sfml.window = nullptr;
        return *this;
    }

    ui_ctx(const ui_ctx&) = delete;
    ui_ctx& operator=(const ui_ctx&) = delete;

    void handle_event(sf::Event& evt) {
        nk_sfml_handle_event(&_sfml, &evt);
    }

    void render() {
        nk_sfml_render(
            &_sfml, NK_ANTI_ALIASING_ON, UI_NK_MAX_VERTEX_BUFFER, UI_NK_MAX_ELEMENT_BUFFER);
    }

    [[nodiscard]]
    nk_context* nk_ctx() {
        return &_sfml.ctx;
    }

private:
    nk_sfml _sfml;
    struct fonts_t {
        struct nk_font* pt16, *pt17, *pt18, *pt20, *pt22;
    } fonts = {};
};

inline struct nk_image make_nk_image(const sf::Texture& txtr) {
    struct nk_image image{};

    auto size = txtr.getSize();

    image.handle.id = static_cast<int>(txtr.getNativeHandle());
    image.w         = static_cast<u16>(size.x);
    image.h         = static_cast<u16>(size.y);
    image.region[2] = static_cast<u16>(size.x);
    image.region[3] = static_cast<u16>(size.y);

    return image;
}

inline struct nk_image load_nk_image(const std::string& path) {
    return make_nk_image(texture_mgr().load(path));
}

inline sf::Color nk_color_to_sfml(const struct nk_color& color) {
    return {color.r, color.g, color.b, color.a};
}

inline struct nk_color sfml_color_to_nk(const sf::Color& color) {
    return {color.r, color.g, color.b, color.a};
}

inline sf::Color nk_color_to_sfml(const struct nk_colorf& color) {
    auto c = nk_rgba_cf(color);
    return {c.r, c.g, c.b, c.a};
}

inline struct nk_colorf sfml_color_to_nk_float(const sf::Color& color) {
    return nk_color_cf(sfml_color_to_nk(color));
}

} // namespace dfdh
