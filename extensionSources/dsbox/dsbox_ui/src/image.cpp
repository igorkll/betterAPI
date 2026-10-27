#include <dsbox_ui/image.hpp>
#include <dsbox_ui/render_backend.hpp>

namespace dsbox_ui
{
    Image::Image(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // Container's destructor -- see that class's own comment.
    Image::~Image() = default;

    UiSize Image::measure(const UiConstraints& constraints, IUiRenderBackend& /*backend*/)
    {
        return {
            resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth),
            resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight)
        };
    }

    void Image::arrange(const UiRect& rect, IUiRenderBackend& /*backend*/)
    {
        m_bounds = rect;
    }

    void Image::paint(DisplayListBuilder& out) const
    {
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;

        if (hasArea && m_image != kInvalidImageHandle)
            out.image(m_bounds, m_image, m_tint);
    }
}
