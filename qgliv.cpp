/* This file is NOT part of KDE ; )
   Copyright ( C ) 2006/2010-11 Thomas Lübking <thomas.luebking@web.de>

   This application is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or ( at your option ) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QFuture>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QStringList>
#include <QSysInfo>
#include <QtConcurrent>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include "dirfilecombo.h"

#include "qgliv.h"

#include <QDebug>

#ifndef CLAMP
#define CLAMP( x, l, u ) ( x ) < ( l ) ? ( l ) :\
( x ) > ( u ) ? ( u ) :\
( x )
#endif

#ifndef QMAX
#define QMAX( x, y ) ( x ) > ( y ) ? ( x ) : ( y )
#endif

/**
Ok, hello and welcome to al little KGlImageView demo app.
This is not a maintaned app but only a short tutorial for using the QGLImageViewer.
Please don't mourn about functional bugs - THIS DEMO IS NOT MAINTAINED
*******************************************************/

static GLhandleARB shader_vert, shader_frag;

static int numPoly = 1;
static bool supportsAnimation = false;

static void
getNumPoly( QString file )
{
    QFile f( file );

    if ( f.open( QIODevice::ReadOnly ) )
    {
        QTextStream ts( &f );
        bool found = false;
        QString line;
        int index;
        while ( !found )
        {
            line = ts.readLine().simplified();
            supportsAnimation = supportsAnimation || line.startsWith("uniform const float time;");
            index = line.indexOf( "NUMPOLY=" );
            if ( index > -1 ) // hahaa!
            {
                found = true;
                int indr;
                indr = line.indexOf( ' ', index );
                if ( indr > -1 )
                    line = line.mid( index+8, indr - ( index+8 ));
                else
                    line = line.right( line.length() - ( index+8 ));
                int newPoly = line.toInt();
                if ( newPoly > numPoly )
                    numPoly = newPoly;
            }
        }
        f.close();
    }
    else
        qWarning( "No such shader file: %s", file.toLatin1().constData() );
}

Image::Image( QGLImage *image, int idx )
{
    this->image = image;
    qGlImage = 0;
    this->index = idx;
}


static QString _firstFile;

#define xstr(s) str(s)
#define str(s) #s

