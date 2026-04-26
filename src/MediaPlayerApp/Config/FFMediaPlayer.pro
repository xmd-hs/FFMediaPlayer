TEMPLATE = app
TARGET = FFMediaPlayer
VERSION = 1.0.0

QT += core opengl gui multimedia widgets
CONFIG += c++17 console debug_and_release
CONFIG -= debug_and_release_target

DEFINES += QT_DLL QT_OPENGL_LIB QT_OPENGLEXTENSIONS_LIB QT_WIDGETS_LIB QT_MULTIMEDIA_LIB

INCLUDEPATH += ./GeneratedFiles \
    . \
    ./GeneratedFiles/$$CONFIG \
    ./../../../include \
    ../UI \
    ../Core \
    ../Thread \
    ../../ThreadPool \
    ../../MemoryPool

DEPENDPATH += .

win32 {
    CONFIG(debug, debug|release) {
        TARGET = FFMediaPlayer
        DESTDIR = ../../../bin/win64/debug
        OBJECTS_DIR = ../../../build/win64/debug/obj
        MOC_DIR = ../../../build/win64/debug/moc
        RCC_DIR = ../../../build/win64/debug/rcc
        UI_DIR = ../../../build/win64/debug/ui
        DEFINES += _DEBUG
    }
    CONFIG(release, debug|release) {
        DESTDIR = ../../../bin/win64/release
        OBJECTS_DIR = ../../../build/win64/release/obj
        MOC_DIR = ../../../build/win64/release/moc
        RCC_DIR = ../../../build/win64/release/rcc
        UI_DIR = ../../../build/win64/release/ui
        DEFINES += NDEBUG
    }
    LIBS += -L"./../../../lib/win64"
    RC_FILE += FFMediaPlayer.rc
}

unix {
    CONFIG(debug, debug|release) {
        DESTDIR = ../../../bin/linux64/debug
        OBJECTS_DIR = ../../../build/linux64/debug/obj
        MOC_DIR = ../../../build/linux64/debug/moc
        RCC_DIR = ../../../build/linux64/debug/rcc
        UI_DIR = ../../../build/linux64/debug/ui
        DEFINES += _DEBUG
    }
    CONFIG(release, debug|release) {
        DESTDIR = ../../../bin/linux64/release
        OBJECTS_DIR = ../../../build/linux64/release/obj
        MOC_DIR = ../../../build/linux64/release/moc
        RCC_DIR = ../../../build/linux64/release/rcc
        UI_DIR = ../../../build/linux64/release/ui
        DEFINES += NDEBUG
    }
    LIBS += -L"./../../../lib/linux64"
    QMAKE_RPATHDIR += \$ORIGIN
    QMAKE_LFLAGS += -Wl,--no-as-needed
}

LIBS += -lavcodec -lavformat -lavutil -lswresample -lswscale -lavfilter -lavdevice -lpostproc

msvc {
    QMAKE_CXXFLAGS += /utf-8
    QMAKE_CXXFLAGS_DEBUG += /Zi /Od /W3 /MP
    QMAKE_CXXFLAGS_RELEASE += /O2 /GL /DNDEBUG /W3 /MP
    QMAKE_LFLAGS_DEBUG += /DEBUG
    QMAKE_LFLAGS_RELEASE += /LTCG
}

gcc {
    QMAKE_CXXFLAGS += -Wno-unused-parameter -Wall -Wextra
    QMAKE_CXXFLAGS_DEBUG += -g -O0 -D_DEBUG
    QMAKE_CXXFLAGS_RELEASE += -O2 -DNDEBUG
}

include(FFMediaPlayer.pri)
