#include "OpticalD3D11.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

static void Check(HRESULT result)
{
    if (FAILED(result)) throw std::runtime_error("D3D11 benchmark operation failed");
}
static void Snapshot(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *image,
                     const std::filesystem::path &path)
{
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R32_UINT)
        throw std::runtime_error("Unsupported diagnostic image format");
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), image);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::ofstream output(path, std::ios::binary);
    for (unsigned y = 0; y < desc.Height; ++y)
        output.write(reinterpret_cast<char *>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    if (!output) throw std::runtime_error("Diagnostic image write failed");
}

int main(int argc, char **argv)
{
    try
    {
        if (argc < 4) throw std::runtime_error("Usage: OpticalFlowBench width height capture-directory [...]");
        unsigned width = std::stoul(argv[1]), height = std::stoul(argv[2]);
        bool dump = std::string(argv[argc - 1]) == "--gpu-dump";
        bool gpu = dump || std::string(argv[argc - 1]) == "--gpu";
        int capturesEnd = argc - (gpu ? 1 : 0);
        if (!width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) throw std::runtime_error("Invalid image dimensions");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &device, nullptr, &context));
        for (int capture = 3; capture < capturesEnd; ++capture)
        {
            ComPtr<ID3D11Texture2D> images[3];
            for (unsigned i = 0; i < 3; ++i)
            {
                auto path = std::filesystem::path(argv[capture]) / ("world-" + std::to_string(i) + ".rgba");
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                std::size_t bytes = std::size_t(width) * height * 4;
                if (!file || file.tellg() != static_cast<std::streamoff>(bytes)) throw std::runtime_error("RGBA capture size mismatch");
                file.seekg(0);
                std::vector<unsigned char> pixels(bytes);
                if (!file.read(reinterpret_cast<char *>(pixels.data()), bytes)) throw std::runtime_error("RGBA capture read failed");
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
                Check(device->CreateTexture2D(&desc, &initial, &images[i]));
            }
            for (auto preset : {NV_OF_PERF_LEVEL_SLOW, NV_OF_PERF_LEVEL_MEDIUM, NV_OF_PERF_LEVEL_FAST})
            {
                OpticalD3D11 flow(device.Get(), width, height, L"", true, preset);
                std::vector<double> times;
                unsigned reasons[8]{};
                std::uint64_t safePixels = 0, synthesizedPixels = 0;
                double timestamp = 1;
                for (unsigned repeat = 0; repeat < 40; ++repeat)
                {
                    timestamp += .3;
                    flow.Submit(images[0].Get(), timestamp, false);
                    if (!flow.Wait()) throw std::runtime_error("History upload timed out");
                    for (unsigned i = 1; i < 3; ++i)
                    {
                        timestamp += 1.0 / 30;
                        auto start = std::chrono::steady_clock::now();
                        auto ticket = flow.Submit(images[i].Get(), timestamp, true, gpu);
                        if (!gpu) flow.Generate(ticket, 5000);
                        if (!flow.Wait()) throw std::runtime_error("Flow work timed out");
                        if (gpu)
                        {
                            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                            while (!flow.ResolveGpu(ticket))
                            {
                                if (std::chrono::steady_clock::now() >= deadline)
                                    throw std::runtime_error("GPU readback did not become available");
                                SwitchToThread();
                            }
                        }
                        if (repeat >= 10)
                        {
                            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                            ++reasons[ticket->rejection];
                            safePixels += ticket->counts[4];
                            synthesizedPixels += ticket->counts[5];
                        }
                        if (dump && repeat == 39 && i == 2 && preset == NV_OF_PERF_LEVEL_FAST)
                        {
                            std::filesystem::create_directories("mask-replay");
                            auto stem = std::filesystem::path("mask-replay") / ("capture-" + std::to_string(capture - 3));
                            Snapshot(device.Get(), context.Get(), flow.Output(), stem.string() + "-generated.rgba");
                            Snapshot(device.Get(), context.Get(), flow.DiagnosticMask(), stem.string() + "-mask.u32");
                        }
                    }
                }
                std::sort(times.begin(), times.end());
                std::cout << "{\"capture\":" << capture - 3 << ",\"preset\":" << int(preset)
                          << ",\"samples\":" << times.size() << ",\"median_ms\":" << times[times.size()/2]
                          << ",\"p95_ms\":" << times[times.size()*95/100] << ",\"reasons\":[";
                for (unsigned i = 0; i < 8; ++i) std::cout << (i ? "," : "") << reasons[i];
                std::cout << "],\"safe_pixels\":" << safePixels << ",\"synthesized_pixels\":" << synthesizedPixels
                          << ",\"gpu_path\":" << gpu << "}" << std::endl;
            }
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
