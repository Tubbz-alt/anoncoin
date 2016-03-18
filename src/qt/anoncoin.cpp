// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2013-2015 The Anoncoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "anoncoingui.h"

// Many builder specific things set in the config file, for any source files where we rely on moc_xxx files being generated
// it is best to include the anoncoin-config.h in the header file itself.  This file is unusual in that it has a .moc file
// generated for the cpp src itself, with ENABLE_WALLET dependencies, so we include it again here now, attempting to resolve
// some missing slots for windows builds, although the problem appears to be in anoncoingui...
#if defined(HAVE_CONFIG_H)
#include "config/anoncoin-config.h"
#endif

#include "clientmodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "intro.h"
#include "optionsmodel.h"
#include "splashscreen.h"
#include "utilitydialog.h"
#include "winshutdownmonitor.h"
#ifdef ENABLE_WALLET
#include "paymentserver.h"
#include "walletmodel.h"
#endif

#include "init.h"
#include "main.h"
#include "rpcserver.h"
#include "ui_interface.h"
#include "util.h"

#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

#include "i2pmanager.h"

#include <stdint.h>

#include <boost/filesystem/operations.hpp>
#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTranslator>

// Needed for Stylesheet code
#include <QFile>
#include <QTextStream>
// #include <QDir>


#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if QT_VERSION < 0x050000
Q_IMPORT_PLUGIN(qcncodecs)
Q_IMPORT_PLUGIN(qjpcodecs)
Q_IMPORT_PLUGIN(qtwcodecs)
Q_IMPORT_PLUGIN(qkrcodecs)
Q_IMPORT_PLUGIN(qtaccessiblewidgets)
#else
Q_IMPORT_PLUGIN(AccessibleFactory)
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif
#endif

#if QT_VERSION < 0x050000
#include <QTextCodec>
#endif

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)

static void InitMessage(const std::string &message)
{
    LogPrintf("init message: %s\n", message);
}

/*
   Translate string to current locale using Qt.
 */
static std::string Translate(const char* psz)
{
    return QCoreApplication::translate("anoncoin-core", psz).toStdString();
}

static QString GetLangTerritory()
{
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings = settings.value("language", "").toString();
    if(!lang_territory_qsettings.isEmpty())
        lang_territory = lang_territory_qsettings;
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(GetArg("-lang", lang_territory.toStdString()));
    return lang_territory;
}

/** Set up translations */
static void initTranslations(QTranslator &qtTranslatorBase, QTranslator &qtTranslator, QTranslator &translatorBase, QTranslator &translator)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory();

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslatorBase);

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    // Load e.g. anoncoin_de.qm (shortcut "de" needs to be defined in anoncoin_locale.qrc)
    if (translatorBase.load(lang, ":/translations/"))
        QApplication::installTranslator(&translatorBase);
    // Load e.g. anoncoin_de_DE.qm (shortcut "de_DE" needs to be defined in anoncoin_locale.qrc)
    if (translator.load(lang_territory, ":/translations/"))
        QApplication::installTranslator(&translator);
}

/* qDebug() message handler --> debug.log */
#if QT_VERSION < 0x050000
void DebugMessageHandler(QtMsgType type, const char *msg)
{
    Q_UNUSED(type);
    LogPrint("gui", "GUI: %s\n", msg);
}
#else
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString &msg)
{
    Q_UNUSED(type);
    Q_UNUSED(context);
    LogPrint("gui", "GUI: %s\n", qPrintable(msg));
}
#endif

/** Class encapsulating Anoncoin Core startup and shutdown.
 * Allows running startup and shutdown in a different thread from the UI thread.
 */
class AnoncoinCoin: public QObject
{
    Q_OBJECT
public:
    explicit AnoncoinCoin();

public Q_SLOTS:
    void initialize();
    void shutdown();

Q_SIGNALS:
    void initializeResult(int retval);
    void shutdownResult(int retval);
    void runawayException(const QString &message);

private:
    boost::thread_group threadGroup;

