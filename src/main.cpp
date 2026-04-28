#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#endif

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

using json = nlohmann::json;

namespace {

std::atomic_bool g_stop{false};

struct CameraConfig {
    std::string name;
    std::string input_url;
    std::string host;
    int port = 554;
    std::string username;
    std::string password;
    std::string path = "/stream1";
    std::string output_url;
    std::string rtsp_transport = "tcp";
    int reconnect_delay_ms = 3000;
    int open_timeout_ms = 5000;
    int read_timeout_ms = 5000;
    int analyze_duration_us = 0;
    int probe_size = 32768;
    int max_delay_us = 500000;
    bool fast_open = false;
};

std::string ff_error(int errnum) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buffer, sizeof(buffer));
    return buffer;
}

void signal_handler(int) {
    g_stop.store(true);
}

std::string trim_left_slash(std::string value) {
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    return value;
}

std::string strip_host_port(const std::string& host) {
    if (host.empty() || host.front() == '[') {
        return host;
    }

    const auto colon = host.rfind(':');
    if (colon == std::string::npos) {
        return host;
    }

    const std::string port = host.substr(colon + 1);
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) {
        return host;
    }

    return host.substr(0, colon);
}

bool is_url_unreserved(unsigned char value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           value == '-' || value == '.' || value == '_' || value == '~';
}

std::string url_encode_component(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;

    for (const unsigned char ch : value) {
        if (is_url_unreserved(ch)) {
            encoded << ch;
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }

    return encoded.str();
}

int64_t estimate_packet_duration(const AVStream* stream, const AVPacket* packet) {
    if (packet->duration > 0) {
        return packet->duration;
    }

    const AVCodecParameters* codec = stream->codecpar;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        AVRational frame_rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
        if (frame_rate.num > 0 && frame_rate.den > 0) {
            const int64_t duration = av_rescale_q(1, av_inv_q(frame_rate), stream->time_base);
            if (duration > 0) {
                return duration;
            }
        }
    }

    return 1;
}

void normalize_packet_timestamps(AVPacket* packet, const AVStream* stream, std::vector<int64_t>& next_dts) {
    const int stream_index = packet->stream_index;
    const int64_t duration = estimate_packet_duration(stream, packet);

    if (packet->duration <= 0) {
        packet->duration = duration;
    }

    if (packet->pts == AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE) {
        packet->pts = packet->dts;
    }
    if (packet->dts == AV_NOPTS_VALUE && packet->pts != AV_NOPTS_VALUE) {
        packet->dts = packet->pts;
    }
    if (packet->pts == AV_NOPTS_VALUE && packet->dts == AV_NOPTS_VALUE) {
        packet->dts = next_dts[stream_index] == AV_NOPTS_VALUE ? 0 : next_dts[stream_index];
        packet->pts = packet->dts;
    }

    if (next_dts[stream_index] != AV_NOPTS_VALUE && packet->dts < next_dts[stream_index]) {
        const int64_t delta = next_dts[stream_index] - packet->dts;
        packet->dts += delta;
        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts += delta;
        }
    }

    if (packet->pts < packet->dts) {
        packet->pts = packet->dts;
    }

    next_dts[stream_index] = packet->dts + std::max<int64_t>(packet->duration, 1);
}

std::string build_rtsp_url(const CameraConfig& camera) {
    if (!camera.input_url.empty()) {
        return camera.input_url;
    }

    std::ostringstream url;
    url << "rtsp://";
    if (!camera.username.empty()) {
        url << url_encode_component(camera.username);
        if (!camera.password.empty()) {
            url << ":" << url_encode_component(camera.password);
        }
        url << "@";
    }
    url << strip_host_port(camera.host) << ":" << camera.port << "/" << trim_left_slash(camera.path);
    return url.str();
}

std::string masked_url(const CameraConfig& camera) {
    if (!camera.input_url.empty()) {
        return camera.name + " input_url";
    }

    std::ostringstream url;
    url << "rtsp://";
    if (!camera.username.empty()) {
        url << camera.username << ":***@";
    }
    url << strip_host_port(camera.host) << ":" << camera.port << "/" << trim_left_slash(camera.path);
    return url.str();
}

