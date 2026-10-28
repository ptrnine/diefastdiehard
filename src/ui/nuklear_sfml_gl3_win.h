/*
 * Windows variant of nuklear_sfml_gl3.h (selected on _WIN32).
 *
 * Renders Nuklear's draw list through SFML's own draw path (sf::Window::draw
 * with sf::Vertex triangles) instead of issuing GL calls directly. The
 * target machine's driver (Intel GL-on-DX bridge, reports GL 4.6) corrupts
 * client-side vertex-array state shared between consumers: any
 * glEnableClientState/glVertexPointer/glDrawArrays sequence from the UI, no
 * matter how carefully saved/restored, breaks SFML's world draws whenever
 * the world has many draws per frame (e.g. while bullets/casings render).
 * SFML's own machinery is rock solid on this driver, so the UI reuses it:
 * Nuklear vertices are converted to sf::Vertex and drawn exactly like the
 * world's sprites (same vertex cache, same client-array discipline). The UI
 * draws in window-pixel coordinates, so the render switches the window view
 * to a full-window identity view for the duration of the draw.
 *
 * SFML texture coordinates are in pixels (Nuklear's are normalized), and a
 * Nuklear clip rect becomes a view whose viewport is that rect.
 *
 * Texture handles are sf::Texture pointers (make_nk_image), except the font
 * atlas: NK_SFML_FONT_TEX_ID (the atlas lives in the nk_sfml, which is copied
 * around by ui_ctx, so its address is not stable). A pointer tagged with
 * nk_sfml_flipped_texture_handle is sampled in raw OpenGL row order, like the
 * GL backend does: SFML corrects render textures' bottom-up rows, and the
 * game draws its UI render textures upside down to compensate the GL backend.
 */
#ifndef NK_SFML_GL3_WIN_H_
#define NK_SFML_GL3_WIN_H_

#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/Vertex.hpp>
#include <SFML/Window.hpp>
#include <SFML/Graphics/View.hpp>
#include <SFML/Graphics/RenderStates.hpp>
#include <SFML/Graphics/PrimitiveType.hpp>
#include <SFML/Graphics/RenderWindow.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

/* nk_handle id of the font atlas (any other handle is a sf::Texture*) */
static constexpr int NK_SFML_FONT_TEX_ID = 1;

inline void* nk_sfml_flipped_texture_handle(const sf::Texture* texture) {
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(texture) | 1); // NOLINT
}

struct nk_sfml_device {
    struct nk_buffer cmds;
    struct nk_draw_null_texture tex_null;
    /* Shared, not copied, when ui_ctx copies the nk_sfml */
    std::shared_ptr<sf::Texture> font_tex; /* baked font atlas */
    std::vector<sf::Vertex> verts;         /* reused between frames */
};
struct nk_sfml_vertex {
    float position[2];
    float uv[2];
    nk_byte col[4];
};
struct nk_sfml {
    sf::Window* window;
    struct nk_sfml_device ogl;
    struct nk_context ctx;
    struct nk_font_atlas atlas;
};

NK_API struct nk_context*   nk_sfml_init(nk_sfml* sfml);
NK_API void                 nk_sfml_font_stash_begin(nk_sfml* sfml);
NK_API void                 nk_sfml_font_stash_end(nk_sfml* sfml);
NK_API int                  nk_sfml_handle_event(nk_sfml* sfml, sf::Event* event);
NK_API void                 nk_sfml_render(nk_sfml* sfml, enum nk_anti_aliasing, int max_vertex_buffer, int max_element_buffer);
NK_API void                 nk_sfml_shutdown(nk_sfml* sfml);

NK_API void                 nk_sfml_device_create(nk_sfml* sfml);
NK_API void                 nk_sfml_device_destroy(nk_sfml* sfml);

#endif
/*
 * ==============================================================
 *
 *                          IMPLEMENTATION
 *
 * ===============================================================
 */
 #ifdef NK_SFML_GL3_IMPLEMENTATION

NK_API void
nk_sfml_device_create(nk_sfml* sfml)
{
    (void)sfml;
}

