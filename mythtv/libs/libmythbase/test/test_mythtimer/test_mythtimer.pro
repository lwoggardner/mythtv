include ( ../../../../settings.pro )

CONFIG += qtestlib
TEMPLATE = app
TARGET = test_mythtimer
DEPENDPATH += . ../..
INCLUDEPATH += . ../..

# Input
HEADERS += test_mythtimer.h
SOURCES += test_mythtimer.cpp

HEADERS += mythtimer.h
SOURCES += mythtimer.cpp

QMAKE_CLEAN += $(TARGET) $(TARGETA) $(TARGETD) $(TARGET0) $(TARGET1) $(TARGET2)