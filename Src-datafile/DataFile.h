#ifndef DATAFILE_H
#define DATAFILE_H

#include "DAQ.h"
#include "KVParams.h"

#define SHA1_HAS_TCHAR
#include "SHA1.h"

#include <QFile>
#include <QMutex>

class DFWriter;

/* ---------------------------------------------------------------- */
/* Types ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

// virtual base class
//
class DataFile
{
    friend class DFWriterWorker;
    friend class DFCloseAsyncWorker;

private:
    enum IOMode {
        Undefined,
        Input,
        Output
    };

    // Input and Output mode
    QFile                   binFile;
    QString                 metaName;
    quint64                 scanCt;
    IOMode                  mode;

    // Input mode
    QString                 trgStream;
    int                     trgChan;    // neg if not using

    // Output mode only
    mutable QMutex          statsMtx;
    mutable QVector<uint>   statsBytes;
    CSHA1                   sha;
    DFWriter                *dfw;
    int                     nMeasMax;
    bool                    wrAsync;

protected:
    // Input and Output mode
    KVParams                kvp;
    QVector<uint>           chanIds;    // orig (acq) ids
    VRange                  _vRange;
    double                  sRate;
    int                     nSavedChans;

public:
    DataFile();
    virtual ~DataFile();

    // ----------
    // Open/close
    // ----------

    bool openForRead( const QString &filename, QString &error );
    bool openForWrite(
        const DAQ::Params   &p,
        const QString       &binName,
        bool                bForceName );

    // Special purpose method for FileViewerWindow exporter.
    // Data from preexisting 'other' file are copied to 'filename'.
    // 'idxOtherChans' are chanIds[] indices, not array elements.
    // For example, if other contains channels: {0,1,2,3,6,7,8},
    // export the last three by setting idxOtherChans = {4,5,6}.

    bool openForExport(
        const DataFile      &other,
        const QString       &filename,
        const QVector<uint> &idxOtherChans );

    bool isOpen() const         {return binFile.isOpen();}
    bool isOpenForRead() const  {return isOpen() && mode == Input;}
    bool isOpenForWrite() const {return isOpen() && mode == Output;}

    virtual QString subtypeFromObj() const = 0;
    virtual QString streamFromObj() const = 0;
    virtual QString fileLblFromObj() const = 0;

    QString binFileName() const         {return binFile.fileName();}
    const QString &metaFileName() const {return metaName;}

    bool closeAndFinalize();

    DataFile *closeAsync( const KeyValMap &kvm );

    // ------
    // Output
    // ------

    void setAsyncWriting( bool async )  {wrAsync = async;}

    bool writeAndInvalScans( vec_i16 &scans );
    bool writeAndInvalSubset( const DAQ::Params &p, vec_i16 &scans );

    // -----
    // Input
    // -----

    // Read scans from file (after openForRead()).
    // Return number of scans actually read or -1 on failure.
    // If num2read > available, available count is used.

    qint64 readScans(
        vec_i16         &dst,
        quint64         scan0,
        quint64         num2read,
        const QBitArray &keepBits ) const;

    // ---------
    // Meta data
    // ---------

    void setFirstSample( quint64 firstCt );
    void setParam( const QString &name, const QVariant &value );
    void setRemoteParams( const KeyValMap &kvm );

    QString notes() const;
    quint64 firstCt() const;
    quint64 scanCount() const               {return scanCt;}
    double samplingRateHz() const           {return sRate;}
    double fileTimeSecs() const             {return scanCt/sRate;}
    const VRange &vRange() const            {return _vRange;}
    int numChans() const                    {return nSavedChans;}
    const QVector<uint> &channelIDs() const {return chanIds;}
    bool isTrigChan( int acqChan ) const    {return acqChan == trgChan;}

    virtual const int *cumTypCnt() const = 0;
    virtual double origID2Gain( int ic ) const = 0;
    virtual ChanMap* chanMap() const = 0;
    virtual ShankMap* shankMap() const = 0;

    const QVariant getParam( const QString &name ) const;

    // ----
    // SHA1
    // ----

    static bool verifySHA1( const QString &filename );

    // ----------------------
    // Performance monitoring
    // ----------------------

    double percentFull() const;
    double writtenBytes() const;
    double requiredBps() const  {return sRate*nSavedChans*sizeof(qint16);}

protected:
    virtual void subclassParseMetaData() = 0;
    virtual void subclassStoreMetaData( const DAQ::Params &p ) = 0;
    virtual int subclassGetAcqChanCount( const DAQ::Params &p ) = 0;
    virtual int subclassGetSavChanCount( const DAQ::Params &p ) = 0;

    virtual void subclassSetSNSChanCounts(
        const DAQ::Params   *p,
        const DataFile      *dfSrc ) = 0;

    virtual void subclassUpdateShankMap(
        const DataFile      &other,
        const QVector<uint> &idxOtherChans ) = 0;

    virtual void subclassUpdateChanMap(
        const DataFile      &other,
        const QVector<uint> &idxOtherChans ) = 0;

private:
    bool doFileWrite( const vec_i16 &scans );
};

#endif  // DATAFILE_H


