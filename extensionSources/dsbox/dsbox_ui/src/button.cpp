#include <dsbox_ui/button.hpp>
#include <dsbox_ui/stack_layout.hpp>

namespace dsbox_ui
{
    ButtonParts makeButton(UiElementId containerId, UiElementId labelId, std::string labelText, UiElementIdMode idMode)
    {
        auto container = std::make_unique<Container>(containerId, idMode, "button");
        container->setBackgroundColor(UiColor{ 0.25f, 0.45f, 0.85f, 1.0f });
        container->setPressedColor(UiColor{ 0.15f, 0.30f, 0.65f, 1.0f });
        container->setCornerRadius(8.0f);
        container->setPreferredWidth(UiLength::pixels(120.0f));  // see makeButton()'s own "v1 sizing limitation"
        container->setPreferredHeight(UiLength::pixels(40.0f));
        container->setPadding(0.0f, 16.0f, 0.0f, 16.0f);
        container->setFocusable(true);

        // Horizontal, single child -- exists only so the label
        // actually gets arrange()'d at all (see Container::arrange()'s
        // own "no strategy -> children simply aren't positioned"
        // behavior). CrossAlign::Center is what actually centers the
        // label vertically; StackLayout's default (Stretch) would
        // instead give it the full button height and pin its baseline
        // near the top -- see StackLayout's own CrossAlign doc.
        container->setLayoutStrategy(
            std::make_unique<StackLayout>(StackDirection::Horizontal, 0.0f, CrossAlign::Center));

        auto& label = static_cast<Text&>(
            container->addChild(std::make_unique<Text>(labelId, idMode, "buttonLabel")));
        label.setText(std::move(labelText));
        label.setColor(UiColor{ 1.0f, 1.0f, 1.0f, 1.0f });

        ButtonParts parts;
        parts.label = &label;
        parts.root = std::move(container);
        return parts;
    }
}
