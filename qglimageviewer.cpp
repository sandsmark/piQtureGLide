/* QGLImageViewer.cpp v0.0 - This file is NOT part of KDE ;)
   Copyright (C) 2006/2010 Thomas Lübking <thomas.luebking@web.de>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#define GL_GLEXT_PROTOTYPES

#define DO_DEBUG 0

#include <cmath>

// #include <QtGui>
#include <QCursor>
#include <QImage>
#include <QFile>
#include <QPixmap>
#include <QMap>
#include <QTimer>
#include <QFontInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDebug>
#include <QTime>
#include <QGLPixelBuffer>
#define QMAX(x,y) (x) > (y) ? (x) : (y)
#define QMIN(x,y) (x) < (y) ? (x) : (y)



#include "qglimageviewer.h"

#ifndef CLAMP
#define CLAMP(x,l,u) (x) < (l) ? (l) :\
(x) > (u) ? (u) :\
(x)
#endif

#ifndef QABS
#define QABS(x) (x) < 0 ? -(x) : (x)
#endif
// adapted from lighthouse3d tutorial
static void printInfoLog(GLhandleARB obj, QString message)
{
   int infologLength = 0;
   int charsWritten  = 0;
   char *infoLog;

   glGetObjectParameterivARB(obj, GL_OBJECT_INFO_LOG_LENGTH_ARB, &infologLength);

   if (infologLength > 1)
   {
      infoLog = (char *)malloc(infologLength);
      glGetInfoLogARB(obj, infologLength, &charsWritten, infoLog);
      qDebug() << message << "\n------------------ | " << infologLength;
      qDebug() << infoLog;
      qDebug() << "============================";
      free(infoLog);
   }
}

using namespace QGLImageView;

QGLImage::QGLImage(QGLImageViewer *parent, const uint id, const GLuint object,
                   GLuint *textures, const int texAmount, int width, int height, bool hasAlpha)
: _id(id),
_object(object),
_blurObj(0),
_blurTex(0),
_blurW(0),
_blurH(0),
_textures(textures),
_texAmount(texAmount),
_basicWidth(width),
_basicHeight(height),
_isShown(false),
_activeAnimations(0),
_hasAlpha(hasAlpha),
_hasClipping(false),
_inverted(false),
_shaderProgram(0)
{
   if (!parent)
   {
      qWarning("QGLImage can not exist without QGLImageViewer!!!");
      return;
   }

   _parent = parent;
   for (int i = 0; i < 3; ++i)
   {
      _scale[i] = 1.0;
      _rotation[i] = 0.0;
      _translation[i] = 0.0;
      _color[i] = 1.0;
      _colorI[i] = 0;

      _scaleStep[i] = 0.0;
      _rotationStep[i] = 0.0;
      _translationStep[i] = 0.0;
      _colorStep[i] = 0.0;

      _desiredScale[i] = 0.0;
      _desiredRotation[i] = 0.0;
      _desiredTranslation[i] = 0.0;
      _desiredColor[i] = 0.0;

   }
   _colorI[3] = 1.0;
   _colorStep[3] = 0.0;
   _desiredColor[3] = 0.0;
   _brightness = 1.0;
   _brightnessStep = 0.0;
   _desiredBrightness = 0.0;
   _blur = 0.0;
   _blurStep = 0.0;
   _blurType = 0;
   _desiredBlur = 0.0;
   _combineRGB = GL_ADD;
   _ratio = (float)height/width;

   for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j)
         _clip[i][j] = 0.0;
   _clip[0][0] = 1.0; _clip[1][0] = -1.0; _clip[2][1] = -1.0; _clip[3][1] = 1.0;
}

/*
QGLImage::~QGLImage()
{
}
*/
int QGLImage::width() const
{
   return (int) (3.0 * _scale[X] * _parent->scaleFactor(X) * _parent->width() / -(_translation[Z] + _parent->position(Z)));
}
int QGLImage::height() const
{
   return (int) (3.0 * _scale[Y] * _parent->scaleFactor(Y) * _parent->width() * _basicHeight / (-(_translation[Z] + _parent->position(Z)) * _basicWidth));
}


void QGLImage::setAlpha(float percent, int msecs)
{
    // stop running alpha animagtions (iff)
    if (_colorStep[3] != 0.0)
    {
        --_activeAnimations;
        _colorStep[3] = 0.0;
    }
    if (msecs == 0)
    {
        _colorI[3] = percent / 100.0;
        if (_shaderProgram)
        {
            _parent->makeCurrent();
            glUseProgramObjectARB(_shaderProgram); // this seems to be necessary
            glUniform1fARB(_sAlpha, _colorI[3]);
        }
        if (_isShown)
            _parent->updateGL();
        return;
    }
    _desiredColor[3] = percent / 100.0;
    _colorStep[3] = (_desiredColor[3] - _colorI[3]) * _parent->fpsDelay() / msecs;
    if (_colorStep[3] != 0.0) // need to animate
    {
        ++_activeAnimations;
        _parent->ensureTimerIsActive();
    }
}

/**
* Ok, this is gonna be tricky, so i'll better explain it (also for myself ;)
* OpenGL - even 1.5 - does not support directly a sane combination of
* coloring and brightness using the rgb combination between the fracture color
* and the texture (the image...)
* you can have GL_MODULATE (the default) which ends up in a multiplication of the color
* and the texture - as colors are ranged [0,1] this can never get brighter than the
* original color, so it's useless for brightening
* GL_ADD can only 'add' the color to the texture, so it can never be darker than the original
* newer versions of OpenGL (any current system should run such) also support GL_SUBTRACT - now
* guess what it does.
* Now you could say: "If (brightness > 1) use GL_ADD, else use GL_SUBTRACT with the inverted color"
* and in genereral it's what we're gonna do, but we're in trouble with coloring.
* Coloring means to lower or raise a specific color channel in coparision to the others.
* Now GL_ADD can only achieve this by  adding a color, GL_SUBTRACT only by reducing the others and this
* is gonna cause an ugly break on the border between the two combination modes as adding also means
* brightening while subtracting means darkening...
* Luckily there's also GL_ADD_SIGNED that supports adding and subtracting, depending on if the color is
* less or greater than 0.5 - so we want to use it as glue between ADD and SUBTRACT and shift the coloring
* between increasing a color channel and decreasing the others
*/

void QGLImageViewer::mergeCnB(QGLImage & img)
{
   /** first step: figure out the ratio of each channel ranged [0,1/3] */
   float sum = img._color[0] + img._color[1] + img._color[2];
   if (sum > 0.0)
   {
      for (int i=0; i < 3; ++i)
         img._colorI[i] = (img._color[i]*img._color[i]/(3.0*sum));
   }
   else /** handle division by 0: lim(_colorI[i] : _color[0,1,2] -> 0) = 0 */
   {
      for (int i=0; i < 3; ++i)
         img._colorI[i] = 0.0;
   }

   float max = QMAX(QMAX(img._colorI[0],img._colorI[1]),img._colorI[2]);
   float min = QMIN(QMIN(img._colorI[0],img._colorI[1]),img._colorI[2]);

   /** next figure out whether subtract, add or where between */
   if (min + img._brightness < 5.0/6.0)
//    == (min - 1.0/3.0 + _brightness - 1.0 < -0.5)
//    == (_brightness < 0.5 - min + 1/3)
   {
      /** This means we want to subtract more than 0.5, what add_signed cannot handle */
      img._combineRGB = GL_SUBTRACT; // qWarning("subtract");
      for (int i=0; i < 3; ++i)
         img._colorI[i] = 1.0/4.5 - img._colorI[i] + (1.0 - img._brightness);
//          == _colorI[i] = 1.0/3.0 - _colorI[i]*sum/_color[i] + (1.0 - _brightness);
   }
   else if (max + img._brightness > 1.5)
//    == (max + _brightness - 1.0 > 0.5)
//    == (_brightness > 1.5 - max)
   {
      /** This means we want to add more than 0.5, what add_signed cannot handle */
      img._combineRGB = GL_ADD; // qWarning("add");
      for (int i=0; i < 3; ++i)
         img._colorI[i] = img._colorI[i] + img._brightness - 1.0;
   }
   else
   {
      /** glueing ;) - NOTICE that we 'should' include the +1/12 shift
      (which is necessary to end up with no change for color == white / brightnes == 1)
      into the shift function "(1.0/mi * _brightness - (1.5-max)/mi) * 1.0/3.0"
      but i decided this is useless (coloring isn't a precise function at all anyway)
      and would only add complexity */
      img._combineRGB = GL_ADD_SIGNED; // qWarning("add_signed");
      float mi = (max - 2.0/3.0 - min);
      for (int i=0; i < 3; ++i)
         img._colorI[i] = img._colorI[i] - (1.0/mi * img._brightness - (1.5-max)/mi) * 1.0/3.0 + img._brightness - 5.0/12.0;
//          == _colorI[i] = _colorI[i] - (1.0/mi * _brightness - (1.5-max)/mi) * 1.0/3.0 + _brightness - 1.0 + 0.5 + 1.0/12.0;
   }
}

void QGLImage::paint()
{
//    glDisable( GL_TEXTURE_2D );
//    glDisable( GL_DEPTH_TEST );
//    glColor3f(1.0,0.0,0.0);
//    glRectf(-1.0,-1.0,1.0,1.0);
// //    glRectf(-_scaleTarget[X]-0.05,-_scaleTarget[Y]-0.05,-_scaleTarget[X]+0.05,-_scaleTarget[Y]+0.05);
//    glEnable( GL_TEXTURE_2D );
//    glEnable( GL_DEPTH_TEST );
//    return;
    if (!_isShown)
        return;
    glUseProgramObjectARB(_shaderProgram);
    glUniform1fARB(_sTime, _parent->_animCounter); // updated on ::timerEvent(), where we can't set it
    glPushMatrix();
    glTranslatef( position(X), position(Y), position(Z));
    // TODO: make only one rotate call iff possible
    glRotatef( rotation(X), 1.0, 0.0, 0.0 );
    glRotatef( rotation(Y), 0.0, 1.0, 0.0 );
    glRotatef( rotation(Z), 0.0, 0.0, 1.0 );
    glScalef( _scale[X], _scale[Y], _scale[Z] );
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, _combineRGB);

    if (hasClipping())
    {
        glClipPlane (GL_CLIP_PLANE0, _clip[0]);
        glEnable (GL_CLIP_PLANE0);
        glClipPlane (GL_CLIP_PLANE1, _clip[1]);
        glEnable (GL_CLIP_PLANE1);
        glClipPlane (GL_CLIP_PLANE2, _clip[2]);
        glEnable (GL_CLIP_PLANE2);
        glClipPlane (GL_CLIP_PLANE3, _clip[3]);
        glEnable (GL_CLIP_PLANE3);
    }

