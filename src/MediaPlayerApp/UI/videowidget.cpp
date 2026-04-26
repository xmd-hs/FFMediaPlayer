#include "videowidget.h"
#include <cstring>
extern "C" {
#include <libavutil/frame.h>
}
#include <QPainter>
#include <QFont>

#define A_VER 3
#define T_VER 4

static const char *vString =
    "attribute vec4 vertexIn;\n"
    "attribute vec2 textureIn;\n"
    "varying vec2 textureOut;\n"
    "void main(void)\n"
    "{\n"
    "    gl_Position = vertexIn;\n"
    "    textureOut = textureIn;\n"
    "}\n";

static const char *tString =
    "varying vec2 textureOut;\n"
    "uniform sampler2D tex_y;\n"
    "uniform sampler2D tex_u;\n"
    "uniform sampler2D tex_v;\n"
    "void main(void)\n"
    "{\n"
    "    vec3 yuv;\n"
    "    vec3 rgb;\n"
    "    yuv.x = texture2D(tex_y, textureOut).r;\n"
    "    yuv.y = texture2D(tex_u, textureOut).r - 0.5;\n"
    "    yuv.z = texture2D(tex_v, textureOut).r - 0.5;\n"
    "    rgb = mat3(1.0, 1.0, 1.0,\n"
    "               0.0, -0.39465, 2.03211,\n"
    "               1.13983, -0.58060, 0.0) * yuv;\n"
    "    gl_FragColor = vec4(rgb, 1.0);\n"
    "}\n";

