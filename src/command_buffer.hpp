#pragma once

#include <queue>
#include <map>
#include <functional>
#include <vector>
#include <variant>

#include "base/split_view.hpp"
#include "base/log.hpp"
#include "base/ston.hpp"

namespace dfdh {

template <typename T>
class cmd_opt : public std::optional<T> {
public:
    using std::optional<T>::optional;

    friend std::ostream& operator<<(std::ostream& os, cmd_opt o) {
        if (o)
            return os << *o;
        return os << "none";
    }
};

template <>
class cmd_opt<bool> : public std::optional<bool> {
public:
    enum cmd_opt_e { none = 0, off = 1, on = 2 };

    cmd_opt() = default;
    cmd_opt(cmd_opt_e opt_e) {
        if (opt_e == on)
            this->emplace(true);
        else if (opt_e == off)
            this->emplace(false);
    }
    cmd_opt(bool value): std::optional<bool>(value) {}

    operator cmd_opt_e() const {
        if (*this)
            return **this ? on : off;
        return none;
    }

    [[nodiscard]]
    bool test() const {
        if (*this)
            return **this;
        throw std::runtime_error("cmd_opt is not set");
    }

    friend std::ostream& operator<<(std::ostream& os, cmd_opt o) {
        if (o)
            return os << (*o ? "on" : "off");
        return os << "none";
    }
};

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
    command_buffer_singleton() {
        cmd_tree = std::make_unique<command_node>();
    }

    ~command_buffer_singleton() = default;

public:
    using arg_view     = split_when_viewer<std::string::iterator, skip_whitespace_outside_quotes>;
    using arg_iterator = split_when_iterator<std::string::iterator, skip_whitespace_outside_quotes>;
    using args_t       = std::vector<arg_view>;

    struct command_node {
        std::string cmd;
        std::map<std::string, std::unique_ptr<command_node>> subcmds = {};
        std::function<void(arg_iterator, arg_iterator)>      handler = {};
    };

    template <typename F>
    struct func_traits;

    template <typename RetT, typename... ArgsT>
    struct func_traits<std::function<RetT(ArgsT...)>> {
        using return_type = RetT;
        static constexpr size_t arity = sizeof...(ArgsT);

        template <size_t I>
        using arg_type = std::tuple_element_t<I, std::tuple<ArgsT...>>;
    };

    template <typename T>
    struct optional_t: std::false_type {};

    template <typename T>
    struct optional_t<std::optional<T>>: std::true_type {};

    template <typename T>
    struct optional_t<cmd_opt<T>>: std::true_type {};

    template <typename T>
    struct number_t
        : std::integral_constant<bool,
                                 (std::is_floating_point_v<T> || std::is_integral_v<T>) && !std::is_same_v<T, bool>> {};

    template <typename T>
    struct inner_type_s {
        using type = T;
    };

    template <typename T>
    struct inner_type_s<std::optional<T>> : inner_type_s<T> {};

    template <typename T>
    struct inner_type_s<cmd_opt<T>> : inner_type_s<T> {};

    template <typename T>
    using inner_type = typename inner_type_s<std::remove_const_t<std::remove_reference_t<T>>>::type;

