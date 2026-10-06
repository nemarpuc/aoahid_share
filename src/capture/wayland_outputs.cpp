// SPDX-License-Identifier: MIT
#include "wayland_outputs.hpp"

#include "xdg-output-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace aoas {
namespace {

struct Output {
    wl_output* output{};
    zxdg_output_v1* xdg{};
    Monitor monitor;
    int transform{};
};

struct State {
    zxdg_output_manager_v1* manager{};
    std::vector<std::unique_ptr<Output>> outputs;
};

void output_geometry(void* data, wl_output*, int32_t, int32_t, const int32_t width_mm,
                     const int32_t height_mm, int32_t, const char*, const char*,
                     const int32_t transform) {
    auto* output = static_cast<Output*>(data);
    output->monitor.width_mm = width_mm;
    output->monitor.height_mm = height_mm;
    output->transform = transform;
}
void output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void output_done(void*, wl_output*) {}
void output_scale(void*, wl_output*, int32_t) {}
void output_name(void* data, wl_output*, const char* name) {
    static_cast<Output*>(data)->monitor.name = name;
}
void output_description(void*, wl_output*, const char*) {}

const wl_output_listener output_listener = {output_geometry, output_mode, output_done,
                                            output_scale,    output_name, output_description};

void xdg_position(void* data, zxdg_output_v1*, const int32_t x, const int32_t y) {
    auto* output = static_cast<Output*>(data);
    output->monitor.rect.x = x;
    output->monitor.rect.y = y;
}
void xdg_size(void* data, zxdg_output_v1*, const int32_t width, const int32_t height) {
    auto* output = static_cast<Output*>(data);
    output->monitor.rect.w = width;
    output->monitor.rect.h = height;
}
void xdg_done(void*, zxdg_output_v1*) {}
void xdg_name(void* data, zxdg_output_v1*, const char* name) {
    auto* output = static_cast<Output*>(data);
    if (output->monitor.name.empty())
        output->monitor.name = name;
}
void xdg_description(void*, zxdg_output_v1*, const char*) {}

const zxdg_output_v1_listener xdg_listener = {xdg_position, xdg_size, xdg_done, xdg_name,
                                              xdg_description};

void registry_global(void* data, wl_registry* registry, const uint32_t name, const char* interface,
                     const uint32_t version) {
    auto* state = static_cast<State*>(data);
    if (std::strcmp(interface, wl_output_interface.name) == 0) {
        auto output = std::make_unique<Output>();
        output->output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, std::min<uint32_t>(version, 4)));
        wl_output_add_listener(output->output, &output_listener, output.get());
        state->outputs.push_back(std::move(output));
    } else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        state->manager = static_cast<zxdg_output_manager_v1*>(wl_registry_bind(
            registry, name, &zxdg_output_manager_v1_interface, std::min<uint32_t>(version, 3)));
    }
}
void registry_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {registry_global, registry_remove};

} // namespace

std::vector<Monitor> query_wayland_outputs() {
    std::vector<Monitor> monitors;
    wl_display* display = wl_display_connect(nullptr);
    if (display == nullptr)
        return monitors;

    State state;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    // First round trip announces the globals, the second delivers the events
    // of the objects bound in the first.
    bool ok = wl_display_roundtrip(display) >= 0 && state.manager != nullptr;
    if (ok) {
        for (const std::unique_ptr<Output>& output : state.outputs) {
            output->xdg = zxdg_output_manager_v1_get_xdg_output(state.manager, output->output);
            zxdg_output_v1_add_listener(output->xdg, &xdg_listener, output.get());
        }
        ok = wl_display_roundtrip(display) >= 0;
    }
    if (ok) {
        for (const std::unique_ptr<Output>& output : state.outputs) {
            Monitor monitor = output->monitor;
            // The physical size is of the panel as built; a rotated output
            // shows it turned.
            const bool turned = output->transform == WL_OUTPUT_TRANSFORM_90 ||
                                output->transform == WL_OUTPUT_TRANSFORM_270 ||
                                output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                                output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
            if (turned)
                std::swap(monitor.width_mm, monitor.height_mm);
            if (monitor.rect.w > 0 && monitor.rect.h > 0)
                monitors.push_back(std::move(monitor));
        }
    }

    for (const std::unique_ptr<Output>& output : state.outputs) {
        if (output->xdg != nullptr)
            zxdg_output_v1_destroy(output->xdg);
        wl_output_destroy(output->output);
    }
    if (state.manager != nullptr)
        zxdg_output_manager_v1_destroy(state.manager);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return monitors;
}

} // namespace aoas