//    if (isInverted())
//       glBlendFunc(GL_ONE_MINUS_SRC_COLOR, GL_SRC_COLOR);

    glUseProgramObjectARB(_shaderProgram);
    glColor4fv(_colorI);

    glCallList( _blur > 0.0 ? _blurObj : glObject() );

//    if (isInverted())
//       glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (hasClipping())
    {
        glDisable(GL_CLIP_PLANE0);
        glDisable(GL_CLIP_PLANE1);
        glDisable(GL_CLIP_PLANE2);
        glDisable(GL_CLIP_PLANE3);
    }
#if 0
      glDisable( GL_TEXTURE_2D );
      glDisable(GL_DEPTH_TEST);
      glBegin(GL_LINES/*GL_LINE_LOOP*/);
      glColor3f(0.0,0.0,1.0);
      glLineWidth(10.0);
//       glLineStipple(1, 0x3F07);
      glVertex2f(-1.0, -1.0*(*it)._ratio);
      glVertex2f(-1.0, -1.0*(*it)._ratio + 2.0*(*it)._ratio);
      glVertex2f(1.0, -1.0*(*it)._ratio + 2.0*(*it)._ratio);
      glVertex2f(1.0, -1.0*(*it)._ratio);
      glEnd();
      glEnable(GL_DEPTH_TEST);
      glEnable( GL_TEXTURE_2D );
#endif
    glPopMatrix();
    glUseProgramObjectARB(0);
}


void QGLImage::setBrightness(float percent, int msecs)
{
    // stop running color animagtions (iff)
   if (_brightnessStep != 0.0)
   {
      _brightnessStep = 0.0;
      --_activeAnimations;
   }
   float oldBrightness = _brightness;
   _brightness = CLAMP(percent/100.0, 0.0, 2.0);
   if (oldBrightness == _brightness)
      return;
   if (!msecs)
   {
      if (_shaderProgram)
      {
         _parent->makeCurrent();
         glUseProgramObjectARB(_shaderProgram); // this seems to be necessary
         glUniform1fARB(_sBrightness, _brightness-1.0);
      }
      _parent->mergeCnB(*this);
      if (_isShown)
         _parent->updateGL();
      return;
   }

   _desiredBrightness = _brightness;
   _brightness = oldBrightness;
   _brightnessStep = (_desiredBrightness - _brightness) * _parent->fpsDelay() / msecs;

   if (_brightnessStep != 0.0) // need to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
}

void QGLImage::tint(const QColor & color, int msecs)
{
    // stop running color animations (iff)
    if (_colorStep[0] != 0.0 || _colorStep[1] != 0.0 || _colorStep[2] != 0.0)
    {
        _colorStep[0] = 0.0; _colorStep[1] = 0.0; _colorStep[2] = 0.0;
        --_activeAnimations;
    }
    bool sameColor = _color[0] == (float)color.red()/255.0 &&
                     _color[1] == (float)color.green()/255.0 &&
                     _color[2] == (float)color.blue()/255.0;
    if (msecs == 0 || sameColor)
    {
        _color[0] = (float)color.red()/255.0;
        _color[1] = (float)color.green()/255.0;
        _color[2] = (float)color.blue()/255.0;
        // export shader variable
        if (_shaderProgram)
        {
            _parent->makeCurrent();
            glUseProgramObjectARB(_shaderProgram); // this seems to be necessary
            glUniform3fARB(_sColor, _color[0], _color[1], _color[2]);
        }
        // done
        _parent->mergeCnB(*this);
        if (_isShown)
            _parent->updateGL();
        return;
    }
    _desiredColor[0] = (float)color.red()/255.0;
    _desiredColor[1] = (float)color.green()/255.0;
    _desiredColor[2] = (float)color.blue()/255.0;

    _colorStep[0] = (_desiredColor[0] - _color[0]) * _parent->fpsDelay() / msecs;
    _colorStep[1] = (_desiredColor[1] - _color[1]) * _parent->fpsDelay() / msecs;
    _colorStep[2] = (_desiredColor[2] - _color[2]) * _parent->fpsDelay() / msecs;

    if (_colorStep[0] != 0.0 || _colorStep[1] != 0.0 || _colorStep[2] != 0.0) // need to animate
    {
        ++_activeAnimations;
        _parent->ensureTimerIsActive();
    }
}

void QGLImage::invert(bool inverted, bool update)
{
   _inverted = inverted;
   if (_shaderProgram)
   {
      _parent->makeCurrent();
      glUseProgramObjectARB(_shaderProgram); // this seems to be necessary
      glUniform1fARB(_sInverted, inverted ? 1.0 : 0.0);
   }
   if (update)
      _parent->updateGL();
}

void QGLImage::blur(float factor, int msecs, int type)
{
   // stop running color animagtions (iff)
   if (_blurStep != 0.0)
   {
      _blurStep = 0;
      --_activeAnimations;
   }
   GLfloat oldBlur = _blur;
   _blur = QMAX(0.0, factor);
   if ((oldBlur == _blur) && (_blurType == type))
      return;
   _blurType = type;
   if (!msecs && _isShown)
   {
      _parent->blur(*this);
      _parent->updateGL();
      return;
   }

   _desiredBlur = _blur;
   _blur = oldBlur;
   _blurStep = (_desiredBlur - _blur) * _parent->fpsDelay() / msecs;

   if (_blurStep != 0.0) // need to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
}

void QGLImage::show(bool update)
{
   _isShown = true;
   if (update)
      _parent->updateGL();
}
void QGLImage::hide(bool update)
{
   _isShown = false;
   if (update)
      _parent->updateGL();
}

void QGLImage::rotate(Axis a, float degrees, int msecs)
{
   // stop running animations
   if (_rotationStep[a] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[a] = 0.0;
   }
   float deg = a == Z ? -degrees : degrees;
   // TODO: clamp rotation to [0,360] when done
   if (msecs == 0)
   {
      _rotation[a] = deg;
      _parent->updateGL();
      return;
   }
   _desiredRotation[a] = _rotation[a] + deg;
   _rotationStep[a] = deg * _parent->fpsDelay() / msecs;
   if (_rotationStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
}
void QGLImage::rotate(float xDegrees, float yDegrees, float zDegrees, bool update)
{
   // stop running animations
   if (_rotationStep[X] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[X] = 0.0;
   }
   if (_rotationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Y] = 0.0;
   }
   if (_rotationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Z] = 0.0;
   }
   _rotation[X] += xDegrees;
   _rotation[Y] += yDegrees;
   _rotation[Z] -= zDegrees;
   for (int a = 0; a < 3; ++a)
   {
      while (_rotation[a] > 360.0)
         _rotation[a] -= 360.0;
      while (_rotation[a] < 0.0)
         _rotation[a] += 360.0;
   }
   if (update)
      _parent->updateGL();
}
void QGLImage::rotateTo(Axis a, float degrees, int msecs)
{
   // stop running animations
   if (_rotationStep[a] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _rotation[a] = degrees;
      _parent->updateGL();
      return;
   }
   // clamp current rotation
   while (_rotation[a] > 180.0)
      _rotation[a] -= 360.0;
   while (_rotation[a] < -180.0)
      _rotation[a] += 360.0;
   //======
   _desiredRotation[a] = degrees;
   float diff = _desiredRotation[a] - _rotation[a];
   if (QABS(diff) > 180.0)
      diff = 360.0 - QABS(diff);
   _rotationStep[a] = diff * _parent->fpsDelay() / msecs;
   if (_rotationStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
}
void QGLImage::rotateTo(float xDegrees, float yDegrees, float zDegrees, bool update)
{
   // stop running animations
   if (_rotationStep[X] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[X] = 0.0;
   }
   if (_rotationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Y] = 0.0;
   }
   if (_rotationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Z] = 0.0;
   }
   _rotation[X] = xDegrees;
   _rotation[Y] = yDegrees;
   _rotation[Z] = zDegrees;
   for (int a = 0; a < 3; ++a)
   {
      while (_rotation[a] > 360.0)
         _rotation[a] -= 360.0;
      while (_rotation[a] < 0.0)
         _rotation[a] += 360.0;
   }
   if (update)
      _parent->updateGL();
}

