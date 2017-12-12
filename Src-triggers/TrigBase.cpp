
#include "TrigImmed.h"
#include "TrigSpike.h"
#include "TrigTimed.h"
#include "TrigTCP.h"
#include "TrigTTL.h"
#include "Util.h"
#include "MainApp.h"
#include "GraphsWindow.h"

#include <QThread>


/* ---------------------------------------------------------------- */
/* TrigBase ------------------------------------------------------- */
/* ---------------------------------------------------------------- */

TrigBase::TrigBase(
    const DAQ::Params   &p,
    GraphsWindow        *gw,
    const QVector<AIQ*> &imQ,
    const AIQ           *niQ )
    :   QObject(0), dfNi(0),
        ovr(p), startT(-1), gateHiT(-1), gateLoT(-1), trigHiT(-1),
        firstCtNi(0), iGate(-1), iTrig(-1), gateHi(false),
        pleaseStop(false), p(p), gw(gw), imQ(imQ), niQ(niQ), statusT(-1),
        nImQ(imQ.size())
{
    if( nImQ ) {
        vS.resize( nImQ );
        for( int ip = 0; ip < nImQ; ++ip )
            vS[ip].init( imQ[ip], ip, p );
    }

    if( niQ ) {
        vS.push_front( SyncStream() );
        vS[0].init( niQ, -1, p );
    }
}


bool TrigBase::allFilesClosed() const
{
    QMutexLocker    ml( &dfMtx );

    return !dfNi && !firstCtIm.size();
}


bool TrigBase::isInUse( const QFileInfo &fi ) const
{
    QMutexLocker    ml( &dfMtx );

    for( int ip = 0, np = firstCtIm.size(); ip < np; ++ip ) {

        if( dfImAp[ip] && fi == QFileInfo( dfImAp[ip]->binFileName() ) )
            return true;

        if( dfImLf[ip] && fi == QFileInfo( dfImLf[ip]->binFileName() ) )
            return true;
    }

    if( dfNi && fi == QFileInfo( dfNi->binFileName() ) )
        return true;

    return false;
}


// BK: This should probably be deprecated
//
QString TrigBase::curNiFilename() const
{
    QMutexLocker    ml( &dfMtx );

    return (dfNi ? dfNi->binFileName() : QString::null);
}


quint64 TrigBase::curImFileStart( uint ip ) const
{
    QMutexLocker    ml( &dfMtx );

    if( ip < (uint)firstCtIm.size() )
        return firstCtIm[ip];

    return 0;
}


quint64 TrigBase::curNiFileStart() const
{
    QMutexLocker    ml( &dfMtx );

    return firstCtNi;
}


void TrigBase::setStartT()
{
    startTMtx.lock();
    startT = getTime();
    startTMtx.unlock();
}


void TrigBase::setGateEnabled( bool enabled )
{
    runMtx.lock();
    gateHiT         = getTime();
    ovr.gateEnab    = enabled;
    runMtx.unlock();

    if( enabled ) {

        if( p.mode.mGate == DAQ::eGateImmed )
            setGate( true );
    }
    else
        setGate( false );
}


void TrigBase::forceGTCounters( int g, int t )
{
    runMtx.lock();
    ovr.set( g, t );
    runMtx.unlock();
}


// All callers must manage runMtx around this.
//
void TrigBase::baseSetGate( bool hi )
{
    if( hi ) {

        if( !ovr.gateEnab )
            return;

        bool    started = false;

        startTMtx.lock();
        started = (startT >= 0);
        startTMtx.unlock();

        if( !started ) {
            Warning()
                <<  "Received setGate(hi) before acquisition started"
                    " -- IGNORED.";
            return;
        }

        gateHiT = getTime();

        if( ovr.forceGT )
            ovr.get( iGate, iTrig );
        else {
            ++iGate;
            iTrig = -1;
        }
    }
    else
        gateLoT = getTime();

    gateHi = hi;

    QMetaObject::invokeMethod(
        gw, "setGateLED",
        Qt::QueuedConnection,
        Q_ARG(bool, hi) );
}