/** Just building the app framework - just some Qt code, nothing GL special... */
QGLIV::QGLIV( QWidget* parent, const char* name ) : QWidget( parent )
{
    setAccessibleName( name );
    filter.setPatternSyntax( QRegExp::Wildcard );
    filter.setCaseSensitivity( Qt::CaseInsensitive );

    current = prev = next = 0;
    features = ShowMessage;
    diaTime = 10000;
    transitionEffect = 0;
    _lastDirection = 0;
    _animationDone = true;
    transitionFinisher = new QTimer(this);
    transitionFinisher->setSingleShot(true);
    connect(transitionFinisher, SIGNAL(timeout()), this, SLOT(finishImageChange()));
    _oldImage = 0L;

    QStringList args = QCoreApplication::arguments();

    QString startDir;
    if ( args.count() == 2 ) // one paramenter
    {
        QDir dir( args.at(1) );
        if ( !dir.exists() )
        {
            QFileInfo fi( args.at(1) );
            dir = fi.absoluteDir();
            _firstFile = fi.absoluteFilePath();
            args[1] = dir.absolutePath();
        }
        startDir = dir.absolutePath();
    }

    files = QStringList();

    for ( int i = 1; i < args.count(); ++i )
    {
        QDir dir( args.at(i) );
        if ( dir.exists() )
        {
            QStringList fileList = dir.entryList ( QDir::Files | QDir::Readable, QDir::Name );
            for (int i = 0; i < fileList.count(); ++i)
                fileList[i].prepend(dir.absolutePath() + '/');
            files += fileList;
        }
        else
            files += args.at(i);
    }

    //BEGIN  Create & setup the GL viewport                  .
    view = new QGLImageView::QGLImageViewer( this, "glbox1" );
    if ( view->providesShaders() )
    {
#define xstr(s) str(s)
#define str(s) #s
        QString globalDir(xstr(DATADIR)"/piQtureGLide/");
#undef str
#undef xstr
        QString localDir = QStandardPaths::writableLocation( QStandardPaths::GenericDataLocation ) + "/piQtureGLide/";
        char *env = getenv( "QGLIV_SHADER_VERT" );
        QString path = env ? env : "shader.vert";
        if (QFile::exists(globalDir + path))
            path = globalDir + path;
        else if (QFile::exists(localDir + path))
            path = localDir + path;
        else {
            path = QString();
#define CHARS(_S_) _S_.toLocal8Bit().data()
            qWarning("No such shader %s in %s or %s", CHARS(path), CHARS(globalDir), CHARS(localDir) );
            path = "shader.vert";
        }
        shader_vert = 0;
        if (!path.isNull()) {
            getNumPoly( path );
            shader_vert = view->loadShader( path, GL_VERTEX_SHADER_ARB );
        }

        env = getenv( "QGLIV_SHADER_FRAG" );
        path = env ? env : "shader.frag";
        if (QFile::exists(globalDir + path))
            path = globalDir + path;
        else if (QFile::exists(localDir + path))
            path = localDir + path;
        else {
            path = QString();
            qWarning("No such shader %s in %s or %s", CHARS(path), CHARS(globalDir), CHARS(localDir) );
            path = "shader.vert";
#undef CHARS
        }
        qDebug() << "Loading shader from" << path;
        shader_frag = path.isNull() ? 0 : view->loadShader( path, GL_FRAGMENT_SHADER_ARB );
    }

    QFont fnt = view->font(); fnt.setPointSize( 20 ); view->setFont( fnt );
    view->setFocusPolicy( Qt::WheelFocus );
    view->installEventFilter( this );


    //BEGIN  Create a menu                     .
    QMenu *file = new QMenu( "&File", this );
    file->addAction( "Set as wallpaper",  this, SLOT( setAsWallpaper() ), Qt::Key_W );
    file->addAction( "Edit in image editor",  this, SLOT( editImage() ), Qt::Key_E );
    file->addAction( "Exit",  qApp, SLOT( quit() ), Qt::CTRL+Qt::Key_Q );

    QMenu *edit = new QMenu( "&View", this );
    edit->addAction( "Toggle Filename Display", this, SLOT( toggleMessage() ), Qt::Key_M );
    //    edit->addAction( "Toggle Freeze", this, SLOT( toggleFreeze() ), Qt::Key_F );
    edit->addAction( "Blur more", this, SLOT( blurMore() ), Qt::SHIFT+Qt::Key_S );
    edit->addAction( "Blur less", this, SLOT( blurLess() ), Qt::Key_S );
    edit->addAction( "Tunnel more", this, SLOT( tunnelMore() ), Qt::SHIFT+Qt::Key_T );
    edit->addAction( "Tunnel less", this, SLOT( tunnelLess() ), Qt::Key_T );
    if ( supportsAnimation )
        edit->addAction( "Toggle animation", this, SLOT( toggleAnimation() ), Qt::Key_F );

    // Create a rmb menu
    ui.rmbPopup = new QMenu( this );
    ui.level = 0;
    ui.rmbPopup->addMenu( file );
    ui.rmbPopup->addMenu( edit );

    //BEGIN apply actions to viewport       .
    view->addActions( file->actions() );
    view->addActions( edit->actions() );

    setupUI( startDir );

    resetView(0);

    // Put the GL widget inside the frame
    QVBoxLayout *vl = new QVBoxLayout( this );
    vl->setMargin( 0 );
    vl->setSpacing( 0 );
    vl->addWidget( (QWidget*)ui.toolbar );
    QHBoxLayout *hl = new QHBoxLayout;
    hl->addWidget( (QWidget*)ui.tools );
    hl->addWidget( view, 1 );
    vl->addLayout( hl, 1 );
    vl->addWidget( (QWidget*)ui.viewBar );

    QSettings settings("piQtureGLide");
    iAmTouchy = settings.value("TouchMode", false).toBool();
    editorCmd = settings.value("EditorCmd", "gimp-remote \"%f\"").toString();
    wallpaperCmd = settings.value("SetWallpaperCmd", "qdbus org.kde.be.shell /Desktop setWallpaper \"%f\" 0").toString();
    ui.autoSize->setChecked( settings.value( "AutoSize", true ).toBool() );
    setAutosize( ui.autoSize->isChecked() );
    ui.transitions->setCurrentIndex( settings.value( "Transition", 0 ).toUInt() );
    int uilevel = settings.value( "UILevel", 0 ).toUInt();
    for ( int i = 0; i < uilevel; ++i )
        toggleUI();
    connect ( qApp, SIGNAL(aboutToQuit()), SLOT(saveSettings()) );
    QMetaObject::invokeMethod(this, "init", Qt::QueuedConnection);
}