float QGLImage::scale(Axis a, float percent, int msecs)
{
   if (percent < 0.0 || a == Z) //invalid
      return 0.0;
   // stop running animations
   if (_scaleStep[a] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _scale[a] *= percent/100.0;
      _parent->updateGL();
      return _scale[a]*100.0;
   }
   _desiredScale[a] = _scale[a]*percent/100.0;
   _scaleStep[a] = (_desiredScale[a] - _scale[a]) * _parent->fpsDelay() / msecs;
   if (_scaleStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
   return _scale[a]*100.0;
}

void QGLImage::scale(float xPercent, float yPercent, bool update)
{
   if (xPercent < 0.0 && yPercent < 0.0 ) //invalid
      return;
   // stop running animations
   if (_scaleStep[X] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[X] = 0.0;
   }
   if (_scaleStep[Y] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[Y] = 0.0;
   }
   _scale[X] *= xPercent/100.0;
   _scale[Y] *= yPercent/100.0;
   if (_scale[X] < 0.0) _scale[X] = _scale[Y];
   if (_scale[Y] < 0.0) _scale[Y] = _scale[X];
   if (update)
      _parent->updateGL();
}

float QGLImage::scaleTo(Axis a, float percent, int msecs, bool viewRelative, float assumedViewScale)
{
   if (percent < 0.0 || a == Z) //invalid
      return 0.0;
   // stop running animations
   if (_scaleStep[a] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[a] = 0.0;
   }
   float iPercent = percent;
   if (viewRelative) //this means we shall fill <percent> % of the viewport on this axis
   {
      float aParentScale = assumedViewScale < 0.0 ? _parent->scaleFactor(a) : assumedViewScale/100.0;
      iPercent = -(_translation[Z] + _parent->position(Z))/3.0 * percent / aParentScale;
      if (a == Y)
         iPercent *= ((float)(_parent->height()*_basicWidth))/(_parent->width()*_basicHeight);
   }
   if (msecs == 0)
   {
      _scale[a] = iPercent/100.0;
      _parent->updateGL();
      return iPercent;
   }
   _desiredScale[a] = iPercent/100.0;
   _scaleStep[a] = (_desiredScale[a] - _scale[a]) * _parent->fpsDelay() / msecs;
   if (_scaleStep[a] != 0.0) // need  to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
   return iPercent;
}

void QGLImage::scaleTo(float xPercent, float yPercent, bool update, bool viewRelative, float assumedViewScaleX, float assumedViewScaleY)
{
   if (xPercent < 0.0 && yPercent < 0.0 ) //invalid
      return;
   // stop running animations
   if (_scaleStep[X] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[X] = 0.0;
   }
   if (_scaleStep[Y] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[Y] = 0.0;
   }
   float xScale = xPercent/100.0;
   float yScale = yPercent/100.0;
   if (viewRelative)
   {
      float xParentScale = assumedViewScaleX < 0.0 ? _parent->scaleFactor(X) : assumedViewScaleX/100.0;
      float yParentScale = assumedViewScaleY < 0.0 ? _parent->scaleFactor(Y) : assumedViewScaleY/100.0;
      xScale = -(_translation[Z] + _parent->position(Z))/3.0 * xScale / xParentScale;
      yScale = -(_translation[Z] + _parent->position(Z))/3.0 * yScale / yParentScale * (_parent->height()*_basicWidth)/(_parent->width()*_basicHeight);
   }
   _scale[X] = xScale;
   _scale[Y] = yScale;
   if (_scale[X] < 0.0) _scale[X] = _scale[Y];
   if (_scale[Y] < 0.0) _scale[Y] = _scale[X];
   if (update)
      _parent->updateGL();
}

void QGLImage::resize(int width, int height, int msecs, float assumedViewScaleX, float assumedViewScaleY)
{
   // convenience function, calcs the necessary scale factor and calls scaleTo
   // notice: the pixel size depends on the position on the Z axis
   // (as well as the view position) and the scale factor, i.e. the view size
   // if any of this changes, the pixel size will change as well!

   float xParentScale = assumedViewScaleX < 0.0 ? _parent->scaleFactor(X) : assumedViewScaleX/100.0;
   float yParentScale = assumedViewScaleY < 0.0 ? _parent->scaleFactor(Y) : assumedViewScaleY/100.0;
   float xScale = ( -(_translation[Z] + _parent->position(Z))/3.0 * width / _parent->width()) / xParentScale;
   float yScale = ( -(_translation[Z] + _parent->position(Z))/3.0 * (height * _basicWidth)/(_parent->width()*_basicHeight)) / yParentScale;

   scaleTo(X, 100.0 * xScale, msecs);
   scaleTo(Y, 100.0 * yScale, msecs);
}


void QGLImage::move(Axis a, float percent, int msecs)
{
   float v = 0;
   switch (a)
   {
   case X: v = -1.0 + percent/50.0; break;
   case Y: v = ((float)_parent->height())/_parent->width() - percent*_parent->height()/(50.0*_parent->width());break;
   case Z: v = -percent/100.0; break;
   }
   // stop running animations
   if (_translationStep[a] != 0.0)
   {
      --_activeAnimations;
      _translationStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _translation[a] += v;
      _parent->updateGL();
      return;
   }
   _desiredTranslation[a] = _translation[a] + v;
   _translationStep[a] = v * _parent->fpsDelay() / msecs;
   if (_translationStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
}
void QGLImage::move(float xPercent, float yPercent, float zPercent, bool update)
{
   // stop running animations
   if (_translationStep[X] != 0.0)
   {
      --_activeAnimations;
      _translationStep[X] = 0.0;
   }
   if (_translationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Y] = 0.0;
   }
   if (_translationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Z] = 0.0;
   }
   _translation[X] += -1.0 + xPercent/50.0;
   _translation[Y] += ((float)_parent->height())/_parent->width() - yPercent*_parent->height()/(50.0*_parent->width());
   _translation[Z] -= zPercent/100.0;
   if (update)
      _parent->updateGL();
}
void QGLImage::moveTo(Axis a, float percent, int msecs)
{
   float v = 0;
   switch (a)
   {
   case X: v = -1.0 + percent/50.0; break;
   case Y: v = ((float)_parent->height())/_parent->width() - percent*_parent->height()/(50.0*_parent->width());break;
   case Z: v = (50.0-percent)/100.0; break;
   }
   // stop running animations
   if (_translationStep[a] != 0.0)
   {
      --_activeAnimations;
      _translationStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _translation[a] = v;
      _parent->updateGL();
      return;
   }
   _desiredTranslation[a] = v;
   _translationStep[a] = (_desiredTranslation[a] - _translation[a]) * _parent->fpsDelay() / msecs;
   if (_translationStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      _parent->ensureTimerIsActive();
   }
}
void QGLImage::moveTo(float xPercent, float yPercent, float zPercent, bool update)
{
   // stop running animations
   if (_translationStep[X] != 0.0)
   {
      --_activeAnimations;
      _translationStep[X] = 0.0;
   }
   if (_translationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Y] = 0.0;
   }
   if (_translationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Z] = 0.0;
   }
   _translation[X] = -1.0 + xPercent/50.0;
   _translation[Y] = 1.0 - yPercent*_parent->height()/(50.0*_parent->width());
   _translation[Z] = (50.0-zPercent)/100.0;
   if (update)
      _parent->updateGL();
}

void QGLImage::setClipRect(int x, int y, int w, int h, bool update)
{
   _hasClipping = true;

/*   GLdouble eqn[0][4] = {1.0, 0.0, 0.0, 0.6}; // > -0.6
   GLdouble eqn[1][4] = {-1.0, 0.0, 0.0, 0.2}; // < 0.2
   GLdouble eqn[2][4] = {0.0, -1.0, 0.0, 0.5}; // > -0.5
   GLdouble eqn[3][4] = {0.0, 1.0, 0.0, 0.5}; // < 0.5*/

   float xf = -1.0 + 2.0*x/basicWidth();
   float yf = -_ratio + 2.0*_ratio*y/basicHeight();

   _clip[0][3] = -xf;
   _clip[1][3] = xf + 2.0*w/basicWidth();
   _clip[2][3] = -yf;
   _clip[3][3] = yf + 2.0*_ratio*h/basicHeight();

   if (update)
      _parent->updateGL();
}

void QGLImage::addShader(GLhandleARB shader, bool linkProgram, bool update)
{
    if (_shaders.contains( shader ))
        return; // the shader is allready included - no need for action
    _parent->makeCurrent();
    if (!_shaderProgram)
        _shaderProgram = glCreateProgramObjectARB();
    glAttachObjectARB(_shaderProgram, shader);
    _shaders.append(shader);
    if (linkProgram)
    {
        glLinkProgramARB(_shaderProgram);
        glUseProgramObjectARB(_shaderProgram); // this seems to be necessary
        _sColor = glGetUniformLocationARB(_shaderProgram, "color");
        _sAlpha = glGetUniformLocationARB(_shaderProgram, "alpha");
        _sBrightness = glGetUniformLocationARB(_shaderProgram, "brightness");
        _sInverted = glGetUniformLocationARB(_shaderProgram, "inverted");
        _sTime = glGetUniformLocationARB(_shaderProgram, "time");
        glUniform3fARB(_sColor, _color[0], _color[1], _color[2]);
        glUniform1fARB(_sAlpha, _colorI[3]);
        glUniform1fARB(_sBrightness, _brightness - 1.0);
        glUniform1fARB(_sInverted, _inverted ? 1.0 : 0.0);
        glUniform1fARB(_sTime, 0.0);
        if (update)
            _parent->updateGL();
    }
}

void QGLImage::removeShader(GLhandleARB shader, bool relinkProgram, bool update)
{
   if (!_shaders.contains( shader ))
      return; // there's no such shader - no need for action
   _shaders.removeAll ( shader );
   _parent->makeCurrent();
   glDetachObjectARB(_shaderProgram, shader);
   if (_shaders.empty())
   {
      glDeleteObjectARB(_shaderProgram);
      _shaderProgram = 0;
      if (update)
         _parent->updateGL();
   }
   else if (relinkProgram)
   {
      glLinkProgramARB(_shaderProgram);
      if (update)
         _parent->updateGL();
   }
}

void QGLImage::setShaderUniform(const int var, const int varSize, const float val1, const float val2, const float val3, const float val4)
{
   _parent->makeCurrent();
   glUseProgramObjectARB(_shaderProgram); // this seems to be necessary
   int vs = CLAMP(varSize,1,4);
   switch (vs)
   {
   case 1: glUniform1fARB(var, val1); break;
   case 2: glUniform2fARB(var, val1, val2); break;
   case 3: glUniform3fARB(var, val1, val2, val3); break;
   default: glUniform4fARB(var, val1, val2, val3, val4);
   }
}

int QGLImage::setShaderUniform(QString var, const int varSize, const float val1, const float val2, const float val3, const float val4)
{
   _parent->makeCurrent();
   glUseProgramObjectARB(_shaderProgram); // this seems to be necessary
   int id = glGetUniformLocationARB(_shaderProgram, var.toLocal8Bit().data());
   int vs = CLAMP(varSize,1,4);
   switch (vs)
   {
   case 1: glUniform1fARB(id, val1); break;
   case 2: glUniform2fARB(id, val1, val2); break;
   case 3: glUniform3fARB(id, val1, val2, val3); break;
   default: glUniform4fARB(id, val1, val2, val3, val4);
   }
   return id;
}