CameraConfig parse_camera(const json& item) {
    CameraConfig camera;
    camera.name = item.value("name", "");
    camera.input_url = item.value("input_url", "");
    camera.host = item.value("host", "");
    camera.port = item.value("port", 554);
    camera.username = item.value("username", "");
    camera.password = item.value("password", "");
    camera.path = item.value("path", "/stream1");
    camera.output_url = item.value("output_url", "");
    camera.rtsp_transport = item.value("rtsp_transport", "tcp");
    camera.reconnect_delay_ms = item.value("reconnect_delay_ms", 3000);
    camera.open_timeout_ms = item.value("open_timeout_ms", 5000);
    camera.read_timeout_ms = item.value("read_timeout_ms", 5000);
    camera.analyze_duration_us = item.value("analyze_duration_us", 0);
    camera.probe_size = item.value("probe_size", 32768);
    camera.max_delay_us = item.value("max_delay_us", 500000);
    camera.fast_open = item.value("fast_open", false);

    if (camera.name.empty()) {
        camera.name = camera.host.empty() ? "camera" : camera.host;
    }

    if (camera.input_url.empty() && camera.host.empty()) {
        throw std::runtime_error("camera '" + camera.name + "' precisa de input_url ou host");
    }

    return camera;
}

std::vector<CameraConfig> load_config(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("nao foi possivel abrir " + path.string());
    }

    const json root = json::parse(file);
    std::vector<CameraConfig> cameras;

    for (const auto& item : root.at("cameras")) {
        cameras.push_back(parse_camera(item));
    }

    if (cameras.empty()) {
        throw std::runtime_error("configuracao sem cameras");
    }

    return cameras;
}

#ifdef _WIN32

std::wstring widen_ascii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

struct CameraState {
    std::string name;
    std::mutex mutex;
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    bool connected = false;
    std::string status = "conectando";
};

class MosaicWindow {
public:
    explicit MosaicWindow(std::vector<std::shared_ptr<CameraState>> states)
        : states_(std::move(states)) {
        register_class();

        RECT rect{0, 0, 1280, 720};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd_ = CreateWindowExW(
            0,
            class_name(),
            L"RTSP Lite Player",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);

        if (!hwnd_) {
            throw std::runtime_error("nao foi possivel criar janela do mosaico");
        }

        SetTimer(hwnd_, 1, 33, nullptr);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }

    ~MosaicWindow() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }

    bool pump_messages() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_stop.store(true);
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return !g_stop.load();
    }

    bool is_open() const {
        return hwnd_ != nullptr && IsWindow(hwnd_) && !g_stop.load();
    }

