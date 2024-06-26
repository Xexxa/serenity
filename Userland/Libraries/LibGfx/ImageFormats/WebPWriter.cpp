/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Container: https://developers.google.com/speed/webp/docs/riff_container
// Lossless format: https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification

#include <AK/BitStream.h>
#include <AK/Debug.h>
#include <LibCompress/DeflateTables.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/WebPWriter.h>
#include <LibRIFF/RIFF.h>

namespace Gfx {

// https://developers.google.com/speed/webp/docs/riff_container#webp_file_header
static ErrorOr<void> write_webp_header(Stream& stream, unsigned data_size)
{
    TRY(stream.write_until_depleted("RIFF"sv));
    TRY(stream.write_value<LittleEndian<u32>>(4 + data_size)); // Including size of "WEBP" and the data size itself.
    TRY(stream.write_until_depleted("WEBP"sv));
    return {};
}

static ErrorOr<void> write_chunk_header(Stream& stream, StringView chunk_fourcc, unsigned vp8l_data_size)
{
    TRY(stream.write_until_depleted(chunk_fourcc));
    TRY(stream.write_value<LittleEndian<u32>>(vp8l_data_size));
    return {};
}

// https://developers.google.com/speed/webp/docs/riff_container#simple_file_format_lossless
// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#7_overall_structure_of_the_format
static ErrorOr<void> write_VP8L_header(Stream& stream, unsigned width, unsigned height, bool alpha_is_used_hint)
{
    // "The 14-bit precision for image width and height limits the maximum size of a WebP lossless image to 16384✕16384 pixels."
    if (width > 16384 || height > 16384)
        return Error::from_string_literal("WebP lossless images can't be larger than 16384x16384 pixels");

    if (width == 0 || height == 0)
        return Error::from_string_literal("WebP lossless images must be at least one pixel wide and tall");

    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // Signature byte.
    TRY(bit_stream.write_bits(0x2fu, 8u)); // Signature byte

    // 14 bits width-1, 14 bits height-1, 1 bit alpha hint, 3 bit version_number.
    TRY(bit_stream.write_bits(width - 1, 14u));
    TRY(bit_stream.write_bits(height - 1, 14u));

    // "The alpha_is_used bit is a hint only, and should not impact decoding.
    //  It should be set to 0 when all alpha values are 255 in the picture, and 1 otherwise."
    TRY(bit_stream.write_bits(alpha_is_used_hint, 1u));

    // "The version_number is a 3 bit code that must be set to 0."
    TRY(bit_stream.write_bits(0u, 3u));

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.flush_buffer_to_stream());

    return {};
}

static bool are_all_pixels_opaque(Bitmap const& bitmap)
{
    for (ARGB32 pixel : bitmap) {
        if ((pixel >> 24) != 0xff)
            return false;
    }
    return true;
}

