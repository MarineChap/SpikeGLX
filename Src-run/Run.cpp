
#include "Run.h"
#include "Util.h"
#include "MainApp.h"
#include "ConfigCtl.h"
#include "IMReader.h"
#include "NIReader.h"
#include "GateTCP.h"
#include "TrigTCP.h"
#include "GraphsWindow.h"
#include "GraphFetcher.h"
#include "AOCtl.h"
#include "Version.h"

#include <QAction>
#include <QMessageBox>




/* ---------------------------------------------------------------- */
/* Ctor ----------------------------------------------------------- */
/* ---------------------------------------------------------------- */

Run::Run( MainApp *app )
    :   QObject(0), app(app), imQ(0), niQ(0),
        graphsWindow(0), graphFetcher(0),
        imReader(0), niReader(0),
        gate(0), trg(0), running(false)
{
}

/* ---------------------------------------------------------------- */
/* Owned GraphsWindow ops ----------------------------------------- */
/* ---------------------------------------------------------------- */

bool Run::grfIsUsrOrderIm()
{
    QMutexLocker    ml( &runMtx );
    bool            isUsrOrder = false;

    if( graphsWindow ) {

        QMetaObject::invokeMethod(
            graphsWindow, "remoteIsUsrOrderIm",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, isUsrOrder) );
    }

    return isUsrOrder;
}


bool Run::grfIsUsrOrderNi()
{
    QMutexLocker    ml( &runMtx );
    bool            isUsrOrder = false;

    if( graphsWindow ) {

        QMetaObject::invokeMethod(
            graphsWindow, "remoteIsUsrOrderNi",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, isUsrOrder) );
    }

    return isUsrOrder;
}


void Run::grfRemoteSetsRunLE( const QString &fn )
{
    QMutexLocker    ml( &runMtx );

    if( graphsWindow )
        graphsWindow->remoteSetRunLE( fn );
}


void Run::grfSetStreams( std::vector<GFStream> &gfs )
{
    QMutexLocker    ml( &runMtx );

    for( int is = 0, ns = gfs.size(); is < ns; ++is ) {

        GFStream    &S = gfs[is];

        if( S.stream == "nidq" )
            S.aiQ = niQ;
        else
            S.aiQ = imQ;
    }

    if( graphFetcher )
        graphFetcher->setStreams( gfs );
}


void Run::grfHardPause( bool pause )
{
    QMutexLocker    ml( &runMtx );

    if( graphFetcher )
        graphFetcher->hardPause( pause );
}


void Run::grfSetFocus()
{
    QMutexLocker    ml( &runMtx );

    if( graphsWindow )
        graphsWindow->setFocus( Qt::OtherFocusReason );
}


void Run::grfShowHide()
{
    QMutexLocker    ml( &runMtx );

    if( !graphsWindow )
        return;

    if( graphsWindow->isHidden() ) {
        graphsWindow->eraseGraphs();
        graphsWindow->showNormal();
    }
    else {
        bool    hadfocus = (QApplication::focusWidget() == graphsWindow);

        graphsWindow->hide();

        if( hadfocus )
            app->giveFocus2Console();
    }
}


void Run::grfUpdateRHSFlags()
{
    QMutexLocker    ml( &runMtx );

    if( graphsWindow ) {

        QMetaObject::invokeMethod(
            graphsWindow, "updateRHSFlags",
            Qt::QueuedConnection );
    }
}


void Run::grfUpdateWindowTitles()
{
// ------------
// Craft a name
// ------------

    DAQ::Params &p  = app->cfgCtl()->acceptedParams;
    QString     run = p.sns.runName,
                stat;

    if( running )
        stat = "RUNNING - " + run;
    else
        stat = "READY";

// -----------------------
// Apply to console window
// -----------------------

    app->updateConsoleTitle( stat );

// ----------------------
// Apply to graphs window
// ----------------------

    if( graphsWindow ) {

        graphsWindow->setWindowTitle(
                QString(APPNAME " Graphs - %1").arg( stat ) );

        app->win.copyWindowTitleToAction( graphsWindow );
    }
}

/* ---------------------------------------------------------------- */
/* Owned AIStream ops --------------------------------------------- */
/* ---------------------------------------------------------------- */

