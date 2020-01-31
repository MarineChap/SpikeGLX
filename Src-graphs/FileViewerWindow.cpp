
#include "ui_ChanListDialog.h"
#include "ui_FVW_MapDialog.h"
#include "ui_FVW_OptionsDialog.h"
#include "ui_FVW_NotesDialog.h"

#include "Pixmaps/close_but_16px.xpm"

#include "Util.h"
#include "MainApp.h"
#include "ConsoleWindow.h"
#include "FileViewerWindow.h"
#include "FVToolbar.h"
#include "FVScanGrp.h"
#include "DataFileIMAP.h"
#include "DataFileIMLF.h"
#include "DataFileNI.h"
#include "DFName.h"
#include "MGraph.h"
#include "Biquad.h"
#include "ExportCtl.h"
#include "ClickableLabel.h"
#include "Subset.h"
#include "Version.h"

#include <QKeyEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QStatusBar>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QPixmap>
#include <QCursor>
#include <QSettings>
#include <QMessageBox>

#include <math.h>

/* ---------------------------------------------------------------- */
/* class TaggableLabel -------------------------------------------- */
/* ---------------------------------------------------------------- */

class TaggableLabel : public ClickableLabel
{
private:
    int mtag;
public:
    TaggableLabel( QWidget *parent, Qt::WindowFlags f = 0 )
    :   ClickableLabel( parent, f ), mtag(-1)   {}
    void noTag()            {mtag = -1;}
    void setTag( int tag )  {mtag = tag;}
    int tag() const         {return mtag;}
};

/* ---------------------------------------------------------------- */
/* class DCAve ---------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::DCAve::init( int nChannels, int nNeural )
{
    nC  = nChannels;
    nN  = nNeural;
    lvl.assign( nN, 0 );
}


void FileViewerWindow::DCAve::updateLvl(
    const DataFile  *df,
    qint64          xpos,
    qint64          nRem,
    qint64          chunk,
    int             dwnSmp )
{
    if( nN <= 0 )
        return;

    std::vector<float>  sum( nN, 0.0F );

    int     *L      = &lvl[0];
    float   *S      = &sum[0];
    int     dStep   = nC * dwnSmp,
            nSamp   = 0;

    for(;;) {

        if( nRem <= 0 )
            break;

        // read this block

        vec_i16 data;
        qint64  nthis = qMin( chunk, nRem );
        int     ntpts;

        ntpts = df->readScans( data, xpos, nthis, QBitArray() );

        if( ntpts <= 0 )
            break;

        // update counting

        xpos    += ntpts;
        nRem    -= ntpts;
        nSamp   += (ntpts + dwnSmp - 1) / dwnSmp;

        // accumulate sums

        const qint16    *d = &data[0];

        for( int it = 0; it < ntpts; it += dwnSmp, d += dStep ) {

            for( int ig = 0; ig < nN; ++ig )
                S[ig] += d[ig];
        }
    }

    if( nSamp ) {

        for( int ig = 0; ig < nN; ++ig )
            L[ig] = S[ig]/nSamp;
    }
}


void FileViewerWindow::DCAve::apply(
    qint16          *d,
    int             ntpts,
    int             dwnSmp )
{
    if( nN <= 0 )
        return;

    int *L      = &lvl[0];
    int dStep   = nC * dwnSmp;

    for( int it = 0; it < ntpts; it += dwnSmp, d += dStep ) {

        for( int ig = 0; ig < nN; ++ig )
            d[ig] -= L[ig];
    }
}

/* ---------------------------------------------------------------- */
/* Statics -------------------------------------------------------- */
/* ---------------------------------------------------------------- */

std::vector<FVOpen> FileViewerWindow::vOpen;
QSet<QString>       FileViewerWindow::linkedRuns;

#define MAXCHANPERMENU  200

/* ---------------------------------------------------------------- */
/* FileViewerWindow ----------------------------------------------- */
/* ---------------------------------------------------------------- */

FileViewerWindow::FileViewerWindow()
    :   QMainWindow(0), tMouseOver(-1.0), yMouseOver(-1.0),
        df(0), shankMap(0), chanMap(0), hipass(0),
        igSelected(-1), igMaximized(-1), igMouseOver(-1),
        didLayout(false), selDrag(false), zoomDrag(false)
{
    initDataIndepStuff();

    setAttribute( Qt::WA_DeleteOnClose, false );
    show();
}


FileViewerWindow::~FileViewerWindow()
{
    if( df )
        delete df;

    if( shankMap )
        delete shankMap;

    if( chanMap )
        delete chanMap;

    if( hipass )
        delete hipass;
}


bool FileViewerWindow::viewFile( const QString &fname, QString *errMsg )
{
    if( igMaximized != -1 )
        toggleMaximized();

    igSelected      = -1;
    dragL           = -1;
    dragR           = -1;
    tMouseOver      = -1.0;
    yMouseOver      = -1.0;
    igMouseOver     = -1;

// --------
// Get data
// --------

    if( !openFile( fname, errMsg ) )
        return false;

    loadSettings();

// --------------------
// Data-dependent inits
// --------------------

    addToolBar( tbar = new FVToolbar( this, fType ) );
    scanGrp->setRanges( true );
    scanGrp->enableManualUpdate( sav.all.manualUpdate );
    initHipass();

// --------------------------
// Manage previous array data
// --------------------------

    killActions();

// ---------------------------
// Resize arrays for this file
// ---------------------------

    int nG = df->numChans();

    grfY.resize( nG );
    grfParams.resize( nG );
    grfActShowHide.resize( nG );
    order2ig.resize( nG );
    ig2ic.resize( nG );

// -------------------
// Init new array data
// -------------------

    ic2ig.fill( -1, df->channelIDs()[nG-1] + 1 );

    grfVisBits.fill( true, nG );

    initGraphs();

    sAveTable( tbGetSAveSel() );

// ------------
// Adjust menus
// ------------

    if( !nSpikeChans || !shankMap ) {

        QList<QAction*> la = mscroll->theM->actions();

        foreach( QAction* a, la ) {
            if( a->text().contains( "ShankMap", Qt::CaseInsensitive ) )
                a->setEnabled( false );
        }
    }

// ---------------
// Initialize view
// ---------------

    selectGraph( 0, false );

    sav.all.sortUserOrder = !sav.all.sortUserOrder;
    tbToggleSort();

    return true;
}


QString FileViewerWindow::file() const
{
    if( df && df->isOpen() )
        return df->binFileName();

    return QString::null;
}


double FileViewerWindow::tbGetfileSecs() const
{
    return (df ? df->fileTimeSecs() : 0);
}


void FileViewerWindow::getInverseGains(
    std::vector<double> &invGain,
    const QBitArray     &exportBits ) const
{
    invGain.clear();

    for( int i = 0, nC = exportBits.size(); i < nC; ++i ) {

        if( exportBits.testBit( i ) )
            invGain.push_back( 1.0 / grfParams[i].gain );
    }
}

/* ---------------------------------------------------------------- */
/* Toolbar -------------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::tbToggleSort()
{
    sav.all.sortUserOrder = !sav.all.sortUserOrder;
    saveSettings();

    if( sav.all.sortUserOrder ) {
        tbar->setSortButText( "Usr Order" );
        chanMap->userOrder( order2ig );
    }
    else {
        tbar->setSortButText( "Acq Order" );
        chanMap->defaultOrder( order2ig );
    }

    layoutGraphs();
}


void FileViewerWindow::tbScrollToSelected()
{
    mscroll->scrollToSelected();
}


void FileViewerWindow::tbSetXScale( double d )
{
    sav.all.xSpan = d;
    saveSettings();

    updateNDivText();

    scanGrp->setRanges( false );
    scanGrp->guiSetPos( scanGrp->curPos() );

    if( isActiveWindow() )
        linkSendPos( 2 );
}


// BK: Spinboxes invoking updateGraphs show strange behavior:
// If window showing a lot of data so update is slow, then
// clicks in spinner arrows cause two steps of value change.
// This doesn't happen if activating arrows by keyboard. It
// doesn't help to defer drawing with a queued call. Using
// a timer can help if the delay is a second or more. Very
// annoying.
//
void FileViewerWindow::tbSetYPix( int n )
{
    double grafsOffTop = double(mscroll->theX->clipTop)
                            / mscroll->theX->ypxPerGrf;

    sav.all.yPix = n;
    saveSettings();

    mscroll->theX->ypxPerGrf = n;

    layoutGraphs();
    mscroll->scrollTo( grafsOffTop * n );
}


void FileViewerWindow::tbSetYScale( double d )
{
    if( igSelected < 0 )
        return;

    grfY[igSelected].yscl = d;

    updateNDivText();
    mscroll->theM->update();
}


void FileViewerWindow::tbSetMuxGain( double d )
{
    if( igSelected < 0 )
        return;

    grfParams[igSelected].gain = d;

    updateNDivText();
    printStatusMessage();
}


void FileViewerWindow::tbSetNDivs( int n )
{
    sav.all.nDivs = n;
    saveSettings();

    updateNDivText();
    mscroll->theX->setVGridLines( sav.all.nDivs );
    mscroll->theM->update();
}


void FileViewerWindow::tbHipassClicked( bool b )
{
    if( fType == 0 )
        sav.im.bp300Hz = b;
    else if( fType == 2 )
        sav.ni.bp300Hz = b;

    saveSettings();

    updateGraphs();
}


void FileViewerWindow::tbDcClicked( bool b )
{
    if( fType == 0 )
        sav.im.dcChkOnAp = b;
    else if( fType == 1 )
        sav.im.dcChkOnLf = b;
    else
        sav.ni.dcChkOn   = b;

    saveSettings();

    updateGraphs();
}


void FileViewerWindow::tbSAveSelChanged( int sel )
{
    if( fType < 2 )
        sav.im.sAveSel = sel;
    else
        sav.ni.sAveSel = sel;

    saveSettings();

    sAveTable( sel );
    updateGraphs();
}


void FileViewerWindow::tbBinMaxChanged( int n )
{
    if( fType < 2 )
        sav.im.binMax = n;
    else
        sav.ni.binMax = n;

    saveSettings();

    updateGraphs();
}


void FileViewerWindow::tbApplyAll()
{
    if( igSelected < 0 )
        return;

    GraphParams &P      = grfParams[igSelected];
    double      yScale  = grfY[igSelected].yscl;
    int         type    = grfY[igSelected].usrType;

    if( type == 0 ) {

        switch( fType ) {
            case 0:  sav.im.ySclAp  = yScale; break;
            case 1:  sav.im.ySclLf  = yScale; break;
            default: sav.ni.ySclNeu = yScale;
        }
    }
    else if( type == 1 )
        sav.all.ySclAux = yScale;

    if( type < 2 )
        saveSettings();

    for( int ig = 0, nG = grfParams.size(); ig < nG; ++ig ) {

        if( ig != igSelected && grfY[ig].usrType == type ) {

            grfY[ig].yscl   = yScale;
            grfParams[ig]   = P;
        }
    }

    if( didLayout )
        updateGraphs();
}

/* ---------------------------------------------------------------- */
/* FVW_MapDialog -------------------------------------------------- */
/* ---------------------------------------------------------------- */

