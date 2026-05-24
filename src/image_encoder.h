/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "configuration.h"
#include "encoder.h"

// Image layout structure: stores information about how data is embedded in image
struct ImageLayout {
    int image_width;
    int image_height;
    int blocks_per_row;
    int blocks_per_col;
    int total_blocks;
    int bits_per_image;
    int bytes_per_image;
};

// Compute image layout based on width and height
ImageLayout compute_image_layout();

ImageLayout compute_image_layout(int width, int height);

// Get maximum packet bytes that can fit in one image
std::size_t max_packet_bytes_per_image();

class ImageEncoder {
public:
    // Constructor: initializes encoder with output image path
    explicit ImageEncoder(const std::string &output_path);

    ~ImageEncoder();

    ImageEncoder(const ImageEncoder &) = delete;

    ImageEncoder &operator=(const ImageEncoder &) = delete;

    ImageEncoder(ImageEncoder &&) = delete;

    ImageEncoder &operator=(ImageEncoder &&) = delete;

    // Add single packet to encoding buffer
    void add_packet(const Packet &packet);

    // Add multiple packets at once
    void encode_packets(const std::vector<Packet> &packets);

    // Finalize encoding: write remaining buffered data and close file
    void finalize();

    // Get number of images written
    [[nodiscard]] int64_t images_written() const { return image_index; }

    // Static method: get number of packets per image
    [[nodiscard]] static int packets_per_image();

private:
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    AVStream *stream = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *av_packet = nullptr;

    std::vector<std::byte> image_data_buffer;
    ImageLayout layout_{};
    int64_t image_index = 0;
    bool finalized = false;

    // Initialize the encoder with output path
    void init_encoder(const std::string &output_path);

    // Embed data bytes into image frame using DCT-based patterns
    void embed_data_in_image(const std::vector<std::byte> &data) const;

    // Encode frame to output file
    void encode_image();

    // Flush remaining frames in encoder
    void flush_encoder() const;

    // Flush buffered data as image frame
    void flush_image_buffer();
};