static ErrorOr<void> write_VP8L_image_data(Stream& stream, Bitmap const& bitmap)
{
    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // optional-transform   =  (%b1 transform optional-transform) / %b0
    TRY(bit_stream.write_bits(0u, 1u)); // No transform for now.

    // https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#5_image_data
    // spatially-coded-image =  color-cache-info meta-prefix data

    // color-cache-info      =  %b0
    // color-cache-info      =/ (%b1 4BIT) ; 1 followed by color cache size
    TRY(bit_stream.write_bits(0u, 1u)); // No color cache for now.

    // meta-prefix           =  %b0 / (%b1 entropy-image)
    TRY(bit_stream.write_bits(0u, 1u)); // No meta prefix for now.

    // data                  =  prefix-codes lz77-coded-image
    // prefix-codes          =  prefix-code-group *prefix-codes
    // prefix-code-group     =
    //     5prefix-code ; See "Interpretation of Meta Prefix Codes" to
    //                  ; understand what each of these five prefix
    //                  ; codes are for.

    // We're writing a single prefix-code-group.
    // "These codes are (in bitstream order):

    //  Prefix code #1: Used for green channel, backward-reference length, and color cache.
    //  Prefix code #2, #3, and #4: Used for red, blue, and alpha channels, respectively.
    //  Prefix code #5: Used for backward-reference distance."

    // We use neither back-references not color cache entries yet.
    // We write prefix trees for 256 literals all of length 8, which means each byte is encoded as itself.
    // That doesn't give any compression, but is a valid bit stream.
    // We can make this smarter later on.

    size_t const color_cache_size = 0;
    constexpr Array alphabet_sizes = to_array<size_t>({ 256 + 24 + static_cast<size_t>(color_cache_size), 256, 256, 256, 40 }); // XXX Shared?

    // If you add support for color cache: At the moment, CanonicalCodes does not support writing more than 288 symbols.
    if (alphabet_sizes[0] > 288)
        return Error::from_string_literal("Invalid alphabet size");

    bool all_pixels_are_opaque = are_all_pixels_opaque(bitmap);

    int number_of_full_channels = all_pixels_are_opaque ? 3 : 4;
    for (int i = 0; i < number_of_full_channels; ++i) {
        TRY(bit_stream.write_bits(0u, 1u)); // Normal code length code.

        // Write code length codes.
        constexpr int kCodeLengthCodes = 19;
        Array<int, kCodeLengthCodes> kCodeLengthCodeOrder = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
        int num_code_lengths = max(4u, find_index(kCodeLengthCodeOrder.begin(), kCodeLengthCodeOrder.end(), 8) + 1);

        // "int num_code_lengths = 4 + ReadBits(4);"
        TRY(bit_stream.write_bits(num_code_lengths - 4u, 4u));

        for (int i = 0; i < num_code_lengths - 1; ++i)
            TRY(bit_stream.write_bits(0u, 3u));
        TRY(bit_stream.write_bits(1u, 3u));

        // Write code lengths.
        if (alphabet_sizes[i] == 256) {
            TRY(bit_stream.write_bits(0u, 1u)); // max_symbol is alphabet_size
        } else {
            TRY(bit_stream.write_bits(1u, 1u)); // max_symbol is explicitly coded
            // "int length_nbits = 2 + 2 * ReadBits(3);
            //  int max_symbol = 2 + ReadBits(length_nbits);"
            TRY(bit_stream.write_bits(3u, 3u));   // length_nbits = 2 + 2 * 3
            TRY(bit_stream.write_bits(254u, 8u)); // max_symbol = 2 + 254
        }

        // The code length codes only contain a single entry for '8'. WebP streams with a single element store 0 bits per element.
        // (This is different from deflate, which needs 1 bit per element.)
    }

    if (all_pixels_are_opaque) {
        // Use a simple 1-element code.
        TRY(bit_stream.write_bits(1u, 1u));   // Simple code length code.
        TRY(bit_stream.write_bits(0u, 1u));   // num_symbols - 1
        TRY(bit_stream.write_bits(1u, 1u));   // is_first_8bits
        TRY(bit_stream.write_bits(255u, 8u)); // symbol0
    }

    // For code #5, use a simple empty code, since we don't use this yet.
    TRY(bit_stream.write_bits(1u, 1u)); // Simple code length code.
    TRY(bit_stream.write_bits(0u, 1u)); // num_symbols - 1
    TRY(bit_stream.write_bits(0u, 1u)); // is_first_8bits
    TRY(bit_stream.write_bits(0u, 1u)); // symbol0

    // Image data.
    for (ARGB32 pixel : bitmap) {
        u8 a = pixel >> 24;
        u8 r = pixel >> 16;
        u8 g = pixel >> 8;
        u8 b = pixel;

        // We wrote a huffman table that gives every symbol 8 bits. That means we can write the image data
        // out uncompressed –- but we do need to reverse the bit order of the bytes.
        TRY(bit_stream.write_bits(Compress::reverse8_lookup_table[g], 8u));
        TRY(bit_stream.write_bits(Compress::reverse8_lookup_table[r], 8u));
        TRY(bit_stream.write_bits(Compress::reverse8_lookup_table[b], 8u));

        // If all pixels are opaque, we wrote a one-element huffman table for alpha, which needs 0 bits per element.
        if (!all_pixels_are_opaque)
            TRY(bit_stream.write_bits(Compress::reverse8_lookup_table[a], 8u));
    }

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.align_to_byte_boundary());
    TRY(bit_stream.flush_buffer_to_stream());

    return {};
}