static Ui::FVW_MapDialog*   mapUI;


void FileViewerWindow::cmDefaultBut()
{
    int ne = chanMap->e.size();

    for( int i = 0; i < ne; ++i )
        chanMap->e[i].order = i;

    chanMap->userOrder( order2ig );
    layoutGraphs();
}


void FileViewerWindow::cmMetaBut()
{
    if( chanMap )
        delete chanMap;

    chanMap = df->chanMap();

    chanMap->userOrder( order2ig );
    layoutGraphs();
}


void FileViewerWindow::cmApplyBut()
{
// Make mapping from user channel name to chanMap entry index
// Make mapping from table order to entry index

    QMap<int,int>   nam2Idx;
    QMap<int,int>   ord2Idx;
    QRegExp         re(";(\\d+)");
    int             ne = chanMap->e.size();

    for( int i = 0; i < ne; ++i ) {

        chanMap->e[i].name.contains( re );
        nam2Idx[re.cap(1).toInt()]      = i;
        ord2Idx[chanMap->e[i].order]    = i;
    }

// Initialize new order array with -1
// Mark all entry indices initially unused

    std::vector<int>    newo( ne, -1 );
    std::vector<bool>   used( ne, false );

// Parse user list and assign named chans to newo array

    QMap<int,int>::iterator it;
    int onext = 0;

    QString     s       = mapUI->listTE->toPlainText();
    QStringList terms   = s.split(
                            QRegExp("[,;]"),
                            QString::SkipEmptyParts );

    foreach( const QString &t, terms ) {

        QStringList rng = t.split(
                            QRegExp("[:-]"),
                            QString::SkipEmptyParts );
        int         n   = rng.count(),
                    r1, r2, id;
        bool        ok1, ok2;

        if( n > 2 ) {
            QMessageBox::critical(
                this,
                "Format Error",
                QString("Bad format: %1").arg( t ) );
            return;
        }

        if( n == 2 ) {

            r1  = rng[0].toUInt( &ok1 ),
            r2  = rng[1].toUInt( &ok2 );

            if( !ok1 || !ok2 )
                continue;

            if( r1 == r2 )
                goto justR1;

            if( r1 < r2 ) {

                for( int r = r1; r <= r2; ++r ) {

                    it = nam2Idx.find( r );

                    if( it == nam2Idx.end() )
                        continue;

                    id = it.value();

                    if( used[id] )
                        continue;

                    newo[id] = onext++;
                    used[id] = true;
                }
            }
            else {

                for( int r = r1; r >= r2; --r ) {

                    it = nam2Idx.find( r );

                    if( it == nam2Idx.end() )
                        continue;

                    id = it.value();

                    if( used[id] )
                        continue;

                    newo[id] = onext++;
                    used[id] = true;
                }
            }
        }
        else if( n == 1 ) {

justR1:
            int r1 = rng[0].toUInt( &ok1 );

            if( !ok1 )
                continue;

            it = nam2Idx.find( r1 );

            if( it == nam2Idx.end() )
                continue;

            id = it.value();

            if( used[id] )
                continue;

            newo[id] = onext++;
            used[id] = true;
        }
    }

// Walk all entries in order, appending if unused

    for( it = ord2Idx.begin(); it != ord2Idx.end(); ++it ) {

        int id = it.value();

        if( !used[id] ) {
            newo[id] = onext++;
            used[id] = true;
        }
    }

// Update

    for( int i = 0; i < ne; ++i )
        chanMap->e[i].order = newo[i];

    chanMap->userOrder( order2ig );
    layoutGraphs();
}

