#include "dbusservice.h"
#include "powerapi.h"
#include "wifiapi.h"
#include "appsapi.h"
#include "systemapi.h"
#include "screenapi.h"
#include "notificationapi.h"

#include <systemd/sd-daemon.h>

using namespace std::chrono;

DBusService* DBusService::singleton(){
    static DBusService* instance;
    if(instance == nullptr){
        qRegisterMetaType<QMap<QString, QDBusObjectPath>>();
        O_DEBUG("Creating DBusService instance");
        instance = new DBusService(qApp);
        auto bus = QDBusConnection::systemBus();
        if(!bus.isConnected()){
#ifdef SENTRY
            sentry_breadcrumb("dbusservice", "Failed to connect to system bus.", "error");
#endif
            qFatal("Failed to connect to system bus.");
        }
        QDBusConnectionInterface* interface = bus.interface();
        O_DEBUG("Registering service...");
        auto reply = interface->registerService(OXIDE_SERVICE);
        bus.registerService(OXIDE_SERVICE);
        if(!reply.isValid()){
            QDBusError ex = reply.error();
#ifdef SENTRY
            sentry_breadcrumb("dbusservice", "Unable to register service", "error");
#endif
            qFatal("Unable to register service: %s", ex.message().toStdString().c_str());
        }
        O_DEBUG("Registering object...");
        if(!bus.registerObject(OXIDE_SERVICE_PATH, instance, QDBusConnection::ExportAllContents)){
#ifdef SENTRY
            sentry_breadcrumb("dbusservice", "Unable to register interface", "error");
#endif
            qFatal("Unable to register interface: %s", bus.lastError().message().toStdString().c_str());
        }
        connect(
            bus.interface(),
            SIGNAL(serviceOwnerChanged(QString,QString,QString)),
            instance,
            SLOT(serviceOwnerChanged(QString,QString,QString))
        );
        O_DEBUG("Registered");
    }
    return instance;
}

DBusService::DBusService(QObject* parent)
: APIBase(parent),
  apis(),
  m_exiting{false}
{
    uint64_t time;
    int res = sd_watchdog_enabled(0, &time);
    if(res > 0){
        auto usec = microseconds(time);
        auto hrs = duration_cast<hours>(usec);
        auto mins = duration_cast<minutes>(usec - hrs);
        auto secs = duration_cast<seconds>(usec - hrs - mins);
        auto ms = duration_cast<milliseconds>(usec - hrs - mins - secs);
        qInfo()
            << "Watchdog notification expected every"
            << QString("%1:%2:%3.%4")
               .arg(hrs.count())
               .arg(mins.count())
               .arg(secs.count())
               .arg(ms.count());
        usec = usec / 2;
        hrs = duration_cast<hours>(usec);
        mins = duration_cast<minutes>(usec - hrs);
        secs = duration_cast<seconds>(usec - hrs - mins);
        ms = duration_cast<milliseconds>(usec - hrs - mins - secs);
        qInfo()
            << "Watchdog scheduled for every "
            << QString("%1:%2:%3.%4")
               .arg(hrs.count())
               .arg(mins.count())
               .arg(secs.count())
               .arg(ms.count());
        m_watchdogTimer = startTimer(duration_cast<milliseconds>(usec).count(), Qt::PreciseTimer);
        if(m_watchdogTimer == 0){
            qCritical() << "Watchdog timer failed to start";
        }else{
            qInfo() << "Watchdog timer running";
        }
    }else if(res < 0){
        qWarning() << "Failed to detect if watchdog timer is expected:" << strerror(-res);
    }else{
        qInfo() << "No watchdog timer required";
    }
#ifdef SENTRY
    sentry_breadcrumb("dbusservice", "Initializing APIs", "info");
#endif
    Oxide::Sentry::sentry_transaction("DBus Service Init", "init", [this](Oxide::Sentry::Transaction* t){
        Oxide::Sentry::sentry_span(t, "apis", "Initialize APIs", [this](Oxide::Sentry::Span* s){
            Oxide::Sentry::sentry_span(s, "wifi", "Initialize wifi API", [this]{
                apis.insert("wifi", APIEntry{
                    .path = QString(OXIDE_SERVICE_PATH) + "/wifi",
                    .dependants = new QStringList(),
                    .instance = new WifiAPI(this),
                });
            });
            Oxide::Sentry::sentry_span(s, "system", "Initialize system API", [this]{
                apis.insert("system", APIEntry{
                    .path = QString(OXIDE_SERVICE_PATH) + "/system",
                    .dependants = new QStringList(),
                    .instance = new SystemAPI(this),
                });
            });
            Oxide::Sentry::sentry_span(s, "power", "Initialize power API", [this]{
                apis.insert("power", APIEntry{
                    .path = QString(OXIDE_SERVICE_PATH) + "/power",
                    .dependants = new QStringList(),
                    .instance = new PowerAPI(this),
                });
            });
            Oxide::Sentry::sentry_span(s, "screen", "Initialize screen API", [this]{
                apis.insert("screen", APIEntry{
                    .path = QString(OXIDE_SERVICE_PATH) + "/screen",
                    .dependants = new QStringList(),
                    .instance = new ScreenAPI(this),
                });
            });
            Oxide::Sentry::sentry_span(s, "apps", "Initialize apps API", [this]{
                apis.insert("apps", APIEntry{
                    .path = QString(OXIDE_SERVICE_PATH) + "/apps",
                    .dependants = new QStringList(),
                    .instance = new AppsAPI(this),
                });
            });
            Oxide::Sentry::sentry_span(s, "notification", "Initialize notification API", [this]{
                apis.insert("notification", APIEntry{
                    .path = QString(OXIDE_SERVICE_PATH) + "/notification",
                    .dependants = new QStringList(),
                    .instance = new NotificationAPI(this),
                });
            });
        });
        Oxide::Sentry::sentry_span(t, "connect", "Connect events", []{
            connect(powerAPI, &PowerAPI::chargerStateChanged, systemAPI, &SystemAPI::activity);
        });
#ifdef SENTRY
        sentry_breadcrumb("dbusservice", "Cleaning up", "info");
#endif
        Oxide::Sentry::sentry_span(t, "cleanup", "Cleanup", [this]{
            auto bus = QDBusConnection::systemBus();
            for(auto api : apis){
                bus.unregisterObject(api.path);
            }
        });
#ifdef SENTRY
        sentry_breadcrumb("dbusservice", "APIs initialized", "info");
#endif
    });
}

