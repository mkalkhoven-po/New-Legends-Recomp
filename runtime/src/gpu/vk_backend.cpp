// Vulkan backend. The NV2A's render targets live in Vulkan images keyed by
// their guest address; textures are decoded from guest memory on the CPU and
// cached by content; draws use pipelines keyed by shader text and state.
#include "vk_backend.h"

#include <windows.h>

#include <shaderc/shaderc.h>

#include <condition_variable>
#include <algorithm>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>

#include "settings.h"
#include "version.h"
#include "texture.h"

#define VKCHECK(x)                                                    \
    do {                                                              \
        VkResult r_ = (x);                                            \
        if (r_ != VK_SUCCESS) fatal("%s failed: %d", #x, (int)r_);   \
    } while (0)

// --------------------------------------------------------------- window
static HWND g_hwnd;
static std::mutex g_win_m;
static std::condition_variable g_win_cv;
static bool g_resized;
static bool g_fullscreen;
static RECT g_windowed_rect;  // outer window rect to return to from fullscreen

constexpr DWORD WINDOWED_STYLE = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
constexpr DWORD FULLSCREEN_STYLE = WS_POPUP | WS_VISIBLE;

// Borderless fullscreen on the monitor the window is on.
static void set_fullscreen(HWND h, bool on) {
    if (on == g_fullscreen) return;
    if (on) {
        GetWindowRect(h, &g_windowed_rect);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, FULLSCREEN_STYLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongW(h, GWL_STYLE, WINDOWED_STYLE);
        const RECT& r = g_windowed_rect;
        SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_fullscreen = on;
    settings_set_fullscreen(on);
    g_resized = true;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: g_resized = true; return 0;
    case WM_CLOSE: ExitProcess(0);
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {  // Alt+Enter, not a repeat
            set_fullscreen(h, !g_fullscreen);
            return 0;
        }
        if (wp == VK_F10) return 0;  // F10 would open the (absent) window menu
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;  // no beep for Alt+Enter
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1 << 30))) {
            set_fullscreen(h, !g_fullscreen);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void window_thread() {
    const Settings& cfg = settings();
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"NewLegends";
    RegisterClassW(&wc);
    RECT r{0, 0, cfg.window_width, cfg.window_height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (cfg.window_x != -1 || cfg.window_y != -1) {
        // Keep a remembered position only while it is still on a monitor.
        RECT want{cfg.window_x, cfg.window_y, cfg.window_x + r.right - r.left, cfg.window_y + r.bottom - r.top};
        if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) x = cfg.window_x, y = cfg.window_y;
    }
    HWND h = CreateWindowW(L"NewLegends", L"" NL_PRODUCT_NAME " " NL_VERSION_STRING, WINDOWED_STYLE, x, y, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
    if (cfg.fullscreen) set_fullscreen(h, true);
    {
        std::lock_guard<std::mutex> lk(g_win_m);
        g_hwnd = h;
    }
    g_win_cv.notify_all();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool host_window_focused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }
void host_window_hide() {
    if (!g_hwnd) return;
    // A level change starts a new process: hand it the window position and size.
    RECT r = g_windowed_rect;
    if (!g_fullscreen) GetWindowRect(g_hwnd, &r);
    RECT client{0, 0, 0, 0};
    AdjustWindowRect(&client, WS_OVERLAPPEDWINDOW, FALSE);  // border sizes (negative left/top)
    settings_set_window(r.left, r.top, (r.right - r.left) - (client.right - client.left),
                        (r.bottom - r.top) - (client.bottom - client.top));
    ShowWindow(g_hwnd, SW_HIDE);
}

// --------------------------------------------------------------- device
static VkInstance g_inst;
static VkPhysicalDevice g_pdev;
static VkDevice g_dev;
static VkQueue g_queue;
static u32 g_qfam;
static VkSurfaceKHR g_surface;
static VkPhysicalDeviceMemoryProperties g_memprops;
static PFN_vkCmdPushDescriptorSetKHR p_vkCmdPushDescriptorSetKHR;

static VkSwapchainKHR g_swapchain;
static std::vector<VkImage> g_sc_images;
static std::vector<VkSemaphore> g_sc_done;  // per swapchain image
static VkExtent2D g_sc_extent;

constexpr u32 FRAMES = 2;
constexpr VkDeviceSize RING_SIZE = 96ull << 20;
struct Frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    VkBuffer ring;
    VkDeviceMemory ring_mem;
    u8* ring_ptr;
    VkDeviceSize ring_used;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> garbage;  // freed when the frame's fence signals
};
static Frame g_frames[FRAMES];
static u32 g_fi;

static u32 mem_type(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits >> i & 1) && (g_memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    fatal("no Vulkan memory type %X", want);
}

static VkDeviceMemory alloc_mem(VkMemoryRequirements req, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, want);
    VkDeviceMemory m;
    VKCHECK(vkAllocateMemory(g_dev, &ai, nullptr, &m));
    return m;
}

static void create_swapchain() {
    vkDeviceWaitIdle(g_dev);
    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pdev, g_surface, &caps));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFF) ext = {1280, 960};
    if (ext.width == 0 || ext.height == 0) return;  // minimized
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = caps.minImageCount + 1 > 3 ? caps.minImageCount + 1 : 3;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swapchain;
    VkSwapchainKHR sc;
    VKCHECK(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc));
    if (g_swapchain) vkDestroySwapchainKHR(g_dev, g_swapchain, nullptr);
    g_swapchain = sc;
    g_sc_extent = ext;
    u32 n = 0;
    vkGetSwapchainImagesKHR(g_dev, sc, &n, nullptr);
    g_sc_images.resize(n);
    vkGetSwapchainImagesKHR(g_dev, sc, &n, g_sc_images.data());
    for (VkSemaphore s : g_sc_done) vkDestroySemaphore(g_dev, s, nullptr);
    g_sc_done.resize(n);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : g_sc_done) VKCHECK(vkCreateSemaphore(g_dev, &si, nullptr, &s));
}

