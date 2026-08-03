// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "citra_switch/camera/image_decode.h"
#include "citra_switch/camera/still_image_camera.h"
#include "common/logging/log.h"
#include "core/frontend/camera/blank_camera.h"
#include "core/hle/service/cam/cam.h"

namespace Camera::StillImage {

namespace {

using SwitchFrontend::DecodedImage;

// The 3DS never asks for more than VGA, so the cached copy is kept just big enough to cover one.
constexpr int kCoverWidth = 640;
constexpr int kCoverHeight = 480;

// Headroom over VGA for anything that asks for an odd size.
constexpr std::size_t kMaxFramePixels = 1024 * 1024;

std::mutex s_mutex;
std::string s_cached_path;
std::shared_ptr<const DecodedImage> s_cached_image;

std::shared_ptr<const DecodedImage> Load(const std::string& path, std::string& error) {
    std::scoped_lock lock{s_mutex};
    if (!path.empty() && path == s_cached_path && s_cached_image) {
        return s_cached_image;
    }
    auto image = std::make_shared<DecodedImage>();
    error = SwitchFrontend::DecodeImageFile(path, kCoverWidth, kCoverHeight, *image);
    if (!error.empty()) {
        return nullptr;
    }
    s_cached_path = path;
    s_cached_image = image;
    return image;
}

struct Rgb {
    int r;
    int g;
    int b;
};

Rgb Sample(const DecodedImage& image, double x, double y) {
    const double cx = std::clamp(x, 0.0, static_cast<double>(image.width - 1));
    const double cy = std::clamp(y, 0.0, static_cast<double>(image.height - 1));
    const auto x0 = static_cast<int>(cx);
    const auto y0 = static_cast<int>(cy);
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const double fx = cx - x0;
    const double fy = cy - y0;

    const auto texel = [&](int px, int py, int channel) {
        return static_cast<double>(
            image.rgba[(static_cast<std::size_t>(py) * image.width + px) * 4 + channel]);
    };

    Rgb out{};
    int* const channels[]{&out.r, &out.g, &out.b};
    for (int channel = 0; channel < 3; ++channel) {
        const double top =
            texel(x0, y0, channel) * (1.0 - fx) + texel(x1, y0, channel) * fx;
        const double bottom =
            texel(x0, y1, channel) * (1.0 - fx) + texel(x1, y1, channel) * fx;
        *channels[channel] = static_cast<int>(std::lround(top * (1.0 - fy) + bottom * fy));
    }
    return out;
}

// Full range ITU-R BT.601, the transform Y2R undoes on the other side.
int Luma(const Rgb& c) {
    return std::clamp((77 * c.r + 150 * c.g + 29 * c.b) >> 8, 0, 255);
}

int ChromaU(const Rgb& c) {
    return std::clamp(((-43 * c.r - 85 * c.g + 128 * c.b) >> 8) + 128, 0, 255);
}

int ChromaV(const Rgb& c) {
    return std::clamp(((128 * c.r - 107 * c.g - 21 * c.b) >> 8) + 128, 0, 255);
}

class Interface final : public CameraInterface {
public:
    Interface(std::shared_ptr<const DecodedImage> image_, const Service::CAM::Flip& flip)
        : image{std::move(image_)} {
        base_mirror = mirror =
            flip == Service::CAM::Flip::Horizontal || flip == Service::CAM::Flip::Reverse;
        base_invert = invert =
            flip == Service::CAM::Flip::Vertical || flip == Service::CAM::Flip::Reverse;
    }

    void StartCapture() override {}
    void StopCapture() override {}
    void SetEffect(Service::CAM::Effect) override {}
    void SetFrameRate(Service::CAM::FrameRate) override {}

    void SetResolution(const Service::CAM::Resolution& resolution_) override {
        resolution = resolution_;
        dirty = true;
    }

