#pragma once

#include <QWidget>
#include <memory>

#ifdef Q_OS_WIN

class D3d11VideoViewHost final : public QWidget {
    Q_OBJECT
public:
    explicit D3d11VideoViewHost(QWidget* parent = nullptr);
    ~D3d11VideoViewHost() override;

    // texture: ID3D11Texture2D* from FFmpeg; keepAlive retains the AVFrame ref.
    void setDecodeTexture(void* texture, int subresourceIndex, std::shared_ptr<void> keepAlive);
    void clearFrame();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void ensureInitialized();
    void presentPending();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // Q_OS_WIN
