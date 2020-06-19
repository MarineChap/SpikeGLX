
#include "TrigSpike.h"
#include "Util.h"
#include "Biquad.h"
#include "MainApp.h"
#include "Run.h"
#include "GraphsWindow.h"

#include <QTimer>
#include <QThread>


#define LOOP_MS     100


static TrigSpike    *ME;


/* ---------------------------------------------------------------- */
/* TrSpkWorker ---------------------------------------------------- */
/* ---------------------------------------------------------------- */

void TrSpkWorker::run()
{
    const int   nID = vID.size();
    bool        ok  = true;

    for(;;) {

        if( !shr.wake( ok ) )
            break;

        for( int iID = 0; iID < nID; ++iID ) {

            if( !(ok = writeSomeIM( vID[iID] )) )
                break;
        }
    }

    emit finished();
}


bool TrSpkWorker::writeSomeIM( int ip )
{
    TrigSpike::CountsIm &C      = ME->imCnt;
    vec_i16             data;
    quint64             headCt  = C.nextCt[ip];
    int                 nMax    = C.remCt[ip];

    if( !ME->nScansFromCt( data, headCt, nMax, ip ) )
        return false;

    uint    size = data.size();

    if( !size )
        return true;

// ---------------
// Update tracking
// ---------------

    C.nextCt[ip]    += size / imQ[ip]->nChans();
    C.remCt[ip]     -= C.nextCt[ip] - headCt;

// -----
// Write
// -----

    return ME->writeAndInvalData( ME->DstImec, ip, data, headCt );
}

/* ---------------------------------------------------------------- */
/* TrSpkThread ---------------------------------------------------- */
/* ---------------------------------------------------------------- */

TrSpkThread::TrSpkThread(
    TrSpkShared         &shr,
    const QVector<AIQ*> &imQ,
    std::vector<int>    &vID )
{
    thread  = new QThread;
    worker  = new TrSpkWorker( shr, imQ, vID );

    worker->moveToThread( thread );

    Connect( thread, SIGNAL(started()), worker, SLOT(run()) );
    Connect( worker, SIGNAL(finished()), worker, SLOT(deleteLater()) );
    Connect( worker, SIGNAL(destroyed()), thread, SLOT(quit()), Qt::DirectConnection );

    thread->start();
}


TrSpkThread::~TrSpkThread()
{
// worker object auto-deleted asynchronously
// thread object manually deleted synchronously (so we can call wait())

    if( thread->isRunning() )
        thread->wait();

    delete thread;
}

/* ---------------------------------------------------------------- */
/* struct HiPassFnctr --------------------------------------------- */
/* ---------------------------------------------------------------- */

// IMPORTANT!!
// -----------
// The Biquad filter functions have internal memory, so if there's
// a discontinuity in a filter's input stream, transients ensue.
// Here's the strategy to combat filter transients...
// We look for edges using findFltFallingEdge(), starting from the
// position 'edgeCt'. Every time we modify edgeCt we will tell the
// filter to reset its 'zero' counter to BIQUAD_TRANS_WIDE. We'll
// have the filter zero that many leading data points.

TrigSpike::HiPassFnctr::HiPassFnctr( const DAQ::Params &p )
{
    fltbuf.resize( nmax = 256 );

    chan    = p.trgSpike.aiChan;
    flt     = 0;

    if( p.trgSpike.stream == "nidq" ) {

        if( chan < p.ni.niCumTypCnt[CniCfg::niSumNeural] ) {

            flt     = new Biquad( bq_type_highpass, 300/p.ni.srate );
            maxInt  = 32768;
        }
    }
    else {

        // Highpass filtering in the Imec AP band is primarily
        // used to remove DC offsets, rather than LFP.

        const CimCfg::AttrEach  &E =
                p.im.each[p.streamID( p.trgSpike.stream )];

        if( chan < E.imCumTypCnt[CimCfg::imSumAP] ) {

            flt     = new Biquad( bq_type_highpass, 300/E.srate );
            maxInt  = E.roTbl->maxInt();
        }
    }

    reset();
}


TrigSpike::HiPassFnctr::~HiPassFnctr()
{
    if( flt )
        delete flt;
}


void TrigSpike::HiPassFnctr::reset()
{
    nzero = BIQUAD_TRANS_WIDE;
}


void TrigSpike::HiPassFnctr::operator()( int nflt )
{
    if( flt ) {

        flt->apply1BlockwiseMem1( &fltbuf[0], maxInt, nflt, 1, 0 );

        if( nzero > 0 ) {

            // overwrite with zeros

            if( nflt > nzero )
                nflt = nzero;

            memset( &fltbuf[0], 0, nflt*sizeof(qint16) );
            nzero -= nflt;
        }
    }
}

/* ---------------------------------------------------------------- */
/* CountsIm ------------------------------------------------------- */
/* ---------------------------------------------------------------- */