struct VP8XHeader {
    bool has_icc { false };
    bool has_alpha { false };
    bool has_exif { false };
    bool has_xmp { false };
    bool has_animation { false };
    u32 width { 0 };
    u32 height { 0 };
};

static u8 vp8x_flags_from_header(VP8XHeader const& header)
{
    u8 flags = 0;

    // "Reserved (Rsv): 2 bits
    //  MUST be 0. Readers MUST ignore this field."

    // "ICC profile (I): 1 bit
    //  Set if the file contains an 'ICCP' Chunk."
    if (header.has_icc)
        flags |= 0x20;

    // "Alpha (L): 1 bit
    //  Set if any of the frames of the image contain transparency information ("alpha")."
    if (header.has_alpha)
        flags |= 0x10;

    // "Exif metadata (E): 1 bit
    //  Set if the file contains Exif metadata."
    if (header.has_exif)
        flags |= 0x8;

    // "XMP metadata (X): 1 bit
    //  Set if the file contains XMP metadata."
    if (header.has_xmp)
        flags |= 0x4;

    // "Animation (A): 1 bit
    //  Set if this is an animated image. Data in 'ANIM' and 'ANMF' Chunks should be used to control the animation."
    if (header.has_animation)
        flags |= 0x2;

    // "Reserved (R): 1 bit
    //  MUST be 0. Readers MUST ignore this field."

    return flags;
}

// https://developers.google.com/speed/webp/docs/riff_container#extended_file_format
static ErrorOr<void> write_VP8X_chunk(Stream& stream, VP8XHeader const& header)
{
    if (header.width > (1 << 24) || header.height > (1 << 24))
        return Error::from_string_literal("WebP dimensions too large for VP8X chunk");

    if (header.width == 0 || header.height == 0)
        return Error::from_string_literal("WebP lossless images must be at least one pixel wide and tall");

    // "The product of Canvas Width and Canvas Height MUST be at most 2^32 - 1."
    u64 product = static_cast<u64>(header.width) * static_cast<u64>(header.height);
    if (product >= (1ull << 32))
        return Error::from_string_literal("WebP dimensions too large for VP8X chunk");

    TRY(write_chunk_header(stream, "VP8X"sv, 10));

    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // Don't use bit_stream.write_bits() to write individual flags here:
    // The spec describes bit flags in MSB to LSB order, but write_bits() writes LSB to MSB.
    TRY(bit_stream.write_bits(vp8x_flags_from_header(header), 8u));

    // "Reserved: 24 bits
    //  MUST be 0. Readers MUST ignore this field."
    TRY(bit_stream.write_bits(0u, 24u));

    // "Canvas Width Minus One: 24 bits
    //  1-based width of the canvas in pixels. The actual canvas width is 1 + Canvas Width Minus One."
    TRY(bit_stream.write_bits(header.width - 1, 24u));

    // "Canvas Height Minus One: 24 bits
    //  1-based height of the canvas in pixels. The actual canvas height is 1 + Canvas Height Minus One."
    TRY(bit_stream.write_bits(header.height - 1, 24u));

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.flush_buffer_to_stream());

    return {};
}

// FIXME: Consider using LibRIFF for RIFF writing details. (It currently has no writing support.)
static ErrorOr<void> align_to_two(AllocatingMemoryStream& stream)
{
    // https://developers.google.com/speed/webp/docs/riff_container
    // "If Chunk Size is odd, a single padding byte -- which MUST be 0 to conform with RIFF -- is added."
    if (stream.used_buffer_size() % 2 != 0)
        TRY(stream.write_value<u8>(0));
    return {};
}