/* ---------------------------------------------------------------- */
/* Menu items ----------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::file_Link()
{
    FVOpen  *me = linkFindMe();

    if( !me )
        return;

// ----------------
// Turn linking off
// ----------------

    if( linkIsLinked( me ) ) {
        linkSetLinked( me, false );
        return;
    }

// ---------------
// Turn linking on
// ---------------

// If I'm only one, open all siblings

    if( linkNSameRun( me ) <= 1 ) {

        QString base    = DFName::chopType( df->binFileName() );
        QPoint  corner  = pos();

        if( fType != 0 ) {

            linkOpenName(
                QString("%1.imec%2.ap.bin")
                .arg( base ).arg( df->probeNum() ),
                corner );
        }

        if( fType != 1 ) {

            linkOpenName(
                QString("%1.imec%2.lf.bin")
                .arg( base ).arg( df->probeNum() ),
                corner );
        }

        if( fType != 2 )
            linkOpenName( base + ".nidq.bin", corner );
    }

// Update (me) after editing vOpen

    me = linkFindMe();

    if( linkNSameRun( me ) > 1 ) {

        linkSetLinked( me, true );
        linkSendManualUpdate( false );
        guiBreathe();
        linkSendPos( 3 );
        linkSendSel();
        guiBreathe();
        linkSendManualUpdate( sav.all.manualUpdate );
    }
}


void FileViewerWindow::file_Export()
{
// communicate settings across sessions/windows
    STDSETTINGS( settings, "fileviewer" );
    exportCtl->loadSettings( settings );

    exportCtl->initDataFile( df );
    exportCtl->initGrfRange( grfVisBits, igSelected );
    exportCtl->initTimeRange( dragL, dragR );
    exportCtl->showExportDlg( this );
}


void FileViewerWindow::file_ChanMap()
{
// Show effects

    if( !sav.all.sortUserOrder )
        tbToggleSort();

// Make a backup chanMap

    ChanMap *cmBack;

    if( chanMap->type() == "nidq" )
        cmBack  = new ChanMapNI( *dynamic_cast<ChanMapNI*>(chanMap) );
    else
        cmBack  = new ChanMapIM( *dynamic_cast<ChanMapIM*>(chanMap) );

// Dialog setup

    QDialog             dlg;
    Ui::FVW_MapDialog   ui;

    dlg.setWindowFlags( dlg.windowFlags()
        & (~Qt::WindowContextHelpButtonHint
            | Qt::WindowCloseButtonHint) );

    mapUI = &ui;

    ui.setupUi( &dlg );
    ui.listTE->appendPlainText( cmChanStr );
    ConnectUI( ui.defaultBut, SIGNAL(clicked()), this, SLOT(cmDefaultBut()) );
    ConnectUI( ui.metaBut, SIGNAL(clicked()), this, SLOT(cmMetaBut()) );
    ConnectUI( ui.applyBut, SIGNAL(clicked()), this, SLOT(cmApplyBut()) );

// Run dialog

    if( QDialog::Accepted == dlg.exec() ) {
    }
    else {

        if( *chanMap != *cmBack ) {

            delete chanMap;
            chanMap = cmBack;
            cmBack  = 0;

            chanMap->userOrder( order2ig );
            layoutGraphs();
        }
    }

// Cleanup

    cmChanStr = ui.listTE->toPlainText();

    if( cmBack )
        delete cmBack;
}


void FileViewerWindow::file_ZoomIn()
{
    double  spn = sav.all.xSpan;
    qint64  pos = scanGrp->curPos(),
            mid = pos + scanGrp->posFromTime( spn/2 );

    spn = qMax( 0.0001, spn/2 );
    pos = qMax( 0LL, mid - scanGrp->posFromTime( spn/2 ) );

    linkRecvPos( scanGrp->timeFromPos( pos ), spn, 3 );
    linkSendPos( 2 );
}


void FileViewerWindow::file_ZoomOut()
{
    double  spn = sav.all.xSpan;
    qint64  pos = scanGrp->curPos(),
            mid = pos + scanGrp->posFromTime( spn/2 );

    spn = qMin( qMin( 30.0, tbGetfileSecs() ), 2*spn );
    pos = qMax( 0LL, mid - scanGrp->posFromTime( spn/2 ) );

    linkRecvPos( scanGrp->timeFromPos( pos ), spn, 3 );
    linkSendPos( 2 );
}


void FileViewerWindow::file_Options()
{
    QDialog                 dlg;
    Ui::FVW_OptionsDialog   ui;

    dlg.setWindowFlags( dlg.windowFlags()
        & (~Qt::WindowContextHelpButtonHint
            | Qt::WindowCloseButtonHint) );

    ui.setupUi( &dlg );
    ui.arrowSB->setValue( sav.all.fArrowKey );
    ui.pageSB->setValue( sav.all.fPageKey );
    ui.manualChk->setChecked( sav.all.manualUpdate );

    if( QDialog::Accepted == dlg.exec() ) {

        sav.all.fArrowKey       = ui.arrowSB->value();
        sav.all.fPageKey        = ui.pageSB->value();
        sav.all.manualUpdate    = ui.manualChk->isChecked();
        saveSettings();

        scanGrp->enableManualUpdate( sav.all.manualUpdate );
        linkSendManualUpdate( sav.all.manualUpdate );
    }
}


void FileViewerWindow::file_Notes()
{
    QDialog             dlg;
    Ui::FVW_NotesDialog ui;

    dlg.setWindowFlags( dlg.windowFlags()
        & (~Qt::WindowContextHelpButtonHint
            | Qt::WindowCloseButtonHint) );

    ui.setupUi( &dlg );

    QString notes = df->notes();

    if( notes.isEmpty() )
        ui.notesTE->setText( "-- No user notes --" );
    else
        ui.notesTE->setText( notes );

    dlg.exec();
}


void FileViewerWindow::channels_ShowAll()
{
    hideCloseLabel();

    igMaximized = -1;

    int nG = grfY.size();

    grfVisBits.fill( true, nG );

    for( int ig = 0; ig < nG; ++ig )
        grfActShowHide[ig]->setChecked( true );

    if( igSelected == -1 )
        selectGraph( 0, false );

    mscroll->theX->ypxPerGrf = sav.all.yPix;

    layoutGraphs();
}


void FileViewerWindow::channels_HideAll()
{
    hideCloseLabel();

    igMaximized = -1;

    int nG = grfY.size();

    grfVisBits.fill( false, nG );

    for( int ig = 0; ig < nG; ++ig )
        grfActShowHide[ig]->setChecked( false );

    selectGraph( -1, false );

    layoutGraphs();
}


void FileViewerWindow::channels_Edit()
{
    QDialog             dlg;
    Ui::ChanListDialog  ui;

    dlg.setWindowFlags( dlg.windowFlags()
        & (~Qt::WindowContextHelpButtonHint
            | Qt::WindowCloseButtonHint) );

    ui.setupUi( &dlg );
    dlg.setWindowTitle( "Show These Channels" );

// List shown channels

    QVector<uint>   chans;
    int             nG = grfY.size();

    for( int ig = 0; ig < nG; ++ig ) {

        if( grfVisBits.testBit( ig ) )
            chans.push_back( ig2ic[ig] );
    }

    QString s = Subset::vec2RngStr( chans );

    ui.curLbl->setText( s );
    ui.chansLE->setText( s );

// Run dialog

    if( QDialog::Accepted == dlg.exec() ) {

        hideCloseLabel();

        igMaximized = -1;

        // Translate string to bits

        if( Subset::isAllChansStr( ui.chansLE->text() ) )
            Subset::defaultBits( grfVisBits, nG );
        else {

            QVector<uint>   chans;

            if( !Subset::rngStr2Vec( chans, ui.chansLE->text() ) ) {

                QMessageBox::critical(
                    this,
                    "Channels Error",
                    "Graphs list has bad format, view not changed." );
                return;
            }

            grfVisBits.fill( false, nG );

            int nC = chans.size();

            for( int ic = 0; ic < nC; ++ic ) {

                // IMPORTANT:
                // User chan label not necessarily within span of
                // true channels, hence, span of ic2ig[], so don't
                // use ic2ig for this lookup.

                int ig = ig2ic.indexOf( chans[ic] );

                if( ig >= 0 )
                    grfVisBits.setBit( ig );
            }
        }

        // Adjust menu items

        for( int ig = 0; ig < nG; ++ig )
            grfActShowHide[ig]->setChecked( grfVisBits.testBit( ig ) );

        // Select lowest graph or -1

        if( igSelected == -1 || !grfVisBits.testBit( igSelected ) ) {

            int lowest = -1;

            for( int ig = 0; ig < nG; ++ig ) {

                if( grfVisBits.testBit( ig ) ) {
                    lowest = ig;
                    break;
                }
            }

            selectGraph( lowest, false );
        }
        else if( !grfVisBits.count( true ) )
            selectGraph( -1, false );

        mscroll->theX->ypxPerGrf = sav.all.yPix;

        layoutGraphs();
    }
}


void FileViewerWindow::help_ShowHelp()
{
    showHelp( "FileViewer_Help" );
}

/* ---------------------------------------------------------------- */
/* CloseLabel ----------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::hideCloseLabel()
{
    closeLbl->hide();
    closeLbl->noTag();
    hideCloseTimer->stop();
}


void FileViewerWindow::hideCloseTimeout()
{
// underMouse() very unreliable
//    if( !closeLbl->underMouse() )
//        hideCloseLabel();

    if( !closeLbl->geometry().contains( QCursor::pos() ) )
        hideCloseLabel();
}

/* ---------------------------------------------------------------- */
/* Context menu --------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::shankmap_Tog()
{
    if( shankMap && shankMap->e.size() > igMouseOver ) {

        shankMap->e[igMouseOver].u = !shankMap->e[igMouseOver].u;
        updateGraphs();
    }
}


void FileViewerWindow::shankmap_Edit()
{
    if( !shankMap )
        return;

    QDialog             dlg;
    Ui::ChanListDialog  ui;

    dlg.setWindowFlags( dlg.windowFlags()
        & (~Qt::WindowContextHelpButtonHint
            | Qt::WindowCloseButtonHint) );

    ui.setupUi( &dlg );
    dlg.setWindowTitle( "Turn Off in ShankMap" );

// List OFF channels

    QVector<uint>   chans;

    for( int ig = 0; ig < nSpikeChans; ++ig ) {

        if( !shankMap->e[ig].u )
            chans.push_back( ig2ic[ig] );
    }

    QString s = Subset::vec2RngStr( chans );

    ui.curLbl->setText( s );
    ui.chansLE->setText( s );

// Run dialog

    if( QDialog::Accepted == dlg.exec() ) {

        // Translate string

        QVector<uint>   chans;

        if( !Subset::rngStr2Vec( chans, ui.chansLE->text() ) ) {

            QMessageBox::critical(
                this,
                "Channels Error",
                "Channel list has bad format, view not changed." );
            return;
        }

        // Loop

        int nC = chans.size();

        for( int ic = 0; ic < nC; ++ic ) {

            // IMPORTANT:
            // User chan label not necessarily within span of
            // true channels, hence, span of ic2ig[], so don't
            // use ic2ig for this lookup.

            int ig = ig2ic.indexOf( chans[ic] );

            if( ig >= 0 )
                shankMap->e[ig].u = 0;
        }

        updateGraphs();
    }
}


void FileViewerWindow::shankmap_Restore()
{
    if( shankMap ) {

        delete shankMap;
        shankMap = df->shankMap();
        updateGraphs();
    }
}

/* ---------------------------------------------------------------- */
/* Mouse ---------------------------------------------------------- */
/* ---------------------------------------------------------------- */

// Position readout and selection tracking.
//
// x in range [0,1].
// y in range [-1,1].
//
void FileViewerWindow::mouseOverGraph( double x, double y, int iy )
{
// ------------
// Which graph?
// ------------

    igMouseOver = mscroll->theX->Y[iy]->usrChan;

    if( igMouseOver < 0 || igMouseOver >= (int)grfY.size() ) {
        statusBar()->clearMessage();
        return;
    }

// ----------------
// Position readout
// ----------------

    tMouseOver = scanGrp->curTime() + x * sav.all.xSpan;
    yMouseOver = scalePlotValue( y );

// ------------------
// Selection tracking
// ------------------

// Note:
// -----
// Each graph sets tracking true (GLGraph::attach) so we get
// mouseMove messages from the first click through the final
// mouse-up, all from the same graph, by the way.

    if( selDrag || zoomDrag ) {

        qint64  pos = scanGrp->curPos(),
                p   = qBound(
                        0LL,
                        scanGrp->posFromTime( tMouseOver ),
                        dfCount - 1 );
        bool    moved = false;

        if( p < pos )
            moved = scanGrp->guiSetPos( p );
        else if( p > pos + nScansPerGraph() )
            moved = scanGrp->guiSetPos( qMax( 0LL, p - nScansPerGraph() ) );

        if( p >= dragAnchor ) {
            dragL   = dragAnchor;
            dragR   = p;
        }
        else {
            dragL   = p;
            dragR   = dragAnchor;
        }

        if( moved )
            updateGraphs();
        else
            updateXSel();

        linkSendSel();
    }

    printStatusMessage();
}


void FileViewerWindow::clickGraph( double x, double y, int iy )
{
    mouseOverGraph( x, y, iy );
    selectGraph( igMouseOver );

// Initiate either a selection drag for export (shift-click), or,
// a zoom selection to change the time view (ctrl-click). In the
// zoom case, any existing selection range is temporarily saved
// and then reinstated after zooming completes.

    selDrag  = QApplication::keyboardModifiers() & Qt::ShiftModifier;
    zoomDrag = QApplication::keyboardModifiers() & Qt::ControlModifier;

    if( zoomDrag ) {

        selDrag     = false;    // one purpose at a time
        savedDragL  = dragL;
        savedDragR  = dragR;
    }

    if( selDrag || zoomDrag ) {

        dragAnchor  =
        dragL       =
        dragR       = scanGrp->curPos() + x * nScansPerGraph();

        updateXSel();
    }
}


void FileViewerWindow::dragDone()
{
    if( selDrag || zoomDrag ) {

        // First adjust time view

        if( zoomDrag ) {

            zoomTime();

            dragL = savedDragL;
            dragR = savedDragR;
        }

        // Now adjust selection

        if( dragR <= dragL )
            dragL = dragR = -1;

        updateXSel();
        linkSendSel();

        selDrag  = false;
        zoomDrag = false;

        printStatusMessage();
    }
}


void FileViewerWindow::dblClickGraph( double x, double y, int iy )
{
    clickGraph( x, y, iy );
    toggleMaximized();
}


void FileViewerWindow::mouseOverLabel( int x, int y, int iy )
{
    if( selDrag )
        return;

    if( !didLayout || igMaximized > -1 )
        return;

    int nV = grfVisBits.count( true );

    if( !nV )
        return;

    QSize   sz      = closeLbl->size();
    int     hLbl    = sz.height(),
            xLbl    = mscroll->viewport()->width() - sz.width(),
            yGrf    = (nV > 1 ? sav.all.yPix : mscroll->viewport()->height()),
            yLbl    = (yGrf - hLbl) / 2;

    if( x > xLbl && y > yLbl && y < yLbl + hLbl ) {

        hideCloseTimer->stop();

        QPoint  p( xLbl, yLbl + iy*sav.all.yPix - mscroll->theX->clipTop );
        p = mscroll->theM->mapToGlobal( p );

        int ig = mscroll->theX->Y[iy]->usrChan;

        closeLbl->setTag( ig );
        closeLbl->setToolTip( grfY[ig].lhsLabel );
        closeLbl->move( p.x(), p.y() );
        closeLbl->show();

        hideCloseTimer->start();
    }
}