void
QGLIV::init()
{
    // prepare the first image
    prev = new Image;
    current = new Image(0, 0);
    if (!_firstFile.isNull())
        current->index = files.indexOf(_firstFile);
    next = new Image;
    load( current, true );
    if ( !current->image )
    {
        QImage img( 640, 128, QImage::Format_ARGB32 );
        img.fill(Qt::transparent);
        QPainter p(&img);
        QFont font; font.setPixelSize( 24 ); font.setBold( true );
        font.setPixelSize( 24 * 600 / QFontMetrics(font).width( "piQtureGLide" ) );
        p.setFont( font ); p.setPen( Qt::white );
        p.drawText( img.rect(), Qt::AlignCenter, "piQtureGLide" );
        p.end();
        current->qGlImage = new QImage( QGLWidget::convertToGLFormat( img ) );
        current->index = 0;
        addToView( current );
        resetView();
        current->image->rotate( QGLImageView::Y, 100 * 360, 100 * 4000 );
    }
    else
        autoSize() ? maxW() : resetView();
    if ( showMessage() )
        view->message( 20, 20, currentImageInfo(), 5000 );

    // ...and cache in case
    if ( files.count() > 1 )
    {
        next->index = current->index + 1;
        load( next );
        if ( features & Diashow )
        {
            QTimer::singleShot(1000, this, SLOT( maxW() ) );
            _lastDirection = 1;
            QTimer::singleShot(diaTime, this, SLOT( tryChangeImage() ) );
        }
        else if ( features & Cycle )
            load( prev );
    }
}


void QGLIV::setCycle( bool active )
{
    if ( active )
    {
        features |= Cycle;
        if ( !next->image ) load( next );
        if ( !prev->image ) load( prev );
    }
    else
        features &= ~Cycle;
}

void QGLIV::setDiaShow( bool active )
{
    if ( active )
    {
        features |= Diashow;
        _lastDirection = 1;
        QTimer::singleShot(diaTime, this, SLOT( tryChangeImage() ) );
    }
    else
        features &= ~Diashow;
}

void
QGLIV::setDirectory( const QString &path )
{
    QDir dir( path );
    if ( !dir.exists() )
        return; // tsts...

    files.clear();
    currentBasePath = dir.absolutePath() + '/';

    QFuture<QStringList> future = QtConcurrent::run(dir, &QDir::entryList, QDir::Files | QDir::Readable, QDir::Name);
    QFutureWatcher<QStringList>* watcher = new QFutureWatcher<QStringList>;
    watcher->setFuture( future );
    connect( watcher, SIGNAL( finished() ), this, SLOT( dirLoaded() ) );
}

void
QGLIV::dirLoaded()
{
    if ( QFutureWatcher<QStringList>* watcher = dynamic_cast< QFutureWatcher<QStringList>* >( sender() ) )
    {
        files = watcher->result();
        watcher->deleteLater(); // has done it's job
    }

    for (int i = 0; i < files.count(); ++i)
        files[i].prepend( currentBasePath );

    unload( prev ); unload( current ); unload( next );
    prev = current = next = 0;

    init();
}

bool
QGLIV::filterMatches( int index ) const
{
    if (index < 0 || index >= files.count())
        return false;
    return QFileInfo( files.at( index ) ).fileName().contains( filter );
}

