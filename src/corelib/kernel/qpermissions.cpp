// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qpermissions.h"
#include "qpermissions_p.h"
#include "qhashfunctions.h"

#include <QtCore/qshareddata.h>
#include <QtCore/qdebug.h>

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(lcPermissions, "qt.permissions", QtWarningMsg);

/*!
    \page permissions.html
    \title Application Permissions
    \brief Managing application permissions

    Many features of today's devices and operating systems can have
    significant privacy, security, and performance implications if
    misused. It's therefore increasingly common for platforms to
    require explicit consent from the user before accessing these
    features.

    The Qt permission APIs allow the application to check or request
    permission for such features in a cross platform manner.

    \section1 Usage

    A feature that commonly requires user consent is access to the
    microphone of the device. An application for recording voice
    memos would perhaps look something like this initially:

    \code
    void VoiceMemoWidget::onRecordingInitiated()
    {
        m_microphone->startRecording();
    }
    \endcode

    To ensure this application works well on platforms that
    require user consent for microphone access we would extend
    it like this:

    \code
    void VoiceMemoWidget::onRecordingInitiated()
    {
    #if QT_CONFIG(permissions)
        QMicrophonePermission microphonePermission;
        switch (qApp->checkPermission(microphonePermission)) {
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(microphonePermission, this
                        &VoiceMemoWidget::onRecordingInitiated);
            return;
        case Qt::PermissionStatus::Denied:
            m_permissionInstructionsDialog->show();
            return;
        case Qt::PermissionStatus::Granted:
            break; // Proceed
        }
    #endif
        m_microphone->startRecording();
    }
    \endcode

    We first check if we already know the status of the microphone permission.
    If we don't we initiate a permission request to determine the current
    status, which will potentially ask the user for consent. We connect the
    result of the request to the slot we're already in, so that we get another
    chance at evaluating the permission status.

    Once the permission status is known, either because we had been granted or
    denied permission at an earlier time, or after getting the result back from
    the request we just initiated, we redirect the user to a dialog explaining
    why we can not record voice memos at this time (if the permission was denied),
    or proceed to using the microphone (if permission was granted).

    The use of the \c{QT_CONFIG(permissions)} macro ensures that the code
    will work as before on platforms where permissions are not available.

    \section2 Declaring Permissions

    Some platforms require that the permissions you request are declared
    up front at build time.

    \section3 Apple platforms
    \target apple-usage-description

    Each permission you request must be accompanied by a so called
    \e {usage description} string in the application's \c Info.plist
    file, describing why the application needs to access the given
    permission. For example:

    \badcode
        <key>NSMicrophoneUsageDescription</key>
        <string>The microphone is used to record voice memos.</string>
    \endcode

    The relevant usage description keys are described in the documentation
    for each permission type.

    \sa {Information Property List Files}.

    \section3 Android
    \target android-uses-permission

    Each permission you request must be accompanied by a \c uses-permission
    entry in the application's \c AndroidManifest.xml file. For example:

    \badcode
        <manifest ...>
            <uses-permission android:name="android.permission.RECORD_AUDIO"/>
        </manifest>
    \endcode

    The relevant permission names are described in the documentation
    for each permission type.

    \sa {Qt Creator: Editing Manifest Files}.

    \section1 Available Permissions

    The following permissions types are available:

    \annotatedlist permissions

    \section1 Best Practices

    To ensure the best possible user experience for the end user we recommend
    adopting the following best practices for managing application permissions:

    \list

        \li Request the minimal set of permissions needed. For example, if you only
        need access to the microphone, do \e not request camera permission just in case.
        Use the properties of individual permission types to limit the permission scope
        even further, for example QContactsPermission::setReadOnly() to request read
        only access.

        \li Request permissions in response to specific actions by the user. For example,
        defer requesting microphone permission until the user presses the button to record
        audio. Associating the permission request to a specific action gives the user a clearer
        context of why the permission is needed. Do \e not request all needed permission on
        startup.

        \li Present extra context and explanation if needed. Sometimes the action by the user
        is not enough context. Consider presenting an explanation-dialog after the user has
        initiated the action, but before requesting the permission, so the user is aware of
        what's about to happen when the system permission dialog subsequently pops up.

        \li Be transparent and explicit about why permissions are needed. In explanation
        dialogs and usage descriptions, be transparent about why the particular permission
        is needed for your application to provide a specific feature, so users can make
        informed decisions.

        \li Account for denied permissions. The permissions you request may be denied
        for various reasons. You should always account for this situation, by gracefully
        degrading the experience of your application, and presenting clear explanations
        the user about the situation.

        \li Never request permissions from a library. The request of permissions should
        be done as close as possible to the user, where the information needed to make
        good decisions on the points above is available. Libraries can check permissions,
        to ensure they have the prerequisites for doing their work, but if the permission
        is undetermined or denied this should be reflected through the library's API,
        so that the application in turn can request the necessary permissions.

    \endlist
*/


