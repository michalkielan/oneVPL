/*############################################################################
  # Copyright (C) 2005 Intel Corporation
  #
  # SPDX-License-Identifier: MIT
  ############################################################################*/

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <exception>
#include <iostream>
extern "C" {
#include <drm.h>
#include <drm_fourcc.h>
#include <intel_bufmgr.h>
#include <xf86drm.h>
}
#include "class_wayland.h"
#include "listener_wayland.h"
#include "wayland-drm-client-protocol.h"

#define BATCH_SIZE 0x80000

struct buffer {
    struct wl_buffer* buffer;
    mfxFrameSurface1* pInSurface;
};

static const struct wl_callback_listener frame_listener = { handle_done };

static const struct wl_buffer_listener buffer_listener = { buffer_release };

Wayland::Wayland()
        : m_display(NULL),
          m_registry(NULL),
          m_compositor(NULL),
          m_shell(NULL),
          m_drm(NULL),
#if defined(WAYLAND_LINUX_DMABUF_SUPPORT)
          m_dmabuf(NULL),
#endif
          m_shm(NULL),
          m_pool(NULL),
          m_surface(NULL),
          m_shell_surface(NULL),
          m_callback(NULL),
          m_event_queue(NULL),
          m_pending_frame(0),
          m_shm_pool(NULL),
          m_display_fd(-1),
          m_fd(-1),
          m_bufmgr(NULL),
          m_device_name(NULL),
          m_x(0),
          m_y(0),
          m_perf_mode(false) {
    std::memset(&m_poll, 0, sizeof(m_poll));
}

bool Wayland::InitDisplay() {
    static const struct wl_registry_listener registry_listener = { .global = registry_handle_global,
                                                                   .global_remove =
                                                                       remove_registry_global };

    m_display = wl_display_connect(NULL);
    if (NULL == m_display) {
        std::cout << "Error: Cannot connect to wayland display\n";
        return false;
    }
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &registry_listener, this);

    m_display_fd = wl_display_get_fd(m_display);
    wl_display_roundtrip(m_display);
    wl_display_roundtrip(m_display);
    m_event_queue = wl_display_create_queue(m_display);
    if (NULL == m_event_queue)
        return false;

    m_poll.fd     = m_display_fd;
    m_poll.events = POLLIN;
    return true;
}

int Wayland::DisplayRoundtrip() {
    return wl_display_roundtrip(m_display);
}

bool Wayland::CreateSurface() {
    static const struct wl_shell_surface_listener shell_surface_listener = {
        shell_surface_ping,
        shell_surface_configure
    };

    m_surface = wl_compositor_create_surface(m_compositor);
    if (NULL == m_surface)
        return false;

    m_shell_surface = wl_shell_get_shell_surface(m_shell, m_surface);
    if (NULL == m_shell_surface) {
        wl_surface_destroy(m_surface);
        return false;
    }

    wl_shell_surface_add_listener(m_shell_surface, &shell_surface_listener, 0);
    wl_shell_surface_set_toplevel(m_shell_surface);
    wl_shell_surface_set_user_data(m_shell_surface, m_surface);
    wl_surface_set_user_data(m_surface, NULL);
    return true;
}

void Wayland::FreeSurface() {
    if (NULL != m_shell_surface)
        wl_shell_surface_destroy(m_shell_surface);
    if (NULL != m_surface)
        wl_surface_destroy(m_surface);
}

void Wayland::Sync() {
    int ret;
    while (NULL != m_callback) {
        while (wl_display_prepare_read_queue(m_display, m_event_queue) < 0)
            wl_display_dispatch_queue_pending(m_display, m_event_queue);

        wl_display_flush(m_display);

        ret = poll(&m_poll, 1, -1);
        if (ret < 0)
            wl_display_cancel_read(m_display);
        else
            wl_display_read_events(m_display);
        wl_display_dispatch_queue_pending(m_display, m_event_queue);
    }
}

void Wayland::SetPerfMode(bool perf_mode) {
    m_perf_mode = perf_mode;
}

void Wayland::SetRenderWinPos(int x, int y) {
    m_x = x;
    m_y = y;
}

void Wayland::RenderBuffer(struct wl_buffer* buffer, mfxFrameSurface1* surface) {
    wld_buffer* m_buffer = new wld_buffer;
    if (m_buffer == NULL)
        return;

    m_buffer->buffer     = buffer;
    m_buffer->pInSurface = surface;

    wl_surface_attach(m_surface, buffer, 0, 0);
    wl_surface_damage(m_surface, m_x, m_y, surface->Info.CropW, surface->Info.CropH);

    wl_proxy_set_queue((struct wl_proxy*)buffer, m_event_queue);

    AddBufferToList(m_buffer);
    wl_buffer_add_listener(buffer, &buffer_listener, this);
    m_pending_frame = 1;
    if (m_perf_mode)
        m_callback = wl_display_sync(m_display);
    else
        m_callback = wl_surface_frame(m_surface);
    wl_callback_add_listener(m_callback, &frame_listener, this);
    wl_proxy_set_queue((struct wl_proxy*)m_callback, m_event_queue);
    wl_surface_commit(m_surface);
    wl_display_dispatch_queue(m_display, m_event_queue);
    /* Force a Sync before and after render to ensure client handles
      wayland events in a timely fashion. This also fixes the one time
      flicker issue on wl_shell_surface pointer enter */
    Sync();
}