/*!
  Create a QGLImageViewer widget
*/

QGLImageViewer::QGLImageViewer( QWidget* parent, const char* name, float fps, bool interactive )
: QGLWidget( parent )
, _activeAnimations(0)
, _timer(0)
, _pbuffer(0L)
, _interactive(interactive)
, _animated(false)
, _animCounter(0.0)
{
    setAccessibleName( name );
    _fpsDelay = (uint)(1000/fps);
    for (int i = 0; i < 3; ++i)
    {
        _scale[i] = 1.0;
        _rotation[i] = 0.0;
        _translation[i] = 0.0;

        _scaleStep[i] = 0.0;
        _rotationStep[i] = 0.0;
        _translationStep[i] = 0.0;

        _desiredScale[i] = 0.0;
        _desiredRotation[i] = 0.0;
        _desiredTranslation[i] = 0.0;

        _canvasColor[i] = 0.0;
    }
    _translation[Z] = -3.0;
    _messageTimeOut = 0;
    _messageColor[3] = 0.0;

    makeCurrent();
    QString *exStr = new QString((char*)glGetString(GL_EXTENSIONS));
    _providesShaders = exStr->contains("GL_ARB_fragment_shader") && exStr->contains("GL_ARB_vertex_shader");
    delete exStr;
}


/*!
  Release allocated resources
*/

QGLImageViewer::~QGLImageViewer()
{
#if 0 // we can't do this while stuff is getting deleted...
   makeCurrent();
   // we cannot handle this in the image destructor as the gllists are flat copies
   // if we did, we'd loose them when copying the image into the image list and destroying the original
   QGLImageList::iterator it;
   for ( it = _images.begin(); it != _images.end(); ++it )
   {
      glDeleteLists( (*it).glObject(), 1 );
      glDeleteTextures((*it)._texAmount,  (*it)._textures);
      glDeleteLists( (*it)._blurObj, 1 );
      glDeleteTextures(1,  &(*it)._blurTex);
      // Qt deletes this one... (later on)
//       if ((*it)._textures) { delete (*it)._textures; (*it)._textures = 0L; }
   }
//       glDeleteLists( (*it).glObject(), 1 );
   _images.clear();
#endif
}

bool QGLImageViewer::isTimerActive() const
{
   return _timer;
}

void QGLImageViewer::remove(uint id)
{
   if (_images.empty())
      return;

   // we cannot handle this in the image destructor as the gllists are flat copies
   // if we did, we'd loose them when copying the image into the image list and destroying the original
   QGLImageList::iterator it2;
   for ( it2 = _images.begin(); it2 != _images.end(); ++it2 )
   {
      if ((*it2).id() == id)
      {
         ObjectCounter::Iterator it = _objectCounter.find((*it2).glObject());
         if (it != _objectCounter.end())
         {
            if (it.value() == 1)
            { // this is the only image on it's glList
               makeCurrent();
               glDeleteLists( (*it2).glObject(), 1 );
               glDeleteLists( (*it2)._blurObj, 1 );
               glDeleteTextures((*it2)._texAmount,  (*it2)._textures);
               glDeleteTextures(1,  &(*it2)._blurTex);
               if ((*it2)._textures) { delete (*it2)._textures; (*it2)._textures = 0L; };
               _objectCounter.erase(it);
            }
            else
               it.value()--;
         }
         _images.erase(it2);
         break;
      }
   }
}

void QGLImageViewer::handleAnimationsPrivate(float (*value)[3], float (*desiredValue)[3], float (*valueStep)[3], int *animCounter)
{
    if (!(*animCounter))
        return;
    for (int i = 0; i < 3; ++i)
    {
        if ((*valueStep)[i] != 0.0)
        {
            (*value)[i] += (*valueStep)[i];
            if ( ((*valueStep)[i] > 0.0 && (*value)[i] >= (*desiredValue)[i]) ||
                 ((*valueStep)[i] < 0.0 && (*value)[i] <= (*desiredValue)[i]) ) // we're done with this
            {
                (*valueStep)[i] = 0.0;
                (*value)[i] = (*desiredValue)[i]; // make sure we have a match
                if ((float*)value == _rotation)
                {
                    (*value)[i] -= (int((*value)[i])/360)*360.0f;
                    if ( (*value)[i] < 0.0f )
                        (*value)[i] = 360.0f + (*value)[i];
                }
                --(*animCounter);
                if (!(*animCounter))
                    break; // no need to test move/rotate
            }
        }
    }
}

void QGLImageViewer::timerEvent(QTimerEvent *te)
{
    if ( te->timerId() != _timer )
    {
        QGLWidget::timerEvent(te);
        return;
    }
    int allAnims = 0;
    // view ========
    if (_activeAnimations)
    {
        // scale, move, rotate
        handleAnimationsPrivate(&_scale, &_desiredScale, &_scaleStep, &_activeAnimations);
        handleAnimationsPrivate(&_translation, &_desiredTranslation, &_translationStep, &_activeAnimations);
        handleAnimationsPrivate(&_rotation, &_desiredRotation, &_rotationStep, &_activeAnimations);
        // message timer
        if (_messageTimeOut > 0)
        {
            --_messageTimeOut;
            _messageColor[3] += (float)_fpsDelay/300.0;
            _messageColor[3] = QMIN(_messageColor[3], (float)_messageTimeOut*_fpsDelay/300.0);
            _messageColor[3] = CLAMP(_messageColor[3],0.0,1.0);
            if (_messageTimeOut == 0)
                --_activeAnimations;
        }
        allAnims += _activeAnimations;
    }
    QGLImageList::iterator it;
    for ( it = _images.begin(); it != _images.end(); ++it )
    {
        // images ========
        if (!it->_activeAnimations)
            continue;
        // scale, move, rotate
        handleAnimationsPrivate(&it->_scale, &it->_desiredScale, &it->_scaleStep, &it->_activeAnimations);
        handleAnimationsPrivate(&it->_translation, &it->_desiredTranslation, &it->_translationStep, &it->_activeAnimations);
        handleAnimationsPrivate(&it->_rotation, &it->_desiredRotation, &it->_rotationStep, &it->_activeAnimations);
        // alpha
        if (it->_colorStep[3] != 0.0)
        {
            float &aS = it->_colorStep[3], &a = it->_colorI[3], &dA = it->_desiredColor[3];
            a += aS;
            if ( (aS > 0.0 && a >= dA) || (aS < 0.0 && a <= dA) ) // we're done with this
            {
                aS = 0.0;
                a = dA; // make sure we have a match
                --(it->_activeAnimations);
            }
            if (it->_shaderProgram)
            {
                makeCurrent();
                glUseProgramObjectARB(it->_shaderProgram); // this seems to be necessary
                glUniform1fARB(it->_sAlpha, a);
            }
        }
        // color
        int colorsDone = 0;
        bool needColorMerge = false;
        for (int i = 0; i < 3; ++i)
        {
            float &cS = it->_colorStep[i], &c = it->_color[i], &dC = it->_desiredColor[i];
            if ( cS == 0.0 )
                ++colorsDone;
            else
            {
                needColorMerge = true;
                c += cS; c = CLAMP(c,-0.5,1.5);
                colorsDone += (cS > 0.0 && c >= dC) || (cS < 0.0 && c <= dC);
            }
        }
        if ( needColorMerge && colorsDone > 2 )
        {
            for (int i = 0; i < 3; ++i)
            {
                it->_colorStep[i] = 0.0;
                it->_color[i] = it->_desiredColor[i]; // make sure we have a match
            }
            --(it->_activeAnimations);
        }
        if ( needColorMerge )
        {
            if (it->_shaderProgram)
            {
                makeCurrent();
                glUseProgramObjectARB(it->_shaderProgram); // this seems to be necessary
                glUniform3fARB(it->_sColor, it->_color[0], it->_color[1], it->_color[2]);
            }
            mergeCnB(*it);
        }
        // brightness
        if (it->_brightnessStep != 0.0)
        {
            float &bS = it->_brightnessStep, &b = it->_brightness, &dB = it->_desiredBrightness;
            b += bS;
            if ( (bS > 0.0 && b >= dB) || (bS < 0.0 && b <= dB) ) // we're done with this
            {
                bS = 0.0;
                b = dB; // make sure we have a match
                --(it->_activeAnimations);
            }
            if (it->_shaderProgram)
            {
                makeCurrent();
                glUseProgramObjectARB(it->_shaderProgram); // this seems to be necessary
                glUniform1fARB(it->_sBrightness, it->_brightness-1.0);
            }
            mergeCnB(*it);
        }
        // blur
        if (it->_blurStep != 0.0)
        {
            float &bS = it->_blurStep, &b = it->_blur, &dB = it->_desiredBlur;
            b += bS;
            blur(*it);
            if ( (bS > 0.0 && b >= dB) || (bS < 0.0 && b <= dB) ) // we're done with this
            {
                bS = 0.0;
                b = dB; // make sure we have a match
                --(it->_activeAnimations);
            }
        }
        allAnims += it->_activeAnimations;
    }
    Q_ASSERT( allAnims > -1 );
    if (!allAnims)
        { killTimer(_timer); _timer = 0; }
    _animCounter += 0.1;
    QGLWidget::updateGL();
}

