#ifndef MAINAPP_H
#define MAINAPP_H

#include "Main_Actions.h"
#include "Main_Msg.h"
#include "Main_WinMenu.h"

#include <QApplication>
#include <QMutex>

class ConsoleWindow;
class Par2Window;
class ConfigCtl;
class AOCtl;
class CmdSrvDlg;
class RgtSrvDlg;
class Run;
class FileViewerWindow;
class FramePool;
class AIQ;

class QSharedMemory;
class QDialog;
class QMessageBox;
class QAbstractButton;
class QSettings;

/* ---------------------------------------------------------------- */
/* App persistent data -------------------------------------------- */
/* ---------------------------------------------------------------- */

struct AppData {
    QString runDir,
            lastViewedFile;
    bool    debug,
            saveChksShowing,
            editLog;
};

/* ---------------------------------------------------------------- */
/* MainApp -------------------------------------------------------- */
/* ---------------------------------------------------------------- */

// Global object/data repository:
// - Create app's main objects
// - Store them
// - Pass messages among them
// - Clean up
//
class MainApp : public QApplication
{
    Q_OBJECT

    friend int main( int, char** );
    friend class Main_Actions;

private:
    QSharedMemory   *singleton;
    ConsoleWindow   *consoleWindow;
    Par2Window      *par2Win;
    QDialog         *helpWindow;
    ConfigCtl       *configCtl;
    AOCtl           *aoCtl;
    Run             *run;
    CmdSrvDlg       *cmdSrv;
    RgtSrvDlg       *rgtSrv;
    QMessageBox     *runInitingDlg;
    mutable QMutex  remoteMtx;
    AppData         appData;
    bool            initialized,
                    runIsWaitingForPool,
                    dum[2];

public:
    Main_Actions    act;
    Main_Msg        msg;
    Main_WinMenu    win;
    FramePool       *pool;

// ------------
// Construction
// ------------

private:
    MainApp( int &argc, char **argv );  // < constructed by main()
public:
    virtual ~MainApp();

// ------------------
// Main object access
// ------------------

public:
    static MainApp *instance()
        {return dynamic_cast<MainApp*>(qApp);}

    ConsoleWindow *console() const
        {return const_cast<ConsoleWindow*>(consoleWindow);}

    ConfigCtl *cfgCtl() const
        {return configCtl;}

    AOCtl *getAOCtl() const
        {return aoCtl;}

    Run *getRun() const
        {return run;}

// ----------
// Properties
// ----------

public:
    bool isInitialized() const
        {QMutexLocker ml(&remoteMtx); return initialized;}
    bool isDebugMode() const            {return appData.debug;}
    bool isConsoleHidden() const;
    bool isShiftPressed();
    bool isReadyToRun();
    bool areSaveChksShowing() const     {return appData.saveChksShowing;}
    bool isLogEditable() const          {return appData.editLog;}

    bool remoteSetsRunDir( const QString &path );
    QString runDir() const
        {QMutexLocker ml(&remoteMtx); return appData.runDir;}
    void makePathAbsolute( QString &path );

    void saveSettings() const;

// ----------------
// Event processing
// ----------------

    void giveFocus2Console();
    void updateConsoleTitle( const QString &status );

// ------------------
// Menu item handlers
// ------------------

public slots:
// File menu
    void file_Open();
    void file_NewRun();
    bool file_AskStopRun();
    void file_AskQuit();

// Options
    void options_ToggleDebug();
    void options_PickRunDir();
    void options_AODlg();
    void options_ToggleSaveChks();

// Tools
    void tools_VerifySha1();
    void tools_ShowPar2Win();
    void tools_ToggleEditLog();
    void tools_SaveLogFile();

// Window
    void window_ShowHideConsole();
    void window_ShowHideGraphs();

// Help
    void help_HelpDlg();
    void help_About();
    void help_AboutQt();

// -----
// Slots
// -----

public slots:
// FileViewer
    void fileOpen( FileViewerWindow *fvThis );

// Window needs app service
    void aoCtlClosed();
    void par2WinClosed();
    void helpWindowClosed();

// App gets startup status
    void poolReady();

// CmdSrv
    bool remoteGetsIsConsoleHidden() const;
    void remoteSetsRunName( const QString &name );
    QString remoteStartsRun();
    void remoteStopsRun();
    void remoteSetsDigitalOut( const QString &chan, bool onoff );
    void remoteShowsConsole( bool show );

// Run synchronizes with app
    void runIniting();
    void runInitAbortedByUser( QAbstractButton * );
    void runStarted();
    void runStopped();
    void runDaqError( const QString &e );

// -------
// Private
// -------

private:
    void showStartupMessages();
    void loadRunDir( QSettings &settings );
    void loadSettings();
    bool runCmdStart( QString *errTitle = 0, QString *errMsg = 0 );
};

#endif  // MAINAPP_H