static const GLfloat s_ver[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
static const GLfloat s_tex[] = { 0, 1, 1, 1, 0, 0, 1, 0 };

VideoWidget::VideoWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

VideoWidget::~VideoWidget()
{
    for (int i = 0; i < 3; i++)
    {
        if (datas[i] && dataSizes[i] > 0)
        {
            Kama_memoryPool::MemoryPool::deallocate(datas[i], dataSizes[i]);
        }
        datas[i] = NULL;
        dataSizes[i] = 0;
    }
}

void VideoWidget::Init(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    QMutexLocker locker(&mux);
    this->width = width;
    this->height = height;
    hasVideo_ = true;

    for (int i = 0; i < 3; i++)
    {
        if (datas[i] && dataSizes[i] > 0)
        {
            Kama_memoryPool::MemoryPool::deallocate(datas[i], dataSizes[i]);
        }
        datas[i] = NULL;
        dataSizes[i] = 0;
    }

    dataSizes[0] = width * height;
    dataSizes[1] = width * height / 4;
    dataSizes[2] = width * height / 4;

    datas[0] = (unsigned char*)Kama_memoryPool::MemoryPool::allocate(dataSizes[0]);
    datas[1] = (unsigned char*)Kama_memoryPool::MemoryPool::allocate(dataSizes[1]);
    datas[2] = (unsigned char*)Kama_memoryPool::MemoryPool::allocate(dataSizes[2]);

    if (!datas[0] || !datas[1] || !datas[2])
    {
        for (int i = 0; i < 3; i++)
        {
            if (datas[i] && dataSizes[i] > 0)
                Kama_memoryPool::MemoryPool::deallocate(datas[i], dataSizes[i]);
            datas[i] = NULL;
            dataSizes[i] = 0;
        }
        hasVideo_ = false;
        return;
    }

    memset(datas[0], 0, width * height);
    memset(datas[1], 128, width * height / 4);
    memset(datas[2], 128, width * height / 4);

    makeCurrent();
    if (texs[0]) glDeleteTextures(3, texs);
    glGenTextures(3, texs);

    glBindTexture(GL_TEXTURE_2D, texs[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    glBindTexture(GL_TEXTURE_2D, texs[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    glBindTexture(GL_TEXTURE_2D, texs[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, 0);

    doneCurrent();
}

void VideoWidget::Repaint(AVFrame *frame)
{
    if (!frame) return;

    QMutexLocker locker(&mux);

    if (frame->width != this->width || frame->height != this->height)
    {
        int newW = frame->width;
        int newH = frame->height;
        if (newW <= 0 || newH <= 0) { av_frame_free(&frame); return; }
        locker.unlock();
        Init(newW, newH);
        locker.relock();
        if (!hasVideo_ || !datas[0]) { av_frame_free(&frame); return; }
    }

    if (!datas[0] || width * height == 0)
    {
        av_frame_free(&frame);
        return;
    }

    if (width == frame->linesize[0])
    {
        memcpy(datas[0], frame->data[0], width * height);
        memcpy(datas[1], frame->data[1], width * height / 4);
        memcpy(datas[2], frame->data[2], width * height / 4);
    }
    else
    {
        for (int i = 0; i < height; i++)
            memcpy(datas[0] + width * i, frame->data[0] + frame->linesize[0] * i, width);
        for (int i = 0; i < height / 2; i++)
            memcpy(datas[1] + width / 2 * i, frame->data[1] + frame->linesize[1] * i, width / 2);
        for (int i = 0; i < height / 2; i++)
            memcpy(datas[2] + width / 2 * i, frame->data[2] + frame->linesize[2] * i, width / 2);
    }

    av_frame_free(&frame);
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();

    if (!program.addShaderFromSourceCode(QOpenGLShader::Fragment, tString))
    {
        qDebug("Fragment shader compile failed: %s", program.log().toUtf8().constData());
        return;
    }
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vString))
    {
        qDebug("Vertex shader compile failed: %s", program.log().toUtf8().constData());
        return;
    }

    program.bindAttributeLocation("vertexIn", A_VER);
    program.bindAttributeLocation("textureIn", T_VER);

    if (!program.link())
    {
        qDebug("Shader program link failed: %s", program.log().toUtf8().constData());
        return;
    }

    program.bind();

    glVertexAttribPointer(A_VER, 2, GL_FLOAT, 0, 0, s_ver);
    glEnableVertexAttribArray(A_VER);
    glVertexAttribPointer(T_VER, 2, GL_FLOAT, 0, 0, s_tex);
    glEnableVertexAttribArray(T_VER);

    unis[0] = program.uniformLocation("tex_y");
    unis[1] = program.uniformLocation("tex_u");
    unis[2] = program.uniformLocation("tex_v");

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    glInitialized_ = true;
}

void VideoWidget::bindVideoState()
{
    program.bind();

    glVertexAttribPointer(A_VER, 2, GL_FLOAT, 0, 0, s_ver);
    glEnableVertexAttribArray(A_VER);
    glVertexAttribPointer(T_VER, 2, GL_FLOAT, 0, 0, s_tex);
    glEnableVertexAttribArray(T_VER);

    glUniform1i(unis[0], 0);
    glUniform1i(unis[1], 1);
    glUniform1i(unis[2], 2);

    glDisable(GL_BLEND);
}

void VideoWidget::drawDefaultScreen()
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int side = qMin(QWidget::width(), QWidget::height());
    int iconSize = side / 3;

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 20));
    painter.drawEllipse(rect().center(), iconSize / 2, iconSize / 2);

    int triSize = iconSize / 5;
    QPoint center = rect().center();
    center.setY(center.y() - iconSize / 8);

    QPolygon triangle;
    triangle << QPoint(center.x() - triSize / 2, center.y() - triSize / 2)
             << QPoint(center.x() - triSize / 2, center.y() + triSize / 2)
             << QPoint(center.x() + triSize, center.y());

    painter.setBrush(QColor(255, 255, 255, 50));
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(triangle);

    QPen textPen(QColor(200, 200, 200, 180));
    textPen.setWidth(1);
    painter.setPen(textPen);

    QFont font;
#ifdef Q_OS_WIN
    font.setFamily("Microsoft YaHei");
#else
    font.setFamily("Sans Serif");
#endif
    font.setPixelSize(qMax(iconSize / 7, 18));
    font.setBold(true);
    painter.setFont(font);

    QRect textRect = rect();
    textRect.setTop(center.y() + triSize);
    painter.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop,
        QString::fromUtf8("请打开文件"));

    painter.end();
}

void VideoWidget::paintGL()
{
    if (!glInitialized_) return;

    QMutexLocker locker(&mux);

    if (!hasVideo_ || width <= 0 || height <= 0)
    {
        locker.unlock();
        drawDefaultScreen();
        return;
    }

    bindVideoState();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texs[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, datas[0]);
    glUniform1i(unis[0], 0);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, texs[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_BYTE, datas[1]);
    glUniform1i(unis[1], 1);

    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, texs[2]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RED, GL_UNSIGNED_BYTE, datas[2]);
    glUniform1i(unis[2], 2);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void VideoWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}
