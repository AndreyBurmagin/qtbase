// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include "qwasmscreen.h"
#include "qwasmwindow.h"
#include "qwasmeventtranslator.h"
#include "qwasmcompositor.h"
#include "qwasmintegration.h"
#include "qwasmstring.h"
#include "qwasmcssstyle.h"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <qpa/qwindowsysteminterface.h>
#include <QtCore/qcoreapplication.h>
#include <QtGui/qguiapplication.h>
#include <private/qhighdpiscaling_p.h>

#include <tuple>

QT_BEGIN_NAMESPACE

using namespace emscripten;

const char *QWasmScreen::m_canvasResizeObserverCallbackContextPropertyName =
        "data-qtCanvasResizeObserverCallbackContext";

QWasmScreen::QWasmScreen(const emscripten::val &containerOrCanvas)
    : m_container(containerOrCanvas),
      m_shadowContainer(emscripten::val::undefined()),
      m_compositor(new QWasmCompositor(this)),
      m_eventTranslator(new QWasmEventTranslator())
{
    auto document = m_container["ownerDocument"];
    // Each screen is represented by a div container. All of the windows exist therein as
    // its children. Qt versions < 6.5 used to represent screens as canvas. Support that by
    // transforming the canvas into a div.
    if (m_container["tagName"].call<std::string>("toLowerCase") == "canvas") {
        qWarning() << "Support for canvas elements as an element backing screen is deprecated. The "
                      "canvas provided for the screen will be transformed into a div.";
        auto container = document.call<emscripten::val>("createElement", emscripten::val("div"));
        m_container["parentNode"].call<void>("replaceChild", container, m_container);
        m_container = container;
    }
    auto shadowOptions = emscripten::val::object();
    shadowOptions.set("mode", "open");
    auto shadow = m_container.call<emscripten::val>("attachShadow", shadowOptions);

    m_shadowContainer = document.call<emscripten::val>("createElement", emscripten::val("div"));

    shadow.call<void>("appendChild", QWasmCSSStyle::createStyleElement(m_shadowContainer));

    shadow.call<void>("appendChild", m_shadowContainer);

    m_shadowContainer.set("id", std::string("qt-screen-") + std::to_string(uintptr_t(this)));

    m_shadowContainer["classList"].call<void>("add", std::string("qt-screen"));

    // Set contenteditable so that the canvas gets clipboard events,
    // then hide the resulting focus frame, and reset the cursor.
    m_shadowContainer.set("contentEditable", std::string("true"));
    // set inputmode to none to stop mobile keyboard opening
    // when user clicks  anywhere on the canvas.
    m_shadowContainer.set("inputmode", std::string("none"));

    // Hide the canvas from screen readers.
    m_shadowContainer.call<void>("setAttribute", std::string("aria-hidden"), std::string("true"));

    // Disable the default context menu; Qt applications typically
    // provide custom right-click behavior.
    m_onContextMenu = std::make_unique<qstdweb::EventCallback>(
            m_shadowContainer, "contextmenu",
            [](emscripten::val event) { event.call<void>("preventDefault"); });

    // Create "specialHTMLTargets" mapping for the canvas - the element  might be unreachable based
    // on its id only under some conditions, like the target being embedded in a shadow DOM or a
    // subframe.
    emscripten::val::module_property("specialHTMLTargets")
            .set(eventTargetId().toStdString(), m_shadowContainer);

    emscripten::val::module_property("specialHTMLTargets")
            .set(outerScreenId().toStdString(), m_container);

    // Install event handlers on the container/canvas. This must be
    // done after the canvas has been created above.
    m_compositor->initEventHandlers();

    updateQScreenAndCanvasRenderSize();
    m_shadowContainer.call<void>("focus");
}

QWasmScreen::~QWasmScreen()
{
    Q_ASSERT(!m_compositor); // deleteScreen should have been called to remove this screen

    emscripten::val::module_property("specialHTMLTargets")
            .set(eventTargetId().toStdString(), emscripten::val::undefined());

    m_shadowContainer.set(m_canvasResizeObserverCallbackContextPropertyName,
                          emscripten::val(intptr_t(0)));
}

