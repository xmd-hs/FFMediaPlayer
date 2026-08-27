#pragma once

#include <QWidget>

#ifdef Q_OS_MAC

#include <memory>

class MetalVideoViewHost final : public QWidget {
    Q_OBJECT
public:
    explicit MetalVideoViewHost(QWidget* parent = nullptr);
    ~MetalVideoViewHost() override;

    void setPixelBuffer(void* cvPixelBufferRetainAlreadyHeld);
    void clearFrame();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void syncNativeFrame();

    struct Native;
    std::unique_ptr<Native> native_;
};

#endif // Q_OS_MAC