// Return smaller of {secsMax seconds, fracMax of available RAM}.
//
// Note: Running with
//   + 1 second long NI stream of 8 analog chans
//   + two shank viewers
//   + audio
// takes about 110 MB RAM as measured by enabling NI PERFMON switch.
// We therefore set baseline "startup" memory use to 120 MB.
//
int Run::streamSpanMax( const DAQ::Params &p, bool warn )
{
    double  startup = 0.12 * 1024.0 * 1024.0 * 1024.0,
            fracMax = 0.40,
            ram     = fracMax * (getRAMBytes32BitApp() - startup),
            bps     = 0.0;
    int     secsMax = 30,
            secs;

    if( p.im.enabled )
        bps += p.im.srate * p.im.imCumTypCnt[CimCfg::imSumAll];

    if( p.ni.enabled )
        bps += p.ni.srate * p.ni.niCumTypCnt[CniCfg::niSumAll];

    bps *= 2.0;
    secs = qBound( 2, int(ram/bps), secsMax );

    if( warn && secs < secsMax )
        Warning() << "Stream length limited to " << secs << " seconds.";

    return secs;
}


quint64 Run::getImScanCount() const
{
    QMutexLocker    ml( &runMtx );

    if( imQ )
        return imQ->endCount();

    return 0;
}


quint64 Run::getNiScanCount() const
{
    QMutexLocker    ml( &runMtx );

    if( niQ )
        return niQ->endCount();

    return 0;
}


const AIQ* Run::getImQ() const
{
    QMutexLocker    ml( &runMtx );

    return imQ;
}


const AIQ* Run::getNiQ() const
{
    QMutexLocker    ml( &runMtx );

    return niQ;
}

/* ---------------------------------------------------------------- */
/* Run control ---------------------------------------------------- */
/* ---------------------------------------------------------------- */

bool Run::isRunning() const
{
    if( runMtx.tryLock( 10 ) ) {

        bool    result = running;
        runMtx.unlock();
        return result;
    }
    else
        return true;
}


bool Run::startRun( QString &errTitle, QString &errMsg )
{
    QMutexLocker    ml( &runMtx );

// ----------
// OK to run?
// ----------

    if( running ) {
        errTitle    = "Already running";
        errMsg      = "Stop previous run before starting another.";
        return false;
    }

// -------------
// General setup
// -------------

    app->runIniting();

    DAQ::Params &p = app->cfgCtl()->acceptedParams;

    setPreciseTiming( true );

// ------
// Graphs
// ------

    createGraphsWindow( p );

// -----------
// IMEC stream
// -----------

    int streamSecs = streamSpanMax( p );

    if( p.im.enabled ) {

        imQ =
            new AIQ(
                p.im.srate,
                p.im.imCumTypCnt[CimCfg::imSumAll],
                streamSecs );

        imReader = new IMReader( p, imQ );
        ConnectUI( imReader->worker, SIGNAL(daqError(QString)), app, SLOT(runDaqError(QString)) );
        ConnectUI( imReader->worker, SIGNAL(finished()), this, SLOT(workerStopsRun()) );
    }

// -----------
// NIDQ stream
// -----------

    if( p.ni.enabled ) {

        niQ =
            new AIQ(
                p.ni.srate,
                p.ni.niCumTypCnt[CniCfg::niSumAll],
                streamSecs );

        niReader = new NIReader( p, niQ );
        ConnectUI( niReader->worker, SIGNAL(daqError(QString)), app, SLOT(runDaqError(QString)) );
        ConnectUI( niReader->worker, SIGNAL(finished()), this, SLOT(workerStopsRun()) );
    }

// -------
// Trigger
// -------

    trg = new Trigger( p, graphsWindow, imQ, niQ );
    ConnectUI( trg->worker, SIGNAL(daqError(QString)), app, SLOT(runDaqError(QString)) );
    ConnectUI( trg->worker, SIGNAL(finished()), this, SLOT(workerStopsRun()) );

// -----
// Start
// -----

    gate = new Gate( p, imReader, niReader, trg->worker );
    ConnectUI( gate->worker, SIGNAL(runStarted()), this, SLOT(gettingSamples()) );
    ConnectUI( gate->worker, SIGNAL(daqError(QString)), app, SLOT(runDaqError(QString)) );
    ConnectUI( gate->worker, SIGNAL(finished()), this, SLOT(workerStopsRun()) );

    running = true;

    grfUpdateWindowTitles();

    QString s = "Acquisition starting up ...";

    Systray() << s;
    Status() << s;
    Log() << s;

    ml.unlock();    // ensure runMtx available to startup agents

    gate->startRun();

    return true;
}


