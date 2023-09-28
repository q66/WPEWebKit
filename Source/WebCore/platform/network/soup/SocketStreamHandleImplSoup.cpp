/*
 * Copyright (C) 2009, 2011 Google Inc.  All rights reserved.
 * Copyright (C) 2012 Samsung Electronics Ltd. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SocketStreamHandleImpl.h"

#if USE(SOUP)

#include "DeprecatedGlobalSettings.h"
#include "Logging.h"
#include "NetworkStorageSession.h"
#include "ResourceError.h"
#include "SocketStreamError.h"
#include "SocketStreamHandleClient.h"
#include "SoupNetworkSession.h"
#include "URL.h"
#include "URLSoup.h"
#include <gio/gio.h>
#include <glib.h>
#include <wtf/Vector.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/text/CString.h>

#define READ_BUFFER_SIZE 1024

namespace WebCore {

static gboolean acceptCertificateCallback(GTlsConnection*, GTlsCertificate* certificate, GTlsCertificateFlags errors, SocketStreamHandleImpl* handle)
{
    // FIXME: Using DeprecatedGlobalSettings from here is a layering violation.
    if (DeprecatedGlobalSettings::allowsAnySSLCertificate())
        return TRUE;

    return !SoupNetworkSession::checkTLSErrors(handle->url(), certificate, errors);
}

static void socketClientEventCallback(GSocketClient*, GSocketClientEvent event, GSocketConnectable*, GIOStream* connection, SocketStreamHandleImpl* handle)
{
    if (event != G_SOCKET_CLIENT_TLS_HANDSHAKING)
        return;

    g_signal_connect(connection, "accept-certificate", G_CALLBACK(acceptCertificateCallback), handle);
}

Ref<SocketStreamHandleImpl> SocketStreamHandleImpl::create(const URL& url, SocketStreamHandleClient& client, PAL::SessionID sessionID, const String&, SourceApplicationAuditToken&&)
{
    Ref<SocketStreamHandleImpl> socket = adoptRef(*new SocketStreamHandleImpl(url, client));

    auto* networkStorageSession = NetworkStorageSession::storageSession(sessionID);
    if (!networkStorageSession)
        return socket;

    Ref<SocketStreamHandle> protectedSocketStreamHandle = socket.copyRef();
    auto uri = urlToSoupURI(url);
    auto soupMessage = adoptGRef(soup_message_new_from_uri(SOUP_METHOD_GET, uri.get()));
    if (url.protocolIs("wss"))
        g_signal_connect(soupMessage.get(), "accept-certificate", G_CALLBACK(acceptCertificateCallback), &protectedSocketStreamHandle.leakRef());
    char* protocols[] = {"ws", "wss", nullptr};
    soup_session_websocket_connect_async(networkStorageSession->getOrCreateSoupNetworkSession().soupSession(), soupMessage.get(), nullptr,
        protocols, SOUP_MESSAGE_PRIORITY_NORMAL, socket->m_cancellable.get(),
        reinterpret_cast<GAsyncReadyCallback>(connectedCallback), &protectedSocketStreamHandle.leakRef());

    return socket;
}

Ref<SocketStreamHandleImpl> SocketStreamHandleImpl::create(GSocketConnection* socketConnection, SocketStreamHandleClient& client)
{
    Ref<SocketStreamHandleImpl> socket = adoptRef(*new SocketStreamHandleImpl(URL(), client));
    GRefPtr<GIOStream> stream = G_IO_STREAM(socketConnection);
    socket->connected(GRefPtr<SoupWebsocketConnection>(), WTFMove(stream));
    return socket;
}

SocketStreamHandleImpl::SocketStreamHandleImpl(const URL& url, SocketStreamHandleClient& client)
    : SocketStreamHandle(url, client)
    , m_cancellable(adoptGRef(g_cancellable_new()))
{
    LOG(Network, "SocketStreamHandle %p new client %p", this, &m_client);
}

SocketStreamHandleImpl::~SocketStreamHandleImpl()
{
    LOG(Network, "SocketStreamHandle %p delete", this);
}

void SocketStreamHandleImpl::connected(GRefPtr<SoupWebsocketConnection>&& connection, GRefPtr<GIOStream>&& stream)
{
    m_connection = WTFMove(connection);
    m_stream = WTFMove(stream);
    m_outputStream = G_POLLABLE_OUTPUT_STREAM(g_io_stream_get_output_stream(m_stream.get()));
    m_inputStream = g_io_stream_get_input_stream(m_stream.get());
    m_readBuffer = makeUniqueArray<char>(READ_BUFFER_SIZE);

    RefPtr<SocketStreamHandleImpl> protectedThis(this);
    g_input_stream_read_async(m_inputStream.get(), m_readBuffer.get(), READ_BUFFER_SIZE, RunLoopSourcePriority::AsyncIONetwork, m_cancellable.get(),
        reinterpret_cast<GAsyncReadyCallback>(readReadyCallback), protectedThis.leakRef());

    m_state = Open;
    m_client.didOpenSocketStream(*this);
}

void SocketStreamHandleImpl::connectedCallback(GObject* object, GAsyncResult* result, SocketStreamHandleImpl* handle)
{
    RefPtr<SocketStreamHandle> protectedThis = adoptRef(handle);

    // Always finish the connection, even if this SocketStreamHandle was cancelled earlier.
    GUniqueOutPtr<GError> error;
    GRefPtr<SoupWebsocketConnection> connection =  adoptGRef(soup_session_websocket_connect_finish(SOUP_SESSION(object), result, &error.outPtr()));

    // The SocketStreamHandle has been cancelled, so just close the connection, ignoring errors.
    if (g_cancellable_is_cancelled(handle->m_cancellable.get())) {
        if (connection)
            soup_websocket_connection_close(connection.get(), 0, nullptr);
        return;
    }

    if (error)
        handle->didFail(SocketStreamError(error->code, { }, error->message));
    else {
        GRefPtr<GIOStream> stream = adoptGRef(soup_websocket_connection_get_io_stream(connection.get()));
        handle->connected(WTFMove(connection), WTFMove(stream));
    }
}

void SocketStreamHandleImpl::readBytes(gssize bytesRead)
{
    if (!bytesRead) {
        close();
        return;
    }

    // The client can close the handle, potentially removing the last reference.
    RefPtr<SocketStreamHandle> protectedThis(this);
    if (bytesRead == -1)
        m_client.didFailToReceiveSocketStreamData(*this);
    else
        m_client.didReceiveSocketStreamData(*this, m_readBuffer.get(), static_cast<size_t>(bytesRead));

    if (m_inputStream) {
        g_input_stream_read_async(m_inputStream.get(), m_readBuffer.get(), READ_BUFFER_SIZE, RunLoopSourcePriority::AsyncIONetwork, m_cancellable.get(),
            reinterpret_cast<GAsyncReadyCallback>(readReadyCallback), protectedThis.leakRef());
    }
}

void SocketStreamHandleImpl::readReadyCallback(GInputStream* stream, GAsyncResult* result, SocketStreamHandleImpl* handle)
{
    RefPtr<SocketStreamHandle> protectedThis = adoptRef(handle);

    // Always finish the read, even if this SocketStreamHandle was cancelled earlier.
    GUniqueOutPtr<GError> error;
    gssize bytesRead = g_input_stream_read_finish(stream, result, &error.outPtr());

    if (g_cancellable_is_cancelled(handle->m_cancellable.get()))
        return;

    if (error)
        handle->didFail(SocketStreamError(error->code, String(), error->message));
    else
        handle->readBytes(bytesRead);
}

void SocketStreamHandleImpl::didFail(SocketStreamError&& error)
{
    m_client.didFailSocketStream(*this, WTFMove(error));
}

void SocketStreamHandleImpl::writeReady()
{
    // We no longer have buffered data, so stop waiting for the socket to be writable.
    if (!bufferedAmount()) {
        stopWaitingForSocketWritability();
        return;
    }

    sendPendingData();
}

std::optional<size_t> SocketStreamHandleImpl::platformSendInternal(const uint8_t* data, size_t length)
{
    LOG(Network, "SocketStreamHandle %p platformSend", this);
    if (!m_outputStream || !data)
        return 0;

    GUniqueOutPtr<GError> error;
    gssize written = g_pollable_output_stream_write_nonblocking(m_outputStream.get(), reinterpret_cast<const char*>(data), length, m_cancellable.get(), &error.outPtr());
    if (error) {
        if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            beginWaitingForSocketWritability();
        else
            didFail(SocketStreamError(error->code, String(), error->message));
        return std::nullopt;
    }

    // If we did not send all the bytes we were given, we know that
    // SocketStreamHandle will need to send more in the future.
    if (written == -1 || static_cast<size_t>(written) < length)
        beginWaitingForSocketWritability();

    if (written == -1)
        return std::nullopt;

    return static_cast<size_t>(written);
}

void SocketStreamHandleImpl::platformClose()
{
    LOG(Network, "SocketStreamHandle %p platformClose", this);
    // We cancel this handle first to disable all callbacks.
    g_cancellable_cancel(m_cancellable.get());
    stopWaitingForSocketWritability();

    if (m_stream) {
        GUniqueOutPtr<GError> error;
        g_io_stream_close(m_stream.get(), nullptr, &error.outPtr());
        if (error)
            didFail(SocketStreamError(error->code, { }, error->message));
        m_stream = nullptr;
    }

    m_outputStream = nullptr;
    m_inputStream = nullptr;
    m_readBuffer = nullptr;

    m_client.didCloseSocketStream(*this);
}

void SocketStreamHandleImpl::beginWaitingForSocketWritability()
{
    if (m_writeReadySource) // Already waiting.
        return;

    m_writeReadySource = adoptGRef(g_pollable_output_stream_create_source(m_outputStream.get(), m_cancellable.get()));
    ref();
    g_source_set_callback(m_writeReadySource.get(), reinterpret_cast<GSourceFunc>(reinterpret_cast<GCallback>(writeReadyCallback)), this, [](gpointer handle) {
        static_cast<SocketStreamHandleImpl*>(handle)->deref();
    });
    g_source_attach(m_writeReadySource.get(), g_main_context_get_thread_default());
}

void SocketStreamHandleImpl::stopWaitingForSocketWritability()
{
    if (!m_writeReadySource) // Not waiting.
        return;

    g_source_destroy(m_writeReadySource.get());
    m_writeReadySource = nullptr;
}

gboolean SocketStreamHandleImpl::writeReadyCallback(GPollableOutputStream*, SocketStreamHandleImpl* handle)
{
    if (g_cancellable_is_cancelled(handle->m_cancellable.get()))
        return G_SOURCE_REMOVE;

    handle->writeReady();
    return G_SOURCE_CONTINUE;
}

} // namespace WebCore

#endif
