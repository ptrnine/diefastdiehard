#pragma once

#include "lua.hpp"

#include "typeinit/vec2f_init.hpp"
#include "typeinit/player_init.hpp"
#include "typeinit/game_state_init.hpp"
#include "typeinit/timer_init.hpp"
#include "typeinit/log_init.hpp"
#include "typeinit/cfg_init.hpp"
#include "typeinit/ai_init.hpp"
#include "typeinit/sfml_event_init.hpp"

namespace dfdh
{
inline void lua_ai_instance_init(luacpp::luactx& ctx) {
    lua_vec2f_init(ctx);
    lua_log_init(ctx);
    lua_timer_init(ctx);
    lua_ai_init(ctx);
}

inline void lua_global_instance_init(luacpp::luactx& ctx) {
    lua_vec2f_init(ctx);
    lua_log_init(ctx);
    lua_timer_init(ctx);
    lua_player_init(ctx);
    lua_game_state_init(ctx);
    lua_sfml_event_init(ctx);
    lua_cfg_init(ctx);
}

void luactx_mgr::load() {
    std::string file_name = "dfdh";
    std::string assist_file_name = "assist";

    if (_instance_name == "ai") {
        file_name += "_ai";
        assist_file_name += "_ai";
    }

    try {
        auto dir = fs::current_path() / "data/scripts";
        auto fullpath = (dir / (file_name + ".lua")).string();

        _ctx->load_and_call(fullpath.data());
        loaded = true;

        auto ofd = std::ofstream(dir / (assist_file_name + ".lua"));
        if (ofd.is_open())
            ofd << _ctx->generate_assist();
        else
            glog().error("Cannot generate lua assist file: {}", dir / (assist_file_name + ".lua"));
    }
    catch (const std::exception& e) {
        glog().error("lua load failed: {}", e.what());
        loaded = false;
    }
}

luactx_mgr::luactx_mgr(std::string instance_name, bool deferred_load):
    _ctx(std::make_unique<luacpp::luactx>(true)), _instance_name(std::move(instance_name)) {
    //glog().info("Init lua instance \"{}\"", _instance_name);

    auto package_path = fs::current_path() / "data/scripts/?.lua";
    _ctx->enable_implicit_assist(false);
    _ctx->provide(LUA_TNAME("package.path"), package_path.string());
    _ctx->enable_implicit_assist();

    if (_instance_name == "global")
        lua_global_instance_init(*_ctx);
    else if (_instance_name == "ai")
        lua_ai_instance_init(*_ctx);
    else
        throw std::runtime_error("Invalid lua instance name: " + _instance_name);

    if (!deferred_load)
        load();
}

void luactx_mgr::execute_line(const std::string& line) {
    try {
        _ctx->load_and_call(luacpp::lua_code{line});
    }
    catch (const std::exception& e) {
        glog().error("lua error: {}", e.what());
    }
}

using namespace std::chrono_literals;

template <typename F>
class lua_caller {
public:
    using lua_func_t = decltype(luacpp::luactx().extract<F>(luacpp::lua_name{""}));
    using return_t   = typename luacpp::details::lua_function_traits<std::add_pointer_t<F>>::return_t;

    template <typename RT>
    static auto empty_return() {
        if constexpr (!std::is_same_v<RT, void>)
            return std::optional<RT>();
    }

    lua_caller() = default;
    lua_caller(luacpp::luactx*           ictx,
               std::string               function_name,
               std::chrono::microseconds iretry_timeout  = 0us,
               bool                      isuppress_error = false):
        ctx(ictx),
        func_name(std::move(function_name)),
        retry_timeout(iretry_timeout),
        suppress_error(isuppress_error) {}

    template <typename... Ts>
    decltype(empty_return<return_t>()) operator()(Ts&&... args) {
        if (!ctx)
            return empty_return<return_t>();

        if (function) {
            return try_call(std::forward<Ts>(args)...);
        }
        else {
            auto now = std::chrono::steady_clock::now();
            if (now - last_fail_tp < retry_timeout)
                return empty_return<return_t>();

            try {
                function.emplace(ctx->extract<F>(func_name));
                return try_call(std::forward<Ts>(args)...);
            }
            catch (const std::exception& e) {
                if (!suppress_error)
                    glog().error("lua extract failed: {}", e.what());

                function.reset();
                last_fail_tp = std::chrono::steady_clock::now();
                return empty_return<return_t>();
            }
        }
    }

    explicit operator bool() const {
        return function;
    }

private:
    template <typename... Ts>
    decltype(empty_return<return_t>()) try_call(Ts&&... args) {
        try {
            return (*function)(std::forward<Ts>(args)...);
        }
        catch (const std::exception& e) {
            if (!suppress_error)
                glog().error("lua call failed: {}", e.what());

            function.reset();
            last_fail_tp = std::chrono::steady_clock::now();
            return empty_return<return_t>();
        }
    }

private:
    luacpp::luactx*                       ctx            = nullptr;
    std::chrono::steady_clock::time_point last_fail_tp   = std::chrono::steady_clock::now();
    luacpp::lua_name                      func_name      = {{}};

public:
    std::chrono::microseconds retry_timeout{0};
    bool                      suppress_error = false;

private:
    std::optional<lua_func_t> function;
};

template <typename F>
lua_caller<F> luactx_mgr::get_caller(std::string name, std::chrono::microseconds retry_timeout, bool suppress_error) {
    return lua_caller<F>(_ctx.get(), std::move(name), retry_timeout, suppress_error);
}

template <typename F>
std::function<F>
luactx_mgr::get_caller_type_erased(std::string name, std::chrono::microseconds retry_timeout, bool suppress_error) {
    return {get_caller<F>(std::move(name), retry_timeout, suppress_error)};
}

} // namespace dfdh
