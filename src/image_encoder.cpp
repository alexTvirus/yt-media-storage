// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) 2026 Brandon Li <https://brandonli.me/>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "image_encoder.h"
#include "configuration.h"
#include "dct_common.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

// Image compression constants: use PNG codec for lossless compression
// PNG preserves pixel values exactly, allowing reliable data recovery
constexpr const char* IMAGE_CODEC = "png";
constexpr const char* IMAGE_CONTAINER = "png";
constexpr int IMAGE_WIDTH = FRAME_WIDTH;
constexpr int IMAGE_HEIGHT = FRAME_HEIGHT;

ImageLayout compute_image_layout() {
    return compute_image_layout(IMAGE_WIDTH, IMAGE_HEIGHT);
}

ImageLayout compute_image_layout(const int width, const int height) {
    ImageLayout layout{};
    layout.image_width = width;
    layout.image_height = height;
    layout.blocks_per_row = width / 8;
    layout.blocks_per_col = height / 8;
    layout.total_blocks = layout.blocks_per_row * layout.blocks_per_col;
    layout.bits_per_image = layout.total_blocks * BITS_PER_BLOCK;
    layout.bytes_per_image = layout.bits_per_image / 8;
    return layout;
}

std::size_t max_packet_bytes_per_image() {
    return static_cast<std::size_t>(compute_image_layout().bytes_per_image);
}

ImageEncoder::ImageEncoder(const std::string &output_path) {
    init_encoder(output_path);
}

ImageEncoder::~ImageEncoder() {
    if (!finalized) {
        try { 
            finalize(); 
        } catch (...) {
            // Silently catch finalization errors in destructor
        }
    }
    if (av_packet) av_packet_free(&av_packet);
    if (frame) av_frame_free(&frame);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (format_ctx) {
        if (format_ctx->pb) avio_closep(&format_ctx->pb);
        avformat_free_context(format_ctx);
    }
}

void ImageEncoder::init_encoder(const std::string &output_path) {
    // Allocate output context for PNG format
    int ret = avformat_alloc_output_context2(&format_ctx, nullptr, "image2", output_path.c_str());
    if (ret < 0 || !format_ctx) {
        throw std::runtime_error("Failed to create output context for image encoding");
    }

    // Find PNG encoder
    const AVCodec *codec = avcodec_find_encoder_by_name(IMAGE_CODEC);
    if (!codec) {
        throw std::runtime_error(std::string("Failed to find encoder: ") + IMAGE_CODEC);
    }

    // Create video stream
    stream = avformat_new_stream(format_ctx, nullptr);
    if (!stream) {
        throw std::runtime_error("Failed to create image stream");
    }

    // Allocate codec context
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        throw std::runtime_error("Failed to allocate codec context for image encoder");
    }

    // Configure codec context for image encoding
    codec_ctx->width = IMAGE_WIDTH;
    codec_ctx->height = IMAGE_HEIGHT;
    codec_ctx->time_base = {1, 1}; // 1 fps for image sequences
    codec_ctx->framerate = {1, 1};
    codec_ctx->pix_fmt = AV_PIX_FMT_GRAY8; // 8-bit grayscale for DCT embedding
    codec_ctx->thread_count = 0;
    codec_ctx->thread_type = FF_THREAD_SLICE;

    // Set PNG compression level
    av_opt_set_int(codec_ctx->priv_data, "compression_level", 9, 0);

    if (format_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open codec
    ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret < 0) {
        char error_buffer[256];
        av_strerror(ret, error_buffer, sizeof(error_buffer));
        throw std::runtime_error(std::string("Failed to open image codec: ") + error_buffer);
    }

    // Copy codec parameters to stream
    ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
    if (ret < 0) {
        throw std::runtime_error("Failed to copy codec parameters to stream");
    }

    stream->time_base = codec_ctx->time_base;

    // Allocate frame buffer
    frame = av_frame_alloc();
    if (!frame) {
        throw std::runtime_error("Failed to allocate frame for image encoding");
    }

    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        throw std::runtime_error("Failed to allocate frame buffer for image");
    }

    // Allocate packet
    av_packet = av_packet_alloc();
    if (!av_packet) {
        throw std::runtime_error("Failed to allocate packet for image encoding");
    }

    // Compute image layout and reserve buffer space
    layout_ = compute_image_layout();
    image_data_buffer.reserve(layout_.bytes_per_image);

    // Open output file for writing
    ret = avio_open(&format_ctx->pb, output_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        throw std::runtime_error("Failed to open output file for image encoding");
    }

    // Write format header
    ret = avformat_write_header(format_ctx, nullptr);
    if (ret < 0) {
        throw std::runtime_error("Failed to write image format header");
    }
}

int ImageEncoder::packets_per_image() {
    const auto layout = compute_image_layout();
    constexpr std::size_t packet_size = HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES;
    return static_cast<int>(layout.bytes_per_image / packet_size);
}

