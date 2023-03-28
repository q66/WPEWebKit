#pragma once

#if USE(CAIRO)

#include "ImageBufferCairoSurfaceBackend.h"

#include "GraphicsContextCairo.h"
#include "NicosiaContentLayerTextureMapperImpl.h"
#include <array>

namespace WebCore {

class ImageBufferCairoGLDisplayDelegate;

class ImageBufferCairoGLSurfaceBackend : public ImageBufferCairoSurfaceBackend, public Nicosia::ContentLayerTextureMapperImpl::Client {
    WTF_MAKE_ISO_ALLOCATED(ImageBufferCairoGLSurfaceBackend);
    WTF_MAKE_NONCOPYABLE(ImageBufferCairoGLSurfaceBackend);
public:
    static unsigned calculateBytesPerRow(const IntSize& backendSize);
    static size_t calculateMemoryCost(const Parameters&);

    static std::unique_ptr<ImageBufferCairoGLSurfaceBackend> create(const Parameters&, const ImageBuffer::CreationContext&);

    virtual ~ImageBufferCairoGLSurfaceBackend();

    RefPtr<GraphicsLayerContentsDisplayDelegate> layerContentsDisplayDelegate() const final;

private:
    ImageBufferCairoGLSurfaceBackend(const Parameters&, const std::array<uint32_t, 2>&, const std::array<RefPtr<cairo_surface_t>, 2>&);

    void swapBuffersIfNeeded() final;

    RefPtr<Nicosia::ContentLayer> m_nicosiaLayer;
    RefPtr<ImageBufferCairoGLDisplayDelegate> m_layerContentsDisplayDelegate;

    std::array<uint32_t, 2> m_textures;
    std::array<RefPtr<cairo_surface_t>, 2> m_surfaces;

    RefPtr<cairo_t> m_compositorContext;
};

} // namespace WebCore

#endif // USE(CAIRO)
