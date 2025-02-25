// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include "qwasmcompositor.h"
#include "qwasmwindow.h"
#include "qwasmeventtranslator.h"
#include "qwasmeventdispatcher.h"
#include "qwasmclipboard.h"
#include "qwasmevent.h"

#include <QtGui/private/qwindow_p.h>

#include <private/qguiapplication_p.h>

#include <qpa/qwindowsysteminterface.h>
#include <QtCore/qcoreapplication.h>
#include <QtGui/qguiapplication.h>

#include <emscripten/bind.h>

namespace {
QWasmWindow *asWasmWindow(QWindow *window)
{
    return static_cast<QWasmWindow*>(window->handle());
}
}  // namespace

using namespace emscripten;

Q_GUI_EXPORT int qt_defaultDpiX();

bool g_scrollingInvertedFromDevice = false;

static void mouseWheelEvent(emscripten::val event)
{
    emscripten::val wheelInverted = event["webkitDirectionInvertedFromDevice"];
    if (wheelInverted.as<bool>())
        g_scrollingInvertedFromDevice = true;
}

EMSCRIPTEN_BINDINGS(qtMouseModule) {
    function("qtMouseWheelEvent", &mouseWheelEvent);
}

QWasmCompositor::QWasmCompositor(QWasmScreen *screen)
    : QObject(screen),
      m_windowStack(std::bind(&QWasmCompositor::onTopWindowChanged, this)),
      m_eventTranslator(std::make_unique<QWasmEventTranslator>())
{
    m_touchDevice = std::make_unique<QPointingDevice>(
            "touchscreen", 1, QInputDevice::DeviceType::TouchScreen,
            QPointingDevice::PointerType::Finger,
            QPointingDevice::Capability::Position | QPointingDevice::Capability::Area
                | QPointingDevice::Capability::NormalizedPosition,
            10, 0);

    QWindowSystemInterface::registerInputDevice(m_touchDevice.get());
    QWindowSystemInterface::setSynchronousWindowSystemEvents(true);
}

QWasmCompositor::~QWasmCompositor()
{
    m_windowUnderMouse.clear();

    if (m_requestAnimationFrameId != -1)
        emscripten_cancel_animation_frame(m_requestAnimationFrameId);

    deregisterEventHandlers();
    destroy();
}

void QWasmCompositor::deregisterEventHandlers()
{
    QByteArray screenElementSelector = screen()->eventTargetId().toUtf8();
    emscripten_set_keydown_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_keyup_callback(screenElementSelector.constData(), 0, 0, NULL);

    emscripten_set_focus_callback(screenElementSelector.constData(), 0, 0, NULL);

    emscripten_set_wheel_callback(screenElementSelector.constData(), 0, 0, NULL);

    emscripten_set_touchstart_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_touchend_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_touchmove_callback(screenElementSelector.constData(), 0, 0, NULL);
    emscripten_set_touchcancel_callback(screenElementSelector.constData(), 0, 0, NULL);

    screen()->element().call<void>("removeEventListener", std::string("drop"),
                                   val::module_property("qtDrop"), val(true));
}

void QWasmCompositor::destroy()
{
    // TODO(mikolaj.boc): Investigate if m_isEnabled is needed at all. It seems like a frame should
    // not be generated after this instead.
    m_isEnabled = false; // prevent frame() from creating a new m_context
}

