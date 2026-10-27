#include <dsbox/input.hpp>

#include <utility>

namespace dsbox
{
    Input::Input(CommunicationManager& bus, WindowHandle handle, InputOptions options)
        : m_bus(bus)
        , m_handle(handle)
        , m_options(options)
    {
    }

    Input::~Input() = default;

    void Input::setKeyState(KeyCode key, bool pressed, bool repeat, KeyModifier modifiers)
    {
        if (m_options.trackState)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (pressed)
                m_pressedKeys.insert(key);
            else
                m_pressedKeys.erase(key);
        }

        if (m_options.dispatchEvents)
        {
            KeyEvent payload{ m_handle, key, modifiers, pressed, repeat };
            m_bus.emit<KeyEvent>("window.key", payload);
        }
    }

    void Input::setMouseButtonState(MouseButton button, bool pressed, int x, int y)
    {
        if (m_options.trackState)
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (pressed)
                m_pressedButtons.insert(button);
            else
                m_pressedButtons.erase(button);

            m_mouseX = x;
            m_mouseY = y;
        }

        if (m_options.dispatchEvents)
        {
            MouseButtonEvent payload{ m_handle, button, pressed, x, y };
            m_bus.emit<MouseButtonEvent>("window.mouse_button", payload);
        }
    }

    void Input::setMousePosition(int x, int y, int deltaX, int deltaY)
    {
        if (m_options.trackState)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_mouseX = x;
            m_mouseY = y;
        }

        if (m_options.dispatchEvents)
        {
            MouseMotionEvent payload{ m_handle, x, y, deltaX, deltaY };
            m_bus.emit<MouseMotionEvent>("window.mouse_motion", payload);
        }
    }

    void Input::setMouseWheel(float scrollX, float scrollY)
    {
        // Deliberately no state update -- see MouseWheelEvent's own
        // comment for why wheel motion is never tracked.
        if (m_options.dispatchEvents)
        {
            MouseWheelEvent payload{ m_handle, scrollX, scrollY };
            m_bus.emit<MouseWheelEvent>("window.mouse_wheel", payload);
        }
    }

    bool Input::isKeyDown(KeyCode key) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pressedKeys.contains(key);
    }

    bool Input::isMouseButtonDown(MouseButton button) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pressedButtons.contains(button);
    }

    bool Input::mousePosition(int& outX, int& outY) const
    {
        if (!m_options.trackState)
            return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        outX = m_mouseX;
        outY = m_mouseY;
        return true;
    }
}