/*!
    \class QPermission
    \inmodule QtCore
    \inheaderfile QPermissions
    \brief An opaque wrapper of a typed permission.

    The QPermission class is an opaque wrapper of a \l{typed permission},
    used when checking or requesting permissions. You do not need to construct
    this type explicitly, as the type is automatically used when checking or
    requesting permissions:

    \code
    qApp->checkPermission(QCameraPermission{});
    \endcode

    When requesting permissions, the given functor will
    be passed an instance of a QPermissions, which can be used
    to check the result of the request:

    \code
    qApp->requestPermission(QCameraPermission{}, [](const QPermission &permission) {
        if (permission.status() == Qt::PermissionStatus:Granted)
            takePhoto();
    });
    \endcode

    To inspect the properties of the original typed permission,
    use the data() function:

    \code
    QLocationPermission locationPermission;
    locationPermission.setAccuracy(QLocationPermission::Precise);
    qApp->requestPermission(locationPermission, this, &LocationWidget::permissionUpdated);
    \endcode

    \code
    void LocationWidget::permissionUpdated(const QPermission &permission)
    {
        if (permission.status() != Qt::PermissionStatus:Granted)
            return;
        auto locationPermission = permission.data<QLocationPermission>();
        if (locationPermission.accuracy() != QLocationPermission::Precise)
            return;
        updatePreciseLocation();
    }
    \endcode

    \target typed permission
    \section2 Typed Permissions

    The following permissions are available:

    \annotatedlist permissions

    \sa {Application Permissions}
*/

/*!
    \fn template <typename Type> QPermission::QPermission(const Type &type)

    Constructs a permission from the given \l{typed permission} \a type.

    You do not need to construct this type explicitly, as the type is automatically
    used when checking or requesting permissions.
*/

/*!
    \fn template <typename Type> Type QPermission::data() const

    Returns the \l{typed permission} of type \c Type.

    The type must match the type that was originally used to request
    the permission. Use type() for dynamically choosing which typed
    permission to request.
*/

/*!
    Returns the status of the permission.
*/
Qt::PermissionStatus QPermission::status() const
{
    return m_status;
}

/*!
    Returns the type of the permission.
*/
QMetaType QPermission::type() const
{
    return m_data.metaType();
}

