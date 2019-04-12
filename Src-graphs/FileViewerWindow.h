#ifndef FILEVIEWERWINDOW_H
#define FILEVIEWERWINDOW_H

#include <QMainWindow>
#include <QBitArray>

class FileViewerWindow;
class FVToolbar;
class FVScanGrp;
class DataFile;
struct ShankMap;
struct ChanMap;
class MGraphY;
class MGScroll;
class Biquad;
class ExportCtl;
class TaggableLabel;

/* ---------------------------------------------------------------- */
/* Types ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

struct FVOpen {
    FileViewerWindow*   fvw;
    QString             runName;

    FVOpen()
    :   fvw(0)                  {}
    FVOpen( FileViewerWindow *fvw, const QString &s )
    :   fvw(fvw), runName(s)    {}
};


class FileViewerWindow : public QMainWindow
{
    Q_OBJECT

    friend class FVScanGrp;

private:
    struct SaveAll {
        double  fArrowKey,
                fPageKey,
                xSpan,
                ySclAux;
        int     yPix,
                nDivs;
        bool    sortUserOrder,
                manualUpdate;

        SaveAll() : fArrowKey(0.1), fPageKey(0.5)   {}
    };

    struct SaveIm {
        double  ySclAp,
                ySclLf;
        int     sAveSel,    // {0=Off, 1=Local, 2=Global}
                binMax;
        bool    bp300Hz,
                dcChkOnAp,
                dcChkOnLf;
    };

    struct SaveNi {
        double  ySclNeu;
        int     sAveSel,    // {0=Off, 1=Local, 2=Global}
                binMax;
        bool    bp300Hz,
                dcChkOn;
    };

    struct SaveSet {
        SaveAll all;
        SaveIm  im;
        SaveNi  ni;
    };

    struct GraphParams {
        // Copiable to other graphs of same type
        double  gain;

        GraphParams() : gain(1.0)   {}
    };

    class DCAve {
    private:
        int                 nC,
                            nN;
    public:
        std::vector<int>    lvl;
    public:
        void init( int nChannels, int nNeural );
        void updateLvl(
            const DataFile  *df,
            qint64          xpos,
            qint64          nRem,
            qint64          chunk,
            int             dwnSmp );
        void apply(
            qint16          *d,
            int             ntpts,
            int             dwnSmp );
    };

    FVToolbar               *tbar;
    FVScanGrp               *scanGrp;
    SaveSet                 sav;
    DCAve                   dc;
    QString                 cmChanStr;
    double                  tMouseOver,
                            yMouseOver;
    qint64                  dfCount,
                            dragAnchor,
                            dragL,              // or -1
                            dragR,
                            savedDragL,         // zoom: temp save sel
                            savedDragR;
    DataFile                *df;
    ShankMap                *shankMap;
    ChanMap                 *chanMap;
    Biquad                  *hipass;
    ExportCtl               *exportCtl;
    QMenu                   *channelsMenu;
    MGScroll                *mscroll;
    QAction                 *linkAction;
    TaggableLabel           *closeLbl;
    QTimer                  *hideCloseTimer;
    std::vector<MGraphY>    grfY;
    std::vector<GraphParams>grfParams;          // per-graph params
    std::vector<QMenu*>     chanSubMenus;
    std::vector<QAction*>   grfActShowHide;
    QVector<int>            order2ig,           // sort order
                            ig2ic,              // saved to acquired
                            ic2ig;              // acq to saved or -1
    QBitArray               grfVisBits;
    std::vector<std::vector<int> >  TSM;
    int                     fType,              // {0=imap, 1=imlf, 2=ni}
                            igSelected,         // if >= 0
                            igMaximized,        // if >= 0
                            igMouseOver,        // if >= 0
                            nSpikeChans,
                            nNeurChans;
    bool                    didLayout,
                            selDrag,
                            zoomDrag;

    static std::vector<FVOpen>  vOpen;
    static QSet<QString>        linkedRuns;

public:
    FileViewerWindow();
    virtual ~FileViewerWindow();

    bool viewFile( const QString &fname, QString *errMsg );

    // Return currently open (.bin) path or null
    QString file() const;

// Toolbar
    double tbGetfileSecs() const;
    double tbGetxSpanSecs() const   {return sav.all.xSpan;}
    double tbGetyScl() const
        {
            switch( fType ) {
                case 0:  return sav.im.ySclAp;
                case 1:  return sav.im.ySclLf;
                default: return sav.ni.ySclNeu;
            }
        }
    int     tbGetyPix() const       {return sav.all.yPix;}
    int     tbGetNDivs() const      {return sav.all.nDivs;}
    int     tbGetSAveSel() const
        {
            switch( fType ) {
                case 0:  return sav.im.sAveSel;
                case 1:  return 0;
                default: return sav.ni.sAveSel;
            }
        }
    bool    tbGet300HzOn() const
        {
            switch( fType ) {
                case 0:  return sav.im.bp300Hz;
                case 2:  return sav.ni.bp300Hz;
                default: return false;
            }
        }
    bool    tbGetDCChkOn() const
        {
            switch( fType ) {
                case 0:  return sav.im.dcChkOnAp;
                case 1:  return sav.im.dcChkOnLf;
                default: return sav.ni.dcChkOn;
            }
        }
    int     tbGetBinMax() const
        {
            switch( fType ) {
                case 0:  return sav.im.binMax;
                case 1:  return 0;
                default: return sav.ni.binMax;
            }
        }

// Export
    void getInverseGains(
        std::vector<double> &invGain,
        const QBitArray     &exportBits ) const;

public slots:
// Toolbar
    void tbToggleSort();
    void tbScrollToSelected();
    void tbSetXScale( double d );
    void tbSetYPix( int n );
    void tbSetYScale( double d );
    void tbSetMuxGain( double d );
    void tbSetNDivs( int n );
    void tbHipassClicked( bool b );
    void tbDcClicked( bool b );
    void tbSAveSelChanged( int sel );
    void tbBinMaxChanged( int n );
    void tbApplyAll();

// FVW_MapDialog
    void cmDefaultBut();
    void cmMetaBut();
    void cmApplyBut();

private slots:
// Menu
    void file_Link();
    void file_Export();
    void file_ChanMap();
    void file_ZoomIn();
    void file_ZoomOut();
    void file_Options();
    void file_Notes();
    void channels_ShowAll();
    void channels_HideAll();
    void channels_Edit();
    void help_ShowHelp();

// CloseLabel
    void hideCloseLabel();
    void hideCloseTimeout();

// Context menu
    void shankmap_Tog();
    void shankmap_Edit();
    void shankmap_Restore();

// Mouse
    void mouseOverGraph( double x, double y, int iy );
    void clickGraph( double x, double y, int iy );
    void dragDone();
    void dblClickGraph( double x, double y, int iy );
    void mouseOverLabel( int x, int y, int iy );

// Actions
    void menuShowHideGraph();
    void cursorHere( QPoint p );
    void clickedCloseLbl();

// Timer targets
    void layoutGraphs();

// Stream linking
    void linkRecvPos( double t0, double tSpan, int fChanged );
    void linkRecvSel( double tL, double tR );
    void linkRecvManualUpdate( bool manualUpdate );

protected:
    virtual bool eventFilter( QObject *obj, QEvent *e );
    virtual void closeEvent( QCloseEvent *e );

    void linkMenuChanged( bool linked );

private:
// Data-independent inits
    void initMenus();
    void initContextMenu();
    void initCloseLbl();
    void initDataIndepStuff();

// Data-dependent inits
    bool openFile( const QString &fname, QString *errMsg );
    void initHipass();
    void killActions();
    void initGraphs();

    void loadSettings();
    void saveSettings() const;

    qint64 nScansPerGraph() const;
    void updateNDivText();

    double scalePlotValue( double v );

    QString nameGraph( int ig ) const;
    void hideGraph( int ig );
    void showGraph( int ig );
    void selectGraph( int ig, bool updateGraph = true );
    void toggleMaximized();
    void sAveTable( int sel );
    int sAveApplyLocal( const qint16 *d_ig, int ig );
    void sAveApplyGlobal(
        qint16  *d,
        int     ntpts,
        int     nC,
        int     nAP,
        int     dwnSmp );
    void sAveApplyGlobalStride(
        qint16  *d,
        int     ntpts,
        int     nC,
        int     nAP,
        int     stride,
        int     dwnSmp );
    void updateXSel();
    void zoomTime();
    void updateGraphs();

    void printStatusMessage();
    bool queryCloseOK();

// Stream linking
    FVOpen* linkFindMe();
    bool linkIsLinked( const FVOpen *me );
    bool linkIsSameRun( const FVOpen *W, const FVOpen *me );
    bool linkIsSibling( const FVOpen *W, const FVOpen *me );
    int linkNSameRun( const FVOpen *me );
    bool linkOpenName( const QString &name, QPoint &corner );
    void linkAddMe( const QString &runName );
    void linkRemoveMe();
    void linkSetLinked( FVOpen *me, bool linked );
    void linkSendPos( int fChanged );
    void linkSendSel();
    void linkSendManualUpdate( bool manualUpdate );
};

#endif  // FILEVIEWERWINDOW_H