void QGLImageViewer::paintGL()
{
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

    if (_images.empty())
    {
//       qWarning("no images available");
        return;
    }

    glPushMatrix();
    glScalef( _scale[X], _scale[Y], _scale[Z]);
    glTranslatef( _translation[X], _translation[Y], _translation[Z]);
    glRotatef( _rotation[X], 1.0, 0.0, 0.0 );
    glRotatef( _rotation[Y], 0.0, 1.0, 0.0 );
    glRotatef( _rotation[Z], 0.0, 0.0, 1.0 );
    QGLImageList::iterator it;
    for ( it = _images.begin(); it != _images.end(); ++it )
        (*it).paint();
    if (_messageTimeOut != 0)
    {
        // TODO: make this instance and only update on fontchanges
        int fontHeight = QFontInfo(font()).pixelSize();
        glDisable( GL_TEXTURE_2D );
        glDisable(GL_DEPTH_TEST);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);

        glPushAttrib( GL_TRANSFORM_BIT | GL_VIEWPORT_BIT | GL_LIST_BIT | GL_CURRENT_BIT );
        glMatrixMode( GL_PROJECTION );
        glPushMatrix();
        glLoadIdentity();
        glOrtho( 0, width(), height(), 0, -1, 1 );
        glMatrixMode( GL_MODELVIEW );
        glPushMatrix();
        glLoadIdentity();

        glColor4f(1.0-_messageColor[0], 1.0-_messageColor[1], 1.0-_messageColor[2], CLAMP(_messageColor[3], 0.0, 0.6));
        glRecti(0, _messagePos.y()-fontHeight, (int)width(), _messagePos.y()+(int)((_message.count()-1)*fontHeight*1.5)+15);
#if 0
      glLineWidth(5.0);
//       glLineStipple(1, 0x1C47);
      glBegin(GL_LINE_LOOP);
      glColor3f(0.0,0.0,1.0);
      glVertex2i(10, 10);
      glVertex2i(10, 50);
      glVertex2i((int)width()-10, 50);
      glVertex2i((int)width()-10, 10);
      glEnd();
#endif
//       glRecti(0, _messagePos.y()-2, width(), _messagePos.y()+2);

        // restore the matrix stacks and GL state
        glPopMatrix();
        glMatrixMode( GL_PROJECTION );
        glPopMatrix();
        glPopAttrib();

        glColor4fv(_messageColor);
        float i = 0.0;
        for ( QStringList::Iterator it = _message.begin(); it != _message.end(); ++it )
        {
            renderText(_messagePos.x(), (int)(_messagePos.y()+i*fontHeight), *it, font());
            i += 1.5;
        }
        glEnable(GL_DEPTH_TEST);
        glEnable( GL_TEXTURE_2D );
    }
    glPopMatrix();
    glFlush();

}


void QGLImageViewer::setCanvas(const QColor & color, bool update)
{
    makeCurrent();
    _canvasColor[0] = (float)color.red()/255.0;
    _canvasColor[1] = (float)color.green()/255.0;
    _canvasColor[2] = (float)color.blue()/255.0;
    glClearColor( _canvasColor[0], _canvasColor[1], _canvasColor[2], 1.0 );
    if (update)
        updateGL();
}

void QGLImageViewer::initializeGL()
{
    glEnable( GL_TEXTURE_2D );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, /*GL_LINEAR_MIPMAP_LINEAR*/GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);

    glDisable(GL_LIGHTING);

    glEnable(GL_DEPTH_TEST);
//    glDepthMask (GL_TRUE );
    glDepthFunc(GL_LEQUAL);
//     glDepthFunc(GL_ALWAYS);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
//    glBlendEquationEXT(GL_FUNC_ADD_EXT);
//    glEnable(GL_COLOR_LOGIC_OP);
//    glLogicOp(GL_CLEAR); // 1bit, all black
//    glLogicOp(GL_SET); // 1bit, all white
//    glLogicOp(GL_COPY); // RGB, no A
//    glLogicOp(GL_COPY_INVERTED); // RGB, no A inverted
//    glLogicOp(GL_NOOP); // "transparent"
//    glLogicOp(GL_INVERT); // 1bit, inverted
//    glLogicOp(GL_AND); // 1bit, combine
//    glLogicOp(GL_NAND); // 1bit, xor
//    glLogicOp(GL_OR); // RGB, fails with black on white
//    glLogicOp(GL_NOR); // RGB, fails with white on black?
//    glLogicOp(GL_XOR); // RGB, no A, white inverts bg.
//    glLogicOp(GL_EQUIV);
//    glLogicOp(GL_AND_REVERSE);
//    glLogicOp(GL_AND_INVERTED);
//    glLogicOp(GL_OR_REVERSE);
//    glLogicOp(GL_OR_INVERTED);

    glEnable(GL_LINE_STIPPLE);

    glClearColor( _canvasColor[0], _canvasColor[1], _canvasColor[2], 1.0 );

//     glEnable( GL_CULL_FACE );  	// don't need Z testing for convex objects

    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);
    glHint( GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST );
    glHint( GL_POINT_SMOOTH_HINT, GL_NICEST );
    glHint( GL_LINE_SMOOTH_HINT, GL_NICEST );
    glHint( GL_POLYGON_SMOOTH_HINT, GL_NICEST );
    glShadeModel (GL_SMOOTH);

//    GLfloat value1[2], value2;
//    glGetFloatv(GL_LINE_WIDTH_RANGE, value1);
//    glGetFloatv(GL_LINE_WIDTH_GRANULARITY, &value2);
//    qWarning("%f - %f, %f",value1[0],value1[1],value2);
//    qWarning("%s",glGetString(GL_EXTENSIONS));
}
/*
GL_ARB_depth_texture GL_ARB_fragment_program GL_ARB_fragment_program_shadow GL_ARB_fragment_shader GL_ARB_half_float_pixel GL_ARB_imaging GL_ARB_multisample GL_ARB_multitexture GL_ARB_occlusion_query GL_ARB_pixel_buffer_object GL_ARB_point_parameters GL_ARB_point_sprite GL_ARB_shadow GL_ARB_shader_objects GL_ARB_shading_language_100 GL_ARB_texture_border_clamp GL_ARB_texture_compression GL_ARB_texture_cube_map GL_ARB_texture_env_add GL_ARB_texture_env_combine GL_ARB_texture_env_dot3 GL_ARB_texture_mirrored_repeat GL_ARB_texture_rectangle GL_ARB_transpose_matrix GL_ARB_vertex_buffer_object GL_ARB_vertex_program GL_ARB_vertex_shader GL_ARB_window_pos GL_S3_s3tc GL_EXT_texture_env_add GL_EXT_abgr GL_EXT_bgra GL_EXT_blend_color GL_EXT_blend_func_separate GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_compiled_vertex_array GL_EXT_Cg_shader GL_EXT_draw_range_elements GL_EXT_fog_coord GL_EXT_framebuffer_object GL_EXT_multi_draw_arrays GL_EXT_packed_depth_stencil GL_EXT_packed_pixels GL_EXT_paletted_texture GL_EXT_pixel_buffer_object GL_EXT_point_parameters GL_EXT_rescale_normal GL_EXT_secondary_color GL_EXT_separate_specular_color GL_EXT_shadow_funcs GL_EXT_shared_texture_palette GL_EXT_stencil_two_side GL_EXT_stencil_wrap GL_EXT_texture3D GL_EXT_texture_compression_s3tc GL_EXT_texture_cube_map GL_EXT_texture_edge_clamp GL_EXT_texture_env_combine GL_EXT_texture_env_dot3 GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod GL_EXT_texture_lod_bias GL_EXT_texture_object GL_EXT_texture_sRGB GL_EXT_timer_query GL_EXT_vertex_array GL_HP_occlusion_test GL_IBM_rasterpos_clip GL_IBM_texture_mirrored_repeat GL_KTX_buffer_region GL_NV_blend_square GL_NV_copy_depth_to_color GL_NV_depth_clamp GL_NV_fence GL_NV_float_buffer GL_NV_fog_distance GL_NV_fragment_program GL_NV_fragment_program_option GL_NV_half_float GL_NV_light_max_exponent GL_NV_multisample_filter_hint GL_NV_occlusion_query GL_NV_packed_depth_stencil GL_NV_pixel_data_range GL_NV_point_sprite GL_NV_primitive_restart GL_NV_register_combiners GL_NV_register_combiners2 GL_NV_texgen_reflection GL_NV_texture_compression_vtc GL_NV_texture_env_combine4 GL_NV_texture_expand_normal GL_NV_texture_rectangle GL_NV_texture_shader GL_NV_texture_shader2 GL_NV_texture_shader3 GL_NV_vertex_array_range GL_NV_vertex_array_range2 GL_NV_vertex_program GL_NV_vertex_program1_1 GL_NV_vertex_program2 GL_NV_vertex_program2_option GL_SGIS_generate_mipmap GL_SGIS_texture_lod GL_SGIX_depth_texture GL_SGIX_shadow GL_SUN_slice_accum
*/

/*!
  Set up the OpenGL view port, matrix mode, etc.
*/

void QGLImageViewer::resizeGL( int w, int h )
{
   glViewport( 0, 0, w, h );
   glMatrixMode( GL_PROJECTION );
   glLoadIdentity();
   glFrustum( -1.0, 1.0, -1.0*h/w, 1.0*h/w, 3.0, 50.0 );
   glMatrixMode( GL_MODELVIEW );
   glLoadIdentity();
//    glTranslatef( 0.0, 0.0, -3.0 );
}

void QGLImageViewer::mousePressEvent ( QMouseEvent * e )
{
    if (!_interactive)
        return;
    _lastPos = e->pos();
    _scaleTarget[X] = _translation[X] - (2.0*e->pos().x()/width()-1.0)/(_scale[X]*3.0/-_translation[Z]);
    _scaleTarget[Y] = _translation[Y] + (2.0*e->pos().y()-height())/((_scale[Y]*3.0/-_translation[Z])*width());
}

void QGLImageViewer::mouseReleaseEvent ( QMouseEvent *  )
{
    if (!_interactive)
        return;
    setCursor(Qt::ArrowCursor);
}

