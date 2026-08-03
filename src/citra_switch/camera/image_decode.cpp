// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <png.h>

#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csetjmp>
#include <cstring>

#include "citra_switch/camera/image_decode.h"
#include "common/file_util.h"

namespace SwitchFrontend {

namespace {

// Interlaced PNGs are the one format here that cannot be consumed a row at a time, so they are
// the only ones that need a size limit.
constexpr std::uint64_t kMaxInterlacedPixels = 8'000'000;

// An image whose aspect ratio is far from the frame's still covers it with one dimension left
// very long.
constexpr std::uint64_t kMaxWorkPixels = 2'000'000;

// Averages source rows into a smaller destination as they arrive.
class BoxScaler {
public:
    BoxScaler(int src_width, int src_height, int dst_width, int dst_height)
        : src_width{src_width}, src_height{src_height}, dst_width{dst_width},
          dst_height{dst_height}, sums(static_cast<std::size_t>(dst_width) * 4),
          counts(static_cast<std::size_t>(dst_width)),
          pixels(static_cast<std::size_t>(dst_width) * dst_height * 4) {}

    void AddRow(const std::uint8_t* rgba) {
        if (src_y >= src_height) {
            return;
        }
        const int row =
            static_cast<int>(static_cast<std::int64_t>(src_y) * dst_height / src_height);
        if (row != dst_y) {
            FlushRow();
            dst_y = row;
        }
        for (int x = 0; x < src_width; ++x) {
            const auto column =
                static_cast<std::size_t>(static_cast<std::int64_t>(x) * dst_width / src_width);
            std::uint32_t* sum = &sums[column * 4];
            sum[0] += rgba[x * 4 + 0];
            sum[1] += rgba[x * 4 + 1];
            sum[2] += rgba[x * 4 + 2];
            sum[3] += rgba[x * 4 + 3];
            ++counts[column];
        }
        ++src_y;
    }

    std::vector<std::uint8_t> Take() {
        FlushRow();
        return std::move(pixels);
    }

private:
    void FlushRow() {
        std::uint8_t* out = &pixels[static_cast<std::size_t>(dst_y) * dst_width * 4];
        for (int x = 0; x < dst_width; ++x) {
            const std::uint32_t count = std::max(counts[static_cast<std::size_t>(x)], 1u);
            for (int channel = 0; channel < 4; ++channel) {
                out[x * 4 + channel] =
                    static_cast<std::uint8_t>(sums[static_cast<std::size_t>(x) * 4 + channel] /
                                              count);
            }
        }
        std::fill(sums.begin(), sums.end(), 0u);
        std::fill(counts.begin(), counts.end(), 0u);
    }

    int src_width;
    int src_height;
    int dst_width;
    int dst_height;
    int src_y = 0;
    int dst_y = 0;
    std::vector<std::uint32_t> sums;
    std::vector<std::uint32_t> counts;
    std::vector<std::uint8_t> pixels;
};

struct Size {
    int width;
    int height;
};

// The smallest size that still covers `cover`, never upscaling.
Size WorkingSize(int src_width, int src_height, int cover_width, int cover_height) {
    double factor = std::min(1.0, std::max(static_cast<double>(cover_width) / src_width,
                                           static_cast<double>(cover_height) / src_height));
    // An aspect ratio far from the frame's leaves one dimension long even once the other one
    // fits, so what the cached copy may cost is capped.
    const auto pixels = static_cast<std::uint64_t>(std::ceil(src_width * factor)) *
                        static_cast<std::uint64_t>(std::ceil(src_height * factor));
    if (pixels > kMaxWorkPixels) {
        factor *= std::sqrt(static_cast<double>(kMaxWorkPixels) / static_cast<double>(pixels));
    }
    if (factor >= 1.0) {
        return {src_width, src_height};
    }
    return {std::max(1, static_cast<int>(std::ceil(src_width * factor))),
            std::max(1, static_cast<int>(std::ceil(src_height * factor)))};
}

struct PngReader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset;
};

void PngRead(png_structp png, png_bytep out, png_size_t count) {
    auto* reader = static_cast<PngReader*>(png_get_io_ptr(png));
    if (count > reader->size - reader->offset) {
        png_error(png, "ran off the end of the file");
    }
    std::memcpy(out, reader->data + reader->offset, count);
    reader->offset += count;
}

void PngNoWarning(png_structp, png_const_charp) {}

std::string DecodePng(const std::vector<std::uint8_t>& file, int cover_width, int cover_height,
                      DecodedImage& out) {
    png_structp png =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, PngNoWarning);
    if (png == nullptr) {
        return "the PNG reader could not be created";
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return "the PNG reader could not be created";
    }