NK_API void
nk_sfml_device_destroy(nk_sfml* sfml)
{
    sfml->ogl.font_tex.reset();
    nk_buffer_free(&sfml->ogl.cmds);
}

NK_INTERN void
nk_sfml_device_upload_atlas(nk_sfml* sfml, const void* image, int width, int height)
{
    struct nk_sfml_device* dev = &sfml->ogl;
    dev->font_tex = std::make_shared<sf::Texture>();
    dev->font_tex->create(static_cast<unsigned>(width), static_cast<unsigned>(height));
    dev->font_tex->update(static_cast<const sf::Uint8*>(image));
    dev->font_tex->setSmooth(true);
}

NK_API void
nk_sfml_render(nk_sfml* sfml, enum nk_anti_aliasing AA, int /*max_vertex_buffer*/, int /*max_element_buffer*/)
{
    struct nk_sfml_device* dev = &sfml->ogl;
    /* The window is always the engine's sf::RenderWindow */
    auto rt = static_cast<sf::RenderWindow*>(sfml->window);
    auto saved_view = rt->getView();
    auto wnd_w = static_cast<float>(rt->getSize().x);
    auto wnd_h = static_cast<float>(rt->getSize().y);

    /* convert from command queue into draw list */
    struct nk_convert_config config;
    static const struct nk_draw_vertex_layout_element vertex_layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_sfml_vertex, position)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_sfml_vertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct nk_sfml_vertex, col)},
        {NK_VERTEX_LAYOUT_END}
    };
    NK_MEMSET(&config, 0, sizeof(config));
    config.vertex_layout = vertex_layout;
    config.vertex_size = sizeof(struct nk_sfml_vertex);
    config.vertex_alignment = NK_ALIGNOF(struct nk_sfml_vertex);
    config.tex_null = dev->tex_null;
    config.circle_segment_count = 22;
    config.curve_segment_count = 22;
    config.arc_segment_count = 22;
    config.global_alpha = 1.0f;
    config.shape_AA = AA;
    config.line_AA = AA;

    struct nk_buffer vbuf, ebuf;
    nk_buffer_init_default(&vbuf);
    nk_buffer_init_default(&ebuf);
    nk_convert(&sfml->ctx, &dev->cmds, &vbuf, &ebuf, &config);

    auto vertices = static_cast<const nk_sfml_vertex*>(nk_buffer_memory_const(&vbuf));
    auto elements = static_cast<const nk_draw_index*>(nk_buffer_memory_const(&ebuf));

    /* execute each draw command through SFML's own draw path */
    const struct nk_draw_command* cmd;
    nk_draw_foreach(cmd, &sfml->ctx, &dev->cmds)
    {
        auto elems = elements;
        elements += cmd->elem_count;
        if (!cmd->elem_count)
            continue;

        auto handle = reinterpret_cast<std::uintptr_t>(cmd->texture.ptr);
        bool flipped = handle != NK_SFML_FONT_TEX_ID && (handle & 1);
        const sf::Texture* texture = handle == NK_SFML_FONT_TEX_ID
            ? dev->font_tex.get()
            : reinterpret_cast<const sf::Texture*>(handle & ~std::uintptr_t(1)); // NOLINT
        if (!texture)
            continue;

        /* scissor: a view showing the clip rect in the same window rect */
        auto x0 = std::max(cmd->clip_rect.x, 0.f);
        auto y0 = std::max(cmd->clip_rect.y, 0.f);
        auto x1 = std::min(cmd->clip_rect.x + cmd->clip_rect.w, wnd_w);
        auto y1 = std::min(cmd->clip_rect.y + cmd->clip_rect.h, wnd_h);
        if (x1 <= x0 || y1 <= y0)
            continue;
        sf::View view(sf::FloatRect(x0, y0, x1 - x0, y1 - y0));
        view.setViewport(sf::FloatRect(x0 / wnd_w, y0 / wnd_h, (x1 - x0) / wnd_w, (y1 - y0) / wnd_h));
        rt->setView(view);

        /* de-index; SFML texture coordinates are in pixels */
        auto tex_size = sf::Vector2f(texture->getSize());
        dev->verts.resize(cmd->elem_count);
        for (unsigned i = 0; i < cmd->elem_count; ++i) {
            auto& v = vertices[elems[i]];
            auto& o = dev->verts[i];
            o.position  = sf::Vector2f(v.position[0], v.position[1]);
            o.color     = sf::Color(v.col[0], v.col[1], v.col[2], v.col[3]);
            o.texCoords = sf::Vector2f(v.uv[0] * tex_size.x, (flipped ? 1.f - v.uv[1] : v.uv[1]) * tex_size.y);
        }

        sf::RenderStates rs;
        rs.texture = texture;
        rt->draw(dev->verts.data(), dev->verts.size(), sf::Triangles, rs);
    }

    nk_clear(&sfml->ctx);
    nk_buffer_clear(&dev->cmds);
    nk_buffer_free(&vbuf);
    nk_buffer_free(&ebuf);

    rt->setView(saved_view);
}

