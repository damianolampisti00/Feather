APP_NAME = Feather

CONFIG += qt warn_on cascades10
QT += network
LIBS += -lbbdata -lbbsystem -lbbutility -lbbmultimedia

INCLUDEPATH += src src/tls

SOURCES += src/main.cpp \
           src/browsercontroller.cpp \
           src/tls/tlsnetworkaccessmanager.cpp \
           src/tls/tlsnetworkreply.cpp \
           src/tls/wsclient.cpp

HEADERS += src/browsercontroller.hpp \
           src/tls/tlsnetworkaccessmanager.hpp \
           src/tls/tlsnetworkreply.hpp \
           src/tls/bbportlog.hpp \
           src/tls/wsclient.hpp

# Native TLS 1.2 (mbedTLS 2.28, third_party/mbedtls) for https:// and wss://
# -- device only: BB10's own OpenSSL/QSslSocket only speaks TLS 1.0. Plain
# http:// requests fall through to Qt's normal backend, so a LAN test server
# works without it.
MBEDTLS_DIR = $$quote($$_PRO_FILE_PWD_/../third_party/mbedtls)
device {
    DEFINES += BBPORT_HAVE_NATIVE_TLS
    INCLUDEPATH += $$quote($$MBEDTLS_DIR/include)
    LIBS += $$quote($$MBEDTLS_DIR/lib/armv7/libmbedtls_all.a)
    # libjpeg-turbo's NEON decoder for the frames (built by tools/build-libjpeg-turbo.sh)
    INCLUDEPATH += $$quote($$_PRO_FILE_PWD_/../third_party/libjpeg-turbo/include)
    LIBS += $$quote($$_PRO_FILE_PWD_/../third_party/libjpeg-turbo/lib/armv7/libjpeg-turbo.a)
}

OTHER_FILES += assets/main.qml assets/cacert.pem bar-descriptor.xml
