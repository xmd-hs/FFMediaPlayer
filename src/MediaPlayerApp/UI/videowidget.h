#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QMutex>
#include "ivideocallback.h"
#include "MemoryPool.h"
#include "GlobalThreadPool.h"

/**
 * @brief OpenGL 视频渲染组件类
 *
 * 继承 QOpenGLWidget 和 IVideoCallback，
 * 实现 YUV420P 视频帧的 OpenGL 硬件加速渲染。
 * 支持播放前显示默认"请打开文件"提示界面。
 */
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions, public IVideoCallback
{
    Q_OBJECT

public:
    VideoWidget(QWidget *parent = NULL);
    ~VideoWidget();

    /**
     * @brief 初始化视频尺寸和纹理参数
     * @param width 视频宽度
     * @param height 视频高度
     */
    void Init(int width, int height);

    /**
     * @brief 接收并缓存视频帧数据
     * @param frame 解码后的视频帧（函数内部会释放）
     *
     * 将 AVFrame 的 YUV 数据复制到内部缓冲区，
     * 然后触发 update() 请求异步重绘。
     */
    void Repaint(AVFrame *frame) override;

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;

private:
    /**
     * @brief 绘制默认提示界面（黑色背景 + "请打开文件"）
     *
     * 使用 QPainter 绘制，仅在未加载视频时调用。
     * 调用后 OpenGL 状态会被修改，下次视频渲染前
     * 需调用 bindVideoState() 恢复。
     */
    void drawDefaultScreen();

    /**
     * @brief 重新绑定视频渲染所需的 OpenGL 状态
     *
     * QPainter 使用后会破坏着色器程序和顶点属性绑定，
     * 此方法重新绑定 QOpenGLShaderProgram 和顶点属性，
     * 确保视频帧渲染管线正确。
     */
    void bindVideoState();

    QMutex mux;                          ///< 渲染数据互斥锁
    unsigned char* datas[3] = { 0 };     ///< Y/U/V 数据缓冲区
    size_t dataSizes[3] = { 0 };         ///< Y/U/V 缓冲区大小（用于MemoryPool释放）
    GLuint texs[3] = { 0 };             ///< Y/U/V 三个纹理 ID
    GLuint unis[3] = { 0 };             ///< Y/U/V 纹理 uniform 位置
    QOpenGLShaderProgram program;        ///< OpenGL 着色器程序
    int width = 0;                       ///< 视频宽度
    int height = 0;                      ///< 视频高度
    bool hasVideo_ = false;              ///< 是否已加载视频（用于区分默认界面/视频渲染）
    bool glInitialized_ = false;         ///< OpenGL 是否已初始化完成
};
