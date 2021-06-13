#pragma once

#include <queue>
#include <map>
#include <functional>
#include <vector>
#include <variant>

#include "split_view.hpp"
#include "log.hpp"
#include "ston.hpp"

namespace dfdh {

class command_buffer_singleton {
public:
    static command_buffer_singleton& instance() {
        static command_buffer_singleton inst;
        return inst;
    }

    command_buffer_singleton(const command_buffer_singleton&) = delete;
    command_buffer_singleton& operator=(const command_buffer_singleton&) = delete;
    command_buffer_singleton(command_buffer_singleton&&) = delete;
    command_buffer_singleton& operator=(command_buffer_singleton&&) = delete;

private:
    command_buffer_singleton() = default;
    ~command_buffer_singleton() = default;

public:

    using arg_view     = split_viewer<std::string::iterator, 2>;
    using arg_iterator = split_iterator<std::string::iterator, 2>;
    using args_t       = std::vector<arg_view>;

    template <typename F>
    struct func_traits;

    template <typename RetT, typename... ArgsT>
    struct func_traits<std::function<RetT(ArgsT...)>> {
        using return_type = RetT;
        static constexpr size_t arity = sizeof...(ArgsT);

        template <size_t I>
        using arg_type = std::tuple_element_t<I, std::tuple<ArgsT...>>;
    };

    template <typename F, size_t I = 0, typename... Ts>
    static void command_dispatch(const std::string& command_name,
                                 F&&                func,
                                 arg_iterator       arg_begin,
                                 arg_iterator       arg_end,
                                 Ts&&... args) {
        if (arg_begin == arg_end) {
            if constexpr (I < func_traits<std::decay_t<F>>::arity) {
                LOG_ERR("command '{}' accepts {} arguments (called with {} arguments)",
                        command_name,
                        func_traits<std::decay_t<F>>::arity,
                        I);
            }
            else {
                func(std::forward<Ts>(args)...);
            }
        }
        else {
            if constexpr (I == func_traits<std::decay_t<F>>::arity) {
                LOG_ERR("command '{}' accepts {} arguments (called with {} arguments)",
                        command_name,
                        func_traits<std::decay_t<F>>::arity,
                        I + 1);
            } else {
                using arg_type = typename func_traits<std::decay_t<F>>::template arg_type<I>;
                if constexpr (Number<std::decay_t<arg_type>>) {
                    std::decay_t<arg_type> v;
                    try {
                        v = ston<arg_type>(std::string((*arg_begin).begin(), (*arg_end).end()));
                    }
                    catch (const std::exception& e) {
                        LOG_ERR("{}: argument[{}] must be a number", command_name, I);
                        return;
                    }
                    command_dispatch<F, I + 1>(
                        command_name, std::forward<F>(func), ++arg_begin, arg_end, std::forward<Ts>(args)..., v);
                }
                if constexpr (std::is_same_v<std::decay_t<arg_type>, std::string>) {
                    auto v = std::string((*arg_begin).begin(), (*arg_end).end());
                    command_dispatch<F, I + 1>(
                        command_name, std::forward<F>(func), ++arg_begin, arg_end, std::forward<Ts>(args)..., v);
                }
            }
        }
    }

    template <typename F>
    auto command_dispatcher(const std::string& command_name, F&& func) {
        return [command_name, func](arg_iterator arg_begin, arg_iterator arg_end) mutable {
            command_dispatch(command_name, std::forward<F>(func), arg_begin, arg_end);
        };
    }

    void push(std::string command) {
        commands.push(std::move(command));
    }

    void run_handlers() {
        while (!commands.empty()) {
            auto args = commands.front() / split(' ', '\t');
            auto command_name = std::string((*args.begin()).begin(), (*args.begin()).end());

            auto found = handlers.find(command_name);
            if (found != handlers.end())
                found->second(++args.begin(), args.end());
            else
                LOG_ERR("command '{}' not found", command_name);

            commands.pop();
        }
    }

    template <typename F>
    void add_handler(const std::string& command_name, F func) {
        handlers.emplace(command_name, command_dispatcher(command_name, std::function{func}));
    }

    template <typename T, typename RetT, typename... ArgsT>
    void add_handler(const std::string& command_name, RetT(T::*func)(ArgsT...), T* it) {
        handlers.emplace(command_name, command_dispatcher(command_name, std::function{[it, func](ArgsT... args) mutable {
            return (it->*func)(args...);
        }}));
    }

private:
    std::queue<std::string>                                                commands;
    std::map<std::string, std::function<void(arg_iterator, arg_iterator)>> handlers;
};

inline command_buffer_singleton& command_buffer() {
    return command_buffer_singleton::instance();
}

} // namespace dfdh