ErrorOr<void> WebPWriter::encode(Stream& stream, Bitmap const& bitmap, Options const& options)
{
    bool alpha_is_used_hint = !are_all_pixels_opaque(bitmap);
    dbgln_if(WEBP_DEBUG, "Writing WebP of size {} with alpha hint: {}", bitmap.size(), alpha_is_used_hint);

    // The chunk headers need to know their size, so we either need a SeekableStream or need to buffer the data. We're doing the latter.
    // FIXME: The whole writing-and-reading-into-buffer over-and-over is awkward and inefficient.
    AllocatingMemoryStream vp8l_header_stream;
    TRY(write_VP8L_header(vp8l_header_stream, bitmap.width(), bitmap.height(), alpha_is_used_hint));
    auto vp8l_header_bytes = TRY(vp8l_header_stream.read_until_eof());

    AllocatingMemoryStream vp8l_data_stream;
    TRY(write_VP8L_image_data(vp8l_data_stream, bitmap));
    auto vp8l_data_bytes = TRY(vp8l_data_stream.read_until_eof());

    AllocatingMemoryStream vp8l_chunk_stream;
    TRY(write_chunk_header(vp8l_chunk_stream, "VP8L"sv, vp8l_header_bytes.size() + vp8l_data_bytes.size()));
    TRY(vp8l_chunk_stream.write_until_depleted(vp8l_header_bytes));
    TRY(vp8l_chunk_stream.write_until_depleted(vp8l_data_bytes));
    TRY(align_to_two(vp8l_chunk_stream));
    auto vp8l_chunk_bytes = TRY(vp8l_chunk_stream.read_until_eof());

    ByteBuffer vp8x_chunk_bytes;
    ByteBuffer iccp_chunk_bytes;
    if (options.icc_data.has_value()) {
        dbgln_if(WEBP_DEBUG, "Writing VP8X and ICCP chunks.");
        AllocatingMemoryStream iccp_chunk_stream;
        TRY(write_chunk_header(iccp_chunk_stream, "ICCP"sv, options.icc_data.value().size()));
        TRY(iccp_chunk_stream.write_until_depleted(options.icc_data.value()));
        TRY(align_to_two(iccp_chunk_stream));
        iccp_chunk_bytes = TRY(iccp_chunk_stream.read_until_eof());

        AllocatingMemoryStream vp8x_chunk_stream;
        TRY(write_VP8X_chunk(vp8x_chunk_stream, { .has_icc = true, .has_alpha = alpha_is_used_hint, .width = (u32)bitmap.width(), .height = (u32)bitmap.height() }));
        VERIFY(vp8x_chunk_stream.used_buffer_size() % 2 == 0);
        vp8x_chunk_bytes = TRY(vp8x_chunk_stream.read_until_eof());
    }

    u32 total_size = vp8x_chunk_bytes.size() + iccp_chunk_bytes.size() + vp8l_chunk_bytes.size();
    TRY(write_webp_header(stream, total_size));
    TRY(stream.write_until_depleted(vp8x_chunk_bytes));
    TRY(stream.write_until_depleted(iccp_chunk_bytes));
    TRY(stream.write_until_depleted(vp8l_chunk_bytes));
    return {};
}

class WebPAnimationWriter : public AnimationWriter {
public:
    WebPAnimationWriter(SeekableStream& stream, IntSize dimensions, u8 original_vp8x_flags)
        : m_stream(stream)
        , m_dimensions(dimensions)
        , m_vp8x_flags(original_vp8x_flags)
    {
    }

    virtual ErrorOr<void> add_frame(Bitmap&, int, IntPoint) override;

    ErrorOr<void> update_size_in_header();
    ErrorOr<void> set_alpha_bit_in_header();

private:
    SeekableStream& m_stream;
    IntSize m_dimensions;
    u8 m_vp8x_flags { 0 };
};

static ErrorOr<void> align_to_two(SeekableStream& stream)
{
    // https://developers.google.com/speed/webp/docs/riff_container
    // "If Chunk Size is odd, a single padding byte -- which MUST be 0 to conform with RIFF -- is added."
    if (TRY(stream.tell()) % 2 != 0)
        TRY(stream.write_value<u8>(0));
    return {};
}