void
QGLIV::clearFilter()
{
    disconnect (this, SIGNAL( imageChanged() ), this, SLOT( clearFilter() ) );
    setFilter( QString() );
}

bool
QGLIV::setFilter( const QString &string )
{
    const bool growing = !string.contains(filter);
    filter.setPattern( string.trimmed() );

    // get rid of junk
    int newIndex = -1;
    if (!filterMatches(current->index))
    {
        int i = 0; bool valid = true;
        while (valid)
        {
            valid = false;
            if (current->index + i < files.count())
            {
                valid = true;
                if (filterMatches( current->index + i ))
                    { newIndex = current->index + i; break; }
            }
            if (current->index - i > -1)
            {
                valid = true;
                if (filterMatches( current->index - i ))
                    { newIndex = current->index - i; break; }
            }
            ++i;
        }
    }
    if (newIndex < 0) // not valid or required
        newIndex = current->index;
    qDebug() << "filter" << string << growing << current->index << "->" << newIndex;
    if (growing || prev->index > newIndex || !filterMatches(prev->index))
        { qDebug() << "-> prev"; unload( prev ); prev = new Image( 0, newIndex-1*(newIndex>=current->index) ); load(prev); }
    if (growing || next->index < newIndex || !filterMatches(next->index))
        { qDebug() << "-> next"; unload( next ); next = new Image( 0, newIndex+1*(newIndex<=current->index) ); load(next); }
    changeImage(newIndex - current->index);
    return (newIndex != current->index);
}

void
QGLIV::jumpTo( const QString &string )
{
    connect (this, SIGNAL( imageChanged() ), this, SLOT( clearFilter() ) );
    if (!setFilter( string ))
    {
        disconnect (this, SIGNAL( imageChanged() ), this, SLOT( clearFilter() ) );
        clearFilter();
    }
}

QString
QGLIV::currentImageInfo()
{
    if ( files.count() <= current->index )
        return "";
    return "\"" + QFileInfo( files.at( current->index ) ).fileName() +
           "\" (" + QString::number(current->image->basicWidth()) + "x" +
                    QString::number(current->image->basicHeight()) + ")";
}

void
QGLIV::editImage()
{
    if ( files.count() <= current->index )
        return;
    view->message( 20, 20, "Opening in editor...", 1000 );
    QProcess::startDetached( QString(editorCmd).replace("%f", files.at( current->index ) ) );
}

void
QGLIV::setAsWallpaper()
{
    if ( files.count() <= current->index )
        return;
    view->message( 20, 20, "Setting wallpaper...", 1000 );
    QProcess::startDetached( QString(wallpaperCmd).replace("%f", files.at( current->index ) ) );
}

bool
QGLIV::cacheIsReady()
{
    return ( prev->image || prev->index < 0 ) && ( next->image || next->index < 0 );
}


void
QGLIV::unload( Image *client )
{
    if ( !client )
        return;
    if ( client->image )
    {
        view->remove( client->image->id() );
        client->image = 0;
    }
    delete client;
}

