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

#include "TrackPrivateBaseGStreamer.h"

#include "GStreamerCommon.h"
#include "Logging.h"
#include "TrackPrivateBase.h"
#include <glib-object.h>
#include <gst/gst.h>
#include <gst/tag/tag.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/CString.h>

GST_DEBUG_CATEGORY_EXTERN(webkit_media_player_debug);
#define GST_CAT_DEFAULT webkit_media_player_debug

namespace WebCore {

AtomString TrackPrivateBaseGStreamer::generateUniquePlaybin2StreamID(TrackType trackType, unsigned index)
{
    auto prefix = [trackType]() -> char {
        switch (trackType) {
        case TrackPrivateBaseGStreamer::TrackType::Audio:
            return 'A';
        case TrackPrivateBaseGStreamer::TrackType::Video:
            return 'V';
        case TrackPrivateBaseGStreamer::TrackType::Text:
            return 'T';
        default:
            ASSERT_NOT_REACHED();
            return 'U';
        }
    }();

    return AtomString(makeString(prefix, index));
}

TrackPrivateBaseGStreamer::TrackPrivateBaseGStreamer(TrackType type, TrackPrivateBase* owner, gint index, GRefPtr<GstPad> pad)
    : m_notifier(MainThreadNotifier<MainThreadNotification>::create())
    , m_index(index)
    , m_pad(pad)
    , m_owner(owner)
{
    ASSERT(m_pad);

    m_id = AtomString(trackIdFromPadStreamStartOrUniqueID(type, index, m_pad));

    g_signal_connect_swapped(m_pad.get(), "notify::tags", G_CALLBACK(tagsChangedCallback), this);

    // We can't call notifyTrackOfTagsChanged() directly, because we need tagsChanged() to setup m_tags.
    tagsChanged();
}

TrackPrivateBaseGStreamer::TrackPrivateBaseGStreamer(TrackPrivateBase* owner, gint index, GRefPtr<GstStream> stream)
    : m_notifier(MainThreadNotifier<MainThreadNotification>::create())
    , m_index(index)
    , m_stream(stream)
    , m_owner(owner)
{
    ASSERT(m_stream);

    m_id = AtomString(gst_stream_get_stream_id(m_stream.get()));

    // We can't call notifyTrackOfTagsChanged() directly, because we need tagsChanged() to setup m_tags.
    tagsChanged();
}

TrackPrivateBaseGStreamer::~TrackPrivateBaseGStreamer()
{
    disconnect();
    m_notifier->invalidate();
}

void TrackPrivateBaseGStreamer::disconnect()
{
    m_tags.clear();

    if (m_stream)
        m_stream.clear();

    m_notifier->cancelPendingNotifications();

    if (!m_pad)
        return;

    g_signal_handlers_disconnect_matched(m_pad.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    m_pad.clear();
}

void TrackPrivateBaseGStreamer::tagsChangedCallback(TrackPrivateBaseGStreamer* track)
{
    track->tagsChanged();
}

void TrackPrivateBaseGStreamer::tagsChanged()
{
    GRefPtr<GstTagList> tags;
    if (m_pad) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(m_pad.get()), "tags"))
            g_object_get(m_pad.get(), "tags", &tags.outPtr(), nullptr);
        else
            tags = adoptGRef(gst_tag_list_new_empty());
    }
    else if (m_stream)
        tags = adoptGRef(gst_stream_get_tags(m_stream.get()));
    else
        tags = adoptGRef(gst_tag_list_new_empty());

    GST_DEBUG("Inspecting track at index %d with tags: %" GST_PTR_FORMAT, m_index, tags.get());
    {
        LockHolder lock(m_tagMutex);
        m_tags.swap(tags);
    }

    m_notifier->notify(MainThreadNotification::TagsChanged, [this] { notifyTrackOfTagsChanged(); });
}

bool TrackPrivateBaseGStreamer::getLanguageCode(GstTagList* tags, AtomString& value)
{
    String language;
    if (getTag(tags, GST_TAG_LANGUAGE_CODE, language)) {
        language = gst_tag_get_language_code_iso_639_1(language.utf8().data());
        GST_DEBUG("Converted track %d's language code to %s.", m_index, language.utf8().data());
        if (language != value) {
            value = language;
            return true;
        }
    }
    return false;
}

template<class StringType>
bool TrackPrivateBaseGStreamer::getTag(GstTagList* tags, const gchar* tagName, StringType& value)
{
    GUniqueOutPtr<gchar> tagValue;
    if (gst_tag_list_get_string(tags, tagName, &tagValue.outPtr())) {
        GST_DEBUG("Track %d got %s %s.", m_index, tagName, tagValue.get());
        value = tagValue.get();
        return true;
    }
    return false;
}

void TrackPrivateBaseGStreamer::notifyTrackOfTagsChanged()
{
    TrackPrivateBaseClient* client = m_owner->client();

    GRefPtr<GstTagList> tags;
    {
        LockHolder lock(m_tagMutex);
        tags.swap(m_tags);
    }

    if (!tags)
        return;

    if (getTag(tags.get(), GST_TAG_TITLE, m_label) && client)
        client->labelChanged(m_label);

    AtomString language;
    if (!getLanguageCode(tags.get(), language))
        return;

    if (language == m_language)
        return;

    m_language = language;
    if (client)
        client->languageChanged(m_language);
}

String TrackPrivateBaseGStreamer::trackIdFromPadStreamStartOrUniqueID(TrackType type, unsigned index, const GRefPtr<GstPad>& pad)
{
    String streamId = nullString();
    if (!pad)
        return generateUniquePlaybin2StreamID(type, index);

    auto streamStart = adoptGRef(gst_pad_get_sticky_event(pad.get(), GST_EVENT_STREAM_START, 0));
    if (!streamStart)
        return generateUniquePlaybin2StreamID(type, index);

    const gchar* streamIdAsCharacters;
    gst_event_parse_stream_start(streamStart.get(), &streamIdAsCharacters);

    if (!streamIdAsCharacters)
        return generateUniquePlaybin2StreamID(type, index);

    StringView streamIdView = StringView(streamIdAsCharacters);
    size_t position = streamIdView.find('/');
    if (position == notFound || position + 1 == streamIdView.length())
        return generateUniquePlaybin2StreamID(type, index);

    return streamIdView.substring(position + 1).toString();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && ENABLE(VIDEO_TRACK)
