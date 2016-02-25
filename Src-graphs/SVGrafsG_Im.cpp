
#include "Util.h"
#include "MainApp.h"
#include "ConfigCtl.h"
#include "GraphsWindow.h"
#include "SVGrafsG_Im.h"
#include "FramePool.h"

#include <QStatusBar>
#include <QSettings>




/* ---------------------------------------------------------------- */
/* class SVGrafsG_Im ---------------------------------------------- */
/* ---------------------------------------------------------------- */

// BK: Of course, need expanded trigChan

SVGrafsG_Im::SVGrafsG_Im( GraphsWindow *gw, DAQ::Params &p )
    :   SVGrafsG( gw, p ), hipass(true)
{
}


SVGrafsG_Im::~SVGrafsG_Im()
{
    saveSettings();
}


static void addLFP(
    short   *data,
    int     ntpts,
    int     nchans,
    int     nNeu )
{
    for( int it = 0; it < ntpts; ++it, data += nchans ) {

        for( int ic = 0; ic < nNeu; ++ic )
            data[ic] += data[ic+nNeu];
    }
}


/*  Time Scaling
    ------------
    Each graph has its own wrapping data buffer (ydata) and its
    own time axis span. As fresh data arrive they wrap around such
    that the latest data are present as well as one span's worth of
    past data. We will draw the data using a wipe effect. Older data
    remain visible while they are progressively overwritten by the
    new from left to right. In this mode selection ranges do not
    make sense, nor do precise cursor readouts of time-coordinates.
    Rather, min_x and max_x suggest only the span of depicted data.
*/

void SVGrafsG_Im::putScans( vec_i16 &data, quint64 headCt )
{
#if 0
    double	tProf	= getTime();
#endif
    double      ysc		= 1.0 / 32768.0;
    const int   nC      = ic2G.size(),
                ntpts   = (int)data.size() / nC;

/* ------------------------ */
/* Add LFP to AP if !hipass */
/* ------------------------ */

    if( !hipass
        && p.im.imCumTypCnt[CimCfg::imSumNeural] ==
            2 * p.im.imCumTypCnt[CimCfg::imSumAP] ) {

        addLFP( &data[0], ntpts, nC, p.im.imCumTypCnt[CimCfg::imSumAP] );
    }

/* --------------------- */
/* Append data to graphs */
/* --------------------- */

    drawMtx.lock();

    QVector<float>  ybuf( ntpts );	// append en masse

    for( int ic = 0; ic < nC; ++ic ) {

        GLGraph *G = ic2G[ic];

        if( !G )
            continue;

        // Collect points, update mean, stddev

        GLGraphX    &X      = ic2X[ic];
        GraphStats  &stat   = ic2stat[ic];
        qint16      *d      = &data[ic];
        int         dwnSmp  = X.dwnSmp,
                    dstep   = dwnSmp * nC,
                    ny      = 0;

        stat.clear();

        if( ic < p.im.imCumTypCnt[CimCfg::imSumAP] ) {

            // -------------------
            // Neural downsampling
            // -------------------

            // Withing each bin, report the greatest
            // amplitude (pos or neg) extremum. This
            // ensures spikes are not missed.

            if( dwnSmp <= 1 )
                goto pickNth;

            int ndRem = ntpts;

            for( int it = 0; it < ntpts; it += dwnSmp ) {

                int binMin = *d,
                    binMax = binMin,
                    binWid = dwnSmp;

                    stat.add( *d );

                    d += nC;

                    if( ndRem < binWid )
                        binWid = ndRem;

                for( int ib = 1; ib < binWid; ++ib, d += nC ) {

                    int	val = *d;

                    stat.add( *d );

                    if( val < binMin )
                        binMin = val;

                    if( val > binMax )
                        binMax = val;
                }

                ndRem -= binWid;

                if( abs( binMin ) > abs( binMax ) )
                    binMax = binMin;

                ybuf[ny++] = binMax * ysc;
            }
        }
        else if( ic < p.im.imCumTypCnt[CimCfg::imSumNeural] ) {

            // ---
            // LFP
            // ---

pickNth:
            for( int it = 0; it < ntpts; it += dwnSmp, d += dstep ) {

                ybuf[ny++] = *d * ysc;
                stat.add( *d );
            }
        }
        else {

            // ----
            // Sync
            // ----

            for( int it = 0; it < ntpts; it += dwnSmp, d += dstep )
                ybuf[ny++] = *d;
        }

        // Append points en masse
        // Renormalize x-coords -> consecutive indices.

        X.dataMtx->lock();
        X.ydata.putData( &ybuf[0], ny );
        X.dataMtx->unlock();

        // Update pseudo time axis

        double  span =  X.spanSecs();

        X.max_x = (headCt + ntpts) / p.im.srate;
        X.min_x = X.max_x - span;

        // Draw

        QMetaObject::invokeMethod( G, "update", Qt::QueuedConnection );
    }

    drawMtx.unlock();

/* --------- */
/* Profiling */
/* --------- */

#if 0
    tProf = getTime() - tProf;
    Log() << "Graph milis " << 1000*tProf;
#endif
}