void
loadImage( void *ptr, Image *client )
{
    QGLIV *qgliv = static_cast<QGLIV*>(ptr);
    const bool bwd = ( client == qgliv->prev );
    bool wantCycle = qgliv->cycle();

    if ( client->index < 0 || client->index > qgliv->files.count() - 1 )
    {
        if ( wantCycle )
        {
            wantCycle = false;
            if ( bwd )
                client->index = qgliv->files.count() - 1;
            else
                client->index = 0;
        }
        else
        {
            client->index = -1; // invalidate
            return;
        }
    }

    QImage *buffer = new QImage();
    const int inc = bwd ? -1 : 1;
//     int safer = 0;
    while ( !qgliv->files.isEmpty() )
    {
        while ( !qgliv->filterMatches( client->index ) )
        {   // search next valid entry
            client->index += inc;
            if ( client->index < 0 || client->index > qgliv->files.count() - 1 )
            {
                if ( wantCycle )
                {
                    wantCycle = false;
                    if ( bwd )
                        client->index = qgliv->files.count() - 1;
                    else
                        client->index = 0;
                }
                else
                    { client->index = -1; delete buffer; return; }
            }
        }

        // try to load
        if ( buffer->load( qgliv->files.at( client->index ) ) )
            break;
        else
        {   // this entry is not a supported Image
            qgliv->files.removeAt( client->index );
            qgliv->filterIsDirty = true;

            // TODO this code is the same as above ==================
            client->index += inc;
            if ( client->index < 0 || client->index > qgliv->files.count() - 1 )
            {
                if ( wantCycle )
                {
                    wantCycle = false;
                    if ( bwd )
                        client->index = qgliv->files.count() - 1;
                    else
                        client->index = 0;
                }
                else
                    { client->index = -1; delete buffer; return; }
                // ========================
            }
        }
    }
    // TODO pimp qglimageview for use on raw GL char data
//     if ( !buffer->hasAlphaChannel() )
    // pre-convert the image w/o blocking the UI/GLView

    // stolen from QGLWidget::convertToGLFormat
    const int width = buffer->width();
    const int height = buffer->height();
    uint *p = (uint*) buffer->scanLine(height - 1);
    uint *q = (uint*) buffer->scanLine(0);
    uint r;
    const int height_2 = (height + 1) / 2;
    if (QSysInfo::ByteOrder == QSysInfo::BigEndian) {
        for (int i=0; i < height_2; ++i) {
            const uint *end = p + width;
            while (p < end) {
                r = (*q << 8) | ((*q >> 24) & 0xff);
                *q = (*p << 8) | ((*p >> 24) & 0xff);
                *p = r;
                p++;
                q++;
            }
            p -= 2 * width;
        }
    }
    else
    {
        for (int i=0; i < height_2; ++i) {
            const uint *end = p + width;
            while (p < end) {
                r = ((*q << 16) & 0xff0000) | ((*q >> 16) & 0xff) | (*q & 0xff00ff00);
                *q = ((*p << 16) & 0xff0000) | ((*p >> 16) & 0xff) | (*p & 0xff00ff00);
                *p = r;
                ++p;
                ++q;
            }
            p -= 2 * width;
        }
    }

    client->qGlImage = buffer;
}

void
QGLIV::load( Image *New, bool block )
{
    if ( block )
    {
        loadImage( this, New );
        imageLoaded();
    }
    else
    {
        QFuture<void> future = QtConcurrent::run( loadImage, this, New );
        QFutureWatcher<void>* watcher = new QFutureWatcher<void>;
        watcher->setFuture( future );
        connect( watcher, SIGNAL( finished() ), this, SLOT( imageLoaded() ) );
    }
}


void
QGLIV::addToView( Image *client )
{
    if ( !client->qGlImage || client->image || client->index < 0 )
        return;
    view->load( *client->qGlImage, client == current, numPoly, true );
    client->image = &view->images().last();
    client->image->addShader( shader_vert, false );
    client->image->addShader( shader_frag );
    delete client->qGlImage; client->qGlImage = 0;
}

void
QGLIV::imageLoaded()
{
    if ( QFutureWatcher<void>* watcher = dynamic_cast< QFutureWatcher<void>* >( sender() ) )
        watcher->deleteLater(); // has done it's job

    if ( _animationDone )
    {
        if ( filterIsDirty )
        {
            ui.fileCombo->blockSignals( true );
            const QString text = ui.fileCombo->lineEdit()->text();
            ui.fileCombo->setEntries( files );
            ui.fileCombo->setEditText( text );
            ui.fileCombo->blockSignals( false );
        }
        addToView( prev );
        addToView( current );
        addToView( next );
        if ( cacheIsReady() )
            emit cacheReady( QString() );
    }
    else
        QTimer::singleShot( 50, this, SLOT( imageLoaded() ) );
}


static QPoint drag_start;
static bool is_rotation = false;