// All callers must manage runMtx around this.
//
void TrigBase::baseResetGTCounters()
{
    ovr.reset();
    iGate = -1;
    iTrig = -1;
}


void TrigBase::endTrig()
{
    dfMtx.lock();
        for( int ip = 0, np = firstCtIm.size(); ip < np; ++ip ) {

            if( dfImAp[ip] )
                dfImAp[ip]->closeAsync( kvmRmt );

            if( dfImLf[ip] )
                dfImLf[ip]->closeAsync( kvmRmt );
        }
        dfImAp.clear();
        dfImLf.clear();
        firstCtIm.clear();

        if( dfNi )
            dfNi = (DataFileNI*)dfNi->closeAsync( kvmRmt );
        firstCtNi = 0;
    dfMtx.unlock();

    trigHiT = -1;

    QMetaObject::invokeMethod(
        gw, "setTriggerLED",
        Qt::QueuedConnection,
        Q_ARG(bool, false) );
}


bool TrigBase::newTrig( int &ig, int &it, bool trigLED )
{
    endTrig();

    it = incTrig( ig );

// Create files

    dfMtx.lock();
        if( nImQ ) {
            for( int ip = 0; ip < nImQ; ++ip ) {

                firstCtIm.push_back( 0 );

                dfImAp.push_back(
                    p.im.each[ip].apSaveChanCount() ?
                    new DataFileIMAP( ip ) : 0 );

                dfImLf.push_back(
                    p.im.each[ip].lfSaveChanCount() ?
                    new DataFileIMLF( ip ) : 0 );
            }
        }
        if( niQ ) {
            firstCtNi   = 0;
            dfNi        = new DataFileNI();
        }
    dfMtx.unlock();

// Open files

    for( int ip = 0; ip < nImQ; ++ip ) {

        if( dfImAp[ip] && !openFile( dfImAp[ip], ig, it ) )
            return false;

        if( dfImLf[ip] && !openFile( dfImLf[ip], ig, it ) )
            return false;
    }

    if( !openFile( dfNi, ig, it ) )
        return false;

// Reset state tracking

    trigHiT = getTime();

    if( trigLED ) {
        QMetaObject::invokeMethod(
            gw, "setTriggerLED",
            Qt::QueuedConnection,
            Q_ARG(bool, true) );
    }

    QString sGT = QString("<G%1 T%2>").arg( ig ).arg( it );

    QMetaObject::invokeMethod(
        gw, "updateGT",
        Qt::QueuedConnection,
        Q_ARG(QString, sGT) );

    return true;
}


void TrigBase::setSyncWriteMode()
{
    for( int ip = 0, np = firstCtIm.size(); ip < np; ++ip ) {

        if( dfImAp[ip] )
            dfImAp[ip]->setAsyncWriting( false );

        if( dfImLf[ip] )
            dfImLf[ip]->setAsyncWriting( false );
    }

    if( dfNi )
        dfNi->setAsyncWriting( false );
}


// This function dispatches ALL stream writing to the
// proper DataFile(s).
//
bool TrigBase::writeAndInvalVB(
    DstStream                   dst,
    uint                        ip,
    std::vector<AIQ::AIQBlock>  &vB )
{
    if( dst == DstImec )
        return writeVBIM( vB, ip );
    else
        return writeVBNI( vB );
}


quint64 TrigBase::scanCount( DstStream dst )
{
    QMutexLocker    ml( &dfMtx );
    DataFile        *df = 0;

    if( dst == DstImec ) {

        for( int ip = 0, np = firstCtIm.size(); ip < np; ++ip ) {

            if( (df = dfImAp[ip]) )
                goto count;

            if( (df = dfImLf[ip]) )
                goto count;
        }
    }
    else
        df = dfNi;

count:
    return (df ? df->scanCount() : 0);
}