    // These outlive the setjmp frame, so they are owned by hand rather than by destructors
    // longjmp would skip.
    BoxScaler* volatile scaler = nullptr;
    std::uint8_t* volatile pixels = nullptr;
    png_bytep* volatile rows = nullptr;
    const auto release = [&] {
        delete scaler;
        delete[] pixels;
        delete[] rows;
    };

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        release();
        return "the PNG could not be decoded";
    }

    PngReader reader{file.data(), file.size(), 0};
    png_set_read_fn(png, &reader, PngRead);
    png_read_info(png, info);

    const auto width = static_cast<int>(png_get_image_width(png, info));
    const auto height = static_cast<int>(png_get_image_height(png, info));
    const png_byte colour_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);
    if (width <= 0 || height <= 0) {
        png_destroy_read_struct(&png, &info, nullptr);
        release();
        return "the PNG has no pixels";
    }

    // Whatever the file holds, the rows come out as 8-bit RGBA.
    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
    if (colour_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (colour_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (colour_type == PNG_COLOR_TYPE_GRAY || colour_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);

    if (png_get_rowbytes(png, info) != static_cast<png_size_t>(width) * 4) {
        png_destroy_read_struct(&png, &info, nullptr);
        release();
        return "the PNG has an unsupported pixel format";
    }

    const Size size = WorkingSize(width, height, cover_width, cover_height);
    scaler = new BoxScaler{width, height, size.width, size.height};

    if (passes == 1) {
        pixels = new std::uint8_t[static_cast<std::size_t>(width) * 4];
        for (int y = 0; y < height; ++y) {
            png_read_row(png, pixels, nullptr);
            scaler->AddRow(pixels);
        }
    } else {
        // An interlaced file only resolves once every pass has landed.
        if (static_cast<std::uint64_t>(width) * height > kMaxInterlacedPixels) {
            png_destroy_read_struct(&png, &info, nullptr);
            release();
            return "the interlaced PNG is over 8 megapixels";
        }
        pixels = new std::uint8_t[static_cast<std::size_t>(width) * height * 4];
        rows = new png_bytep[static_cast<std::size_t>(height)];
        for (int y = 0; y < height; ++y) {
            rows[y] = pixels + static_cast<std::size_t>(y) * width * 4;
        }
        png_read_image(png, rows);
        for (int y = 0; y < height; ++y) {
            scaler->AddRow(rows[y]);
        }
    }

    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);

    out.width = size.width;
    out.height = size.height;
    out.rgba = scaler->Take();
    release();
    return {};
}

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

[[noreturn]] void JpegErrorExit(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1);
}

void JpegNoOutput(j_common_ptr) {}

std::string DecodeJpeg(const std::vector<std::uint8_t>& file, int cover_width, int cover_height,
                       DecodedImage& out) {
    jpeg_decompress_struct cinfo{};
    JpegError error{};
    // The scaler outlives the setjmp frame, so it is owned by hand rather than by a destructor
    // longjmp would skip.
    BoxScaler* volatile scaler = nullptr;

    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = JpegErrorExit;
    error.pub.output_message = JpegNoOutput;

    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&cinfo);
        delete scaler;
        return "the JPEG could not be decoded";
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, file.data(), file.size());
    jpeg_read_header(&cinfo, TRUE);

    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    while (cinfo.scale_denom < 8 &&
           cinfo.image_width / (cinfo.scale_denom * 2) >= static_cast<unsigned>(cover_width) &&
           cinfo.image_height / (cinfo.scale_denom * 2) >= static_cast<unsigned>(cover_height)) {
        cinfo.scale_denom *= 2;
    }
    cinfo.out_color_space = JCS_EXT_RGBA;

    jpeg_start_decompress(&cinfo);
    const auto source_width = static_cast<int>(cinfo.output_width);
    const auto source_height = static_cast<int>(cinfo.output_height);
    if (source_width <= 0 || source_height <= 0 || cinfo.output_components != 4) {
        jpeg_destroy_decompress(&cinfo);
        return "the JPEG has an unsupported pixel format";
    }

    const Size size = WorkingSize(source_width, source_height, cover_width, cover_height);
    scaler = new BoxScaler{source_width, source_height, size.width, size.height};

    JSAMPARRAY row = (*cinfo.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&cinfo), JPOOL_IMAGE,
                                                static_cast<JDIMENSION>(source_width) * 4, 1);
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, row, 1);
        scaler->AddRow(reinterpret_cast<const std::uint8_t*>(row[0]));
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    out.width = size.width;
    out.height = size.height;
    out.rgba = scaler->Take();
    delete scaler;
    return {};
}

} // namespace

std::string DecodeImageFile(const std::string& path, int cover_width, int cover_height,
                            DecodedImage& out) {
    std::string contents;
    if (FileUtil::ReadFileToString(false, path, contents) == 0 || contents.size() < 8) {
        return "the file could not be read";
    }
    const std::vector<std::uint8_t> file(contents.begin(), contents.end());

    static constexpr std::array<std::uint8_t, 8> kPngMagic{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A,
                                                           0x0A};
    if (std::equal(kPngMagic.begin(), kPngMagic.end(), file.begin())) {
        return DecodePng(file, cover_width, cover_height, out);
    }
    if (file[0] == 0xFF && file[1] == 0xD8 && file[2] == 0xFF) {
        return DecodeJpeg(file, cover_width, cover_height, out);
    }
    return "only PNG and JPEG images are supported";
}

} // namespace SwitchFrontend