static VkCommandBuffer cmd() { return g_frames[g_fi].cmd; }

static double ms_since(const LARGE_INTEGER& t0);
static double g_fence_wait_ms;  // NL_TRACE_FPS: total time waiting for the GPU

static void begin_frame() {
    Frame& f = g_frames[g_fi];
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    VKCHECK(vkWaitForFences(g_dev, 1, &f.fence, VK_TRUE, UINT64_MAX));
    g_fence_wait_ms += ms_since(t0);
    VKCHECK(vkResetFences(g_dev, 1, &f.fence));
    for (auto& g : f.garbage) {
        vkDestroyBuffer(g_dev, g.first, nullptr);
        vkFreeMemory(g_dev, g.second, nullptr);
    }
    f.garbage.clear();
    f.ring_used = 0;
    VKCHECK(vkResetCommandPool(g_dev, f.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(f.cmd, &bi));
}

// Per-frame upload space (vertices, indices, uniforms, texture staging).
struct Upload {
    VkBuffer buf;
    VkDeviceSize off;
    u8* ptr;
};
static Upload upload(const void* data, VkDeviceSize size, VkDeviceSize align = 16) {
    Frame& f = g_frames[g_fi];
    VkDeviceSize off = (f.ring_used + align - 1) & ~(align - 1);
    if (off + size > RING_SIZE) {
        // Rare: a frame larger than the ring gets a one-off buffer.
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VkBuffer b;
        VKCHECK(vkCreateBuffer(g_dev, &bci, nullptr, &b));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_dev, b, &req);
        VkDeviceMemory m = alloc_mem(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKCHECK(vkBindBufferMemory(g_dev, b, m, 0));
        void* p;
        VKCHECK(vkMapMemory(g_dev, m, 0, size, 0, &p));
        if (data) memcpy(p, data, size);
        f.garbage.push_back({b, m});
        return {b, 0, (u8*)p};
    }
    f.ring_used = off + size;
    if (data) memcpy(f.ring_ptr + off, data, size);
    return {f.ring, off, f.ring_ptr + off};
}

// --------------------------------------------------------------- images
struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    u32 w = 0, h = 0, levels = 1, layers = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

static void create_image(Image& im, VkFormat fmt, u32 w, u32 h, u32 levels, u32 layers, VkImageUsageFlags usage,
                         bool cube) {
    im.fmt = fmt;
    im.w = w;
    im.h = h;
    im.levels = levels;
    im.layers = layers;
    im.aspect = fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                    : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VKCHECK(vkCreateImage(g_dev, &ci, nullptr, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, im.img, &req);
    im.mem = alloc_mem(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkBindImageMemory(g_dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    // Sampling a depth/stencil image reads depth.
    VkImageAspectFlags view_aspect = im.aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : im.aspect;
    vi.subresourceRange = {view_aspect, 0, levels, 0, layers};
    VKCHECK(vkCreateImageView(g_dev, &vi, nullptr, &im.view));
    im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

static void destroy_image(Image& im) {
    // Only called between frames or for images no command buffer in flight
    // references; callers wait for the device first.
    if (im.view) vkDestroyImageView(g_dev, im.view, nullptr);
    if (im.img) vkDestroyImage(g_dev, im.img, nullptr);
    if (im.mem) vkFreeMemory(g_dev, im.mem, nullptr);
    im = Image{};
}

// Conservative barrier: everything before, everything after.
static void transition(Image& im, VkImageLayout to) {
    if (im.layout == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.img;
    b.subresourceRange = {im.aspect, 0, im.levels, 0, im.layers};
    vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    im.layout = to;
}

// --------------------------------------------------------- render targets
static std::unordered_map<u32, Image> g_color_surfaces, g_zeta_surfaces;
static RenderTargets g_rt;
static Image* g_cur_color;
static Image* g_cur_zeta;
static bool g_rendering;
static VkRect2D g_scissor;

static void end_rendering() {
    if (!g_rendering) return;
    vkCmdEndRendering(cmd());
    g_rendering = false;
}

static Image* surface(std::unordered_map<u32, Image>& cache, u32 addr, VkFormat fmt, u32 w, u32 h) {
    Image& im = cache[addr];
    if (im.img && (im.fmt != fmt || im.w < w || im.h < h)) {
        // Grown or reformatted: the old image may still be in flight.
        end_rendering();
        vkDeviceWaitIdle(g_dev);
        w = w > im.w ? w : im.w;
        h = h > im.h ? h : im.h;
        destroy_image(im);
    }
    if (!im.img) {
        bool depth = fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        create_image(im, fmt, w, h, 1, 1, usage, false);
    }
    return &im;
}

void vkb_set_targets(const RenderTargets& rt) {
    if (rt.color_addr == g_rt.color_addr && rt.zeta_addr == g_rt.zeta_addr && rt.color == g_rt.color &&
        rt.zeta == g_rt.zeta && rt.width == g_rt.width && rt.height == g_rt.height)
        return;
    end_rendering();
    g_rt = rt;
    u32 w = rt.width ? rt.width : 1, h = rt.height ? rt.height : 1;
    g_cur_color = rt.color != ColorFmt::None && rt.color_addr
                      ? surface(g_color_surfaces, rt.color_addr, VK_FORMAT_B8G8R8A8_UNORM, w, h)
                      : nullptr;
    g_cur_zeta = rt.zeta != ZetaFmt::None && rt.zeta_addr
                     ? surface(g_zeta_surfaces, rt.zeta_addr, VK_FORMAT_D32_SFLOAT_S8_UINT, w, h)
                     : nullptr;
    g_scissor = {{0, 0}, {w, h}};
}

void vkb_set_scissor(int x, int y, int w, int h) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    g_scissor = {{x, y}, {(u32)(w > 0 ? w : 0), (u32)(h > 0 ? h : 0)}};
}

static bool begin_rendering() {
    if (g_rendering) return true;
    if (!g_cur_color && !g_cur_zeta) return false;
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo za{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    u32 w = 0, h = 0;
    if (g_cur_color) {
        transition(*g_cur_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ca.imageView = g_cur_color->view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        w = g_cur_color->w;
        h = g_cur_color->h;
    }
    if (g_cur_zeta) {
        transition(*g_cur_zeta, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        za.imageView = g_cur_zeta->view;
        za.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        za.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        za.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &za;
        ri.pStencilAttachment = &za;
        if (!g_cur_color || g_cur_zeta->w < w) w = g_cur_zeta->w;
        if (!g_cur_color || g_cur_zeta->h < h) h = g_cur_zeta->h;
    }
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    vkCmdBeginRendering(cmd(), &ri);
    g_rendering = true;
    return true;
}

void vkb_clear(u32 flags, u32 argb, float depth, u32 stencil, int x0, int y0, int x1, int y1) {
    if (!begin_rendering()) return;
    VkClearAttachment att[2];
    u32 n = 0;
    if ((flags & 0xF0) && g_cur_color) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        att[n].clearValue.color = {{((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f, (argb & 0xFF) / 255.f,
                                    (argb >> 24) / 255.f}};
        n++;
    }
    if ((flags & 3) && g_cur_zeta) {
        att[n].aspectMask = (flags & 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (flags & 2 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        att[n].clearValue.depthStencil = {depth, stencil};
        n++;
    }
    if (!n) return;
    u32 w = g_cur_color ? g_cur_color->w : g_cur_zeta->w, h = g_cur_color ? g_cur_color->h : g_cur_zeta->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)w - 1) x1 = w - 1;
    if (y1 > (int)h - 1) y1 = h - 1;
    if (x1 < x0 || y1 < y0) return;
    VkClearRect r{{{x0, y0}, {(u32)(x1 - x0 + 1), (u32)(y1 - y0 + 1)}}, 0, 1};
    vkCmdClearAttachments(cmd(), n, att, 1, &r);
}

BlitResult vkb_blit(u32 src, u32 dst, u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h, u32 src_pitch, u32, u32 bpp) {
    auto s = g_color_surfaces.find(src), d = g_color_surfaces.find(dst);
    if (s == g_color_surfaces.end() && d == g_color_surfaces.end()) return BlitResult::Memory;
    if (s == g_color_surfaces.end()) {
        if (bpp != 4) return BlitResult::Memory;
        // Guest pixels into a render target (BGRA, like the surface).
        Image& im = d->second;
        end_rendering();
        transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        Upload up = upload(nullptr, (VkDeviceSize)w * h * 4, 16);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                u32 px = rd32(src + (sy + y) * src_pitch + (sx + x) * 4) | 0xFF000000u;
                memcpy(up.ptr + (size_t)(y * w + x) * 4, &px, 4);
            }
        VkBufferImageCopy r{};
        r.bufferOffset = up.off;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {(int)dx, (int)dy, 0};
        r.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd(), up.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return BlitResult::Done;
    }
    if (d == g_color_surfaces.end()) return BlitResult::SourceIsTarget;
    if (s == d) return BlitResult::Done;  // overlapping self-copy: not used by D3D paths seen
    end_rendering();
    transition(s->second, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(d->second, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.srcOffset = {(int)sx, (int)sy, 0};
    c.dstOffset = {(int)dx, (int)dy, 0};
    c.extent = {w, h, 1};
    vkCmdCopyImage(cmd(), s->second.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->second.img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return BlitResult::Done;
}

// ------------------------------------------------------------ disk cache
// Shaders are generated while the game runs, so a first visit to a place
// would stall on GLSL compilation and pipeline creation. Both results are
// kept on disk for the next run:
//   cache/spirv.bin      header, then {u64 key, u32 words, SPIR-V words} records, appended as compiled
//   cache/pipelines.bin  the driver's VkPipelineCache blob (it ignores a blob from another GPU or driver)
static const char* SPIRV_CACHE = "cache/spirv.bin";
static const char* PIPELINE_CACHE = "cache/pipelines.bin";
// Bump when the GLSL compile options change: old SPIR-V is then ignored.
static const char SPIRV_MAGIC[8] = {'N', 'L', 'S', 'P', 'V', '0', '0', '1'};

static std::unordered_map<u64, std::vector<u32>> g_spirv_disk;
static FILE* g_spirv_out;
static VkPipelineCache g_pcache;
static u32 g_pipes_unsaved;

struct CacheStats {
    u32 compiled = 0, from_disk = 0, pipelines = 0;
    double compile_ms = 0, pipeline_ms = 0;
};
static CacheStats g_cstats, g_cstats_logged;

static double ms_since(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}

static u64 shader_key(const std::string& src, bool fragment) {
    u64 h = 1469598103934665603ull ^ (fragment ? 0x9E3779B97F4A7C15ull : 0);
    for (unsigned char ch : src) h = (h ^ ch) * 1099511628211ull;
    return h ^ src.size();
}

static void cache_load() {
    CreateDirectoryA("cache", nullptr);
    if (FILE* f = fopen(SPIRV_CACHE, "rb")) {
        char magic[8];
        bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, SPIRV_MAGIC, 8);
        while (ok) {
            u64 key;
            u32 words;
            if (fread(&key, 8, 1, f) != 1 || fread(&words, 4, 1, f) != 1 || words == 0 || words > (16u << 20)) break;
            std::vector<u32> code(words);
            if (fread(code.data(), 4, words, f) != words) break;  // a record cut short by a crash
            g_spirv_disk[key] = std::move(code);
        }
        fclose(f);
        if (!ok) remove(SPIRV_CACHE);
    }
    // Rewrite the file from what was read: drops a torn last record.
    if ((g_spirv_out = fopen(SPIRV_CACHE, "wb"))) {
        fwrite(SPIRV_MAGIC, 1, 8, g_spirv_out);
        for (auto& [key, code] : g_spirv_disk) {
            u32 words = (u32)code.size();
            fwrite(&key, 8, 1, g_spirv_out);
            fwrite(&words, 4, 1, g_spirv_out);
            fwrite(code.data(), 4, words, g_spirv_out);
        }
        fflush(g_spirv_out);
    }
    std::vector<u8> blob;
    if (FILE* f = fopen(PIPELINE_CACHE, "rb")) {
        fseek(f, 0, SEEK_END);
        blob.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        if (fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        VKCHECK(vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache));
    }
    log_msg("shader cache: %zu SPIR-V shaders, %zu KB of pipeline data", g_spirv_disk.size(), blob.size() / 1024);
}

// Called from the GPU thread, which also creates the pipelines.
static void cache_save_pipelines() {
    if (!g_pipes_unsaved) return;
    g_pipes_unsaved = 0;
    size_t size = 0;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<u8> blob(size);
    if (vkGetPipelineCacheData(g_dev, g_pcache, &size, blob.data()) != VK_SUCCESS) return;
    std::string tmp = std::string(PIPELINE_CACHE) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(blob.data(), 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if (ok) MoveFileExA(tmp.c_str(), PIPELINE_CACHE, MOVEFILE_REPLACE_EXISTING);
}

// --------------------------------------------------------------- shaders
static shaderc_compiler_t g_shaderc;
static std::unordered_map<std::string, VkShaderModule> g_modules;
static std::unordered_map<VkShaderModule, u32> g_module_ids;

static VkShaderModule make_module(const u32* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    VKCHECK(vkCreateShaderModule(g_dev, &ci, nullptr, &m));
    return m;
}

static VkShaderModule shader_module(const std::string& src, bool fragment) {
    auto it = g_modules.find(src);
    if (it != g_modules.end()) return it->second;
    u64 key = shader_key(src, fragment);
    auto disk = g_spirv_disk.find(key);
    if (disk != g_spirv_disk.end() && !getenv("NL_DUMP_SHADERS")) {
        VkShaderModule m = make_module(disk->second.data(), disk->second.size() * 4);
        g_cstats.from_disk++;
        g_module_ids[m] = (u32)g_modules.size();
        g_modules.emplace(src, m);
        return m;
    }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    // NL_DUMP_SHADERS: write every new shader to shaders/NNNN.vert|frag.
    static bool dump = getenv("NL_DUMP_SHADERS") != nullptr;
    if (dump) {
        CreateDirectoryA("shaders", nullptr);
        char name[64];
        snprintf(name, sizeof name, "shaders/%04zu.%s", g_modules.size(), fragment ? "frag" : "vert");
        if (FILE* f = fopen(name, "w")) {
            fputs(src.c_str(), f);
            fclose(f);
        }
    }
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_optimization_level(opt, shaderc_optimization_level_performance);
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t res =
        shaderc_compile_into_spv(g_shaderc, src.data(), src.size(),
                                 fragment ? shaderc_fragment_shader : shaderc_vertex_shader, "nv2a", "main", opt);
    shaderc_compile_options_release(opt);
    if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
        log_msg("shader compile failed:\n%s\n----\n%s", shaderc_result_get_error_message(res), src.c_str());
        fatal("GLSL compilation failed");
    }
    const u32* code = (const u32*)shaderc_result_get_bytes(res);
    size_t bytes = shaderc_result_get_length(res);
    VkShaderModule m = make_module(code, bytes);
    if (g_spirv_out) {
        u32 words = (u32)(bytes / 4);
        fwrite(&key, 8, 1, g_spirv_out);
        fwrite(&words, 4, 1, g_spirv_out);
        fwrite(code, 4, words, g_spirv_out);
        fflush(g_spirv_out);
    }
    g_spirv_disk[key].assign(code, code + bytes / 4);
    shaderc_result_release(res);
    g_cstats.compiled++;
    g_cstats.compile_ms += ms_since(t0);
    g_module_ids[m] = (u32)g_modules.size();
    g_modules.emplace(src, m);
    return m;
}

// Index of a shader in creation order (the NL_DUMP_SHADERS file number).
u32 vkb_shader_id(const std::string& src, bool fragment) {
    VkShaderModule m = shader_module(src, fragment);
    auto it = g_module_ids.find(m);
    return it != g_module_ids.end() ? it->second : ~0u;
}

// ------------------------------------------------------------- pipelines
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_playout;

struct PipeKey {
    VkShaderModule vs, fs;
    PipelineState ps;
    VertexLayout layout;
    VkFormat color, depth;
    bool operator==(const PipeKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
};
struct PipeKeyHash {
    size_t operator()(const PipeKey& k) const {
        const u8* p = (const u8*)&k;
        u64 h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(k); i++) h = (h ^ p[i]) * 1099511628211ull;
        return (size_t)h;
    }
};
static std::unordered_map<PipeKey, VkPipeline, PipeKeyHash> g_pipes;

static VkPipeline pipeline(const PipeKey& k) {
    auto it = g_pipes.find(k);
    if (it != g_pipes.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = k.vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = k.fs;
    st[1].pName = "main";

    VkVertexInputBindingDescription bind{0, k.layout.stride ? k.layout.stride : 4, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[16];
    u32 na = 0;
    for (u32 i = 0; i < 16; i++)
        if (k.layout.attr[i].format != VK_FORMAT_UNDEFINED) attrs[na++] = {i, 0, k.layout.attr[i].format, k.layout.attr[i].offset};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = (VkPrimitiveTopology)k.ps.topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = (VkPolygonMode)k.ps.polygon_mode;
    rs.cullMode = k.ps.cull_mode;
    rs.frontFace = (VkFrontFace)k.ps.front_face;
    rs.depthBiasEnable = VK_TRUE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.ps.depth_test;
    ds.depthWriteEnable = k.ps.depth_write;
    ds.depthCompareOp = (VkCompareOp)k.ps.depth_op;
    ds.stencilTestEnable = k.ps.stencil_test;
    ds.front = {(VkStencilOp)k.ps.st_fail, (VkStencilOp)k.ps.st_pass, (VkStencilOp)k.ps.st_zfail, (VkCompareOp)k.ps.st_op,
                k.ps.st_read_mask, k.ps.st_write_mask, k.ps.st_ref};
    ds.back = ds.front;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = k.ps.blend_enable;
    cb.srcColorBlendFactor = cb.srcAlphaBlendFactor = (VkBlendFactor)k.ps.blend_src;
    cb.dstColorBlendFactor = cb.dstAlphaBlendFactor = (VkBlendFactor)k.ps.blend_dst;
    cb.colorBlendOp = cb.alphaBlendOp = (VkBlendOp)k.ps.blend_op;
    cb.colorWriteMask = k.ps.color_mask;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = k.color != VK_FORMAT_UNDEFINED ? 1 : 0;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rinfo.colorAttachmentCount = cbs.attachmentCount;
    rinfo.pColorAttachmentFormats = &k.color;
    rinfo.depthAttachmentFormat = k.depth;
    rinfo.stencilAttachmentFormat = k.depth;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rinfo;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dy;
    ci.layout = g_playout;
    VkPipeline p;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    VKCHECK(vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &ci, nullptr, &p));
    g_cstats.pipelines++;
    g_cstats.pipeline_ms += ms_since(t0);
    g_pipes_unsaved++;
    g_pipes.emplace(k, p);
    return p;
}

// -------------------------------------------------------------- textures
struct TexEntry {
    Image im;
    u64 hash;
    u32 last_frame;
    u32 checked_frame = ~0u;  // the frame whose contents `hash` describes
};
static std::unordered_map<u64, TexEntry> g_textures;
static u32 g_frame_no;
u32 vkb_frame_no() { return g_frame_no; }
static Image g_dummy_tex, g_dummy_cube;
static std::map<u64, VkSampler> g_samplers;

static u64 fnv(const void* p, size_t n, u64 h = 1469598103934665603ull) {
    const u8* b = (const u8*)p;
    // Word-at-a-time FNV variant in four independent lanes, so the
    // multiplications overlap instead of waiting for each other.
    size_t i = 0;
    u64 l1 = h ^ 0x9E3779B97F4A7C15ull, l2 = h ^ 0xC2B2AE3D27D4EB4Full, l3 = h ^ 0x165667B19E3779F9ull;
    for (; i + 32 <= n; i += 32) {
        u64 w[4];
        memcpy(w, b + i, 32);
        h = (h ^ w[0]) * 1099511628211ull;
        l1 = (l1 ^ w[1]) * 1099511628211ull;
        l2 = (l2 ^ w[2]) * 1099511628211ull;
        l3 = (l3 ^ w[3]) * 1099511628211ull;
    }
    h = (h ^ l1 * 31) * 1099511628211ull;
    h = (h ^ l2 * 37) * 1099511628211ull;
    h = (h ^ l3 * 41) * 1099511628211ull;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void upload_texture(Image& im, const DecodedTexture& t) {
    transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize total = 0;
    for (auto& s : t.subs) total += s.pixels.size() * 4;
    Upload up = upload(nullptr, total, 16);
    VkDeviceSize off = 0;
    for (auto& s : t.subs) {
        memcpy(up.ptr + off, s.pixels.data(), s.pixels.size() * 4);
        VkBufferImageCopy r{};
        r.bufferOffset = up.off + off;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s.level, s.layer, 1};
        r.imageExtent = {s.w, s.h, 1};
        regions.push_back(r);
        off += s.pixels.size() * 4;
    }
    vkCmdCopyBufferToImage(cmd(), up.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (u32)regions.size(),
                           regions.data());
    transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static Image* texture(const TextureDesc& d) {
    // A render target sampled as a texture.
    auto rt = g_color_surfaces.find(d.addr);
    if (rt != g_color_surfaces.end() && !d.cube) {
        if (rt->second.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) end_rendering();
        transition(rt->second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return &rt->second;
    }
    u32 bytes = texture_size(d);
    if (!bytes) return d.cube ? &g_dummy_cube : &g_dummy_tex;
    u64 key = fnv(&d, offsetof(TextureDesc, address));
    TexEntry& e = g_textures[key];
    e.last_frame = g_frame_no;
    // Contents are checked once per frame: draws later in the same frame
    // reuse the result (hashing every texture per draw cost ~17% of the GPU
    // thread).
    if (e.im.img && e.checked_frame == g_frame_no) return &e.im;
    e.checked_frame = g_frame_no;
    u64 h = fnv(gptr(d.addr), bytes);
    if (d.palette_addr) h = fnv(gptr(d.palette_addr), d.palette_len * 4, h);
    if (e.im.img && e.hash == h) return &e.im;
    DecodedTexture t;
    if (!decode_texture(d, t)) return d.cube ? &g_dummy_cube : &g_dummy_tex;
    end_rendering();  // uploads are transfers
    if (!e.im.img) {
        create_image(e.im, VK_FORMAT_B8G8R8A8_UNORM, t.w, t.h, t.levels, d.cube ? 6 : 1,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, d.cube);
    }
    e.hash = h;
    upload_texture(e.im, t);
    return &e.im;
}

static VkSamplerAddressMode addr_mode(u32 m) {
    switch (m) {
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSampler sampler(const TextureDesc& d, u32 levels) {
    u64 key = ((u64)d.address << 32) | (d.filter & 0x0FFF0000) | (levels > 1 ? 1 : 0);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    u32 mag = (d.filter >> 24) & 0xF, min = (d.filter >> 16) & 0xFF;
    ci.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (min == 1 || min == 3 || min == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = (min == 5 || min == 6) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.maxLod = (min <= 2 || levels <= 1) ? 0.25f : 16.0f;  // LOD0 filters sample the base level only
    ci.addressModeU = addr_mode(d.address & 0xF);
    ci.addressModeV = addr_mode((d.address >> 8) & 0xF);
    ci.addressModeW = addr_mode((d.address >> 16) & 0xF);
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s;
    VKCHECK(vkCreateSampler(g_dev, &ci, nullptr, &s));
    g_samplers[key] = s;
    return s;
}

// ------------------------------------------------------------------ draw
void vkb_draw(const DrawCall& d) {
    if (d.indices.empty()) return;
    // Textures first: uploads are transfers, not allowed inside rendering.
    Image* tex[4];
    VkSampler smp[4];
    for (u32 i = 0; i < 4; i++) {
        tex[i] = nullptr;
        if (d.tex_mask >> i & 1) {
            tex[i] = texture(d.tex[i]);
            smp[i] = sampler(d.tex[i], tex[i]->levels);
        }
    }
    if (!begin_rendering()) return;

    PipeKey k{};
    // The generator hands out one string object per distinct shader, so the
    // pointer is enough to find its module without hashing the text.
    static std::unordered_map<const std::string*, VkShaderModule> by_ptr;
    auto mod = [&](const std::string* s, bool frag) {
        auto it = by_ptr.find(s);
        return it != by_ptr.end() ? it->second : (by_ptr[s] = shader_module(*s, frag));
    };
    k.vs = mod(d.vs, false);
    k.fs = mod(d.fs, true);
    k.ps = d.ps;
    k.layout = d.layout;
    k.color = g_cur_color ? g_cur_color->fmt : VK_FORMAT_UNDEFINED;
    k.depth = g_cur_zeta ? g_cur_zeta->fmt : VK_FORMAT_UNDEFINED;
    VkCommandBuffer cb = cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(k));

    u32 w = g_cur_color ? g_cur_color->w : g_cur_zeta->w, h = g_cur_color ? g_cur_color->h : g_cur_zeta->h;
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = g_scissor;
    if (sc.offset.x + sc.extent.width > w) sc.extent.width = w > (u32)sc.offset.x ? w - sc.offset.x : 0;
    if (sc.offset.y + sc.extent.height > h) sc.extent.height = h > (u32)sc.offset.y ? h - sc.offset.y : 0;
    vkCmdSetScissor(cb, 0, 1, &sc);
    float bc[4] = {((d.blend_color >> 16) & 0xFF) / 255.f, ((d.blend_color >> 8) & 0xFF) / 255.f,
                   (d.blend_color & 0xFF) / 255.f, (d.blend_color >> 24) / 255.f};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetDepthBias(cb, d.depth_bias_units, 0, d.depth_bias_factor);

    Upload vb = upload(d.vertices, d.vertex_bytes ? d.vertex_bytes : 4, 16);
    Upload ib = upload(d.indices.data(), d.indices.size() * 4, 4);
    Upload ub = upload(d.uniforms, d.uniform_bytes, 256);
    vkCmdBindVertexBuffers(cb, 0, 1, &vb.buf, &vb.off);
    vkCmdBindIndexBuffer(cb, ib.buf, ib.off, VK_INDEX_TYPE_UINT32);

    // One push for all bindings: uniforms, then per stage a 2D (1-4) and a
    // cube (5-8) sampler, the unused kind getting a placeholder.
    VkDescriptorBufferInfo bi{ub.buf, ub.off, d.uniform_bytes};
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet wr[9]{};
    for (auto& w : wr) {
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    wr[0].dstBinding = 0;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[0].pBufferInfo = &bi;
    VkSampler def = sampler(TextureDesc{}, 1);
    for (u32 i = 0; i < 4; i++) {
        bool cube = tex[i] && d.tex[i].cube;
        ii[i] = {def, g_dummy_tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[4 + i] = {def, g_dummy_cube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (tex[i]) ii[cube ? 4 + i : i] = {smp[i], tex[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wr[1 + i].dstBinding = 1 + i;
        wr[1 + i].pImageInfo = &ii[i];
        wr[5 + i].dstBinding = 5 + i;
        wr[5 + i].pImageInfo = &ii[4 + i];
    }
    p_vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_playout, 0, 9, wr);
    vkCmdDrawIndexed(cb, (u32)d.indices.size(), 1, 0, 0, 0);
}

// --------------------------------------------------------------- present
// NL_DUMP_FRAMES=n writes every n-th presented frame to frames/NNNNN.bmp.
static u32 g_dump_every = getenv("NL_DUMP_FRAMES") ? (u32)atoi(getenv("NL_DUMP_FRAMES")) : 0;

static void dump_frame(Image& src, u32 w, u32 h, u32 rt_addr = 0) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    VKCHECK(vkCreateBuffer(g_dev, &bci, nullptr, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, buf, &req);
    VkDeviceMemory mem = alloc_mem(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkBindBufferMemory(g_dev, buf, mem, 0));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_frames[g_fi].pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VKCHECK(vkBeginCommandBuffer(cb, &bi));
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE));
    vkQueueWaitIdle(g_queue);
    u8* px;
    VKCHECK(vkMapMemory(g_dev, mem, 0, bci.size, 0, (void**)&px));
    CreateDirectoryA("frames", nullptr);
    char name[64];
    if (rt_addr) snprintf(name, sizeof name, "frames/%05u_%08X.bmp", g_frame_no, rt_addr);
    else snprintf(name, sizeof name, "frames/%05u.bmp", g_frame_no);
    if (FILE* f = fopen(name, "wb")) {
        BITMAPFILEHEADER fh{0x4D42, (DWORD)(54 + bci.size), 0, 0, 54};
        BITMAPINFOHEADER ih{sizeof ih, (LONG)w, -(LONG)h, 1, 32, BI_RGB};
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        fwrite(px, 1, bci.size, f);
        fclose(f);
    }
    vkDestroyBuffer(g_dev, buf, nullptr);
    vkFreeMemory(g_dev, mem, nullptr);
}

// NL_TRACE_FPS: every 300 frames, the spread of frame times (present to
// present) and where the FIFO thread waited: for the next vblank (the game
// is ahead) or for the GPU (fence wait in begin_frame).
static bool g_trace_fps = getenv("NL_TRACE_FPS") != nullptr;
double video_vblank_wait_ms();  // video.cpp: total time spent in video_wait_vblank
double pfifo_idle_ms();         // pfifo.cpp: total time the FIFO waited for the game

static void trace_fps() {
    static std::vector<double> intervals;
    static LARGE_INTEGER last;
    static double fence0, vblank0, idle0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last.QuadPart) intervals.push_back(ms_since(last));
    last = now;
    if (intervals.size() < 300) return;
    std::vector<double> v = intervals;
    std::sort(v.begin(), v.end());
    double sum = 0;
    u32 long_frames = 0;
    for (double x : v) {
        sum += x;
        if (x > 25) long_frames++;
    }
    double vb = video_vblank_wait_ms();
    log_msg("fps: %.1f, frame ms p50 %.1f p90 %.1f p99 %.1f max %.1f, %u over 25 ms; per frame: vblank wait %.1f ms, "
            "gpu wait %.1f ms, waiting for the game %.1f ms",
            1000.0 * v.size() / sum, v[v.size() / 2], v[v.size() * 9 / 10], v[v.size() * 99 / 100], v.back(), long_frames,
            (vb - vblank0) / v.size(), (g_fence_wait_ms - fence0) / v.size(), (pfifo_idle_ms() - idle0) / v.size());
    idle0 = pfifo_idle_ms();
    vblank0 = vb;
    fence0 = g_fence_wait_ms;
    intervals.clear();
}

void vkb_present(u32 color_addr) {
    if (g_trace_fps) trace_fps();
    end_rendering();
    Frame& f = g_frames[g_fi];
    auto it = g_color_surfaces.find(color_addr);
    if (g_resized) {
        g_resized = false;
        create_swapchain();
    }
    u32 idx = 0, dump_w = 0, dump_h = 0;
    std::vector<std::pair<u32, Image*>> dump_rts;
    Image* dump_src = nullptr;
    bool have_image = false;
    if (g_swapchain) {
        VkResult r = vkAcquireNextImageKHR(g_dev, g_swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            g_resized = true;
        } else {
            have_image = true;
        }
    }
    if (have_image) {
        VkImage dst = g_sc_images[idx];
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dst;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkClearColorValue black{};
        VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd(), dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
        if (it != g_color_surfaces.end()) {
            Image& src = it->second;
            transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            // Fit the 4:3 frame into the window, keeping the aspect ratio unless stretched.
            u32 sw = g_rt.color_addr == color_addr && g_rt.width ? g_rt.width : src.w;
            u32 sh = g_rt.color_addr == color_addr && g_rt.height ? g_rt.height : src.h;
            if (sw > src.w) sw = src.w;
            if (sh > src.h) sh = src.h;
            float scale = (float)g_sc_extent.width / sw < (float)g_sc_extent.height / sh
                              ? (float)g_sc_extent.width / sw
                              : (float)g_sc_extent.height / sh;
            int dw = (int)(sw * scale), dh = (int)(sh * scale);
            if (settings().aspect == Aspect::Stretch) dw = (int)g_sc_extent.width, dh = (int)g_sc_extent.height;
            int dx = ((int)g_sc_extent.width - dw) / 2, dy = ((int)g_sc_extent.height - dh) / 2;
            VkImageBlit bl{};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.srcOffsets[1] = {(int)sw, (int)sh, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.dstOffsets[0] = {dx, dy, 0};
            bl.dstOffsets[1] = {dx + dw, dy + dh, 1};
            vkCmdBlitImage(cmd(), src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &bl, settings().smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            if (g_dump_every && g_frame_no % g_dump_every == 0) {
                dump_w = sw, dump_h = sh, dump_src = &src;
                // NL_DUMP_RT=addr,addr: these render targets too (debugging
                // render-to-texture effects such as the water simulation).
                if (const char* list = getenv("NL_DUMP_RT"))
                    for (const char* p = list; *p;) {
                        u32 a = (u32)strtoul(p, (char**)&p, 16);
                        auto rt = g_color_surfaces.find(a);
                        if (rt != g_color_surfaces.end() && &rt->second != &src) {
                            transition(rt->second, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                            dump_rts.push_back({a, &rt->second});
                        }
                        while (*p == ',') p++;
                    }
            }
        }
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    }
    VKCHECK(vkEndCommandBuffer(f.cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    if (have_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &g_sc_done[idx];
    }
    VKCHECK(vkQueueSubmit(g_queue, 1, &si, f.fence));
    if (have_image) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g_sc_done[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_swapchain;
        pi.pImageIndices = &idx;
        VkResult r = vkQueuePresentKHR(g_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_resized = true;
    }
    if (dump_src) {
        vkQueueWaitIdle(g_queue);
        dump_frame(*dump_src, dump_w, dump_h);
        for (auto& d : dump_rts) dump_frame(*d.second, d.second->w, d.second->h, d.first);
    }
    g_fi = (g_fi + 1) % FRAMES;
    g_frame_no++;
    if (g_frame_no % 300 == 0) {
        cache_save_pipelines();
        if (memcmp(&g_cstats, &g_cstats_logged, sizeof g_cstats)) {
            log_msg("shader cache: %u compiled (%.0f ms), %u from disk; %u pipelines created (%.0f ms)",
                    g_cstats.compiled, g_cstats.compile_ms, g_cstats.from_disk, g_cstats.pipelines,
                    g_cstats.pipeline_ms);
            g_cstats_logged = g_cstats;
        }
    }
    begin_frame();
    // Image layouts persist across command buffers; nothing to reset.
}

// ------------------------------------------------------------------ init
void vkb_init() {
    std::thread(window_thread).detach();
    {
        std::unique_lock<std::mutex> lk(g_win_m);
        g_win_cv.wait(lk, [] { return g_hwnd != nullptr; });
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "New Legends";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* inst_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_ext;
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    if (getenv("NL_VK_VALIDATION")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
    }
    VKCHECK(vkCreateInstance(&ici, nullptr, &g_inst));

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = g_hwnd;
    VKCHECK(vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &g_surface));

    u32 n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g_inst, &n, devs.data());
    // Prefer a discrete GPU.
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (!g_pdev || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) g_pdev = d;
    }
    if (!g_pdev) fatal("no Vulkan device");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_pdev, &props);
    log_msg("vulkan: %s", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(g_pdev, &g_memprops);

    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &n, qf.data());
    g_qfam = ~0u;
    for (u32 i = 0; i < n; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_pdev, i, g_surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            g_qfam = i;
            break;
        }
    }
    if (g_qfam == ~0u) fatal("no Vulkan queue with graphics and present");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.fillModeNonSolid = VK_TRUE;
    f2.features.samplerAnisotropy = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = dev_ext;
    VKCHECK(vkCreateDevice(g_pdev, &dci, nullptr, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    p_vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(g_dev, "vkCmdPushDescriptorSetKHR");

    create_swapchain();

    // Descriptor layout: uniforms, then 2D samplers 1-4 and cube samplers 5-8.
    VkDescriptorSetLayoutBinding b[9]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (u32 i = 1; i < 9; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 9;
    dl.pBindings = b;
    VKCHECK(vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &g_dsl));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_dsl;
    VKCHECK(vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_playout));

    for (Frame& f : g_frames) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = g_qfam;
        VKCHECK(vkCreateCommandPool(g_dev, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(g_dev, &cai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHECK(vkCreateFence(g_dev, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(g_dev, &si, nullptr, &f.acquired));
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = RING_SIZE;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VKCHECK(vkCreateBuffer(g_dev, &bci, nullptr, &f.ring));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_dev, f.ring, &req);
        f.ring_mem = alloc_mem(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKCHECK(vkBindBufferMemory(g_dev, f.ring, f.ring_mem, 0));
        VKCHECK(vkMapMemory(g_dev, f.ring_mem, 0, RING_SIZE, 0, (void**)&f.ring_ptr));
    }
    g_shaderc = shaderc_compiler_initialize();
    cache_load();

    begin_frame();
    // Placeholder textures for unbound stages: opaque black.
    create_image(g_dummy_tex, VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 1,
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
    create_image(g_dummy_cube, VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 6,
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
    DecodedTexture black;
    black.w = black.h = 1;
    black.levels = 1;
    black.subs.push_back({0, 0, 1, 1, {0xFF000000u}});
    upload_texture(g_dummy_tex, black);
    for (u32 i = 1; i < 6; i++) black.subs.push_back({0, i, 1, 1, {0xFF000000u}});
    upload_texture(g_dummy_cube, black);
}