void QWasmScreen::deleteScreen()
{
    // Delete the compositor before removing the screen, since its destruction routine needs to use
    // the fully operational screen.
    m_compositor.reset();
    // Deletes |this|!
    QWindowSystemInterface::handleScreenRemoved(this);
}

QWasmScreen *QWasmScreen::get(QPlatformScreen *screen)
{
    return static_cast<QWasmScreen *>(screen);
}

QWasmScreen *QWasmScreen::get(QScreen *screen)
{
    if (!screen)
        return nullptr;
    return get(screen->handle());
}

QWasmCompositor *QWasmScreen::compositor()
{
    return m_compositor.get();
}

QWasmEventTranslator *QWasmScreen::eventTranslator()
{
    return m_eventTranslator.get();
}

emscripten::val QWasmScreen::element() const
{
    return m_shadowContainer;
}

QString QWasmScreen::eventTargetId() const
{
    // Return a globally unique id for the canvas. We can choose any string,
    // as long as it starts with a "!".
    return QString("!qtcanvas_%1").arg(uintptr_t(this));
}

QString QWasmScreen::outerScreenId() const
{
    return QString("!outerscreen_%1").arg(uintptr_t(this));
}

QRect QWasmScreen::geometry() const
{
    return m_geometry;
}

int QWasmScreen::depth() const
{
    return m_depth;
}

QImage::Format QWasmScreen::format() const
{
    return m_format;
}

QDpi QWasmScreen::logicalDpi() const
{
    emscripten::val dpi = emscripten::val::module_property("qtFontDpi");
    if (!dpi.isUndefined()) {
        qreal dpiValue = dpi.as<qreal>();
        return QDpi(dpiValue, dpiValue);
    }
    const qreal defaultDpi = 96;
    return QDpi(defaultDpi, defaultDpi);
}

qreal QWasmScreen::devicePixelRatio() const
{
    // window.devicePixelRatio gives us the scale factor between CSS and device pixels.
    // This property reflects hardware configuration, and also browser zoom on desktop.
    //
    // window.visualViewport.scale gives us the zoom factor on mobile. If the html page is
    // configured with "<meta name="viewport" content="width=device-width">" then this scale
    // factor will be 1. Omitting the viewport configuration typically results on a zoomed-out
    // viewport, with a scale factor <1. User pinch-zoom will change the scale factor; an event
    // handler is installed in the QWasmIntegration constructor. Changing zoom level on desktop
    // does not appear to change visualViewport.scale.
    //
    // The effective devicePixelRatio is the product of these two scale factors, upper-bounded
    // by window.devicePixelRatio in order to avoid e.g. allocating a 10x widget backing store.
    double dpr = emscripten::val::global("window")["devicePixelRatio"].as<double>();
    emscripten::val visualViewport = emscripten::val::global("window")["visualViewport"];
    double scale = visualViewport.isUndefined() ? 1.0 : visualViewport["scale"].as<double>();
    double effectiveDevicePixelRatio = std::min(dpr * scale, dpr);
    return qreal(effectiveDevicePixelRatio);
}

QString QWasmScreen::name() const
{
    return QWasmString::toQString(m_shadowContainer["id"]);
}

QPlatformCursor *QWasmScreen::cursor() const
{
    return const_cast<QWasmCursor *>(&m_cursor);
}

void QWasmScreen::resizeMaximizedWindows()
{
    if (!screen())
        return;
    QPlatformScreen::resizeMaximizedWindows();
}

QWindow *QWasmScreen::topWindow() const
{
    return m_compositor->keyWindow();
}

QWindow *QWasmScreen::topLevelAt(const QPoint &p) const
{
    return m_compositor->windowAt(p);
}

QPoint QWasmScreen::mapFromLocal(const QPoint &p) const
{
    return geometry().topLeft() + p;
}

