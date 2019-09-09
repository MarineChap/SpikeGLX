#ifndef IMROTBL_T0_H
#define IMROTBL_T0_H

#include "IMROTbl.h"

#include <QVector>

/* ---------------------------------------------------------------- */
/* Types ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

struct IMRODesc_T0
{
    qint16  bank,
            apgn,   // gain, not index
            lfgn;   // gain, not index
    qint8   refid,  // reference index
            apflt;  // bool

    IMRODesc_T0()
    :   bank(0), apgn(500), lfgn(250), refid(0), apflt(1)               {}
    IMRODesc_T0( int bank, int refid, int apgn, int lfgn, bool apflt )
    :   bank(bank), apgn(apgn), lfgn(lfgn), refid(refid), apflt(apflt)  {}
    int chToEl( int ch ) const;
    bool operator==( const IMRODesc_T0 &rhs ) const
        {return bank==rhs.bank && apgn==rhs.apgn && lfgn==rhs.lfgn
            && refid==rhs.refid && apflt==rhs.apflt;}
    QString toString( int chn ) const;
    static IMRODesc_T0 fromString( const QString &s );
};


struct IMROTbl_T0 : public IMROTbl
{
    enum imLims_T0 {
        imType0Type     = 0,
        imType0Elec     = 960,
        imType0Banks    = 3,
        imType0Chan     = 384,
        imType0Refids   = 5,
        imType0Gains    = 8
    };

    QVector<IMRODesc_T0>    e;

    IMROTbl_T0()            {type=imType0Type;}
    virtual ~IMROTbl_T0()   {}

    virtual void copyFrom( const IMROTbl *rhs )
    {
        type    = rhs->type;
        e       = ((const IMROTbl_T0*)rhs)->e;
    }

    virtual void fillDefault();

    virtual int nShank() const      {return 1;}
    virtual int nCol() const        {return 2;}
    virtual int nRow() const        {return imType0Elec/2;}
    virtual int nChan() const       {return e.size();}
    virtual int nAP() const         {return imType0Chan;}
    virtual int nLF() const         {return imType0Chan;}
    virtual int nSY() const         {return 1;}
    virtual int nElec() const       {return imType0Elec;}
    virtual int maxInt() const      {return 512;}
    virtual double maxVolts() const {return 0.6;}

    virtual double unityToVolts( double u ) const
        {return 1.2*u - 0.6;}

    virtual bool operator==( const IMROTbl &rhs ) const
        {return type==rhs.type && e == ((const IMROTbl_T0*)&rhs)->e;}
    virtual bool operator!=( const IMROTbl &rhs ) const
        {return !(*this == rhs);}

    virtual bool isConnectedSame( const IMROTbl *rhs ) const;

    virtual QString toString() const;
    virtual void fromString( const QString &s );

    virtual bool loadFile( QString &msg, const QString &path );
    virtual bool saveFile( QString &msg, const QString &path ) const;

    virtual int shnk( int /* ch */ ) const  {return 0;}
    virtual int bank( int ch ) const        {return e[ch].bank;}
    virtual int elShankAndBank( int &bank, int ch ) const;
    virtual int elShankColRow( int &col, int &row, int ch ) const;
    virtual void eaChansOrder( QVector<int> &v ) const;
    virtual int refid( int ch ) const       {return e[ch].refid;}
    virtual int refTypeAndFields( int &shank, int &bank, int ch ) const;
    virtual int apGain( int ch ) const      {return e[ch].apgn;}
    virtual int lfGain( int ch ) const      {return e[ch].lfgn;}
    virtual int apFlt( int ch ) const       {return e[ch].apflt;}

    virtual bool chIsRef( int ch ) const;
    virtual int idxToGain( int idx ) const;
    virtual int gainToIdx( int gain ) const;

    virtual void muxTable( int &nADC, int &nChn, std::vector<int> &T ) const;
};

#endif  // IMROTBL_T0_H



