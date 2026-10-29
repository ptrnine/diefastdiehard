#pragma once

#include "usertypes.hpp"
#include <luaffi/luacpp_ctx.hpp>

namespace dfdh
{

inline luacpp::explicit_return lua_return_cfg_value(luacpp::luactx& ctx, cfg_value<std::string, false>& v) {
    auto str = v.value();

    if (!v.has_value())
        return {ctx, nullptr};

    if (str == "true" || str == "on")
        return {ctx, true};
    if (str == "false" || str == "off")
        return {ctx, false};

    if (!isdigit(str.front()) && str.front() != '-' && str.front() != '+')
        return {ctx, str};

    if (str.find_first_of(" \t") == std::string::npos) {
        try {
            return {ctx, ston<double>(str)};
        }
        catch (...) {
        }
    }

    try {
        if (str.find(' ') != std::string::npos || str.find('\t') != std::string::npos) {
            auto splits = str / split(' ', '\t');
            auto first  = splits.begin();
            auto second = std::next(splits.begin());
            return {ctx,
                    vec2f(ston<float>({(*first).begin(), (*first).end()}),
                          ston<float>({(*second).begin(), (*second).end()}))};
        }
    }
    catch (...) {
    }

    return {ctx, str};
}

inline void lua_cfg_init(luacpp::luactx& ctx) {
    using cfg_ptr_t = cfg*;
    ctx.annotate({.comment = "Get section by name", .argument_names = {"config", "section_name"}});
    ctx.provide_member<cfg*>(LUA_TNAME("get_section"), [](const cfg_ptr_t& conf, const std::string& section_name) {
        return conf->try_get_section({section_name});
    });
    ctx.annotate({.comment = "Get section by name", .argument_names = {"config", "section_name"}});
    ctx.provide_member<cfg>(LUA_TNAME("get_section"), [](cfg& conf, const std::string& section_name) {
        return conf.try_get_section({section_name});
    });
    ctx.provide_member<cfg*>(LUA_TNAME("__tostring"), [](const cfg_ptr_t& conf) { return dfdh::format("{}", *conf); });
    ctx.provide_member<cfg>(LUA_TNAME("__tostring"), [](const cfg& conf) { return dfdh::format("{}", conf); });

    ctx.annotate({.explicit_type = "boolean"});
    ctx.set_member_table(
        luacpp::member_table<cfg*>{{"readonly",
                                    {[](const cfg_ptr_t& conf, luacpp::luactx& ctx) { ctx.push(conf->is_readonly()); },
                                     [](cfg_ptr_t& conf, luacpp::luactx& ctx) {
                                         bool v;
                                         ctx.get_new(v);
                                         conf->set_readonly(v);
                                     }}}});
    ctx.annotate({.explicit_type = "boolean"});
    ctx.set_member_table(
        luacpp::member_table<cfg>{{"readonly",
                                   {[](const cfg& conf, luacpp::luactx& ctx) { ctx.push(conf.is_readonly()); },
                                    [](cfg& conf, luacpp::luactx& ctx) {
                                        bool v;
                                        ctx.get_new(v);
                                        conf.set_readonly(v);
                                    }}}});
    ctx.annotate({.comment = "Read config at specified path", .argument_names = {"config_path"}});
    ctx.provide_member<cfg>(LUA_TNAME("open"), [](const std::string& path) { return cfg(path); });

    ctx.annotate({.comment = "Create or read config at specified path", .argument_names = {"config_path"}});
    ctx.provide_member<cfg>(LUA_TNAME("new"),
                            [](const std::string& path) { return cfg(path, cfg_mode::create_if_not_exists); });

    ctx.annotate({.comment = "Write config on disk"});
    ctx.provide_member<cfg>(LUA_TNAME("commit"), [](cfg& conf) { conf.commit(); });

    ctx.annotate({.comment = "Get or create section", .argument_names = {"config", "section_name"}});
    ctx.annotate({.argument_names = {"config", "section_name", "preffered_file_path_or_insert_mode"}});
    ctx.annotate({.argument_names = {"config", "section_name", "preffered_file_path", "insert_mode"}});
    ctx.provide_member<cfg>(
        LUA_TNAME("section"),
        [](cfg& conf, const std::string& section_name) { return &conf.get_or_create(section_name); },
        [&ctx](cfg& conf, const std::string& section_name, const std::string& preffered_file_path_or_insert_mode) {
            if (preffered_file_path_or_insert_mode == "at_the_start")
                return luacpp::explicit_return(ctx,
                                               &conf.get_or_create(section_name, {}, cfg::insert_mode::at_the_start));
            else if (preffered_file_path_or_insert_mode == "at_the_end")
                return luacpp::explicit_return(ctx,
                                               &conf.get_or_create(section_name, {}, cfg::insert_mode::at_the_end));
            else if (preffered_file_path_or_insert_mode == "lexicographicaly")
                return luacpp::explicit_return(
                    ctx, &conf.get_or_create(section_name, {}, cfg::insert_mode::lexicographicaly));
            else {
                try {
                    return luacpp::explicit_return(ctx, &conf.get_or_create(section_name, preffered_file_path_or_insert_mode));
                }
                catch (const cfg_not_found&) {
                    return luacpp::explicit_return(ctx, nullptr, "cfg not found");
                }
            }
        },
        [&ctx](cfg&               conf,
               const std::string& section_name,
               const std::string& preffered_file_path,
               const std::string& insert_mode) {
            auto im = cfg::insert_mode::lexicographicaly;
            if (insert_mode == "at_the_start")
                im = cfg::insert_mode::at_the_start;
            else if (insert_mode == "at_the_end")
                im = cfg::insert_mode::at_the_end;

            try {
                return luacpp::explicit_return(ctx, &conf.get_or_create(section_name, preffered_file_path, im));
            }
            catch (const cfg_not_found&) {
                return luacpp::explicit_return(ctx, nullptr, "cfg not found");
            }
        });

    // NOTE: lambdas must be passed inline (as rvalues) here. A named lambda variable is
    // an lvalue, which fails luacpp's LuaFunctionLike constraint (it doesn't decay T), so
    // luaprovide falls back to the generic value overload and luapush picks the
    // luapush(bool) overload — lambdas convert to bool — registering `true` instead of
    // the C function.
    ctx.annotate({.comment = "Get sections list", .argument_names = {"config"}});
    ctx.provide_member<cfg_ptr_t>(LUA_TNAME("list_sections"), [](const cfg_ptr_t& conf) { return conf->list_sections(); });
    ctx.annotate({.comment = "Get sections list", .argument_names = {"config"}});
    ctx.provide_member<cfg>(LUA_TNAME("list_sections"), [](cfg& conf) { return conf.list_sections(); });

    ctx.annotate({.comment = "Check is section exists", .argument_names = {"config", "section_name"}});
    ctx.provide_member<cfg_ptr_t>(
        LUA_TNAME("has_section"),
        [](const cfg_ptr_t& conf, const std::string& section_name) { return conf->has_section(section_name); });
    ctx.annotate({.comment = "Check is section exists", .argument_names = {"config", "section_name"}});
    ctx.provide_member<cfg>(
        LUA_TNAME("has_section"), [](cfg& conf, const std::string& section_name) { return conf.has_section(section_name); });

    using section_ptr_t = cfg_section<false>*;
    ctx.annotate({.comment = "Get value by key", .argument_names = {"section", "key"}});
    ctx.provide_member<cfg_section<false>*>(LUA_TNAME("value"), [&](const section_ptr_t& sect, const std::string& key) {
        if (auto value = sect->try_get<std::string>(key))
            return lua_return_cfg_value(ctx, *value);
        return luacpp::explicit_return{0};
    });

    ctx.annotate({.comment = "Global config"});
    ctx.provide(LUA_TNAME("Cfg"), &cfg::mutable_global());
}

} // namespace dfdh