bool SVGrafsG_Im::isSelAnalog() const
{
    return selected < p.im.imCumTypCnt[CimCfg::imSumNeural];
}


void SVGrafsG_Im::hipassClicked( bool checked )
{
    hipass = checked;
    saveSettings();
}


void SVGrafsG_Im::mySaveGraphClicked( bool checked )
{
    int thisChan = sender()->objectName().toInt();

    mainApp()->cfgCtl()->graphSetsImSaveBit( thisChan, checked );
}


void SVGrafsG_Im::myMouseOverGraph( double x, double y )
{
    int		ic			= lastMouseOverChan = graph2Chan( sender() );
    bool	isNowOver	= true;

    if( ic < 0 || ic >= myChanCount() ) {
        gw->statusBar()->clearMessage();
        return;
    }

    QWidget	*w = QApplication::widgetAt( QCursor::pos() );

    if( !w || !dynamic_cast<GLGraph*>(w) )
        isNowOver = false;

    double      mean, rms, stdev;
    QString		msg;
    const char	*unit,
                *swhere = (isNowOver ? "Mouse over" : "Last mouse-over");
    int			h,
                m;

    h = int(x / 3600);
    x = x - h * 3600;
    m = x / 60;
    x = x - m * 60;

    if( ic < p.im.imCumTypCnt[CimCfg::imSumNeural] ) {

        // neural readout

        computeGraphMouseOverVars( ic, y, mean, stdev, rms, unit );

        msg = QString(
            "%1 %2 @ pos (%3h%4m%5s, %6 %7)"
            " -- {mean, rms, stdv} %7: {%8, %9, %10}")
            .arg( swhere )
            .arg( STR2CHR( myChanName( ic ) ) )
            .arg( h, 2, 10, QChar('0') )
            .arg( m, 2, 10, QChar('0') )
            .arg( x, 0, 'f', 3 )
            .arg( y, 0, 'f', 4 )
            .arg( unit )
            .arg( mean, 0, 'f', 4 )
            .arg( rms, 0, 'f', 4 )
            .arg( stdev, 0, 'f', 4 );
    }
    else {

        // sync readout

        msg = QString(
            "%1 %2 @ pos %3h%4m%5s")
            .arg( swhere )
            .arg( STR2CHR( myChanName( ic ) ) )
            .arg( h, 2, 10, QChar('0') )
            .arg( m, 2, 10, QChar('0') )
            .arg( x, 0, 'f', 3 );
    }

    gw->statusBar()->showMessage( msg );
}


void SVGrafsG_Im::myClickGraph( double x, double y )
{
    myMouseOverGraph( x, y );
    selectChan( lastMouseOverChan );
}


int SVGrafsG_Im::myChanCount()
{
    return p.im.imCumTypCnt[CimCfg::imSumAll];
}


double SVGrafsG_Im::mySampRate()
{
    return p.im.srate;
}


