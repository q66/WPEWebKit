/*
 * Copyright (C) 2013 Cable Television Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && ENABLE(VIDEO_TRACK)

#include "AudioTrackPrivateGStreamer.h"

#include "MediaPlayerPrivateGStreamer.h"
#include <glib-object.h>
#include <gst/base/gstbitreader.h>

GST_DEBUG_CATEGORY_EXTERN(webkit_media_player_debug);
#define GST_CAT_DEFAULT webkit_media_player_debug

namespace WebCore {

// Utility codec functions (for audio use case only) not yet available in gst 1.18 borrowed from:
// https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gst-plugins-base/gst-libs/gst/pbutils/codec-utils.c

static gboolean
gst_codec_utils_aac_get_audio_object_type (GstBitReader * br,
        guint8 * audio_object_type)
{
    guint8 aot;

    if (!gst_bit_reader_get_bits_uint8 (br, &aot, 5))
        return FALSE;

    if (aot == 31) {
        if (!gst_bit_reader_get_bits_uint8 (br, &aot, 6))
            return FALSE;
        aot += 32;
    }

    *audio_object_type = aot;

    return TRUE;
}

static gboolean
aac_caps_structure_get_audio_object_type (GstStructure * caps_st,
        guint8 * audio_object_type)
{
    gboolean ret = FALSE;
    const GValue *codec_data_value = NULL;
    GstBuffer *codec_data = NULL;
    GstMapInfo map;
    guint8 *data = NULL;
    gsize size;
    GstBitReader br;

    codec_data_value = gst_structure_get_value (caps_st, "codec_data");
    if (!codec_data_value) {
        GST_DEBUG
                ("audio/mpeg pad did not have codec_data set, cannot parse audio object type");
        return FALSE;
    }

    codec_data = gst_value_get_buffer (codec_data_value);
    if (!gst_buffer_map (codec_data, &map, GST_MAP_READ)) {
        return FALSE;
    }
    data = map.data;
    size = map.size;

    if (size < 2) {
        GST_WARNING ("aac codec data is too small");
        goto done;
    }

    gst_bit_reader_init (&br, data, size);
    ret = gst_codec_utils_aac_get_audio_object_type (&br, audio_object_type);

done:
    gst_buffer_unmap (codec_data, &map);

    return ret;
}

static gchar *
codec_utils_caps_get_mime_codec (GstCaps * caps)
{
    gchar *mime_codec = NULL;
    GstStructure *caps_st = NULL;
    const gchar *media_type = NULL;

    g_return_val_if_fail (caps != NULL, NULL);
    g_return_val_if_fail (gst_caps_is_fixed (caps), NULL);

    caps_st = gst_caps_get_structure (caps, 0);
    if (caps_st == NULL) {
        GST_WARNING ("Failed to get structure from caps");
        goto done;
    }

    media_type = gst_structure_get_name (caps_st);

    if (g_strcmp0 (media_type, "audio/mpeg") == 0) {
        guint8 audio_object_type = 0;
        if (aac_caps_structure_get_audio_object_type (caps_st, &audio_object_type)) {
            mime_codec = g_strdup_printf ("mp4a.40.%u", audio_object_type);
        } else {
            mime_codec = g_strdup ("mp4a.40");
        }
    } else if (g_strcmp0 (media_type, "audio/x-opus") == 0) {
        mime_codec = g_strdup ("opus");
    } else if (g_strcmp0 (media_type, "audio/x-mulaw") == 0) {
        mime_codec = g_strdup ("ulaw");
    } else if (g_strcmp0 (media_type, "audio/x-adpcm") == 0) {
        if (g_strcmp0 (gst_structure_get_string (caps_st, "layout"), "g726") == 0) {
            mime_codec = g_strdup ("g726");
        }
    }

done:
    return mime_codec;
}

AudioTrackPrivateGStreamer::AudioTrackPrivateGStreamer(WeakPtr<MediaPlayerPrivateGStreamer> player, gint index, GRefPtr<GstPad> pad, AtomString streamID)
    : TrackPrivateBaseGStreamer(this, index, pad)
    , m_player(player)
{
    if (streamID.isNull())
        m_id = "A" + String::number(index);
    else
        m_id = streamID;

    if (m_pad) {
        g_signal_connect_swapped(m_pad.get(), "notify::caps", G_CALLBACK(+[](AudioTrackPrivateGStreamer* track) {
            track->m_taskQueue.enqueueTask([track]() {
                if (!track->m_pad)
                    return;
                auto caps = adoptGRef(gst_pad_get_current_caps(track->m_pad.get()));
                if (!caps)
                    return;
                track->updateConfigurationFromCaps();
            });
        }), this);
    }
}

AudioTrackPrivateGStreamer::AudioTrackPrivateGStreamer(WeakPtr<MediaPlayerPrivateGStreamer> player, gint index, GRefPtr<GstStream> stream)
    : TrackPrivateBaseGStreamer(this, index, stream)
    , m_player(player)
{
    gint kind;
    auto tags = adoptGRef(gst_stream_get_tags(m_stream.get()));

    if (tags && gst_tag_list_get_int(tags.get(), "webkit-media-stream-kind", &kind) && kind == static_cast<int>(VideoTrackPrivate::Kind::Main)) {
        GstStreamFlags streamFlags = gst_stream_get_stream_flags(stream.get());
        gst_stream_set_stream_flags(stream.get(), static_cast<GstStreamFlags>(streamFlags | GST_STREAM_FLAG_SELECT));
    }

    m_id = gst_stream_get_stream_id(stream.get());

    if (m_stream) {
        g_signal_connect_swapped(m_stream.get(), "notify::caps", G_CALLBACK(+[](AudioTrackPrivateGStreamer* track) {
            track->m_taskQueue.enqueueTask([track]() {
                track->updateConfigurationFromCaps();
            });
        }), this);
    }
}

void AudioTrackPrivateGStreamer::updateConfigurationFromTags(const GRefPtr<GstTagList>& tags)
{
    ASSERT(isMainThread());
    if (!tags)
        return;

    unsigned bitrate;
    if (gst_tag_list_get_uint(tags.get(), GST_TAG_BITRATE, &bitrate)) {
        if (m_stream)
            GST_DEBUG_OBJECT(m_stream.get(), "Setting bitrate to %u", bitrate);
        else if (m_pad)
            GST_DEBUG_OBJECT(m_pad.get(), "Setting bitrate to %u", bitrate);
        setBitrate(bitrate);
    }
}

static GRefPtr<GstPad> findOriginalCapsSrcPad(GRefPtr<GstPad> sinkPad) {
    if (!sinkPad)
        return nullptr;
    
    GRefPtr<GstPad> srcPad = adoptGRef(gst_pad_get_peer(sinkPad.get()));
    GRefPtr<GstElement> element;

    const int PAD_INDEX_NOT_FOUND = -1;
    struct PadSearchContext {
        int currentPadIndex;
        int targetPadIndex;
        GRefPtr<GstPad> targetPad;
    };

    GRefPtr<GstCaps> srcPadCaps = adoptGRef(gst_pad_get_current_caps(srcPad.get()));
    if (!srcPadCaps || !gst_caps_is_fixed(srcPadCaps.get()))
        return nullptr;

    const auto* structure = gst_caps_get_structure(srcPadCaps.get(), 0);
    if (!structure)
        return nullptr;

    while (!g_strcmp0(gst_structure_get_name(structure), "audio/x-raw")) {
        while (GST_IS_GHOST_PAD(srcPad.get()))
            srcPad = adoptGRef(gst_ghost_pad_get_target(GST_GHOST_PAD(srcPad.get())));

        element = adoptGRef(gst_pad_get_parent_element(srcPad.get()));
        PadSearchContext padSearchContext { 0, PAD_INDEX_NOT_FOUND, srcPad };

        gst_element_foreach_src_pad(element.get(), (GstElementForeachPadFunc)(+[](GstElement*, GstPad* pad, gpointer userData) {
            PadSearchContext* padSearchContextPtr(static_cast<PadSearchContext*>(userData));
            if (pad == padSearchContextPtr->targetPad.get()) {
                padSearchContextPtr->targetPadIndex = padSearchContextPtr->currentPadIndex;
                return FALSE;
            }
            padSearchContextPtr->currentPadIndex++;
            return TRUE;
        }), &padSearchContext);
        if (padSearchContext.targetPadIndex == PAD_INDEX_NOT_FOUND)
            return nullptr;

        padSearchContext = PadSearchContext { 0, padSearchContext.targetPadIndex, nullptr };

        gst_element_foreach_sink_pad(element.get(), (GstElementForeachPadFunc)(+[](GstElement*, GstPad* pad, gpointer userData) {
            PadSearchContext* padSearchContextPtr(static_cast<PadSearchContext*>(userData));
            if (padSearchContextPtr->currentPadIndex == padSearchContextPtr->targetPadIndex) {
                padSearchContextPtr->targetPad = pad;
                return FALSE;
            }
            padSearchContextPtr->currentPadIndex++;
            return TRUE;
        }), &padSearchContext);

        if (!padSearchContext.targetPad)
            return nullptr;

        srcPad = adoptGRef(gst_pad_get_peer(padSearchContext.targetPad.get()));
        if (!srcPad)
            return nullptr;

        srcPadCaps = adoptGRef(gst_pad_get_current_caps(srcPad.get()));
        if (!srcPadCaps || !gst_caps_is_fixed(srcPadCaps.get()))
            return nullptr;

        structure = gst_caps_get_structure(srcPadCaps.get(), 0);
        if (!structure)
            return nullptr;
    };
        
    return srcPad;
}

void AudioTrackPrivateGStreamer::updateConfigurationFromCaps()
{
    ASSERT(isMainThread());
    GRefPtr<GstCaps> caps;

    if (m_stream)
        caps = adoptGRef(gst_stream_get_caps(m_stream.get()));
    else if (m_pad)
        caps = adoptGRef(gst_pad_get_current_caps(m_pad.get()));

    if (!caps || !gst_caps_is_fixed(caps.get()))
        return;

    const auto* structure = gst_caps_get_structure(caps.get(), 0);
    if (areEncryptedCaps(caps.get())) {
        int sampleRate, numberOfChannels;
        if (gst_structure_get_int(structure, "rate", &sampleRate))
            setSampleRate(sampleRate);
        if (gst_structure_get_int(structure, "channels", &numberOfChannels))
            setNumberOfChannels(numberOfChannels);
        return;
    }

    GstAudioInfo info;
    if (gst_audio_info_from_caps(&info, caps.get())) {
        setSampleRate(GST_AUDIO_INFO_RATE(&info));
        setNumberOfChannels(GST_AUDIO_INFO_CHANNELS(&info));
    }

    GRefPtr<GstCaps> originalCaps = caps;
    if (structure && !g_strcmp0(gst_structure_get_name(structure), "audio/x-raw")) {
        // This is decoded audio. Try to get the original caps before the decoder.
        if (m_pad) {
            GRefPtr<GstPad> originalCapsPad = findOriginalCapsSrcPad(m_pad);
            if (originalCapsPad)
                originalCaps = adoptGRef(gst_pad_get_current_caps(originalCapsPad.get()));
        }
    }

    // TODO: Use GStreamer native implementation after gst 1.20.
    if (originalCaps) {
        GUniquePtr<char> codec(codec_utils_caps_get_mime_codec(originalCaps.get()));
        if (codec)
            setCodec(codec.get());
    }
}

AudioTrackPrivate::Kind AudioTrackPrivateGStreamer::kind() const
{
    if (m_stream.get() && gst_stream_get_stream_flags(m_stream.get()) & GST_STREAM_FLAG_SELECT)
        return AudioTrackPrivate::Kind::Main;

    return AudioTrackPrivate::kind();
}

void AudioTrackPrivateGStreamer::disconnect()
{
    m_player = nullptr;
    TrackPrivateBaseGStreamer::disconnect();
}

void AudioTrackPrivateGStreamer::setEnabled(bool enabled)
{
    if (enabled == this->enabled())
        return;
    AudioTrackPrivate::setEnabled(enabled);

    if (m_player)
        m_player->updateEnabledAudioTrack();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && ENABLE(VIDEO_TRACK)