void QGLImageViewer::mouseMoveEvent ( QMouseEvent * e )
{
    if (!_interactive)
        return;

    if (e->modifiers() & Qt::ControlModifier) // rotate
    {
        setCursor(Qt::SizeAllCursor);
        int sign = ((int)_rotation[X]%360 > 90 && (int)_rotation[X]%360 < 270) ? -1 : 1;
        if (e->modifiers() & Qt::ShiftModifier) // X-Axis
        {
            rotate(X, (float)(e->pos().y() - _lastPos.y())/3.0);
//         if (e->modifiers() & Qt::AltModifier) // Y-Axis
            rotate(Y, (float)(sign*(e->pos().x() - _lastPos.x()))/3.0);
        }
        else  // Z-Axis
        {
            if ((int)_rotation[Y]%360 > 90 && (int)_rotation[Y]%360 < 270)
                sign = -sign;
            float diff1 = 180.0*(e->pos().x() - _lastPos.x())/width();
            if (e->pos().y() < height()/2)
                diff1 *= sign;
            else
                diff1 *= -sign;
            float diff2 = 180.0*(e->pos().y() - _lastPos.y())/width();
            if (e->pos().x() > width()/2)
                diff2 *= sign;
            else
                diff2 *= -sign;
            rotate(Z, diff1 + diff2);
        }
        _lastPos = e->pos();
        updateGL();
        return;
    }

    if (e->modifiers() & Qt::ShiftModifier) //scale
    {
        setCursor(Qt::BlankCursor);
        // scale
        float diff = 1.0 + (float)(e->pos().y() - _lastPos.y())/80.0;
        _lastPos = e->pos();
        if (_scale[X] * diff != 0.0)
            _scale[X] *= diff;
        if (_scale[Y] * diff != 0.0)
            _scale[Y] *= diff;
        // and shift the image (i.e. zoom to cursor)
        _translation[X] += (_scaleTarget[X]-_translation[X])/6.0;
        _translation[Y] += (_scaleTarget[Y]-_translation[Y])/6.0;
        updateGL();
        return;
    }

    setCursor(Qt::BlankCursor);
    _translation[X] += 2.0*(e->pos().x() - _lastPos.x())/(_scale[X]*3.0/-_translation[Z]*width());
    _translation[Y] -= 2.0*(e->pos().y() - _lastPos.y())/(_scale[Y]*3.0/-_translation[Z]*width());
    _lastPos = e->pos();
    updateGL();
}

void QGLImageViewer::wheelEvent ( QWheelEvent * )
{
//     if (!_interactive)
//         return;
}

uint QGLImageViewer::newUniqueId()
{
    uint maxId = 0; int i;
    QGLImageList::iterator it;
    for ( it = _images.begin(); it != _images.end(); ++it )
    {
        if ((*it).id() > maxId)
            maxId = (*it).id();
    }

    maxId+=2;
    bool *ids = new bool[maxId];
    for (i = 0; i < maxId; ++i)
        ids[i] = false;
    for ( it = _images.begin(); it != _images.end(); ++it )
        ids[(*it).id()] = true;
    i = 0;
    while (ids[i]) ++i;
    delete [] ids;
    return i;
}



int QGLImageViewer::load( const QString& imgPath, bool show , int numPol )
{
   QImage *buf = new QImage();
   if ( !buf->load( imgPath ) )
   {
      qWarning( "Image %s not found - not loaded", imgPath.toLatin1().data() );
      return -1;
   }
   uint ret = load(*buf, show, numPol);
   delete buf;
   return ret;
}

uint QGLImageViewer::load( const QPixmap& pix, bool show , int numPol )
{
   QImage *buf = new QImage();
   *buf = pix.toImage();
   uint ret = load(*buf, show, numPol);
   delete buf;
   return ret;
}

uint QGLImageViewer::load( const QGLImage &image, bool show )
{
   uint id = newUniqueId();
   _images.append(QGLImage(this, id, image.glObject(), image._textures, image._texAmount, image.basicWidth(), image.basicHeight()));
   _images.last()._blurObj = image._blurObj; _images.last()._blurTex = image._blurTex;
   ObjectCounter::Iterator it = _objectCounter.find(image.glObject());
   if (it == _objectCounter.end())
      _objectCounter.insert(image.glObject(), 1);
   else
      it.value()++;
   if (show) _images.last().show(false);
   return id;
}

uint QGLImageViewer::load( const QImage& img, bool show , int numPol, bool isGLFormat )
{
    makeCurrent();

    // TODO check GL texture abilities and simplify this in case...
    /* Ok, textures need to be of 2^m[+2*f], where m is any positive integer and f is 1 or 0 -
     * depending on whether you want a frame.
     * Unfortunately images usually don't match these conditions so they have to be somehow transformed.
     * Usually one would simply scale the image and just press it into a proper sized rect, but this
     * sounds inefficient, first as scaling (without texture HW scalers...) is slow and second
     * you'll have to e.g. create a 2048x2048 texture for a 1600x1200 image.
     * Now we simply split the image into several parts that all are sized with powers of 2 and in
     * case the image size isn't dividable by 64 (minimum texture size) just let the texture
     * overflow the rect a bit.
     * (i think at least about the tiling, gliv works similar - i have not checked it's code however
     * but try to reevent the wheel, as i want to learn a bit about OpenGL and how to trick on it ;)
     */

    int _w_ = img.width(); if (_w_%64) _w_ += 64 - (_w_%64);
    int _h_ = img.height(); if (_h_%64) _h_ += 64 - (_h_%64);
    int _w, _h, handledW = 0, handledH = 0, i = 0;

    int maxTexSize = -1;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);

    // count the tile demand ----------------------------------
    while (handledW < _w_)
    {
        _w = 64;
        while ((_w<<1) <= maxTexSize && handledW+(_w<<1) <= _w_)
            _w = (_w<<1);
        handledW += _w;
        ++i;
    }
    _w = i;
    int w[_w];

    i = 0;
    while (handledH < _h_)
    {
        _h = 64;
        while ((_h<<1) <= maxTexSize && handledH+(_h<<1) <= _h_)
            _h = (_h<<1);
        handledH += _h;
        ++i;
    }
    int h[i];
    // ========================================================

    int texCounter = _w*i;
    GLuint *textures = new GLuint[texCounter];
    glGenTextures(texCounter, textures);
    handledW = 0; handledH = 0; i = 0;

    // create size arrays -------------------------------------
    while (handledW < _w_)
    {
        _w = 64;
        while ((_w<<1) <= maxTexSize && handledW+(_w<<1) <= _w_)
            _w = (_w<<1);
        w[i] = _w;
        handledW += _w;
        ++i;
    }
    _w = i;

    i = 0;
    while (handledH < _h_)
    {
        _h = 64;
        while ((_h<<1) <= maxTexSize && handledH+(_h<<1) <= _h_)
            _h = (_h<<1);
        h[i] = _h;
        handledH += _h;
        ++i;
    }
    _h = i;
    // ========================================================

    QImage tile;
    handledW = 0;
    texCounter = 0;

    // create texture ------------------------------------------
    for (i = 0; i < _w; ++i)
    {
        handledH = 0;
        for (int j = 0; j < _h; ++j)
        {
            if ( isGLFormat )
                tile = img.copy( handledW, img.height() - ( handledH + h[j] ), w[i], h[j] );
            else
            {
                tile = img.copy( handledW, handledH, w[i], h[j] );
                tile = convertToGLFormat( tile );  // flipped 32bit RGBA
            }

            glBindTexture(GL_TEXTURE_2D, textures[texCounter]); ++texCounter;
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, /*GL_LINEAR_MIPMAP_LINEAR*/GL_LINEAR );
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexImage2D( GL_TEXTURE_2D, 0, 4, tile.width(), tile.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, tile.bits() );
            handledH += h[j];
        }
        handledW += w[i];
    }
    tile = QImage();
    // ========================================================

    float left, right, top, bottom;
    float ratio = ((float)img.height())/img.width();

    handledW = 0;
    texCounter = 0;

    // create gllist, i.e. the image ----------------------------
    GLuint texImage = glGenLists(1);
    glNewList( texImage, GL_COMPILE );

    int polyCount = 0;
    float steph = 0.5*sqrt((float)numPol/ratio); // half the amount of horizontal polygons

    for (i = 0; i < _w; ++i)
    {
        handledH = 0;
        for (int j = 0; j < _h; ++j)
        {

            glBindTexture(GL_TEXTURE_2D, textures[texCounter]); ++texCounter;

            left = -1.0 + 2.0*handledW/img.width();

            if (handledW + w[i] > img.width())
                _w_ = img.width()-handledW;
            else
                _w_ = w[i];

            right = -1.0 + 2.0*(handledW + _w_)/img.width();
            top = -1.0*ratio + 2.0*ratio*(img.height()-handledH)/img.height();
            if (handledH + h[j] > img.height())
                _h_ = img.height()-handledH;
            else
                _h_ = h[j];
            handledH += _h_;
            bottom = -1.0*ratio + 2.0*ratio*(img.height() - handledH)/img.height();

//          glBegin(GL_QUADS);
//          glTexCoord2f(0.0, 1.0-((float)_h_/h[j])); glVertex3f(left, bottom, 0.0);
//          glTexCoord2f(0.0, 1.0); glVertex3f(left, top, 0.0);
//          glTexCoord2f((float)_w_/w[i], 1.0); glVertex3f(right, top, 0.0);
//          glTexCoord2f((float)_w_/w[i], 1.0-((float)_h_/h[j])); glVertex3f(right, bottom, 0.0);
//          glEnd();

            float xsteps = (top-bottom)*steph*ratio;
            int vsteps = (int)xsteps; if ((xsteps - vsteps) >= 0.5) vsteps += 1;
            xsteps = (int)((right-left)*steph);
            int hsteps = (int)xsteps; if ((xsteps - hsteps) >= 0.5) hsteps += 1;
            if (!vsteps) vsteps = 1; if (!hsteps) hsteps = 1;
            polyCount += vsteps*hsteps;
            float vi = (top-bottom)/vsteps;
            float vc = bottom;
            float tvc = 1.0-((float)_h_/h[j]);
            float tvi = (1.0 - tvc)/vsteps;
            float hi = (right-left)/hsteps;
            float thi = ((float)_w_/w[i])/hsteps;
            float hc, thc;
            for (int i = 0; i < vsteps; ++i)
            {
                hc = left; thc = 0.0;
                glBegin(GL_TRIANGLE_STRIP);
                for (int j = 0; j < hsteps; ++j)
                {
                    glTexCoord2f(thc, tvc); glVertex3f(hc, vc, 0.0);
                    glTexCoord2f(thc, tvc+tvi); glVertex3f(hc, vc+vi, 0.0);
                    hc += hi;
                    thc += thi;
                }
                glTexCoord2f(thc, tvc); glVertex3f(hc, vc, 0.0);
                glTexCoord2f(thc, tvc+tvi); glVertex3f(hc, vc+vi, 0.0);
                glEnd();
                vc += vi;
                tvc +=tvi;
            }
        }
        handledW += w[i];
    }
    // ========================================================

    glEndList();
