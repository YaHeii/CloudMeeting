#ifndef DEVICEENUMERATOR_H
#define DEVICEENUMERATOR_H

#include <QStringList>
#include "VideoCapture.h"

class DeviceEnumerator
{
public:
    static QStringList getDevices(MediaType mediaType);
};

#endif // DEVICEENUMERATOR_H
