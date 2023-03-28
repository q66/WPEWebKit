#include "config.h"
#include "ImageBufferCairoGLSurfaceBackend.h"

#if USE(CAIRO)

#include "GLContextEGL.h"
#include "TextureMapperPlatformLayerBuffer.h"
#include "TextureMapperPlatformLayerProxyGL.h"
#include <cairo-gl.h>
#include <epoxy/gl.h>
#include <mutex>
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(ImageBufferCairoGLSurfaceBackend);

static cairo_device_t* cairoDevice()
{
    static cairo_device_t* s_device { nullptr };

    static std::once_flag s_flag;
    std::call_once(s_flag,
        [&] {
            auto& platformDisplay = PlatformDisplay::sharedDisplayForCompositing();
            auto* context = platformDisplay.sharingGLContext();
            if (is<GLContextEGL>(context))
                s_device = cairo_egl_device_create(platformDisplay.eglDisplay(), downcast<GLContextEGL>(context)->context());
        });

    return s_device;
}

class ImageBufferCairoGLDisplayDelegate final : public GraphicsLayerContentsDisplayDelegate {
public:
    ImageBufferCairoGLDisplayDelegate(Ref<Nicosia::ContentLayer>&& nicosiaLayer)
        : m_nicosiaLayer(WTFMove(nicosiaLayer))
    { }

    PlatformLayer* platformLayer() const final { return m_nicosiaLayer.ptr(); }

private:
    Ref<Nicosia::ContentLayer> m_nicosiaLayer;
};

unsigned ImageBufferCairoGLSurfaceBackend::calculateBytesPerRow(const IntSize& backendSize)
{
    ASSERT(!backendSize.isEmpty());
    return cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, backendSize.width());
}

size_t ImageBufferCairoGLSurfaceBackend::calculateMemoryCost(const Parameters& parameters)
{
    IntSize backendSize = calculateBackendSize(parameters);
    return ImageBufferBackend::calculateMemoryCost(backendSize, calculateBytesPerRow(backendSize));
}

static inline void clearSurface(cairo_surface_t* surface)
{
    RefPtr<cairo_t> cr = adoptRef(cairo_create(surface));
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr.get());
}

std::unique_ptr<ImageBufferCairoGLSurfaceBackend> ImageBufferCairoGLSurfaceBackend::create(const Parameters& parameters, const ImageBuffer::CreationContext&)
{
    IntSize backendSize = calculateBackendSize(parameters);
    if (backendSize.isEmpty())
        return { };

    auto* context = PlatformDisplay::sharedDisplayForCompositing().sharingGLContext();
    context->makeContextCurrent();

    std::array<uint32_t, 2> textures { 0, 0 };
    glGenTextures(2, textures.data());

    for (auto texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, backendSize.width(), backendSize.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    }

    auto* device = cairoDevice();
    if (!device) {
        glDeleteTextures(2, textures.data());
        return { };
    }
    cairo_gl_device_set_thread_aware(device, FALSE);

    std::array<RefPtr<cairo_surface_t>, 2> surfaces;
    surfaces[0] = adoptRef(cairo_gl_surface_create_for_texture(device, CAIRO_CONTENT_COLOR_ALPHA, textures[0], backendSize.width(), backendSize.height()));
    if (cairo_surface_status(surfaces[0].get()) != CAIRO_STATUS_SUCCESS) {
        glDeleteTextures(2, textures.data());
        return nullptr;
    }
    clearSurface(surfaces[0].get());

    surfaces[1] = adoptRef(cairo_gl_surface_create_for_texture(device, CAIRO_CONTENT_COLOR_ALPHA, textures[1], backendSize.width(), backendSize.height()));
    if (cairo_surface_status(surfaces[1].get()) != CAIRO_STATUS_SUCCESS) {
        glDeleteTextures(2, textures.data());
        return nullptr;
    }
    clearSurface(surfaces[1].get());

    return std::unique_ptr<ImageBufferCairoGLSurfaceBackend>(new ImageBufferCairoGLSurfaceBackend(parameters, textures, surfaces));
}

ImageBufferCairoGLSurfaceBackend::ImageBufferCairoGLSurfaceBackend(const Parameters& parameters, const std::array<uint32_t, 2>& textures, const std::array<RefPtr<cairo_surface_t>, 2>& surfaces)
    : ImageBufferCairoSurfaceBackend(parameters, RefPtr { surfaces[0] })
    , m_textures(textures)
    , m_surfaces(surfaces)
{
    m_nicosiaLayer = Nicosia::ContentLayer::create(Nicosia::ContentLayerTextureMapperImpl::createFactory(*this));
    m_layerContentsDisplayDelegate = adoptRef(new ImageBufferCairoGLDisplayDelegate(Ref { *m_nicosiaLayer}));

    m_compositorContext = adoptRef(cairo_create(m_surfaces[1].get()));
}

ImageBufferCairoGLSurfaceBackend::~ImageBufferCairoGLSurfaceBackend()
{
    downcast<Nicosia::ContentLayerTextureMapperImpl>(m_nicosiaLayer->impl()).invalidateClient();

    GLContext* previousActiveContext = GLContext::current();

    glDeleteTextures(2, m_textures.data());

    if (previousActiveContext)
        previousActiveContext->makeContextCurrent();
}

RefPtr<GraphicsLayerContentsDisplayDelegate> ImageBufferCairoGLSurfaceBackend::layerContentsDisplayDelegate() const
{
    return m_layerContentsDisplayDelegate;
}

void ImageBufferCairoGLSurfaceBackend::swapBuffersIfNeeded()
{
    auto backendSize = this->backendSize();

    GLContext* previousActiveContext = GLContext::current();

    // It would be great if we could just swap the buffers here as we do with webgl, but that breaks the cases
    // where one frame uses the content already rendered in the previous frame. So we just copy the content
    // into the compositor buffer.
    cairo_set_source_surface(m_compositorContext.get(), m_surfaces[0].get(), 0, 0);
    cairo_set_operator(m_compositorContext.get(), CAIRO_OPERATOR_SOURCE);
    cairo_paint(m_compositorContext.get());

    {
        auto& proxy = downcast<Nicosia::ContentLayerTextureMapperImpl>(m_nicosiaLayer->impl()).proxy();
        ASSERT(is<TextureMapperPlatformLayerProxyGL>(proxy));

        Locker locker { proxy.lock() };
        downcast<TextureMapperPlatformLayerProxyGL>(proxy).pushNextBuffer(makeUnique<TextureMapperPlatformLayerBuffer>(m_textures[1], backendSize, TextureMapperGL::ShouldBlend, GL_RGBA));
    }

    if (previousActiveContext)
        previousActiveContext->makeContextCurrent();
}

} // namespace WebCore

#endif // USE(CAIRO)
