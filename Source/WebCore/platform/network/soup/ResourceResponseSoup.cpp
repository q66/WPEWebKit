/*
 * Copyright (C) 2009 Gustavo Noronha Silva
 * Copyright (C) 2009 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#if USE(SOUP)

#include "ResourceResponse.h"

#include "HTTPHeaderNames.h"
#include "HTTPParsers.h"
#include "MIMETypeRegistry.h"
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

ResourceResponse::ResourceResponse(SoupMessage* soupMessage, const CString& sniffedContentType)
{
    m_url = URL(soup_message_get_uri(soupMessage));

    switch (soup_message_get_http_version(soupMessage)) {
    case SOUP_HTTP_1_0:
        m_httpVersion = AtomicString("HTTP/1.0", AtomicString::ConstructFromLiteral);
        break;
    case SOUP_HTTP_1_1:
        m_httpVersion = AtomicString("HTTP/1.1", AtomicString::ConstructFromLiteral);
        break;
    }
    m_httpStatusCode = soupMessage->status_code;
    setHTTPStatusText(soupMessage->reason_phrase);

    GTlsCertificate* certificate = 0;
    soup_message_get_https_status(soupMessage, &certificate, &m_tlsErrors);
    m_certificate = certificate;

    updateFromSoupMessageHeaders(soupMessage->response_headers);

    String contentType;
    const char* officialType = soup_message_headers_get_one(soupMessage->response_headers, "Content-Type");
    if (!sniffedContentType.isNull() && m_httpStatusCode != SOUP_STATUS_NOT_MODIFIED && sniffedContentType != officialType)
        contentType = sniffedContentType.data();
    else
        contentType = officialType;
    setMimeType(extractMIMETypeFromMediaType(contentType));
    if (m_mimeType.isEmpty() && m_httpStatusCode != SOUP_STATUS_NOT_MODIFIED)
        setMimeType(MIMETypeRegistry::getMIMETypeForPath(m_url.path()));
    setTextEncodingName(extractCharsetFromMediaType(contentType));

    setExpectedContentLength(soup_message_headers_get_content_length(soupMessage->response_headers));
}

void ResourceResponse::updateSoupMessageHeaders(SoupMessageHeaders* soupHeaders) const
{
    for (const auto& header : httpHeaderFields())
        soup_message_headers_append(soupHeaders, header.key.utf8().data(), header.value.utf8().data());
}

void ResourceResponse::updateFromSoupMessageHeaders(SoupMessageHeaders* soupHeaders)
{
    SoupMessageHeadersIter headersIter;
    const char* headerName;
    const char* headerValue;
    soup_message_headers_iter_init(&headersIter, soupHeaders);
    while (soup_message_headers_iter_next(&headersIter, &headerName, &headerValue))
        addHTTPHeaderField(String(headerName), String(headerValue));
}

CertificateInfo ResourceResponse::platformCertificateInfo() const
{
    return CertificateInfo(m_certificate.get(), m_tlsErrors);
}

String ResourceResponse::platformSuggestedFilename() const
{
    String contentDisposition(httpHeaderField(HTTPHeaderName::ContentDisposition));
    if (contentDisposition.isEmpty())
        return String();

    if (contentDisposition.is8Bit())
        contentDisposition = String::fromUTF8WithLatin1Fallback(contentDisposition.characters8(), contentDisposition.length());
    SoupMessageHeaders* soupHeaders = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(soupHeaders, "Content-Disposition", contentDisposition.utf8().data());
    GRefPtr<GHashTable> params;
    soup_message_headers_get_content_disposition(soupHeaders, nullptr, &params.outPtr());
    soup_message_headers_free(soupHeaders);
    char* filename = params ? static_cast<char*>(g_hash_table_lookup(params.get(), "filename")) : nullptr;
    return filename ? String::fromUTF8(filename) : String();
}

}

#endif