bool
QGLIV::eventFilter ( QObject *o, QEvent * e )
{
//    if ( e->type() != QEvent::Resize )
//       return false;
//    ( (QGLImageViewer* )o )->images().last().scaleTo( 100.0, 100.0, true, true );

    if ( !( features & Diashow ) && e->type() == QEvent::Wheel )
    {
        QWheelEvent *we = static_cast<QWheelEvent*>( e );
        int d = we->delta();
        if ( we->modifiers() & Qt::ControlModifier )
            { d > 0 ? zoomIn() : zoomOut(); }
        else
            changeImage( -d );
    }

    else if ( e->type() == QEvent::MouseButtonPress )
    {
        QMouseEvent *me = static_cast<QMouseEvent*>( e );
        if ( me->button() == Qt::LeftButton ) {
            drag_start = me->pos();
            const int tw = view->width()/8, th = view->height()/8, x = drag_start.x(), y = drag_start.y();
            is_rotation = iAmTouchy &&
                        ((x < tw && y < th) || (x > view->width() - tw && y < th) ||
                        (x < tw && y > view->height() - th) || (x > view->width() - tw && y > view->height() - th));
        }
        else if ( me->button() == Qt::RightButton )
            ui.rmbPopup->exec( QCursor::pos() );
    }

    else if ( e->type() == QEvent::MouseMove ) {
        QMouseEvent *me = static_cast<QMouseEvent*>( e );
        if (is_rotation && !(me->modifiers() & Qt::ControlModifier)) {
            QMouseEvent me2( QEvent::MouseMove, me->pos(), me->globalPos(), me->button(), me->buttons(), Qt::ControlModifier );
            QCoreApplication::sendEvent(o, &me2);
            return true;
        }
        else
            return false;
    }

    else if ( e->type() == QEvent::MouseButtonRelease )
    {
        QMouseEvent *me = static_cast<QMouseEvent*>( e );
        if ( iAmTouchy && !dont_dragswitch && me->button() == Qt::LeftButton )
        {
            if (is_rotation) {
                maxW(150);
                return false;
            }
            const int dx = drag_start.x() - me->pos().x();
            const int dy = drag_start.y() - me->pos().y();
            int d = 0;
            if ( qAbs(dx) < view->width()/8 && qAbs(dy) > view->height()/2 ) {
                transitionEffect = VerticalSlide;
                d = dy;
            }
            else if ( qAbs(dx) > view->width()/2 && qAbs(dy) < view->height()/8 ) {
                transitionEffect = HorizontalSlide;
                d = dx;
            }
            if (!changeImage( d ))
                maxW(qMax(qAbs(dx), qAbs(dy))/4);
        }
        else if ( me->button() == Qt::RightButton )
            ui.rmbPopup->exec( QCursor::pos() );
    }

    else if ( e->type() == QEvent::MouseButtonDblClick )
    {
        QMouseEvent *me = static_cast<QMouseEvent*>( e );
        if ( me->button() != Qt::LeftButton )
            return false;
        if ( me->modifiers() & Qt::ControlModifier )
            resetView();
        else
            maxW();
    }

    return false;
}

void
QGLIV::tryChangeImage()
{
    if ( !_lastDirection )
        return;
    if ( cacheIsReady() ) // TODO: cacheIsReady( direction )? requires cancelling the thread and ensure the map isn't halfwise borked...
        changeImage( _lastDirection );
    else
        QTimer::singleShot( 50, this, SLOT( tryChangeImage() ) );
}

#define BREAK_BECAUSE(_STRING_) { view->message( 20, 20, _STRING_, 2000 ); _lastDirection = 0; return false; } //