void Wayland::RenderBufferWinPosSize(struct wl_buffer* buffer,
                                     int x,
                                     int y,
                                     int32_t width,
                                     int32_t height) {
    wl_surface_attach(m_surface, buffer, 0, 0);
    wl_surface_damage(m_surface, x, y, width, height);

    wl_proxy_set_queue((struct wl_proxy*)buffer, m_event_queue);

    wl_buffer_add_listener(buffer, &buffer_listener, NULL);
    m_pending_frame = 1;
    if (m_perf_mode)
        m_callback = wl_display_sync(m_display);
    else
        m_callback = wl_surface_frame(m_surface);
    wl_callback_add_listener(m_callback, &frame_listener, this);
    wl_proxy_set_queue((struct wl_proxy*)m_callback, m_event_queue);
    wl_surface_commit(m_surface);
    wl_display_dispatch_queue(m_display, m_event_queue);
}

void Wayland::DestroyCallback() {
    if (m_callback) {
        wl_callback_destroy(m_callback);
        m_callback      = NULL;
        m_pending_frame = 0;
    }
}

//ShmPool
bool Wayland::CreateShmPool(int fd, int32_t size, int prot) {
    m_shm_pool = new struct ShmPool;
    if (NULL == m_shm_pool)
        return false;

    m_shm_pool->capacity = size;
    m_shm_pool->size     = 0;
    m_shm_pool->fd       = fd;

    m_shm_pool->memory = static_cast<uint32_t*>(mmap(0, size, prot, MAP_SHARED, m_shm_pool->fd, 0));
    if (MAP_FAILED == m_shm_pool->memory) {
        delete m_shm_pool;
        return false;
    }

    m_pool = wl_shm_create_pool(m_shm, m_shm_pool->fd, size);
    if (NULL == m_pool) {
        munmap(m_shm_pool->memory, size);
        delete m_shm_pool;
        return false;
    }
    wl_shm_pool_set_user_data(m_pool, m_shm_pool);
    return true;
}

void Wayland::FreeShmPool() {
    wl_shm_pool_destroy(m_pool);
    munmap(m_shm_pool->memory, m_shm_pool->capacity);
    delete m_shm_pool;
}

struct wl_buffer* Wayland::CreateShmBuffer(unsigned width,
                                           unsigned height,
                                           unsigned stride,
                                           uint32_t PIXEL_FORMAT_ID) {
    struct wl_buffer* buffer;
    buffer = wl_shm_pool_create_buffer(m_pool,
                                       m_shm_pool->size * sizeof(uint32_t),
                                       width,
                                       height,
                                       stride,
                                       PIXEL_FORMAT_ID);
    if (NULL == buffer)
        return NULL;

    m_shm_pool->size += stride * height;
    return buffer;
}

void Wayland::FreeShmBuffer(struct wl_buffer* buffer) {
    wl_buffer_destroy(buffer);
}

int Wayland::Dispatch() {
    return wl_display_dispatch(m_display);
}

struct wl_buffer* Wayland::CreatePlanarBuffer(uint32_t name,
                                              int32_t width,
                                              int32_t height,
                                              uint32_t format,
                                              int32_t offsets[3],
                                              int32_t pitches[3]) {
    struct wl_buffer* buffer = NULL;
    if (NULL == m_drm)
        return NULL;

    buffer = wl_drm_create_planar_buffer(m_drm,
                                         name,
                                         width,
                                         height,
                                         format,
                                         offsets[0],
                                         pitches[0],
                                         offsets[1],
                                         pitches[1],
                                         offsets[2],
                                         pitches[2]);
    return buffer;
}

struct wl_buffer* Wayland::CreatePrimeBuffer(uint32_t name,
                                             int32_t width,
                                             int32_t height,
                                             uint32_t format,
                                             int32_t offsets[3],
                                             int32_t pitches[3]) {
    struct wl_buffer* buffer = NULL;

#if defined(WAYLAND_LINUX_DMABUF_SUPPORT)
    if (format == WL_DRM_FORMAT_NV12) {
        if (NULL == m_dmabuf)
            return NULL;

        struct zwp_linux_buffer_params_v1* dmabuf_params = NULL;
        int i                                            = 0;
        uint64_t modifier                                = I915_FORMAT_MOD_Y_TILED;

        dmabuf_params = zwp_linux_dmabuf_v1_create_params(m_dmabuf);
        for (i = 0; i < 2; i++) {
            zwp_linux_buffer_params_v1_add(dmabuf_params,
                                           name,
                                           i,
                                           offsets[i],
                                           pitches[i],
                                           modifier >> 32,
                                           modifier & 0xffffffff);
        }

        buffer = zwp_linux_buffer_params_v1_create_immed(dmabuf_params, width, height, format, 0);

        zwp_linux_buffer_params_v1_destroy(dmabuf_params);
    }
    else
#endif
    {
        if (NULL == m_drm)
            return NULL;

        buffer = wl_drm_create_prime_buffer(m_drm,
                                            name,
                                            width,
                                            height,
                                            format,
                                            offsets[0],
                                            pitches[0],
                                            offsets[1],
                                            pitches[1],
                                            offsets[2],
                                            pitches[2]);
    }

    return buffer;
}