void TrigBase::endRun()
{
    QMetaObject::invokeMethod(
        gw, "setTriggerLED",
        Qt::QueuedConnection,
        Q_ARG(bool, false) );

    dfMtx.lock();
        for( int ip = 0, np = firstCtIm.size(); ip < np; ++ip ) {

            if( dfImAp[ip] ) {
                dfImAp[ip]->setRemoteParams( kvmRmt );
                dfImAp[ip]->closeAndFinalize();
                delete dfImAp[ip];
            }

            if( dfImLf[ip] ) {
                dfImLf[ip]->setRemoteParams( kvmRmt );
                dfImLf[ip]->closeAndFinalize();
                delete dfImLf[ip];
            }
        }
        dfImAp.clear();
        dfImLf.clear();
        firstCtIm.clear();

        if( dfNi ) {
            dfNi->setRemoteParams( kvmRmt );
            dfNi->closeAndFinalize();
            delete dfNi;
            dfNi        = 0;
            firstCtNi   = 0;
        }
    dfMtx.unlock();
}


void TrigBase::statusOnSince( QString &s, double nowT, int ig, int it )
{
    double  t;
    int     h, m;

    startTMtx.lock();
    t = (startT >= 0 ? nowT - startT : 0);
    startTMtx.unlock();

    h = int(t / 3600);
    t = t - h * 3600;
    m = t / 60;
    t = t - m * 60;

// Statusbar

    QString ch, chim, chni;

    if( p.im.enabled ) {

        int allChans = 0;

        for( int ip = 0; ip < nImQ; ++ip )
            allChans += p.im.each[ip].imCumTypCnt[CimCfg::imSumAll];

        chim = QString("%1CH@%2kHz")
                .arg( allChans )
                .arg( p.im.all.srate / 1e3, 0, 'f', 3 );
    }

    if( p.ni.enabled ) {
        chni = QString("%1CH@%2kHz")
                .arg( p.ni.niCumTypCnt[CniCfg::niSumAll] )
                .arg( p.ni.srate / 1e3, 0, 'f', 3 );
    }

    if( p.im.enabled && p.ni.enabled )
        ch = QString("{%1, %2}").arg( chim ).arg( chni );
    else if( p.im.enabled )
        ch = chim;
    else
        ch = chni;

    s = QString("ON %1h%2m%3s %4 <G%5 T%6>")
        .arg( h, 2, 10, QChar('0') )
        .arg( m, 2, 10, QChar('0') )
        .arg( t, 0, 'f', 1 )
        .arg( ch )
        .arg( ig )
        .arg( it );

// RunToolbar::On-time

    QString sGW;

    if( t ) {
        sGW = QString("%1:%2:%3")
                .arg( h, 2, 10, QChar('0') )
                .arg( m, 2, 10, QChar('0') )
                .arg( (int)t, 2, 10, QChar('0') );
    }
    else
        sGW = "--:--:--";

    QMetaObject::invokeMethod(
        gw, "updateOnTime",
        Qt::QueuedConnection,
        Q_ARG(QString, sGW) );

// RunToolbar::Rec-time

    if( trigHiT >= 0 ) {

        t = nowT - trigHiT;
        h = int(t / 3600);
        t = t - h * 3600;
        m = t / 60;
        t = t - m * 60;

        sGW = QString("%1:%2:%3")
                .arg( h, 2, 10, QChar('0') )
                .arg( m, 2, 10, QChar('0') )
                .arg( (int)t, 2, 10, QChar('0') );
    }
    else
        sGW = "--:--:--";

    QMetaObject::invokeMethod(
        gw, "updateRecTime",
        Qt::QueuedConnection,
        Q_ARG(QString, sGW) );
}


void TrigBase::statusWrPerf( QString &s )
{
    int np = firstCtIm.size();

    if( dfNi || np ) {

        // report worst case values

        double  imFull  = 0.0,
                niFull  = 0.0,
                wbps    = 0.0,
                rbps    = 0.0;

        for( int ip = 0; ip < np; ++ip ) {

            if( dfImAp[ip] ) {
                imFull   = qMax( imFull, dfImAp[ip]->percentFull() );
                wbps    += dfImAp[ip]->writeSpeedBps();
                rbps    += dfImAp[ip]->requiredBps();
            }

            if( dfImLf[ip] ) {
                imFull  = qMax( imFull, dfImLf[ip]->percentFull() );
                wbps   += dfImLf[ip]->writeSpeedBps();
                rbps   += dfImLf[ip]->requiredBps();
            }
        }

        if( dfNi ) {
            niFull  = dfNi->percentFull();
            wbps   += dfNi->writeSpeedBps();
            rbps   += dfNi->requiredBps();
        }

        s = QString(" FileQFill%=(%1,%2) MB/s=%3 (%4 req)")
            .arg( imFull, 0, 'f', 1 )
            .arg( niFull, 0, 'f', 1 )
            .arg( wbps/(1024*1024), 0, 'f', 1 )
            .arg( rbps/(1024*1024), 0, 'f', 1 );
    }
    else
        s = QString::null;
}