bool
QGLIV::changeImage( int direction )
{
    if ( !direction ) // should not happen
        return false;

    if ( direction > 0 && next->index < 0 )
        BREAK_BECAUSE( "This is the last image");

    if ( direction < 0 && prev->index < 0)
        BREAK_BECAUSE( "This is the first image");

    if ( !cacheIsReady() )
    {
        if ( _lastDirection )
            view->message( 20, 20, "Wait longer...", 0 );
        else
        {
            _lastDirection = direction;
            view->message( 20, 20, "Please wait...", 0 );
            QTimer::singleShot( 50, this, SLOT( tryChangeImage() ) );
        }
        return true;
    }

    if (transitionFinisher->isActive())
    {
        transitionFinisher->stop();
        finishImageChange();
    }

    _lastDirection = 0;
    _oldImage = current->image;

    int sign = 1;
    _effect = transitionEffect ? transitionEffect : random() % KnownEffects;

    if ( direction > 0 )
    {   // fwd
        unload ( prev );
        prev = current; current = next; next = new Image( 0, current->index + 1 ); load(next);
    }
    else if ( cycle() || current->index > 0 )
    {   // bwd
        unload( next );
        next = current; current = prev; prev = new Image( 0, current->index - 1 );  load(prev);
        sign = -1;
    }
    else
        return false; // no history B.C. ;-P

    // display change
    int delay = 340;
    _animationDone = false;
    QGLImageView::Axis axis = QGLImageView::X;
    switch ( _effect )
    {
        default:
        case Crossfade:
            delay = (features & Diashow) ? 0 : 520;
            autoSize() ? maxW( delay ) : resetView( delay );
            current->image->setAlpha( 0.0 );
            current->image->show( false );
            _oldImage->setAlpha( 0.0, 520 );
            current->image->setAlpha( 100.0, 520 );
            delay = 540;
            break;
        case FadeOutAndIn:
            autoSize() ? maxW( 520 ) : resetView( 520 );
            current->image->setAlpha( 0.0 );
            _oldImage->setAlpha( 0.0, 320 );
            break;
        case HorizontalRotation:
            axis = QGLImageView::Y;
        case VerticalRotation:
            autoSize() ? maxW( 520 ) : resetView( 520 );
            _oldImage->rotate( axis, 90.0*sign, 320 );
            current->image->rotate( axis, -90.0*sign );
            break;
        case ZSlide:
            axis = QGLImageView::Z;
            sign = -sign;
            current->image->setAlpha( 0.0 );
            current->image->resize( current->image->basicWidth(), current->image->basicHeight(), 0, 100.0, 100.0 );
            _oldImage->setAlpha( 0.0, 420-sign*100 );
            current->image->setAlpha( 100.0, 420+sign*100 );
        case VerticalSlide:
            if ( axis == QGLImageView::X )
                { axis = QGLImageView::Y; }
        case HorizontalSlide:
            if ( axis != QGLImageView::Z )
            {
                autoSize() ? maxW( 520 ) : resetView( 520 );
            }
            if (iAmTouchy)
                sign = -3*sign;
            current->image->moveTo( axis, -100.0*sign );
            current->image->show( false );
            _oldImage->moveTo( axis, (250.0*sign)*_oldImage->scaleFactor( axis ), 320 );
            current->image->moveTo( axis, 50.0, 520 );
            delay = 540;
    }

    _effect *= sign;
    transitionFinisher->start(delay);
    QTimer::singleShot( 1000, this, SLOT( setAnimationDone() ) );
    return true;
}

void QGLIV::finishImageChange()
{
    if (!_oldImage)
        return;
    _oldImage->hide();

    //     if ( current->image->hasAlpha() )
    //         view->setCanvas( Qt::gray );
    //     else
    //         view->setCanvas( Qt::black );

    int sign = 1;
    if ( _effect < 0 )
    {
        _effect = -_effect;
        sign = -1;
    }


    QGLImageView::Axis axis = QGLImageView::X;
    switch ( _effect )
    {
        default:
        case Crossfade:
            _oldImage->setAlpha( 100.0 );
            break;
        case FadeOutAndIn:
            _oldImage->setAlpha( 100.0 );
            current->image->show( false );
            current->image->setAlpha( 100.0, 320 );
            break;
        case HorizontalRotation:
            axis = QGLImageView::Y;
        case VerticalRotation:
            _oldImage->rotateTo( 0, 0, 0, false );
            current->image->show( false );
            current->image->rotate( axis, sign*90.0, 320 );
            break;
        case ZSlide:
            autoSize() ? maxW( 160 ) : resetView( 320 );
            _oldImage->setAlpha( 100.0 );
            axis = QGLImageView::Z;
        case VerticalSlide:
            if ( axis == QGLImageView::X ) axis = QGLImageView::Y;
        case HorizontalSlide:
            _oldImage->moveTo( axis, 50.0*_oldImage->scaleFactor( axis ) );
    }

    if ( showMessage() )
        view->message( 20, 20, currentImageInfo(), 5000 );

    setWindowTitle ( files.at( current->index ) );

    if (prev && _oldImage == prev->image && !filterMatches(prev->index))
        { unload(prev); prev = new Image(0, current->index - 1); load(prev); }
    if (next && _oldImage == next->image && !filterMatches(next->index))
        { unload(next); next = new Image(0, current->index + 1); load(next); }

    _oldImage = 0;

    emit imageChanged();

    if ( features & Diashow )
    {
        _lastDirection = 1;
        QTimer::singleShot(diaTime, this, SLOT( tryChangeImage() ) );
    }
}

