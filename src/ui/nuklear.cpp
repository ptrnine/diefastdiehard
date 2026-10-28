#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SFML_GL3_IMPLEMENTATION

#include <SFML/Window.hpp>

/* Third-party implementation: false positive at -O2 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include <nuklear.h>
#pragma GCC diagnostic pop
#if defined(_WIN32)
#include "nuklear_sfml_gl3_win.h"
#else
#include "nuklear_sfml_gl3.h"
#endif

NK_LIB void nk_draw_selectable_icon(struct nk_command_buffer*         out,
                                    nk_flags                          state,
                                    const struct nk_style_selectable* style,
                                    nk_bool                           active,
                                    const struct nk_rect*             bounds,
                                    const struct nk_rect*             icon,
                                    const struct nk_image*            img) {
    const struct nk_style_item* background;

    /* select correct colors/images */
    if (!active) {
        if (state & NK_WIDGET_STATE_ACTIVED)
            background = &style->pressed;
        else if (state & NK_WIDGET_STATE_HOVER)
            background = &style->hover;
        else
            background = &style->normal;
    }
    else {
        if (state & NK_WIDGET_STATE_ACTIVED)
            background = &style->pressed_active;
        else if (state & NK_WIDGET_STATE_HOVER)
            background = &style->hover_active;
        else
            background = &style->normal_active;
    }
    /* draw selectable background and text */
    if (background->type == NK_STYLE_ITEM_IMAGE)
        nk_draw_image(out, *bounds, &background->data.image, nk_white);
    else
        nk_fill_rect(out, *bounds, style->rounding, background->data.color);
    if (icon)
        nk_draw_image(out, *icon, img, nk_white);
}

NK_LIB nk_bool nk_do_selectable_icon(nk_flags*                         state,
                                     struct nk_command_buffer*         out,
                                     struct nk_rect                    bounds,
                                     nk_bool*                          value,
                                     const struct nk_image*            img,
                                     const struct nk_style_selectable* style,
                                     const struct nk_input*            in) {
    nk_bool        old_value;
    struct nk_rect touch;
    struct nk_rect icon;

    NK_ASSERT(state);
    NK_ASSERT(out);
    NK_ASSERT(value);
    NK_ASSERT(style);

    if (!state || !out || !value || !style) return 0;
    old_value = *value;

    /* toggle behavior */
    touch.x = bounds.x - style->touch_padding.x;
    touch.y = bounds.y - style->touch_padding.y;
    touch.w = bounds.w + style->touch_padding.x * 2;
    touch.h = bounds.h + style->touch_padding.y * 2;
    if (nk_button_behavior(state, touch, in, NK_BUTTON_DEFAULT))
        *value = !(*value);

    icon.x = bounds.x + 2 * style->padding.x;
    icon.y = bounds.y + style->padding.y;
    icon.w = bounds.w - 2 * style->padding.x;
    icon.h = bounds.h - 2 * style->padding.y;

    icon.x += style->image_padding.x;
    icon.y += style->image_padding.y;
    icon.w -= 2 * style->image_padding.x;
    icon.h -= 2 * style->image_padding.y;

    /* draw selectable */
    if (style->draw_begin) style->draw_begin(out, style->userdata);
    nk_draw_selectable_icon(out, *state, style, *value, &bounds, &icon, img);
    if (style->draw_end) style->draw_end(out, style->userdata);
    return old_value != *value;
}

NK_API nk_bool nk_selectable_image(struct nk_context* ctx,
                                   struct nk_image    img,
                                   nk_bool*           value) {
    struct nk_window*      win;
    struct nk_panel*       layout;
    const struct nk_input* in;
    const struct nk_style* style;

    enum nk_widget_layout_states state;
    struct nk_rect               bounds;

    NK_ASSERT(ctx);
    NK_ASSERT(value);
    NK_ASSERT(ctx->current);
    NK_ASSERT(ctx->current->layout);
    if (!ctx || !ctx->current || !ctx->current->layout || !value)
        return 0;

    win    = ctx->current;
    layout = win->layout;
    style  = &ctx->style;

    state = nk_widget(&bounds, ctx);
    if (!state)
        return 0;
    in = (state == NK_WIDGET_ROM || layout->flags & NK_WINDOW_ROM) ? 0 : &ctx->input;
    return nk_do_selectable_icon(&ctx->last_widget_state,
                                 &win->buffer,
                                 bounds,
                                 value,
                                 &img,
                                 &style->selectable,
                                 in);
}