void TrigBase::setYieldPeriod_ms( int loopPeriod_ms )
{
    if( loopPeriod_ms > 0 )
        loopPeriod_us = 1000 * loopPeriod_ms;
    else
        loopPeriod_us = 1000 * daqAIFetchPeriodMillis();
}


void TrigBase::yield( double loopT )
{
    loopT = 1e6 * (getTime() - loopT);  // microsec

    if( loopT < loopPeriod_us )
        usleep( loopPeriod_us - loopT );
    else
        usleep( 1000 * 10 );
}


bool TrigBase::openFile( DataFile *df, int ig, int it )
{
    if( !df )
        return true;

    QString name = QString("%1/%2_g%3_t%4.%5.bin")
                    .arg( mainApp()->runDir() )
                    .arg( p.sns.runName )
                    .arg( ig )
                    .arg( it )
                    .arg( df->fileLblFromObj() );

    if( !df->openForWrite( p, name ) ) {
        Error()
            << "Error opening file: ["
            << name
            << "].";
        return false;
    }

    return true;
}


// Write LF samples on X12 boundaries (sample%12==0).
//
// - inplace true means block vB[i] will not be used
// for AP, so we can write the X12 samples directly into
// the block. Otherwise we allocate an alternate dst.
//
// - xtra true means that for vB[0] the first sample
// is not an X12, so we will need to construct the prior
// X12 LF data by extrapolating from the nearest forward
// X12 and the timepoint preceding it. The constructed
// sync data are a copy of the vB[0] values.
//
bool TrigBase::write1LF(
    std::vector<AIQ::AIQBlock> &vB,
    int                         i,
    uint                        ip,
    bool                        inplace,
    bool                        xtra )
{
    vec_i16 dstAlt;
    vec_i16 *dst;
    qint16  *D, *S;
    int     R   = vB[i].headCt % 12,
            nCh = p.im.each[ip].imCumTypCnt[CimCfg::imSumAll],
            nTp = (int)vB[i].data.size() / nCh;

// Set up dst = destination workspace
// D points to first destination for X12 copies

    if( inplace )
        dst = &vB[i].data;
    else {
        dstAlt.resize( ((xtra ? 2 : 1) + nTp) * nCh );
        dst = &dstAlt;
    }

    D = &dst->front();

// S points to first source timepoint

    if( R )
        R = 12 - R;

    S = &vB[i].data[R*nCh];

// Extrapolate extra first timepoint if needed

    if( xtra ) {

        // Which blocks hold timepoints 1, 2, where 2 is X12
        // and 1 is just before. Set p1, p2 to their starts.

        qint16  *p1     = 0,
                *p2     = 0;
        int     off1    = R - 1,
                off2    = R;

        for( int ib = 0, nb = vB.size(); ib < nb; ++ib ) {

            int nt = vB[ib].data.size() / nCh;

            if( !p1 ) {
                if( off1 < nt )
                    p1 =  &vB[ib].data[off1*nCh];
                else
                    off1 -= nt;
            }

            if( !p2 ) {
                if( off2 < nt ) {
                    p2 =  &vB[ib].data[off2*nCh];
                    break;
                }
                else
                    off2 -= nt;
            }
        }

        int nAP = p.im.each[ip].imCumTypCnt[CimCfg::imSumAP],
            nLF = p.im.each[ip].imCumTypCnt[CimCfg::imSumNeural] - nAP;

        p1 += nAP;
        p2 += nAP;
        D  += nAP;  // D offset temporarily to LF channels

        for( int lf = 0; lf < nLF; ++lf )
            D[lf] = p2[lf] - (p2[lf] - p1[lf]) * 12;

        D -= nAP;   // D normalized

        // sync channels

        for( int is = nAP + nLF; is < nCh; ++is )
            D[is] = vB[0].data[is];

        D += nCh;
    }

// S to D X12 copies

    for( int it = R; it < nTp; it += 12, D += nCh, S += 12*nCh )
        memcpy( D, S, nCh * sizeof(qint16) );

    dst->resize( D - &dst->front() );

    if( dst->size() && !dfImLf[ip]->writeAndInvalSubset( p, *dst ) )
        return false;

    return true;
}