TrigSpike::CountsIm::CountsIm( const DAQ::Params &p )
    :   offset(p.ni.enabled ? 1 : 0),
        np(p.im.get_nProbes())
{
    nextCt.resize( np );
    remCt.resize( np );

    periEvtCt.resize( np );
    refracCt.resize( np );
    latencyCt.resize( np );

    for( int ip = 0; ip < np; ++ip ) {

        double  srate = p.im.each[ip].srate;

        periEvtCt[ip]   = p.trgSpike.periEvtSecs * srate;
        refracCt[ip]    = qMax( p.trgSpike.refractSecs * srate, 5.0 );
        latencyCt[ip]   = 0.25 * srate;
    }
}


void TrigSpike::CountsIm::setupWrite( const std::vector<quint64> &vEdge )
{
    for( int ip = 0; ip < np; ++ip ) {
        nextCt[ip]  = vEdge[offset+ip] - periEvtCt[ip];
        remCt[ip]   = 2 * periEvtCt[ip] + 1;
    }
}


quint64 TrigSpike::CountsIm::minCt( int ip )
{
    return periEvtCt[ip] + latencyCt[ip];
}


bool TrigSpike::CountsIm::remCtDone()
{
    for( int ip = 0; ip < np; ++ip ) {

        if( remCt[ip] > 0 )
            return false;
    }

    return true;
}

/* ---------------------------------------------------------------- */
/* CountsNi ------------------------------------------------------- */
/* ---------------------------------------------------------------- */

TrigSpike::CountsNi::CountsNi( const DAQ::Params &p )
    :   nextCt(0), remCt(0),
        periEvtCt(p.trgSpike.periEvtSecs * p.ni.srate),
        refracCt(qMax( p.trgSpike.refractSecs * p.ni.srate, 5.0 )),
        latencyCt(0.25 * p.ni.srate)
{
}


void TrigSpike::CountsNi::setupWrite(
    const std::vector<quint64>  &vEdge,
    bool                        enabled )
{
    nextCt  = vEdge[0] - periEvtCt;
    remCt   = (enabled ? 2 * periEvtCt + 1 : 0);
}


quint64 TrigSpike::CountsNi::minCt()
{
    return periEvtCt + latencyCt;
}

/* ---------------------------------------------------------------- */
/* TrigSpike ------------------------------------------------------ */
/* ---------------------------------------------------------------- */

TrigSpike::TrigSpike(
    const DAQ::Params   &p,
    GraphsWindow        *gw,
    const QVector<AIQ*> &imQ,
    const AIQ           *niQ )
    :   TrigBase( p, gw, imQ, niQ ),
        usrFlt(new HiPassFnctr( p )),
        imCnt( p ),
        niCnt( p ),
        spikesMax(p.trgSpike.isNInf ? UNSET64 : p.trgSpike.nS),
        aEdgeCtNext(0),
        thresh(p.trigThreshAsInt())
{
}


#define ISSTATE_GetEdge     (state == 0)
#define ISSTATE_Write       (state == 1)
#define ISSTATE_Done        (state == 2)


// Spike logic is driven by TrgSpikeParams:
// {periEvtSecs, refractSecs, inarow, nS, T}.
// Corresponding states defined above.
//
void TrigSpike::run()
{
    Debug() << "Trigger thread started.";

// ---------
// Configure
// ---------

    ME = this;

// Create worker threads

    const int                   nPrbPerThd = 2;

    std::vector<TrSpkThread*>   trT;
    TrSpkShared                 shr( p );

    nThd = 0;

    for( int ip0 = 0; ip0 < nImQ; ip0 += nPrbPerThd ) {

        std::vector<int>    vID;

        for( int id = 0; id < nPrbPerThd; ++id ) {

            if( ip0 + id < nImQ )
                vID.push_back( ip0 + id );
            else
                break;
        }

        trT.push_back( new TrSpkThread( shr, imQ, vID ) );
        ++nThd;
    }

// Wait for threads to reach ready (sleep) state

    shr.runMtx.lock();
        while( shr.asleep < nThd ) {
            shr.runMtx.unlock();
                QThread::usleep( 10 );
            shr.runMtx.lock();
        }
    shr.runMtx.unlock();

// -----
// Start
// -----

    setYieldPeriod_ms( LOOP_MS );

    initState();

    QString err;

    while( !isStopped() ) {

        double  loopT = getTime();
        bool    inactive;

        // -------
        // Active?
        // -------

        inactive = ISSTATE_Done || !isGateHi();

        if( inactive ) {

            initState();
            goto next_loop;
        }

        // --------------
        // Seek next edge
        // --------------

        if( ISSTATE_GetEdge ) {

            if( p.trgSpike.stream == "nidq" ) {

                if( !getEdge( 0 ) )
                    goto next_loop;
            }
            else {

                int iSrc = (niQ ? 1 : 0) + p.streamID( p.trgSpike.stream );

                if( !getEdge( iSrc ) )
                    goto next_loop;
            }

            QMetaObject::invokeMethod(
                gw, "blinkTrigger",
                Qt::QueuedConnection );

            // ---------------
            // Start new files
            // ---------------

            {
                int ig, it;

                if( !newTrig( ig, it, false ) ) {
                    err = "open file failed";
                    break;
                 }

                setSyncWriteMode();
            }

            SETSTATE_Write();
        }

        // ----------------
        // Handle this edge
        // ----------------

        if( ISSTATE_Write ) {

            if( !xferAll( shr, err ) )
                break;

            // -----
            // Done?
            // -----

            if( niCnt.remCt <= 0 && imCnt.remCtDone() ) {

                endTrig();

                if( ++nSpikes >= spikesMax )
                    SETSTATE_Done();
                else {

                    usrFlt->reset();

                    for( int is = 0, ns = vS.size(); is < ns; ++is ) {

                        if( vS[is].ip >= 0 )
                            vEdge[is] += imCnt.refracCt[is];
                        else
                            vEdge[is] += niCnt.refracCt;
                    }

                    SETSTATE_GetEdge();
                }
            }
        }

        // ------
        // Status
        // ------

next_loop:
       if( loopT - statusT > 1.0 ) {

            QString sOn, sWr;

            statusOnSince( sOn );
            statusWrPerf( sWr );

            Status() << sOn << sWr;

            statusT = loopT;
        }

        // -------------------
        // Moderate fetch rate
        // -------------------

        yield( loopT );
    }

// Kill all threads

    shr.kill();

    for( int iThd = 0; iThd < nThd; ++iThd ) {
        trT[iThd]->thread->wait( 10000/nThd );
        delete trT[iThd];
    }

// Done

    endRun( err );
}