    /// Pass fatal exception message to UI thread
    void handleRunawayException(std::exception *e);
};

/** Main Anoncoin application object */
class AnoncoinApplication: public QApplication
{
    Q_OBJECT
public:
    explicit AnoncoinApplication(int &argc, char **argv);
    ~AnoncoinApplication();

#ifdef ENABLE_WALLET
    /// Create payment server
    void createPaymentServer();
#endif
    /// Create options model
    void createOptionsModel();
    /// Create main window
    void createWindow(bool isaTestNet);
    /// Create splash screen
    void createSplashScreen(bool isaTestNet);

    /// Request core initialization
    void requestInitialize();
    /// Request core shutdown
    void requestShutdown();

    /// Get process return value
    int getReturnValue() { return returnValue; }

    /// Get window identifier of QMainWindow (AnoncoinGUI)
    WId getMainWinId() const;

public Q_SLOTS:
    void initializeResult(int retval);
    void shutdownResult(int retval);
    /// Handle runaway exceptions. Shows a message box with the problem and quits the program.
    void handleRunawayException(const QString &message);

Q_SIGNALS:
    void requestedInitialize();
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget *window);

private:
    QThread *coreThread;
    OptionsModel *optionsModel;
    ClientModel *clientModel;
    AnoncoinGUI *window;
    QTimer *pollShutdownTimer;
#ifdef ENABLE_WALLET
    PaymentServer* paymentServer;
    WalletModel *walletModel;
#endif
    int returnValue;

    void startThread();
};

#include "anoncoin.moc"

AnoncoinCoin::AnoncoinCoin():
    QObject()
{
}

void AnoncoinCoin::handleRunawayException(std::exception *e)
{
    PrintExceptionContinue(e, "Runaway exception");
    Q_EMIT runawayException(QString::fromStdString(strMiscWarning));
}

