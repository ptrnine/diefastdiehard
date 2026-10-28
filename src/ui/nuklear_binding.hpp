/* Generated with PEngine/lang_tool (and slightly adapted) */
#pragma once

#include "base/types.hpp"
#include "base/vec2.hpp"

#include <nuklear.h>
#include <SFML/Graphics/Color.hpp>

NK_API nk_bool nk_selectable_image(struct nk_context* ctx, struct nk_image img, nk_bool* value);
NK_API void nk_text_multiline_colored(struct nk_context* ctx, const char* str, int len, struct nk_color color);

namespace dfdh {
template <typename F>
using nkfunc = F*;

class ui_nuklear_base {
public:
    bool init_fixed(void* memory, nk_size size, const struct nk_user_font* user_font) {
        return nk_init_fixed(_ctx, memory, size, user_font) == nk_true;
    }

    bool init(struct nk_allocator* allocator, const struct nk_user_font* user_font) {
        return nk_init(_ctx, allocator, user_font) == nk_true;
    }

    bool init_custom(struct nk_buffer* cmds, struct nk_buffer* pool, const struct nk_user_font* user_font) {
        return nk_init_custom(_ctx, cmds, pool, user_font) == nk_true;
    }

    void clear() {
        return nk_clear(_ctx);
    }

    void free() {
        return nk_free(_ctx);
    }

    void input_begin() {
        return nk_input_begin(_ctx);
    }

    void input_motion(int x, int y) {
        return nk_input_motion(_ctx, x, y);
    }

    void input_key(enum nk_keys keys, bool down) {
        return nk_input_key(_ctx, keys, (down ? nk_true : nk_false));
    }

    void input_button(enum nk_buttons buttons, int x, int y, bool down) {
        return nk_input_button(_ctx, buttons, x, y, (down ? nk_true : nk_false));
    }

    void input_scroll(vec2f val) {
        return nk_input_scroll(_ctx, nk_vec2(val.x, val.y));
    }

    void input_char(char sybmol) {
        return nk_input_char(_ctx, sybmol);
    }

    void input_glyph(const nk_glyph glyph) {
        return nk_input_glyph(_ctx, glyph);
    }

    void input_unicode(nk_rune rune) {
        return nk_input_unicode(_ctx, rune);
    }

    void input_end() {
        return nk_input_end(_ctx);
    }

    const struct nk_command* _begin() {
        return nk__begin(_ctx);
    }

    const struct nk_command* _next(const struct nk_command* command) {
        return nk__next(_ctx, command);
    }

    bool begin(const char* title, struct nk_rect bounds, nk_flags flags) {
        return nk_begin(_ctx, title, bounds, flags) == nk_true;
    }

    bool begin_titled(const char* name, const char* title, struct nk_rect bounds, nk_flags flags) {
        return nk_begin_titled(_ctx, name, title, bounds, flags) == nk_true;
    }

    void end() {
        return nk_end(_ctx);
    }

    struct nk_window * window_find(const char* name) {
        return nk_window_find(_ctx, name);
    }

    struct nk_rect window_get_bounds() {
        return nk_window_get_bounds(_ctx);
    }

    vec2f window_get_position() {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_window_get_position(_ctx));
    }

