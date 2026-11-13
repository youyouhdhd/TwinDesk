// TwinDesk — Copyright (C) 2026 inukaasith. GPL-3.0, see LICENSE.
// WGC includes Windows' accessibility cursor. Share native BGRA textures, without resizing.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

struct Slot {
    com_ptr<ID3D11Texture2D> texture;
    HANDLE local = nullptr, remote = nullptr;
    bool busy = false;
};

int main(int argc, char** argv) {
    HANDLE parent = nullptr;
    std::array<Slot, 3> slots;
    std::mutex mutex;
    try {
        if (argc != 7) throw std::invalid_argument("Expected parent PID, x, y, width, height and fps");
        auto number = [](const char* value) {
            size_t end = 0;
            int result = std::stoi(value, &end);
            if (value[end]) throw std::invalid_argument("Invalid number");
            return result;
        };
        const int pid = number(argv[1]), x = number(argv[2]), y = number(argv[3]);
        const int askedWidth = number(argv[4]), askedHeight = number(argv[5]), fps = number(argv[6]);
        if (pid <= 0 || askedWidth < 2 || askedHeight < 2 || askedWidth > 32768 || askedHeight > 32768 || fps < 1 || fps > 240)
            throw std::invalid_argument("Invalid capture bounds or frame rate");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        init_apartment(apartment_type::multi_threaded);
        if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Windows Graphics Capture is unavailable");
        parent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        if (!parent) throw_last_error();

        com_ptr<ID3D11Device> device;
        com_ptr<ID3D11DeviceContext> context;
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
        auto dxgi = device.as<IDXGIDevice>();
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        auto direct3d = inspectable.as<IDirect3DDevice>();

        HMONITOR monitor = MonitorFromPoint({ x + askedWidth / 2, y + askedHeight / 2 }, MONITOR_DEFAULTTONULL);
        MONITORINFO monitorInfo{ sizeof(MONITORINFO) };
        if (!monitor || !GetMonitorInfo(monitor, &monitorInfo)) throw std::runtime_error("The selected monitor changed");
        // Electron's DIP-to-pixel conversion can be off by a few pixels on a scaled monitor that is not at
        // the origin (1920 x 1200 at 125% came back as 1920 x 1202). Capture the monitor's real pixels.
        const RECT& real = monitorInfo.rcMonitor;
        const int width = real.right - real.left, height = real.bottom - real.top;
        const int slack = 4;
        if (std::abs(real.left - x) > slack || std::abs(real.top - y) > slack ||
            std::abs(width - askedWidth) > slack || std::abs(height - askedHeight) > slack)
            throw std::runtime_error("The selected monitor changed");
        GraphicsCaptureItem item{ nullptr };
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForMonitor(monitor, guid_of<GraphicsCaptureItem>(), put_abi(item)));
        if (item.Size().Width != width || item.Size().Height != height)
            throw std::runtime_error("Native capture size differs from the selected monitor");

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        for (auto& slot : slots) {
            check_hresult(device->CreateTexture2D(&desc, nullptr, slot.texture.put()));
            check_hresult(slot.texture.as<IDXGIResource1>()->CreateSharedHandle(nullptr,
                DXGI_SHARED_RESOURCE_READ, nullptr, &slot.local));
            if (!DuplicateHandle(GetCurrentProcess(), slot.local, parent, &slot.remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
                throw_last_error();
        }
        com_ptr<ID3D11Query> copied;
        D3D11_QUERY_DESC query{ D3D11_QUERY_EVENT, 0 };
        check_hresult(device->CreateQuery(&query, copied.put()));
        auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(direct3d,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        auto session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(true);
        try { session.IsBorderRequired(false); } catch (const hresult_error&) { }
        bool stopped = false, first = true;
        auto last = std::chrono::steady_clock::time_point::min();
        const auto interval = std::chrono::microseconds(1000000 / fps);
        auto token = pool.FrameArrived([&](auto const& sender, auto const&) {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped) return;
            try {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                if (frame.ContentSize().Width != width || frame.ContentSize().Height != height)
                    throw std::runtime_error("The captured monitor changed size");
                auto now = std::chrono::steady_clock::now();
                if (last != std::chrono::steady_clock::time_point::min() && now - last < interval * 9 / 10) return;
                size_t index = 0;
                while (index < slots.size() && slots[index].busy) ++index;
                if (index == slots.size()) return; // the encoder still owns every texture
                auto& slot = slots[index];
                auto access = frame.Surface().template as<IDirect3DDxgiInterfaceAccess>();
                com_ptr<ID3D11Texture2D> source;
                check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), source.put_void()));
                D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1 };
                context->CopySubresourceRegion(slot.texture.get(), 0, 0, 0, 0, source.get(), 0, &box);
                context->End(copied.get());
                context->Flush();
                // Complete the producer GPU write before Chromium opens the texture on its device.
                HRESULT ready;
                while ((ready = context->GetData(copied.get(), nullptr, 0, 0)) == S_FALSE) SwitchToThread();
                check_hresult(ready);
                slot.busy = true;
                last = now;
                if (first) {
                    first = false;
                    std::cout << "{\"ready\":true,\"width\":" << width << ",\"height\":" << height << "}" << std::endl;
                }
                std::cout << "{\"frame\":" << index << ",\"handle\":\""
                    << reinterpret_cast<uintptr_t>(slot.remote) << "\",\"width\":" << width
                    << ",\"height\":" << height << ",\"timestamp\":"
                    << frame.SystemRelativeTime().count() / 10 << "}" << std::endl;
            } catch (const hresult_error& error) {
                stopped = true;
                std::cout << "{\"error\":\"WGC frame failed: " << static_cast<unsigned>(error.code()) << "\"}" << std::endl;
            } catch (const std::exception&) {
                stopped = true;
                std::cout << "{\"error\":\"The captured monitor changed\"}" << std::endl;
            }
        });
        session.StartCapture();
        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream command(line);
            size_t index;
            if (command >> index && index < slots.size()) {
                std::lock_guard<std::mutex> lock(mutex);
                slots[index].busy = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
        }
        pool.FrameArrived(token);
        session.Close();
        pool.Close();
    } catch (const hresult_error& error) {
        std::cout << "{\"error\":\"WGC startup failed: " << static_cast<unsigned>(error.code()) << "\"}" << std::endl;
    } catch (const std::exception&) {
        std::cout << "{\"error\":\"Invalid or unavailable WGC capture\"}" << std::endl;
    }
    for (auto& slot : slots) {
        if (slot.remote && parent) {
            HANDLE temporary = nullptr;
            if (DuplicateHandle(parent, slot.remote, GetCurrentProcess(), &temporary, 0, FALSE,
                DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) CloseHandle(temporary);
        }
        if (slot.local) CloseHandle(slot.local);
    }
    if (parent) CloseHandle(parent);
}