#if DO_DEBUG
    qDebug() <<polyCount << "Sqares";
#endif

    uint id = newUniqueId();

    _images.append(QGLImage(this, id, texImage, textures, texCounter, img.width(), img.height(), !isGLFormat && img.hasAlphaChannel()));

    _images.last()._numPol = numPol;

    _objectCounter.insert(texImage, 1);

    if (show) _images.last().show(false);
    return id;
}

void QGLImageViewer::blur(QGLImage &img)
{
   // figure out text size
   int texW = img._blurW, texH = img._blurH;
   if (!texW)
   {
      texW = 128;
      while (texW < 513 && texW < img.basicWidth()+1)
         texW = (texW << 1);
      texW = (texW >> 1);
      img._blurW = texW;
   }

   if (!texH)
   {
      texH = 128;
      while (texH < 513 && texH < img.basicHeight()+1)
         texH = (texH << 1);
      texH = (texH >> 1);
      img._blurH = texH;
   }

   float ratio = ((float)img.basicHeight())/img.basicWidth();
   int numPass = (int)img._blur;

   float alpha = img._blur - numPass;
   if (alpha < 0.001)
      alpha = 0.0;
   else if (1.0-alpha < 0.001)
   {
      ++numPass; alpha = 0.0;
   }

   if (!_pbuffer)
   {
      _pbuffer = new QGLPixelBuffer( QSize(512, 512), format(), this );
      _pbuffer->makeCurrent();

      // init the pbuffer
      glEnable( GL_TEXTURE_2D );
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

      glDisable(GL_LIGHTING);
      glDisable(GL_DEPTH_TEST);
      glEnable(GL_BLEND);
      glEnable(GL_LINE_SMOOTH);
      glEnable(GL_POINT_SMOOTH);
      glHint( GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST );
      glHint( GL_POINT_SMOOTH_HINT, GL_NICEST );
      glHint( GL_LINE_SMOOTH_HINT, GL_NICEST );
      glHint( GL_POLYGON_SMOOTH_HINT, GL_NICEST );
      glShadeModel (GL_SMOOTH);
      // done
   }
   else
      _pbuffer->makeCurrent();

   // set up a plain viewport of matching size
   glViewport(0,0,texW,texH);

   glMatrixMode(GL_PROJECTION);
   glPushMatrix();
   glLoadIdentity();
   glOrtho( -1.0, 1.0, -1.0, 1.0, -100.0, 100.0 );

   glMatrixMode(GL_MODELVIEW);

   if (!img._blurTex)
   {
      int size = (texW * texH)* 4 * sizeof(uint);
      uint *data = (uint*)new GLuint[size];
      glGenTextures(1, &img._blurTex);
      glBindTexture(GL_TEXTURE_2D,  img._blurTex);
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
      glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
      glTexImage2D( GL_TEXTURE_2D, 0, 4, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );
      delete [] data;
   }

   if (!img._blurObj)
   {
      makeCurrent();
      // create new GLlist with the blur texture
      img._blurObj = glGenLists(1);
      glNewList( img._blurObj, GL_COMPILE );
      glBindTexture(GL_TEXTURE_2D, img._blurTex);
//       glBegin(GL_QUADS);
//       glTexCoord2f(0.0, 0.0); glVertex3f(-1.0, -1.0*ratio, 0.0); //topleft
//       glTexCoord2f(0.0, 1.0); glVertex3f(-1.0, 1.0*ratio, 0.0); //bottomleft
//       glTexCoord2f(1.0, 1.0); glVertex3f(1.0, 1.0*ratio, 0.0); //bottomright
//       glTexCoord2f(1.0, 0.0); glVertex3f(1.0, -1.0*ratio, 0.0); //topright
//       glEnd();


      float steph = 0.5*sqrt((float)img._numPol/ratio); // half the amount of horizontal polygons
      float xsteps = 2.0*steph*ratio;
      int vsteps = (int)xsteps; if ((xsteps - vsteps) >= 0.5) vsteps += 1;
      xsteps = (int)(2.0*steph);
      int hsteps = (int)xsteps; if ((xsteps - hsteps) >= 0.5) hsteps += 1;
      if (!vsteps) vsteps = 1; if (!hsteps) hsteps = 1;
      float vi = 2.0*ratio/vsteps;
      float vc = -1.0*ratio;
      float tvc = 0.0;
      float tvi = 1.0/vsteps;
      float hi = 2.0/hsteps;
      float thi = 1.0/hsteps;
      float hc, thc;
      for (int i = 0; i < vsteps; ++i)
      {
         hc = -1.0; thc = 0.0;
         glBegin(GL_TRIANGLE_STRIP);
         for (int j = 0; j < hsteps; ++j)
         {
            glTexCoord2f(thc, tvc); glVertex3f(hc, vc, 0.0);
            glTexCoord2f(thc, tvc+tvi); glVertex3f(hc, vc+vi, 0.0);
            hc += hi;
            thc += thi;
         }
         glTexCoord2f(thc, tvc); glVertex3f(hc, vc, 0.0);
         glTexCoord2f(thc, tvc+tvi); glVertex3f(hc, vc+vi, 0.0);
         glEnd();
         vc += vi;
         tvc +=tvi;
      }

      glEndList();
   }

   _pbuffer->makeCurrent();

    glPushMatrix();
    glLoadIdentity();
    const float color[4] = { 1.0, 1.0, 1.0, 1.0/(2*numPass+1) };
    glColor4fv(color);
    glScalef( 1.0, 1.0/ratio, 1.0 );

    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

   if (img._blurType == 1)
   {
      const float factor = 4.0/texW;
      const float factor2 = 4.0/texH;
      for (int i = -numPass; i <= numPass; ++i)
      {
         glPushMatrix();
            glScalef( 1.0+factor*i, 1.0+factor2*i, 1.0 ); glCallList( img.glObject() );
         glPopMatrix();
      }
      glBindTexture(GL_TEXTURE_2D,  img._blurTex);
      glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, texW, texH, 0);

      if (alpha != 0.0)
      {
         ++numPass;
         glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
         glColor4f(1.0, 1.0, 1.0, 1.0-2.0/(2*numPass+1));
         glCallList( img._blurObj );
         glColor4f(1.0, 1.0, 1.0, 1.0/(2*numPass+1));
         glPushMatrix();
            glScalef( 1.0-numPass*factor, 1.0-numPass*factor2, 1.0); glCallList( img.glObject() );
         glPopMatrix();
//          glBlendFunc(GL_SRC_ALPHA, GL_ONE);
         glPushMatrix();
            glScalef( 1.0+numPass*factor, 1.0+numPass*factor2, 1.0); glCallList( img.glObject() );
         glPopMatrix();
         glBlendFunc(GL_SRC_ALPHA, GL_CONSTANT_ALPHA);
         glBlendColor(1.0, 1.0,  1.0, alpha);
         glColor4f(1.0, 1.0, 1.0, (1.0-alpha)); glCallList( img._blurObj );
         glBlendFunc(GL_SRC_ALPHA, GL_ONE);
         --numPass;
         glColor4fv(color);
         glBindTexture(GL_TEXTURE_2D,  img._blurTex);
         glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, texW, texH, 0);
      }

   }
   else
   {
      float factor = 2.0/texW;

      for (int i = -numPass; i <= numPass; ++i)
      {
         glPushMatrix();
            glTranslatef( factor*i, 0.0, 0.0 ); glCallList( img.glObject() );
         glPopMatrix();
      }
      glBindTexture(GL_TEXTURE_2D,  img._blurTex);
      glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, texW, texH, 0);

      if (alpha != 0.0)
      {
         ++numPass;
         glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
         glColor4f(1.0, 1.0, 1.0, 1.0-2.0/(2*numPass+1));
         glCallList( img._blurObj );
         glColor4f(1.0, 1.0, 1.0, 1.0/(2*numPass+1));
         glPushMatrix();
            glTranslatef( -numPass*factor, 0.0, 0.0); glCallList( img.glObject() );
         glPopMatrix();
         glPushMatrix();
            glTranslatef( numPass*factor, 0.0, 0.0); glCallList( img.glObject() );
         glPopMatrix();
         glBlendFunc(GL_SRC_ALPHA, GL_CONSTANT_ALPHA);
         glBlendColor(1.0, 1.0,  1.0, alpha);
         glColor4f(1.0, 1.0, 1.0, (1.0-alpha)); glCallList( img._blurObj );
         glBlendFunc(GL_SRC_ALPHA, GL_ONE);
         --numPass;
         glColor4fv(color);
         glBindTexture(GL_TEXTURE_2D,  img._blurTex);
         glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, texW, texH, 0);
      }


      factor = 2.0/texH;

      glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
      for (int i = -numPass; i <= numPass; ++i)
      {
         glPushMatrix();
            glTranslatef( 0.0, factor*i, 0.0); glCallList( img._blurObj );
         glPopMatrix();
      }
      glBindTexture(GL_TEXTURE_2D,  img._blurTex);
      glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, texW, texH, 0);

      if (alpha != 0.0)
      {
         ++numPass;
         glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
         glColor4f(1.0, 1.0, 1.0, 1.0-2.0/(2*numPass+1));
         glCallList( img._blurObj );
         glColor4f(1.0, 1.0, 1.0, 1.0/(2*numPass+1));
         glPushMatrix();
            glTranslatef( 0.0, -numPass*factor, 0.0); glCallList( img._blurObj );
         glPopMatrix();
         glPushMatrix();
            glTranslatef( 0.0, numPass*factor, 0.0); glCallList( img._blurObj );
         glPopMatrix();
         glBlendFunc(GL_SRC_ALPHA, GL_CONSTANT_ALPHA);
         glBlendColor(1.0, 1.0,  1.0, alpha);
         glColor4f(1.0, 1.0, 1.0, (1.0-alpha)); glCallList( img._blurObj );
         glBindTexture(GL_TEXTURE_2D,  img._blurTex);
         glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, texW, texH, 0);
      }
   }

   glPopMatrix();

   _pbuffer->doneCurrent();
}