#define QT_DEFINE_PERMISSION_SPECIAL_FUNCTIONS(ClassName) \
    ClassName::ClassName() : d(new ClassName##Private) {} \
    ClassName::ClassName(const ClassName &other) noexcept = default; \
    ClassName::ClassName(ClassName &&other) noexcept = default; \
    ClassName::~ClassName() noexcept = default; \
    ClassName &ClassName::operator=(const ClassName &other) noexcept = default;

/*!
    \class QCameraPermission
    \brief Access the camera for taking pictures or videos.

    \section1 Requirements

    \include permissions.qdocinc begin-usage-declarations
      \row
        \li Apple
        \li \l{apple-usage-description}{Usage description}
        \li \c NSCameraUsageDescription
      \row
        \li Android
        \li \l{android-uses-permission}{\c{uses-permission}}
        \li \c android.permission.CAMERA
    \include permissions.qdocinc end-usage-declarations

    \include permissions.qdocinc permission-metadata
*/
class QCameraPermissionPrivate : public QSharedData {};
QT_DEFINE_PERMISSION_SPECIAL_FUNCTIONS(QCameraPermission)

/*!
    \class QMicrophonePermission
    \brief Access the microphone for monitoring or recording sound.

    \section1 Requirements

    \include permissions.qdocinc begin-usage-declarations
      \row
        \li Apple
        \li \l{apple-usage-description}{Usage description}
        \li \c NSMicrophoneUsageDescription
      \row
        \li Android
        \li \l{android-uses-permission}{\c{uses-permission}}
        \li \c android.permission.RECORD_AUDIO
    \include permissions.qdocinc end-usage-declarations

    \include permissions.qdocinc permission-metadata
*/
class QMicrophonePermissionPrivate : public QSharedData {};
QT_DEFINE_PERMISSION_SPECIAL_FUNCTIONS(QMicrophonePermission)

/*!
    \class QBluetoothPermission
    \brief Access Bluetooth peripherals.

    \section1 Requirements

    \include permissions.qdocinc begin-usage-declarations
      \row
        \li Apple
        \li \l{apple-usage-description}{Usage description}
        \li \c NSBluetoothAlwaysUsageDescription
      \row
        \li Android
        \li \l{android-uses-permission}{\c{uses-permission}}
        \li \c android.permission.BLUETOOTH
    \include permissions.qdocinc end-usage-declarations

    \include permissions.qdocinc permission-metadata
*/
class QBluetoothPermissionPrivate : public QSharedData {};
QT_DEFINE_PERMISSION_SPECIAL_FUNCTIONS(QBluetoothPermission)

/*!
    \class QLocationPermission
    \brief Access the user's location.

    By default the request is for approximate accuracy,
    and only while the application is in use. Use
    setAccuracy() and/or setAvailability() to override
    the default.

    \section1 Requirements

    \include permissions.qdocinc begin-usage-declarations
      \row
        \li Apple
        \li \l{apple-usage-description}{Usage description}
        \li \c NSLocationWhenInUseUsageDescription, and
            \c NSLocationAlwaysUsageDescription if requesting
            QLocationPermission::Always
      \row
        \li Android
        \li \l{android-uses-permission}{\c{uses-permission}}
        \li \list
                \li \c android.permission.ACCESS_FINE_LOCATION for QLocationPermission::Precise
                \li \c android.permission.ACCESS_COARSE_LOCATION for QLocationPermission::Approximate
                \li \c android.permission.ACCESS_BACKGROUND_LOCATION for QLocationPermission::Always
            \endlist
            \note QLocationPermission::Always \c uses-permission string has
                to be combined with one or both of QLocationPermission::Precise
                and QLocationPermission::Approximate strings.
    \include permissions.qdocinc end-usage-declarations

    \include permissions.qdocinc permission-metadata
*/
class QLocationPermissionPrivate : public QSharedData
{
public:
    using Accuracy = QLocationPermission::Accuracy;
    Accuracy accuracy = Accuracy::Approximate;

    using Availability = QLocationPermission::Availability;
    Availability availability = Availability::WhenInUse;
};

QT_DEFINE_PERMISSION_SPECIAL_FUNCTIONS(QLocationPermission)

/*!
    \enum QLocationPermission::Accuracy

    This enum is used to control the accuracy of the location data.

    \value Approximate An approximate location is requested.
    \value Precise A precise location is requested.
*/

/*!
    \enum QLocationPermission::Availability

    This enum is used to control the availability of the location data.

    \value WhenInUse The location is only available only when the
    application is in use.
    \value Always The location is available at all times, including when
    the application is in the background.
*/

/*!
    Sets the desired \a accuracy of the request.
*/
void QLocationPermission::setAccuracy(Accuracy accuracy)
{
    d.detach();
    d->accuracy = accuracy;
}

/*!
    Returns the accuracy of the request.
*/
QLocationPermission::Accuracy QLocationPermission::accuracy() const
{
    return d->accuracy;
}

/*!
    Sets the desired \a availability of the request.
*/
void QLocationPermission::setAvailability(Availability availability)
{
    d.detach();
    d->availability = availability;
}

/*!
    Returns the availability of the request.
*/
QLocationPermission::Availability QLocationPermission::availability() const
{
    return d->availability;
}

/*!
    \class QContactsPermission
    \brief Access the user's contacts.

    By default the request is for both read and write access.
    Use setReadOnly() to override the default.

    \section1 Requirements

    \include permissions.qdocinc begin-usage-declarations
      \row
        \li Apple
        \li \l{apple-usage-description}{Usage description}
        \li \c NSContactsUsageDescription
      \row
        \li Android
        \li \l{android-uses-permission}{\c{uses-permission}}
        \li \c android.permission.READ_CONTACTS. \c android.permission.WRITE_CONTACTS if
            QContactsPermission::isReadOnly() is set to \c false.
    \include permissions.qdocinc end-usage-declarations

    \include permissions.qdocinc permission-metadata
*/
class QContactsPermissionPrivate : public QSharedData
{
public:
    bool isReadOnly = false;
};

QT_DEFINE_PERMISSION_SPECIAL_FUNCTIONS(QContactsPermission)

/*!
    Sets whether to \a enable read-only access to the contacts.
*/
void QContactsPermission::setReadOnly(bool enable)
{
    d.detach();
    d->isReadOnly = enable;
}

/*!
    Returns whether the request is for read-only access to the contacts.
*/
bool QContactsPermission::isReadOnly() const
{
    return d->isReadOnly;
}

/*!
    \class QCalendarPermission
    \brief Access the user's calendar.

    By default the request is for both read and write access.
    Use setReadOnly() to override the default.

    \section1 Requirements

    \include permissions.qdocinc begin-usage-declarations
      \row
        \li Apple
        \li \l{apple-usage-description}{Usage description}
        \li \c NSCalendarsUsageDescription
      \row
        \li Android
        \li \l{android-uses-permission}{\c{uses-permission}}
        \li \c android.permission.READ_CALENDAR. \c android.permission.WRITE_CALENDAR if
            QContactsPermission::isReadOnly() is set to \c false.
    \include permissions.qdocinc end-usage-declarations

    \include permissions.qdocinc permission-metadata
*/
class QCalendarPermissionPrivate : public QSharedData
{
public:
    bool isReadOnly = false;
};

QT_DEFINE_PERMISSION_SPECIAL_FUNCTIONS(QCalendarPermission)

/*!
    Sets whether to \a enable read-only access to the calendar.
*/
void QCalendarPermission::setReadOnly(bool enable)
{
    d.detach();
    d->isReadOnly = enable;
}

/*!
    Returns whether the request is for read-only access to the calendar.
*/
bool QCalendarPermission::isReadOnly() const
{
    return d->isReadOnly;
}

/*!
 * \internal
*/

QPermissionPlugin::~QPermissionPlugin() = default;

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug debug, const QPermission &permission)
{
    const auto verbosity = debug.verbosity();
    QDebugStateSaver saver(debug);
    debug.nospace().setVerbosity(0);
    if (verbosity >= QDebug::DefaultVerbosity)
        debug << permission.type().name() << "(";
    debug << permission.status();
    if (verbosity >= QDebug::DefaultVerbosity)
        debug << ")";
    return debug;
}
#endif

QT_END_NAMESPACE

#include "moc_qpermissions.cpp"