void QWasmCompositor::initEventHandlers()
{
    if (platform() == Platform::MacOS) {
        if (!emscripten::val::global("window")["safari"].isUndefined()) {
            screen()->element().call<void>("addEventListener", val("wheel"),
                                           val::module_property("qtMouseWheelEvent"));
        }
    }

    constexpr EM_BOOL UseCapture = 1;

    const QByteArray screenElementSelector = screen()->eventTargetId().toUtf8();
    emscripten_set_keydown_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                    &keyboard_cb);
    emscripten_set_keyup_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                  &keyboard_cb);

    val screenElement = screen()->element();
    const auto callback = std::function([this](emscripten::val event) {
        if (processPointer(*PointerEvent::fromWeb(event)))
            event.call<void>("preventDefault");
    });

    m_pointerDownCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerdown", callback);
    m_pointerMoveCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointermove", callback);
    m_pointerUpCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerup", callback);
    m_pointerEnterCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerenter", callback);
    m_pointerLeaveCallback =
            std::make_unique<qstdweb::EventCallback>(screenElement, "pointerleave", callback);

    emscripten_set_focus_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                  &focus_cb);

    emscripten_set_wheel_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                  &wheel_cb);

    emscripten_set_touchstart_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                       &touchCallback);
    emscripten_set_touchend_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                     &touchCallback);
    emscripten_set_touchmove_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                      &touchCallback);
    emscripten_set_touchcancel_callback(screenElementSelector.constData(), (void *)this, UseCapture,
                                        &touchCallback);

    screenElement.call<void>("addEventListener", std::string("drop"),
                             val::module_property("qtDrop"), val(true));
    screenElement.set("data-qtdropcontext", // ? unique
                      emscripten::val(quintptr(reinterpret_cast<void *>(screen()))));
}

void QWasmCompositor::addWindow(QWasmWindow *window)
{
    m_windowStack.pushWindow(window);
    m_windowStack.topWindow()->requestActivateWindow();

    updateEnabledState();
}

void QWasmCompositor::removeWindow(QWasmWindow *window)
{
    m_requestUpdateWindows.remove(window);
    m_windowStack.removeWindow(window);
    if (m_lastMouseTargetWindow == window->window())
        m_lastMouseTargetWindow = nullptr;
    if (m_windowStack.topWindow())
        m_windowStack.topWindow()->requestActivateWindow();

    updateEnabledState();
}

void QWasmCompositor::updateEnabledState()
{
    m_isEnabled = std::any_of(m_windowStack.begin(), m_windowStack.end(), [](QWasmWindow *window) {
        return !window->context2d().isUndefined();
    });
}

void QWasmCompositor::raise(QWasmWindow *window)
{
    m_windowStack.raise(window);
}

void QWasmCompositor::lower(QWasmWindow *window)
{
    m_windowStack.lower(window);
}

QWindow *QWasmCompositor::windowAt(QPoint targetPointInScreenCoords, int padding) const
{
    const auto found = std::find_if(
            m_windowStack.begin(), m_windowStack.end(),
            [padding, &targetPointInScreenCoords](const QWasmWindow *window) {
                const QRect geometry = window->windowFrameGeometry().adjusted(-padding, -padding,
                                                                              padding, padding);

                return window->isVisible() && geometry.contains(targetPointInScreenCoords);
            });
    return found != m_windowStack.end() ? (*found)->window() : nullptr;
}

QWindow *QWasmCompositor::keyWindow() const
{
    return m_windowStack.topWindow() ? m_windowStack.topWindow()->window() : nullptr;
}

void QWasmCompositor::requestUpdateAllWindows()
{
    m_requestUpdateAllWindows = true;
    requestUpdate();
}

void QWasmCompositor::requestUpdateWindow(QWasmWindow *window, UpdateRequestDeliveryType updateType)
{
    auto it = m_requestUpdateWindows.find(window);
    if (it == m_requestUpdateWindows.end()) {
        m_requestUpdateWindows.insert(window, updateType);
    } else {
        // Already registered, but upgrade ExposeEventDeliveryType to UpdateRequestDeliveryType.
        // if needed, to make sure QWindow::updateRequest's are matched.
        if (it.value() == ExposeEventDelivery && updateType == UpdateRequestDelivery)
            it.value() = UpdateRequestDelivery;
    }

    requestUpdate();
}

// Requests an update/new frame using RequestAnimationFrame
void QWasmCompositor::requestUpdate()
{
    if (m_requestAnimationFrameId != -1)
        return;

    static auto frame = [](double frameTime, void *context) -> int {
        Q_UNUSED(frameTime);

        QWasmCompositor *compositor = reinterpret_cast<QWasmCompositor *>(context);

        compositor->m_requestAnimationFrameId = -1;
        compositor->deliverUpdateRequests();

        return 0;
    };
    m_requestAnimationFrameId = emscripten_request_animation_frame(frame, this);
}