struct ANMFChunk {
    u32 frame_x { 0 };
    u32 frame_y { 0 };
    u32 frame_width { 0 };
    u32 frame_height { 0 };
    u32 frame_duration_in_milliseconds { 0 };

    enum class BlendingMethod {
        UseAlphaBlending = 0,
        DoNotBlend = 1,
    };
    BlendingMethod blending_method { BlendingMethod::UseAlphaBlending };

    enum class DisposalMethod {
        DoNotDispose = 0,
        DisposeToBackgroundColor = 1,
    };
    DisposalMethod disposal_method { DisposalMethod::DoNotDispose };

    ReadonlyBytes frame_data;
};

static ErrorOr<void> write_ANMF_chunk(Stream& stream, ANMFChunk const& chunk)
{
    TRY(write_chunk_header(stream, "ANMF"sv, 16 + chunk.frame_data.size()));

    LittleEndianOutputBitStream bit_stream { MaybeOwned<Stream>(stream) };

    // "Frame X: 24 bits (uint24)
    //  The X coordinate of the upper left corner of the frame is Frame X * 2."
    TRY(bit_stream.write_bits(chunk.frame_x / 2, 24u));

    // "Frame Y: 24 bits (uint24)
    //  The Y coordinate of the upper left corner of the frame is Frame Y * 2."
    TRY(bit_stream.write_bits(chunk.frame_y / 2, 24u));

    // "Frame Width: 24 bits (uint24)
    //  The 1-based width of the frame. The frame width is 1 + Frame Width Minus One."
    TRY(bit_stream.write_bits(chunk.frame_width - 1, 24u));

    // "Frame Height: 24 bits (uint24)
    //  The 1-based height of the frame. The frame height is 1 + Frame Height Minus One."
    TRY(bit_stream.write_bits(chunk.frame_height - 1, 24u));

    // "Frame Duration: 24 bits (uint24)"
    TRY(bit_stream.write_bits(chunk.frame_duration_in_milliseconds, 24u));

    // Don't use bit_stream.write_bits() to write individual flags here:
    // The spec describes bit flags in MSB to LSB order, but write_bits() writes LSB to MSB.
    u8 flags = 0;
    // "Reserved: 6 bits
    //  MUST be 0. Readers MUST ignore this field."

    // "Blending method (B): 1 bit"
    if (chunk.blending_method == ANMFChunk::BlendingMethod::DoNotBlend)
        flags |= 0x2;

    // "Disposal method (D): 1 bit"
    if (chunk.disposal_method == ANMFChunk::DisposalMethod::DisposeToBackgroundColor)
        flags |= 0x1;

    TRY(bit_stream.write_bits(flags, 8u));

    // FIXME: Make ~LittleEndianOutputBitStream do this, or make it VERIFY() that it has happened at least.
    TRY(bit_stream.flush_buffer_to_stream());

    TRY(stream.write_until_depleted(chunk.frame_data));

    if (chunk.frame_data.size() % 2 != 0)
        TRY(stream.write_value<u8>(0));

    return {};
}