    void SetFlip(Service::CAM::Flip flip) override {
        mirror = base_mirror ^ (flip == Service::CAM::Flip::Horizontal ||
                                flip == Service::CAM::Flip::Reverse);
        invert = base_invert ^
                 (flip == Service::CAM::Flip::Vertical || flip == Service::CAM::Flip::Reverse);
        dirty = true;
    }

    void SetFormat(Service::CAM::OutputFormat format_) override {
        format = format_;
        dirty = true;
    }

    std::vector<u16> ReceiveFrame() override {
        if (dirty) {
            Rebuild();
            dirty = false;
        }
        return frame;
    }

    bool IsPreviewAvailable() override {
        return true;
    }

private:
    void Rebuild();

    std::shared_ptr<const DecodedImage> image;
    Service::CAM::Resolution resolution{};
    Service::CAM::OutputFormat format{Service::CAM::OutputFormat::YUV422};
    bool base_mirror{};
    bool base_invert{};
    bool mirror{};
    bool invert{};
    std::vector<u16> frame;
    bool dirty = true;
};

void Interface::Rebuild() {
    const int width = resolution.width;
    const int height = resolution.height;
    const bool rgb565 = format == Service::CAM::OutputFormat::RGB565;

    frame.clear();
    // SetDetailSize takes the size straight from the guest.
    if (static_cast<std::size_t>(width) * height > kMaxFramePixels) {
        LOG_ERROR(Service_CAM, "Refusing a {}x{} camera frame", width, height);
        return;
    }

    frame.assign(static_cast<std::size_t>(width) * height, rgb565 ? 0 : 0x8000);
    if (!image || image->width <= 0 || image->height <= 0 || width <= 0 || height <= 0) {
        return;
    }

    // The picture covers the frame and the overflow is cropped off centre, which is what the
    // desktop frontend does with a still image.
    const double scale = std::max(static_cast<double>(width) / image->width,
                                  static_cast<double>(height) / image->height);
    const double offset_x = (image->width * scale - width) / 2.0;
    const double offset_y = (image->height * scale - height) / 2.0;

    std::vector<Rgb> row(static_cast<std::size_t>(width));
    for (int y = 0; y < height; ++y) {
        const double source_y = ((invert ? height - 1 - y : y) + offset_y + 0.5) / scale - 0.5;
        for (int x = 0; x < width; ++x) {
            const double source_x = ((mirror ? width - 1 - x : x) + offset_x + 0.5) / scale - 0.5;
            row[static_cast<std::size_t>(x)] = Sample(*image, source_x, source_y);
        }

        u16* const line = &frame[static_cast<std::size_t>(y) * width];
        if (rgb565) {
            for (int x = 0; x < width; ++x) {
                const Rgb& c = row[static_cast<std::size_t>(x)];
                line[x] = static_cast<u16>(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
            }
        } else {
            for (int x = 0; x < width; x += 2) {
                const Rgb& first = row[static_cast<std::size_t>(x)];
                const Rgb& second = row[static_cast<std::size_t>(std::min(x + 1, width - 1))];
                const int u = (ChromaU(first) + ChromaU(second)) / 2;
                const int v = (ChromaV(first) + ChromaV(second)) / 2;
                line[x] = static_cast<u16>(Luma(first) | (u << 8));
                if (x + 1 < width) {
                    line[x + 1] = static_cast<u16>(Luma(second) | (v << 8));
                }
            }
        }
    }
}

} // namespace

std::string Preload(const std::string& path) {
    std::string error;
    return Load(path, error) ? std::string{} : error;
}

std::unique_ptr<CameraInterface> Factory::Create(const std::string& config,
                                                 const Service::CAM::Flip& flip) {
    std::string error;
    auto image = Load(config, error);
    if (!image) {
        LOG_ERROR(Service_CAM, "Cannot use camera image {}: {}", config, error);
        return std::make_unique<BlankCamera>();
    }
    return std::make_unique<Interface>(std::move(image), flip);
}

} // namespace Camera::StillImage
