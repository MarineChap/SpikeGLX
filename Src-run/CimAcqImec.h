#ifndef CIMACQIMEC_H
#define CIMACQIMEC_H

#ifdef HAVE_IMEC

#include "CimAcq.h"
#include "IMEC/NeuropixAPI.h"

#include <QSet>

class CimAcqImec;


/* ---------------------------------------------------------------- */
/* Types ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

struct ImAcqShared {
    double          startT;
    QMutex          runMtx;
    QWaitCondition  condWake;
    int             awake,
                    asleep;
    bool            stop;

    ImAcqShared();

    bool wait()
    {
        bool    run;
        runMtx.lock();
            ++asleep;
            condWake.wait( &runMtx );
            ++awake;
            run = !stop;
        runMtx.unlock();
        return run;
    }

    bool stopping()
    {
        bool    _stop;
        runMtx.lock();
            _stop = stop;
        runMtx.unlock();
        return _stop;
    }

    void kill()
    {
        runMtx.lock();
            stop = true;
        runMtx.unlock();
        condWake.wakeAll();
    }
};


struct ImAcqProbe {
    double          tPreEnq,
                    tPostEnq,
                    peakDT,
                    sumTot,
                    sumLag,
                    sumGet,
                    sumScl,
                    sumEnq;
    quint64         totPts;
// Experiment to detect gaps in timestamps across fetches.
    quint32         tStampLastFetch;
    int             ip,
                    nAP,
                    nLF,
                    nSY,
                    nCH,
                    slot,
                    port,
                    fetchType,  // accommodate custom probe architectures
                    sumN;
    mutable bool    zeroFill;

    ImAcqProbe()    {}
    ImAcqProbe(
        const CimCfg::ImProbeTable  &T,
        const DAQ::Params           &p,
        int                         ip );
};


class ImAcqWorker : public QObject
{
    Q_OBJECT

private:
    CimAcqImec              *acq;
    QVector<AIQ*>           &imQ;
    ImAcqShared             &shr;
    std::vector<ImAcqProbe> probes;
    std::vector<qint32>     E;
    double                  loopT,
                            lastCheckT;

public:
    ImAcqWorker(
        CimAcqImec              *acq,
        QVector<AIQ*>           &imQ,
        ImAcqShared             &shr,
        std::vector<ImAcqProbe> &probes )
    :   acq(acq), imQ(imQ), shr(shr), probes(probes)    {}
    virtual ~ImAcqWorker()                              {}

signals:
    void finished();

public slots:
    void run();

private:
    bool doProbe( float *lfLast, vec_i16 &dst1D, ImAcqProbe &P );
    void workerYield();
    bool keepingUp( const ImAcqProbe &P );
    void profile( ImAcqProbe &P );
};


class ImAcqThread
{
public:
    QThread     *thread;
    ImAcqWorker *worker;

public:
    ImAcqThread(
        CimAcqImec              *acq,
        QVector<AIQ*>           &imQ,
        ImAcqShared             &shr,
        std::vector<ImAcqProbe> &probes );
    virtual ~ImAcqThread();
};


// Hardware IMEC input
//
class CimAcqImec : public CimAcq
{
    friend class ImAcqWorker;

private:
    const CimCfg::ImProbeTable  &T;
    NeuropixAPI                 IM;
    ImAcqShared                 shr;
    std::vector<ImAcqThread*>   imT;
    QSet<int>                   pausPortsReported;
    int                         pausPortsRequired,
                                pausSlot,
                                nThd;

public:
    CimAcqImec( IMReaderWorker *owner, const DAQ::Params &p );
    virtual ~CimAcqImec();

    virtual void run();
    virtual void update( int ip );

private:
    void pauseSlot( int slot );
    int  pausedSlot() const {QMutexLocker ml( &runMtx ); return pausSlot;}
    bool pauseAck( int port );
    bool pauseAllAck() const;

    bool fetchE( int &nE, qint32 *E, const ImAcqProbe &P );
    int fifoPct( const ImAcqProbe &P );

    void SETLBL( const QString &s, bool zero = false );
    void SETVAL( int val );
    void SETVALBLOCKING( int val );

    bool _open( const CimCfg::ImProbeTable &T );

    bool _openProbe( const CimCfg::ImProbeDat &P );
    bool _calibrateADC( const CimCfg::ImProbeDat &P );
    bool _calibrateGain( const CimCfg::ImProbeDat &P );
    bool _dataGenerator( const CimCfg::ImProbeDat &P );
    bool _setLEDs( const CimCfg::ImProbeDat &P );
    bool _selectElectrodes( const CimCfg::ImProbeDat &P );
    bool _setReferences( const CimCfg::ImProbeDat &P );
    bool _setGains( const CimCfg::ImProbeDat &P );
    bool _setHighPassFilter( const CimCfg::ImProbeDat &P );
    bool _setStandby( const CimCfg::ImProbeDat &P );
    bool _writeProbe( const CimCfg::ImProbeDat &P );

    bool _setTrigger();
    bool _setArm();

    bool _softStart();

    bool configure();
    bool startAcq();
    void runError( QString err );
};

#endif  // HAVE_IMEC

#endif  // CIMACQIMEC_H