ErrorOr<void> WebPAnimationWriter::add_frame(Bitmap& bitmap, int duration_ms, IntPoint at)
{
    if (at.x() < 0 || at.y() < 0 || at.x() + bitmap.width() > m_dimensions.width() || at.y() + bitmap.height() > m_dimensions.height())
        return Error::from_string_literal("Frame does not fit in animation dimensions");

    // FIXME: The whole writing-and-reading-into-buffer over-and-over is awkward and inefficient.
    AllocatingMemoryStream vp8l_header_stream;
    TRY(write_VP8L_header(vp8l_header_stream, bitmap.width(), bitmap.height(), true));
    auto vp8l_header_bytes = TRY(vp8l_header_stream.read_until_eof());

    AllocatingMemoryStream vp8l_data_stream;
    TRY(write_VP8L_image_data(vp8l_data_stream, bitmap));
    auto vp8l_data_bytes = TRY(vp8l_data_stream.read_until_eof());

    AllocatingMemoryStream vp8l_chunk_stream;
    TRY(write_chunk_header(vp8l_chunk_stream, "VP8L"sv, vp8l_header_bytes.size() + vp8l_data_bytes.size()));
    TRY(vp8l_chunk_stream.write_until_depleted(vp8l_header_bytes));
    TRY(vp8l_chunk_stream.write_until_depleted(vp8l_data_bytes));
    TRY(align_to_two(vp8l_chunk_stream));
    auto vp8l_chunk_bytes = TRY(vp8l_chunk_stream.read_until_eof());

    ANMFChunk chunk;
    chunk.frame_x = static_cast<u32>(at.x());
    chunk.frame_y = static_cast<u32>(at.y());
    chunk.frame_width = static_cast<u32>(bitmap.width());
    chunk.frame_height = static_cast<u32>(bitmap.height());
    chunk.frame_duration_in_milliseconds = static_cast<u32>(duration_ms);
    chunk.blending_method = ANMFChunk::BlendingMethod::DoNotBlend;
    chunk.disposal_method = ANMFChunk::DisposalMethod::DoNotDispose;
    chunk.frame_data = vp8l_chunk_bytes;

    TRY(write_ANMF_chunk(m_stream, chunk));

    TRY(update_size_in_header());

    if (!(m_vp8x_flags & 0x10) && !are_all_pixels_opaque(bitmap))
        TRY(set_alpha_bit_in_header());

    return {};
}

ErrorOr<void> WebPAnimationWriter::update_size_in_header()
{
    auto current_offset = TRY(m_stream.tell());
    TRY(m_stream.seek(4, SeekMode::SetPosition));
    VERIFY(current_offset > 8);
    TRY(m_stream.write_value<LittleEndian<u32>>(current_offset - 8));
    TRY(m_stream.seek(current_offset, SeekMode::SetPosition));
    return {};
}

ErrorOr<void> WebPAnimationWriter::set_alpha_bit_in_header()
{
    m_vp8x_flags |= 0x10;

    auto current_offset = TRY(m_stream.tell());
    TRY(m_stream.seek(20, SeekMode::SetPosition));
    TRY(m_stream.write_value<u8>(m_vp8x_flags));
    TRY(m_stream.seek(current_offset, SeekMode::SetPosition));
    return {};
}

struct ANIMChunk {
    u32 background_color { 0 };
    u16 loop_count { 0 };
};

static ErrorOr<void> write_ANIM_chunk(Stream& stream, ANIMChunk const& chunk)
{
    TRY(write_chunk_header(stream, "ANIM"sv, 6)); // Size of the ANIM chunk.
    TRY(stream.write_value<LittleEndian<u32>>(chunk.background_color));
    TRY(stream.write_value<LittleEndian<u16>>(chunk.loop_count));
    return {};
}

ErrorOr<NonnullOwnPtr<AnimationWriter>> WebPWriter::start_encoding_animation(SeekableStream& stream, IntSize dimensions, int loop_count, Color background_color, Options const& options)
{
    // We'll update the stream with the actual size later.
    TRY(write_webp_header(stream, 0));

    VP8XHeader vp8x_header;
    vp8x_header.has_icc = options.icc_data.has_value();
    vp8x_header.width = dimensions.width();
    vp8x_header.height = dimensions.height();
    vp8x_header.has_animation = true;
    TRY(write_VP8X_chunk(stream, vp8x_header));
    VERIFY(TRY(stream.tell()) % 2 == 0);

    ByteBuffer iccp_chunk_bytes;
    if (options.icc_data.has_value()) {
        TRY(write_chunk_header(stream, "ICCP"sv, options.icc_data.value().size()));
        TRY(stream.write_until_depleted(options.icc_data.value()));
        TRY(align_to_two(stream));
    }

    TRY(write_ANIM_chunk(stream, { .background_color = background_color.value(), .loop_count = static_cast<u16>(loop_count) }));

    auto writer = make<WebPAnimationWriter>(stream, dimensions, vp8x_flags_from_header(vp8x_header));
    TRY(writer->update_size_in_header());
    return writer;
}

}
