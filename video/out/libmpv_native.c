/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>

#include "demux/stheader.h"
#include "libmpv.h"
#include "misc/bstr.h"

static bool is_hvcc(const AVCodecParameters *params)
{
    return params->extradata && params->extradata_size >= 23 && params->extradata[0] == 1;
}

static bool hdr10_nal(bstr nal)
{
    if (nal.len < 2)
        return false;
    unsigned type = (nal.start[0] >> 1) & 63;
    if (type == 62 || type == 63) // Dolby Vision RPU or enhancement layer.
        return false;
    if (type == 39 || type == 40) { // Prefix or suffix SEI.
        // HDR10+ registered ITU-T T.35 identifier, without the application version.
        // None of these bytes need emulation prevention within the identifier.
        static const uint8_t hdr10plus[] = {0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04};
        bstr payload = bstr_cut(nal, 2);
        if (bstr_find(payload, (bstr){(uint8_t *)hdr10plus, sizeof(hdr10plus)}) >= 0)
            return false;
    }
    return true;
}

static const uint8_t *start_code(const uint8_t *p, const uint8_t *end)
{
    if (end - p < 3)
        return NULL;
    for (p += 2; (p = memchr(p, 1, end - p)); p++) {
        if (!p[-1] && !p[-2])
            return p - 2;
    }
    return NULL;
}

static bstr next_annexb_nal(const uint8_t **start, const uint8_t *end)
{
    const uint8_t *nal = *start + 3;
    *start = start_code(nal, end);
    return (bstr){(uint8_t *)nal, (*start ? *start : end) - nal};
}

static bstr trim_trailing_zeros(bstr nal)
{
    while (nal.len && !nal.start[nal.len - 1])
        nal.len--;
    return nal;
}

// With an SPS to match, also require that the decoder configuration already
// contains it. Annex B can leave trailing_zero_8bits after the NAL payload.
static bool hdr10_extradata(const AVCodecParameters *params, bstr sps)
{
    const uint8_t *hvcc = params->extradata;
    if (!hvcc || params->extradata_size <= 0)
        return !sps.len;
    const uint8_t *end = hvcc + params->extradata_size;
    bool found = !sps.len;
    if (!is_hvcc(params)) {
        const uint8_t *start = start_code(hvcc, end);
        if (!start)
            return false;
        do {
            bstr nal = next_annexb_nal(&start, end);
            if (!hdr10_nal(nal))
                return false;
            found |= bstr_equals(trim_trailing_zeros(nal), sps);
        } while (start);
        return found;
    }

    const uint8_t *p = hvcc + 23;
    for (unsigned i = 0; i < hvcc[22]; i++) {
        if (end - p < 3)
            return false;
        unsigned count = AV_RB16(p + 1);
        p += 3;
        for (unsigned j = 0; j < count; j++) {
            if (end - p < 2)
                return false;
            size_t size = AV_RB16(p);
            p += 2;
            if (size > end - p)
                return false;
            bstr nal = {(uint8_t *)p, size};
            if (!hdr10_nal(nal))
                return false;
            found |= bstr_equals(trim_trailing_zeros(nal), sps);
            p += size;
        }
    }
    return found;
}

bool mp_native_hdr10_codec(const struct mp_codec_params *codec)
{
    const AVCodecParameters *params = codec ? codec->lav_codecpar : NULL;
    if (!params || codec->dovi || codec->rotate || params->codec_id != AV_CODEC_ID_HEVC ||
        (params->profile != AV_PROFILE_UNKNOWN && params->profile != AV_PROFILE_HEVC_MAIN_10) ||
        (params->bits_per_raw_sample && params->bits_per_raw_sample != 10))
        return false;

    if ((params->color_primaries != AVCOL_PRI_UNSPECIFIED &&
         params->color_primaries != AVCOL_PRI_BT2020) ||
        (codec->color.primaries && codec->color.primaries != PL_COLOR_PRIM_BT_2020) ||
        (params->color_trc != AVCOL_TRC_UNSPECIFIED &&
         params->color_trc != AVCOL_TRC_SMPTE2084) ||
        (codec->color.transfer && codec->color.transfer != PL_COLOR_TRC_PQ) ||
        (params->color_space != AVCOL_SPC_UNSPECIFIED &&
         params->color_space != AVCOL_SPC_BT2020_NCL) ||
        (codec->repr.sys && codec->repr.sys != PL_COLOR_SYSTEM_BT_2020_NC))
        return false;

    if (!(params->color_primaries == AVCOL_PRI_BT2020 || codec->color.primaries) ||
        !(params->color_trc == AVCOL_TRC_SMPTE2084 || codec->color.transfer) ||
        !(params->color_space == AVCOL_SPC_BT2020_NCL || codec->repr.sys))
        return false;

    if (av_packet_side_data_get(params->coded_side_data, params->nb_coded_side_data,
                                AV_PKT_DATA_DOVI_CONF) ||
        av_packet_side_data_get(params->coded_side_data, params->nb_coded_side_data,
                                AV_PKT_DATA_DYNAMIC_HDR10_PLUS) ||
        !hdr10_extradata(params, (bstr){0}))
        return false;
    if (is_hvcc(params)) {
        // Matroska can supply hvcC without parsed profile or pixel format.
        // HEVCDecoderConfigurationRecord contains the profile, chroma and depth.
        const uint8_t *hvcc = params->extradata;
        if ((hvcc[1] & 0xdf) != AV_PROFILE_HEVC_MAIN_10 || hvcc[16] != 0xfd ||
            hvcc[17] != 0xfa || hvcc[18] != 0xfa)
            return false;
    } else if (params->profile == AV_PROFILE_UNKNOWN || params->format == AV_PIX_FMT_NONE) {
        return false;
    }
    if (params->format == AV_PIX_FMT_NONE)
        return true;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(params->format);
    return desc && desc->nb_components == 3 && desc->log2_chroma_w == 1 &&
           desc->log2_chroma_h == 1 && !(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
           desc->comp[0].depth == 10 && desc->comp[1].depth == 10 &&
           desc->comp[2].depth == 10;
}

static bool hdr10_packet_nal(const AVCodecParameters *params, bstr nal)
{
    if (!hdr10_nal(nal))
        return false;
    if (((nal.start[0] >> 1) & 63) != 33) // HEVC SPS.
        return true;
    // The decoder can change bit depth or color when it sees a replacement SPS.
    // Opaque MediaCodec frames cannot prove that the original format survived.
    return hdr10_extradata(params, trim_trailing_zeros(nal));
}

bool mp_native_hdr10_packet(const AVCodecParameters *params, const AVPacket *pkt)
{
    if (av_packet_get_side_data(pkt, AV_PKT_DATA_DYNAMIC_HDR10_PLUS, NULL) ||
        av_packet_get_side_data(pkt, AV_PKT_DATA_DOVI_CONF, NULL) ||
        av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, NULL) || !pkt->size)
        return false;
    const uint8_t *p = pkt->data;
    const uint8_t *end = p + pkt->size;
    if (!is_hvcc(params)) {
        const uint8_t *start = start_code(p, end);
        if (!start)
            return false;
        do {
            if (!hdr10_packet_nal(params, next_annexb_nal(&start, end)))
                return false;
        } while (start);
        return true;
    }

    unsigned length_size = (params->extradata[21] & 3) + 1;
    while (end - p >= length_size) {
        uint32_t size = 0;
        for (unsigned i = 0; i < length_size; i++)
            size = (size << 8) | *p++;
        if (size > end - p || !hdr10_packet_nal(params, (bstr){(uint8_t *)p, size}))
            return false;
        p += size;
    }
    return p == end;
}