void QGLImageViewer::ensureTimerIsActive()
{
   if (!_timer)
       _timer = startTimer(_fpsDelay);
}

void QGLImageViewer::rotate(Axis a, float degrees, int msecs)
{
    // stop running animations
   if (_rotationStep[a] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[a] = 0.0;
   }
   float deg = a == Z ? -degrees : degrees;
   if (msecs == 0)
   {
      _rotation[a] += deg;
      updateGL();
      return;
   }
   _desiredRotation[a] = _rotation[a] + deg;
   _rotationStep[a] = deg * _fpsDelay / msecs;
   if (_rotationStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      ensureTimerIsActive();
   }
}
void QGLImageViewer::rotate(float xDegrees, float yDegrees, float zDegrees, bool update)
{
   // stop running animations
   if (_rotationStep[X] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[X] = 0.0;
   }
   if (_rotationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Y] = 0.0;
   }
   if (_rotationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Z] = 0.0;
   }
   _rotation[X] += xDegrees;
   _rotation[Y] += yDegrees;
   _rotation[Z] -= zDegrees;
   for (int a = 0; a < 3; ++a)
   {
       _rotation[a] -= (int(_rotation[a])/360)*360.0f;
       if ( _rotation[a] < 0.0f )
           _rotation[a] = 360.0f + _rotation[a];
   }
   if (update)
      updateGL();
}
void QGLImageViewer::rotateTo(Axis a, float degrees, int msecs)
{
   // stop running animations
   if (_rotationStep[a] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _rotation[a] = degrees;
      updateGL();
      return;
   }
   //======
   _desiredRotation[a] = degrees;
   float diff = _desiredRotation[a] - _rotation[a];
   while (diff < 0.0) diff += 360.0;
   while (diff > 360.0) diff = 360.0;
   if (diff > 180.0) diff = 360.0 - diff;
   _rotationStep[a] = diff * _fpsDelay / msecs;
   if (_rotationStep[a] != 0.0) // need to animate
   {
       if ( _desiredRotation[a] < _rotation[a] )
           _rotationStep[a] = -_rotationStep[a];
      ++_activeAnimations;
      ensureTimerIsActive();
   }
}

void QGLImageViewer::rotateTo(float xDegrees, float yDegrees, float zDegrees, bool update)
{
   // stop running animations
   if (_rotationStep[X] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[X] = 0.0;
   }
   if (_rotationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Y] = 0.0;
   }
   if (_rotationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _rotationStep[Z] = 0.0;
   }
   _rotation[X] = xDegrees;
   _rotation[Y] = yDegrees;
   _rotation[Z] = zDegrees;
   for (int a = 0; a < 3; ++a)
   {
      while (_rotation[a] > 360.0)
         _rotation[a] -= 360.0;
      while (_rotation[a] < 0.0)
         _rotation[a] += 360.0;
   }
   if (update)
      updateGL();
}

void QGLImageViewer::scale(Axis a, float percent, int msecs)
{
   if (a == Z) //invalid
      return;
   // stop running animations
   if (_scaleStep[a] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _scale[a] *= percent/100.0;
      updateGL();
      return;
   }
   _desiredScale[a] = _scale[a]*percent/100.0;
   _scaleStep[a] = (_desiredScale[a] - _scale[a]) * _fpsDelay / msecs;
   if (_scaleStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      ensureTimerIsActive();
   }
}
void QGLImageViewer::scale(float xPercent, float yPercent, bool update)
{
   // stop running animations
   if (_scaleStep[X] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[X] = 0.0;
   }
   if (_scaleStep[Y] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[Y] = 0.0;
   }
   _scale[X] *= xPercent;
   _scale[Y] *= yPercent;
   if (update)
      updateGL();
}
void QGLImageViewer::scaleTo(Axis a, float percent, int msecs)
{
   if (a == Z) //invalid
      return;
   // stop running animations
   if (_scaleStep[a] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _scale[a] = percent/100.0;
      updateGL();
      return;
   }
   _desiredScale[a] = percent/100.0;
   _scaleStep[a] = (_desiredScale[a] - _scale[a]) * _fpsDelay / msecs;
   if (_scaleStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      ensureTimerIsActive();
   }
}
void QGLImageViewer::scaleTo(float xPercent, float yPercent, bool update)
{
   // stop running animations
   if (_scaleStep[X] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[X] = 0.0;
   }
   if (_scaleStep[Y] != 0.0)
   {
      --_activeAnimations;
      _scaleStep[Y] = 0.0;
   }
   _scale[X] = xPercent/100.0;
   _scale[Y] = yPercent/100.0;
   if (update)
      updateGL();
}

void QGLImageViewer::move(Axis a, float percent, int msecs)
{
   float v = 0;
   switch (a)
   {
   case X: v = percent/100.0; break;
   case Y: v = -percent*height()/(100.0*width()); break;
   case Z: v = -50.0*percent/100.0; break;
   }
   // stop running animations
   if (_translationStep[a] != 0.0)
   {
      --_activeAnimations;
      _translationStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _translation[a] += v;
      updateGL();
      return;
   }
   _desiredTranslation[a] = _translation[a] + v;
   _translationStep[a] = v * _fpsDelay / msecs;
   if (_translationStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      ensureTimerIsActive();
   }
}
void QGLImageViewer::move(float xPercent, float yPercent, float zPercent, bool update)
{
      // stop running animations
   if (_translationStep[X] != 0.0)
   {
      --_activeAnimations;
      _translationStep[X] = 0.0;
   }
   if (_translationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Y] = 0.0;
   }
   if (_translationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Z] = 0.0;
   }
   _translation[X] += -1.0 + xPercent/50.0;
   _translation[Y] += ((float)height())/width() - yPercent*height()/(50.0*width());
   _translation[Z] -= 50.0*zPercent/100.0;
   if (update)
      updateGL();
}
void QGLImageViewer::moveTo(Axis a, float percent, int msecs)
{
   float v = 0;
   switch (a)
   {
   case X: v = -1.0 + percent/50.0; break;
   case Y: v = ((float)height())/width() - percent*height()/(50.0*width());break;
   case Z: v = -3.0 - 47.0*percent/100.0; break;
   }
   // stop running animations
   if (_translationStep[a] != 0.0)
   {
      --_activeAnimations;
      _translationStep[a] = 0.0;
   }
   if (msecs == 0)
   {
      _translation[a] = v;
      updateGL();
      return;
   }
   _desiredTranslation[a] = v;
   _translationStep[a] = (_desiredTranslation[a] - _translation[a]) * _fpsDelay / msecs;
   if (_translationStep[a] != 0.0) // need to animate
   {
      ++_activeAnimations;
      ensureTimerIsActive();
   }
}
void QGLImageViewer::moveTo(float xPercent, float yPercent, float zPercent, bool update)
{
   // stop running animations
   if (_translationStep[X] != 0.0)
   {
      --_activeAnimations;
      _translationStep[X] = 0.0;
   }
   if (_translationStep[Y] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Y] = 0.0;
   }
   if (_translationStep[Z] != 0.0)
   {
      --_activeAnimations;
      _translationStep[Z] = 0.0;
   }
   _translation[X] = -1.0 + xPercent/50.0;
   _translation[Y] = ((float)height())/width() - yPercent*height()/(50.0*width());
   _translation[Z] = -3.0 - 47.0*zPercent/100.0;
   if (update)
      updateGL();
}

void QGLImageViewer::hideMessage()
{
   _messageTimeOut = 300 / _fpsDelay;
   if (_messageTimeOut < 0) // stop message
   {
      ++_activeAnimations;
      ensureTimerIsActive();
   }
}

void QGLImageViewer::message(int x, int y, QString message, int msecs, const QColor *color)
{
   if (!color)
   {
      _messageColor[0] = (float)palette().color(QPalette::Active, QPalette::Foreground).red()/255.0;
      _messageColor[1] = (float)palette().color(QPalette::Active, QPalette::Foreground).green()/255.0;
      _messageColor[2] = (float)palette().color(QPalette::Active, QPalette::Foreground).blue()/255.0;
   }
   else
   {
      _messageColor[0] = (float)color->red()/255.0;
      _messageColor[1] = (float)color->green()/255.0;
      _messageColor[2] = (float)color->blue()/255.0;
   }
   if (_messageTimeOut > 0) // stop message
   {
      --_activeAnimations;
      _messageTimeOut = -1;
   }
   _message = message.split("\n");
   _messagePos = QPoint(x,y+QFontInfo(font()).pixelSize());
   if (msecs > 0)
   {
      _messageTimeOut = msecs / _fpsDelay;
      if (_messageTimeOut > 0) // animate!
      {
         ++_activeAnimations;
         ensureTimerIsActive();
      }
   }
   else
   {
      _messageColor[3] = 1.0;
      updateGL();
   }
}

void QGLImageViewer::animate(bool yes)
{
    if (yes)
    {
        if (_animated)
            return; // stupid user calls us to animate, but we're allready
        _animated = true;
        ++_activeAnimations;
        ensureTimerIsActive();
    }
    else
    {
        _animated = false;
        --_activeAnimations;
    }
}

void QGLImageViewer::updateGL()
{
    if (_timer)
        return;
    QGLWidget::updateGL();
}

GLhandleARB QGLImageViewer::loadShader(QString file, GLenum shaderType)
{
    QFile f(file);
    if (f.open( QIODevice::ReadOnly ))
    {
        makeCurrent();
        GLhandleARB shader = glCreateShaderObjectARB(shaderType);
        QTextStream ts( &f );
        QString sourceCode = ts.readAll();
        f.close();
        QByteArray ba = sourceCode.toLocal8Bit();
        const char * sc = ba.data();
        glShaderSourceARB(shader, 1, &sc, 0L);
        glCompileShaderARB(shader);
#if DO_DEBUG
        printInfoLog(shader, file + ": compile status");
#endif
        _shaders.append(shader);
        return shader;
    }
    else
        qWarning("No such shader file: %s", file.toLatin1().constData());
    return 0;
}