void ImageEncoder::embed_data_in_image(const std::vector<std::byte> &data) const {
#if defined(__APPLE__) && defined(_OPENMP)
    const auto &blocks = get_precomputed_blocks();
    const auto &patterns = blocks.patterns;
#else
    const auto &patterns = get_precomputed_blocks().patterns;
#endif

    av_frame_make_writable(frame);

    // Calculate total bits and active blocks needed
    const std::size_t total_bits = data.size() * 8;
    const int total_blocks = layout_.blocks_per_row * layout_.blocks_per_col;
    const int active_blocks = static_cast<int>(
        std::min(static_cast<std::size_t>(total_blocks),
                 (total_bits + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK));
    const auto *src = reinterpret_cast<const uint8_t *>(data.data());
    const int blocks_per_row = layout_.blocks_per_row;

    // Initialize frame with neutral gray value (128)
    uint8_t *dst_base = frame->data[0];
    const int dst_stride = frame->linesize[0];
    for (int y = 0; y < IMAGE_HEIGHT; ++y)
        std::memset(dst_base + y * dst_stride, 128, IMAGE_WIDTH);

    // Embed data using DCT-based patterns in parallel
#pragma omp parallel for schedule(static)
    for (int block_idx = 0; block_idx < active_blocks; ++block_idx) {
        const int block_row = block_idx / blocks_per_row;
        const int block_col = block_idx % blocks_per_row;
        const int base_x = block_col * 8;
        const int base_y = block_row * 8;

        const std::size_t bit_start = static_cast<std::size_t>(block_idx) * BITS_PER_BLOCK;
        const std::size_t bit_end = std::min(bit_start + BITS_PER_BLOCK, total_bits);

        // Extract bit pattern from data
        int pattern = 0;
        for (std::size_t bit_index = bit_start; bit_index < bit_end; ++bit_index) {
            const std::size_t byte_idx = bit_index / 8;
            const int bit_pos = 7 - static_cast<int>(bit_index % 8);
            const int bit = (src[byte_idx] >> bit_pos) & 1;
            pattern = (pattern << 1) | bit;
        }

        // Pad pattern if necessary
        const int bits_extracted = static_cast<int>(bit_end - bit_start);
        pattern <<= (BITS_PER_BLOCK - bits_extracted);

        // Copy precomputed pattern to frame
        const auto &block = patterns[pattern];
        for (int y = 0; y < 8; ++y) {
            std::memcpy(dst_base + (base_y + y) * dst_stride + base_x,
                        block[y], 8);
        }
    }
}

void ImageEncoder::encode_image() {
    int ret = av_frame_make_writable(frame);
    if (ret < 0) {
        throw std::runtime_error("Image frame not writable");
    }

    frame->pts = image_index++;

    // Send frame to encoder
    ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0) {
        throw std::runtime_error("Error sending frame to image encoder");
    }

    // Receive and write encoded packets
    while (true) {
        ret = avcodec_receive_packet(codec_ctx, av_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error receiving packet from image encoder");
        }

        av_packet->stream_index = stream->index;

        ret = av_interleaved_write_frame(format_ctx, av_packet);
        if (ret < 0) {
            throw std::runtime_error("Error writing image frame to file");
        }

        av_packet_unref(av_packet);
    }
}

void ImageEncoder::add_packet(const Packet &packet) {
    if (finalized) {
        throw std::runtime_error("Image encoder already finalized");
    }

    // Check if buffer would overflow after adding this packet
    if (const auto max_bytes = static_cast<std::size_t>(layout_.bytes_per_image);
        image_data_buffer.size() + packet.bytes.size() > max_bytes) {
        flush_image_buffer();
    }

    // Append packet to buffer
    image_data_buffer.insert(image_data_buffer.end(),
                             packet.bytes.begin(),
                             packet.bytes.end());
}

void ImageEncoder::encode_packets(const std::vector<Packet> &packets) {
    for (const auto &pkt : packets) {
        add_packet(pkt);
    }
}

void ImageEncoder::flush_image_buffer() {
    if (image_data_buffer.empty()) return;

    // Embed buffered data into image and encode
    embed_data_in_image(image_data_buffer);
    encode_image();
    image_data_buffer.clear();
}

void ImageEncoder::flush_encoder() const {
    // Send EOF to encoder
    int ret = avcodec_send_frame(codec_ctx, nullptr);

    // Receive and write remaining packets
    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx, av_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            throw std::runtime_error("Error flushing image encoder");
        }

        av_packet->stream_index = stream->index;

        av_interleaved_write_frame(format_ctx, av_packet);
        av_packet_unref(av_packet);
    }
}

void ImageEncoder::finalize() {
    if (finalized) return;
    finalized = true;
    
    // Flush any remaining buffered data
    flush_image_buffer();
    
    // Flush encoder queue
    flush_encoder();
    
    // Write file trailer
    av_write_trailer(format_ctx);
}