/* ---------------------------------------------------------------- */
/* Actions -------------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::menuShowHideGraph()
{
    QAction *s = dynamic_cast<QAction*>(sender());

    if( !s )
        return;

    bool    ok = false;
    int     ig = s->objectName().toInt( &ok );

    if( !ok )
        return;

    if( ig >= 0 && ig < grfY.size() ) {

        if( grfVisBits.testBit( ig ) )
            hideGraph( ig );
        else
            showGraph( ig );
    }
}


void FileViewerWindow::cursorHere( QPoint p )
{
    QCursor::setPos( p );
}


void FileViewerWindow::clickedCloseLbl()
{
    int tag = closeLbl->tag();

    if( tag >= 0 )
        hideGraph( tag );

    hideCloseLabel();

// -------------------------
// Force mouse move event(s)
// -------------------------

// This sequence causes status data to update after a graph
// is closed, yet the cursor stays still overall. Moreover,
// this let's the user hover over the close label and click
// a run of graphs closed. Unfortunately, setPos( curpos )
// is effectively a noop, or we'd just do that.

    // Here's where we are
    QPoint  p = QCursor::pos();

    // Immed: Jog to the left (cause a move event)
    QCursor::setPos( p.x() - 1, p.y() );

    // After: Jog back again (cause another move event)
    QMetaObject::invokeMethod(
        this, "cursorHere",
        Qt::QueuedConnection,
        Q_ARG(QPoint, p) );
}

/* ---------------------------------------------------------------- */
/* Timer targets -------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::layoutGraphs()
{
    didLayout = false;

    hideCloseLabel();

// ---------------------------------------
// Point to visible graphs in sorted order
// ---------------------------------------

    MGraphX *theX = mscroll->theX;

    theX->Y.clear();

    if( igMaximized > -1 )
        theX->Y.push_back( &grfY[igMaximized] );
    else {
        for( int is = 0, nG = grfY.size(); is < nG; ++is ) {

            int ig = order2ig[is];

            if( grfVisBits.testBit( ig ) )
                theX->Y.push_back( &grfY[ig] );
        }
    }

// ------
// Layout
// ------

    mscroll->adjustLayout();
    theX->setYSelByUsrChan( igSelected );
    didLayout = true;
    updateGraphs();
}

/* ---------------------------------------------------------------- */
/* Stream linking ------------------------------------------------- */
/* ---------------------------------------------------------------- */

// Change-flags: {1=pos, 2=span, 3=both}
//
void FileViewerWindow::linkRecvPos( double t0, double tSpan, int fChanged )
{
    if( fChanged & 2 ) {

        sav.all.xSpan =
            qBound( 0.0001, tSpan, qMin( 30.0, tbGetfileSecs() ) );
        saveSettings();

        tbar->setXScale( sav.all.xSpan );
        updateNDivText();

        if( !(fChanged & 1) )
            updateGraphs();
    }

    scanGrp->setRanges( false );
    scanGrp->guiSetPos( scanGrp->posFromTime( t0 ) );
}


void FileViewerWindow::linkRecvSel( double tL, double tR )
{
    if( tR > tL ) {

        dragR = qBound( 0LL, scanGrp->posFromTime( tR ), dfCount - 1 );
        dragL = qBound( 0LL, scanGrp->posFromTime( tL ), dragR );
    }
    else
        dragL = dragR = -1;

    updateXSel();
}


void FileViewerWindow::linkRecvManualUpdate( bool manualUpdate )
{
    sav.all.manualUpdate = manualUpdate;
    scanGrp->enableManualUpdate( manualUpdate );
}

/* ---------------------------------------------------------------- */
/* Protected ------------------------------------------------------ */
/* ---------------------------------------------------------------- */

bool FileViewerWindow::eventFilter( QObject *obj, QEvent *e )
{
    if( (obj == mscroll || obj == scanGrp->getSliderObj())
        && e->type() == QEvent::KeyPress ) {

        QKeyEvent   *keyEvent   = static_cast<QKeyEvent*>(e);
        double      newPos      = -1.0; // illegal
        qint64      pos         = scanGrp->curPos();

        switch( keyEvent->key() ) {

            case Qt::Key_Home:
                newPos = 0;
                break;
            case Qt::Key_End:
                newPos = scanGrp->maxPos();
                break;
            case Qt::Key_Left:
            case Qt::Key_Up:
                newPos =
                    qMax( 0.0, pos - sav.all.fArrowKey * nScansPerGraph() );
                break;
            case Qt::Key_Right:
            case Qt::Key_Down:
                newPos = pos + sav.all.fArrowKey * nScansPerGraph();
                break;
            case Qt::Key_PageUp:
                newPos =
                    qMax( 0.0, pos - sav.all.fPageKey * nScansPerGraph() );
                break;
            case Qt::Key_PageDown:
                newPos = pos + sav.all.fPageKey * nScansPerGraph();
                break;
        }

        if( newPos >= 0.0 )
            return scanGrp->guiSetPos( newPos );
    }

    return QMainWindow::eventFilter( obj, e );
}


void FileViewerWindow::closeEvent( QCloseEvent *e )
{
    if( queryCloseOK() ) {

        QWidget::closeEvent( e );

        if( e->isAccepted() ) {
            linkRemoveMe();
            mainApp()->modelessClosed( this );
            deleteLater();
        }
    }
}


void FileViewerWindow::linkMenuChanged( bool linked )
{
    linkAction->setText( linked ? "Un&link" : "&Link" );
}

/* ---------------------------------------------------------------- */
/* Data-independent inits ----------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::initMenus()
{
//#ifdef Q_OS_MACX
//    // shared menu bar on OSX
//  QMenuBar *mb = mainApp()->console()->menuBar();
//#else
//    // otherwise window-specific menu bar
    QMenuBar    *mb = menuBar();
//#endif

    QMenu   *m;

    m = mb->addMenu( "&File" );
    linkAction = m->addAction( "&Link", this, SLOT(file_Link()), QKeySequence( tr("Ctrl+L") ) );
    m->addAction( "&Export...", this, SLOT(file_Export()), QKeySequence( tr("Ctrl+E") ) );
    m->addSeparator();
    m->addAction( "&Channel Mapping...", this, SLOT(file_ChanMap()), QKeySequence( tr("Ctrl+M") ) );
    m->addAction( "Zoom &In...", this, SLOT(file_ZoomIn()), QKeySequence( tr("Ctrl++") ) );
    m->addAction( "Zoom &Out...", this, SLOT(file_ZoomOut()), QKeySequence( tr("Ctrl+-") ) );
    m->addAction( "&Time Scrolling...", this, SLOT(file_Options()) );
    m->addSeparator();
    m->addAction( "&View Notes", this, SLOT(file_Notes()) );

    m = mb->addMenu( "&Channels" );
    m->addAction( "&Show All", this, SLOT(channels_ShowAll()) );
    m->addAction( "&Hide All", this, SLOT(channels_HideAll()) );
    m->addAction( "&Edit List...", this, SLOT(channels_Edit()) );
    m->addSeparator();
    channelsMenu = m;

    m = mb->addMenu( "&Help" );
    m->addAction( "File Viewer &Help", this, SLOT(help_ShowHelp()), QKeySequence( tr("Ctrl+H") ) );
}


void FileViewerWindow::initContextMenu()
{
    MGraph  *theM = mscroll->theM;
    QAction *A;

    A = new QAction( "ShankMap: Toggle This Chan", this );
    ConnectUI( A, SIGNAL(triggered()), this, SLOT(shankmap_Tog()) );
    theM->addAction( A );

    A = new QAction( "ShankMap: Turn Off List...", this );
    ConnectUI( A, SIGNAL(triggered()), this, SLOT(shankmap_Edit()) );
    theM->addAction( A );

    A = new QAction( "ShankMap: Restore Original", this );
    ConnectUI( A, SIGNAL(triggered()), this, SLOT(shankmap_Restore()) );
    theM->addAction( A );

    A = new QAction( this );
    A->setSeparator( true );
    theM->addAction( A );

    A = new QAction( "Export...", this );
    ConnectUI( A, SIGNAL(triggered()), this, SLOT(file_Export()) );
    theM->addAction( A );

    theM->setContextMenuPolicy( Qt::ActionsContextMenu );
}


void FileViewerWindow::initCloseLbl()
{
    QPixmap pm( close_but_16px );

// BK: In Qt 5.3, setting Qt::Popup for a child window issues a warning
// that the flag is for top-level windows...but it behaves perfectly in
// all Qt versions.

    closeLbl = new TaggableLabel(
                    mscroll->theM,
                    Qt::Popup | Qt::FramelessWindowHint );
    closeLbl->setPixmap( pm );
    closeLbl->resize( pm.size() );
    closeLbl->setCursor( Qt::ArrowCursor );
    closeLbl->hide();
    closeLbl->move( -100, -100 );
    ConnectUI( closeLbl, SIGNAL(clicked()), this, SLOT(clickedCloseLbl()) );

    hideCloseTimer = new QTimer( this );
    hideCloseTimer->setInterval( 250 );
    ConnectUI( hideCloseTimer, SIGNAL(timeout()), this, SLOT(hideCloseTimeout()) );
}


void FileViewerWindow::initDataIndepStuff()
{
    setCursor( Qt::ArrowCursor );
    resize( 1060, 640 );

// Top-most controls
// Toolbar added after file type known

    initMenus();

// CentralWidget gets vertical layout of two items:
// - graphs
// - 'slider' (position controls)

    QWidget *cw = new QWidget;
    setCentralWidget( cw );

    mscroll = new MGScroll( "fvw", cw );
    mscroll->installEventFilter( this );

    QVBoxLayout *VL = new QVBoxLayout( cw );
    VL->addWidget( mscroll, 1 );
    VL->addWidget( scanGrp = new FVScanGrp( this ) );

// Status bar below

    statusBar();

// Additional once-only

    initContextMenu();

    MGraph  *theM = mscroll->theM;
    theM->setImmedUpdate( true );
    ConnectUI( theM, SIGNAL(cursorOver(double,double,int)), this, SLOT(mouseOverGraph(double,double,int)) );
    ConnectUI( theM, SIGNAL(lbutClicked(double,double,int)), this, SLOT(clickGraph(double,double,int)) );
    ConnectUI( theM, SIGNAL(lbutReleased()), this, SLOT(dragDone()) );
    ConnectUI( theM, SIGNAL(lbutDoubleClicked(double,double,int)), this, SLOT(dblClickGraph(double,double,int)) );
    ConnectUI( theM, SIGNAL(cursorOverWindowCoords(int,int,int)), this, SLOT(mouseOverLabel(int,int,int)) );

    initCloseLbl();

    exportCtl = new ExportCtl( this );
}

/* ---------------------------------------------------------------- */
/* Data-dependent inits ------------------------------------------- */
/* ---------------------------------------------------------------- */