void AnoncoinCoin::initialize()
{
    try
    {
        LogPrintf("Running AppInit2 in thread\n");
        int rv = AppInit2(threadGroup);
        if(rv)
        {
            /* Start a dummy RPC thread if no RPC thread is active yet
             * to handle timeouts.
             */
            StartDummyRPCThread();
        }
        Q_EMIT initializeResult(rv);
    } catch (std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

void AnoncoinCoin::shutdown()
{
    try
    {
        LogPrintf("Running Shutdown in thread\n");
        threadGroup.interrupt_all();
        threadGroup.join_all();
        Shutdown();
        LogPrintf("Shutdown finished\n");
        Q_EMIT shutdownResult(1);
    } catch (std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

AnoncoinApplication::AnoncoinApplication(int &argc, char **argv):
    QApplication(argc, argv),
    coreThread(0),
    optionsModel(0),
    clientModel(0),
    window(0),
    pollShutdownTimer(0),
#ifdef ENABLE_WALLET
    paymentServer(0),
    walletModel(0),
#endif
    returnValue(0)
{
    setQuitOnLastWindowClosed(false);
}

AnoncoinApplication::~AnoncoinApplication()
{
    if(coreThread)
    {
        LogPrintf("Stopping thread\n");
        Q_EMIT stopThread();
        coreThread->wait();
        LogPrintf("Stopped thread\n");
    }

    delete window;
    window = 0;
#ifdef ENABLE_WALLET
    delete paymentServer;
    paymentServer = 0;
#endif
    delete optionsModel;
    optionsModel = 0;
}

#ifdef ENABLE_WALLET
void AnoncoinApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
}
#endif

void AnoncoinApplication::createOptionsModel()
{
    optionsModel = new OptionsModel();
}

void AnoncoinApplication::createWindow(bool isaTestNet)
{
    window = new AnoncoinGUI(isaTestNet, 0);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, SIGNAL(timeout()), window, SLOT(detectShutdown()));
    pollShutdownTimer->start(200);
}

void AnoncoinApplication::createSplashScreen(bool isaTestNet)
{
    SplashScreen *splash = new SplashScreen(QPixmap(), 0, isaTestNet);
    splash->setAttribute(Qt::WA_DeleteOnClose);
    splash->show();
    connect(this, SIGNAL(splashFinished(QWidget*)), splash, SLOT(slotFinish(QWidget*)));
}

void AnoncoinApplication::startThread()
{
    if(coreThread)
        return;
    coreThread = new QThread(this);
    AnoncoinCoin *executor = new AnoncoinCoin();
    executor->moveToThread(coreThread);

    /*  communication to and from thread */
    connect(executor, SIGNAL(initializeResult(int)), this, SLOT(initializeResult(int)));
    connect(executor, SIGNAL(shutdownResult(int)), this, SLOT(shutdownResult(int)));
    connect(executor, SIGNAL(runawayException(QString)), this, SLOT(handleRunawayException(QString)));
    connect(this, SIGNAL(requestedInitialize()), executor, SLOT(initialize()));
    connect(this, SIGNAL(requestedShutdown()), executor, SLOT(shutdown()));
    /*  make sure executor object is deleted in its own thread */
    connect(this, SIGNAL(stopThread()), executor, SLOT(deleteLater()));
    connect(this, SIGNAL(stopThread()), coreThread, SLOT(quit()));

    coreThread->start();
}

void AnoncoinApplication::requestInitialize()
{
    LogPrintf("Requesting initialize\n");
    startThread();
    Q_EMIT requestedInitialize();
}

void AnoncoinApplication::requestShutdown()
{
    LogPrintf("Requesting shutdown\n");
    startThread();
    window->hide();
    window->setClientModel(0);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    window->removeAllWallets();
    delete walletModel;
    walletModel = 0;
#endif
    delete clientModel;
    clientModel = 0;

    // Show a simple window indicating shutdown status
    ShutdownWindow::showShutdownWindow(window);

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void AnoncoinApplication::initializeResult(int retval)
{
    LogPrintf("Initialization result: %i\n", retval);

    // Regardless of the initialization result, we can now load the i2p
    // destination address information into the ShowI2PAddresses class
    // now, and any parameter interaction will have finished.
    window->UpdateI2PAddressDetails();

    // Set exit result: 0 if successful, 1 if failure
    returnValue = retval ? 0 : 1;
    if(retval)
    {
#ifdef ENABLE_WALLET
        PaymentServer::LoadRootCAs();
        paymentServer->setOptionsModel(optionsModel);
#endif

        clientModel = new ClientModel(optionsModel);
        window->setClientModel(clientModel);

#ifdef ENABLE_WALLET
        if(pwalletMain)
        {
            walletModel = new WalletModel(pwalletMain, optionsModel);

            window->addWallet("~Default", walletModel);
            window->setCurrentWallet("~Default");

            connect(walletModel, SIGNAL(coinsSent(CWallet*,SendCoinsRecipient,QByteArray)),
                             paymentServer, SLOT(fetchPaymentACK(CWallet*,const SendCoinsRecipient&,QByteArray)));
        }
#endif

        // If -min option passed, start window minimized.
        if(GetBoolArg("-min", false))
        {
            window->showMinimized();
        }
        else
        {
            window->show();
        }
        Q_EMIT splashFinished(window);

#ifdef ENABLE_WALLET
        // Now that initialization/startup is done, process any command-line
        // anoncoin: URIs or payment requests:
        connect(paymentServer, SIGNAL(receivedPaymentRequest(SendCoinsRecipient)),
                         window, SLOT(handlePaymentRequest(SendCoinsRecipient)));
        connect(window, SIGNAL(receivedURI(QString)),
                         paymentServer, SLOT(handleURIOrFile(QString)));
        connect(paymentServer, SIGNAL(message(QString,QString,unsigned int)),
                         window, SLOT(message(QString,QString,unsigned int)));
        QTimer::singleShot(100, paymentServer, SLOT(uiReady()));
#endif

        // If the commandline included -generatei2pdestination, and the user selected 'APPLY' then that is the reason we
        // are here, and can now show the generated destination details.
        if( GetBoolArg( "-generatei2pdestination", false ) )
            window->ShowI2pDestination();
    } else {
        quit(); // Exit main loop
    }
}

void AnoncoinApplication::shutdownResult(int retval)
{
    LogPrintf("Shutdown result: %i\n", retval);
    quit(); // Exit main loop after shutdown finished
}

void AnoncoinApplication::handleRunawayException(const QString &message)
{
    QMessageBox::critical(0, "Runaway exception", AnoncoinGUI::tr("A fatal error occurred. Anoncoin can no longer continue safely and will quit.") + QString("\n\n") + message);
    ::exit(1);
}

WId AnoncoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

#ifndef ANONCOIN_QT_TEST
int main(int argc, char *argv[])
{
    SetupEnvironment();

    /// 1. Parse command-line options. These take precedence over anything else.
    // Command-line options take precedence:
    ParseParameters(argc, argv);

    // Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

    /// 2. Basic Qt initialization (not dependent on parameters or configuration)
#if QT_VERSION < 0x050000
    // Internal string conversion is all UTF-8
    QTextCodec::setCodecForTr(QTextCodec::codecForName("UTF-8"));
    QTextCodec::setCodecForCStrings(QTextCodec::codecForTr());
#endif

    Q_INIT_RESOURCE(anoncoin);
    Q_INIT_RESOURCE(anoncoin_locale);

    AnoncoinApplication app(argc, argv);
#if QT_VERSION > 0x050100
    // Generate high-dpi pixmaps
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
#ifdef Q_OS_MAC
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
#endif

    // Register meta types used for QMetaObject::invokeMethod
    qRegisterMetaType< bool* >();
    //   Need to pass name here as CAmount is a typedef (see http://qt-project.org/doc/qt-5/qmetatype.html#qRegisterMetaType)
    //   IMPORTANT if it is no longer a typedef use the normal variant above
    qRegisterMetaType< CAmount >("CAmount");

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);
    GUIUtil::SubstituteFonts(GetLangTerritory());

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);
    uiInterface.Translate.connect(Translate);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (mapArgs.count("-?") || mapArgs.count("--help"))
    {
        HelpMessageDialog help(NULL);
        help.showOrPrint();
        return 1;
    }

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    Intro::pickDataDirectory();

    /// 6. Determine availability of data directory and parse anoncoin.conf
    /// - Do not call GetDataDir(true) before this step finishes
    if (!boost::filesystem::is_directory(GetDataDir(false)))
    {
        QMessageBox::critical(0, QObject::tr("Anoncoin"),
                              QObject::tr("Error: Specified data directory \"%1\" does not exist.").arg(QString::fromStdString(mapArgs["-datadir"])));
        return 1;
    }

    // The Custom Anoncoin startup wizard....
    if (!boost::filesystem::exists(GetConfigFile().string()))
    {
        // ToDo: Not linked in yet
        // Run wizard
        // runFirstRunWizard();
    }

    // Read config after it's potentional written by the wizard.
    try {
        ReadConfigFile(mapArgs, mapMultiArgs);
    } catch(std::exception &e) {
        QMessageBox::critical(0, QObject::tr("Anoncoin"),
                              QObject::tr("Error: Cannot parse configuration file: %1. Only use key=value syntax.").arg(e.what()));
        return false;
    }
#define SEPARATED_I2P_SETTINGS_FILE
#if defined(SEPARATED_I2P_SETTINGS_FILE)
    //   Check if it exists.
    // x    load settings from file into memory.
    // x    Updating general settings to reflect file
    //      Update QT to reflect settings from file
    // If it does not exist
    // x   Create file
    // x   Initialize with default values
    // x   Flush to disk
    try {
        I2PManager = new I2PManager();
        if (boost::filesystem::exists( I2PManager->GetI2PSettingsFilePath() ))
        {
            if (I2PManager->ReadI2PSettingsFile())
            {
                I2PManager->LogDataFile();
            }
        }
        else
        {
            I2PManager->FileI2P.initDefaultValues();
            I2PManager->WriteToI2PSettingsFile();
        }

        // Update mapArgs with data file values
        I2PManager->UpdateMapArguments();
    } catch(std::exception &e) {
        QMessageBox::critical(0, QObject::tr("Anoncoin"),
                              QObject::tr("Error: Cannot set up I2P Data File file: %1. ").arg(e.what()));
        return false;
    }
#endif

    /// 7. Determine network (and switch to network specific options)
    // - Do not call Params() before this step
    // - Do this after parsing the configuration file, as the network can be switched there
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel



    // With this you can load a Qt Stylesheet file into the GUI to colorize or customize different objects.
    // By Meeh & GroundRod edits to integrate into v0.9 code as default behavior to load <pathtodatadir>anoncoin.qss
    // ToDo: Work in progress...
    QString filename = GUIUtil::boostPathToQString( GetQtStyleFile());
    QFile file(filename);
    if( file.exists() ) {        // Check if the file exists
        if ( file.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
            QTextStream in(&file);
            QString content = "";
            while (!in.atEnd())
                content.append(in.readLine());
            app.setStyleSheet(content);                 // Load the Stylesheet, not sure what conditions cause an error here, or how it's reported.
        } else                                          // ...Unlikely the file couldn't be opened, this should never happen
            QMessageBox::warning( NULL, AnoncoinGUI::tr("Failed to open stylesheet file."),
                                  filename.toLocal8Bit().data(),
                                  QMessageBox::Ok, QMessageBox::Ok );
    } else if( mapArgs.count("-style") )                // ...unless the user specifically requested a given file, and it doesn't exist.  That would be odd.
        QMessageBox::warning( NULL, AnoncoinGUI::tr("Failed to find stylesheet file."),
                              filename.toLocal8Bit().data(),
                              QMessageBox::Ok, QMessageBox::Ok );
    // else don't worry about stylesheets, proceed without error...

    app.processEvents();
    app.setQuitOnLastWindowClosed(false);

    // Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
    if (!SelectParamsFromCommandLine()) {
        QMessageBox::critical(0, QObject::tr("Anoncoin"), QObject::tr("Error: Invalid combination of -regtest and -testnet."));
        return 1;
    }
#ifdef ENABLE_WALLET
    // Parse URIs on command line -- this can affect Params()
    if (!PaymentServer::ipcParseCommandLine(argc, argv))
        exit(0);
#endif
    bool isaTestNet = !isMainNetwork();
    // Allow for separate UI settings for testnets
    if (isaTestNet)
        QApplication::setApplicationName(QAPP_APP_NAME_TESTNET);
    else
        QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);

#ifdef ENABLE_WALLET
    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        exit(0);

    // Start up the payment server early, too, so impatient users that click on
    // anoncoin: links repeatedly have their payment requests routed to this process:
    app.createPaymentServer();
#endif

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));
#if QT_VERSION < 0x050000
    // Install qDebug() message handler to route to debug.log
    qInstallMsgHandler(DebugMessageHandler);
#else
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    qApp->installNativeEventFilter(new WinShutdownMonitor());
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
#endif
    // Load GUI settings from QSettings
    app.createOptionsModel();

    // Subscribe to global signals from core
    uiInterface.InitMessage.connect(InitMessage);

    if (GetBoolArg("-splash", true) && !GetBoolArg("-min", false))
        app.createSplashScreen(isaTestNet);

    try
    {
        app.createWindow(isaTestNet);
        app.requestInitialize();
#if defined(Q_OS_WIN) && QT_VERSION >= 0x050000
        WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("Anoncoin Core didn't yet exit safely..."), (HWND)app.getMainWinId());
#endif
        app.exec();
        app.requestShutdown();
        app.exec();
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(strMiscWarning));
    } catch (...) {
        PrintExceptionContinue(NULL, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(strMiscWarning));
    }
    return app.getReturnValue();
}
#endif // ANONCOIN_QT_TEST