void TrigSpike::SETSTATE_GetEdge()
{
    aEdgeCtNext = 0;
    state       = 0;
}


void TrigSpike::SETSTATE_Write()
{
    imCnt.setupWrite( vEdge );
    niCnt.setupWrite( vEdge, niQ != 0 );

    state = 1;
}


void TrigSpike::SETSTATE_Done()
{
    state = 2;
    mainApp()->getRun()->dfSetRecordingEnabled( false, true );
}


void TrigSpike::initState()
{
    usrFlt->reset();
    vEdge.clear();
    nSpikes = 0;
    SETSTATE_GetEdge();
}


// Find edge in iSrc stream but translate to all others.
//
// Return true if found.
//
bool TrigSpike::getEdge( int iSrc )
{
// Start getEdge() search at gate edge, subject to
// periEvent criteria. Precision not needed here;
// sync only applied to getEdge() results.

    if( !vEdge.size() ) {

        const SyncStream    &S = vS[iSrc];
        quint64             minCt;

        usrFlt->reset();
        vEdge.resize( vS.size() );

        minCt = (S.ip >= 0 ? imCnt.minCt( S.ip ) : niCnt.minCt());

        vEdge[iSrc] = qMax(
            S.TAbs2Ct( getGateHiT() ),
            S.Q->qHeadCt() + minCt );
    }

// It may take several tries to achieve pulser sync for multi streams.
// aEdgeCtNext saves us from costly refinding of edge-A while hunting.

    int     ns;
    bool    found;

    if( aEdgeCtNext )
        found = true;
    else {
        found = vS[iSrc].Q->findFltFallingEdge(
                    aEdgeCtNext,
                    vEdge[iSrc],
                    thresh,
                    p.trgSpike.inarow,
                    *usrFlt );

        if( !found ) {
            vEdge[iSrc] = aEdgeCtNext;  // pick up search here
            aEdgeCtNext = 0;
        }
    }

    if( found && (ns = vS.size()) > 1 ) {

        syncDstTAbsMult( aEdgeCtNext, iSrc, vS, p );

        for( int is = 0; is < ns; ++is ) {

            if( is != iSrc ) {

                const SyncStream    &S = vS[is];

                if( p.sync.sourceIdx != DAQ::eSyncSourceNone && !S.bySync )
                    return false;

                vEdge[is] = S.TAbs2Ct( S.tAbs );
            }
        }
    }

    if( found )
        vEdge[iSrc] = aEdgeCtNext;

    return found;
}


bool TrigSpike::writeSomeNI()
{
    if( !niQ )
        return true;

    CountsNi    &C = niCnt;
    vec_i16     data;
    quint64     headCt = C.nextCt;

    if( !nScansFromCt( data, headCt, C.remCt, -1 ) )
        return false;

    uint    size = data.size();

    if( !size )
        return true;

// ---------------
// Update tracking
// ---------------

    C.nextCt    += size / niQ->nChans();
    C.remCt     -= C.nextCt - headCt;

// -----
// Write
// -----

    return writeAndInvalData( DstNidq, 0, data, headCt );
}


// Return true if no errors.
//
bool TrigSpike::xferAll( TrSpkShared &shr, QString &err )
{
    bool    niOK;

    shr.awake   = 0;
    shr.asleep  = 0;
    shr.errors  = 0;

// Wake all imec threads

    shr.condWake.wakeAll();

// Do nidq locally

    niOK = writeSomeNI();

// Wait all threads started, and all done

    shr.runMtx.lock();
        while( shr.awake  < nThd
            || shr.asleep < nThd ) {

            shr.runMtx.unlock();
                QThread::msleep( LOOP_MS/8 );
            shr.runMtx.lock();
        }
    shr.runMtx.unlock();

    if( niOK && !shr.errors )
        return true;

    err = "write failed";
    return false;
}