void Run::stopRun()
{
    aoStopDev();

    QMutexLocker    ml( &runMtx );

    if( !running )
        return;

    running = false;

    if( graphFetcher ) {
        delete graphFetcher;
        graphFetcher = 0;
    }

// Note: gate sends messages to trg, so must delete gate before trg.

    if( gate ) {
        delete gate;
        gate = 0;
    }

    if( trg ) {
        delete trg;
        trg = 0;
    }

    if( niReader ) {
        delete niReader;
        niReader = 0;
    }

    if( imReader ) {
        delete imReader;
        imReader = 0;
    }

    if( niQ ) {
        delete niQ;
        niQ = 0;
    }

    if( imQ ) {
        delete imQ;
        imQ = 0;
    }

// Note: graphFetcher (e.g. putScans), gate and trg (e.g. setTriggerLED)
// talk to graphsWindow. Therefore, we must wait for those threads to
// complete before tearing graphsWindow down.

    if( graphsWindow ) {
        app->modelessClosed( graphsWindow );
        delete graphsWindow;
        graphsWindow = 0;
    }

    grfUpdateWindowTitles();

    setPreciseTiming( false );

    QString s = "Acquisition stopped.";

    Systray() << s;
    Status() << s;
    Log() << s;

    app->runStopped();
}