// Split the data into (AP+SY) and (LF+SY) components,
// directing each to the appropriate data file.
//
// Here, all AP data are written, but only LF samples
// on X12-boundary (sample%12==0) are written.
//
bool TrigBase::writeVBIM( std::vector<AIQ::AIQBlock> &vB, uint ip )
{
    uint    np      = firstCtIm.size();
    bool    isAP    = (ip < np && dfImAp[ip]),
            isLF    = (ip < np && dfImLf[ip]),
            xtra    = false;

    if( !(isAP || isLF) )
        return true;

    int nb = (int)vB.size();

    if( nb && !firstCtIm[ip] ) {

        firstCtIm[ip] = vB[0].headCt;

        if( isAP )
            dfImAp[ip]->setFirstSample( firstCtIm[ip] );

        if( isLF ) {

            if( firstCtIm[ip] % 12 )
                xtra = true;

            dfImLf[ip]->setFirstSample( firstCtIm[ip] / 12 );
        }
    }

    for( int i = 0; i < nb; ++i ) {

        if( isLF && !write1LF( vB, i, ip, !isAP, xtra ) )
            return false;

        if( isAP && !dfImAp[ip]->writeAndInvalSubset( p, vB[i].data ) )
            return false;

        xtra = false;
    }

    return true;
}


bool TrigBase::writeVBNI( std::vector<AIQ::AIQBlock> &vB )
{
    if( !dfNi )
        return true;

    int nb = (int)vB.size();

    if( nb && !firstCtNi ) {
        firstCtNi = vB[0].headCt;
        dfNi->setFirstSample( firstCtNi );
    }

    for( int i = 0; i < nb; ++i ) {

        if( !dfNi->writeAndInvalSubset( p, vB[i].data ) )
            return false;
    }

    return true;
}

/* ---------------------------------------------------------------- */
/* Trigger -------------------------------------------------------- */
/* ---------------------------------------------------------------- */

Trigger::Trigger(
    const DAQ::Params   &p,
    GraphsWindow        *gw,
    const QVector<AIQ*> &imQ,
    const AIQ           *niQ )
{
    thread  = new QThread;

    if( p.mode.mTrig == DAQ::eTrigImmed )
        worker = new TrigImmed( p, gw, imQ, niQ );
    else if( p.mode.mTrig == DAQ::eTrigTimed )
        worker = new TrigTimed( p, gw, imQ, niQ );
    else if( p.mode.mTrig == DAQ::eTrigTTL )
        worker = new TrigTTL( p, gw, imQ, niQ );
    else if( p.mode.mTrig == DAQ::eTrigSpike )
        worker = new TrigSpike( p, gw, imQ, niQ );
    else
        worker = new TrigTCP( p, gw, imQ, niQ );

    worker->moveToThread( thread );

    Connect( thread, SIGNAL(started()), worker, SLOT(run()) );
    Connect( worker, SIGNAL(finished()), worker, SLOT(deleteLater()) );
    Connect( worker, SIGNAL(destroyed()), thread, SLOT(quit()), Qt::DirectConnection );

    thread->start();
}


Trigger::~Trigger()
{
// worker object auto-deleted asynchronously
// thread object manually deleted synchronously (so we can call wait())

    if( thread->isRunning() ) {

        worker->stop();
        thread->wait();
    }

    delete thread;
}