NK_LIB int nk_text_trimline(const char* text, int text_len) {
    if (text[text_len - 2] == '\r' && text[text_len - 1] == '\n')
        return text_len - 2; /* trailing \r\n */
    else if (text[text_len - 1] == '\r' || text[text_len - 1] == '\n')
        return text_len - 1; /* trailing \r or \n */
    return text_len;
}

NK_LIB int nk_text_linelen(const char* text, int text_len, int* glyphs) {
    int     glyph_len = 0;
    nk_rune unicode   = 0;
    nk_rune lastuni   = 0;
    int     len       = 0;
    int     g         = 0;

    glyph_len = nk_utf_decode(text, &unicode, text_len);
    while (glyph_len && (len < text_len)) {
        len += glyph_len;
        /* \r doesn't count as newline by itself, since it might be part of \r\n. */
        int newline = unicode == '\n';

        lastuni   = unicode;
        glyph_len = nk_utf_decode(&text[len], &unicode, text_len - len);
        /* If we had an \r, but the upcoming character wasn't an \n,
         * then the \r actually was a newline. End before consuming next glyph. */
        if (lastuni == '\r' && unicode != '\n')
            break;
        g++;
        if (newline)
            break;
    }
    *glyphs = g;
    return len;
}

NK_LIB void nk_widget_text_multiline(struct nk_command_buffer*  o,
                                     struct nk_rect             b,
                                     const char*                string,
                                     int                        len,
                                     const struct nk_text*      t,
                                     const struct nk_user_font* f) {
    int            glyphs  = 0;
    int            fitting = 0;
    int            done    = 0;
    struct nk_rect line;
    struct nk_text text;

    NK_ASSERT(o);
    NK_ASSERT(t);
    if (!o || !t)
        return;

    text.padding    = nk_vec2(0, 0);
    text.background = t->background;
    text.text       = t->text;

    b.w = NK_MAX(b.w, 2 * t->padding.x);
    b.h = NK_MAX(b.h, 2 * t->padding.y);
    b.h = b.h - 2 * t->padding.y;

    line.x = b.x + t->padding.x;
    line.y = b.y + t->padding.y;
    line.w = b.w - 2 * t->padding.x;
    line.h = 2 * t->padding.y + f->height;

    fitting = nk_text_linelen(string, len, &glyphs);
    while (done < len) {
        if (!fitting || line.y + line.h >= (b.y + b.h))
            break;
        nk_widget_text(o, line, &string[done], nk_text_trimline(&string[done], fitting), &text, NK_TEXT_LEFT, f);
        done += fitting;
        line.y += f->height + 2 * t->padding.y;
        fitting = nk_text_linelen(&string[done], len - done, &glyphs);
    }
}

/* https://github.com/Immediate-Mode-UI/Nuklear/pull/153 */
NK_API void nk_text_multiline_colored(struct nk_context* ctx, const char* str, int len, struct nk_color color) {
    struct nk_window*      win;
    const struct nk_style* style;

    struct nk_vec2 item_padding;
    struct nk_rect bounds;
    struct nk_text text;

    NK_ASSERT(ctx);
    NK_ASSERT(ctx->current);
    NK_ASSERT(ctx->current->layout);
    if (!ctx || !ctx->current || !ctx->current->layout)
        return;

    win   = ctx->current;
    style = &ctx->style;
    nk_panel_alloc_space(&bounds, ctx);
    item_padding = style->text.padding;

    text.padding.x  = item_padding.x;
    text.padding.y  = item_padding.y;
    text.background = style->window.background;
    text.text       = color;
    nk_widget_text_multiline(&win->buffer, bounds, str, len, &text, style->font);
}
