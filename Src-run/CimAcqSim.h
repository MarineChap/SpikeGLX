#ifndef CIMACQSIM_H
#define CIMACQSIM_H

#include "CimAcq.h"

/* ---------------------------------------------------------------- */
/* Types ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

// Simulated IMEC input
//
class CimAcqSim : public CimAcq
{
public:
    CimAcqSim( IMReaderWorker *owner, const Params &p )
    : CimAcq( owner, p ) {}

    virtual void run();
};

#endif  // CIMACQSIM_H