bool FileViewerWindow::openFile( const QString &fname, QString *errMsg )
{
    if( errMsg )
        errMsg->clear();

// -------------------------
// Decode stream type and IP
// -------------------------

    QString fname_no_path = QFileInfo( fname ).fileName(),
            error;
    int     ip;

    fType = DFName::typeAndIP( ip, fname_no_path, errMsg );

    if( fType < 0 )
        return false;

// ----------------------------------
// Create new file of correct type/IP
// ----------------------------------

    if( df )
        delete df;

    switch( fType ) {
        case 0:  df = new DataFileIMAP( ip ); break;
        case 1:  df = new DataFileIMLF( ip ); break;
        default: df = new DataFileNI;
    }

// ----------------------------
// Open and read key data items
// ----------------------------

    if( !df->openForRead( fname, error ) ) {

        if( errMsg )
            *errMsg = error;

        Error() << error;
        return false;
    }

    if( !(dfCount = df->scanCount()) ) {

        error = QString("File empty '%1'.").arg( fname_no_path );

        if( errMsg )
            *errMsg = error;

        Error() << error;
        return false;
    }

    if( shankMap )
        delete shankMap;

    shankMap = df->shankMap();

    if( chanMap )
        delete chanMap;

    chanMap = df->chanMap();

    if( !chanMap->e.size() ) {

        error =
        QString("No channel map in meta data '%1'.").arg( fname_no_path );

        if( errMsg )
            *errMsg = error;

        Error() << error;
        return false;
    }

    double  srate   = df->samplingRateHz(),
            t0      = df->firstCt() / srate,
            dt      = dfCount / srate;

    setWindowTitle(
        QString(APPNAME " File Viewer: %1 [%2 chans @ %3 Hz] (t0, dt)=(%4, %5)")
        .arg( fname_no_path )
        .arg( df->numChans() )
        .arg( srate )
        .arg( t0, 0, 'f', 3 )
        .arg( dt, 0, 'f', 3 ) );

    mainApp()->modelessOpened( this );
    linkAddMe( fname_no_path );

    return true;
}


void FileViewerWindow::initHipass()
{
    if( hipass )
        delete hipass;

    hipass =
    new Biquad( bq_type_highpass, 300.0 / df->samplingRateHz() );
}


void FileViewerWindow::killActions()
{
// Remove submenus referencing actions

    for( int im = 0, nM = chanSubMenus.size(); im < nM; ++im ) {

        QAction *a = chanSubMenus[im]->menuAction();
        channelsMenu->removeAction( a );
    }

    chanSubMenus.clear();

// Delete the actions

    for( int ig = 0, nG = grfY.size(); ig < nG; ++ig )
        delete grfActShowHide[ig];
}


void FileViewerWindow::initGraphs()
{
    MGraphX *theX           = mscroll->theX;
    QMenu   *subMenu        = 0;
    int     nG              = grfY.size(),
            igNewSubMenu    = 0;

    mscroll->scrollTo( 0 );

    // std neural
    theX->yColor.resize( 1 );

    // add lfp color if imec
    if( fType < 2 )
        theX->yColor.push_back( QColor( 0xff, 0x55, 0x00 ) );

    // add aux color
    theX->yColor.push_back( QColor( 0x44, 0xee, 0xff ) );

    theX->setVGridLines( sav.all.nDivs );
    theX->Y.clear();
    theX->ypxPerGrf     = sav.all.yPix;
    theX->drawCursor    = false;

    nSpikeChans = 0;
    nNeurChans  = 0;

    for( int ig = 0; ig < nG; ++ig ) {

        QAction     *a;
        int         &C  = ig2ic[ig];
        MGraphY     &Y  = grfY[ig];
        GraphParams &P  = grfParams[ig];

        C           = df->channelIDs()[ig];
        ic2ig[C]    = ig;

        switch( fType ) {
            case 0:
                Y.usrType   = ((DataFileIMAP*)df)->origID2Type( C );
                Y.yscl      = (!Y.usrType ? sav.im.ySclAp : sav.all.ySclAux);

                if( Y.usrType == 0 ) {
                    ++nSpikeChans;
                    ++nNeurChans;
                }
            break;
            case 1:
                Y.usrType   = ((DataFileIMLF*)df)->origID2Type( C );
                Y.yscl      = (!Y.usrType ? sav.im.ySclLf : sav.all.ySclAux);

                if( Y.usrType == 1 )
                    ++nNeurChans;
            break;
            default:
                Y.usrType   = ((DataFileNI*)df)->origID2Type( C );
                Y.yscl      = (!Y.usrType ? sav.ni.ySclNeu : sav.all.ySclAux);

                if( Y.usrType == 0 ) {
                    ++nSpikeChans;
                    ++nNeurChans;
                }
        }

        if( Y.usrType == 2 )
            Y.yscl = 1;

        Y.lhsLabel      = nameGraph( ig );
        Y.usrChan       = ig;
        Y.iclr          = (Y.usrType < 2 ? Y.usrType : 1);
        Y.drawBinMax    = false;
        Y.isDigType     = Y.usrType == 2;

        P.gain          = df->origID2Gain( C );

        a = new QAction( QString::number( C ), this );
        a->setObjectName( QString::number( ig ) );
        a->setCheckable( true );
        a->setChecked( true );
        ConnectUI( a, SIGNAL(triggered()), this, SLOT(menuShowHideGraph()) );

        // Create new subMenu?

        if( ig == igNewSubMenu ) {

            igNewSubMenu += MAXCHANPERMENU;

            int iend = qMin( igNewSubMenu, nG ) - 1;

            subMenu = channelsMenu->addMenu(
                        QString("Channels %1 - %2")
                        .arg( C )
                        .arg( df->channelIDs()[iend] ) );

            chanSubMenus.push_back( subMenu );
        }

        subMenu->addAction( a );

        grfActShowHide[ig]  = a;
        order2ig[ig]        = ig; // default is acqsort
    }
}

/* ---------------------------------------------------------------- */
/* Private -------------------------------------------------------- */
/* ---------------------------------------------------------------- */

void FileViewerWindow::loadSettings()
{
    STDSETTINGS( settings, "fileviewer" );

// ---
// All
// ---

    settings.beginGroup( "FileViewer_All" );
    sav.all.fArrowKey   = settings.value( "fArrowKey", 0.1 ).toDouble();
    sav.all.fPageKey    = settings.value( "fPageKey", 0.5 ).toDouble();
    sav.all.xSpan       = settings.value( "xSpan", 4.0 ).toDouble();
    sav.all.ySclAux     = settings.value( "ySclAux", 1.0 ).toDouble();
    sav.all.yPix        = settings.value( "yPix", 100 ).toInt();
    sav.all.nDivs       = settings.value( "nDivs", 4 ).toInt();
    sav.all.sortUserOrder   = settings.value( "sortUserOrder", false ).toBool();
    sav.all.manualUpdate    = settings.value( "manualUpdate", false ).toBool();
    settings.endGroup();

    if( fabs( sav.all.fArrowKey ) < 0.0001 )
        sav.all.fArrowKey = 0.1;

    if( fabs( sav.all.fPageKey ) < 0.0001 )
        sav.all.fPageKey = 0.5;

    sav.all.xSpan   = qMin( sav.all.xSpan, df->fileTimeSecs() );
    sav.all.yPix    = qMax( sav.all.yPix, 4 );
    sav.all.nDivs   = qMax( sav.all.nDivs, 1 );

// ----
// Imec
// ----

    settings.beginGroup( "FileViewer_Imec" );
    sav.im.ySclAp       = settings.value( "ySclAp", 1.0 ).toDouble();
    sav.im.ySclLf       = settings.value( "ySclLf", 1.0 ).toDouble();
    sav.im.sAveSel      = settings.value( "sAveSel", 0 ).toInt();
    sav.im.binMax       = settings.value( "binMax", 0 ).toInt();
    sav.im.bp300Hz      = settings.value( "bp300Hz", false ).toBool();
    sav.im.dcChkOnAp    = settings.value( "dcChkOnAp", true ).toBool();
    sav.im.dcChkOnLf    = settings.value( "dcChkOnLf", true ).toBool();
    settings.endGroup();

// ----
// Nidq
// ----

    settings.beginGroup( "FileViewer_Nidq" );
    sav.ni.ySclNeu      = settings.value( "ySclNeu", 1.0 ).toDouble();
    sav.ni.sAveSel      = settings.value( "sAveSel", 0 ).toInt();
    sav.ni.binMax       = settings.value( "binMax", 0 ).toInt();
    sav.ni.bp300Hz      = settings.value( "bp300Hz", true ).toBool();
    sav.ni.dcChkOn      = settings.value( "dcChkOn", true ).toBool();
    settings.endGroup();

    exportCtl->loadSettings( settings );
}