void QWasmCompositor::deliverUpdateRequests()
{
    // We may get new update requests during the window content update below:
    // prepare for recording the new update set by setting aside the current
    // update set.
    auto requestUpdateWindows = m_requestUpdateWindows;
    m_requestUpdateWindows.clear();
    bool requestUpdateAllWindows = m_requestUpdateAllWindows;
    m_requestUpdateAllWindows = false;

    // Update window content, either all windows or a spesific set of windows. Use the correct
    // update type: QWindow subclasses expect that requested and delivered updateRequests matches
    // exactly.
    m_inDeliverUpdateRequest = true;
    if (requestUpdateAllWindows) {
        for (QWasmWindow *window : m_windowStack) {
            auto it = requestUpdateWindows.find(window);
            UpdateRequestDeliveryType updateType =
                (it == m_requestUpdateWindows.end() ? ExposeEventDelivery : it.value());
            deliverUpdateRequest(window, updateType);
        }
    } else {
        for (auto it = requestUpdateWindows.constBegin(); it != requestUpdateWindows.constEnd(); ++it) {
            auto *window = it.key();
            UpdateRequestDeliveryType updateType = it.value();
            deliverUpdateRequest(window, updateType);
        }
    }
    m_inDeliverUpdateRequest = false;
    frame(requestUpdateAllWindows, requestUpdateWindows.keys());
}

void QWasmCompositor::deliverUpdateRequest(QWasmWindow *window, UpdateRequestDeliveryType updateType)
{
    // update by deliverUpdateRequest and expose event accordingly.
    if (updateType == UpdateRequestDelivery) {
        window->QPlatformWindow::deliverUpdateRequest();
    } else {
        QWindow *qwindow = window->window();
        QWindowSystemInterface::handleExposeEvent(
            qwindow, QRect(QPoint(0, 0), qwindow->geometry().size()));
    }
}

void QWasmCompositor::handleBackingStoreFlush(QWindow *window)
{
    // Request update to flush the updated backing store content, unless we are currently
    // processing an update, in which case the new content will flushed as a part of that update.
    if (!m_inDeliverUpdateRequest)
        requestUpdateWindow(asWasmWindow(window));
}

int dpiScaled(qreal value)
{
    return value * (qreal(qt_defaultDpiX()) / 96.0);
}

void QWasmCompositor::frame(bool all, const QList<QWasmWindow *> &windows)
{
    if (!m_isEnabled || m_windowStack.empty() || !screen())
        return;

    if (all) {
        std::for_each(m_windowStack.rbegin(), m_windowStack.rend(),
                      [](QWasmWindow *window) { window->paint(); });
    } else {
        std::for_each(windows.begin(), windows.end(), [](QWasmWindow *window) { window->paint(); });
    }
}

void QWasmCompositor::onTopWindowChanged()
{
    constexpr int zOrderForElementInFrontOfScreen = 3;
    int z = zOrderForElementInFrontOfScreen;
    std::for_each(m_windowStack.rbegin(), m_windowStack.rend(),
                  [&z](QWasmWindow *window) { window->setZOrder(z++); });

    auto it = m_windowStack.begin();
    if (it == m_windowStack.end()) {
        return;
    }
    (*it)->onActivationChanged(true);
    ++it;
    for (; it != m_windowStack.end(); ++it) {
        (*it)->onActivationChanged(false);
    }
}

QWasmScreen *QWasmCompositor::screen()
{
    return static_cast<QWasmScreen *>(parent());
}

int QWasmCompositor::keyboard_cb(int eventType, const EmscriptenKeyboardEvent *keyEvent, void *userData)
{
    QWasmCompositor *wasmCompositor = reinterpret_cast<QWasmCompositor *>(userData);
    return static_cast<int>(wasmCompositor->processKeyboard(eventType, keyEvent));
}

int QWasmCompositor::focus_cb(int eventType, const EmscriptenFocusEvent *focusEvent, void *userData)
{
    Q_UNUSED(eventType)
    Q_UNUSED(focusEvent)
    Q_UNUSED(userData)

    return 0;
}