void QGLIV::saveSettings()
{
    QSettings settings("piQtureGLide");
    settings.setValue( "AutoSize", ui.autoSize->isChecked() );
    settings.setValue( "Transition", ui.transitions->currentIndex() );
    settings.setValue( "UILevel", ui.level );
}

void
QGLIV::setAnimationDone()
{
    _animationDone = true;
}


/** SLOTS for QActions ========================================================================== */


const char* blurstring = "\nBlurrage: %1x%1 kernel";
const char* tunnelstring = "\nTunnel: %1x%1 kernel";

void QGLIV::blurMore()
{
    float fact = current->image->blurrage()+0.5;
    current->image->boxBlur( fact );
    view->message( 20, 20, QString( currentImageInfo() + blurstring ).arg( 2*fact+1 ), 1000 );
}

void QGLIV::blurLess()
{
    float fact = QMAX( 0, current->image->blurrage()-0.5 );
    current->image->boxBlur( fact );
    if ( fact )
        view->message( 20, 20, QString( currentImageInfo() + blurstring ).arg( 2*fact+1 ), 1000 );
    else
        view->message( 20, 20, QString( currentImageInfo() + "\nBlurrage: off" ), 1000 );
}

void QGLIV::tunnelMore()
{
    float fact = current->image->blurrage()+0.5;
    current->image->tunnel( fact );
    view->message( 20, 20, QString( currentImageInfo() + tunnelstring ).arg( 2*fact+1 ), 1000 );
}

void QGLIV::tunnelLess()
{
    float fact = QMAX( 0, current->image->blurrage()-0.5 );
    current->image->tunnel( fact );
    if ( fact )
        view->message( 20, 20, QString( currentImageInfo() + tunnelstring ).arg( 2*fact+1 ), 1000 );
    else
        view->message( 20, 20, QString( currentImageInfo() + "\nTunnel: off" ), 1000 );
}

void QGLIV::toggleAnimation()
{
    if ( !supportsAnimation )
        return;
    view->animate( !view->animated() );
    qDebug() << "animating";
}

void QGLIV::changeColor( int key, int v )
{
    QColor c = current->image->color();
    float a = current->image->alpha();
    if ( key == Qt::Key_A ) // alpha
    {
        a = CLAMP( a+v, 0.0, 100.0 );
        current->image->setAlpha( a, 200 );
    }
    else
    {
        if ( key == Qt::Key_R )
            c = QColor( CLAMP( c.red()+v, 0, 255 ), c.green(), c.blue() );
        else if ( key == Qt::Key_G )
            c = QColor( c.red(), CLAMP( c.green()+v, 0, 255 ), c.blue() );
        else if ( key == Qt::Key_B )
            c = QColor( c.red(), c.green(), CLAMP( c.blue()+v, 0, 255 ));
        current->image->tint( c, 200 );
    }
    view->message( 20, 20, QString( "R: %1, G: %2, B: %3, A: %4\%" ).arg( c.red() ).arg( c.green() ).arg( c.blue() ).arg( a ), 1000 );
}




void QGLIV::toggleMessage()
{
    if ( features & ShowMessage )
    {
        features &= ~ShowMessage;
        view->hideMessage();
    }
    else
        features |= ShowMessage;
}