static void
nk_sfml_clipboard_paste(nk_handle usr, struct nk_text_edit* edit)
{
    (void)usr;
    (void)edit;
}

static void
nk_sfml_clipboard_copy(nk_handle usr, const char* text, int len)
{
    (void)usr;
    (void)text;
    (void)len;
}

NK_API struct nk_context*
nk_sfml_init(nk_sfml* sfml)
{
    nk_init_default(&sfml->ctx, nullptr);
    sfml->ctx.clip.copy = nk_sfml_clipboard_copy;
    sfml->ctx.clip.paste = nk_sfml_clipboard_paste;
    sfml->ctx.clip.userdata = nk_handle_ptr(0);
    nk_buffer_init_default(&sfml->ogl.cmds);
    nk_sfml_device_create(sfml);
    return &sfml->ctx;
}

NK_API void
nk_sfml_font_stash_begin(nk_sfml* sfml)
{
    nk_font_atlas_init_default(&sfml->atlas);
    nk_font_atlas_begin(&sfml->atlas);
}

NK_API void
nk_sfml_font_stash_end(nk_sfml* sfml)
{
    const void* image;
    int w, h;
    image = nk_font_atlas_bake(&sfml->atlas, &w, &h, NK_FONT_ATLAS_RGBA32);
    nk_sfml_device_upload_atlas(sfml, image, w, h);
    nk_font_atlas_end(&sfml->atlas, nk_handle_id(NK_SFML_FONT_TEX_ID), &sfml->ogl.tex_null);
    if(sfml->atlas.default_font)
        nk_style_set_font(&sfml->ctx, &sfml->atlas.default_font->handle);
}