int QWasmCompositor::wheel_cb(int eventType, const EmscriptenWheelEvent *wheelEvent, void *userData)
{
    QWasmCompositor *compositor = (QWasmCompositor *) userData;
    return static_cast<int>(compositor->processWheel(eventType, wheelEvent));
}

int QWasmCompositor::touchCallback(int eventType, const EmscriptenTouchEvent *touchEvent, void *userData)
{
    auto compositor = reinterpret_cast<QWasmCompositor*>(userData);
    return static_cast<int>(compositor->processTouch(eventType, touchEvent));
}

bool QWasmCompositor::processPointer(const PointerEvent& event)
{
    if (event.pointerType != PointerType::Mouse)
        return false;

    const auto pointInScreen = screen()->mapFromLocal(event.localPoint);

    QWindow *const targetWindow = ([this, pointInScreen]() -> QWindow * {
        auto *targetWindow = m_mouseCaptureWindow != nullptr
                ? m_mouseCaptureWindow.get()
                : windowAt(pointInScreen, 5);

        return targetWindow ? targetWindow : m_lastMouseTargetWindow.get();
    })();
    if (!targetWindow)
        return false;
    m_lastMouseTargetWindow = targetWindow;

    const QPoint pointInTargetWindowCoords = targetWindow->mapFromGlobal(pointInScreen);
    const bool pointerIsWithinTargetWindowBounds = targetWindow->geometry().contains(pointInScreen);

    if (m_mouseInScreen && m_windowUnderMouse != targetWindow
        && pointerIsWithinTargetWindowBounds) {
        // delayed mouse enter
        enterWindow(targetWindow, pointInTargetWindowCoords, pointInScreen);
        m_windowUnderMouse = targetWindow;
    }

    switch (event.type) {
    case EventType::PointerDown:
    {
        screen()->element().call<void>("setPointerCapture", event.pointerId);

        if (targetWindow)
            targetWindow->requestActivate();

        break;
    }
    case EventType::PointerUp:
    {
        screen()->element().call<void>("releasePointerCapture", event.pointerId);

        break;
    }
    case EventType::PointerEnter:
        processMouseEnter(nullptr);
        break;
    case EventType::PointerLeave:
        processMouseLeave();
        break;
    default:
        break;
    };

    if (!pointerIsWithinTargetWindowBounds && event.mouseButtons.testFlag(Qt::NoButton)) {
        leaveWindow(m_lastMouseTargetWindow);
    }

    const bool eventAccepted = deliverEventToTarget(event, targetWindow);
    if (!eventAccepted && event.type == EventType::PointerDown)
        QGuiApplicationPrivate::instance()->closeAllPopups();
    return eventAccepted;
}

bool QWasmCompositor::deliverEventToTarget(const PointerEvent &event, QWindow *eventTarget)
{
    Q_ASSERT(!m_mouseCaptureWindow || m_mouseCaptureWindow.get() == eventTarget);

    const auto pointInScreen = screen()->mapFromLocal(event.localPoint);

    const QPoint targetPointClippedToScreen(
            std::max(screen()->geometry().left(),
                     std::min(screen()->geometry().right(), pointInScreen.x())),
            std::max(screen()->geometry().top(),
                     std::min(screen()->geometry().bottom(), pointInScreen.y())));

    bool deliveringToPreviouslyClickedWindow = false;

    if (!eventTarget) {
        if (event.type != EventType::PointerUp || !m_lastMouseTargetWindow)
            return false;

        eventTarget = m_lastMouseTargetWindow;
        m_lastMouseTargetWindow = nullptr;
        deliveringToPreviouslyClickedWindow = true;
    }

    WindowArea windowArea = WindowArea::Client;
    if (!deliveringToPreviouslyClickedWindow && !m_mouseCaptureWindow
        && !eventTarget->geometry().contains(targetPointClippedToScreen)) {
        if (!eventTarget->frameGeometry().contains(targetPointClippedToScreen))
            return false;
        windowArea = WindowArea::NonClient;
    }

    const QEvent::Type eventType =
        MouseEvent::mouseEventTypeFromEventType(event.type, windowArea);

    return eventType != QEvent::None &&
           QWindowSystemInterface::handleMouseEvent(
               eventTarget, QWasmIntegration::getTimestamp(),
               eventTarget->mapFromGlobal(targetPointClippedToScreen),
               targetPointClippedToScreen, event.mouseButtons, event.mouseButton,
               eventType, event.modifiers);
}