void FileViewerWindow::saveSettings() const
{
    STDSETTINGS( settings, "fileviewer" );

// ---
// All
// ---

    settings.beginGroup( "FileViewer_All" );
    settings.setValue( "fArrowKey", sav.all.fArrowKey );
    settings.setValue( "fPageKey", sav.all.fPageKey );
    settings.setValue( "xSpan", sav.all.xSpan );
    settings.setValue( "ySclAux", sav.all.ySclAux );
    settings.setValue( "yPix", qMax( sav.all.yPix, 4 ) );
    settings.setValue( "nDivs", qMax( sav.all.nDivs, 1 ) );
    settings.setValue( "sortUserOrder", sav.all.sortUserOrder );
    settings.setValue( "manualUpdate", sav.all.manualUpdate );
    settings.endGroup();

// ---------
// By stream
// ---------

    if( fType < 2 ) {

        // ----
        // Imec
        // ----

        settings.beginGroup( "FileViewer_Imec" );

        if( fType == 0 ) {
            settings.setValue( "ySclAp", sav.im.ySclAp );
            settings.setValue( "sAveSel", sav.im.sAveSel );
            settings.setValue( "binMax", sav.im.binMax );
            settings.setValue( "bp300Hz", sav.im.bp300Hz );
            settings.setValue( "dcChkOnAp", sav.im.dcChkOnAp );
        }
        else {
            settings.setValue( "ySclLf", sav.im.ySclLf );
            settings.setValue( "dcChkOnLf", sav.im.dcChkOnLf );
        }

        settings.endGroup();
    }
    else {

        // ----
        // Nidq
        // ----

        settings.beginGroup( "FileViewer_Nidq" );
        settings.setValue( "ySclNeu", sav.ni.ySclNeu );
        settings.setValue( "sAveSel", sav.ni.sAveSel );
        settings.setValue( "binMax", sav.ni.binMax );
        settings.setValue( "bp300Hz", sav.ni.bp300Hz );
        settings.setValue( "dcChkOn", sav.ni.dcChkOn );
        settings.endGroup();
    }

    exportCtl->saveSettings( settings );
}


qint64 FileViewerWindow::nScansPerGraph() const
{
    return sav.all.xSpan * df->samplingRateHz();
}


void FileViewerWindow::updateNDivText()
{
    if( sav.all.nDivs > 0 ) {

        double  X = sav.all.xSpan / sav.all.nDivs;

        if( igSelected > -1 && grfY[igSelected].usrType < 2 ) {

            const char  *unit   = "V";
            double      gain    = grfParams[igSelected].gain,
                        Y       = df->vRange().span()
                                / (2 * gain * grfY[igSelected].yscl);

            if( Y < 0.001 ) {
                Y   *= 1e6;
                unit = "uV";
            }
            else if( Y < 1.0 ) {
                Y   *= 1e3;
                unit = "mV";
            }

            tbar->setNDivText( QString(" Boxes %1s x %2%3")
                                .arg( X, 0, 'f', 6 )
                                .arg( Y, 0, 'f', 3 )
                                .arg( unit ) );
        }
        else {
            tbar->setNDivText( QString(" Boxes %1s x -")
                                .arg( X, 0, 'f', 6 ) );
        }
    }
    else
        tbar->setNDivText( " Boxes - x -" );
}


// Values (v) are in range [-1,1].
// (v+1)/2 is in range [0,1].
// This is mapped to range [rmin,rmax].
//
double FileViewerWindow::scalePlotValue( double v )
{
    return df->vRange().unityToVolts( (v+1)/2 )
            / grfParams[igMouseOver].gain;
}


QString FileViewerWindow::nameGraph( int ig ) const
{
    if( ig < 0 || ig >= grfY.size() )
        return QString::null;

    return chanMap->name( ig, df->isTrigChan( ig2ic[ig] ) );
}


void FileViewerWindow::hideGraph( int ig )
{
    int nG = grfY.size();

    if( ig < 0 || ig >= nG )
        return;

// Mark it

    grfVisBits.clearBit( ig );
    grfActShowHide[ig]->setChecked( false );

// Adjust selection

    if( igSelected == ig ) {

        QVector<int>    ig2order( nG );

        for( int is = 0; is < nG; ++is )
            ig2order[order2ig[is]] = is;

        // try select next higher
        for( int is = ig2order[ig] + 1; is < nG; ++is ) {

            int ig2 = order2ig[is];

            if( grfVisBits.testBit( ig2 ) ) {

                selectGraph( ig2, false );
                goto doLayout;
            }
        }

        // try select next lower
        for( int is = ig2order[ig] - 1; is >= 0; --is ) {

            int ig2 = order2ig[is];

            if( grfVisBits.testBit( ig2 ) ) {

                selectGraph( ig2, false );
                goto doLayout;
            }
        }

        // select nothing
        selectGraph( -1, false );
    }

doLayout:
    layoutGraphs();
}


void FileViewerWindow::showGraph( int ig )
{
    if( ig < 0 || ig >= grfY.size() )
        return;

    grfVisBits.setBit( ig );
    grfActShowHide[ig]->setChecked( true );

    int nV = grfVisBits.count( true );

    if( nV == 1 )
        selectGraph( ig, false );
    else if( nV == 2 )
        mscroll->theX->ypxPerGrf = sav.all.yPix;

    layoutGraphs();
}


void FileViewerWindow::selectGraph( int ig, bool updateGraph )
{
    if( igSelected == ig )
        return;

    igSelected = ig;

    if( updateGraph ) {
        mscroll->theX->setYSelByUsrChan( ig );
        mscroll->theM->update();
    }

    if( ig >= 0 ) {

        tbar->setSelName( grfY[ig].lhsLabel );

        tbar->setYSclAndGain(
                grfY[ig].yscl,
                grfParams[ig].gain,
                grfY[ig].usrType < 2 );
    }
    else {

        tbar->setSelName( "None" );
        tbar->setYSclAndGain( 1, 1, false );
    }

    updateNDivText();
}


void FileViewerWindow::toggleMaximized()
{
    hideCloseLabel();

    MGraphX *theX = mscroll->theX;

    if( igMaximized == -1 ) {

        igMaximized     = igSelected;
        theX->clipTop   = 0;
        theX->ypxPerGrf = mscroll->theM->height();
    }
    else {
        igMaximized     = -1;
        theX->ypxPerGrf = sav.all.yPix;
    }

    channelsMenu->setEnabled( igMaximized == -1 );
    tbar->enableYPix( igMaximized == -1 );

    layoutGraphs();
    mscroll->scrollToSelected();
}


// For each channel [0,nSpikeChan), calculate an 8-way
// neighborhood of indices into a timepoint's channels.
// - Annulus with {inner, outer} radii {self, 2} or {2, 8}.
// - The list is sorted for cache friendliness.
//
// Sel: {0=Off; 1=Loc 1,2; 2=Loc 2,8; 3=Glb All, 4=Glb Dmx}.
//
void FileViewerWindow::sAveTable( int sel )
{
    TSM.clear();

    if( !(sel == 1 || sel == 2) || nSpikeChans <= 0 )
        return;

    TSM.resize( nSpikeChans );

    QMap<ShankMapDesc,uint> ISM;
    shankMap->inverseMap( ISM );

    int rIn, rOut;

    if( sel == 1 ) {
        rIn     = 0;
        rOut    = 2;
    }
    else if( sel == 2 ) {
        rIn     = 2;
        rOut    = 8;
    }

    for( int ig = 0; ig < nSpikeChans; ++ig ) {

        const ShankMapDesc  &E = shankMap->e[ig];

        if( !E.u )
            continue;

        // ----------------------------------
        // Form map of excluded inner indices
        // ----------------------------------

        QMap<int,int>   inner;  // keys sorted, value is arbitrary

        int xL  = qMax( int(E.c)  - rIn, 0 ),
            xH  = qMin( uint(E.c) + rIn + 1, shankMap->nc ),
            yL  = qMax( int(E.r)  - rIn, 0 ),
            yH  = qMin( uint(E.r) + rIn + 1, shankMap->nr );

        for( int ix = xL; ix < xH; ++ix ) {

            for( int iy = yL; iy < yH; ++iy ) {

                QMap<ShankMapDesc,uint>::iterator   it;

                it = ISM.find( ShankMapDesc( E.s, ix, iy, 1 ) );

                if( it != ISM.end() )
                    inner[it.value()] = 1;
            }
        }

        // -------------------------
        // Fill with annulus members
        // -------------------------

        std::vector<int>    &V = TSM[ig];

        xL  = qMax( int(E.c)  - rOut, 0 );
        xH  = qMin( uint(E.c) + rOut + 1, shankMap->nc );
        yL  = qMax( int(E.r)  - rOut, 0 );
        yH  = qMin( uint(E.r) + rOut + 1, shankMap->nr );

        for( int ix = xL; ix < xH; ++ix ) {

            for( int iy = yL; iy < yH; ++iy ) {

                QMap<ShankMapDesc,uint>::iterator   it;

                it = ISM.find( ShankMapDesc( E.s, ix, iy, 1 ) );

                if( it != ISM.end() ) {

                    int i = it.value();

                    // Exclude inners

                    if( inner.find( i ) == inner.end() )
                        V.push_back( i );
                }
            }
        }

        qSort( V );
    }
}


// Space averaging for value: d_ig = &data[ig].
//
int FileViewerWindow::sAveApplyLocal( const qint16 *d_ig, int ig )
{
    const std::vector<int>  &V = TSM[ig];

    int nv = V.size();

    if( nv ) {

        const qint16    *d  = d_ig - ig;
        const int       *v  = &V[0];
        int             sum = 0;

        for( int iv = 0; iv < nv; ++iv )
            sum += d[v[iv]];

        return *d_ig - sum/nv;
    }

    return *d_ig;
}


// Space averaging for all values.
//
#if 0
// ----------------
// Per-shank method
// ----------------
void FileViewerWindow::sAveApplyGlobal(
    qint16  *d,
    int     ntpts,
    int     nC,
    int     nAP,
    int     dwnSmp )
{
    if( nAP <= 0 )
        return;

    const ShankMapDesc  *E = &shankMap->e[0];

    int                 ns      = shankMap->ns,
                        dStep   = nC * dwnSmp;
    std::vector<int>    _A( ns ),
                        _N( ns );
    std::vector<float>  _S( ns );
    int                 *A  = &_A[0],
                        *N  = &_N[0];
    float               *S  = &_S[0];

    for( int it = 0; it < ntpts; it += dwnSmp, d += dStep ) {

        for( int is = 0; is < ns; ++is ) {
            S[is] = 0;
            N[is] = 0;
            A[is] = 0;
        }

        for( int ig = 0; ig < nAP; ++ig ) {

            const ShankMapDesc  *e = &E[ig];

            if( e->u ) {
                S[e->s] += d[ig];
                ++N[e->s];
            }
        }

        for( int is = 0; is < ns; ++is ) {

            if( N[is] )
                A[is] = S[is] / N[is];
        }

        for( int ig = 0; ig < nAP; ++ig )
            d[ig] -= A[E[ig].s];
    }
}
#else
// ------------------
// Whole-probe method
// ------------------
void FileViewerWindow::sAveApplyGlobal(
    qint16  *d,
    int     ntpts,
    int     nC,
    int     nAP,
    int     dwnSmp )
{
    if( nAP <= 0 )
        return;

    const ShankMapDesc  *E = &shankMap->e[0];

    int dStep = nC * dwnSmp;

    for( int it = 0; it < ntpts; it += dwnSmp, d += dStep ) {

        double  S = 0;
        int     A = 0,
                N = 0;

        for( int ig = 0; ig < nAP; ++ig ) {

            const ShankMapDesc  *e = &E[ig];

            if( e->u ) {
                S += d[ig];
                ++N;
            }
        }

        if( N )
            A = S / N;

        for( int ig = 0; ig < nAP; ++ig )
            d[ig] -= A;
    }
}
#endif