QPoint QWasmScreen::clipPoint(const QPoint &p) const
{
    return QPoint(qBound(screen()->geometry().left(), p.x(), screen()->geometry().right()),
                  qBound(screen()->geometry().top(), p.y(), screen()->geometry().bottom()));
}

void QWasmScreen::invalidateSize()
{
    m_geometry = QRect();
}

void QWasmScreen::setGeometry(const QRect &rect)
{
    m_geometry = rect;
    QWindowSystemInterface::handleScreenGeometryChange(QPlatformScreen::screen(), geometry(),
                                                       availableGeometry());
    resizeMaximizedWindows();
}

void QWasmScreen::updateQScreenAndCanvasRenderSize()
{
    // The HTML canvas has two sizes: the CSS size and the canvas render size.
    // The CSS size is determined according to standard CSS rules, while the
    // render size is set using the "width" and "height" attributes. The render
    // size must be set manually and is not auto-updated on CSS size change.
    // Setting the render size to a value larger than the CSS size enables high-dpi
    // rendering.
    double css_width;
    double css_height;
    emscripten_get_element_css_size(outerScreenId().toUtf8().constData(), &css_width, &css_height);
    QSizeF cssSize(css_width, css_height);

    QSizeF canvasSize = cssSize * devicePixelRatio();

    m_shadowContainer.set("width", canvasSize.width());
    m_shadowContainer.set("height", canvasSize.height());

    // Returns the html elements document/body position
    auto getElementBodyPosition = [](const emscripten::val &element) -> QPoint {
        emscripten::val bodyRect =
                element["ownerDocument"]["body"].call<emscripten::val>("getBoundingClientRect");
        emscripten::val canvasRect = element.call<emscripten::val>("getBoundingClientRect");
        return QPoint(canvasRect["left"].as<int>() - bodyRect["left"].as<int>(),
                      canvasRect["top"].as<int>() - bodyRect["top"].as<int>());
    };

    setGeometry(QRect(getElementBodyPosition(m_shadowContainer), cssSize.toSize()));
    m_compositor->requestUpdateAllWindows();
}

void QWasmScreen::canvasResizeObserverCallback(emscripten::val entries, emscripten::val)
{
    int count = entries["length"].as<int>();
    if (count == 0)
        return;
    emscripten::val entry = entries[0];
    QWasmScreen *screen = reinterpret_cast<QWasmScreen *>(
            entry["target"][m_canvasResizeObserverCallbackContextPropertyName].as<intptr_t>());
    if (!screen) {
        qWarning() << "QWasmScreen::canvasResizeObserverCallback: missing screen pointer";
        return;
    }

    // We could access contentBoxSize|contentRect|devicePixelContentBoxSize on the entry here, but
    // these are not universally supported across all browsers. Get the sizes from the canvas
    // instead.
    screen->updateQScreenAndCanvasRenderSize();
}

EMSCRIPTEN_BINDINGS(qtCanvasResizeObserverCallback)
{
    emscripten::function("qtCanvasResizeObserverCallback",
                         &QWasmScreen::canvasResizeObserverCallback);
}

void QWasmScreen::installCanvasResizeObserver()
{
    emscripten::val ResizeObserver = emscripten::val::global("ResizeObserver");
    if (ResizeObserver == emscripten::val::undefined())
        return; // ResizeObserver API is not available
    emscripten::val resizeObserver =
            ResizeObserver.new_(emscripten::val::module_property("qtCanvasResizeObserverCallback"));
    if (resizeObserver == emscripten::val::undefined())
        return; // Something went horribly wrong

    // We need to get back to this instance from the (static) resize callback;
    // set a "data-" property on the canvas element.
    m_shadowContainer.set(m_canvasResizeObserverCallbackContextPropertyName,
                          emscripten::val(intptr_t(this)));

    resizeObserver.call<void>("observe", m_shadowContainer);
}

QT_END_NAMESPACE