// Return true (and stop) if can stop now.
//
bool Run::askThenStopRun()
{
    if( !isRunning() )
        return true;

    int yesNo = QMessageBox::question(
        0,
        "Stop Current Acquisition",
        "Acquisition in progress.\n"
        "Stop it before proceeding?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No );

    guiBreathe();

    if( yesNo == QMessageBox::Yes ) {

        QMessageBox *M = new QMessageBox(
            QMessageBox::Information,
            "Closing Files",
            "Closing files...please wait.",
            QMessageBox::NoButton,
            0 );

        M->show();
        guiBreathe();

        stopRun();

        delete M;

        return true;
    }

    return false;
}


void Run::imecUpdate()
{
    QMutexLocker    ml( &runMtx );

    if( imReader )
        imReader->worker->update();
}

/* ---------------------------------------------------------------- */
/* GraphFetcher ops ----------------------------------------------- */
/* ---------------------------------------------------------------- */

void Run::grfSoftPause( bool pause )
{
    QMutexLocker    ml( &runMtx );

    if( graphFetcher )
        graphFetcher->softPause( pause );
}

/* ---------------------------------------------------------------- */
/* Owned Datafile ops --------------------------------------------- */
/* ---------------------------------------------------------------- */

// Called by remote process.
//
bool Run::dfIsSaving() const
{
// BK: This is of dubious utility...should be deprecated.
    QMutexLocker    ml( &runMtx );

    return trg && !trg->worker->allFilesClosed();
}


bool Run::dfIsInUse( const QFileInfo &fi ) const
{
    QMutexLocker    ml( &runMtx );

    return trg && trg->worker->isInUse( fi );
}


void Run::dfSetNextFileName( const QString &name )
{
    QMutexLocker    ml( &runMtx );

    if( trg )
        trg->worker->setNextFileName( name );
}


void Run::dfSetRecordingEnabled( bool enabled, bool remote )
{
    QMutexLocker    ml( &runMtx );

    if( remote && graphsWindow ) {

        QMetaObject::invokeMethod(
            graphsWindow, "remoteSetRecordingEnabled",
            Qt::QueuedConnection,
            Q_ARG(bool, enabled) );

        return;
    }

    if( trg )
        trg->worker->setGateEnabled( enabled );
}


void Run::dfForceGTCounters( int g, int t )
{
    QMutexLocker    ml( &runMtx );

    if( trg )
        trg->worker->forceGTCounters( g, t );
}


// Called by remote process.
//
QString Run::dfGetCurNiName() const
{
// BK: This is of dubious utility...should be deprecated.
    QMutexLocker    ml( &runMtx );

    return (trg ? trg->worker->curNiFilename() : QString::null);
}


// Called by remote process.
//
quint64 Run::dfGetImFileStart() const
{
    QMutexLocker    ml( &runMtx );

    if( trg && imQ )
        return trg->worker->curImFileStart();

    return 0;
}


// Called by remote process.
//
quint64 Run::dfGetNiFileStart() const
{
    QMutexLocker    ml( &runMtx );

    if( trg && niQ )
        return trg->worker->curNiFileStart();

    return 0;
}

/* ---------------------------------------------------------------- */
/* Owned gate and trigger ops ------------------------------------- */
/* ---------------------------------------------------------------- */

void Run::rgtSetGate( bool hi )
{
    QMutexLocker    ml( &runMtx );

    if( gate ) {

        DAQ::Params &p = app->cfgCtl()->acceptedParams;

        if( p.mode.mGate == DAQ::eGateTCP )
            dynamic_cast<GateTCP*>(gate->worker)->rgtSetGate( hi );
    }
}


void Run::rgtSetTrig( bool hi )
{
    QMutexLocker    ml( &runMtx );

    if( trg ) {

        DAQ::Params &p = app->cfgCtl()->acceptedParams;

        if( p.mode.mTrig == DAQ::eTrigTCP )
            dynamic_cast<TrigTCP*>(trg->worker)->rgtSetTrig( hi );
    }
}


void Run::rgtSetMetaData( const KeyValMap &kvm )
{
    QMutexLocker    ml( &runMtx );

    if( trg )
        trg->worker->setMetaData( kvm );
}

/* ---------------------------------------------------------------- */
/* Audio ops ------------------------------------------------------ */
/* ---------------------------------------------------------------- */

void Run::aoStart()
{
    aoStop();

    if( isRunning() ) {

        aoStartDev();
        grfUpdateRHSFlags();
    }
}


void Run::aoStop()
{
    if( aoStopDev() )
        grfUpdateRHSFlags();
}

/* ---------------------------------------------------------------- */
/* Private slots -------------------------------------------------- */
/* ---------------------------------------------------------------- */

void Run::gettingSamples()
{
    QMutexLocker    ml( &runMtx );

    if( !running )
        return;

    graphFetcher = new GraphFetcher;

    runMtx.unlock();
        graphsWindow->initColorTTL();
        graphsWindow->initGFStreams();
    runMtx.lock();

    if( app->getAOCtl()->doAutoStart() )
        QMetaObject::invokeMethod( this, "aoStart", Qt::QueuedConnection );

    app->runStarted();
}


void Run::workerStopsRun()
{
    stopRun();
}

/* ---------------------------------------------------------------- */
/* Private -------------------------------------------------------- */
/* ---------------------------------------------------------------- */

void Run::aoStartDev()
{
    AOCtl   *aoC = app->getAOCtl();

    if( !aoC->devStart( imQ, niQ ) ) {
        Warning() << "Could not start audio drivers.";
        aoC->devStop();
    }
}


// Return true if was running.
//
bool Run::aoStopDev()
{
    AOCtl   *aoC = app->getAOCtl();
    bool    wasRunning = false;

    if( aoC->readyForScans() ) {
        aoC->devStop();
        wasRunning  = true;
    }

    return wasRunning;
}


void Run::createGraphsWindow( const DAQ::Params &p )
{
    graphsWindow = new GraphsWindow( p );
    graphsWindow->setAttribute( Qt::WA_DeleteOnClose, false );

    app->act.shwHidGrfsAct->setEnabled( true );
    graphsWindow->show();

    app->modelessOpened( graphsWindow, false );

// Iff app built with Qt Creator, then graphs window
// will not get any mouse events until a modal dialog
// shows on top and is then destroyed!! That's what we
// do here...make an invisible message box.

    {
        QMessageBox XX( 0 );
        XX.setWindowModality( Qt::ApplicationModal );
        XX.setAttribute( Qt::WA_DontShowOnScreen, true );
//        XX.move( QApplication::desktop()->screen()->rect().topLeft() );
        XX.show();
        // auto-destroyed
    }
}