    vec2f window_get_size() {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_window_get_size(_ctx));
    }

    float window_get_width() {
        return nk_window_get_width(_ctx);
    }

    float window_get_height() {
        return nk_window_get_height(_ctx);
    }

    struct nk_panel* window_get_panel() {
        return nk_window_get_panel(_ctx);
    }

    struct nk_rect window_get_content_region() {
        return nk_window_get_content_region(_ctx);
    }

    vec2f window_get_content_region_min() {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_window_get_content_region_min(_ctx));
    }

    vec2f window_get_content_region_max() {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_window_get_content_region_max(_ctx));
    }

    vec2f window_get_content_region_size() {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_window_get_content_region_size(_ctx));
    }

    struct nk_command_buffer* window_get_canvas() {
        return nk_window_get_canvas(_ctx);
    }

    void window_get_scroll(nk_uint* offset_x, nk_uint* offset_y) {
        return nk_window_get_scroll(_ctx, offset_x, offset_y);
    }

    bool window_has_focus() {
        return nk_window_has_focus(_ctx) == nk_true;
    }

    bool window_is_hovered() {
        return nk_window_is_hovered(_ctx) == nk_true;
    }

    bool window_is_collapsed(const char* name) {
        return nk_window_is_collapsed(_ctx, name) == nk_true;
    }

    bool window_is_closed(const char* str) {
        return nk_window_is_closed(_ctx, str) == nk_true;
    }

    bool window_is_hidden(const char* str) {
        return nk_window_is_hidden(_ctx, str) == nk_true;
    }

    bool window_is_active(const char* str) {
        return nk_window_is_active(_ctx, str) == nk_true;
    }

    bool window_is_any_hovered() {
        return nk_window_is_any_hovered(_ctx) == nk_true;
    }

    bool item_is_any_active() {
        return nk_item_is_any_active(_ctx) == nk_true;
    }

    void window_set_bounds(const char* name, struct nk_rect bounds) {
        return nk_window_set_bounds(_ctx, name, bounds);
    }

    void window_set_position(const char* name, vec2f pos) {
        return nk_window_set_position(_ctx, name, nk_vec2(pos.x, pos.y));
    }

    void window_set_size(const char* name, vec2f vec2) {
        return nk_window_set_size(_ctx, name, nk_vec2(vec2.x, vec2.y));
    }

    void window_set_focus(const char* name) {
        return nk_window_set_focus(_ctx, name);
    }

    void window_set_scroll(nk_uint offset_x, nk_uint offset_y) {
        return nk_window_set_scroll(_ctx, offset_x, offset_y);
    }

    void window_close(const char* name) {
        return nk_window_close(_ctx, name);
    }

    void window_collapse(const char* name, enum nk_collapse_states state) {
        return nk_window_collapse(_ctx, name, state);
    }

    void window_collapse_if(const char* name, enum nk_collapse_states collapse_states, int cond) {
        return nk_window_collapse_if(_ctx, name, collapse_states, cond);
    }

    void window_show(const char* name, enum nk_show_states show_states) {
        return nk_window_show(_ctx, name, show_states);
    }

    void window_show_if(const char* name, enum nk_show_states show_states, int cond) {
        return nk_window_show_if(_ctx, name, show_states, cond);
    }

    void layout_set_min_row_height(float height) {
        return nk_layout_set_min_row_height(_ctx, height);
    }

    void layout_reset_min_row_height() {
        return nk_layout_reset_min_row_height(_ctx);
    }

    struct nk_rect layout_widget_bounds() {
        return nk_layout_widget_bounds(_ctx);
    }

    float layout_ratio_from_pixel(float pixel_width) {
        return nk_layout_ratio_from_pixel(_ctx, pixel_width);
    }

    void layout_row_dynamic(float height, int cols) {
        return nk_layout_row_dynamic(_ctx, height, cols);
    }

    void layout_row_static(float height, int item_width, int cols) {
        return nk_layout_row_static(_ctx, height, item_width, cols);
    }

    void layout_row_begin(enum nk_layout_format fmt, float row_height, int cols) {
        return nk_layout_row_begin(_ctx, fmt, row_height, cols);
    }

    void layout_row_push(float value) {
        return nk_layout_row_push(_ctx, value);
    }

    void layout_row_end() {
        return nk_layout_row_end(_ctx);
    }

    void layout_row(enum nk_layout_format layout_format, float height, int cols, const float* ratio) {
        return nk_layout_row(_ctx, layout_format, height, cols, ratio);
    }

    void layout_row_template_begin(float row_height) {
        return nk_layout_row_template_begin(_ctx, row_height);
    }

    void layout_row_template_push_dynamic() {
        return nk_layout_row_template_push_dynamic(_ctx);
    }

    void layout_row_template_push_variable(float min_width) {
        return nk_layout_row_template_push_variable(_ctx, min_width);
    }

    void layout_row_template_push_static(float width) {
        return nk_layout_row_template_push_static(_ctx, width);
    }

    void layout_row_template_end() {
        return nk_layout_row_template_end(_ctx);
    }

    void layout_space_begin(enum nk_layout_format layout_format, float height, int widget_count) {
        return nk_layout_space_begin(_ctx, layout_format, height, widget_count);
    }

    void layout_space_push(struct nk_rect bounds) {
        return nk_layout_space_push(_ctx, bounds);
    }

    void layout_space_end() {
        return nk_layout_space_end(_ctx);
    }

    struct nk_rect layout_space_bounds() {
        return nk_layout_space_bounds(_ctx);
    }

    vec2f layout_space_to_screen(vec2f vec2) {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_layout_space_to_screen(_ctx, nk_vec2(vec2.x, vec2.y)));
    }

    vec2f layout_space_to_local(vec2f vec2) {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_layout_space_to_local(_ctx, nk_vec2(vec2.x, vec2.y)));
    }

    struct nk_rect layout_space_rect_to_screen(struct nk_rect rect) {
        return nk_layout_space_rect_to_screen(_ctx, rect);
    }

    struct nk_rect layout_space_rect_to_local(struct nk_rect rect) {
        return nk_layout_space_rect_to_local(_ctx, rect);
    }

    bool group_begin(const char* title, nk_flags flags) {
        return nk_group_begin(_ctx, title, flags) == nk_true;
    }

    bool group_begin_titled(const char* name, const char* title, nk_flags flags) {
        return nk_group_begin_titled(_ctx, name, title, flags) == nk_true;
    }

    void group_end() {
        return nk_group_end(_ctx);
    }

    bool group_scrolled_offset_begin(nk_uint* x_offset, nk_uint* y_offset, const char* title, nk_flags flags) {
        return nk_group_scrolled_offset_begin(_ctx, x_offset, y_offset, title, flags) == nk_true;
    }

    bool group_scrolled_begin(struct nk_scroll* off, const char* title, nk_flags flags) {
        return nk_group_scrolled_begin(_ctx, off, title, flags) == nk_true;
    }

    void group_scrolled_end() {
        return nk_group_scrolled_end(_ctx);
    }

    void group_get_scroll(const char* id, nk_uint* x_offset, nk_uint* y_offset) {
        return nk_group_get_scroll(_ctx, id, x_offset, y_offset);
    }

    void group_set_scroll(const char* id, nk_uint x_offset, nk_uint y_offset) {
        return nk_group_set_scroll(_ctx, id, x_offset, y_offset);
    }

    bool tree_push_hashed(enum nk_tree_type tree_type, const char* title, enum nk_collapse_states initial_state, const char* hash, int len, int seed) {
        return nk_tree_push_hashed(_ctx, tree_type, title, initial_state, hash, len, seed) == nk_true;
    }

    bool tree_image_push_hashed(enum nk_tree_type tree_type, struct nk_image image, const char* title, enum nk_collapse_states initial_state, const char* hash, int len, int seed) {
        return nk_tree_image_push_hashed(_ctx, tree_type, image, title, initial_state, hash, len, seed) == nk_true;
    }

    void tree_pop() {
        return nk_tree_pop(_ctx);
    }

    bool tree_state_push(enum nk_tree_type tree_type, const char* title, enum nk_collapse_states* state) {
        return nk_tree_state_push(_ctx, tree_type, title, state) == nk_true;
    }

    bool tree_state_image_push(enum nk_tree_type tree_type, struct nk_image image, const char* title, enum nk_collapse_states* state) {
        return nk_tree_state_image_push(_ctx, tree_type, image, title, state) == nk_true;
    }

    void tree_state_pop() {
        return nk_tree_state_pop(_ctx);
    }

    bool tree_element_push_hashed(enum nk_tree_type tree_type, const char* title, enum nk_collapse_states initial_state, nk_bool* selected, const char* hash, int len, int seed) {
        return nk_tree_element_push_hashed(_ctx, tree_type, title, initial_state, selected, hash, len, seed) == nk_true;
    }

    bool tree_element_image_push_hashed(enum nk_tree_type tree_type, struct nk_image image, const char* title, enum nk_collapse_states initial_state, nk_bool* selected, const char* hash, int len, int seed) {
        return nk_tree_element_image_push_hashed(_ctx, tree_type, image, title, initial_state, selected, hash, len, seed) == nk_true;
    }

    void tree_element_pop() {
        return nk_tree_element_pop(_ctx);
    }

    bool list_view_begin(struct nk_list_view* out, const char* id, nk_flags flags, int row_height, int row_count) {
        return nk_list_view_begin(_ctx, out, id, flags, row_height, row_count) == nk_true;
    }

    struct nk_rect widget_bounds() {
        return nk_widget_bounds(_ctx);
    }

    vec2f widget_position() {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_widget_position(_ctx));
    }

    vec2f widget_size() {
        return [](struct nk_vec2 v) { return vec2f{v.x, v.y}; }(nk_widget_size(_ctx));
    }

    float widget_width() {
        return nk_widget_width(_ctx);
    }

    float widget_height() {
        return nk_widget_height(_ctx);
    }

    bool widget_is_hovered() {
        return nk_widget_is_hovered(_ctx) == nk_true;
    }

    bool widget_is_mouse_clicked(enum nk_buttons buttons) {
        return nk_widget_is_mouse_clicked(_ctx, buttons) == nk_true;
    }

    bool widget_has_mouse_click_down(enum nk_buttons buttons, bool down) {
        return nk_widget_has_mouse_click_down(_ctx, buttons, (down ? nk_true : nk_false)) == nk_true;
    }

    void spacing(int cols) {
        return nk_spacing(_ctx, cols);
    }

    void text(const char* str, int len, nk_flags flags) {
        return nk_text(_ctx, str, len, flags);
    }

    void text_colored(const char* str, int len, nk_flags flags, sf::Color color) {
        return nk_text_colored(_ctx, str, len, flags, nk_color{color.r, color.g, color.b, color.a});
    }

    void text_multiline_colored(const char* str, int len, sf::Color color) {
        return nk_text_multiline_colored(_ctx, str, len, nk_color{color.r, color.g, color.b, color.a});
    }

    void text_wrap(const char* str, int len) {
        return nk_text_wrap(_ctx, str, len);
    }

    void text_wrap_colored(const char* str, int len, sf::Color color) {
        return nk_text_wrap_colored(_ctx, str, len, nk_color{color.r, color.g, color.b, color.a});
    }

    void label(const char* str, nk_flags align) {
        return nk_label(_ctx, str, align);
    }

    void label_colored(const char* str, nk_flags align, sf::Color color) {
        return nk_label_colored(_ctx, str, align, nk_color{color.r, color.g, color.b, color.a});
    }

    void label_wrap(const char* str) {
        return nk_label_wrap(_ctx, str);
    }

    void label_colored_wrap(const char* str, sf::Color color) {
        return nk_label_colored_wrap(_ctx, str, nk_color{color.r, color.g, color.b, color.a});
    }

    void image(struct nk_image image) {
        return nk_image(_ctx, image);
    }

    void image_color(struct nk_image image, sf::Color color) {
        return nk_image_color(_ctx, image, nk_color{color.r, color.g, color.b, color.a});
    }

    bool button_text(const char* title, int len) {
        return nk_button_text(_ctx, title, len) == nk_true;
    }

    bool button_label(const char* title) {
        return nk_button_label(_ctx, title) == nk_true;
    }

    bool button_color(sf::Color color) {
        return nk_button_color(_ctx, nk_color{color.r, color.g, color.b, color.a}) == nk_true;
    }

    bool button_symbol(enum nk_symbol_type symbol_type) {
        return nk_button_symbol(_ctx, symbol_type) == nk_true;
    }

    bool button_image(struct nk_image img) {
        return nk_button_image(_ctx, img) == nk_true;
    }

    bool button_symbol_label(enum nk_symbol_type symbol_type, const char* str, nk_flags text_alignment) {
        return nk_button_symbol_label(_ctx, symbol_type, str, text_alignment) == nk_true;
    }

    bool button_symbol_text(enum nk_symbol_type symbol_type, const char* str, int len, nk_flags alignment) {
        return nk_button_symbol_text(_ctx, symbol_type, str, len, alignment) == nk_true;
    }

    bool button_image_label(struct nk_image img, const char* str, nk_flags text_alignment) {
        return nk_button_image_label(_ctx, img, str, text_alignment) == nk_true;
    }

    bool button_image_text(struct nk_image img, const char* str, int len, nk_flags alignment) {
        return nk_button_image_text(_ctx, img, str, len, alignment) == nk_true;
    }

    bool button_text_styled(const struct nk_style_button* style_button, const char* title, int len) {
        return nk_button_text_styled(_ctx, style_button, title, len) == nk_true;
    }

    bool button_label_styled(const struct nk_style_button* style_button, const char* title) {
        return nk_button_label_styled(_ctx, style_button, title) == nk_true;
    }

    bool button_symbol_styled(const struct nk_style_button* style_button, enum nk_symbol_type symbol_type) {
        return nk_button_symbol_styled(_ctx, style_button, symbol_type) == nk_true;
    }

    bool button_image_styled(const struct nk_style_button* style_button, struct nk_image img) {
        return nk_button_image_styled(_ctx, style_button, img) == nk_true;
    }

    bool button_symbol_text_styled(const struct nk_style_button* style_button, enum nk_symbol_type symbol_type, const char* str, int len, nk_flags alignment) {
        return nk_button_symbol_text_styled(_ctx, style_button, symbol_type, str, len, alignment) == nk_true;
    }

    bool button_symbol_label_styled(const struct nk_style_button* style, enum nk_symbol_type symbol, const char* title, nk_flags align) {
        return nk_button_symbol_label_styled(_ctx, style, symbol, title, align) == nk_true;
    }

    bool button_image_label_styled(const struct nk_style_button* style_button, struct nk_image img, const char* str, nk_flags text_alignment) {
        return nk_button_image_label_styled(_ctx, style_button, img, str, text_alignment) == nk_true;
    }

    bool button_image_text_styled(const struct nk_style_button* style_button, struct nk_image img, const char* str, int len, nk_flags alignment) {
        return nk_button_image_text_styled(_ctx, style_button, img, str, len, alignment) == nk_true;
    }

    void button_set_behavior(enum nk_button_behavior button_behavior) {
        return nk_button_set_behavior(_ctx, button_behavior);
    }

    bool button_push_behavior(enum nk_button_behavior button_behavior) {
        return nk_button_push_behavior(_ctx, button_behavior) == nk_true;
    }

    bool button_pop_behavior() {
        return nk_button_pop_behavior(_ctx) == nk_true;
    }

    bool check_label(const char* str, bool active) {
        return nk_check_label(_ctx, str, (active ? nk_true : nk_false)) == nk_true;
    }

    bool check_text(const char* str, int len, bool active) {
        return nk_check_text(_ctx, str, len, (active ? nk_true : nk_false)) == nk_true;
    }

    unsigned check_flags_label(const char* str, unsigned int flags, unsigned int value) {
        return nk_check_flags_label(_ctx, str, flags, value);
    }

    unsigned check_flags_text(const char* str, int len, unsigned int flags, unsigned int value) {
        return nk_check_flags_text(_ctx, str, len, flags, value);
    }

    bool checkbox_label(const char* str, nk_bool* active) {
        return nk_checkbox_label(_ctx, str, active) == nk_true;
    }

    bool checkbox_text(const char* str, int len, nk_bool* active) {
        return nk_checkbox_text(_ctx, str, len, active) == nk_true;
    }

    bool checkbox_flags_label(const char* str, unsigned int* flags, unsigned int value) {
        return nk_checkbox_flags_label(_ctx, str, flags, value) == nk_true;
    }

    bool checkbox_flags_text(const char* str, int len, unsigned int* flags, unsigned int value) {
        return nk_checkbox_flags_text(_ctx, str, len, flags, value) == nk_true;
    }

    bool radio_label(const char* str, nk_bool* active) {
        return nk_radio_label(_ctx, str, active) == nk_true;
    }

    bool radio_text(const char* str, int len, nk_bool* active) {
        return nk_radio_text(_ctx, str, len, active) == nk_true;
    }

    bool option_label(const char* str, bool active) {
        return nk_option_label(_ctx, str, (active ? nk_true : nk_false)) == nk_true;
    }

    bool option_text(const char* str, int len, bool active) {
        return nk_option_text(_ctx, str, len, (active ? nk_true : nk_false)) == nk_true;
    }

    bool selectable_label(const char* str, nk_flags align, nk_bool* value) {
        return nk_selectable_label(_ctx, str, align, value) == nk_true;
    }

    bool selectable_text(const char* str, int len, nk_flags align, nk_bool* value) {
        return nk_selectable_text(_ctx, str, len, align, value) == nk_true;
    }

    bool selectable_image(struct nk_image image, nk_bool* value) {
        return nk_selectable_image(_ctx, image, value) == nk_true;
    }

    bool selectable_image_label(struct nk_image image, const char* str, nk_flags align, nk_bool* value) {
        return nk_selectable_image_label(_ctx, image, str, align, value) == nk_true;
    }

    bool selectable_image_text(struct nk_image image, const char* str, int len, nk_flags align, nk_bool* value) {
        return nk_selectable_image_text(_ctx, image, str, len, align, value) == nk_true;
    }

    bool selectable_symbol_label(enum nk_symbol_type symbol_type, const char* str, nk_flags align, nk_bool* value) {
        return nk_selectable_symbol_label(_ctx, symbol_type, str, align, value) == nk_true;
    }

    bool selectable_symbol_text(enum nk_symbol_type symbol_type, const char* str, int len, nk_flags align, nk_bool* value) {
        return nk_selectable_symbol_text(_ctx, symbol_type, str, len, align, value) == nk_true;
    }

    bool select_label(const char* str, nk_flags align, bool value) {
        return nk_select_label(_ctx, str, align, (value ? nk_true : nk_false)) == nk_true;
    }

    bool select_text(const char* str, int len, nk_flags align, bool value) {
        return nk_select_text(_ctx, str, len, align, (value ? nk_true : nk_false)) == nk_true;
    }

    bool select_image_label(struct nk_image image, const char* str, nk_flags align, bool value) {
        return nk_select_image_label(_ctx, image, str, align, (value ? nk_true : nk_false)) == nk_true;
    }

    bool select_image_text(struct nk_image image, const char* str, int len, nk_flags align, bool value) {
        return nk_select_image_text(_ctx, image, str, len, align, (value ? nk_true : nk_false)) == nk_true;
    }

    bool select_symbol_label(enum nk_symbol_type symbol_type, const char* str, nk_flags align, bool value) {
        return nk_select_symbol_label(_ctx, symbol_type, str, align, (value ? nk_true : nk_false)) == nk_true;
    }

    bool select_symbol_text(enum nk_symbol_type symbol_type, const char* str, int len, nk_flags align, bool value) {
        return nk_select_symbol_text(_ctx, symbol_type, str, len, align, (value ? nk_true : nk_false)) == nk_true;
    }

    float slide_float(float min, float val, float max, float step) {
        return nk_slide_float(_ctx, min, val, max, step);
    }

    int slide_int(int min, int val, int max, int step) {
        return nk_slide_int(_ctx, min, val, max, step);
    }

    bool slider_float(float min, float* val, float max, float step) {
        return nk_slider_float(_ctx, min, val, max, step) == nk_true;
    }

    bool slider_int(int min, int* val, int max, int step) {
        return nk_slider_int(_ctx, min, val, max, step) == nk_true;
    }

    bool progress(nk_size* cur, nk_size max, bool modifyable) {
        return nk_progress(_ctx, cur, max, (modifyable ? nk_true : nk_false)) == nk_true;
    }

    nk_size prog(nk_size cur, nk_size max, bool modifyable) {
        return nk_prog(_ctx, cur, max, (modifyable ? nk_true : nk_false));
    }

    nk_colorf color_picker(const nk_colorf& color, enum nk_color_format color_format) {
        return nk_color_picker(_ctx, color, color_format);
    }

    bool color_pick(nk_colorf& colorf, enum nk_color_format color_format) {
        return nk_color_pick(_ctx, &colorf, color_format) == nk_true;
    }

    bool property_int(const char* name, int min, int* val, int max, int step, float inc_per_pixel) {
        return nk_property_int(_ctx, name, min, val, max, step, inc_per_pixel) == nk_true;
    }

    bool property_float(const char* name, float min, float* val, float max, float step, float inc_per_pixel) {
        return nk_property_float(_ctx, name, min, val, max, step, inc_per_pixel) == nk_true;
    }

    bool property_double(const char* name, double min, double* val, double max, double step, float inc_per_pixel) {
        return nk_property_double(_ctx, name, min, val, max, step, inc_per_pixel) == nk_true;
    }

    int propertyi(const char* name, int min, int val, int max, int step, float inc_per_pixel) {
        return nk_propertyi(_ctx, name, min, val, max, step, inc_per_pixel);
    }

    float propertyf(const char* name, float min, float val, float max, float step, float inc_per_pixel) {
        return nk_propertyf(_ctx, name, min, val, max, step, inc_per_pixel);
    }

    double propertyd(const char* name, double min, double val, double max, double step, float inc_per_pixel) {
        return nk_propertyd(_ctx, name, min, val, max, step, inc_per_pixel);
    }

    nk_flags edit_string(nk_flags flags, char* buffer, int* len, int max, nk_plugin_filter plugin_filter) {
        return nk_edit_string(_ctx, flags, buffer, len, max, plugin_filter);
    }

    nk_flags edit_string_zero_terminated(nk_flags flags, char* buffer, int max, nk_plugin_filter plugin_filter) {
        return nk_edit_string_zero_terminated(_ctx, flags, buffer, max, plugin_filter);
    }

    nk_flags edit_buffer(nk_flags flags, struct nk_text_edit* text_edit, nk_plugin_filter plugin_filter) {
        return nk_edit_buffer(_ctx, flags, text_edit, plugin_filter);
    }

    void edit_focus(nk_flags flags) {
        return nk_edit_focus(_ctx, flags);
    }

    void edit_unfocus() {
        return nk_edit_unfocus(_ctx);
    }

    bool chart_begin(enum nk_chart_type chart_type, int num, float min, float max) {
        return nk_chart_begin(_ctx, chart_type, num, min, max) == nk_true;
    }

    bool chart_begin_colored(enum nk_chart_type chart_type, sf::Color color, sf::Color active, int num, float min, float max) {
        return nk_chart_begin_colored(_ctx, chart_type, nk_color{color.r, color.g, color.b, color.a}, nk_color{active.r, active.g, active.b, active.a}, num, min, max) == nk_true;
    }

    void chart_add_slot(const enum nk_chart_type chart_type, int count, float min_value, float max_value) {
        return nk_chart_add_slot(_ctx, chart_type, count, min_value, max_value);
    }

    void chart_add_slot_colored(const enum nk_chart_type chart_type, sf::Color color, sf::Color active, int count, float min_value, float max_value) {
        return nk_chart_add_slot_colored(_ctx, chart_type, nk_color{color.r, color.g, color.b, color.a}, nk_color{active.r, active.g, active.b, active.a}, count, min_value, max_value);
    }

    nk_flags chart_push(float CHANGE_THIS_NAME) {
        return nk_chart_push(_ctx, CHANGE_THIS_NAME);
    }

    nk_flags chart_push_slot(float CHANGE_THIS_NAME, int CHANGE_THIS_NAME1) {
        return nk_chart_push_slot(_ctx, CHANGE_THIS_NAME, CHANGE_THIS_NAME1);
    }

    void chart_end() {
        return nk_chart_end(_ctx);
    }

    void plot(enum nk_chart_type chart_type, const float* values, int count, int offset) {
        return nk_plot(_ctx, chart_type, values, count, offset);
    }

    void plot_function(enum nk_chart_type chart_type, void* userdata, nkfunc<float(void* user, int index)> value_getter, int count, int offset) {
        return nk_plot_function(_ctx, chart_type, userdata, value_getter, count, offset);
    }

    bool popup_begin(enum nk_popup_type popup_type, const char* str, nk_flags flags, struct nk_rect bounds) {
        return nk_popup_begin(_ctx, popup_type, str, flags, bounds) == nk_true;
    }

    void popup_close() {
        return nk_popup_close(_ctx);
    }

    void popup_end() {
        return nk_popup_end(_ctx);
    }

    void popup_get_scroll(nk_uint* offset_x, nk_uint* offset_y) {
        return nk_popup_get_scroll(_ctx, offset_x, offset_y);
    }

    void popup_set_scroll(nk_uint offset_x, nk_uint offset_y) {
        return nk_popup_set_scroll(_ctx, offset_x, offset_y);
    }

    int combo(const char** items, int count, int selected, int item_height, vec2f size) {
        return nk_combo(_ctx, items, count, selected, item_height, nk_vec2(size.x, size.y));
    }

    int combo_separator(const char* items_separated_by_separator, int separator, int selected, int count, int item_height, vec2f size) {
        return nk_combo_separator(_ctx, items_separated_by_separator, separator, selected, count, item_height, nk_vec2(size.x, size.y));
    }

    int combo_string(const char* items_separated_by_zeros, int selected, int count, int item_height, vec2f size) {
        return nk_combo_string(_ctx, items_separated_by_zeros, selected, count, item_height, nk_vec2(size.x, size.y));
    }

    int combo_callback(nkfunc<void(void*, int, const char**)> item_getter, void* userdata, int selected, int count, int item_height, vec2f size) {
        return nk_combo_callback(_ctx, item_getter, userdata, selected, count, item_height, nk_vec2(size.x, size.y));
    }

    bool combobox(const char** items, int count, int* selected, int item_height, vec2f size) {
        return nk_combobox(_ctx, items, count, selected, item_height, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combobox_string(const char* items_separated_by_zeros, int* selected, int count, int item_height, vec2f size) {
        return nk_combobox_string(_ctx, items_separated_by_zeros, selected, count, item_height, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combobox_separator(const char* items_separated_by_separator, int separator, int* selected, int count, int item_height, vec2f size) {
        return nk_combobox_separator(_ctx, items_separated_by_separator, separator, selected, count, item_height, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combobox_callback(nkfunc<void(void*, int, const char**)> item_getter, void* CHANGE_THIS_NAME, int* selected, int count, int item_height, vec2f size) {
        return nk_combobox_callback(_ctx, item_getter, CHANGE_THIS_NAME, selected, count, item_height, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_text(const char* selected, int CHANGE_THIS_NAME, vec2f size) {
        return nk_combo_begin_text(_ctx, selected, CHANGE_THIS_NAME, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_label(const char* selected, vec2f size) {
        return nk_combo_begin_label(_ctx, selected, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_color(sf::Color color, vec2f size) {
        return nk_combo_begin_color(_ctx, nk_color{color.r, color.g, color.b, color.a}, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_symbol(enum nk_symbol_type symbol_type, vec2f size) {
        return nk_combo_begin_symbol(_ctx, symbol_type, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_symbol_label(const char* selected, enum nk_symbol_type symbol_type, vec2f size) {
        return nk_combo_begin_symbol_label(_ctx, selected, symbol_type, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_symbol_text(const char* selected, int CHANGE_THIS_NAME, enum nk_symbol_type symbol_type, vec2f size) {
        return nk_combo_begin_symbol_text(_ctx, selected, CHANGE_THIS_NAME, symbol_type, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_image(struct nk_image img, vec2f size) {
        return nk_combo_begin_image(_ctx, img, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_image_label(const char* selected, struct nk_image image, vec2f size) {
        return nk_combo_begin_image_label(_ctx, selected, image, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_begin_image_text(const char* selected, int CHANGE_THIS_NAME, struct nk_image image, vec2f size) {
        return nk_combo_begin_image_text(_ctx, selected, CHANGE_THIS_NAME, image, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool combo_item_label(const char* str, nk_flags alignment) {
        return nk_combo_item_label(_ctx, str, alignment) == nk_true;
    }

    bool combo_item_text(const char* str, int CHANGE_THIS_NAME, nk_flags alignment) {
        return nk_combo_item_text(_ctx, str, CHANGE_THIS_NAME, alignment) == nk_true;
    }

    bool combo_item_image_label(struct nk_image image, const char* str, nk_flags alignment) {
        return nk_combo_item_image_label(_ctx, image, str, alignment) == nk_true;
    }

    bool combo_item_image_text(struct nk_image image, const char* str, int CHANGE_THIS_NAME, nk_flags alignment) {
        return nk_combo_item_image_text(_ctx, image, str, CHANGE_THIS_NAME, alignment) == nk_true;
    }

    bool combo_item_symbol_label(enum nk_symbol_type symbol_type, const char* str, nk_flags alignment) {
        return nk_combo_item_symbol_label(_ctx, symbol_type, str, alignment) == nk_true;
    }

    bool combo_item_symbol_text(enum nk_symbol_type symbol_type, const char* str, int CHANGE_THIS_NAME, nk_flags alignment) {
        return nk_combo_item_symbol_text(_ctx, symbol_type, str, CHANGE_THIS_NAME, alignment) == nk_true;
    }

    void combo_close() {
        return nk_combo_close(_ctx);
    }

    void combo_end() {
        return nk_combo_end(_ctx);
    }

    bool contextual_begin(nk_flags flags, vec2f vec2, struct nk_rect trigger_bounds) {
        return nk_contextual_begin(_ctx, flags, nk_vec2(vec2.x, vec2.y), trigger_bounds) == nk_true;
    }

    bool contextual_item_text(const char* str, int CHANGE_THIS_NAME, nk_flags align) {
        return nk_contextual_item_text(_ctx, str, CHANGE_THIS_NAME, align) == nk_true;
    }

    bool contextual_item_label(const char* str, nk_flags align) {
        return nk_contextual_item_label(_ctx, str, align) == nk_true;
    }

    bool contextual_item_image_label(struct nk_image image, const char* str, nk_flags alignment) {
        return nk_contextual_item_image_label(_ctx, image, str, alignment) == nk_true;
    }

    bool contextual_item_image_text(struct nk_image image, const char* str, int len, nk_flags alignment) {
        return nk_contextual_item_image_text(_ctx, image, str, len, alignment) == nk_true;
    }

    bool contextual_item_symbol_label(enum nk_symbol_type symbol_type, const char* str, nk_flags alignment) {
        return nk_contextual_item_symbol_label(_ctx, symbol_type, str, alignment) == nk_true;
    }

    bool contextual_item_symbol_text(enum nk_symbol_type symbol_type, const char* str, int CHANGE_THIS_NAME, nk_flags alignment) {
        return nk_contextual_item_symbol_text(_ctx, symbol_type, str, CHANGE_THIS_NAME, alignment) == nk_true;
    }

    void contextual_close() {
        return nk_contextual_close(_ctx);
    }

    void contextual_end() {
        return nk_contextual_end(_ctx);
    }

    void tooltip(const char* str) {
        return nk_tooltip(_ctx, str);
    }

    bool tooltip_begin(float width) {
        return nk_tooltip_begin(_ctx, width) == nk_true;
    }

    void tooltip_end() {
        return nk_tooltip_end(_ctx);
    }

    void menubar_begin() {
        return nk_menubar_begin(_ctx);
    }

    void menubar_end() {
        return nk_menubar_end(_ctx);
    }

    bool menu_begin_text(const char* title, int title_len, nk_flags align, vec2f size) {
        return nk_menu_begin_text(_ctx, title, title_len, align, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_begin_label(const char* str, nk_flags align, vec2f size) {
        return nk_menu_begin_label(_ctx, str, align, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_begin_image(const char* str, struct nk_image image, vec2f size) {
        return nk_menu_begin_image(_ctx, str, image, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_begin_image_text(const char* str, int CHANGE_THIS_NAME, nk_flags align, struct nk_image image, vec2f size) {
        return nk_menu_begin_image_text(_ctx, str, CHANGE_THIS_NAME, align, image, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_begin_image_label(const char* str, nk_flags align, struct nk_image image, vec2f size) {
        return nk_menu_begin_image_label(_ctx, str, align, image, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_begin_symbol(const char* str, enum nk_symbol_type symbol_type, vec2f size) {
        return nk_menu_begin_symbol(_ctx, str, symbol_type, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_begin_symbol_text(const char* str, int CHANGE_THIS_NAME, nk_flags align, enum nk_symbol_type symbol_type, vec2f size) {
        return nk_menu_begin_symbol_text(_ctx, str, CHANGE_THIS_NAME, align, symbol_type, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_begin_symbol_label(const char* str, nk_flags align, enum nk_symbol_type symbol_type, vec2f size) {
        return nk_menu_begin_symbol_label(_ctx, str, align, symbol_type, nk_vec2(size.x, size.y)) == nk_true;
    }

    bool menu_item_text(const char* str, int CHANGE_THIS_NAME, nk_flags align) {
        return nk_menu_item_text(_ctx, str, CHANGE_THIS_NAME, align) == nk_true;
    }

    bool menu_item_label(const char* str, nk_flags alignment) {
        return nk_menu_item_label(_ctx, str, alignment) == nk_true;
    }

    bool menu_item_image_label(struct nk_image image, const char* str, nk_flags alignment) {
        return nk_menu_item_image_label(_ctx, image, str, alignment) == nk_true;
    }

    bool menu_item_image_text(struct nk_image image, const char* str, int len, nk_flags alignment) {
        return nk_menu_item_image_text(_ctx, image, str, len, alignment) == nk_true;
    }

    bool menu_item_symbol_text(enum nk_symbol_type symbol_type, const char* str, int CHANGE_THIS_NAME, nk_flags alignment) {
        return nk_menu_item_symbol_text(_ctx, symbol_type, str, CHANGE_THIS_NAME, alignment) == nk_true;
    }

    bool menu_item_symbol_label(enum nk_symbol_type symbol_type, const char* str, nk_flags alignment) {
        return nk_menu_item_symbol_label(_ctx, symbol_type, str, alignment) == nk_true;
    }

    void menu_close() {
        return nk_menu_close(_ctx);
    }

    void menu_end() {
        return nk_menu_end(_ctx);
    }

    void style_default() {
        return nk_style_default(_ctx);
    }

    void style_from_table(const sf::Color& color) {
        return nk_style_from_table(_ctx, reinterpret_cast<const struct nk_color*>(&color));
    }

    void style_load_cursor(enum nk_style_cursor style_cursor, const struct nk_cursor* cursor) {
        return nk_style_load_cursor(_ctx, style_cursor, cursor);
    }

    void style_load_all_cursors(struct nk_cursor* cursor) {
        return nk_style_load_all_cursors(_ctx, cursor);
    }

    void style_set_font(const struct nk_user_font* user_font) {
        return nk_style_set_font(_ctx, user_font);
    }

    bool style_set_cursor(enum nk_style_cursor style_cursor) {
        return nk_style_set_cursor(_ctx, style_cursor) == nk_true;
    }

    void style_show_cursor() {
        return nk_style_show_cursor(_ctx);
    }

    void style_hide_cursor() {
        return nk_style_hide_cursor(_ctx);
    }

    bool style_push_font(const struct nk_user_font* user_font) {
        return nk_style_push_font(_ctx, user_font) == nk_true;
    }

    bool style_push_float(float* CHANGE_THIS_NAME, float CHANGE_THIS_NAME1) {
        return nk_style_push_float(_ctx, CHANGE_THIS_NAME, CHANGE_THIS_NAME1) == nk_true;
    }

    bool style_push_vec2(struct nk_vec2* vec2, vec2f vec21) {
        return nk_style_push_vec2(_ctx, vec2, nk_vec2(vec21.x, vec21.y)) == nk_true;
    }

    bool style_push_style_item(struct nk_style_item* style_item, struct nk_style_item style_item1) {
        return nk_style_push_style_item(_ctx, style_item, style_item1) == nk_true;
    }

    bool style_push_flags(nk_flags* flags, nk_flags flags1) {
        return nk_style_push_flags(_ctx, flags, flags1) == nk_true;
    }

    bool style_push_color(sf::Color& color, sf::Color color1) {
        return nk_style_push_color(_ctx, reinterpret_cast<struct nk_color*>(&color), nk_color{color1.r, color1.g, color1.b, color1.a}) == nk_true;
    }

    bool style_pop_font() {
        return nk_style_pop_font(_ctx) == nk_true;
    }

    bool style_pop_float() {
        return nk_style_pop_float(_ctx) == nk_true;
    }

    bool style_pop_vec2() {
        return nk_style_pop_vec2(_ctx) == nk_true;
    }

    bool style_pop_style_item() {
        return nk_style_pop_style_item(_ctx) == nk_true;
    }

    bool style_pop_flags() {
        return nk_style_pop_flags(_ctx) == nk_true;
    }

    bool style_pop_color() {
        return nk_style_pop_color(_ctx) == nk_true;
    }

protected:
    struct nk_context* _ctx;
};
} // namespace ui
