#include "startstream.h"
#include "backend/computermanager.h"
#include "backend/computerseeker.h"
#include "streaming/session.h"
#include "backend/nvhttp.h"

#include <QCoreApplication>
#include <QPointer>
#include <QReadLocker>
#include <QThread>
#include <QTimer>

#define COMPUTER_SEEK_TIMEOUT 30000
#define APP_SEEK_TIMEOUT 10000

namespace CliStartStream
{

enum State {
    StateInit,
    StateSeekComputer,
    StateSeekApp,
    StateStartSession,
    StateFailure,
};

class Event
{
public:
    enum Type {
        AppQuitCompleted,
        AppQuitRequested,
        ComputerFound,
        ComputerUpdated,
        Executed,
        Timedout,
    };

    Event(Type type)
        : type(type), computerManager(nullptr), computer(nullptr) {}

    Type type;
    ComputerManager *computerManager;
    NvComputer *computer;
    QString errorMessage;
};

class LauncherPrivate
{
    Q_DECLARE_PUBLIC(Launcher)

public:
    LauncherPrivate(Launcher *q) : q_ptr(q) {}

    void handleEvent(Event event)
    {
        Q_Q(Launcher);
        Session* session;
        NvApp app;

        switch (event.type) {
        // Occurs when CliStartStreamSegue becomes visible and the UI calls launcher's execute()
        case Event::Executed:
            if (m_State == StateInit && m_CompanionScreen > 0) {
                m_ComputerManager = event.computerManager;
                startCompanion();
            }
            else if (m_State == StateInit) {
                m_State = StateSeekComputer;
                m_ComputerManager = event.computerManager;

                m_ComputerSeeker = new ComputerSeeker(m_ComputerManager, m_ComputerName, q);
                q->connect(m_ComputerSeeker, &ComputerSeeker::computerFound,
                           q, &Launcher::onComputerFound);
                q->connect(m_ComputerSeeker, &ComputerSeeker::errorTimeout,
                           q, &Launcher::onTimeout);
                m_ComputerSeeker->start(COMPUTER_SEEK_TIMEOUT);

                q->connect(m_ComputerManager, &ComputerManager::computerStateChanged,
                           q, &Launcher::onComputerUpdated);
                q->connect(m_ComputerManager, &ComputerManager::quitAppCompleted,
                           q, &Launcher::onQuitAppCompleted);

                emit q->searchingComputer();
            }
            break;
        // Occurs when searched computer is found
        case Event::ComputerFound:
            if (m_State == StateSeekComputer) {
                if (event.computer->pairState == NvComputer::PS_PAIRED) {
                    m_State = StateSeekApp;
                    m_Computer = event.computer;
                    m_TimeoutTimer->start(APP_SEEK_TIMEOUT);
                    emit q->searchingApp();
                } else {
                    m_State = StateFailure;
                    QString msg = QObject::tr("Computer %1 has not been paired. "
                                              "Please open Moonlight to pair before streaming.")
                            .arg(event.computer->name);
                    emit q->failed(msg);
                }
            }
            break;
        // Occurs when a computer is updated
        case Event::ComputerUpdated:
            if (m_State == StateSeekApp) {
                int index = getAppIndex();
                if (-1 != index) {
                    app = m_Computer->appList[index];
                    m_TimeoutTimer->stop();
                    if (isNotStreaming() || isStreamingApp(app)) {
                        m_State = StateStartSession;
                        session = new Session(m_Computer, app, m_Preferences);
                        emit q->sessionCreated(app.name, session);
                    } else {
                        emit q->appQuitRequired(getCurrentAppName());
                    }
                }
            }
            break;
        // Occurs when there was another app running on computer and user accepted quit
        // confirmation dialog
        case Event::AppQuitRequested:
            if (m_State == StateSeekApp) {
                m_ComputerManager->quitRunningApp(m_Computer);
            }
            break;
        // Occurs when the previous app quit has been completed, handles quitting errors if any
        // happened. ComputerUpdated event's handler handles session start when previous app has
        // quit.
        case Event::AppQuitCompleted:
            if (m_State == StateSeekApp && !event.errorMessage.isEmpty()) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Quitting app failed, reason: %1").arg(event.errorMessage));
            }
            break;
        // Occurs when computer or app search timed out
        case Event::Timedout:
            if (m_State == StateSeekComputer) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Failed to connect to %1").arg(m_ComputerName));
            }
            if (m_State == StateSeekApp) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Failed to find application %1").arg(m_AppName));
            }
            break;
        }
    }

    // A companion window for one of the host's extra screens (Apollo "Extra screens"): the extra
    // screen is the same Apollo installation on another port, so it presents the main host's
    // certificate and trusts the devices paired with it. Connect directly, no pairing needed.
    void startCompanion()
    {
        Q_Q(Launcher);
        m_State = StateSeekComputer;
        emit q->searchingComputer();

        QSslCertificate serverCert;
        const auto computers = m_ComputerManager->getComputers();
        for (NvComputer* computer : computers) {
            QReadLocker lock(&computer->lock);
            if (computer->uuid == m_ComputerName) {
                serverCert = computer->serverCert;
                break;
            }
        }
        if (serverCert.isNull()) {
            m_State = StateFailure;
            emit q->failed(QObject::tr("Screen %1: the host is not paired with this PC.").arg(m_CompanionScreen));
            return;
        }

        QString address = m_CompanionAddress;
        uint16_t httpPort = m_CompanionHttpPort;
        uint16_t httpsPort = m_CompanionHttpsPort;
        int screen = m_CompanionScreen;
        QPointer<Launcher> launcher(q);

        // NvHTTP blocks, so talk to the extra screen off the UI thread
        QThread* thread = QThread::create([=]() {
            NvComputer* computer = nullptr;
            QString error;
            try {
                NvHTTP http(NvAddress(address, httpPort), httpsPort, serverCert, true);
                QString serverInfo = http.getServerInfo(NvHTTP::NVLL_ERROR);
                computer = new NvComputer(http, serverInfo);
                if (computer->pairState != NvComputer::PS_PAIRED) {
                    error = QObject::tr("Screen %1 doesn't trust this PC yet. Is \"Extra screens\" set in the host's Apollo, "
                                        "and was this PC paired with the main screen?").arg(screen);
                }
                else {
                    computer->appList = http.getAppList();
                }
            } catch (const std::exception& e) {
                error = QObject::tr("Screen %1: can't reach the host's extra screen on port %2 (%3). "
                                    "Set \"Extra screens\" in the host's Apollo settings.")
                            .arg(screen).arg(httpPort).arg(QString::fromUtf8(e.what()));
            }

            if (launcher) {
                QMetaObject::invokeMethod(launcher.data(), [=]() {
                    if (launcher) {
                        launcher->d_func()->onCompanionReady(computer, error);
                    }
                }, Qt::QueuedConnection);
            }
        });
        q->connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    void onCompanionReady(NvComputer* computer, QString error)
    {
        Q_Q(Launcher);
        if (!error.isEmpty() || computer == nullptr) {
            m_State = StateFailure;
            emit q->failed(error);
            return;
        }

        m_Computer = computer;
        int index = getAppIndex();
        if (index < 0) {
            m_State = StateFailure;
            emit q->failed(QObject::tr("Screen %1: app %2 not found on the host.").arg(m_CompanionScreen).arg(m_AppName));
            return;
        }

        m_State = StateStartSession;
        NvApp app = m_Computer->appList[index];
        Session* session = new Session(m_Computer, app, m_Preferences);
        emit q->sessionCreated(app.name, session);
    }

    int getAppIndex() const
    {
        for (int i = 0; i < m_Computer->appList.length(); i++) {
            if (m_Computer->appList[i].name.toLower() == m_AppName.toLower()) {
                return i;
            }
        }
        return -1;
    }

    bool isNotStreaming() const
    {
        return m_Computer->currentGameId == 0;
    }

    bool isStreamingApp(NvApp app) const
    {
        return m_Computer->currentGameId == app.id;
    }

    QString getCurrentAppName() const
    {
        for (const NvApp& app : std::as_const(m_Computer->appList)) {
            if (m_Computer->currentGameId == app.id) {
                return app.name;
            }
        }
        return "<UNKNOWN>";
    }

    Launcher *q_ptr;
    QString m_ComputerName;
    QString m_AppName;
    int m_CompanionScreen = 0;
    QString m_CompanionAddress;
    uint16_t m_CompanionHttpPort = 0;
    uint16_t m_CompanionHttpsPort = 0;
    StreamingPreferences *m_Preferences;
    ComputerManager *m_ComputerManager;
    ComputerSeeker *m_ComputerSeeker;
    NvComputer *m_Computer;
    State m_State;
    QTimer *m_TimeoutTimer;
};