bool QWasmCompositor::processKeyboard(int eventType, const EmscriptenKeyboardEvent *emKeyEvent)
{
    constexpr bool ProceedToNativeEvent = false;
    Q_ASSERT(eventType == EMSCRIPTEN_EVENT_KEYDOWN || eventType == EMSCRIPTEN_EVENT_KEYUP);

    auto translatedEvent = m_eventTranslator->translateKeyEvent(eventType, emKeyEvent);

    const QFlags<Qt::KeyboardModifier> modifiers = KeyboardModifier::getForEvent(*emKeyEvent);

    const auto clipboardResult = QWasmIntegration::get()->getWasmClipboard()->processKeyboard(
            translatedEvent, modifiers);

    using ProcessKeyboardResult = QWasmClipboard::ProcessKeyboardResult;
    if (clipboardResult == ProcessKeyboardResult::NativeClipboardEventNeeded)
        return ProceedToNativeEvent;

    if (translatedEvent.text.isEmpty())
        translatedEvent.text = QString(emKeyEvent->key);
    if (translatedEvent.text.size() > 1)
        translatedEvent.text.clear();
    const auto result =
            QWindowSystemInterface::handleKeyEvent(
                    0, translatedEvent.type, translatedEvent.key, modifiers, translatedEvent.text);
    return clipboardResult == ProcessKeyboardResult::NativeClipboardEventAndCopiedDataNeeded
            ? ProceedToNativeEvent
            : result;
}

bool QWasmCompositor::processWheel(int eventType, const EmscriptenWheelEvent *wheelEvent)
{
    Q_UNUSED(eventType);

    const EmscriptenMouseEvent* mouseEvent = &wheelEvent->mouse;

    int scrollFactor = 0;
    switch (wheelEvent->deltaMode) {
        case DOM_DELTA_PIXEL:
            scrollFactor = 1;
            break;
        case DOM_DELTA_LINE:
            scrollFactor = 12;
            break;
        case DOM_DELTA_PAGE:
            scrollFactor = 20;
            break;
    };

    scrollFactor = -scrollFactor; // Web scroll deltas are inverted from Qt deltas.

    Qt::KeyboardModifiers modifiers = KeyboardModifier::getForEvent(*mouseEvent);
    QPoint targetPointInScreenElementCoords(mouseEvent->targetX, mouseEvent->targetY);
    QPoint targetPointInScreenCoords =
            screen()->geometry().topLeft() + targetPointInScreenElementCoords;

    QWindow *targetWindow = screen()->compositor()->windowAt(targetPointInScreenCoords, 5);
    if (!targetWindow)
        return 0;
    QPoint pointInTargetWindowCoords = targetWindow->mapFromGlobal(targetPointInScreenCoords);

    QPoint pixelDelta;

    if (wheelEvent->deltaY != 0) pixelDelta.setY(wheelEvent->deltaY * scrollFactor);
    if (wheelEvent->deltaX != 0) pixelDelta.setX(wheelEvent->deltaX * scrollFactor);

    QPoint angleDelta = pixelDelta; // FIXME: convert from pixels?

    bool accepted = QWindowSystemInterface::handleWheelEvent(
            targetWindow, QWasmIntegration::getTimestamp(), pointInTargetWindowCoords,
            targetPointInScreenCoords, pixelDelta, angleDelta, modifiers,
            Qt::NoScrollPhase, Qt::MouseEventNotSynthesized,
            g_scrollingInvertedFromDevice);
    return accepted;
}