DBusService::~DBusService(){}

void DBusService::setEnabled(bool enabled){ Q_UNUSED(enabled); }

QObject* DBusService::getAPI(QString name){
    if(!apis.contains(name)){
        return nullptr;
    }
    return apis[name].instance;
}

QQmlApplicationEngine* DBusService::engine(){ return m_engine; }

int DBusService::tarnishPid(){ return qApp->applicationPid(); }

QDBusObjectPath DBusService::requestAPI(QString name, QDBusMessage message) {
#ifdef SENTRY
    sentry_breadcrumb("dbusservice", ("requestAPI() " + name).toStdString().c_str(), "query");
#endif
    if(!hasPermission(name)){
        return QDBusObjectPath("/");
    }
    if(!apis.contains(name)){
        return QDBusObjectPath("/");
    }
    auto api = apis[name];
    auto bus = QDBusConnection::systemBus();
    if(bus.objectRegisteredAt(api.path) == nullptr){
        bus.registerObject(api.path, api.instance, QDBusConnection::ExportAllContents);
    }
    if(!api.dependants->size()){
        O_DEBUG("Registering " << api.path);
        api.instance->setEnabled(true);
        emit apiAvailable(QDBusObjectPath(api.path));
    }
    api.dependants->append(message.service());
    return QDBusObjectPath(api.path);
}

void DBusService::releaseAPI(QString name, QDBusMessage message) {
#ifdef SENTRY
    sentry_breadcrumb("dbusservice", ("releaseAPI() " + name).toStdString().c_str(), "query");
#endif
    if(!apis.contains(name)){
        return;
    }
    auto api = apis[name];
    auto client = message.service();
    api.dependants->removeAll(client);
    if(!api.dependants->size()){
        O_DEBUG("Unregistering " << api.path);
        api.instance->setEnabled(false);
        QDBusConnection::systemBus().unregisterObject(api.path, QDBusConnection::UnregisterNode);
        emit apiUnavailable(QDBusObjectPath(api.path));
    }
}

QVariantMap DBusService::APIs(){
#ifdef SENTRY
    sentry_breadcrumb("dbusservice", "APIs()", "query");
#endif
    QVariantMap result;
    for(auto key : apis.keys()){
        auto api = apis[key];
        if(api.dependants->size()){
            result[key] = QVariant::fromValue(api.path);
        }
    }
    return result;
}

void DBusService::startup(QQmlApplicationEngine* engine){
#ifdef SENTRY
    sentry_breadcrumb("dbusservice", "startup", "navigation");
#endif
    sd_notify(0, "STATUS=startup");
    m_engine = engine;
    notificationAPI->startup();
    appsAPI->startup();
    sd_notify(0, "STATUS=running");
    sd_notify(0, "READY=1");
}

void DBusService::exit(int exitCode){
    if(calledFromDBus()){
        return;
    }
    if(m_exiting){
        O_WARNING("Already shutting down, forcing stop");
        kill(getpid(), SIGKILL);
        return;
    }
    m_exiting = true;
    sd_notify(0, "STATUS=stopping");
    sd_notify(0, "STOPPING=1");
    emit aboutToQuit();
#ifdef SENTRY
    sentry_breadcrumb("dbusservice", "Disconnecting APIs", "info");
#endif
    O_DEBUG("Removing all APIs");
    auto bus = QDBusConnection::systemBus();
    for(auto key : apis.keys()){
        auto api = apis[key];
        api.instance->setEnabled(false);
        bus.unregisterObject(api.path, QDBusConnection::UnregisterNode);
        emit apiUnavailable(QDBusObjectPath(api.path));
    }
    powerAPI->shutdown();
    appsAPI->shutdown();
    wifiAPI->shutdown();
    notificationAPI->shutdown();
    systemAPI->shutdown();
    bus.unregisterService(OXIDE_SERVICE);
#ifdef SENTRY
    sentry_breadcrumb("dbusservice", "APIs disconnected", "info");
#endif
    O_DEBUG("Exiting");
    std::exit(exitCode);
}

void DBusService::timerEvent(QTimerEvent* event){
    if(event->timerId() == m_watchdogTimer){
        O_DEBUG("Watchdog keepalive sent");
        sd_notify(0, "WATCHDOG=1");
    }
}

void DBusService::serviceOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner){
    Q_UNUSED(oldOwner);
    if(newOwner.isEmpty()){
        auto bus = QDBusConnection::systemBus();
        for(auto key : apis.keys()){
            auto api = apis[key];
            api.dependants->removeAll(name);
            if(!api.dependants->size() && bus.objectRegisteredAt(api.path) != nullptr){
                O_DEBUG("Automatically unregistering " << api.path);
                api.instance->setEnabled(false);
                bus.unregisterObject(api.path, QDBusConnection::UnregisterNode);
                apiUnavailable(QDBusObjectPath(api.path));
            }
        }
        systemAPI->uninhibitAll(name);
    }
}
#include "moc_dbusservice.cpp"