Launcher::Launcher(QString computer, QString app,
                   StreamingPreferences* preferences, QObject *parent)
    : QObject(parent),
      m_DPtr(new LauncherPrivate(this))
{
    Q_D(Launcher);
    d->m_ComputerName = computer;
    d->m_AppName = app;
    d->m_Preferences = preferences;
    d->m_State = StateInit;
    d->m_TimeoutTimer = new QTimer(this);
    d->m_TimeoutTimer->setSingleShot(true);
    connect(d->m_TimeoutTimer, &QTimer::timeout,
            this, &Launcher::onTimeout);
}

Launcher::~Launcher()
{
}

void Launcher::setCompanion(int screen, QString address, uint16_t httpPort, uint16_t httpsPort)
{
    Q_D(Launcher);
    d->m_CompanionScreen = screen;
    d->m_CompanionAddress = address;
    d->m_CompanionHttpPort = httpPort;
    d->m_CompanionHttpsPort = httpsPort;
}

void Launcher::execute(ComputerManager *manager)
{
    Q_D(Launcher);
    Event event(Event::Executed);
    event.computerManager = manager;
    d->handleEvent(event);
}

void Launcher::quitRunningApp()
{
    Q_D(Launcher);
    Event event(Event::AppQuitRequested);
    d->handleEvent(event);
}

bool Launcher::isExecuted() const
{
    Q_D(const Launcher);
    return d->m_State != StateInit;
}

void Launcher::onComputerFound(NvComputer *computer)
{
    Q_D(Launcher);
    Event event(Event::ComputerFound);
    event.computer = computer;
    d->handleEvent(event);
}

void Launcher::onComputerUpdated(NvComputer *computer)
{
    Q_D(Launcher);
    Event event(Event::ComputerUpdated);
    event.computer = computer;
    d->handleEvent(event);
}

void Launcher::onTimeout()
{
    Q_D(Launcher);
    Event event(Event::Timedout);
    d->handleEvent(event);
}

void Launcher::onQuitAppCompleted(QVariant error)
{
    Q_D(Launcher);
    Event event(Event::AppQuitCompleted);
    event.errorMessage = error.toString();
    d->handleEvent(event);
}

}