private:
    static const wchar_t* class_name() {
        return L"RtspLitePlayerMosaicWindow";
    }

    static void register_class() {
        static std::once_flag once;
        std::call_once(once, [] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = &MosaicWindow::window_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = class_name();
            RegisterClassExW(&wc);
        });
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            auto* self = static_cast<MosaicWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }

        auto* self = reinterpret_cast<MosaicWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        switch (message) {
            case WM_ERASEBKGND:
                return 1;
            case WM_LBUTTONDBLCLK:
                self->handle_double_click(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                self->draw(dc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_TIMER:
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_CLOSE:
                g_stop.store(true);
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                self->hwnd_ = nullptr;
                g_stop.store(true);
                return 0;
            default:
                return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    static std::pair<int, int> grid_for_count(size_t count) {
        if (count <= 1) {
            return {1, 1};
        }
        const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
        const int rows = static_cast<int>((count + columns - 1) / columns);
        return {columns, rows};
    }

    void draw(HDC dc) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int client_width = client.right - client.left;
        const int client_height = client.bottom - client.top;
        if (client_width <= 0 || client_height <= 0) {
            return;
        }

        HDC memory_dc = CreateCompatibleDC(dc);
        HBITMAP memory_bitmap = CreateCompatibleBitmap(dc, client_width, client_height);
        HGDIOBJ old_bitmap = SelectObject(memory_dc, memory_bitmap);

        draw_mosaic(memory_dc, client);

        BitBlt(dc, 0, 0, client_width, client_height, memory_dc, 0, 0, SRCCOPY);

        SelectObject(memory_dc, old_bitmap);
        DeleteObject(memory_bitmap);
        DeleteDC(memory_dc);
    }

    void draw_mosaic(HDC dc, const RECT& client) {
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        if (states_.empty()) {
            return;
        }

        if (focused_index_ >= 0 && static_cast<size_t>(focused_index_) < states_.size()) {
            draw_tile(dc, client, *states_[focused_index_]);
            return;
        }

        const int client_width = client.right - client.left;
        const int client_height = client.bottom - client.top;
        const auto [columns, rows] = grid_for_count(states_.size());
        const int tile_width = std::max(1, client_width / columns);
        const int tile_height = std::max(1, client_height / rows);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 235));
        SetStretchBltMode(dc, COLORONCOLOR);

        for (size_t i = 0; i < states_.size(); ++i) {
            const int col = static_cast<int>(i) % columns;
            const int row = static_cast<int>(i) / columns;
            RECT tile{
                col * tile_width,
                row * tile_height,
                col == columns - 1 ? client_width : (col + 1) * tile_width,
                row == rows - 1 ? client_height : (row + 1) * tile_height};

            draw_tile(dc, tile, *states_[i]);
        }
    }

    void handle_double_click(int x, int y) {
        if (focused_index_ >= 0) {
            focused_index_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        const int index = tile_index_at(x, y);
        if (index >= 0) {
            focused_index_ = index;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    int tile_index_at(int x, int y) const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int client_width = client.right - client.left;
        const int client_height = client.bottom - client.top;
        if (client_width <= 0 || client_height <= 0 || states_.empty()) {
            return -1;
        }

        const auto [columns, rows] = grid_for_count(states_.size());
        const int tile_width = std::max(1, client_width / columns);
        const int tile_height = std::max(1, client_height / rows);
        const int col = std::min(columns - 1, std::max(0, x / tile_width));
        const int row = std::min(rows - 1, std::max(0, y / tile_height));
        const int index = row * columns + col;

        return index >= 0 && static_cast<size_t>(index) < states_.size() ? index : -1;
    }

    void draw_tile(HDC dc, const RECT& tile, CameraState& state) {
        std::vector<uint8_t> frame;
        std::string label;
        int frame_width = 0;
        int frame_height = 0;
        bool connected = false;
        std::string status;

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            frame = state.bgra;
            label = state.name;
            frame_width = state.width;
            frame_height = state.height;
            connected = state.connected;
            status = state.status;
        }

        RECT inner = tile;
        InflateRect(&inner, -2, -2);

        if (frame.empty() || frame_width <= 0 || frame_height <= 0) {
            FillRect(dc, &inner, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            draw_label(dc, inner, label + " - " + status);
            return;
        }

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = frame_width;
        bitmap_info.bmiHeader.biHeight = -frame_height;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        const int area_width = inner.right - inner.left;
        const int area_height = inner.bottom - inner.top;
        const double scale = std::min(
            static_cast<double>(area_width) / frame_width,
            static_cast<double>(area_height) / frame_height);
        const int draw_width = std::max(1, static_cast<int>(frame_width * scale));
        const int draw_height = std::max(1, static_cast<int>(frame_height * scale));
        const int x = inner.left + (area_width - draw_width) / 2;
        const int y = inner.top + (area_height - draw_height) / 2;

        StretchDIBits(
            dc,
            x,
            y,
            draw_width,
            draw_height,
            0,
            0,
            frame_width,
            frame_height,
            frame.data(),
            &bitmap_info,
            DIB_RGB_COLORS,
            SRCCOPY);

        draw_label(dc, inner, connected ? label : label + " - " + status);
    }

    void draw_label(HDC dc, const RECT& tile, const std::string& text) {
        RECT background{tile.left, tile.top, tile.right, tile.top + 24};
        HBRUSH brush = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(dc, &background, brush);
        DeleteObject(brush);

        RECT text_rect{tile.left + 8, tile.top + 4, tile.right - 8, tile.top + 22};
        DrawTextW(dc, widen_ascii(text).c_str(), -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    HWND hwnd_ = nullptr;
    std::vector<std::shared_ptr<CameraState>> states_;
    int focused_index_ = -1;
};

int play_once(const CameraConfig& camera, const std::shared_ptr<CameraState>& state) {
    AVFormatContext* input_context = nullptr;
    AVCodecContext* decoder_context = nullptr;
    SwsContext* sws_context = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int ret = 0;
    int video_stream_index = -1;
    std::vector<uint8_t> bgra_buffer;

    if (!packet || !frame) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->connected = false;
        state->status = "conectando";
    }

    {
        const std::string input_url = build_rtsp_url(camera);
        AVDictionary* input_options = nullptr;
        const std::string open_timeout_us = std::to_string(camera.open_timeout_ms * 1000);
        const std::string read_timeout_us = std::to_string(camera.read_timeout_ms * 1000);
        const std::string analyze_duration_us = std::to_string(camera.analyze_duration_us);
        const std::string probe_size = std::to_string(camera.probe_size);
        const std::string max_delay_us = std::to_string(camera.max_delay_us);

        av_dict_set(&input_options, "rtsp_transport", camera.rtsp_transport.c_str(), 0);
        av_dict_set(&input_options, "stimeout", open_timeout_us.c_str(), 0);
        av_dict_set(&input_options, "rw_timeout", read_timeout_us.c_str(), 0);
        av_dict_set(&input_options, "fflags", "nobuffer", 0);
        av_dict_set(&input_options, "flags", "low_delay", 0);
        av_dict_set(&input_options, "probesize", probe_size.c_str(), 0);
        av_dict_set(&input_options, "analyzeduration", analyze_duration_us.c_str(), 0);
        av_dict_set(&input_options, "max_delay", max_delay_us.c_str(), 0);
        if (camera.fast_open) {
            av_dict_set(&input_options, "allowed_media_types", "video", 0);
        }

        std::cout << "[" << camera.name << "] abrindo " << masked_url(camera) << "\n";
        ret = avformat_open_input(&input_context, input_url.c_str(), nullptr, &input_options);
        av_dict_free(&input_options);
        if (ret < 0) {
            std::cerr << "[" << camera.name << "] erro ao abrir entrada: " << ff_error(ret) << "\n";
            std::lock_guard<std::mutex> lock(state->mutex);
            state->status = "erro ao conectar";
            goto cleanup;
        }
    }

    input_context->flags |= AVFMT_FLAG_NOBUFFER;

    if (!camera.fast_open) {
        ret = avformat_find_stream_info(input_context, nullptr);
        if (ret < 0) {
            std::cerr << "[" << camera.name << "] erro ao ler streams: " << ff_error(ret) << "\n";
            std::lock_guard<std::mutex> lock(state->mutex);
            state->status = "erro no stream";
            goto cleanup;
        }
    }

    video_stream_index = av_find_best_stream(input_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0 && camera.fast_open) {
        std::cerr << "[" << camera.name << "] fast_open nao encontrou video, tentando modo normal\n";
        ret = avformat_find_stream_info(input_context, nullptr);
        if (ret >= 0) {
            video_stream_index = av_find_best_stream(input_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        }
    }
    if (video_stream_index < 0) {
        ret = video_stream_index;
        std::cerr << "[" << camera.name << "] nenhum stream de video encontrado\n";
        std::lock_guard<std::mutex> lock(state->mutex);
        state->status = "sem video";
        goto cleanup;
    }

    for (unsigned int i = 0; i < input_context->nb_streams; ++i) {
        input_context->streams[i]->discard = i == static_cast<unsigned int>(video_stream_index)
            ? AVDISCARD_DEFAULT
            : AVDISCARD_ALL;
    }

    {
        AVCodecParameters* codec_params = input_context->streams[video_stream_index]->codecpar;
        const AVCodec* decoder = avcodec_find_decoder(codec_params->codec_id);
        if (!decoder) {
            ret = AVERROR_DECODER_NOT_FOUND;
            std::cerr << "[" << camera.name << "] decoder nao encontrado\n";
            std::lock_guard<std::mutex> lock(state->mutex);
            state->status = "sem decoder";
            goto cleanup;
        }

        decoder_context = avcodec_alloc_context3(decoder);
        if (!decoder_context) {
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        ret = avcodec_parameters_to_context(decoder_context, codec_params);
        if (ret < 0) {
            std::cerr << "[" << camera.name << "] erro ao preparar decoder: " << ff_error(ret) << "\n";
            goto cleanup;
        }

        decoder_context->thread_count = 0;
        decoder_context->flags |= AV_CODEC_FLAG_LOW_DELAY;

        ret = avcodec_open2(decoder_context, decoder, nullptr);
        if (ret < 0) {
            std::cerr << "[" << camera.name << "] erro ao abrir decoder: " << ff_error(ret) << "\n";
            std::lock_guard<std::mutex> lock(state->mutex);
            state->status = "erro no decoder";
            goto cleanup;
        }
    }

    {
        std::cout << "[" << camera.name << "] player ao vivo iniciado\n";
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->connected = true;
            state->status = "ao vivo";
        }

        while (!g_stop.load()) {
            ret = av_read_frame(input_context, packet);
            if (ret < 0) {
                std::cerr << "[" << camera.name << "] leitura encerrada: " << ff_error(ret) << "\n";
                std::lock_guard<std::mutex> lock(state->mutex);
                state->connected = false;
                state->status = "reconectando";
                break;
            }

            if (packet->stream_index != video_stream_index) {
                av_packet_unref(packet);
                continue;
            }

            ret = avcodec_send_packet(decoder_context, packet);
            av_packet_unref(packet);
            if (ret < 0) {
                std::cerr << "[" << camera.name << "] erro ao enviar pacote ao decoder: " << ff_error(ret) << "\n";
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(decoder_context, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    ret = 0;
                    break;
                }
                if (ret < 0) {
                    std::cerr << "[" << camera.name << "] erro ao decodificar frame: " << ff_error(ret) << "\n";
                    goto cleanup;
                }

                sws_context = sws_getCachedContext(
                    sws_context,
                    frame->width,
                    frame->height,
                    static_cast<AVPixelFormat>(frame->format),
                    frame->width,
                    frame->height,
                    AV_PIX_FMT_BGRA,
                    SWS_FAST_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr);
                if (!sws_context) {
                    ret = AVERROR(EINVAL);
                    std::cerr << "[" << camera.name << "] erro ao converter frame\n";
                    goto cleanup;
                }

                bgra_buffer.resize(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4);
                uint8_t* dst_data[4] = {bgra_buffer.data(), nullptr, nullptr, nullptr};
                int dst_linesize[4] = {frame->width * 4, 0, 0, 0};

                sws_scale(
                    sws_context,
                    frame->data,
                    frame->linesize,
                    0,
                    frame->height,
                    dst_data,
                    dst_linesize);

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->bgra = bgra_buffer;
                    state->width = frame->width;
                    state->height = frame->height;
                    state->connected = true;
                    state->status = "ao vivo";
                }
                av_frame_unref(frame);
            }
        }
    }

cleanup:
    sws_freeContext(sws_context);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder_context);
    avformat_close_input(&input_context);
    return ret;
}

void run_camera(CameraConfig camera, std::shared_ptr<CameraState> state) {
    while (!g_stop.load()) {
        const int ret = play_once(camera, state);
        if (g_stop.load()) {
            break;
        }

        std::cerr << "[" << camera.name << "] reconectando em "
                  << camera.reconnect_delay_ms << " ms apos erro " << ff_error(ret) << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(camera.reconnect_delay_ms));
    }
}

#else

void run_camera(CameraConfig, std::shared_ptr<CameraState>) {
    throw std::runtime_error("este player usa a API do Windows e precisa ser compilado no Windows");
}

#endif

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    const std::filesystem::path config_path = argc > 1 ? argv[1] : "config.json";

    try {
        avformat_network_init();

        const auto cameras = load_config(config_path);
        std::vector<std::shared_ptr<CameraState>> states;
        states.reserve(cameras.size());

        for (const auto& camera : cameras) {
            auto state = std::make_shared<CameraState>();
            state->name = camera.name;
            states.push_back(state);
        }

        std::vector<std::thread> threads;
        threads.reserve(cameras.size());

        for (size_t i = 0; i < cameras.size(); ++i) {
            threads.emplace_back(run_camera, cameras[i], states[i]);
        }

#ifdef _WIN32
        MosaicWindow window(states);
        while (window.is_open() && window.pump_messages()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
#else
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#endif

        g_stop.store(true);

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        avformat_network_deinit();
    } catch (const std::exception& ex) {
        std::cerr << "erro: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