// Space averaging for all values.
//
#if 0
// ----------------
// Per-shank method
// ----------------
void FileViewerWindow::sAveApplyGlobalStride(
    qint16  *d,
    int     ntpts,
    int     nC,
    int     nAP,
    int     stride,
    int     dwnSmp )
{
    if( nAP <= 0 )
        return;

    nAP = ig2ic[nAP-1];    // highest acquired channel saved

    const ShankMapDesc  *E = &shankMap->e[0];

    int                 ns      = shankMap->ns,
                        dStep   = nC * dwnSmp;
    std::vector<int>    _A( ns ),
                        _N( ns );
    std::vector<float>  _S( ns );
    int                 *A  = &_A[0],
                        *N  = &_N[0];
    float               *S  = &_S[0];

    for( int it = 0; it < ntpts; it += dwnSmp, d += dStep ) {

        for( int ic0 = 0; ic0 < stride; ++ic0 ) {

            for( int is = 0; is < ns; ++is ) {
                S[is] = 0;
                N[is] = 0;
                A[is] = 0;
            }

            for( int ic = ic0; ic <= nAP; ic += stride ) {

                int ig = ic2ig[ic];

                if( ig >= 0 ) {

                    const ShankMapDesc  *e = &E[ig];

                    if( e->u ) {
                        S[e->s] += d[ig];
                        ++N[e->s];
                    }
                }
            }

            for( int is = 0; is < ns; ++is ) {

                if( N[is] )
                    A[is] = S[is] / N[is];
            }

            for( int ic = ic0; ic <= nAP; ic += stride ) {

                int ig = ic2ig[ic];

                if( ig >= 0 )
                    d[ig] -= A[E[ig].s];
            }
        }
    }
}
#else
// ------------------
// Whole-probe method
// ------------------
void FileViewerWindow::sAveApplyGlobalStride(
    qint16  *d,
    int     ntpts,
    int     nC,
    int     nAP,
    int     stride,
    int     dwnSmp )
{
    if( nAP <= 0 )
        return;

    nAP = ig2ic[nAP-1];    // highest acquired channel saved

    const ShankMapDesc  *E = &shankMap->e[0];

    int dStep = nC * dwnSmp;

    for( int it = 0; it < ntpts; it += dwnSmp, d += dStep ) {

        for( int ic0 = 0; ic0 < stride; ++ic0 ) {

            double  S = 0;
            int     A = 0,
                    N = 0;

            for( int ic = ic0; ic <= nAP; ic += stride ) {

                int ig = ic2ig[ic];

                if( ig >= 0 ) {

                    const ShankMapDesc  *e = &E[ig];

                    if( e->u ) {
                        S += d[ig];
                        ++N;
                    }
                }
            }

            if( N )
                A = S / N;

            for( int ic = ic0; ic <= nAP; ic += stride ) {

                int ig = ic2ig[ic];

                if( ig >= 0 )
                    d[ig] -= A;
            }
        }
    }
}
#endif


void FileViewerWindow::updateXSel()
{
    MGraphX *theX = mscroll->theX;

    if( dragL >= 0
        && dragR > dragL
        && dragR < dfCount ) {

        // transform selection from scans to range [0..1].

        double  span    = nScansPerGraph();
        qint64  pos     = scanGrp->curPos();
        float   gselbeg = (dragL - pos) / span,
                gselend = (dragR - pos) / span;

        theX->setXSelEnabled( true );
        theX->setXSelRange( gselbeg, gselend );
    }
    else
        theX->setXSelEnabled( false );

    mscroll->theM->update();
}


void FileViewerWindow::zoomTime()
{
    if( dragR <= dragL )
        return;

    dragR = qBound( 0LL, dragR, dfCount - 1 );
    dragL = qBound( 0LL, dragL, dragR );

    double  spn = qMax( 0.0001, scanGrp->timeFromPos( dragR - dragL ) );

    linkRecvPos( scanGrp->timeFromPos( dragL ), spn, 3 );
    linkSendPos( 2 );
}

/* ---------------------------------------------------------------- */
/* updateGraphs --------------------------------------------------- */
/* ---------------------------------------------------------------- */

#define MAX10BIT    512
#define MAX16BIT    32768

#define V_S_AVE( d_ig )                                         \
    (sAveLocal ? sAveApplyLocal( d_ig, ig ) : *d_ig)


// Notes:
//
// - User has random access to file data, and if filter is enabled,
// the first xflt data points of any block would ordinarily show a
// transient artifact. Therefore, we will always load an extra xflt
// scans, process the larger block, and trim the lead portion off
// when passing ybuf data to graph.
//
// - The code is simpler if we load a timespan, then process the
// data channel-by-channel. That is, we go over all the timepoints
// again and again. However, if the span is large this causes lots
// of memory paging and that's very slow.
//
// - Rather, we treat a long span as several short chunks. We have to
// retain state data for filters and DC calcs across chunks.
//
void FileViewerWindow::updateGraphs()
{
// -------------
// Channel setup
// -------------

    float   ysc,
            srate   = df->samplingRateHz();
    int     maxInt  = (fType < 2 ? MAX10BIT : MAX16BIT),
            stride  = (fType < 2 ? 24 : df->getParam("niMuxFactor").toInt()),
            nG      = df->numChans(),
            nVis    = grfVisBits.count( true );

    ysc = 1.0F / maxInt;

    QVector<uint>   iv2ig;

    Subset::bits2Vec( iv2ig, grfVisBits );

// -----------
// Scans setup
// -----------

    qint64  pos         = scanGrp->curPos(),
            xpos, num2Read;
    int     xflt,
            dwnSmp,
            binMax;
    bool    sAveLocal   = false;

    if( tbGet300HzOn() )
        xflt = qMin( (qint64)BIQUAD_TRANS_WIDE, pos );
    else
        xflt = 0;

    xpos        = pos - xflt;
    num2Read    = xflt + ceil(sav.all.xSpan * srate);
    dwnSmp      = num2Read / (2 * mscroll->viewport()->width());

// Note: dwnSmp oversamples by 2X.

    if( dwnSmp < 1 )
        dwnSmp = 1;

    binMax = (dwnSmp > 1 ? tbGetBinMax() : 0);

// -----------
// Size graphs
// -----------

    if( xpos >= dfCount )
        return;

    qint64  ntpts   = qMin( num2Read, dfCount - xpos ),
            dtpts   = (ntpts + dwnSmp - 1) / dwnSmp,
            gtpts   = (ntpts - xflt + dwnSmp - 1) / dwnSmp,
            xoff    = dtpts - gtpts;

    if( gtpts <= 0 )
        return;

    for( int iv = 0; iv < nVis; ++iv )
        grfY[iv2ig[iv]].resize( gtpts );

    mscroll->theX->initVerts( gtpts );

// -----------------
// Pick a chunk size
// -----------------

// Nominally 20 millisecond's worth, but a multiple of dwnSmp.
// Smaller chunks reduce memory thrashing, but at the penalty
// of more indexing.

    qint64  chunk =
            qMax( 1, int((tbGet300HzOn() ? 0.05 : 0.02)*srate/dwnSmp) )
            * dwnSmp;

// ------------
// Filter setup
// ------------

    hipass->clearMem();

    // -<T>; not applied if hipass filtered

    if( tbGetDCChkOn() && !tbGet300HzOn() ) {

        dc.init( nG, nNeurChans );
        dc.updateLvl( df, xpos, ntpts, chunk, dwnSmp );
    }

// --------------
// Process chunks
// --------------

    qint64  nRem = ntpts;

    for(;;) {

        if( nRem <= 0 )
            break;

        // ---------------
        // Read this block
        // ---------------

        if( xoff && (chunk + dwnSmp - 1) / dwnSmp <= xoff )
            chunk = (xoff + 1) * dwnSmp;

        vec_i16 data;
        qint64  nthis = qMin( chunk, nRem );

        ntpts = df->readScans( data, xpos, nthis, QBitArray() );

        if( ntpts <= 0 )
            break;

        dtpts = (ntpts + dwnSmp - 1) / dwnSmp;

        if( dtpts <= xoff )
            break;

        // update counting

        xpos    += ntpts;
        nRem    -= ntpts;

        // --------------
        // Filters ------
        // --------------

        // --------
        // Bandpass
        // --------

        if( tbGet300HzOn() ) {
            hipass->applyBlockwiseMem(
                    &data[0], maxInt, ntpts, nG, 0, nSpikeChans );
        }

        // ------------------------------------
        // -<T>; not applied if hipass filtered
        // ------------------------------------

        if( tbGetDCChkOn() && !tbGet300HzOn() )
            dc.apply( &data[0], ntpts, (binMax ? binMax : dwnSmp) );

        // ----
        // -<S>
        // ----

        switch( tbGetSAveSel() ) {

            case 1:
            case 2:
                sAveLocal = true;
                break;
            case 3:
                sAveApplyGlobal(
                    &data[0], ntpts, nG, nSpikeChans,
                    (binMax ? binMax : dwnSmp) );
                break;
            case 4:
                sAveApplyGlobalStride(
                    &data[0], ntpts, nG, nSpikeChans,
                    stride, (binMax ? binMax : dwnSmp) );
                break;
            default:
                ;
        }

        // -------------
        // Result buffer
        // -------------

        std::vector<float>  ybuf( dtpts ),
                            ybuf2( binMax ? dtpts : 0 );

        // -------------------------
        // For each shown channel...
        // -------------------------

        for( int iv = 0; iv < nVis; ++iv ) {

            int     ig      = iv2ig[iv],
                    dstep   = dwnSmp * nG,
                    ny      = 0;
            qint16  *d      = &data[ig];

            if( grfY[ig].usrType == 0 ) {

                // ---------------
                // Neural channels
                // ---------------

                // ---------------
                // Skip references
                // ---------------

                if( shankMap && !shankMap->e[ig].u )
                    continue;

                // -------------------
                // Neural downsampling
                // -------------------

                // Within each bin, report both max and min
                // values. This ensures spikes aren't missed.
                // Max in ybuf, min in ybuf2.

                if( binMax ) {

                    int ndRem = ntpts;

                    grfY[ig].drawBinMax = true;

                    for(
                        int it = 0;
                        it < ntpts;
                        it += dwnSmp, d = &data[ig + it*nG] ) {

                        int val     = V_S_AVE( d ),
                            vmax    = val,
                            vmin    = val,
                            binWid  = dwnSmp;

                        d += binMax*nG;

                        if( ndRem < binWid )
                            binWid = ndRem;

                        for(
                            int ib = binMax;
                            ib < binWid;
                            ib += binMax, d += binMax*nG ) {

                            val = V_S_AVE( d );

                            if( val > vmax )
                                vmax = val;
                            else if( val < vmin )
                                vmin = val;
                        }

                        ndRem -= binWid;

                        ybuf[ny]  = vmax * ysc;
                        ybuf2[ny] = vmin * ysc;
                        ++ny;
                    }

                    grfY[ig].yval2.putData( &ybuf2[xoff], dtpts - xoff );
                }
                else if( sAveLocal ) {

                    grfY[ig].drawBinMax = false;

                    for( int it = 0; it < ntpts; it += dwnSmp, d += dstep )
                        ybuf[ny++] = sAveApplyLocal( d, ig ) * ysc;
                }
                else {
                    grfY[ig].drawBinMax = false;
                    goto draw_analog;
                }
            }
            else if( grfY[ig].usrType == 1 ) {

                // -----------------
                // Analog: LF or Aux
                // -----------------

draw_analog:
                for( int it = 0; it < ntpts; it += dwnSmp, d += dstep )
                    ybuf[ny++] = *d * ysc;
            }
            else {

                // -------
                // Digital
                // -------

                for( int it = 0; it < ntpts; it += dwnSmp, d += dstep )
                    ybuf[ny++] = *d;
            }

            // -------------
            // Copy to graph
            // -------------

            grfY[ig].yval.putData( &ybuf[xoff], dtpts - xoff );
        }

        xoff = 0;   // only first chunk includes offset

    }   // end chunks

// -----------------
// Select and redraw
// -----------------

    updateXSel();
}