bool QWasmCompositor::processTouch(int eventType, const EmscriptenTouchEvent *touchEvent)
{
    QList<QWindowSystemInterface::TouchPoint> touchPointList;
    touchPointList.reserve(touchEvent->numTouches);
    QWindow *targetWindow = nullptr;

    for (int i = 0; i < touchEvent->numTouches; i++) {

        const EmscriptenTouchPoint *touches = &touchEvent->touches[i];

        QPoint targetPointInScreenElementCoords(touches->targetX, touches->targetY);
        QPoint targetPointInScreenCoords =
                screen()->geometry().topLeft() + targetPointInScreenElementCoords;

        targetWindow = screen()->compositor()->windowAt(targetPointInScreenCoords, 5);
        if (targetWindow == nullptr)
            continue;

        QWindowSystemInterface::TouchPoint touchPoint;

        touchPoint.area = QRect(0, 0, 8, 8);
        touchPoint.id = touches->identifier;
        touchPoint.pressure = 1.0;

        touchPoint.area.moveCenter(targetPointInScreenCoords);

        const auto tp = m_pressedTouchIds.constFind(touchPoint.id);
        if (tp != m_pressedTouchIds.constEnd())
            touchPoint.normalPosition = tp.value();

        QPointF pointInTargetWindowCoords = QPointF(targetWindow->mapFromGlobal(targetPointInScreenCoords));
        QPointF normalPosition(pointInTargetWindowCoords.x() / targetWindow->width(),
                               pointInTargetWindowCoords.y() / targetWindow->height());

        const bool stationaryTouchPoint = (normalPosition == touchPoint.normalPosition);
        touchPoint.normalPosition = normalPosition;

        switch (eventType) {
            case EMSCRIPTEN_EVENT_TOUCHSTART:
                if (tp != m_pressedTouchIds.constEnd()) {
                    touchPoint.state = (stationaryTouchPoint
                                        ? QEventPoint::State::Stationary
                                        : QEventPoint::State::Updated);
                } else {
                    touchPoint.state = QEventPoint::State::Pressed;
                }
                m_pressedTouchIds.insert(touchPoint.id, touchPoint.normalPosition);

                break;
            case EMSCRIPTEN_EVENT_TOUCHEND:
                touchPoint.state = QEventPoint::State::Released;
                m_pressedTouchIds.remove(touchPoint.id);
                break;
            case EMSCRIPTEN_EVENT_TOUCHMOVE:
                touchPoint.state = (stationaryTouchPoint
                                    ? QEventPoint::State::Stationary
                                    : QEventPoint::State::Updated);

                m_pressedTouchIds.insert(touchPoint.id, touchPoint.normalPosition);
                break;
            default:
                break;
        }

        touchPointList.append(touchPoint);
    }

    QFlags<Qt::KeyboardModifier> keyModifier = KeyboardModifier::getForEvent(*touchEvent);

    bool accepted = false;

    if (eventType == EMSCRIPTEN_EVENT_TOUCHCANCEL)
        accepted = QWindowSystemInterface::handleTouchCancelEvent(targetWindow, QWasmIntegration::getTimestamp(), m_touchDevice.get(), keyModifier);
    else
        accepted = QWindowSystemInterface::handleTouchEvent(
                targetWindow, QWasmIntegration::getTimestamp(), m_touchDevice.get(), touchPointList, keyModifier);

    return static_cast<int>(accepted);
}

void QWasmCompositor::setCapture(QWasmWindow *window)
{
    Q_ASSERT(std::find(m_windowStack.begin(), m_windowStack.end(), window) != m_windowStack.end());
    m_mouseCaptureWindow = window->window();
}

void QWasmCompositor::releaseCapture()
{
    m_mouseCaptureWindow = nullptr;
}

void QWasmCompositor::leaveWindow(QWindow *window)
{
    m_windowUnderMouse = nullptr;
    QWindowSystemInterface::handleLeaveEvent(window);
}

void QWasmCompositor::enterWindow(QWindow *window, const QPoint &pointInTargetWindowCoords, const QPoint &targetPointInScreenCoords)
{
    QWindowSystemInterface::handleEnterEvent(window, pointInTargetWindowCoords, targetPointInScreenCoords);
}

bool QWasmCompositor::processMouseEnter(const EmscriptenMouseEvent *mouseEvent)
{
    Q_UNUSED(mouseEvent)
    // mouse has entered the screen area
    m_mouseInScreen = true;
    return true;
}

bool QWasmCompositor::processMouseLeave()
{
    m_mouseInScreen = false;
    return true;
}