void SVGrafsG_Im::mySort_ig2ic()
{
    if( set.usrOrder )
        p.sns.imChans.chanMap.userOrder( ig2ic );
    else
        p.sns.imChans.chanMap.defaultOrder( ig2ic );
}


int SVGrafsG_Im::myGrfPerTab() const
{
    int lim = MAX_GRAPHS_PER_TAB;

    lim = 32;

    if( p.sns.maxGrfPerTab && p.sns.maxGrfPerTab <= lim )
        return p.sns.maxGrfPerTab;

    return lim;
}


QString SVGrafsG_Im::myChanName( int ic ) const
{
    return p.sns.imChans.chanMap.name( ic, ic == p.trigChan() );
}


QBitArray& SVGrafsG_Im::mySaveBits()
{
    return p.sns.imChans.saveBits;
}


void SVGrafsG_Im::myCustomXSettings( int ic )
{
    GLGraphX    &X = ic2X[ic];

    if( ic < p.im.imCumTypCnt[CimCfg::imSumAP] ) {
        X.bkgnd_Color   = NeuGraphBGColor;
        X.usrType       = 0;
    }
    else if( ic < p.im.imCumTypCnt[CimCfg::imSumNeural] ) {
        X.bkgnd_Color   = LfpGraphBGColor;
        X.usrType       = 1;
    }
    else {
        X.bkgnd_Color   = DigGraphBGColor;
        X.usrType       = 2;
    }
}


void SVGrafsG_Im::saveSettings()
{
// -----------------------------------
// Display options, channel by channel
// -----------------------------------

    STDSETTINGS( settings, "graphs_G_Im" );

    settings.beginGroup( "All" );
    settings.setValue( "filter", set.filter );
    settings.setValue( "usrOrder", set.usrOrder );
    settings.endGroup();

    settings.beginGroup( "Each" );

    for( int ic = 0, nC = myChanCount(); ic < nC; ++ic ) {

        settings.setValue(
            QString("chan%1").arg( ic ),
            ic2X[ic].toString() );
    }

    settings.endGroup();
}


void SVGrafsG_Im::loadSettings()
{
// -----------------------------------
// Display options, channel by channel
// -----------------------------------

    STDSETTINGS( settings, "graphs_G_Im" );

    settings.beginGroup( "All" );
    set.filter      = settings.value( "filter", false ).toBool();
    set.usrOrder    = settings.value( "usrOrder", false ).toBool();
    settings.endGroup();

    settings.beginGroup( "Each" );

// Note on digital channels:
// The default yscale and color settings loaded here are
// correct for digital channels, and we forbid editing
// those values (see updateToolbar()).

    double  srate = mySampRate();

    for( int ic = 0, nC = myChanCount(); ic < nC; ++ic ) {

        QString s = settings.value(
                        QString("chan%1").arg( ic ),
                        "fg:ffeedd82 xsec:1.0 yscl:1" )
                        .toString();

        ic2X[ic].fromString( s, srate );
        ic2stat[ic].clear();
    }

    settings.endGroup();
}


// Values (v) are in range [-1,1].
// (v+1)/2 is in range [0,1].
// This is mapped to range [rmin,rmax].
//
double SVGrafsG_Im::scalePlotValue( double v, double gain )
{
    return p.im.range.unityToVolts( (v+1)/2 ) / gain;
}


// Call this only for neural channels!
//
void SVGrafsG_Im::computeGraphMouseOverVars(
    int         ic,
    double      &y,
    double      &mean,
    double      &stdev,
    double      &rms,
    const char* &unit )
{
    double  gain = p.im.chanGain( ic );

    y       = scalePlotValue( y, gain );

    drawMtx.lock();

    mean    = scalePlotValue( ic2stat[ic].mean(), gain );
    stdev   = scalePlotValue( ic2stat[ic].stdDev(), gain );
    rms     = scalePlotValue( ic2stat[ic].rms(), gain );

    drawMtx.unlock();

    unit    = "V";

    if( p.im.range.rmax < gain ) {
        y       *= 1000.0;
        mean    *= 1000.0;
        stdev   *= 1000.0;
        rms     *= 1000.0;
        unit     = "mV";
    }
}