void FileViewerWindow::printStatusMessage()
{
    int ig = igMouseOver;

    if( ig < 0 )
        return;

    double  t = tMouseOver,
            y = yMouseOver;
    QString msg;

    if( grfY[ig].usrType <= 1 ) {

        // ---------------
        // Analog channels
        // ---------------

        const char  *unit   = "V";
        double      gain    = grfParams[ig].gain,
                    Y       = df->vRange().span()
                            / (2 * gain * grfY[ig].yscl);

        if( Y < 0.001 ) {
            y   *= 1e6;
            unit = "uV";
        }
        else if( Y < 1.0 ) {
            y   *= 1e3;
            unit = "mV";
        }

        msg = QString("Mouse tracking Graph %1 @ pos (%2 s, %3 %4)")
                .arg( grfY[ig].lhsLabel )
                .arg( t, 0, 'f', 4 )
                .arg( y, 0, 'f', 4 )
                .arg( unit );
    }
    else {

        // -------
        // Digital
        // -------

        msg = QString("Mouse tracking Graph %1 @ pos (%2 s)")
                .arg( grfY[ig].lhsLabel )
                .arg( t, 0, 'f', 4 );
    }

    if( dragR > dragL ) {

        double  TL = scanGrp->timeFromPos( dragL ),
                TR = scanGrp->timeFromPos( dragR );

        msg += QString("   Selection: [%1, %2], span %3")
                .arg( TL, 0, 'f', 4 )
                .arg( TR, 0, 'f', 4 )
                .arg( TR - TL, 0, 'f', 4 );
    }
    else
        msg += "   Ctrl+click to zoom, shift+click to set export range";

    statusBar()->showMessage( msg );
}


bool FileViewerWindow::queryCloseOK()
{
// for now, always ok.
    return true;
}


// Return vOpen entry with my fvw.
//
FVOpen* FileViewerWindow::linkFindMe()
{
    for( int iw = 0, nw = vOpen.size(); iw < nw; ++iw ) {

        FVOpen  *W = &vOpen[iw];

        if( W->fvw == this )
            return W;
    }

    return 0;
}


// Return true if my runName currently linked.
//
bool FileViewerWindow::linkIsLinked( const FVOpen *me )
{
    return me && linkedRuns.contains( me->runName );
}


// Return true if sharing my runName (including me).
//
bool FileViewerWindow::linkIsSameRun( const FVOpen *W, const FVOpen *me )
{
    return W->fvw && W->runName == me->runName;
}


// Return true if sharing my runName, but NOT me.
//
bool FileViewerWindow::linkIsSibling( const FVOpen *W, const FVOpen *me )
{
    return linkIsSameRun( W, me ) && W->fvw != me->fvw;
}


// Return count (including me) sharing my runName.
//
int FileViewerWindow::linkNSameRun( const FVOpen *me )
{
    int n = 0;

    if( me && !me->runName.isEmpty() ) {

        for( int iw = 0, nw = vOpen.size(); iw < nw; ++iw ) {

            if( vOpen[iw].runName == me->runName )
                ++n;
        }
    }

    return n;
}


// Return true if opened.
//
bool FileViewerWindow::linkOpenName( const QString &name, QPoint &corner )
{
    if( !QFile( name ).exists() )
        return false;

    QString         errorMsg;
    ConsoleWindow*  cons = mainApp()->console();

    if( !DFName::isValidInputFile( name, &errorMsg ) ) {

        QMessageBox::critical(
            cons,
            "Error Opening File",
            QString("File cannot be used for input '%1':\n[%2]")
            .arg( QFileInfo( name ).fileName() )
            .arg( errorMsg ) );
        return false;
    }

    FileViewerWindow    *fvw = new FileViewerWindow;

    corner += QPoint( 100, 100 );
    fvw->move( corner );

    if( !fvw->viewFile( name, &errorMsg ) ) {

        QMessageBox::critical(
            cons,
            "Error Opening File",
            errorMsg );

        Error() << "FileViewer open error: " << errorMsg;
        delete fvw;
        return false;
    }

    return true;
}


// Note: Editing vOpen list will invalidate existing FVOpen* pointers.
//
void FileViewerWindow::linkAddMe( const QString &runName )
{
    if( !linkFindMe() )
        vOpen.push_back( FVOpen( this, DFName::chopType( runName ) ) );
}


// Note: Editing vOpen list will invalidate existing FVOpen* pointers.
//
void FileViewerWindow::linkRemoveMe()
{
    FVOpen  *me = linkFindMe();

    if( me ) {

        if( linkIsLinked( me ) && linkNSameRun( me ) <= 2 )
            linkSetLinked( me, false );

        vOpen.erase( vOpen.begin() + (me - &vOpen[0]) );
    }
}


void FileViewerWindow::linkSetLinked( FVOpen *me, bool linked )
{
    for( int iw = 0, nw = vOpen.size(); iw < nw; ++iw ) {

        FVOpen  *W = &vOpen[iw];

        if( linkIsSameRun( W, me ) )
            W->fvw->linkMenuChanged( linked );
    }

    if( linked )
        linkedRuns.insert( me->runName );
    else
        linkedRuns.remove( me->runName );
}


// Change-flags: {1=pos, 2=span, 3=both}
//
void FileViewerWindow::linkSendPos( int fChanged )
{
    FVOpen  *me = linkFindMe();

    if( !linkIsLinked( me ) )
        return;

    double  t0      = scanGrp->curTime(),
            tSpan   = sav.all.xSpan;

    for( int iw = 0, nw = vOpen.size(); iw < nw; ++iw ) {

        FVOpen  *W = &vOpen[iw];

        if( linkIsSibling( W, me ) ) {

            QMetaObject::invokeMethod(
                W->fvw, "linkRecvPos",
                Qt::QueuedConnection,
                Q_ARG(double, t0),
                Q_ARG(double, tSpan),
                Q_ARG(int, fChanged) );
        }
    }
}


void FileViewerWindow::linkSendSel()
{
    FVOpen  *me = linkFindMe();

    if( !linkIsLinked( me ) )
        return;

    double  tL, tR;

    if( dragL < dragR ) {
        tL = scanGrp->timeFromPos( dragL );
        tR = scanGrp->timeFromPos( dragR );
    }
    else
        tL = tR = -1;

    for( int iw = 0, nw = vOpen.size(); iw < nw; ++iw ) {

        FVOpen  *W = &vOpen[iw];

        if( linkIsSibling( W, me ) ) {

            QMetaObject::invokeMethod(
                W->fvw, "linkRecvSel",
                Qt::QueuedConnection,
                Q_ARG(double, tL),
                Q_ARG(double, tR) );
        }
    }
}


void FileViewerWindow::linkSendManualUpdate( bool manualUpdate )
{
    FVOpen  *me = linkFindMe();

    if( !linkIsLinked( me ) )
        return;

    for( int iw = 0, nw = vOpen.size(); iw < nw; ++iw ) {

        FVOpen  *W = &vOpen[iw];

        if( linkIsSibling( W, me ) ) {

            QMetaObject::invokeMethod(
                W->fvw, "linkRecvManualUpdate",
                Qt::QueuedConnection,
                Q_ARG(bool, manualUpdate) );
        }
    }
}