Wayland::~Wayland() {
    if (NULL != m_shell)
        wl_shell_destroy(m_shell);
    if (NULL != m_shm)
        wl_shm_destroy(m_shm);
    if (NULL != m_bufmgr) {
        drm_intel_bufmgr_destroy(m_bufmgr);
    }
    if (NULL != m_compositor)
        wl_compositor_destroy(m_compositor);
    if (NULL != m_event_queue)
        wl_event_queue_destroy(m_event_queue);
    if (0 != m_buffers_list.size())
        DestroyBufferList();
    if (NULL != m_registry)
        wl_registry_destroy(m_registry);
    if (NULL != m_display)
        wl_display_disconnect(m_display);
    if (NULL != m_device_name)
        delete m_device_name;
}

// Registry
void Wayland::RegistryGlobal(struct wl_registry* registry,
                             uint32_t name,
                             const char* interface,
                             uint32_t version) {
    if (0 == strcmp(interface, "wl_compositor"))
        m_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version));
    else if (0 == strcmp(interface, "wl_shell"))
        m_shell =
            static_cast<wl_shell*>(wl_registry_bind(registry, name, &wl_shell_interface, version));
    else if (0 == strcmp(interface, "wl_drm")) {
        static const struct wl_drm_listener drm_listener = { drm_handle_device,
                                                             drm_handle_format,
                                                             drm_handle_authenticated,
                                                             drm_handle_capabilities };
        m_drm = static_cast<wl_drm*>(wl_registry_bind(registry, name, &wl_drm_interface, 2));
        wl_drm_add_listener(m_drm, &drm_listener, this);
    }
#if defined(WAYLAND_LINUX_DMABUF_SUPPORT)
    else if (0 == strcmp(interface, "zwp_linux_dmabuf_v1"))
        m_dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, version));
#endif
}

void Wayland::DrmHandleDevice(const char* name) {
    m_device_name = strdup(name);
    if (!m_device_name)
        return;

    drm_magic_t magic;
    m_fd = open(m_device_name, O_RDWR | O_CLOEXEC);
    if (-1 == m_fd) {
        std::cout << "Error: Could not open " << m_device_name << "\n";
        return;
    }
    int type = drmGetNodeTypeFromFd(m_fd);
    if (type != DRM_NODE_RENDER) {
        drmGetMagic(m_fd, &magic);
        wl_drm_authenticate(m_drm, magic);
    }
}

void Wayland::DrmHandleAuthenticated() {
    m_bufmgr = drm_intel_bufmgr_gem_init(m_fd, BATCH_SIZE);
}

void Wayland::AddBufferToList(wld_buffer* buffer) {
    if (buffer == NULL)
        return;

    if (buffer->pInSurface) {
        msdkFrameSurface* surface = FindUsedSurface(buffer->pInSurface);
        msdk_atomic_inc16(&(surface->render_lock));
        m_buffers_list.push_back(buffer);
    }
}

void Wayland::RemoveBufferFromList(struct wl_buffer* buffer) {
    wld_buffer* m_buffer = NULL;
    m_buffer             = m_buffers_list.front();
    if (NULL != m_buffer && (m_buffer->buffer == buffer)) {
        if (m_buffer->pInSurface) {
            msdkFrameSurface* surface = FindUsedSurface(m_buffer->pInSurface);
            msdk_atomic_dec16(&(surface->render_lock));
        }
        m_buffer->buffer     = NULL;
        m_buffer->pInSurface = NULL;
        m_buffers_list.pop_front();
        delete m_buffer;
    }
}

void Wayland::DestroyBufferList() {
    wld_buffer* m_buffer = NULL;
    while (!m_buffers_list.empty()) {
        m_buffer = m_buffers_list.front();
        if (m_buffer->pInSurface) {
            msdkFrameSurface* surface = FindUsedSurface(m_buffer->pInSurface);
            msdk_atomic_dec16(&(surface->render_lock));
        }
        m_buffers_list.pop_front();
        delete m_buffer;
    }
}

Wayland* WaylandCreate() {
    return new Wayland;
}

void WaylandDestroy(Wayland* pWld) {
    delete pWld;
}