    template <typename F, size_t I = 0, typename... Ts>
    static void command_dispatch(const std::string& command_name,
                                 F&&                func,
                                 arg_iterator       arg_begin,
                                 arg_iterator       arg_end,
                                 Ts&&... args) {
        if (arg_begin == arg_end) {
            if constexpr (I < func_traits<std::decay_t<F>>::arity) {
                if constexpr (func_traits<std::decay_t<F>>::arity - I == 1 &&
                              optional_t<
                                  std::decay_t<typename func_traits<std::decay_t<F>>::template arg_type<I>>>::value)
                    func(std::forward<Ts>(args)..., {});
                else
                    glog().error("command '{}' accepts {} arguments (called with {} arguments)",
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
                glog().error("command '{}' accepts {} arguments (called with {} arguments)",
                        command_name,
                        func_traits<std::decay_t<F>>::arity,
                        I + 1);
            } else {
                auto str = std::string((*arg_begin).begin(), (*arg_begin).end());
                using arg_type = typename func_traits<std::decay_t<F>>::template arg_type<I>;
                if constexpr (std::is_same_v<inner_type<arg_type>, bool>) {
                    if (str == "true" || str == "on")
                        command_dispatch<F, I + 1>(
                            command_name, std::forward<F>(func), ++arg_begin, arg_end, std::forward<Ts>(args)..., true);
                    else if (str == "false" || str == "off")
                        command_dispatch<F, I + 1>(
                            command_name, std::forward<F>(func), ++arg_begin, arg_end, std::forward<Ts>(args)..., false);
                    else
                        glog().error("{}: argument[{}] must be a boolean true/false (on/off)", command_name, I);
                    return;
                }
                if constexpr (Number<inner_type<arg_type>> && !std::is_same_v<inner_type<arg_type>, bool>) {
                    std::decay_t<arg_type> v;
                    try {
                        v = ston<inner_type<arg_type>>(str);
                    }
                    catch (const std::exception& e) {
                        glog().error("{}: argument[{}] must be a number", command_name, I);
                        return;
                    }
                    command_dispatch<F, I + 1>(
                        command_name, std::forward<F>(func), ++arg_begin, arg_end, std::forward<Ts>(args)..., v);
                    return;
                }
                if constexpr (std::is_same_v<inner_type<arg_type>, std::string>) {
                    command_dispatch<F, I + 1>(
                        command_name, std::forward<F>(func), ++arg_begin, arg_end, std::forward<Ts>(args)..., str);
                    return;
                }
                glog().error("{}: function argument[{}] has unknown type", command_name, I);
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
            auto args = commands.front() / split_when(skip_whitespace_outside_quotes());
            execute(*cmd_tree, args.begin(), args.end(), "");
            commands.pop();
        }
    }

    template <typename F>
    void add_handler(const std::string& command, F func) {
        command_node* node = cmd_tree.get();

        bool insert = true;
        for (auto cmd_view : command / split_when(skip_whitespace_outside_quotes())) {
            auto cmd = std::string(cmd_view.begin(), cmd_view.end());
            auto [pos, insert_was]     = node->subcmds
                       .emplace(cmd, std::make_unique<command_node>(command_node{cmd}));
            insert = insert_was;
            node = pos->second.get();
        }
        if (!insert) {
            glog().error("command '{}' already registered", command);
            return;
        }

        if (node == cmd_tree.get()) {
            glog().warn("attempt to register empty command");
            return;
        }

        node->handler = command_dispatcher(command, std::function{func});
    }

    void remove_handler(const std::string& command) {
        command_node* parent = nullptr;
        command_node* node   = cmd_tree.get();
        std::string   name;

        for (auto cmd_view : command / split_when(skip_whitespace_outside_quotes())) {
            name = std::string(cmd_view.begin(), cmd_view.end());
            auto found = node->subcmds.find(name);
            if (found == node->subcmds.end())
                return;

            parent = node;
            node   = found->second.get();
        }

        /* Erased with its subcommands: they can be registered again */
        if (parent)
            parent->subcmds.erase(name);
    }

    template <typename T, typename RetT, typename... ArgsT>
    void add_handler(const std::string& command, RetT (T::*func)(ArgsT...), T* it) {
        command_node* node = cmd_tree.get();

        bool insert = true;
        for (auto cmd_view : command / split_when(skip_whitespace_outside_quotes())) {
            auto cmd = std::string(cmd_view.begin(), cmd_view.end());
            auto [pos, insert_was]     = node->subcmds
                       .emplace(cmd, std::make_unique<command_node>(command_node{cmd}));
            insert = insert_was;
            node = pos->second.get();
        }
        if (!insert) {
            glog().error("command '{}' already registered", command);
            return;
        }

        if (node == cmd_tree.get()) {
            glog().warn("attempt to register empty command");
            return;
        }

        node->handler =
            command_dispatcher(command, std::function{[it, func](ArgsT... args) mutable {
                                   return (it->*func)(args...);
                               }});
    }

    [[nodiscard]]
    std::optional<std::string> find(const std::string& command) const {
        command_node* node = cmd_tree.get();
        std::string path;

        auto args = command / split_when(skip_whitespace_outside_quotes());
        for (auto i = args.begin(); i != args.end();) {
            auto next = std::next(i);
            auto cmd_view = *i;
            auto cmd = std::string(cmd_view.begin(), cmd_view.end());

            if (next == args.end()) {
                auto found = node->subcmds.lower_bound(cmd);
                if (found != node->subcmds.end() && found->first.starts_with(cmd)) {\
                    if (!path.empty())
                        path.push_back(' ');
                    return path + found->first;
                }
            }
            else {
                auto found = node->subcmds.find(cmd);
                if (found != node->subcmds.end()) {
                    if (!path.empty())
                        path.push_back(' ');
                    path += cmd;
                    node = found->second.get();
                } else
                    return {};
            }

            i = next;
        }

        return {};
    }

    [[nodiscard]]
    auto& get_command_tree() const {
        return cmd_tree;
    }

private:
    void execute(command_node& node, arg_iterator begin, arg_iterator end, const std::string& cmd_path) {
        auto next = std::next(begin);
        auto subcmd_view = *begin;
        auto subcmd = std::string(subcmd_view.begin(), subcmd_view.end());
        auto found = node.subcmds.find(subcmd);

        if (found != node.subcmds.end()) {
            execute(*found->second, next, end, cmd_path + ' ' + subcmd);
        }
        else if (node.handler) {
            node.handler(begin, end);
        } else {
            glog().error("unknown command {}", cmd_path);
        }

    }

private:
    std::queue<std::string>       commands;
    std::unique_ptr<command_node> cmd_tree;
};

inline command_buffer_singleton& command_buffer() {
    return command_buffer_singleton::instance();
}

} // namespace dfdh
