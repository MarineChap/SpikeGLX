#ifndef TRIGTCP_H
#define TRIGTCP_H

#include "TrigBase.h"

/* ---------------------------------------------------------------- */
/* Types ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

class TrigTCP : public TrigBase
{
private:
    double          trigHiT,
                    trigLoT;
    volatile bool   trigHi;

public:
    TrigTCP(
        DAQ::Params     &p,
        GraphsWindow    *gw,
        const AIQ       *imQ,
        const AIQ       *niQ )
    : TrigBase( p, gw, imQ, niQ ), trigHiT(-1), trigHi(false) {}

    void rgtSetTrig( bool hi );

    virtual void setGate( bool hi );
    virtual void resetGTCounters();

public slots:
    virtual void run();

private:
    bool isTrigHi()     {QMutexLocker ml( &runMtx ); return trigHi;}
    double getTrigHiT() {QMutexLocker ml( &runMtx ); return trigHiT;}
    double getTrigLoT() {QMutexLocker ml( &runMtx ); return trigLoT;}
    bool bothWriteSome(
        int     &ig,
        int     &it,
        quint64 &imNextCt,
        quint64 &niNextCt );
    bool eachWriteSome(
        DataFile    *df,
        const AIQ   *aiQ,
        quint64     &nextCt );
    bool writeRem(
        DataFile    *df,
        const AIQ   *aiQ,
        quint64     &nextCt,
        double      tlo );
};

#endif  // TRIGTCP_H

