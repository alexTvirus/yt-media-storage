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
#include <libswscale/swscale.h>
}

#include "configuration.h"
#include "image_encoder.h"

class ImageDecoder {
public:
    // Constructor: initializes decoder with input image path
    explicit ImageDecoder(const std::string &input_path);

    ~ImageDecoder();

    ImageDecoder(const ImageDecoder &) = delete;

    ImageDecoder &operator=(const ImageDecoder &) = delete;

    ImageDecoder(ImageDecoder &&) = delete;

    ImageDecoder &operator=(ImageDecoder &&) = delete;

    // Prepare current frame for data extraction (convert to gray if needed)
    void prepare_frame_for_extraction();

    // Extract data bytes from current frame
    std::vector<std::byte> extract_data_from_frame() const;

    // Extract raw packets from current frame
    std::vector<std::vector<std::byte>> extract_packets_from_frame() const;

    // Extract packets and accumulate in buffer, return extracted packets
    std::vector<std::vector<std::byte>> accumulate_frame_and_extract_packets();

    // Decode next image and extract packets from it
    std::vector<std::vector<std::byte>> decode_next_image();

    // Decode all remaining images and extract all packets
    std::vector<std::vector<std::byte>> decode_all_images();

    // Get total number of images in file
    [[nodiscard]] int64_t total_images() const;

    // Check if decoding is complete
    [[nodiscard]] bool is_eof() const { return eof_; }

    // Get number of images processed
    [[nodiscard]] int64_t images_processed() const { return image_index; }

private:
    AVFormatContext *format_ctx_ = nullptr;
    AVCodecContext *codec_ctx_ = nullptr;
    AVStream *stream_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVFrame *gray_frame_ = nullptr;
    AVPacket *av_packet_ = nullptr;
    SwsContext *sws_ctx_ = nullptr;

    ImageLayout layout_{};
    std::vector<std::byte> extract_buffer_;
    int video_stream_index_ = -1;
    int64_t image_index = 0;
    bool eof_ = false;
    bool is_gray8_ = false;

    // Initialize decoder with input file
    void init_decoder(const std::string &input_path);

    // Extract data into destination buffer (internal)
    void extract_data_into(std::vector<std::byte> &dest) const;

    // Extract packets from accumulated buffer
    static void extract_packets_from_buffer(std::vector<std::byte> &accumulated,
                                           std::vector<std::vector<std::byte>> &out_packets);

    // Flush decoder and collect remaining packets
    std::vector<std::vector<std::byte>> flush_decoder_and_collect_packets();
};
