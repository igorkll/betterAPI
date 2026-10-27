#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/length.hpp>

#include <string>

namespace dsbox_ui
{
    // Single-image leaf: draws one ImageCommand over its own bounds().
    // This is v1's one image element -- see Container's own class
    // comment (container.hpp) for the same "future HTML5-like elements
    // arrive as sibling IUiElement implementations" framing this
    // follows.
    //
    // v1 sizing limitation: unlike Text (which sizes itself from its
    // own content via measureText()), there is no
    // IUiRenderBackend::measureImage()/imageSize() call anywhere in
    // dsbox_ui yet -- registerImage() (render_backend.hpp) hands back
    // an opaque ImageHandle with no way to ask it its own pixel
    // dimensions. So Image sizes itself the exact same way Container
    // does: measure() resolves its OWN UiLength width/height (see
    // setPreferredWidth()/Height()) directly against the incoming
    // UiConstraints via resolveLengthDimension() (length.hpp) -- Pixel
    // and Percent both work correctly, but Auto/Content/Fraction do
    // NOT size this Image to the underlying image's natural pixel
    // size (there is nothing here that even knows what that is). Give
    // every Image an explicit Pixel or Percent size for now, same
    // advice as Container's own class comment.
    //
    // v1 identity limitation: same as Container's own (container.hpp)
    // -- the caller supplies id(), there is no id-minting factory yet.
    class DSBOX_UI_API Image final : public IUiElement
    {
    public:
        explicit Image(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~Image() override;

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        // -- IUiElement --
        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return m_idMode; }
        std::string_view name() const noexcept override { return m_name; }

        UiSize measure(const UiConstraints& constraints, IUiRenderBackend& backend) override;
        void arrange(const UiRect& rect, IUiRenderBackend& backend) override;
        UiRect bounds() const noexcept override { return m_bounds; }

        // Emits one ImageCommand covering this element's own bounds(),
        // tinted by setTint(). Emits nothing if bounds() has zero (or
        // negative) area, or if setImage() is still kInvalidImageHandle
        // -- same "nothing meaningful to draw" reasoning as Container::
        // paint()'s own degenerate-container check.
        void paint(DisplayListBuilder& out) const override;

        // Not interactive in v1 -- always returns false. Wrapping an
        // Image in a focusable/clickable Container (see Container's
        // own "-- Interactivity --" section) is the intended way to
        // get a clickable picture for now, the same way an HTML <img>
        // needs a surrounding <a>/<button> (or its own tabindex/
        // onclick) to become one.
        bool handleEvent(const UiEvent&, IUiElementActions&) override { return false; }

        // Always empty -- a leaf, same as Text.
        std::span<std::unique_ptr<IUiElement>> children() override { return {}; }

        // -- Image-specific configuration --

        // kInvalidImageHandle (the default) means "nothing to draw" --
        // see paint()'s own doc. Set this to whatever
        // IUiRenderBackend::registerImage() returned for the desired
        // pixel data; this class never calls registerImage() itself,
        // the same reasoning Text gives for not calling loadFont()
        // itself (see text.hpp) -- several Image elements may want to
        // share one already-registered image.
        void setImage(ImageHandle image) { m_image = image; }
        ImageHandle image() const noexcept { return m_image; }

        // Multiplied into the image's own pixels by whatever
        // IUiRenderBackend implementation actually draws the
        // ImageCommand -- white (the default) means "no tint".
        void setTint(UiColor tint) { m_tint = tint; }
        UiColor tint() const noexcept { return m_tint; }

        // See this class's own "v1 sizing limitation" above.
        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }
        void setPreferredHeight(UiLength height) { m_preferredHeight = height; }

    private:
        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        ImageHandle m_image = kInvalidImageHandle;
        UiColor m_tint{ 1.0f, 1.0f, 1.0f, 1.0f };

        UiLength m_preferredWidth = UiLength::autoLength();
        UiLength m_preferredHeight = UiLength::autoLength();

        UiRect m_bounds;
    };
}
