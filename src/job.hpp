#pragma once

#include <functional>
#include <optional>
#include <utility>

namespace egraph {

// Work done beside the caller, polled until it has its result; dropping it stops the work.
template <class T> using Job = std::move_only_function<std::optional<T>()>;

// A job whose result is known at once.
template <class T> Job<T> ready(T result) {
    return [result = std::move(result)]() mutable {
        return std::optional<T>{std::in_place, std::move(result)};
    };
}

} // namespace egraph