NK_API int
nk_sfml_handle_event(nk_sfml* sfml, sf::Event* evt)
{
    struct nk_context* ctx = &sfml->ctx;
    /* optional grabbing behavior */
    if(ctx->input.mouse.grab)
        ctx->input.mouse.grab = 0;
    else if(ctx->input.mouse.ungrab) {
        int x = static_cast<int>(ctx->input.mouse.prev.x);
        int y = static_cast<int>(ctx->input.mouse.prev.y);
        sf::Mouse::setPosition(sf::Vector2i(x, y), *sfml->window);
        ctx->input.mouse.ungrab = 0;
    }
    if(evt->type == sf::Event::KeyReleased || evt->type == sf::Event::KeyPressed)
    {
        int down = evt->type == sf::Event::KeyPressed;
        sf::Keyboard::Key key = evt->key.code;
        if(key == sf::Keyboard::RShift || key == sf::Keyboard::LShift)
            nk_input_key(ctx, NK_KEY_SHIFT, down);
        else if(key == sf::Keyboard::Delete)
            nk_input_key(ctx, NK_KEY_DEL, down);
        else if(key == sf::Keyboard::Return)
            nk_input_key(ctx, NK_KEY_ENTER, down);
        else if(key == sf::Keyboard::Tab)
            nk_input_key(ctx, NK_KEY_TAB, down);
        else if(key == sf::Keyboard::BackSpace)
            nk_input_key(ctx, NK_KEY_BACKSPACE, down);
        else if(key == sf::Keyboard::Home) {
            nk_input_key(ctx, NK_KEY_TEXT_START, down);
            nk_input_key(ctx, NK_KEY_SCROLL_START, down);
        } else if(key == sf::Keyboard::End) {
            nk_input_key(ctx, NK_KEY_TEXT_END, down);
            nk_input_key(ctx, NK_KEY_SCROLL_END, down);
        } else if(key == sf::Keyboard::PageDown) {
            nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down);
        } else if(key == sf::Keyboard::PageUp) {
            nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down);
        } else if(key == sf::Keyboard::Z) {
            nk_input_key(ctx, NK_KEY_TEXT_UNDO, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
        } else if(key == sf::Keyboard::R) {
            nk_input_key(ctx, NK_KEY_TEXT_REDO, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
        } else if(key == sf::Keyboard::C) {
            nk_input_key(ctx, NK_KEY_COPY, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
        } else if(key == sf::Keyboard::V) {
            nk_input_key(ctx, NK_KEY_PASTE, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
        } else if(key == sf::Keyboard::X) {
            nk_input_key(ctx, NK_KEY_CUT, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
        } else if(key == sf::Keyboard::B) {
            nk_input_key(ctx, NK_KEY_TEXT_LINE_START, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
        } else if(key == sf::Keyboard::E) {
            nk_input_key(ctx, NK_KEY_TEXT_LINE_END, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
        } else if(key == sf::Keyboard::Up) {
            nk_input_key(ctx, NK_KEY_UP, down);
        } else if(key == sf::Keyboard::Down) {
            nk_input_key(ctx, NK_KEY_DOWN, down);
        } else if(key == sf::Keyboard::Left) {
            nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
            nk_input_key(ctx, NK_KEY_LEFT, down);
        } else if(key == sf::Keyboard::Right) {
            nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, down && sf::Keyboard::isKeyPressed(sf::Keyboard::LControl));
            nk_input_key(ctx, NK_KEY_RIGHT, down);
        } else return 0;
        return 1;
    } else if(evt->type == sf::Event::MouseButtonPressed || evt->type == sf::Event::MouseButtonReleased) {
        int down = evt->type == sf::Event::MouseButtonPressed;
        const int x = evt->mouseButton.x, y = evt->mouseButton.y;
        if(evt->mouseButton.button == sf::Mouse::Left)
            nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down);
        if(evt->mouseButton.button == sf::Mouse::Middle)
            nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down);
        if(evt->mouseButton.button == sf::Mouse::Right)
            nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down);
        else return 0;
        return 1;
    } else if(evt->type == sf::Event::MouseMoved) {
        nk_input_motion(ctx, evt->mouseMove.x, evt->mouseMove.y);
        return 1;
    } else if(evt->type == sf::Event::TouchBegan || evt->type == sf::Event::TouchEnded) {
        int down = evt->type == sf::Event::TouchBegan;
        const int x = evt->touch.x, y = evt->touch.y;
        nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down);
        return 1;
    } else if(evt->type == sf::Event::TouchMoved) {
        if(ctx->input.mouse.grabbed) {
            int x = static_cast<int>(ctx->input.mouse.prev.x);
            int y = static_cast<int>(ctx->input.mouse.prev.y);
            nk_input_motion(ctx, x + evt->touch.x, y + evt->touch.y);
        } else nk_input_motion(ctx, evt->touch.x, evt->touch.y);
        return 1;
    } else if(evt->type == sf::Event::TextEntered) {
        /* 8 ~ backspace */
        if (evt->text.unicode != 8 && evt->text.unicode != '\r') {
            nk_input_unicode(ctx, evt->text.unicode);
        }
        return 1;
    } else if(evt->type == sf::Event::MouseWheelScrolled) {
        nk_input_scroll(ctx, nk_vec2(0,evt->mouseWheelScroll.delta));
        return 1;
    }
    return 0;
}

NK_API
void nk_sfml_shutdown(nk_sfml* sfml)
{
    nk_font_atlas_clear(&sfml->atlas);
    nk_free(&sfml->ctx);
    nk_sfml_device_destroy(sfml);
}

#endif
